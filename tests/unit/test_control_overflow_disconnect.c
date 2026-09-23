/*
 * Regression: a control-event queue overflow detected inside a publish must
 * DEFER the disconnect, not run it synchronously.
 *
 * serverSimPublishControl fans an event out to each subscriber's deliver
 * callback while sim->publishing is true. The server's deliver callback
 * (udpClientDeliverControl) disconnects a client whose reliable control channel
 * (CHANNEL_CONTROL) window is full — but serverDisconnectClient broadcasts
 * "X has left." and the paired serverSimRemovePlayer fans out PLAYER_LEFT, and
 * both publish. Doing that from inside the deliver callback re-enters
 * serverSimPublishControl and trips its `assert(!publishing)` reentrancy guard
 * (and, under NDEBUG, corrupts the in-flight fan-out). The fix defers the whole
 * disconnect to transportUdpServerDrainPendingRemovals, which runs outside any
 * publish.
 *
 * The test drives the real UDP server through the loopback harness: a client
 * joins, then — without ever acking (we don't pump the client during the
 * flood) — the test publishes enough control events at it to fill both the
 * CHANNEL_CONTROL window and the backlog behind it. A send that finds the
 * window full waits in the backlog, so it is the send that finds the backlog
 * full too that fails inside its own fan-out: pre-fix that aborts the process
 * on the reentrancy assert; post-fix it defers, and the next server tick's
 * drain disconnects the client cleanly. Found by fuzz_server_dispatch once the
 * JOIN gate was opened.
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
#include "channel_mux.h"           /* CHANNEL_CONTROL_WINDOW, _BACKLOG */
#include "transport_udp.h"         /* transportUdpServerTestPendingRemove */
#include "threads.h"               /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

#define JOIN_MAX        2000  /* join + map download on the clean path */
/* Each flood event is a CTRL_SERVER_TEXT with a full-length text, which costs
 * the backlog 2 (length) + 3 (type, bodyLen) + 2 (source, destination) + 127
 * (text) bytes. Enough of them fill the window and then the backlog, with a
 * margin, whatever the backlog is sized to. */
#define OVERFLOW_TEXT_LEN  127
#define OVERFLOW_MSG_BYTES (2 + 3 + 2 + OVERFLOW_TEXT_LEN)
#define OVERFLOW_EVENTS    (CHANNEL_CONTROL_WINDOW + \
                            CHANNEL_CONTROL_BACKLOG / OVERFLOW_MSG_BYTES + 16)
#define DRAIN_PUMPS     4     /* ticks for the deferred drain to remove the slot */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

int run_control_overflow_defers_disconnect(void) {
    LoopbackHarness h;
    ControlEvent    evt;
    int             numBefore;
    int             i;
    bool            flagged;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Overflow", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0xC0FFEEu),
                  "harness start failed");

    int connectedAt = loopbackHarnessPumpUntil(&h, JOIN_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", JOIN_MAX);
    }
    numBefore = (int)serverSimGetNumPlayers(h.sim);
    UT_ASSERT_MSG(numBefore >= 1, "server has no connected player after join");

    /* Flood the connected client's reliable control channel. We never pump the
     * client during the flood, so its acks never arrive: the send window fills,
     * then the backlog behind it, and the publish whose channelSend finds the
     * backlog full fails inside its own fan-out — the reentrancy this test pins.
     * Hold the tick mutex as serverInstanceTick would when it publishes. Pre-fix,
     * one of these publishes aborts the process on assert(!publishing). */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    memset(evt.u.serverText.text, 'x', OVERFLOW_TEXT_LEN);
    evt.u.serverText.text[OVERFLOW_TEXT_LEN] = '\0';
    evt.u.serverText.destPlayer = 0xFF;  /* every client, not slot 0 alone */

    threadsWaitForMutex();
    for (i = 0; i < OVERFLOW_EVENTS; i++) {
        serverSimPublishControl(h.sim, &evt);
    }
    threadsReleaseMutex();

    /* The flood really overflowed: the deferred-removal flag is set, and not
     * yet cleared, because no server tick has run since. Without this the
     * player count below could drop for some unrelated reason. */
    flagged = false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (transportUdpServerTestPendingRemove(i)) {
            flagged = true;
        }
    }
    if (!flagged) {
        loopbackHarnessStop(&h);
        UT_FAIL("%d control events did not overflow the client's control "
                "channel: no slot was flagged for a deferred disconnect",
                (int)OVERFLOW_EVENTS);
    }

    /* Reaching here at all means no synchronous disconnect re-entered the
     * publish. Now let the server run: serverInstanceTick drains the deferred
     * overflow-disconnect (outside any publish) and removes the slot. */
    for (i = 0; i < DRAIN_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }

    UT_ASSERT_MSG((int)serverSimGetNumPlayers(h.sim) < numBefore,
                  "overflowed client was not disconnected by the deferred drain "
                  "(numPlayers stayed %d)", (int)serverSimGetNumPlayers(h.sim));

    loopbackHarnessStop(&h);
    return 0;
}
