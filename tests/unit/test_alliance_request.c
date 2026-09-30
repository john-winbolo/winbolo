/*
 * Alliance requests at the command arms (issue #421).
 *
 * An accept allies two seats, so the server must only take one the new
 * member asked for. It used to take CMD_ALLIANCE_ACCEPT for any seat, in
 * any state and in a ranked game, so a client could ally itself with a
 * seat that never asked. The server now keeps who asked whom: a request
 * records it, an accept needs it and uses it up, and a seat leaving or a
 * round starting clears it. The three alliance commands are refused
 * outside a running round; in the lobby, teams are how sides change.
 *
 * On the client, a request from a seat that then empties is dropped, so
 * an accept cannot go to whoever takes that seat next.
 *
 *   1. An accept with no request is refused.
 *   2. Request then accept allies, and the request is used up.
 *   3. The requester leaves and a new player takes the seat: refused.
 *   4. An accept in a ranked game is refused, as the request is.
 *   5. Request, accept and leave are each refused in the lobby.
 *   6. A request does not outlive the round it was made in.
 *   7. The client drops a pending request when the requester leaves.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "players.h"
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* A running round, no lobby, with three people on no team. */
static ServerSim *arRunningSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Zero", false);
    serverSimAddPlayer(sim, 1, "One", false);
    serverSimAddPlayer(sim, 2, "Two", false);
    return sim;
}

static CmdResult arSend(ServerSim *sim, BYTE from, ClientCommand *cmd) {
    CmdResult r;
    cmd->cmdSeq = 1;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, from, cmd);
    threadsReleaseMutex();
    return r;
}

static CmdResult arRequest(ServerSim *sim, BYTE from, BYTE to) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_ALLIANCE_REQUEST;
    cmd.u.allianceRequest.toPlayer = to;
    return arSend(sim, from, &cmd);
}

static CmdResult arAccept(ServerSim *sim, BYTE accepter, BYTE newMember) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_ALLIANCE_ACCEPT;
    cmd.u.allianceAccept.newMember = newMember;
    return arSend(sim, accepter, &cmd);
}

static CmdResult arLeave(ServerSim *sim, BYTE from) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_ALLIANCE_LEAVE;
    return arSend(sim, from, &cmd);
}

static bool arAllied(ServerSim *sim, BYTE a, BYTE b) {
    return playersIsAllie(&sim->sim.plyrs, a, b) == TRUE;
}

/* 1. Nobody asked, so there is nothing to accept. */
int run_alliance_request_accept_without_request_refused(void) {
    ServerSim *sim = arRunningSim();
    CmdResult r;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(sim->state == serverStateRunning);

    r = arAccept(sim, 1, 0);
    UT_ASSERT_MSG(r == CMD_REJECT_BAD_STATE,
                  "1 accepting 0, who never asked, returned %d", (int)r);
    UT_ASSERT_MSG(!arAllied(sim, 0, 1),
                  "an accept nobody asked for allied 0 and 1");

    /* A request to someone else is not a request to 1. */
    UT_ASSERT(arRequest(sim, 0, 2) == CMD_OK);
    r = arAccept(sim, 1, 0);
    UT_ASSERT_MSG(r == CMD_REJECT_BAD_STATE,
                  "1 accepted a request 0 made to 2 (returned %d)", (int)r);
    UT_ASSERT(!arAllied(sim, 0, 1));

    serverSimDestroy(sim);
    return 0;
}

/* 2. The ordinary path, and a request allies once. */
int run_alliance_request_then_accept_allies(void) {
    ServerSim *sim = arRunningSim();

    UT_ASSERT(sim != NULL);
    UT_ASSERT(arRequest(sim, 0, 1) == CMD_OK);
    UT_ASSERT_MSG(arAccept(sim, 1, 0) == CMD_OK,
                  "1 accepting 0's request was refused");
    UT_ASSERT_MSG(arAllied(sim, 0, 1), "an accepted request did not ally");

    /* 0 leaves, and the request that allied them is spent. */
    UT_ASSERT(arLeave(sim, 0) == CMD_OK);
    UT_ASSERT(!arAllied(sim, 0, 1));
    UT_ASSERT_MSG(arAccept(sim, 1, 0) == CMD_REJECT_BAD_STATE,
                  "an accepted request was accepted a second time");
    UT_ASSERT(!arAllied(sim, 0, 1));

    serverSimDestroy(sim);
    return 0;
}

/* 3. The request belongs to the player who made it, not to the seat. */
int run_alliance_request_requester_replaced_refused(void) {
    ServerSim *sim = arRunningSim();

    UT_ASSERT(sim != NULL);
    UT_ASSERT(arRequest(sim, 0, 1) == CMD_OK);
    threadsWaitForMutex();
    serverSimRemovePlayer(sim, 0);
    serverSimAddPlayer(sim, 0, "Newcomer", false);
    threadsReleaseMutex();
    UT_ASSERT(serverSimIsPlayerConnected(sim, 0));

    UT_ASSERT_MSG(arAccept(sim, 1, 0) == CMD_REJECT_BAD_STATE,
                  "1 accepted a request from a player who has left, and "
                  "allied the newcomer in that seat");
    UT_ASSERT(!arAllied(sim, 0, 1));

    /* A request the newcomer makes goes through as usual. */
    UT_ASSERT(arRequest(sim, 1, 0) == CMD_OK);
    UT_ASSERT(arAccept(sim, 0, 1) == CMD_OK);
    UT_ASSERT(arAllied(sim, 0, 1));

    serverSimDestroy(sim);
    return 0;
}

/* 4. Ranked games have no alliances, whichever command asks for one. */
int run_alliance_request_ranked_accept_refused(void) {
    ServerSim *sim = arRunningSim();

    UT_ASSERT(sim != NULL);
    /* Asked before the game became ranked, so the request went through. */
    UT_ASSERT(arRequest(sim, 0, 1) == CMD_OK);
    serverSimSetRanked(sim, true);

    UT_ASSERT_MSG(arAccept(sim, 1, 0) == CMD_REJECT_BAD_STATE,
                  "an accept in a ranked game went through");
    UT_ASSERT(!arAllied(sim, 0, 1));
    UT_ASSERT(arRequest(sim, 2, 1) == CMD_REJECT_BAD_STATE);

    serverSimDestroy(sim);
    return 0;
}

/* 5. The lobby changes sides with teams, not with alliance commands. */
int run_alliance_request_refused_in_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    CmdResult r;

    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Zero", false);
    serverSimAddPlayer(sim, 1, "One", false);
    UT_ASSERT(sim->state == serverStateLobby);

    r = arRequest(sim, 0, 1);
    UT_ASSERT_MSG(r == CMD_REJECT_BAD_STATE,
                  "a request in the lobby returned %d", (int)r);
    r = arAccept(sim, 1, 0);
    UT_ASSERT_MSG(r == CMD_REJECT_BAD_STATE,
                  "an accept in the lobby returned %d", (int)r);
    UT_ASSERT(!arAllied(sim, 0, 1));
    r = arLeave(sim, 0);
    UT_ASSERT_MSG(r == CMD_REJECT_BAD_STATE,
                  "a leave in the lobby returned %d", (int)r);

    serverSimDestroy(sim);
    return 0;
}

/* 6. A request made in one round cannot be accepted in the next. */
int run_alliance_request_cleared_at_round_start(void) {
    ServerSim *sim = arRunningSim();

    UT_ASSERT(sim != NULL);
    UT_ASSERT(arRequest(sim, 0, 1) == CMD_OK);
    threadsWaitForMutex();
    serverSimStartGame(sim);
    threadsReleaseMutex();
    UT_ASSERT(sim->state == serverStateRunning);
    UT_ASSERT(serverSimIsPlayerConnected(sim, 0));
    UT_ASSERT(serverSimIsPlayerConnected(sim, 1));

    UT_ASSERT_MSG(arAccept(sim, 1, 0) == CMD_REJECT_BAD_STATE,
                  "last round's request was accepted in this one");
    UT_ASSERT(!arAllied(sim, 0, 1));

    serverSimDestroy(sim);
    return 0;
}

/* 7. The client forgets a request whose sender has gone. */
int run_alliance_request_client_drops_departed_requester(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    ControlEvent evt;
    BYTE i;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 1);
    gs = clientSimGetGameSim(cs);
    for (i = 0; i < 4; i++) {
        gs->plyrs->item[i].inUse = TRUE;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_REQUEST;
    evt.u.allianceRequest.fromPlayer = 3;
    evt.u.allianceRequest.toPlayer   = 1;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetPendingAllianceRequest(cs) == 3);

    /* Somebody else leaving leaves it alone. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 2;
    evt.u.playerLeave.quiet = 1;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetPendingAllianceRequest(cs) == 3,
                  "another seat leaving dropped the request from 3");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 3;
    evt.u.playerLeave.quiet = 1;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetPendingAllianceRequest(cs) == 0xFF,
                  "the request from 3 outlived 3 leaving (still %u)",
                  (unsigned)clientSimGetPendingAllianceRequest(cs));

    clientSimDestroy(cs);
    return 0;
}
