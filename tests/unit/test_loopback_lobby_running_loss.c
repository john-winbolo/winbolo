/*
 * Loopback lobby→running transition under loss.
 *
 * Regression for the reliability bug class found under packet loss (lobby-ack
 * resend, control-seq adoption, drop-don't-reset): a real UDP client joins a real
 * in-process UDP server *in the lobby* under loss=5 / burst=2, the server is
 * driven through the all-ready → countdown → running transition, and the
 * client must (a) reach the running phase and (b) keep its reliable control
 * acks converging so the server never trips the unacked-control disconnect.
 *
 * At game start transportUdpServerOnGameStart wipes the per-client control
 * queues and CTRL_GAME_PHASE_RUNNING is published into the fresh sequence
 * space; the client adopts that space and acks it back (piggybacked on the
 * input it sends each running tick). If adoption or ack resend regressed,
 * the server's controlEventQueues[slot].ackedSeq would lag nextSeq and the
 * client would be force-disconnected after CONTROL_UNACKED_TIMEOUT_TICKS
 * (500 server ticks). We therefore pump well past that window after reaching
 * running and assert the client is still connected and still running —
 * convergence proven behaviourally, with no exact-trace dependence.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_enums.h"          /* netStatus / netRunning */
#include "input_packet.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define LOBBY_CONNECT_MAX 2000   /* join + map download under loss */
#define RUNNING_MAX       2000   /* countdown (250) + RUNNING delivery + loss */
#define STABILITY_PUMPS    700   /* > CONTROL_UNACKED_TIMEOUT_TICKS (500) */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Send one input so the client transmits — piggybacking its controlEventAck
 * — and (once running) drives the sim. Returns the next input tick. */
static uint32_t feed_input(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

int run_loopback_lobby_running_loss(void) {
    LoopbackHarness h;
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Joiner", /*lobbyMode*/ true,
                                       /*impairSpec*/ "loss=5,burst=2",
                                       /*seed*/ 0xC0FFEEu),
                  "harness start failed");

    /* 1. Join the lobby. Lobby control acks ride the standalone CONTROL_ACK
     *    path armed by the server's CONTROL_TICK retransmits — no input
     *    needed yet. */
    int connectedAt = loopbackHarnessPumpUntil(&h, LOBBY_CONNECT_MAX,
                                               pred_connected, NULL);
    fprintf(stderr, "  lobby join (loss): connected after %d pump(s) "
                    "(cap %d)\n", connectedAt, LOBBY_CONNECT_MAX);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps",
                LOBBY_CONNECT_MAX);
    }
    UT_ASSERT_MSG(clientSimIsInLobby(h.cs),
                  "client connected but not in lobby phase");

    /* 2. Drive the all-ready → countdown → running transition. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (no slot assigned?)");

    /* 3. Pump to the running phase, feeding input each tick so the
     *    running-space control ack flows back to the server. */
    uint32_t inputTick = 1;
    int runningAt = -1;
    int i;
    for (i = 1; i <= RUNNING_MAX; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (clientSimGetNetStatus(h.cs) == netRunning) {
            runningAt = i;
            break;
        }
    }
    fprintf(stderr, "  lobby→running (loss): running after %d pump(s) "
                    "(cap %d)\n", runningAt, RUNNING_MAX);
    if (runningAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached running phase within %d pumps",
                RUNNING_MAX);
    }

    /* 4. Stability window: keep playing past the unacked-control timeout.
     *    If the control acks were stuck, the server would disconnect the
     *    client inside this window. */
    for (i = 1; i <= STABILITY_PUMPS; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
    }

    ClientConnectState st = clientSimGetConnectState(h.cs);
    netStatus ns = clientSimGetNetStatus(h.cs);
    fprintf(stderr, "  lobby→running (loss): after %d stability pumps "
                    "state=%d netStatus=%d\n", STABILITY_PUMPS, (int)st, (int)ns);

    if (st != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during stability window (state=%d) — "
                "stuck-ack disconnect", (int)st);
    }
    if (ns != netRunning) {
        loopbackHarnessStop(&h);
        UT_FAIL("client left running phase during stability window "
                "(netStatus=%d)", (int)ns);
    }

    loopbackHarnessStop(&h);
    return 0;
}
