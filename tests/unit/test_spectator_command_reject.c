/*
 * Spectator command rejection — a tankless viewer cannot mutate server state.
 *
 * A spectator connects through the real client transport (loopback harness)
 * and rests in CLIENT_CONNECT_SPECTATING with no tank slot and no sim player.
 * This test pins the isolation invariant from two sides:
 *
 *   1. End-to-end, the production way: the spectator attempts a lobby command
 *      (CMD_READY) and a gameplay command (CMD_ALLIANCE_LEAVE) through the
 *      same clientSimNet* entry points a real player uses. The client
 *      transport only emits commands once joinState is CONNECTED, so a
 *      tankless viewer never puts one on the wire; after pumping, the server
 *      shows no effect — still one spectator, zero players, the viewer never
 *      promoted to a slot.
 *
 *   2. Structurally, at the dispatcher: serverSimApplyCommand resolves a
 *      sender only by player-table slot. A spectator is never in that table,
 *      so its identity can only ever present as an out-of-range slot. Calling
 *      the dispatcher with such a slot must reject with CMD_REJECT_INVALID
 *      (the entry guard), never index sim state. This is the server-side gate
 *      that would still hold even if a forged command reached the dispatcher.
 *
 * serverFindClient is static to the transport TU, so the structural half
 * asserts the dispatcher guard directly rather than the resolver.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAX_TANKS */
#include "client_sim.h"
#include "client_net.h"        /* clientSimNetSendReady / SendAllianceLeave */
#include "client_command.h"    /* ClientCommand, CMD_READY, CMD_REJECT_INVALID */
#include "client_connect_state.h"
#include "server_sim.h"        /* serverSimApplyCommand, serverSimGetNumPlayers */
#include "threads.h"           /* threadsWaitForMutex / threadsReleaseMutex */
#include "transport_udp.h"     /* transportUdpServerGetSpectatorCount */
#include "test_harness.h"
#include "loopback_harness.h"

/* Spectator handshake has no map download; this generous cap keeps a hang
 * distinguishable from a slow pass. */
#define SPEC_CMD_CONNECT_MAX 600
/* Ticks to let any (errantly emitted) spectator command land and apply. */
#define SPEC_CMD_SETTLE_PUMPS 30

static bool pred_spectating(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

int run_spectator_command_reject(void) {
    LoopbackHarness h;

    UT_ASSERT_MSG(loopbackHarnessStartSpectator(&h, "Spectator", /*seed*/ 1u),
                  "spectator harness start failed");

    int spectatingAt = loopbackHarnessPumpUntil(&h, SPEC_CMD_CONNECT_MAX,
                                                 pred_spectating, NULL);
    if (spectatingAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator client never reached SPECTATING within %d pumps",
                SPEC_CMD_CONNECT_MAX);
    }

    /* Baseline: one tankless viewer, no players. */
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "setup: server must count exactly one spectator");
    UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == 0,
                  "setup: spectator must consume no tank slot");

    /* (1) Production-path attempt. A real player readies / leaves an alliance
     * through exactly these calls; for a tankless viewer the client transport
     * refuses to emit (joinState is SPECTATING, not CONNECTED), so nothing
     * reaches the server. */
    clientSimNetSendReady(h.cs, true);
    clientSimNetSendAllianceLeave(h.cs);
    (void)loopbackHarnessPumpUntil(&h, SPEC_CMD_SETTLE_PUMPS, NULL, NULL);

    /* No server-state effect: the viewer gained no slot, is still a spectator,
     * and never crossed into a player connect state. */
    UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == 0,
                  "spectator command must not create a player slot");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "spectator command must not change the spectator roster");
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_SPECTATING,
                  "spectator must stay SPECTATING, never promote to a player state");

    /* (2) Structural dispatcher gate. A spectator is never in the player
     * table, so it can only present as an out-of-range slot. The dispatcher
     * must reject such a sender with CMD_REJECT_INVALID before touching sim
     * state — a connected player's slot is always in [0, MAX_TANKS) and is
     * unaffected by this guard. */
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_READY;
    cmd.u.ready.ready = true;

    threadsWaitForMutex();
    CmdResult rHigh = serverSimApplyCommand(h.sim, MAX_TANKS, &cmd);
    CmdResult rNeg  = serverSimApplyCommand(h.sim, -1, &cmd);
    threadsReleaseMutex();

    UT_ASSERT_MSG(rHigh == CMD_REJECT_INVALID,
                  "dispatcher must reject a sender slot >= MAX_TANKS");
    UT_ASSERT_MSG(rNeg == CMD_REJECT_INVALID,
                  "dispatcher must reject a negative sender slot");

    /* The rejected dispatch calls left the roster untouched. */
    UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == 0,
                  "rejected dispatch must not create a player slot");

    loopbackHarnessStop(&h);
    return 0;
}
