/*
 * Spectator command isolation — a tankless viewer may chat, and nothing else.
 *
 * A spectator connects through the real client transport (loopback harness) in
 * the LOBBY and rests in CLIENT_CONNECT_SPECTATING with no tank slot and no sim
 * player. Opening the command-send path for a spectator (so it can chat) makes
 * the old premise — "the client transport refuses to emit, so nothing reaches
 * the server" — false. The isolation invariant is now pinned from three sides:
 *
 *   1. Chat IS accepted. The spectator sends a broadcast chat the production way
 *      (clientSimNetSendChat); the server's spectator inbound branch accepts
 *      CMD_CHAT in lobby/countdown and publishes a CTRL_SPECTATOR_CHAT, which
 *      round-trips back to the viewer and lands in its own lobby chat log. This
 *      proves the open send path end to end.
 *
 *   2. Nothing else takes effect. The viewer fires a lobby command (CMD_READY)
 *      and a gameplay command (CMD_ALLIANCE_LEAVE) through the same clientSimNet*
 *      entry points a real player uses. The 8e read-only guards stop the client
 *      originating them, and the server's spectator branch would reject any that
 *      reached it (only CMD_CHAT, only in lobby/countdown) — so the server shows
 *      no effect: still one spectator, zero players, the viewer never promoted.
 *
 *   3. Structurally, at the dispatcher: serverSimApplyCommand resolves a sender
 *      only by player-table slot. A spectator is never in that table, so its
 *      identity can only ever present as an out-of-range slot. Calling the
 *      dispatcher with such a slot must reject with CMD_REJECT_INVALID (the
 *      entry guard), never index sim state. This is the server-side gate that
 *      would still hold even if a forged command reached the dispatcher.
 *
 * serverFindSpectator is static to the transport TU, so the structural half
 * asserts the dispatcher guard directly rather than the resolver.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAX_TANKS */
#include "client_sim.h"
#include "client_net.h"        /* clientSimNetSendChat / SendReady / SendAllianceLeave */
#include "client_command.h"    /* ClientCommand, CMD_READY, CMD_REJECT_INVALID */
#include "client_connect_state.h"
#include "server_sim.h"        /* serverSimApplyCommand, serverSimGetNumPlayers */
#include "threads.h"           /* threadsWaitForMutex / threadsReleaseMutex */
#include "transport_udp.h"     /* transportUdpServerGetSpectatorCount */
#include "test_harness.h"
#include "loopback_harness.h"

/* Spectator handshake has no map download; this generous cap keeps a hang
 * distinguishable from a slow pass. */
#define SPEC_CMD_CONNECT_MAX 1200
/* Round-trip / settle budgets (pumpUntil returns early when the pred holds). */
#define SPEC_CMD_ECHO_PUMPS   400
#define SPEC_CMD_SETTLE_PUMPS  60

static bool pred_spectating(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

static bool pred_chat_echoed(LoopbackHarness *h, void *user) {
    const char *needle = (const char *)user;
    const char *hist = clientSimGetLobbyChatHistory(h->cs);
    return hist != NULL && strstr(hist, needle) != NULL;
}

int run_spectator_command_reject(void) {
    LoopbackHarness h;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "Spectator", /*seed*/ 1u),
                  "lobby-spectator harness start failed");

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

    /* (1) Chat IS accepted. The spectator's broadcast chat round-trips back as
     * CTRL_SPECTATOR_CHAT and lands in its own lobby chat log. There is no local
     * echo for a spectator, so the line can only appear via the server path. */
    clientSimNetSendChat(h.cs, 0xFF, "watching");
    int echoedAt = loopbackHarnessPumpUntil(&h, SPEC_CMD_ECHO_PUMPS,
                                            pred_chat_echoed, (void *)"watching");
    UT_ASSERT_MSG(echoedAt > 0,
                  "spectator broadcast chat never round-tripped into the lobby log");

    /* (2) Nothing else takes effect. A real player readies / leaves an alliance
     * through exactly these calls; for a tankless viewer the 8e guards refuse to
     * originate them and the server's spectator branch admits only chat. */
    clientSimNetSendReady(h.cs, true);
    clientSimNetSendAllianceLeave(h.cs);
    (void)loopbackHarnessPumpUntil(&h, SPEC_CMD_SETTLE_PUMPS, NULL, NULL);

    UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == 0,
                  "non-chat spectator command must not create a player slot");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "non-chat spectator command must not change the spectator roster");
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_SPECTATING,
                  "spectator must stay SPECTATING, never promote to a player state");

    /* (3) Structural dispatcher gate. A spectator is never in the player table,
     * so it can only present as an out-of-range slot. The dispatcher must reject
     * such a sender with CMD_REJECT_INVALID before touching sim state — a
     * connected player's slot is always in [0, MAX_TANKS) and is unaffected by
     * this guard. */
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
