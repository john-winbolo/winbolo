/*
 * CMD_LOBBY_TRANSFER_HOST dispatch authority and validation
 * (test_lobby_transfer_host.c). The host may hand the role to another
 * connected human; openHost does NOT grant transfer; bots, self, and
 * unconnected slots are rejected.
 */

#include <stdint.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "everard_map.h"
#include "client_command.h"          /* CMD_LOBBY_TRANSFER_HOST, CmdResult */
#include "server_sim.h"
#include "server_sim_internal.h"      /* sim->botMgr for the bot-target case */
#include "server_sim_lifecycle.h"     /* serverSimSetLobbyEnabled, SetOpenHost */
#include "threads.h"
#include "test_harness.h"

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

static CmdResult transfer(ServerSim *sim, int senderSlot, uint8_t target) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_TRANSFER_HOST;
    cmd.cmdSeq = 1;
    cmd.u.lobbyTransferHost.slot = target;
    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Host (slot 0) hands the role to another connected human. */
int run_transfer_host_promotes_target(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 2, "Player2", false);
    UT_ASSERT_MSG(transfer(sim, 0, 2) == CMD_OK, "host -> human must be CMD_OK");
    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == 2,
                  "hostSlot must move to the target (got %u)",
                  (unsigned)serverSimGetHostSlot(sim));
    serverSimDestroy(sim);
    return 0;
}

/* A non-host sender is rejected. */
int run_transfer_host_rejects_non_host_sender(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 2, "Player2", false);
    serverSimAddPlayer(sim, 3, "Player3", false);
    UT_ASSERT_MSG(transfer(sim, 2, 3) == CMD_REJECT_NOT_HOST,
                  "non-host sender must be CMD_REJECT_NOT_HOST");
    UT_ASSERT(serverSimGetHostSlot(sim) == 0);
    serverSimDestroy(sim);
    return 0;
}

/* openHost must NOT grant transfer — the gate is senderSlot == hostSlot,
 * not lobbyClientMayEdit. */
int run_transfer_host_openhost_does_not_grant(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 2, "Player2", false);
    serverSimAddPlayer(sim, 3, "Player3", false);
    serverSimSetOpenHost(sim, true);
    UT_ASSERT_MSG(transfer(sim, 2, 3) == CMD_REJECT_NOT_HOST,
                  "openHost must not let a non-host transfer the role");
    serverSimDestroy(sim);
    return 0;
}

/* Target == self (the host) is rejected. */
int run_transfer_host_rejects_self(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    UT_ASSERT_MSG(transfer(sim, 0, 0) == CMD_REJECT_INVALID,
                  "transferring to self must be CMD_REJECT_INVALID");
    serverSimDestroy(sim);
    return 0;
}

/* An unconnected target slot is rejected. */
int run_transfer_host_rejects_unconnected(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    UT_ASSERT_MSG(transfer(sim, 0, 5) == CMD_REJECT_INVALID,
                  "unconnected target must be CMD_REJECT_INVALID");
    serverSimDestroy(sim);
    return 0;
}

/* A bot target is rejected — bots can never host. */
int run_transfer_host_rejects_bot_target(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    /* Fake a connected bot at slot 2 (a real bot needs a brain file).
     * serverSimIsBot answers yes to a live bot pool entry or to a roster
     * seat marked as a bot; the pool flag is the half a fake bot can set
     * without the brain. */
    sim->playerConnected[2] = TRUE;
    sim->botMgr.bots[2].active = true;
    UT_ASSERT(serverSimIsBot(sim, 2) == true);
    UT_ASSERT_MSG(transfer(sim, 0, 2) == CMD_REJECT_INVALID,
                  "bot target must be CMD_REJECT_INVALID");
    serverSimDestroy(sim);
    return 0;
}
