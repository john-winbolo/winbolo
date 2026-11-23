/*
 * Voice across a lobby map change and a return to the lobby, over the real
 * loopback transport.
 *
 * Voice is carried for every connected slot, map or no map. It used to be
 * tied to the slot's map download being complete, on the server in both
 * directions and on the client's send side, and that download is re-armed
 * by every lobby map change and by every return to the lobby after a round,
 * so everyone in the room lost voice both ways for the length of each
 * re-download. These cases drive the real paths and check nothing is lost:
 *
 *   1. A talker streaming one frame per tick across a lobby map change loses
 *      nothing: every frame offered to the client goes on the wire and is
 *      accepted, and nothing is dropped anywhere.
 *   2. The same stream on a delayed path, checking that the server really
 *      was re-downloading: downloadComplete reads false straight after the
 *      change and true again later, and segments are accepted while false.
 *   3. The listener side of case 1: with two clients in the room, the one
 *      listening is sent every frame the other says across the change.
 *   4. The server's re-sent JOIN_ACCEPT is lost. Normally it lands in the
 *      same tick as the map-change packet and the client is downloading
 *      before the next voice frame is due; without it the client sits in
 *      JOINING until its own JOIN retry draws another, a retry interval
 *      later. Voice still leaves tick by tick through that window: the
 *      client's ring (eight frames) drops nothing, the server's per-tick
 *      credit drops nothing, and segments are accepted while still joining.
 *   5. A joiner whose map never arrives (its PACKET_MAP_DL_READY is lost)
 *      hears the room and can be heard from its first accept on.
 *   6. The round path: two players start a round, talk during it, the round
 *      ends, the server holds on game over and returns to the lobby, which
 *      re-sends the map to both. The listener is sent every frame the talker
 *      says, before, during and after.
 *
 * Real Opus frames are encoded up front (as test_voice_flood_cap.c does):
 * the server's parse rejects a segment with no payload.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_enums.h"          /* netStatus / netRunning */
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetReady,
                                    * serverSimLobbyCheckAllReady,
                                    * serverSimEnterGameOver */
#include "threads.h"
#include "transport_udp.h"         /* transportUdpServerOnLobbyMapChange,
                                    * transportUdpServerGetVoiceStats,
                                    * PACKET_MAP_DL_READY, PACKET_JOIN_ACCEPT */
#include "voice_core.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX    2000   /* join + map download                         */
#define REDL_MAX       2000   /* pumps allowed for the re-download to finish */
#define ROUND_MAX      4000   /* pumps allowed for a round to start, end and
                               * the lobby to be back with both maps in     */
#define SETTLE_MAX      300   /* pumps allowed for the last frames to land   */
#define PRE_FRAMES       20   /* frames sent before the change               */
#define POST_FRAMES      20   /* frames sent after the change completes      */
#define CREDIT_PUMPS     12   /* fills the per-sender voice credit           */
#define NO_MAP_PUMPS    100   /* pumps the mapless joiner is left waiting    */
#define TALK_FRAMES      64   /* encoded up front, cycled through            */
#define ACCEPTS_LOST      2   /* re-sent JOIN_ACCEPTs dropped in case 4: the
                               * one the map change sends and the reply to
                               * the client's first JOIN, so the second JOIN,
                               * a retry interval later, is the one answered */
#define MIN_JOINING_PUMPS 20  /* case 4 must hold the client in JOINING for at
                               * least this many frames: well past the eight
                               * the voice ring holds, so a client that only
                               * queued them would have dropped some           */

#define TONE_HZ        440.0
#define TONE_AMPLITUDE 0.3

typedef struct {
    uint8_t data[VOICE_MAX_PACKET];
    int     len;
} EncodedFrame;

/* What one stream across a change did, read back from both ends. */
typedef struct {
    uint32_t offered;            /* frames handed to clientSimNetSendVoice    */
    uint32_t wireSent;           /* segments the client put on the wire       */
    uint32_t ringDropped;        /* segments the client's own ring dropped    */
    uint32_t accepted;           /* this talker's segments the pump accepted  */
    uint32_t dropCredit;         /* this talker's segments the credit dropped */
    uint32_t totalDropped;       /* every segment the pump refused            */
    uint32_t acceptedWhileRedl;  /* accepted while downloadComplete was false */
    uint32_t joiningPumps;       /* pumps after the change the client began
                                  * still in CLIENT_CONNECT_JOINING           */
    uint32_t acceptedWhileJoining;/* accepted during those pumps              */
    uint32_t forwardedToListener;/* frames the pump sent the listener slot    */
    uint32_t heardByListener;    /* talker frames the listener client popped  */
    bool     completeAfterChange;/* downloadComplete straight after the change */
    int      recompletedAt;      /* pump the change finished on, or -1        */
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

static bool clientReady(struct ClientSim *cs) {
    return cs != NULL &&
           clientSimGetConnectState(cs) == CLIENT_CONNECT_CONNECTED &&
           transportUdpServerTestDownloadComplete(
               (int)clientSimGetMyPlayerNum(cs));
}

static bool pred_first_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return clientReady(h->cs);
}

static bool pred_second_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return clientReady(h->cs2);
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

/* The event a stream is carried across, made under the server mutex. */
typedef void (*ChangeFn)(LoopbackHarness *h);

static void changeLobbyMap(LoopbackHarness *h) {
    transportUdpServerOnLobbyMapChange(h->sim);
}

/* A win ends the round from inside the running tick; this is the same
 * entry. The server then holds on game over for GAMEOVER_HOLD_TICKS and
 * returns to the lobby, re-sending the map to every client on the way. */
static void endRound(LoopbackHarness *h) {
    serverSimEnterGameOver(h->sim);
}

/* Whether the change has fully played out: the talker (and listener, if any)
 * are connected with their downloads complete again, and the server is in
 * the state the change leads to. */
static bool changeDone(LoopbackHarness *h, struct ClientSim *talker,
                       int talkerSlot, struct ClientSim *listener,
                       int listenerSlot, ServerState endState) {
    (void)talkerSlot;
    (void)listenerSlot;
    if (serverSimGetState(h->sim) != endState) return false;
    if (!clientReady(talker)) return false;
    if (listener != NULL && !clientReady(listener)) return false;
    return true;
}

/* Stream one frame per pump from `talker` before, during and after `change`,
 * and read back what each end made of it. listener may be NULL (listenerSlot
 * is then ignored). Returns false if the change never played out within
 * maxPumps. */
static bool streamAcross(LoopbackHarness *h, struct ClientSim *talker,
                         int talkerSlot, struct ClientSim *listener,
                         int listenerSlot, const EncodedFrame *frames,
                         ChangeFn change, ServerState endState, int maxPumps,
                         StreamResult *r) {
    uint32_t baseSent, baseRing, baseAccepted, baseCredit, baseFwd;
    uint32_t baseTotalAccepted, baseTotalDropped;
    uint32_t sent, ring, accepted, credit, fwd;
    uint32_t totalAccepted, totalDropped;
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
                                         &baseCredit, NULL);
    transportUdpServerTestVoiceSlotStats(listener != NULL ? listenerSlot : -1,
                                         NULL, NULL, &baseFwd);
    transportUdpServerGetVoiceStats(&baseTotalAccepted, &baseTotalDropped,
                                    NULL);

    for (i = 0; i < PRE_FRAMES; i++) {
        (void)talkOnePump(h, talker, talkerSlot, listener, frames, r);
    }

    threadsWaitForMutex();
    change(h);
    threadsReleaseMutex();
    r->completeAfterChange = transportUdpServerTestDownloadComplete(talkerSlot);

    /* Keep talking through it. downloadComplete can only go true after the
     * pump has run in a server tick (serverServiceMapTransfer follows
     * serverPumpVoice), so a pump that starts with it false pumped this
     * talker's voice while it was false. */
    for (i = 1; i <= maxPumps; i++) {
        bool redl = !transportUdpServerTestDownloadComplete(talkerSlot);
        bool joining = clientSimGetConnectState(talker) ==
                       CLIENT_CONNECT_JOINING;
        uint32_t gained = talkOnePump(h, talker, talkerSlot, listener, frames, r);
        if (redl) r->acceptedWhileRedl += gained;
        if (joining) {
            r->joiningPumps++;
            r->acceptedWhileJoining += gained;
        }
        if (changeDone(h, talker, talkerSlot, listener, listenerSlot,
                       endState)) {
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
    transportUdpServerTestVoiceSlotStats(talkerSlot, &accepted, &credit, NULL);
    transportUdpServerTestVoiceSlotStats(listener != NULL ? listenerSlot : -1,
                                         NULL, NULL, &fwd);
    transportUdpServerGetVoiceStats(&totalAccepted, &totalDropped, NULL);
    r->wireSent            = sent - baseSent;
    r->ringDropped         = ring - baseRing;
    r->accepted            = accepted - baseAccepted;
    r->dropCredit          = credit - baseCredit;
    r->totalDropped        = totalDropped - baseTotalDropped;
    r->forwardedToListener = fwd - baseFwd;
    return true;
}

static void printResult(const char *label, const StreamResult *r) {
    fprintf(stderr, "  voice across (%s): offered %u, on the wire %u, "
            "accepted %u (%u while re-downloading, %u while joining), "
            "credit drops %u, all drops %u, forwarded to listener %u, "
            "heard %u, joining for %u pump(s), done after %d pump(s)\n",
            label, (unsigned)r->offered, (unsigned)r->wireSent,
            (unsigned)r->accepted, (unsigned)r->acceptedWhileRedl,
            (unsigned)r->acceptedWhileJoining, (unsigned)r->dropCredit,
            (unsigned)r->totalDropped, (unsigned)r->forwardedToListener,
            (unsigned)r->heardByListener, (unsigned)r->joiningPumps,
            r->recompletedAt);
}

/* The checks every talker stream shares: nothing dropped by the client's
 * ring, by the server's credit or by anything else, and everything that
 * left the client was accepted. Returns NULL when they hold, else a message
 * for UT_FAIL. The listener checks, where there is one, are separate. */
static const char *talkerLost(const StreamResult *r, char *msg, size_t cap) {
    if (r->ringDropped != 0) {
        snprintf(msg, cap, "client's own voice ring dropped %u segments",
                 (unsigned)r->ringDropped);
        return msg;
    }
    if (r->wireSent != r->offered) {
        snprintf(msg, cap, "%u segments left the client for %u offered",
                 (unsigned)r->wireSent, (unsigned)r->offered);
        return msg;
    }
    if (r->dropCredit != 0 || r->totalDropped != 0) {
        snprintf(msg, cap, "segments refused: %u by the per-tick credit, %u "
                 "in all", (unsigned)r->dropCredit,
                 (unsigned)r->totalDropped);
        return msg;
    }
    if (r->accepted != r->wireSent) {
        snprintf(msg, cap, "%u segments left the client but the pump "
                 "accepted %u", (unsigned)r->wireSent, (unsigned)r->accepted);
        return msg;
    }
    return NULL;
}

static const char *listenerLost(const StreamResult *r, char *msg,
                                size_t cap) {
    if (r->forwardedToListener != r->accepted) {
        snprintf(msg, cap, "pump accepted %u of the talker's segments but "
                 "forwarded %u to the listener", (unsigned)r->accepted,
                 (unsigned)r->forwardedToListener);
        return msg;
    }
    if (r->heardByListener != r->forwardedToListener) {
        snprintf(msg, cap, "listener client popped %u of the %u frames "
                 "forwarded to it", (unsigned)r->heardByListener,
                 (unsigned)r->forwardedToListener);
        return msg;
    }
    return NULL;
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

/* Add a second client and wait until it too is connected with its download
 * complete. */
static bool addSecondReady(LoopbackHarness *h, const char *name) {
    if (!loopbackHarnessAddClient(h, name)) {
        return false;
    }
    return loopbackHarnessPumpUntil(h, CONNECT_MAX, pred_second_ready, NULL) > 0;
}

/* Case 1: a talker already in the room keeps every segment across the change. */
int run_voice_map_change_talker_kept(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    char msg[160];
    const char *lost;
    int slot;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "MapChgTalker", NULL, 0x3A9C01u, false)) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never connected with its download complete within %d "
                "pumps", CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    if (!streamAcross(&h, h.cs, slot, NULL, -1, frames, changeLobbyMap,
                      serverStateLobby, REDL_MAX, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("map change, talker", &r);

    lost = talkerLost(&r, msg, sizeof(msg));
    if (lost != NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("across a map change: %s", lost);
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
    char msg[160];
    const char *lost;
    int slot;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "MapChgRedl", "delay=60", 0x3A9C02u, true)) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never connected with its download complete within %d "
                "pumps", CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    if (!streamAcross(&h, h.cs, slot, NULL, -1, frames, changeLobbyMap,
                      serverStateLobby, REDL_MAX, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("map change, re-downloading", &r);

    if (r.completeAfterChange) {
        loopbackHarnessStop(&h);
        UT_FAIL("downloadComplete still read true straight after the map "
                "change; the slot was not re-downloading");
    }
    if (!transportUdpServerTestDownloadComplete(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("downloadComplete did not come back after the re-download");
    }
    if (r.acceptedWhileRedl == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no segment was pumped while the slot was re-downloading, so "
                "this run does not show voice getting through then");
    }
    lost = talkerLost(&r, msg, sizeof(msg));
    if (lost != NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("across a map change on a delayed path: %s", lost);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 3: the listener in the room is sent every frame across the change. */
int run_voice_map_change_listener_kept(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    char msg[160];
    const char *lost;
    int listenerSlot, talkerSlot;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "MapChgListener", NULL, 0x3A9C04u, false)) {
        loopbackHarnessStop(&h);
        UT_FAIL("listener never connected with its download complete within "
                "%d pumps", CONNECT_MAX);
    }
    if (!addSecondReady(&h, "MapChgSpeaker")) {
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

    if (!streamAcross(&h, h.cs2, talkerSlot, h.cs, listenerSlot, frames,
                      changeLobbyMap, serverStateLobby, REDL_MAX, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("map change, listener", &r);

    lost = talkerLost(&r, msg, sizeof(msg));
    if (lost == NULL) lost = listenerLost(&r, msg, sizeof(msg));
    if (lost != NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("across a map change with a listener: %s", lost);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 4: the re-sent JOIN_ACCEPT is lost, so the client stays in JOINING for
 * a retry interval, and the voice it captures meanwhile still goes out tick
 * by tick rather than piling up in its ring. The server sends one accept with
 * the map change and one for each JOIN the client sends; dropping the first
 * ACCEPTS_LOST of them leaves the client's second JOIN, JOIN_RETRY_INTERVAL
 * ticks after its first, as the one that gets through. */
int run_voice_map_change_accept_lost(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    char msg[160];
    const char *lost;
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

    if (!streamAcross(&h, h.cs, slot, NULL, -1, frames, changeLobbyMap,
                      serverStateLobby, REDL_MAX, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-download after the map change never finished within %d "
                "pumps", REDL_MAX);
    }
    printResult("map change, accept lost", &r);

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
    lost = talkerLost(&r, msg, sizeof(msg));
    if (lost != NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("with the accept lost: %s", lost);
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

/* Case 5: a joiner whose map never arrives still hears the room and is heard
 * in it. Its PACKET_MAP_DL_READY is lost every time, so the download never
 * begins and the client stays on DOWNLOADING_MAP with a connected slot. */
int run_voice_joiner_hears_before_map(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    uint32_t baseFwd, fwd, baseAccepted, accepted, baseJoinerAcc, joinerAcc;
    uint32_t heardAtJoiner = 0, heardAtTalker = 0;
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
    if (transportUdpServerTestDownloadComplete(joinerSlot) ||
        clientSimGetConnectState(h.cs2) != CLIENT_CONNECT_DOWNLOADING_MAP) {
        loopbackHarnessStop(&h);
        UT_FAIL("joiner is not stuck without a map: downloadComplete=%d, "
                "client state %d",
                (int)transportUdpServerTestDownloadComplete(joinerSlot),
                (int)clientSimGetConnectState(h.cs2));
    }

    /* The room talks; the joiner hears it. */
    memset(&r, 0, sizeof(r));
    transportUdpServerTestVoiceSlotStats(joinerSlot, NULL, NULL, &baseFwd);
    transportUdpServerTestVoiceSlotStats(talkerSlot, &baseAccepted, NULL, NULL);
    for (i = 0; i < PRE_FRAMES; i++) {
        (void)talkOnePump(&h, h.cs, talkerSlot, h.cs2, frames, &r);
    }
    for (i = 0; i < 10; i++) {
        loopbackHarnessPump(&h);
        heardAtJoiner += drainHeard(h.cs2, talkerSlot);
    }
    heardAtJoiner += r.heardByListener;
    transportUdpServerTestVoiceSlotStats(joinerSlot, NULL, NULL, &fwd);
    transportUdpServerTestVoiceSlotStats(talkerSlot, &accepted, NULL, NULL);

    /* The joiner talks; the room hears it. */
    memset(&r, 0, sizeof(r));
    transportUdpServerTestVoiceSlotStats(joinerSlot, &baseJoinerAcc, NULL, NULL);
    for (i = 0; i < PRE_FRAMES; i++) {
        (void)talkOnePump(&h, h.cs2, joinerSlot, h.cs, frames, &r);
    }
    for (i = 0; i < 10; i++) {
        loopbackHarnessPump(&h);
        heardAtTalker += drainHeard(h.cs, joinerSlot);
    }
    heardAtTalker += r.heardByListener;
    transportUdpServerTestVoiceSlotStats(joinerSlot, &joinerAcc, NULL, NULL);

    fprintf(stderr, "  voice before map: room's %u frames accepted, %u "
            "forwarded to the mapless joiner, %u heard there; joiner's %u "
            "frames accepted, %u heard by the room\n",
            (unsigned)(accepted - baseAccepted), (unsigned)(fwd - baseFwd),
            (unsigned)heardAtJoiner, (unsigned)(joinerAcc - baseJoinerAcc),
            (unsigned)heardAtTalker);

    if (accepted - baseAccepted != PRE_FRAMES) {
        loopbackHarnessStop(&h);
        UT_FAIL("the room's talker had %u of %d frames accepted",
                (unsigned)(accepted - baseAccepted), PRE_FRAMES);
    }
    if (fwd - baseFwd != PRE_FRAMES || heardAtJoiner != PRE_FRAMES) {
        loopbackHarnessStop(&h);
        UT_FAIL("a joiner still waiting for its map was sent %u of the room's "
                "%d voice frames and heard %u", (unsigned)(fwd - baseFwd),
                PRE_FRAMES, (unsigned)heardAtJoiner);
    }
    if (joinerAcc - baseJoinerAcc != PRE_FRAMES || heardAtTalker != PRE_FRAMES) {
        loopbackHarnessStop(&h);
        UT_FAIL("the joiner's %d frames: %u accepted by the server, %u heard "
                "by the room", PRE_FRAMES, (unsigned)(joinerAcc - baseJoinerAcc),
                (unsigned)heardAtTalker);
    }
    if (transportUdpServerTestDownloadComplete(joinerSlot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("joiner's download completed after all, so this run did not "
                "test a mapless slot");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 6: a round. Two players ready up, the countdown runs, the round runs
 * while the talker talks, the round ends, the server holds on game over and
 * returns to the lobby, re-sending the map to both; the talker keeps talking
 * throughout and the listener is sent every frame. */
int run_voice_return_to_lobby_kept(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    StreamResult r;
    char msg[160];
    const char *lost;
    int listenerSlot, talkerSlot;
    int i, runningAt = -1;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");
    if (!startOneReady(&h, "RoundListener", NULL, 0x3A9C06u, false)) {
        loopbackHarnessStop(&h);
        UT_FAIL("listener never connected with its download complete within "
                "%d pumps", CONNECT_MAX);
    }
    if (!addSecondReady(&h, "RoundSpeaker")) {
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

    /* On one team: in a running game voice follows the alliance, so two
     * strangers would not hear each other during the round, and the point
     * here is that the round's end and the return to the lobby change
     * nothing about who hears whom. Then both ready: the all-ready check
     * enters the countdown, and the running tick starts the round when it
     * runs out. */
    threadsWaitForMutex();
    serverSimSetTeam(h.sim, (BYTE)listenerSlot, 1);
    serverSimSetTeam(h.sim, (BYTE)talkerSlot, 1);
    serverSimSetReady(h.sim, (BYTE)talkerSlot, true);
    threadsReleaseMutex();
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (no slot assigned?)");
    for (i = 1; i <= ROUND_MAX; i++) {
        loopbackHarnessPump(&h);
        if (serverSimGetState(h.sim) == serverStateRunning &&
            clientSimGetNetStatus(h.cs) == netRunning &&
            clientSimGetNetStatus(h.cs2) == netRunning) {
            runningAt = i;
            break;
        }
    }
    if (runningAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the round never started within %d pumps (server state %d)",
                ROUND_MAX, (int)serverSimGetState(h.sim));
    }
    fprintf(stderr, "  voice across (round): running after %d pump(s)\n",
            runningAt);

    /* Talk through the round, then end it and keep talking until the lobby
     * is back and both maps are in. */
    if (!streamAcross(&h, h.cs2, talkerSlot, h.cs, listenerSlot, frames,
                      endRound, serverStateLobby, ROUND_MAX, &r)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the lobby never came back with both downloads complete "
                "within %d pumps (server state %d)", ROUND_MAX,
                (int)serverSimGetState(h.sim));
    }
    printResult("round end, listener", &r);

    /* The change went the long way: a game-over hold, then the lobby's map
     * re-send, which re-arms the download. */
    if (r.recompletedAt < GAMEOVER_HOLD_TICKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("lobby was back after %d pump(s), inside the %d tick game-over "
                "hold; the round did not end the way a win ends it",
                r.recompletedAt, (int)GAMEOVER_HOLD_TICKS);
    }
    if (r.acceptedWhileRedl == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no segment was pumped while the talker's slot was "
                "re-downloading the lobby map");
    }
    lost = talkerLost(&r, msg, sizeof(msg));
    if (lost == NULL) lost = listenerLost(&r, msg, sizeof(msg));
    if (lost != NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("across a round and the return to the lobby: %s", lost);
    }

    loopbackHarnessStop(&h);
    return 0;
}
