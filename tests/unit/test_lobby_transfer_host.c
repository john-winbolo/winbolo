/*
 * CMD_LOBBY_TRANSFER_HOST dispatch authority and validation
 * (test_lobby_transfer_host.c). The host may hand the role to another
 * connected human; openHost does NOT grant transfer; bots, self, and
 * unconnected slots are rejected. Also the -firstjoinhost promotion, which
 * moves the role the other way: onto a joiner, when a bot holds it.
 */

#include <stdint.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "everard_map.h"
#include "client_command.h"          /* CMD_LOBBY_TRANSFER_HOST, CmdResult */
#include "player_flags.h"            /* CLIENT_TYPE_UNKNOWN */
#include "server_sim.h"
#include "server_sim_internal.h"      /* sim->botMgr for the bot-target case */
#include "server_sim_join.h"          /* serverSimLocalJoin, LocalJoinResult */
#include "server_sim_lifecycle.h"     /* serverSimSetLobbyEnabled, SetOpenHost */
#include "threads.h"
#include "test_harness.h"

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
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

/* -firstjoinhost promotion. A dedicated server fills its low slots with bots
 * before anybody arrives, so slot 0 — where the role sits — is a bot. Fake
 * one the same way the bot-target case above does. */
static void seat_bot(ServerSim *sim, BYTE slot) {
    sim->playerConnected[slot] = TRUE;
    sim->botMgr.bots[slot].active = true;
}

static CmdResult clear_team(ServerSim *sim, int senderSlot) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_TEAM_CLEAR;
    cmd.cmdSeq = 1;
    cmd.u.lobbyTeamClear.teamId = 1;
    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* The first person onto a bots-only server takes the role off the bot, and
 * the lobby commands open up to them. */
int run_firstjoinhost_promotes_over_bot(void) {
    ServerSim *sim = make_lobby_sim();
    BYTE slot = 0xFF;
    UT_ASSERT(sim != NULL);
    serverSimSetFirstJoinerBecomesHost(sim, true);
    seat_bot(sim, 0);
    UT_ASSERT(serverSimIsBot(sim, 0) == true);
    UT_ASSERT(serverSimGetHostSlot(sim) == 0);

    UT_ASSERT_MSG(serverSimLocalJoin(sim, "Arrival", "", CLIENT_TYPE_UNKNOWN,
                                     0, &slot) == LOCAL_JOIN_OK,
                  "the join must be accepted");
    UT_ASSERT_MSG(slot != 0, "the bot holds slot 0, so the joiner sits above it");
    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == slot,
                  "the joiner must take the host role from the bot "
                  "(host=%u, joined=%u)",
                  (unsigned)serverSimGetHostSlot(sim), (unsigned)slot);
    UT_ASSERT_MSG(clear_team(sim, slot) == CMD_OK,
                  "the promoted joiner must be able to edit the lobby");
    serverSimDestroy(sim);
    return 0;
}

/* Without the flag the role stays on the bot and the joiner has no rights. */
int run_firstjoinhost_off_leaves_host_alone(void) {
    ServerSim *sim = make_lobby_sim();
    BYTE slot = 0xFF;
    UT_ASSERT(sim != NULL);
    seat_bot(sim, 0);
    UT_ASSERT(serverSimGetFirstJoinerBecomesHost(sim) == false);

    UT_ASSERT(serverSimLocalJoin(sim, "Arrival", "", CLIENT_TYPE_UNKNOWN,
                                 0, &slot) == LOCAL_JOIN_OK);
    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == 0,
                  "without -firstjoinhost the role must stay put (host=%u)",
                  (unsigned)serverSimGetHostSlot(sim));
    UT_ASSERT_MSG(clear_team(sim, slot) == CMD_REJECT_NOT_HOST,
                  "an unpromoted joiner must not be able to edit the lobby");
    serverSimDestroy(sim);
    return 0;
}

/* The promotion fires once: whoever holds the role keeps it. */
int run_firstjoinhost_second_joiner_does_not_displace(void) {
    ServerSim *sim = make_lobby_sim();
    BYTE first = 0xFF, second = 0xFF;
    UT_ASSERT(sim != NULL);
    serverSimSetFirstJoinerBecomesHost(sim, true);
    seat_bot(sim, 0);

    UT_ASSERT(serverSimLocalJoin(sim, "First", "", CLIENT_TYPE_UNKNOWN,
                                 0, &first) == LOCAL_JOIN_OK);
    UT_ASSERT(serverSimGetHostSlot(sim) == first);

    UT_ASSERT(serverSimLocalJoin(sim, "Second", "", CLIENT_TYPE_UNKNOWN,
                                 0, &second) == LOCAL_JOIN_OK);
    UT_ASSERT(second != first);
    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == first,
                  "the second joiner must not take the role (host=%u, first=%u)",
                  (unsigned)serverSimGetHostSlot(sim), (unsigned)first);
    UT_ASSERT_MSG(clear_team(sim, second) == CMD_REJECT_NOT_HOST,
                  "the second joiner must not be able to edit the lobby");
    serverSimDestroy(sim);
    return 0;
}
