/*
 * Voice across a lobby map change, over the real loopback transport.
 *
 * A map change re-arms every connected slot's map download, and
 * downloadComplete drops back to false until the new map's bytes are acked.
 * The voice pump used to read that flag, so every player already in the room
 * lost voice both ways for as long as the re-download ran. The pump now reads
 * hasHadMap instead: a slot that has never installed a map is still not a
 * talker and not a listener, and a slot swapping one map for another still is.
 *
 *   1. A talker streaming one frame per tick across the change loses nothing:
 *      every frame offered to the client goes on the wire and is accepted,
 *      and the slot's not-in-game drop counter never moves.
 *   2. The same stream on a delayed path, checking that the server really was
 *      re-downloading: downloadComplete reads false straight after the change
 *      and true again later, and segments are accepted while it is false.
 *   3. A slot that has never finished a download is still left out: a second
 *      client whose PACKET_MAP_DL_READY never reaches the server stays
 *      connected without a map, and hears none of the first client's voice.
 *   4. The listener side of case 1: with two clients in the room, the one
 *      listening is sent every frame the other says across the change.
 *   5. The server's re-sent JOIN_ACCEPT is lost. Normally it lands in the
 *      same tick as the map-change packet and the client is DOWNLOADING_MAP
 *      before the next voice frame is due; without it the client sits in
 *      JOINING until its own JOIN retry draws another, a whole retry interval
 *      later. Voice captured in that window still leaves tick by tick: the
 *      client's ring (eight frames) drops nothing, the server's per-tick
 *      credit drops nothing, and segments are accepted while the client is
 *      still joining.
 *
 * Real Opus frames are encoded up front (as test_voice_flood_cap.c does): the
 * server's parse rejects a segment with no payload.
 *
 * Both ends take part. The client keeps queueing voice while it re-joins
 * after the change (transportUdpClientSendVoice lets a client that has joined
 * once through in JOINING and DOWNLOADING_MAP), its per-tick channel frame
 * carries that voice in both of those states, and the server keeps pumping
 * it while the slot re-downloads. So every frame offered must reach the wire
 * and be accepted.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_sim.h"
#include "threads.h"
#include "transport_udp.h"         /* transportUdpServerOnLobbyMapChange,
                                    * transportUdpServerGetVoiceStats,
                                    * PACKET_MAP_DL_READY */
#include "voice_core.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX    2000   /* join + map download                         */
#define REDL_MAX       2000   /* pumps allowed for the re-download to finish */
#define SETTLE_MAX      300   /* pumps allowed for the last frames to land   */
#define PRE_FRAMES       20   /* frames sent before the map change           */
#define POST_FRAMES      20   /* frames sent after the re-download completes */
#define CREDIT_PUMPS     12   /* fills the per-sender voice credit           */
#define NO_MAP_PUMPS    100   /* pumps the mapless joiner is left waiting    */
#define TALK_FRAMES      64   /* encoded up front, cycled through            */
#define ACCEPTS_LOST      2   /* re-sent JOIN_ACCEPTs dropped in case 5: the
                               * one the map change sends and the reply to
                               * the client's first JOIN, so the second JOIN,
                               * a retry interval later, is the one answered */
#define MIN_JOINING_PUMPS 20  /* case 5 must hold the client in JOINING for at
                               * least this many frames: well past the eight
                               * the voice ring holds, so a client that only
                               * queued them would have dropped some           */

#define TONE_HZ        440.0
#define TONE_AMPLITUDE 0.3

typedef struct {
    uint8_t data[VOICE_MAX_PACKET];
    int     len;
} EncodedFrame;

/* What one stream across a map change did, read back from both ends. */
typedef struct {
    uint32_t offered;            /* frames handed to clientSimNetSendVoice    */
    uint32_t wireSent;           /* segments the client put on the wire       */
    uint32_t ringDropped;        /* segments the client's own ring dropped    */
    uint32_t accepted;           /* this talker's segments the pump accepted  */
    uint32_t dropNotInGame;      /* this talker's segments refused as mapless */
    uint32_t totalDropped;       /* every segment the pump refused            */
    uint32_t acceptedWhileRedl;  /* accepted while downloadComplete was false */
    uint32_t joiningPumps;       /* pumps after the change the client began
                                  * still in CLIENT_CONNECT_JOINING           */
    uint32_t acceptedWhileJoining;/* accepted during those pumps              */
    uint32_t forwardedToListener;/* frames the pump sent the listener slot    */
    uint32_t heardByListener;    /* talker frames the listener client popped  */
    bool     completeAfterChange;/* downloadComplete straight after the change */
    bool     hadMapAfterChange;  /* hasHadMap straight after the change        */
    int      recompletedAt;      /* pump the re-download finished on, or -1   */
} StreamResult;

/* Encoding is stateful, so the frames come from one continuous tone. */
static int encodeFrames(EncodedFrame *frames, int count) {
    VoiceEncoder *enc;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    double phase = 0.0;
    const double phaseStep = 2.0 * 3.14159265358979323846 * TONE_HZ /
                             (double)VOICE_SAMPLE_RATE;
    int f, i;

    enc = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    if (enc == NULL) {
        return 0;
    }

    for (f = 0; f < count; f++) {
        for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
            pcm[i] = (int16_t)(sin(phase) * TONE_AMPLITUDE * 32767.0);
            phase += phaseStep;
        }
        frames[f].len = voiceEncoderEncode(enc, pcm, frames[f].data,
                                           (int)sizeof(frames[f].data));
        if (frames[f].len <= 0) {
            voiceEncoderDestroy(enc);
            return 0;
        }
    }

    voiceEncoderDestroy(enc);
    return 1;
}

static bool pred_first_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           transportUdpServerTestDownloadComplete(
               (int)clientSimGetMyPlayerNum(h->cs));
}

static bool pred_second_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return h->cs2 != NULL &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED &&
           transportUdpServerTestDownloadComplete(
               (int)clientSimGetMyPlayerNum(h->cs2));
}

/* Pop every voice frame waiting on the listener client and count the ones
 * from talkerSlot. */
static uint32_t drainHeard(struct ClientSim *listener, int talkerSlot) {
    uint8_t buf[VOICE_MAX_PACKET];
    uint8_t from, seq, flags;
    uint32_t heard = 0;
    if (listener == NULL) return 0;
    while (clientSimNetReceiveVoice(listener, &from, &seq, &flags, buf,
                                    (int)sizeof(buf)) > 0) {
        if ((int)from == talkerSlot) heard++;
    }
    return heard;
}

/* Send one frame, then pump once, keeping the listener's receive ring
 * drained. Returns the talker's accepted count gained during the pump. */
static uint32_t talkOnePump(LoopbackHarness *h, struct ClientSim *talker,
                            int talkerSlot, struct ClientSim *listener,
                            const EncodedFrame *frames, StreamResult *r) {
    uint32_t before, after;
    const EncodedFrame *fr = &frames[r->offered % TALK_FRAMES];

    clientSimNetSendVoice(talker, fr->data, fr->len, 0);
    r->offered++;
    transportUdpServerTestVoiceSlotStats(talkerSlot, &before, NULL, NULL);
    loopbackHarnessPump(h);
    transportUdpServerTestVoiceSlotStats(talkerSlot, &after, NULL, NULL);
    r->heardByListener += drainHeard(listener, talkerSlot);
    return after - before;
}

/* Stream one frame per pump from `talker` before, during and after a lobby
 * map change, and read back what each end made of it. listener may be NULL
 * (listenerSlot is then ignored). Returns false if the re-download never
 * finished. */
static bool streamAcrossMapChange(LoopbackHarness *h, struct ClientSim *talker,
                                  int talkerSlot, struct ClientSim *listener,
                                  int listenerSlot, const EncodedFrame *frames,
                                  StreamResult *r) {
    uint32_t baseSent, baseRing, baseAccepted, baseNotInGame, baseFwd;
    uint32_t baseTotalAccepted, baseTotalDropped, baseTotalCapped;
    uint32_t sent, ring, accepted, notInGame, fwd;
    uint32_t totalAccepted, totalDropped, totalCapped;
    int i;

    memset(r, 0, sizeof(*r));
    r->recompletedAt = -1;

    /* Fill the credit so a frame that lands late is never the cap's. */
    for (i = 0; i < CREDIT_PUMPS; i++) {
        loopbackHarnessPump(h);
    }
    (void)drainHeard(listener, talkerSlot);

    clientSimNetGetVoiceChannelStats(talker, &baseSent, &baseRing, NULL);
    transportUdpServerTestVoiceSlotStats(talkerSlot, &baseAccepted,
                                         &baseNotInGame, NULL);
    transportUdpServerTestVoiceSlotStats(listener != NULL ? listenerSlot : -1,
                                         NULL, NULL, &baseFwd);
    transportUdpServerGetVoiceStats(&baseTotalAccepted, &baseTotalDropped,
                                    &baseTotalCapped);

    for (i = 0; i < PRE_FRAMES; i++) {
        (void)talkOnePump(h, talker, talkerSlot, listener, frames, r);
    }

    /* The host picks another map. */
    threadsWaitForMutex();
    transportUdpServerOnLobbyMapChange(h->sim);
    threadsReleaseMutex();
    r->completeAfterChange = transportUdpServerTestDownloadComplete(talkerSlot);
    r->hadMapAfterChange   = transportUdpServerTestHasHadMap(talkerSlot);

    /* Keep talking through the re-download. The flag can only go true after
     * the pump has run in a server tick (serverServiceMapTransfer follows
     * serverPumpVoice), so a pump that starts with it false pumped this
     * talker's voice while it was false. */
    for (i = 1; i <= REDL_MAX; i++) {
        bool redl = !transportUdpServerTestDownloadComplete(talkerSlot);
        bool joining = clientSimGetConnectState(talker) ==
                       CLIENT_CONNECT_JOINING;
        uint32_t gained = talkOnePump(h, talker, talkerSlot, listener, frames, r);
        if (redl) r->acceptedWhileRedl += gained;
        if (joining) {
            r->joiningPumps++;
            r->acceptedWhileJoining += gained;
        }
        if (transportUdpServerTestDownloadComplete(talkerSlot) &&
            (listener == NULL ||
             transportUdpServerTestDownloadComplete(listenerSlot)) &&
            clientSimGetConnectState(talker) == CLIENT_CONNECT_CONNECTED) {
            r->recompletedAt = i;
            break;
        }
    }
    if (r->recompletedAt < 0) return false;

    for (i = 0; i < POST_FRAMES; i++) {
        (void)talkOnePump(h, talker, talkerSlot, listener, frames, r);
    }

    /* Drain until the server has accounted for every segment that left the
     * client one way or the other. */
    for (i = 1; i <= SETTLE_MAX; i++) {
        clientSimNetGetVoiceChannelStats(talker, &sent, NULL, NULL);
        transportUdpServerGetVoiceStats(&totalAccepted, &totalDropped, NULL);
        if ((totalAccepted - baseTotalAccepted) +
            (totalDropped - baseTotalDropped) >= sent - baseSent) {
            break;
        }
        loopbackHarnessPump(h);
        r->heardByListener += drainHeard(listener, talkerSlot);
    }
    /* A few more for the forwarded frames to reach the listener. */
    for (i = 0; i < 10; i++) {
        loopbackHarnessPump(h);
        r->heardByListener += drainHeard(listener, talkerSlot);
    }

    clientSimNetGetVoiceChannelStats(talker, &sent, &ring, NULL);
    transportUdpServerTestVoiceSlotStats(talkerSlot, &accepted, &notInGame,
                                         NULL);
    transportUdpServerTestVoiceSlotStats(listener != NULL ? listenerSlot : -1,
                                         NULL, NULL, &fwd);
    transportUdpServerGetVoiceStats(&totalAccepted, &totalDropped,
                                    &totalCapped);
    r->wireSent            = sent - baseSent;
    r->ringDropped         = ring - baseRing;
    r->accepted            = accepted - baseAccepted;
    r->dropNotInGame       = notInGame - baseNotInGame;
    r->totalDropped        = totalDropped - baseTotalDropped;
    r->forwardedToListener = fwd - baseFwd;
    (void)totalCapped;
    (void)baseTotalCapped;
    return true;
}

static void printResult(const char *label, const StreamResult *r) {
    fprintf(stderr, "  voice map change (%s): offered %u, on the wire %u, "
            "accepted %u (%u while re-downloading, %u while joining), "
            "not-in-game drops %u, all drops %u, forwarded to listener %u, "
            "heard %u, joining for %u pump(s), re-download done after %d "
            "pump(s)\n", label, (unsigned)r->offered,
            (unsigned)r->wireSent, (unsigned)r->accepted,
            (unsigned)r->acceptedWhileRedl, (unsigned)r->acceptedWhileJoining,
            (unsigned)r->dropNotInGame, (unsigned)r->totalDropped,
            (unsigned)r->forwardedToListener, (unsigned)r->heardByListener,
            (unsigned)r->joiningPumps, r->recompletedAt);
}

/* Bring up a lobby server with one connected client whose download the server
 * has seen complete. */
static bool startOneReady(LoopbackHarness *h, const char *name,
                          const char *impairSpec, uint64_t seed,
                          bool virtualClock) {
    if (!loopbackHarnessStart(h, name, /*lobbyMode*/ true, impairSpec, seed)) {
        return false;
    }
    if (virtualClock) {
        loopbackHarnessUseVirtualClock(h, true);
    }
    return loopbackHarnessPumpUntil(h, CONNECT_MAX, pred_first_ready, NULL) > 0;
}

/* Case 1: a talker already in the room keeps every segment across the change. */
int run_voice_map_change_talker_kept(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    int slot;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "MapChgTalker", NULL, 0x3A9C01u, false)) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never connected with its download complete within %d "
                "pumps", CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);
    UT_ASSERT_MSG(transportUdpServerTestHasHadMap(slot),
                  "a slot whose download completed must have hasHadMap set");

    if (!streamAcrossMapChange(&h, h.cs, slot, NULL, -1, frames, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("talker", &r);

    if (r.dropNotInGame != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u of the talker's segments were refused as not in the game "
                "across a map change", (unsigned)r.dropNotInGame);
    }
    if (r.ringDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client's own voice ring dropped %u segments",
                (unsigned)r.ringDropped);
    }
    if (r.wireSent != r.offered) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the client for %u offered across the map "
                "change", (unsigned)r.wireSent, (unsigned)r.offered);
    }
    if (r.accepted != r.wireSent || r.totalDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the client across the map change but the "
                "pump accepted %u and dropped %u", (unsigned)r.wireSent,
                (unsigned)r.accepted, (unsigned)r.totalDropped);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 2: the server really was re-downloading while the voice went through.
 * A delayed path stretches the re-download over several round trips, and the
 * segments the client sends meanwhile are pumped while downloadComplete is
 * false. */
int run_voice_map_change_during_redownload(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    int slot;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "MapChgRedl", "delay=60", 0x3A9C02u, true)) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never connected with its download complete within %d "
                "pumps", CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    if (!streamAcrossMapChange(&h, h.cs, slot, NULL, -1, frames, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("re-downloading", &r);

    if (r.completeAfterChange) {
        loopbackHarnessStop(&h);
        UT_FAIL("downloadComplete still read true straight after the map "
                "change; the slot was not re-downloading");
    }
    if (!r.hadMapAfterChange) {
        loopbackHarnessStop(&h);
        UT_FAIL("the map change cleared hasHadMap");
    }
    if (!transportUdpServerTestDownloadComplete(slot) ||
        !transportUdpServerTestHasHadMap(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("after the re-download downloadComplete=%d hasHadMap=%d, "
                "both should be set",
                (int)transportUdpServerTestDownloadComplete(slot),
                (int)transportUdpServerTestHasHadMap(slot));
    }
    if (r.acceptedWhileRedl == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no segment was pumped while the slot was re-downloading, so "
                "this run does not show voice getting through then");
    }
    if (r.dropNotInGame != 0 || r.totalDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("segments refused across the map change: %u not in the game, "
                "%u in all", (unsigned)r.dropNotInGame,
                (unsigned)r.totalDropped);
    }
    if (r.ringDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client's own voice ring dropped %u segments",
                (unsigned)r.ringDropped);
    }
    if (r.wireSent != r.offered) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the client for %u offered across the map "
                "change", (unsigned)r.wireSent, (unsigned)r.offered);
    }
    if (r.accepted != r.wireSent) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the client but the pump accepted %u",
                (unsigned)r.wireSent, (unsigned)r.accepted);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 3: a connected slot that has never had a map hears nothing. */
int run_voice_never_had_map_left_out(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    uint32_t baseFwd, fwd, baseAccepted, accepted;
    uint32_t heard = 0;
    int talkerSlot, joinerSlot = MAX_TANKS;
    int i;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "NoMapTalker", NULL, 0x3A9C03u, false)) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never connected with its download complete within %d "
                "pumps", CONNECT_MAX);
    }
    talkerSlot = (int)clientSimGetMyPlayerNum(h.cs);

    if (!loopbackHarnessAddClient(&h, "NoMapJoiner")) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client failed to connect");
    }
    /* The download begins only once PACKET_MAP_DL_READY arrives, so losing
     * every one leaves the joiner seated with no map. */
    loopbackHarnessDropNextFromClient(&h, h.cs2, PACKET_MAP_DL_READY, 100000);
    for (i = 0; i < NO_MAP_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }
    joinerSlot = (int)clientSimGetMyPlayerNum(h.cs2);
    if (joinerSlot < 0 || joinerSlot >= MAX_TANKS || joinerSlot == talkerSlot) {
        loopbackHarnessStop(&h);
        UT_FAIL("joiner landed on slot %d (talker is on %d)", joinerSlot,
                talkerSlot);
    }
    if (transportUdpServerTestHasHadMap(joinerSlot) ||
        transportUdpServerTestDownloadComplete(joinerSlot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("joiner without a map has hasHadMap=%d downloadComplete=%d",
                (int)transportUdpServerTestHasHadMap(joinerSlot),
                (int)transportUdpServerTestDownloadComplete(joinerSlot));
    }

    memset(&r, 0, sizeof(r));
    transportUdpServerTestVoiceSlotStats(joinerSlot, NULL, NULL, &baseFwd);
    transportUdpServerTestVoiceSlotStats(talkerSlot, &baseAccepted, NULL, NULL);
    for (i = 0; i < PRE_FRAMES; i++) {
        (void)talkOnePump(&h, h.cs, talkerSlot, h.cs2, frames, &r);
    }
    for (i = 0; i < 10; i++) {
        loopbackHarnessPump(&h);
        heard += drainHeard(h.cs2, talkerSlot);
    }
    heard += r.heardByListener;
    transportUdpServerTestVoiceSlotStats(joinerSlot, NULL, NULL, &fwd);
    transportUdpServerTestVoiceSlotStats(talkerSlot, &accepted, NULL, NULL);
    fprintf(stderr, "  voice map change (no map): talker accepted %u, "
            "forwarded to the mapless slot %u, heard there %u\n",
            (unsigned)(accepted - baseAccepted), (unsigned)(fwd - baseFwd),
            (unsigned)heard);

    if (accepted - baseAccepted == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the talker's voice was not accepted at all, so the mapless "
                "slot had nothing to be left out of");
    }
    if (fwd != baseFwd || heard != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("a slot that has never had a map was sent %u voice frames "
                "(%u heard)", (unsigned)(fwd - baseFwd), (unsigned)heard);
    }
    if (transportUdpServerTestHasHadMap(joinerSlot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("joiner gained hasHadMap without a download");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 4: the listener in the room is sent every frame across the change. */
int run_voice_map_change_listener_kept(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    int listenerSlot, talkerSlot;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "MapChgListener", NULL, 0x3A9C04u, false)) {
        loopbackHarnessStop(&h);
        UT_FAIL("listener never connected with its download complete within "
                "%d pumps", CONNECT_MAX);
    }
    if (!loopbackHarnessAddClient(&h, "MapChgSpeaker")) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client (the talker) failed to connect");
    }
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_second_ready, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker never connected with its download complete within %d "
                "pumps", CONNECT_MAX);
    }
    listenerSlot = (int)clientSimGetMyPlayerNum(h.cs);
    talkerSlot   = (int)clientSimGetMyPlayerNum(h.cs2);
    if (talkerSlot >= MAX_TANKS || talkerSlot == listenerSlot) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker landed on slot %d (listener is on %d)", talkerSlot,
                listenerSlot);
    }

    if (!streamAcrossMapChange(&h, h.cs2, talkerSlot, h.cs, listenerSlot,
                               frames, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("listener", &r);

    if (r.dropNotInGame != 0 || r.totalDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("segments refused across the map change: %u not in the game, "
                "%u in all", (unsigned)r.dropNotInGame,
                (unsigned)r.totalDropped);
    }
    if (r.ringDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client's own voice ring dropped %u segments",
                (unsigned)r.ringDropped);
    }
    if (r.wireSent != r.offered) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the client for %u offered across the map "
                "change", (unsigned)r.wireSent, (unsigned)r.offered);
    }
    if (r.accepted != r.wireSent) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the talker but the pump accepted %u",
                (unsigned)r.wireSent, (unsigned)r.accepted);
    }
    if (r.forwardedToListener != r.accepted) {
        loopbackHarnessStop(&h);
        UT_FAIL("pump accepted %u of the talker's segments but forwarded %u "
                "to the listener across the map change",
                (unsigned)r.accepted, (unsigned)r.forwardedToListener);
    }
    if (r.heardByListener != r.forwardedToListener) {
        loopbackHarnessStop(&h);
        UT_FAIL("listener client popped %u of the %u frames forwarded to it",
                (unsigned)r.heardByListener,
                (unsigned)r.forwardedToListener);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 5: the re-sent JOIN_ACCEPT is lost, so the client stays in JOINING for
 * a retry interval, and the voice it captures meanwhile still goes out tick
 * by tick rather than piling up in its ring. The server sends one accept with
 * the map change and one for each JOIN the client sends; dropping the first
 * ACCEPTS_LOST of them leaves the client's second JOIN, JOIN_RETRY_INTERVAL
 * ticks after its first, as the one that gets through. */
int run_voice_map_change_accept_lost(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    int slot;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "MapChgAcceptLost", NULL, 0x3A9C05u, false)) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never connected with its download complete within %d "
                "pumps", CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Armed before the change: no accept is sent to a connected client until
     * the map change, so the first ones dropped are the change's own. */
    loopbackHarnessDropNextToClient(&h, h.cs, PACKET_JOIN_ACCEPT, ACCEPTS_LOST);

    if (!streamAcrossMapChange(&h, h.cs, slot, NULL, -1, frames, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("accept lost", &r);

    if (loopbackHarnessDropNextLeft(&h, h.cs) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("%d of the %d JOIN_ACCEPT drops never happened, so the client "
                "was not held in JOINING",
                loopbackHarnessDropNextLeft(&h, h.cs), ACCEPTS_LOST);
    }
    if (r.joiningPumps < MIN_JOINING_PUMPS) {
        loopbackHarnessStop(&h);
        UT_FAIL("client was in JOINING for only %u pump(s) after the change; "
                "at least %d are needed to show voice flowing from there",
                (unsigned)r.joiningPumps, MIN_JOINING_PUMPS);
    }
    if (r.ringDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client's own voice ring dropped %u segments while it waited "
                "in JOINING for the accept", (unsigned)r.ringDropped);
    }
    if (r.wireSent != r.offered) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the client for %u offered across the map "
                "change", (unsigned)r.wireSent, (unsigned)r.offered);
    }
    if (r.dropNotInGame != 0 || r.totalDropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("segments refused across the map change: %u not in the game, "
                "%u in all", (unsigned)r.dropNotInGame,
                (unsigned)r.totalDropped);
    }
    if (r.accepted != r.wireSent) {
        loopbackHarnessStop(&h);
        UT_FAIL("%u segments left the client but the pump accepted %u",
                (unsigned)r.wireSent, (unsigned)r.accepted);
    }
    /* Flowing, not merely queued and released: segments were accepted in
     * the pumps the client began in JOINING, and in most of them. */
    if (r.acceptedWhileJoining < r.joiningPumps / 2) {
        loopbackHarnessStop(&h);
        UT_FAIL("only %u segments were accepted during the %u pumps the client "
                "spent in JOINING; voice was waiting for the accept, not "
                "flowing", (unsigned)r.acceptedWhileJoining,
                (unsigned)r.joiningPumps);
    }

    loopbackHarnessStop(&h);
    return 0;
}
