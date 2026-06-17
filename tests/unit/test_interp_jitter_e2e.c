/*
 * End-to-end coverage for the estimator-jitter -> render-interpolation wiring:
 * clientSimRenderPrepare reads the timing estimator's jitter
 * (clientSimGetTimingStats) and feeds it to interpRenderControl, which grows
 * the adaptive display delay. The pure test (test_interp_render.c) injects
 * synthetic jitter straight into interpRenderControl; this exercises the whole
 * path over the real loopback transport, which is what the wiring lacked.
 *
 * Determinism: the harness's wall-clock-timed delay/jitter impairment is not
 * reproducible in a tight pump loop, but LOSS is (seeded; a surviving datagram
 * is due immediately). Loss induces the jitter we want — a dropped run of
 * snapshots widens the next applied snapshot's inter-arrival gap, which the
 * estimator reports as jitter.
 *
 * Spec choice: aggregate loss is ~lossPercent*burstLen (net_impair drops a run
 * of burstLen on each loss roll). Keep aggregate ~10% so the reliable control
 * channel's retransmits get through and the client connects — heavy aggregate
 * loss trips the server's unacked-control disconnect before connect. So a low
 * event rate (2%) with a long burst (5): each rare event drops five consecutive
 * snapshots, a gap spike well past INTERP_JITTER_FLOOR_MS, while the 2% rate
 * keeps retransmits flowing. The render-prepare seam (called with a
 * caller-supplied render clock advanced a fixed 16ms/frame) then grows the
 * adaptive delay above zero. A clean link leaves it at zero. Per the harness
 * contract the assertion is "converges within N pumps", not an exact trace.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* interpRenderCtl.currentDelayMs */
#include "client_net.h"            /* clientSimRenderPrepare, clientSimGetTimingStats */
#include "client_connect_state.h"
#include "interpolation.h"         /* INTERP_JITTER_FLOOR_MS */
#include "input_packet.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define E2E_CONNECT_MAX 3000  /* join + map download under loss */
#define E2E_GROW_MAX    3000  /* pumps allowed for jitter to reach the controller */
#define RENDER_FRAME_MS 16    /* ~60fps; a non-discrete dt for interpRenderControl */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Pump the running game while driving the render-prepare seam with an advancing
 * render clock; returns the iteration at which currentDelayMs first grows above
 * zero (the jitter reached the controller end-to-end), or -1 if it never did. */
static int pump_until_delay_grows(LoopbackHarness *h, uint32_t *renderNow,
                                  int *outJitterMs) {
    int i;
    for (i = 1; i <= E2E_GROW_MAX; i++) {
        InputPacket pkt;
        int jitterMs = 0;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick = (uint32_t)i;
        pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
        clientSimNetSendInput(h->cs, &pkt);
        loopbackHarnessPump(h);

        *renderNow += RENDER_FRAME_MS;
        clientSimRenderPrepare(h->cs, *renderNow);

        clientSimGetTimingStats(h->cs, NULL, &jitterMs, NULL, NULL);
        if (outJitterMs) *outJitterMs = jitterMs;
        if (h->cs->interpRenderCtl.currentDelayMs > 0.0f) {
            return i;
        }
    }
    return -1;
}

int run_interp_jitter_e2e(void) {
    /* (A) Heavy loss → estimator jitter past the floor → adaptive delay grows. */
    {
        LoopbackHarness h;
        uint32_t renderNow = 100000;
        int jitterMs = 0;
        int grewAt;

        UT_ASSERT_MSG(loopbackHarnessStart(&h, "Jittery", /*lobbyMode*/ false,
                                           "delay=0,jitter=0,loss=2,burst=5",
                                           /*seed*/ 11u),
                      "harness start failed (loss)");
        UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, E2E_CONNECT_MAX,
                                               pred_connected, NULL) >= 0,
                      "client never connected (loss)");

        grewAt = pump_until_delay_grows(&h, &renderNow, &jitterMs);
        fprintf(stderr,
                "  interp_jitter_e2e: delay grew at pump %d; jitter=%dms "
                "currentDelay=%.2fms\n",
                grewAt, jitterMs, (double)h.cs->interpRenderCtl.currentDelayMs);

        /* The estimator saw loss-induced jitter above the deadband... */
        UT_ASSERT_MSG(jitterMs > INTERP_JITTER_FLOOR_MS,
                      "estimator jitter %dms never exceeded floor %dms",
                      jitterMs, INTERP_JITTER_FLOOR_MS);
        /* ...and clientSimRenderPrepare carried it into the controller. */
        UT_ASSERT_MSG(grewAt >= 0 && h.cs->interpRenderCtl.currentDelayMs > 0.0f,
                      "adaptive display delay never grew under jitter");

        loopbackHarnessStop(&h);
    }

    /* (B) Clean link → jitter stays under the floor → delay stays at zero. */
    {
        LoopbackHarness h;
        uint32_t renderNow = 200000;
        int i;

        UT_ASSERT_MSG(loopbackHarnessStart(&h, "Clean", /*lobbyMode*/ false,
                                           /*impairSpec*/ NULL, /*seed*/ 11u),
                      "harness start failed (clean)");
        UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, E2E_CONNECT_MAX,
                                               pred_connected, NULL) >= 0,
                      "client never connected (clean)");

        for (i = 1; i <= 600; i++) {
            InputPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.tick = (uint32_t)i;
            pkt.playerNum = clientSimGetMyPlayerNum(h.cs);
            clientSimNetSendInput(h.cs, &pkt);
            loopbackHarnessPump(&h);
            renderNow += RENDER_FRAME_MS;
            clientSimRenderPrepare(h.cs, renderNow);
        }

        UT_ASSERT_MSG(h.cs->interpRenderCtl.currentDelayMs == 0.0f,
                      "clean-link adaptive delay grew to %.2fms (expected 0)",
                      (double)h.cs->interpRenderCtl.currentDelayMs);

        loopbackHarnessStop(&h);
    }

    return 0;
}
