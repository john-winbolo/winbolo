/*
 * The reliable control channel under a burst, in two parts.
 *
 * 1. A burst bigger than the send window must NOT cost the client its
 *    connection. CHANNEL_CONTROL_WINDOW is 64 unacked events, and that window
 *    frees only when an ack ARRIVES — which cannot happen inside the call
 *    stack that is publishing, because the same thread reads the socket. A
 *    lobby commit that clears the enemy team and seeds ten bots emits about
 *    sixty events in one stack, and that disconnected the host. Events the
 *    window refuses now wait in the per-client hold buffer
 *    (ControlHoldQueue) and go out on later ticks, oldest first — the same
 *    backpressure the map channel already had.
 *
 * 2. A client that never acks at all must still be reaped, or the hold buffer
 *    would grow without bound. Past its capacity the old last resort stands:
 *    the disconnect is DEFERRED, never run inside the publish.
 *
 * Why deferring matters, and what part 2 still pins: serverSimPublishControl
 * fans an event out to each subscriber while sim->publishing is true.
 * serverDisconnectClient broadcasts "X has left." and serverSimRemovePlayer
 * fans out PLAYER_LEFT — both publish. Running that from inside the deliver
 * callback re-enters serverSimPublishControl and trips its reentrancy guard
 * (and corrupts the in-flight fan-out under NDEBUG). The teardown is deferred
 * to transportUdpServerDrainPendingRemovals, outside any publish. Reaching
 * the end of either flood below at all means no synchronous disconnect
 * re-entered the publish. Found by fuzz_server_dispatch.
 *
 * Sizing, so the two floods stay meaningful if the buffer is resized: the
 * probe text is 14 bytes, so the body is 16, the message 19, and one hold
 * entry 21 bytes. CONTROL_HOLD_BYTES (16384) therefore holds about 780 of
 * them. BURST_EVENTS sits well under that; FLOOD_EVENTS well over.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"                /* BYTE */
#include "server_sim.h"            /* serverSimPublishControl, serverSimGetNumPlayers */
#include "client_sim.h"
#include "client_net.h"            /* clientSimGetConnectState */
#include "client_connect_state.h"  /* CLIENT_CONNECT_CONNECTED */
#include "control_event.h"         /* ControlEvent, CTRL_SERVER_TEXT */
#include "threads.h"               /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

#define JOIN_MAX       2000  /* join + map download on the clean path */
#define BURST_EVENTS    200  /* > the 64-event window, « the hold buffer */
#define FLOOD_EVENTS   2000  /* > the hold buffer's ~780 entries */
#define DRAIN_PUMPS       4  /* ticks for the deferred drain to remove the slot */
#define CATCHUP_PUMPS    60  /* ticks for the held burst to reach the client */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Publish `n` server-text events at the connected client without pumping it,
 * so no ack can arrive and the window only fills. The tick mutex is held the
 * way serverInstanceTick holds it when it publishes. */
static void flood(LoopbackHarness *h, int n) {
    ControlEvent evt;
    int i;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, "overflow probe",
                sizeof(evt.u.serverText.text));
    evt.u.serverText.destPlayer = 0xFF;  /* every client, not slot 0 alone */
    threadsWaitForMutex();
    for (i = 0; i < n; i++) {
        serverSimPublishControl(h->sim, &evt);
    }
    threadsReleaseMutex();
}

/* ---- 1. A burst past the window is held, not fatal. ---- */
int run_control_overflow_holds_burst(void) {
    LoopbackHarness h;
    int numBefore;
    int connectedAt;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Overflow", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0xC0FFEEu),
                  "harness start failed");

    connectedAt = loopbackHarnessPumpUntil(&h, JOIN_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", JOIN_MAX);
    }
    numBefore = (int)serverSimGetNumPlayers(h.sim);
    UT_ASSERT_MSG(numBefore >= 1, "server has no connected player after join");

    flood(&h, BURST_EVENTS);

    /* The burst is three times the window. Pre-hold-buffer this disconnected
     * the client; now it must survive, and the held remainder must go out as
     * the client's acks free the window. */
    for (i = 0; i < CATCHUP_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }

    UT_ASSERT_MSG((int)serverSimGetNumPlayers(h.sim) == numBefore,
                  "a %d-event burst must not disconnect anyone (numPlayers "
                  "%d, was %d)", BURST_EVENTS,
                  (int)serverSimGetNumPlayers(h.sim), numBefore);
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_CONNECTED,
                  "client must still be connected after the held burst");

    loopbackHarnessStop(&h);
    return 0;
}

/* ---- 2. A client that never acks is still reaped, off the publish path. ---- */
int run_control_overflow_defers_disconnect(void) {
    LoopbackHarness h;
    int numBefore;
    int connectedAt;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Overflow", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0xC0FFEEu),
                  "harness start failed");

    connectedAt = loopbackHarnessPumpUntil(&h, JOIN_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", JOIN_MAX);
    }
    numBefore = (int)serverSimGetNumPlayers(h.sim);
    UT_ASSERT_MSG(numBefore >= 1, "server has no connected player after join");

    /* Past the hold buffer as well: the client is not acking at all. */
    flood(&h, FLOOD_EVENTS);

    /* Reaching here means no synchronous disconnect re-entered the publish.
     * The next ticks run the deferred drain, outside any publish. */
    for (i = 0; i < DRAIN_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }

    UT_ASSERT_MSG((int)serverSimGetNumPlayers(h.sim) < numBefore,
                  "a client that filled the hold buffer must be disconnected "
                  "by the deferred drain (numPlayers stayed %d)",
                  (int)serverSimGetNumPlayers(h.sim));

    loopbackHarnessStop(&h);
    return 0;
}
