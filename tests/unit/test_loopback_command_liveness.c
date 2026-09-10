/*
 * A command packet refreshes the sending client's liveness clock.
 *
 * transportUdpServerCheckTimeouts disconnects any slot whose lastReceivedTick
 * has not moved for more than CLIENT_TIMEOUT_TICKS, and the connected-client
 * branch of handleCommandTick is the refresh site under test: a client whose
 * only traffic is commands must not age toward that timeout while it is
 * talking to the server.
 *
 * The obvious test — leave a lobby client running past CLIENT_TIMEOUT_TICKS
 * and check it survives — proves nothing here, because two other things keep
 * the clock fresh on their own:
 *   - the client pings every PING_INTERVAL_TICKS (20), and serverHandlePing
 *     refreshes the clock;
 *   - the lobby republishes CTRL_LOBBY_SLOT for every connected slot every 250
 *     transport ticks (the ping-column heartbeat in server_lifecycle.c), which
 *     is a reliable CHANNEL_CONTROL message; the client acks it on its next
 *     standalone PACKET_CHANNEL, and handleChannel refreshes the clock too.
 * Both are well inside the 1000-tick timeout, so the slot survives either way.
 *
 * So this asserts the property directly instead. Pings are suppressed for this
 * client (WB_NOPING), the test then waits for a window in which the clock is
 * observably standing still — no ping, and the 250-tick republish not due —
 * and sends one command into that quiet window. The clock must move. The wait
 * bound is short enough that the next republish cannot land inside it, so an
 * advance is attributable to the command and nothing else.
 *
 * CMD_PLAYER_MUTE is the command sent: it is the one lobby command the server
 * answers without publishing a control event (server_command_dispatch.c), so
 * it draws no reliable traffic back that the client would ack — which would
 * refresh the clock by the channel path and hide what is being measured.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"            /* clientSimNetSendPlayerMute */
#include "client_connect_state.h"
#include "transport_udp.h"         /* transportUdpServerTestLastReceivedTick */
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX      2000   /* join + map download under loss */
#define SETTLE_PUMPS      300   /* let the join control-replay drain and ack */
#define QUIET_SEARCH_MAX 2000   /* generous: the republish period is 250 ticks */
#define QUIET_RUN          40   /* consecutive unmoved ticks that mark it quiet */
#define REFRESH_MAX       120   /* bound on the command's refresh landing */

/* QUIET_RUN + REFRESH_MAX stays under the 250-tick republish period, so the
 * heartbeat that last moved the clock cannot come round again inside the wait
 * below — whatever moves it there was sent by this test. */

/* Set (or clear) the WB_NOPING override the client reads at connect time.
 * Mirrors the harness's own WB_NETIMPAIR handling; the harness knows nothing
 * about this variable, so the test owns both ends of it. */
static void set_ping_suppressed(bool on) {
#ifdef _WIN32
    _putenv_s("WB_NOPING", on ? "1" : "");
#else
    if (on) {
        setenv("WB_NOPING", "1", 1);
    } else {
        unsetenv("WB_NOPING");
    }
#endif
}

/* Tear down the harness and drop the ping override together. Stop clears
 * WB_NETIMPAIR but not WB_NOPING, and a leaked override would follow every
 * later test in this process. */
static void liveness_teardown(LoopbackHarness *h) {
    loopbackHarnessStop(h);
    set_ping_suppressed(false);
}

/* Connected AND in the lobby phase. The phase is published over
 * CHANNEL_CONTROL, which lands a round-trip after the client reports
 * CONNECTED, so wait for both rather than asserting the phase on connect. */
static bool pred_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsInLobby(h->cs);
}

int run_loopback_command_liveness(void) {
    LoopbackHarness h;
    int connectedAt;
    int slot;
    BYTE target;
    uint32_t seen = 0;
    uint32_t t0 = 0;
    int quietRun = 0;
    int i;
    bool refreshed = false;

    /* Before Start: the client reads WB_NOPING once, at ctx create. */
    set_ping_suppressed(true);

    if (!loopbackHarnessStart(&h, "CmdLive", /*lobbyMode*/ true,
                              /*impairSpec*/ "loss=5,burst=2",
                              /*seed*/ 0x11FE5EEDu)) {
        liveness_teardown(&h);
        UT_FAIL("harness start (command liveness) failed");
    }

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_in_lobby, NULL);
    if (connectedAt < 0) {
        liveness_teardown(&h);
        UT_FAIL("client never reached the lobby phase within %d pumps",
                CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Let the join control-replay finish delivering and being acked, so the
     * only thing still moving the clock afterwards is the periodic republish. */
    for (i = 0; i < SETTLE_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }

    seen = transportUdpServerTestLastReceivedTick(slot);
    if (seen == 0) {
        liveness_teardown(&h);
        UT_FAIL("server slot %d is not connected after settling", slot);
    }

    /* Find the quiet window: pump until the clock has stood still for
     * QUIET_RUN ticks in a row. Reaching that means the last refresh was
     * exactly QUIET_RUN ticks ago, which places the next republish a long way
     * off and leaves the wait below unambiguous. */
    for (i = 1; i <= QUIET_SEARCH_MAX; i++) {
        uint32_t now;
        loopbackHarnessPump(&h);
        now = transportUdpServerTestLastReceivedTick(slot);
        if (now == 0) {
            liveness_teardown(&h);
            UT_FAIL("server dropped slot %d while waiting for a quiet window",
                    slot);
        }
        if (now != seen) {
            seen = now;
            quietRun = 0;
            continue;
        }
        quietRun++;
        if (quietRun >= QUIET_RUN) break;
    }
    if (quietRun < QUIET_RUN) {
        liveness_teardown(&h);
        UT_FAIL("liveness clock never held still for %d ticks within %d pumps "
                "(last value %u) — something other than this test is "
                "refreshing it", QUIET_RUN, QUIET_SEARCH_MAX, (unsigned)seen);
    }
    t0 = seen;

    /* One command into the quiet window. Any slot but our own: the server
     * rejects a self-mute, and it rejects a mute of an empty slot too — both
     * land past the refresh site, which is what makes this a fair test of a
     * command packet rather than of a command's effect. */
    target = (BYTE)((slot + 1) % MAX_TANKS);
    clientSimNetSendPlayerMute(h.cs, target, true);

    for (i = 1; i <= REFRESH_MAX; i++) {
        uint32_t now;
        loopbackHarnessPump(&h);
        now = transportUdpServerTestLastReceivedTick(slot);
        if (now == 0) {
            liveness_teardown(&h);
            UT_FAIL("server dropped slot %d while awaiting the command refresh",
                    slot);
        }
        if (now > t0) {
            seen = now;
            refreshed = true;
            break;
        }
    }

    fprintf(stderr, "  command liveness: quiet at tick %u, refreshed to %u "
                    "after %d pump(s) (bound %d)\n",
            (unsigned)t0, (unsigned)seen, i, REFRESH_MAX);

    if (!refreshed) {
        liveness_teardown(&h);
        UT_FAIL("a command from slot %d did not refresh its liveness clock: "
                "still %u after %d pumps — the command path leaves "
                "lastReceivedTick untouched, so a client whose only traffic is "
                "commands ages toward the timeout", slot, (unsigned)t0,
                REFRESH_MAX);
    }

    liveness_teardown(&h);
    return 0;
}
