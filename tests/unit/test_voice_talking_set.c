/*
 * The lobby's talking set over the real loopback transport.
 *
 * CTRL_VOICE_TALKING is what lights the talking indicator next to a name in
 * the lobby. The server rebuilds the set each tick from how long ago each
 * slot's last voice frame arrived and sends it only when it changes, so every
 * client holds the last set it was given until told otherwise. Two ways that
 * left someone lit up with nothing to stop it:
 *
 *   1. The clock. The bookkeeping measures silence, so it has to run at a
 *      constant rate in every server state. The sim's tick does not — it
 *      stands still for the whole countdown — so a talker who spoke as the
 *      countdown began never aged out of the set and stayed lit until the
 *      round started.
 *   2. The leaver. Clearing the departing slot's arrival tick makes the next
 *      set omit them, but when they were the only talker that set is empty,
 *      which is what was published a moment ago, so nothing was sent and every
 *      other client kept showing a player who was no longer there.
 *
 * Both are observed end to end on the receiving client's mirror of the set
 * (clientSimGetVoiceTalkingMap), which is the state the indicator draws from —
 * a server-side check would pass on defect 2 while every client still showed
 * the leaver talking.
 *
 * Two connected clients: one talks, the other watches. Real Opus frames are
 * encoded up front (as test_voice_flood_cap.c does) because the server's parse
 * rejects a segment it cannot unpack, and a rejected segment is not an arrival.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetReady — the second client's ready flag */
#include "threads.h"
#include "transport_udp.h"         /* transportUdpServerKickPlayer */
#include "voice_core.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX   2000   /* join + map download                          */
#define TALK_MAX       300   /* talking until the watcher's mirror lights up */
#define CLEAR_MAX      300   /* silence until it goes out again              */
#define COUNTDOWN_TALK  10   /* frames sent after the countdown has begun    */
#define TALK_FRAMES     64   /* encoded up front, cycled through while talking */

#define TONE_HZ        440.0
#define TONE_AMPLITUDE 0.3

static const char kWatcherName[] = "TalkWatcher";
static const char kTalkerName[]  = "TalkSpeaker";

typedef struct {
    uint8_t data[VOICE_MAX_PACKET];
    int     len;
} EncodedFrame;

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

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_second_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return h->cs2 != NULL &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

/* True while the watcher's copy of the set names the talker. */
static bool watcherSeesTalker(LoopbackHarness *h, int talkerSlot) {
    return (clientSimGetVoiceTalkingMap(h->cs) &
            ((PlayerBitMap)1u << talkerSlot)) != 0;
}

/* Talk one frame per pump until the watcher's mirror names the talker, up to
 * maxIters pumps. Returns the 1-based pump it happened on, or -1. */
static int talkUntilSeen(LoopbackHarness *h, const EncodedFrame *frames,
                         int talkerSlot, int maxIters) {
    int i;
    for (i = 1; i <= maxIters; i++) {
        clientSimNetSendVoice(h->cs2, frames[i % TALK_FRAMES].data,
                              frames[i % TALK_FRAMES].len, 0);
        loopbackHarnessPump(h);
        if (watcherSeesTalker(h, talkerSlot)) {
            return i;
        }
    }
    return -1;
}

/* Case 1: the countdown's frozen clock. A talker who stops speaking after the
 * countdown has begun must still drop out of the set — the silence that ends an
 * utterance is wall-clock time, and the countdown is five seconds of it. */
int run_voice_talking_stops_in_countdown(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    PlayerBitMap map;   /* read before Stop — it tears the client down */
    int watcherSlot, talkerSlot;
    int at, i;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");

    UT_ASSERT_MSG(loopbackHarnessStart(&h, kWatcherName, /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 0x7A1C0Du),
                  "harness start (talking set, countdown) failed");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("watcher never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    if (!loopbackHarnessAddClient(&h, kTalkerName)) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client (the talker) failed to connect");
    }
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_second_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    watcherSlot = (int)clientSimGetMyPlayerNum(h.cs);
    talkerSlot  = (int)clientSimGetMyPlayerNum(h.cs2);
    if (talkerSlot >= MAX_TANKS || talkerSlot == watcherSlot) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker landed on slot %d (watcher is on %d)", talkerSlot,
                watcherSlot);
    }

    /* Both ready takes the lobby into the countdown. The talker's flag is set
     * directly (only the harness's own client has a ready helper); the watcher's
     * goes through it, which also runs the all-ready check. */
    threadsWaitForMutex();
    serverSimSetReady(h.sim, (BYTE)talkerSlot, true);
    threadsReleaseMutex();
    if (!loopbackHarnessTriggerGameStart(&h)) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not ready the watcher to start the countdown");
    }
    if (serverSimGetState(h.sim) != serverStateCountdown) {
        loopbackHarnessStop(&h);
        UT_FAIL("lobby did not enter the countdown with both clients ready "
                "(state=%d)", (int)serverSimGetState(h.sim));
    }

    /* Talk into the countdown. The last frame has to land after the countdown
     * began: that is what pinned the sim's tick under the arrival, leaving the
     * measured silence at zero for the rest of the countdown. */
    at = talkUntilSeen(&h, frames, talkerSlot, TALK_MAX);
    if (at < 0) {
        map = clientSimGetVoiceTalkingMap(h.cs);
        loopbackHarnessStop(&h);
        UT_FAIL("watcher never saw talker %d in the set within %d pumps of "
                "voice (map=0x%08x)", talkerSlot, TALK_MAX, (unsigned)map);
    }
    for (i = 0; i < COUNTDOWN_TALK; i++) {
        clientSimNetSendVoice(h.cs2, frames[i].data, frames[i].len, 0);
        loopbackHarnessPump(&h);
    }
    if (serverSimGetState(h.sim) != serverStateCountdown) {
        loopbackHarnessStop(&h);
        UT_FAIL("countdown ended while the talker was still speaking");
    }
    if (!watcherSeesTalker(&h, talkerSlot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("watcher lost talker %d from the set while they were still "
                "speaking", talkerSlot);
    }

    /* Silence. The set must drop the talker before the countdown runs out —
     * the round starting would clear the client's copy by itself and prove
     * nothing about the clock. */
    for (i = 1; i <= CLEAR_MAX; i++) {
        loopbackHarnessPump(&h);
        if (!watcherSeesTalker(&h, talkerSlot)) {
            break;
        }
        if (serverSimGetState(h.sim) != serverStateCountdown) {
            loopbackHarnessStop(&h);
            UT_FAIL("talker %d was still in the set when the countdown ended "
                    "after %d silent pumps — the set is aged on a clock that "
                    "stops during the countdown", talkerSlot, i);
        }
    }
    if (watcherSeesTalker(&h, talkerSlot)) {
        map = clientSimGetVoiceTalkingMap(h.cs);
        loopbackHarnessStop(&h);
        UT_FAIL("talker %d stayed in the set for %d silent countdown pumps "
                "(map=0x%08x)", talkerSlot, CLEAR_MAX, (unsigned)map);
    }
    fprintf(stderr, "  voice talking set: talker %d aged out of the set %d "
            "pump(s) into the countdown's silence\n", talkerSlot, i);

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 2: the only talker leaves. The watcher's set must go empty — the
 * departure is the change, even though the value it changes to is the one
 * published before the talking started. */
int run_voice_talking_clears_on_leave(void) {
    EncodedFrame frames[TALK_FRAMES];
    LoopbackHarness h;
    PlayerBitMap map;   /* read before Stop — it tears the client down */
    int watcherSlot, talkerSlot;
    int at, i;

    UT_ASSERT_MSG(encodeFrames(frames, TALK_FRAMES), "opus encode failed");

    UT_ASSERT_MSG(loopbackHarnessStart(&h, kWatcherName, /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 0x7A1C1Eu),
                  "harness start (talking set, leaver) failed");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("watcher never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    if (!loopbackHarnessAddClient(&h, kTalkerName)) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client (the talker) failed to connect");
    }
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_second_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    watcherSlot = (int)clientSimGetMyPlayerNum(h.cs);
    talkerSlot  = (int)clientSimGetMyPlayerNum(h.cs2);
    if (talkerSlot >= MAX_TANKS || talkerSlot == watcherSlot) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker landed on slot %d (watcher is on %d)", talkerSlot,
                watcherSlot);
    }

    at = talkUntilSeen(&h, frames, talkerSlot, TALK_MAX);
    if (at < 0) {
        map = clientSimGetVoiceTalkingMap(h.cs);
        loopbackHarnessStop(&h);
        UT_FAIL("watcher never saw talker %d in the set within %d pumps of "
                "voice (map=0x%08x)", talkerSlot, TALK_MAX, (unsigned)map);
    }
    map = clientSimGetVoiceTalkingMap(h.cs);
    if (map != ((PlayerBitMap)1u << talkerSlot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("watcher's set names more than the one talker (map=0x%08x, "
                "talker=%d) — the leaver case needs them to be the only one",
                (unsigned)map, talkerSlot);
    }
    fprintf(stderr, "  voice talking set: talker %d seen after %d pump(s)\n",
            talkerSlot, at);

    /* The talker leaves mid-utterance, through the server's own disconnect
     * path. Nobody is talking now, which is what the watcher was told before
     * this started — so an equality test against the last published set has
     * nothing to report and leaves the leaver lit up. */
    transportUdpServerKickPlayer(h.sim, kTalkerName);
    for (i = 1; i <= CLEAR_MAX; i++) {
        loopbackHarnessPump(&h);
        map = clientSimGetVoiceTalkingMap(h.cs);
        if (map == 0) {
            break;
        }
    }
    if (map != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("watcher still shows slot %d talking %d pumps after they left "
                "(map=0x%08x)", talkerSlot, CLEAR_MAX, (unsigned)map);
    }
    fprintf(stderr, "  voice talking set: departed talker %d cleared after %d "
            "pump(s)\n", talkerSlot, i);

    loopbackHarnessStop(&h);
    return 0;
}
