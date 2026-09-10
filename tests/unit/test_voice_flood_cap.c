/*
 * Per-client voice flood cap over the real loopback transport.
 *
 * serverPumpVoice forwards at most VOICE_SEGMENTS_PER_TICK segments from one
 * client per tick and drains the rest. That cap is an abuse control: without
 * it a client that ignores the 20 ms capture cadence buys itself unbounded
 * fan-out bandwidth, multiplied by every listener. The transport's cumulative
 * accepted / dropped counters are what makes it observable, so this drives
 * the whole path (encode -> client send wrapper -> CHANNEL_VOICE -> server
 * pump) and reads the counters back.
 *
 *   1. A burst queued inside one client tick rides one channel frame, so the
 *      server sees all of it in a single pump: the cap forwards two and drops
 *      the remainder.
 *   2. A steady one-frame-per-tick stream is never dropped — the cap must
 *      cost a well-behaved talker nothing.
 *   3. The separate concurrent-talker cap stays out of the way throughout:
 *      one talker is far below it, so its suppression counter must not move.
 *
 * Real Opus frames are encoded up front (as test_voice_jitter.c does): the
 * server's parse rejects a segment with no payload, so crafted bytes would
 * risk testing the reject path instead of the cap.
 *
 * The harness is single-client, which is all this needs: the cap is per
 * talker, and a talker with no listeners is still pumped and still counted.
 * That is also why case 3 only pins that the talker cap does not misfire —
 * making it fire needs five simultaneous senders. Its ranking is covered
 * directly in test_voice_talker_select.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "transport_udp.h"         /* transportUdpServerGetVoiceStats */
#include "voice_core.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX   2000   /* join + map download                        */
#define SETTLE_MAX     300   /* pumps allowed for a burst to be pumped out */
#define STEADY_FRAMES   20   /* one frame per tick, the capture cadence    */
#define BURST_FRAMES     5   /* > the cap, queued inside one client tick   */
#define TEST_FRAMES    (STEADY_FRAMES + BURST_FRAMES)

/* Mirrors VOICE_SEGMENTS_PER_TICK in transport_udp_server.c, which is file
 * local to the transport. Must track it. */
#define EXPECT_CAP       2

#define TONE_HZ        440.0
#define TONE_AMPLITUDE 0.3

static const char kPlayerName[] = "FloodTester";

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

int run_voice_flood_cap_enforced(void) {
    EncodedFrame frames[TEST_FRAMES];
    LoopbackHarness h;
    uint32_t baseAccepted, baseDropped, baseCapped;
    uint32_t accepted, dropped, capped;
    int connectedAt;
    int i;

    UT_ASSERT_MSG(encodeFrames(frames, TEST_FRAMES), "opus encode failed");

    UT_ASSERT_MSG(loopbackHarnessStart(&h, kPlayerName, /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0xF100Du),
                  "harness start (voice flood cap) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected,
                                           NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    /* 1. The burst. Every send queues a segment on the client's best-effort
     * voice ring; nothing leaves until the next client tick, so one frame
     * carries all of them and one server pump sees the lot. */
    transportUdpServerGetVoiceStats(&baseAccepted, &baseDropped, &baseCapped);
    for (i = 0; i < BURST_FRAMES; i++) {
        clientSimNetSendVoice(h.cs, frames[i].data, frames[i].len, 0);
    }

    /* Pump until the server has accounted for the whole burst one way or the
     * other, so the counts below are read after it has been pumped, not
     * mid-flight. */
    for (i = 1; i <= SETTLE_MAX; i++) {
        loopbackHarnessPump(&h);
        transportUdpServerGetVoiceStats(&accepted, &dropped, &capped);
        if ((accepted - baseAccepted) + (dropped - baseDropped) >=
            BURST_FRAMES) {
            break;
        }
    }

    transportUdpServerGetVoiceStats(&accepted, &dropped, &capped);
    accepted -= baseAccepted;
    dropped -= baseDropped;
    capped -= baseCapped;

    if (accepted + dropped != BURST_FRAMES) {
        loopbackHarnessStop(&h);
        UT_FAIL("burst of %d segments accounted for as %u accepted + %u "
                "dropped within %d pumps", BURST_FRAMES, (unsigned)accepted,
                (unsigned)dropped, SETTLE_MAX);
    }
    if (accepted > EXPECT_CAP) {
        loopbackHarnessStop(&h);
        UT_FAIL("cap did not bite: %u of %d burst segments forwarded, at most "
                "%d allowed per tick", (unsigned)accepted, BURST_FRAMES,
                EXPECT_CAP);
    }
    if (dropped != (uint32_t)(BURST_FRAMES - EXPECT_CAP)) {
        loopbackHarnessStop(&h);
        UT_FAIL("burst: %u dropped, expected the %d not forwarded",
                (unsigned)dropped, BURST_FRAMES - EXPECT_CAP);
    }
    if (capped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker cap suppressed %u forwards with a single talker",
                (unsigned)capped);
    }
    fprintf(stderr, "  voice flood cap: burst of %d -> %u forwarded, %u "
            "dropped\n", BURST_FRAMES, (unsigned)accepted, (unsigned)dropped);

    /* 2. The steady stream: one frame per tick, the rate a real capture
     * produces. Nothing may be dropped. The cap's second slot absorbs a tick
     * of bunching, so a segment that lands a tick late still gets forwarded. */
    transportUdpServerGetVoiceStats(&baseAccepted, &baseDropped, &baseCapped);
    for (i = 0; i < STEADY_FRAMES; i++) {
        clientSimNetSendVoice(h.cs, frames[BURST_FRAMES + i].data,
                              frames[BURST_FRAMES + i].len, 0);
        loopbackHarnessPump(&h);
    }
    /* Drain the last frames still in flight. */
    for (i = 1; i <= SETTLE_MAX; i++) {
        loopbackHarnessPump(&h);
        transportUdpServerGetVoiceStats(&accepted, &dropped, &capped);
        if (accepted - baseAccepted >= STEADY_FRAMES) {
            break;
        }
    }

    transportUdpServerGetVoiceStats(&accepted, &dropped, &capped);
    accepted -= baseAccepted;
    dropped -= baseDropped;
    capped -= baseCapped;

    if (dropped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("cap penalised a steady one-frame-per-tick talker: %u of %d "
                "segments dropped", (unsigned)dropped, STEADY_FRAMES);
    }
    if (accepted != STEADY_FRAMES) {
        loopbackHarnessStop(&h);
        UT_FAIL("steady stream: %u of %d segments forwarded", (unsigned)accepted,
                STEADY_FRAMES);
    }
    /* 3. The concurrent-talker cap must not fire on a channel that never
     * reaches it. One talker is nowhere near VOICE_MAX_FORWARDED_TALKERS, so
     * any suppression here is an off-by-one in the selection that would
     * silence an ordinary conversation. */
    if (capped != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("talker cap suppressed %u forwards on a steady single-talker "
                "stream", (unsigned)capped);
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped while sending voice");
    }

    loopbackHarnessStop(&h);
    return 0;
}
