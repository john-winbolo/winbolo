/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Sides during a round (issue #424).
 *
 * The server keeps two records of who is on whose side: the alliance
 * bitmap in players, which every game rule reads, and the lobby team,
 * which surrender, pings, the round log and scenarios read. These cases
 * hold what a change of side does to both.
 *
 * The first group holds the game rules. They assert alliances, owners and
 * what a pillbox or a base does, never a team number, so they hold
 * whichever record the server keeps:
 *
 *   accept_merges_groups      — an accept allies two whole teams, and a
 *                               client fed the accept ends with the same
 *                               table
 *   clear_moves_no_owner      — playersClearAlliance takes the bits only
 *   pill_fires_at_ex_ally     — a pillbox holds fire at its owner's ally,
 *                               fires at an enemy, and at the ex-ally
 *                               after a leave
 *   base_refuels_ally_only    — a base refuels an ally, not an enemy, and
 *                               not the ex-ally after a leave
 *   allies_share_base_win     — two allies holding every base between them
 *                               win the round
 *   client_takes_slot_in_game — a client takes a lobby slot and a team's
 *                               settings while the round runs
 *
 * The second group holds what a change of side does to the lobby team:
 *
 *   fold_at_return_to_lobby   — the teams the next lobby opens with
 *   surrender_after_defection — who votes in a team's surrender
 *   ping_after_leave          — who a ping reaches
 *   track_identity_after_defection — the team the round log names
 *   midgame_joiner_team       — the team a player joining mid-round gets
 *   set_team_keeps_player_alliance — a script moving one seat
 *
 * The third group holds the one way a side changes in a round:
 *
 *   accept_moves_whole_team   — an accept moves the new member's whole
 *                               team onto the accepter's
 *   accept_from_no_team       — an accepter on no team takes a free one
 *   accept_same_side_says_nothing — an accept inside one side is silent
 *   leave_takes_free_team     — a leaver takes the lowest free team
 *   leave_with_no_free_team   — with every id held, the leaver has none
 *   free_team_rules           — the id, colour and name a free team gets
 *   team_chat_follows_sides   — team chat goes with the side
 *   panels_follow_team        — scenario panels go with the side
 *   infection_puts_leaver_back — Infection keeps a seat on its side
 *   rejoin_is_a_joiner        — a rejoin gets a free team and its neutral
 *                               pills
 *   joiner_slot_before_join   — a joiner's team goes out before its join
 *   lobby_joiner_keeps_rule   — the lobby keeps the smallest-team pick
 *   lobby_drops_empty_teams   — the next lobby does not list a team the
 *                               round opened and emptied
 *   held_seat_allied_in_round — a held seat added in a round is allied
 *                               with its team
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "client_command.h"
#include "control_event.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server/sim/server_sim_shared.h"
#include "start_sides.h"
#include "scenario_host.h"
#include "server_sim_scenario.h"
#include "scenario_panel.h"
#include "threads.h"
#include "players.h"
#include "pillbox.h"
#include "bases.h"
#include "tank.h"
#include "everard_map.h"
#include "test_harness.h"

/* ── Fixtures ───────────────────────────────────────────────────────── */

/* A running round with a lobby: seat i on teams[i], allied by team at the
   start. */
static ServerSim *tsRunningSim(const BYTE *teams, int numSeats) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    int i;

    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    for (i = 0; i < numSeats; i++) {
        char name[16];
        snprintf(name, sizeof(name), "P%d", i);
        serverSimAddPlayer(sim, (BYTE)i, name, false);
    }
    for (i = 0; i < numSeats; i++) {
        serverSimSetTeamBatch(sim, (BYTE)i, teams[i]);
    }
    serverSimStartGameInPlace(sim);
    if (sim->state != serverStateRunning) {
        serverSimDestroy(sim);
        return NULL;
    }
    return sim;
}

static bool tsAllied(ServerSim *sim, BYTE a, BYTE b) {
    return playersIsAllie(&sim->sim.plyrs, a, b) == TRUE;
}

static BYTE tsTeam(ServerSim *sim, BYTE slot) {
    return sim->lobbyPlayers[slot].teamNumber;
}

/* Each seat's allies, as a bit per seat, for seats 0..n-1. */
static void tsMatrix(players *plyrs, int n, uint16_t *out) {
    int i, j;
    for (i = 0; i < n; i++) {
        out[i] = 0;
        for (j = 0; j < n; j++) {
            if (i != j && playersIsAllie(plyrs, (BYTE)i, (BYTE)j)) {
                out[i] |= (uint16_t)(1u << j);
            }
        }
    }
}

/* Keeps the last alliance accept the server published. */
typedef struct {
    int          accepts;
    ControlEvent lastAccept;
} TsAcceptCapture;

static void tsCaptureAccept(void *ctx, const ControlEvent *evt) {
    TsAcceptCapture *c = (TsAcceptCapture *)ctx;
    if (evt->type == CTRL_ALLIANCE_ACCEPT) {
        c->accepts++;
        c->lastAccept = *evt;
    }
}

/* Put pill 0 beside tank victim, owned by owner and ready to fire, and run
   one pillsUpdate with only the victim connected. True if the pill picked it
   as a target. The same check test_alliance_reset.c makes. */
static bool tsPillTargets(ServerSim *sim, BYTE owner, BYTE victim) {
    GameSim *gs = &sim->sim;
    WORLD tx, ty;
    bool conn[MAX_TANKS];
    BYTE t;

    tankGetWorld(&gs->tanks[victim], &tx, &ty);
    gs->pb->item[0].x        = (BYTE)((tx >> TANK_SHIFT_MAPSIZE) + 1);
    gs->pb->item[0].y        = (BYTE)(ty >> TANK_SHIFT_MAPSIZE);
    gs->pb->item[0].owner    = owner;
    gs->pb->item[0].armour   = 15;
    gs->pb->item[0].inTank   = FALSE;
    gs->pb->item[0].reload   = gs->pb->item[0].speed;
    gs->pb->item[0].justSeen = FALSE;
    gs->pb->active[0]        = TRUE;
    for (t = 0; t < MAX_TANKS; t++) {
        conn[t] = (t == victim);
    }
    pillsUpdate(gs, gs->tanks, conn, MAX_TANKS);
    return gs->pb->item[0].justSeen == TRUE;
}

/* True if base 1, owned by owner and full, gives armour to tank slot. */
static bool tsBaseRefuels(ServerSim *sim, BYTE owner, BYTE slot) {
    GameSim *gs = &sim->sim;
    tank *tnk = &gs->tanks[slot];
    BYTE before;

    gs->bs->item[0].owner      = owner;
    gs->bs->item[0].armour     = (BYTE)gs->rules.base_full_armour;
    gs->bs->item[0].shells     = (BYTE)gs->rules.base_full_shells;
    gs->bs->item[0].mines      = (BYTE)gs->rules.base_full_mines;
    gs->bs->item[0].refuelTime = 0;
    tankSetArmour(tnk, 10);
    before = tankGetArmour(tnk);
    basesRefueling(gs, tnk, 1);
    return tankGetArmour(tnk) > before;
}

/* ================================================================
 * The game rules.
 * ================================================================ */

/* Two teams of two; one member of each allies with the other, and all four
   are one side. A client holding the same world and fed the accept the
   server published ends with the same table. */
int run_team_sides_accept_merges_groups(void) {
    const BYTE teams[4] = { 1, 1, 2, 2 };
    ServerSim *sim = tsRunningSim(teams, 4);
    TsAcceptCapture cap;
    SubscriberHandle h;
    uint16_t srv[4], cli[4];
    ClientSim *cs;
    GameSim *cliGs;
    int i, j;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(tsAllied(sim, 0, 1) && tsAllied(sim, 2, 3));
    UT_ASSERT(!tsAllied(sim, 0, 2));

    memset(&cap, 0, sizeof(cap));
    h = serverSimRegisterSubscriber(sim, tsCaptureAccept, &cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    memset(&cap, 0, sizeof(cap));

    serverSimAcceptAlliance(sim, 2, 0);
    UT_ASSERT_MSG(cap.accepts == 1, "the accept published %d events",
                  cap.accepts);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (i == j) continue;
            UT_ASSERT_MSG(tsAllied(sim, (BYTE)i, (BYTE)j),
                          "after the accept %d is not allied with %d", i, j);
        }
    }
    tsMatrix(&sim->sim.plyrs, 4, srv);
    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);

    /* The client, as it stood before the accept. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 3);
    cliGs = clientSimGetGameSim(cs);
    for (i = 0; i < 4; i++) {
        cliGs->plyrs->item[i].inUse = TRUE;
    }
    playersAcceptAlliance(cliGs, &cliGs->plyrs, 3, 0, 1, FALSE);
    playersAcceptAlliance(cliGs, &cliGs->plyrs, 3, 2, 3, FALSE);
    clientSimApplyControl(cs, &cap.lastAccept);
    tsMatrix(&cliGs->plyrs, 4, cli);
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(cli[i] == srv[i],
                      "seat %d: the client has allies 0x%04x, the server "
                      "0x%04x", i, (unsigned)cli[i], (unsigned)srv[i]);
    }
    clientSimDestroy(cs);
    return 0;
}

/* playersClearAlliance takes a seat's alliance bits and moves no pillbox
   or base. */
int run_team_sides_clear_moves_no_owner(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);
    GameSim *gs;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    pillsSetNumPills(&gs->pb, 2);
    gs->pb->item[0].owner  = 0;
    gs->pb->item[0].inTank = FALSE;
    gs->pb->item[1].owner  = 1;
    gs->pb->item[1].inTank = FALSE;
    basesSetNumBases(&gs->bs, 2);
    gs->bs->item[0].owner = 0;
    gs->bs->item[1].owner = 1;

    playersClearAlliance(gs, &gs->plyrs, NEUTRAL, 0, TRUE);

    UT_ASSERT_MSG(!tsAllied(sim, 0, 1), "the clear left 0 allied with 1");
    UT_ASSERT(pillsGetPillOwner(&gs->pb, 1) == 0);
    UT_ASSERT(pillsGetPillOwner(&gs->pb, 2) == 1);
    UT_ASSERT(basesGetBaseOwner(&gs->bs, 1) == 0);
    UT_ASSERT(basesGetBaseOwner(&gs->bs, 2) == 1);
    serverSimDestroy(sim);
    return 0;
}

/* 0 and 1 are a side and 2 is the enemy. 0's pillbox holds fire at 1 and
   fires at 2; after 1 leaves, it fires at 1. */
int run_team_sides_pill_fires_at_ex_ally(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);

    UT_ASSERT(sim != NULL);
    UT_ASSERT(sim->sim.tanks[0] && sim->sim.tanks[1] && sim->sim.tanks[2]);
    UT_ASSERT_MSG(!tsPillTargets(sim, 0, 1),
                  "0's pillbox fires at 0's ally");
    UT_ASSERT_MSG(tsPillTargets(sim, 0, 2),
                  "0's pillbox holds fire at the enemy");

    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT_MSG(tsPillTargets(sim, 0, 1),
                  "0's pillbox holds fire at the player who left 0's side");
    serverSimDestroy(sim);
    return 0;
}

/* 0's base refuels 1, its ally, and not 2; after 1 leaves, not 1. */
int run_team_sides_base_refuels_ally_only(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);

    UT_ASSERT(sim != NULL);
    UT_ASSERT(sim->sim.tanks[0] && sim->sim.tanks[1] && sim->sim.tanks[2]);
    UT_ASSERT_MSG(tsBaseRefuels(sim, 0, 1), "0's base does not refuel 0's ally");
    UT_ASSERT_MSG(!tsBaseRefuels(sim, 0, 2), "0's base refuels the enemy");

    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT_MSG(!tsBaseRefuels(sim, 0, 1),
                  "0's base refuels the player who left 0's side");
    serverSimDestroy(sim);
    return 0;
}

/* Three seats on three teams. 0 and 1 ally in play, then hold every base
   between them, and the round is theirs. */
int run_team_sides_allies_share_base_win(void) {
    const BYTE teams[3] = { 1, 2, 3 };
    ServerSim *sim = tsRunningSim(teams, 3);
    GameSim *gs;
    BYTE n, i;

    UT_ASSERT(sim != NULL);
    serverSimAcceptAlliance(sim, 1, 0);
    UT_ASSERT(tsAllied(sim, 0, 1));

    gs = &sim->sim;
    n = basesGetNumBases(&gs->bs);
    UT_ASSERT(n >= 2);
    for (i = 0; i < n; i++) {
        gs->bs->item[i].owner  = (BYTE)(i & 1);
        gs->bs->item[i].armour = 50;
    }
    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "two allies holding every base did not win (state %d)",
                  (int)serverSimGetState(sim));
    serverSimDestroy(sim);
    return 0;
}

/* A client in a running round takes a lobby slot and a team's settings, so
   the in-game Surrender button and a scenario see a seat's new team. */
int run_team_sides_client_takes_slot_in_game(void) {
    ClientSim *cs = clientSimAlloc();
    ControlEvent evt;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_RUNNING;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(!clientSimIsInLobby(cs));

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_TEAM_META;
    evt.u.lobbyTeamMeta.teamId = 3;
    evt.u.lobbyTeamMeta.in_use = 1;
    evt.u.lobbyTeamMeta.color  = 2;
    snprintf(evt.u.lobbyTeamMeta.name, sizeof(evt.u.lobbyTeamMeta.name),
             "Team 3");
    clientSimApplyControl(cs, &evt);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SLOT;
    evt.u.lobbySlot.playerNum = 1;
    evt.u.lobbySlot.slot.connected = 1;
    evt.u.lobbySlot.slot.teamNumber = 3;
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimGetLobbySlot(cs, 1)->teamNumber == 3,
                  "the client did not take seat 1's team in game");
    UT_ASSERT(clientSimGetLobbyTeamInUse(cs, 3) == 1);
    UT_ASSERT(clientSimGetLobbyTeamColor(cs, 3) == 2);
    UT_ASSERT(strcmp(clientSimGetLobbyTeamName(cs, 3), "Team 3") == 0);
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * The lobby team.
 * ================================================================ */

/* Teams 1, 1, 2, 2. Seat 2 accepts seat 0, so all four are one side, then
   seat 1 leaves. The teams the next lobby opens with. */
int run_team_sides_fold_at_return_to_lobby(void) {
    const BYTE teams[4] = { 1, 1, 2, 2 };
    ServerSim *sim = tsRunningSim(teams, 4);
    BYTE got[4];
    BYTE i;

    UT_ASSERT(sim != NULL);
    serverSimAcceptAlliance(sim, 2, 0);
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimEnterGameOver(sim);
    serverSimReturnToLobby(sim);
    for (i = 0; i < 4; i++) {
        got[i] = tsTeam(sim, i);
    }
    /* The accept moved seats 0 and 1 onto seat 2's team, and the leave put
       seat 1 on the lowest free team. The lobby opens with the teams the
       round ended with: nothing folds them. */
    UT_ASSERT_MSG(got[0] == 2 && got[1] == 1 && got[2] == 2 && got[3] == 2,
                  "the lobby opened with teams %u %u %u %u",
                  got[0], got[1], got[2], got[3]);
    serverSimDestroy(sim);
    return 0;
}

/* Teams 1, 1, 2, 2. Seat 1 leaves its side and joins the other one. Seat 0
   opens a surrender vote for its team. */
int run_team_sides_surrender_after_defection(void) {
    const BYTE teams[4] = { 1, 1, 2, 2 };
    ServerSim *sim = tsRunningSim(teams, 4);
    ServerGameVoteSnapshot snap;

    UT_ASSERT(sim != NULL);
    serverSimLeaveAlliance(sim, 1);
    serverSimAcceptAlliance(sim, 2, 1);
    UT_ASSERT(tsAllied(sim, 1, 2) && !tsAllied(sim, 0, 1));
    UT_ASSERT(serverSimSidesAgree(sim));

    serverSimGameVoteToggle(sim, 0, GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);
    UT_ASSERT(serverSimGetGameVoteSnapshot(sim, GAME_VOTE_KIND_SURRENDER,
                                           &snap));
    UT_ASSERT_MSG(snap.active == GAME_VOTE_ACTIVE_RUNNING,
                  "the surrender vote did not open (active %u)",
                  (unsigned)snap.active);
    /* Seat 1 votes with the side it is on now, not the one it left. */
    UT_ASSERT_MSG(snap.eligibleCount == 1,
                  "%u seats vote in team 1's surrender",
                  (unsigned)snap.eligibleCount);
    serverSimDestroy(sim);
    return 0;
}

/* Teams 1, 1, 2. Seat 1 leaves; does seat 0's ping reach it? */
int run_team_sides_ping_after_leave(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);

    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimPingReachesClient(sim, 1, 0));
    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT(serverSimSidesAgree(sim));
    UT_ASSERT_MSG(!serverSimPingReachesClient(sim, 1, 0),
                  "a ping from 0 still reaches the player who left");
    UT_ASSERT(!serverSimPingReachesClient(sim, 0, 1));
    UT_ASSERT(!serverSimPingReachesClient(sim, 2, 0));
    serverSimDestroy(sim);
    return 0;
}

/* Teams 1, 1, 2, 2. Seat 1 defects to the other side; the team the round
   log names it with. */
int run_team_sides_track_identity_after_defection(void) {
    const BYTE teams[4] = { 1, 1, 2, 2 };
    ServerSim *sim = tsRunningSim(teams, 4);

    UT_ASSERT(sim != NULL);
    serverSimLeaveAlliance(sim, 1);
    serverSimAcceptAlliance(sim, 2, 1);
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimEnterGameOver(sim);
    UT_ASSERT_MSG(sim->trackIdentity[1].team == 2,
                  "the round log names seat 1 on team %u",
                  (unsigned)sim->trackIdentity[1].team);
    UT_ASSERT(sim->trackIdentity[2].team == 2);
    UT_ASSERT(sim->trackIdentity[0].team == 1);
    serverSimDestroy(sim);
    return 0;
}

/* Teams 1 and 2, and a third player joins the running round. */
int run_team_sides_midgame_joiner_team(void) {
    const BYTE teams[2] = { 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 2);

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 2, "Late", false);
    UT_ASSERT(serverSimIsPlayerConnected(sim, 2));
    UT_ASSERT_MSG(!tsAllied(sim, 2, 0) && !tsAllied(sim, 2, 1),
                  "the joiner is allied with somebody");
    /* A side of its own: the lowest team nobody is on (D3). */
    UT_ASSERT_MSG(tsTeam(sim, 2) == 3, "the joiner is on team %u",
                  (unsigned)tsTeam(sim, 2));
    UT_ASSERT(sim->teams[3].in_use);
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimDestroy(sim);
    return 0;
}

/* Four seats on four teams. 0 and 1 ally in play; then a script moves seat 3
   onto seat 2's team. */
int run_team_sides_set_team_keeps_player_alliance(void) {
    const BYTE teams[4] = { 1, 2, 3, 4 };
    ServerSim *sim = tsRunningSim(teams, 4);

    UT_ASSERT(sim != NULL);
    serverSimAcceptAlliance(sim, 1, 0);
    UT_ASSERT(tsAllied(sim, 0, 1));
    serverSimSetTeam(sim, 3, 3);
    UT_ASSERT(tsAllied(sim, 2, 3));
    UT_ASSERT_MSG(tsAllied(sim, 0, 1),
                  "the move of seat 3 took the alliance 0 and 1 made");
    UT_ASSERT(!tsAllied(sim, 0, 2) && !tsAllied(sim, 1, 3));
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * One way to change sides.
 * ================================================================ */

/* The events a change of side published, in order. */
#define TS_MAX_EVENTS 64

typedef struct {
    int          count;
    ControlEvent evts[TS_MAX_EVENTS];
} TsEvents;

static void tsCaptureAll(void *ctx, const ControlEvent *evt) {
    TsEvents *c = (TsEvents *)ctx;
    if (c->count < TS_MAX_EVENTS) {
        c->evts[c->count] = *evt;
    }
    c->count++;
}

/* The index of the first event of type at or after from, or -1. */
static int tsFind(const TsEvents *c, int from, int type) {
    int i;
    for (i = from; i < c->count && i < TS_MAX_EVENTS; i++) {
        if ((int)c->evts[i].type == type) return i;
    }
    return -1;
}

/* The index of the lobby slot event for slot at or after from, or -1. */
static int tsFindSlot(const TsEvents *c, int from, BYTE slot) {
    int i;
    for (i = from; i < c->count && i < TS_MAX_EVENTS; i++) {
        if (c->evts[i].type == CTRL_LOBBY_SLOT &&
            c->evts[i].u.lobbySlot.playerNum == slot) {
            return i;
        }
    }
    return -1;
}

/* Teams 1, 1, 2, 2 and 3. Seat 2 accepts seat 0: the accepter's team
   survives and seat 0's whole team moves onto it (D2). Seat 4 is not part
   of it. Each moved seat's slot goes out after the accept. */
int run_team_sides_accept_moves_whole_team(void) {
    const BYTE teams[5] = { 1, 1, 2, 2, 3 };
    ServerSim *sim = tsRunningSim(teams, 5);
    TsEvents *cap = (TsEvents *)calloc(1, sizeof(TsEvents));
    SubscriberHandle h;
    int acc;

    UT_ASSERT(sim != NULL && cap != NULL);
    h = serverSimRegisterSubscriber(sim, tsCaptureAll, cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    memset(cap, 0, sizeof(*cap));

    serverSimAcceptAlliance(sim, 2, 0);
    UT_ASSERT_MSG(tsTeam(sim, 0) == 2 && tsTeam(sim, 1) == 2 &&
                  tsTeam(sim, 2) == 2 && tsTeam(sim, 3) == 2 &&
                  tsTeam(sim, 4) == 3,
                  "after the accept the teams are %u %u %u %u %u",
                  tsTeam(sim, 0), tsTeam(sim, 1), tsTeam(sim, 2),
                  tsTeam(sim, 3), tsTeam(sim, 4));
    UT_ASSERT(serverSimSidesAgree(sim));
    UT_ASSERT(tsAllied(sim, 1, 3) && !tsAllied(sim, 0, 4));

    acc = tsFind(cap, 0, CTRL_ALLIANCE_ACCEPT);
    UT_ASSERT_MSG(acc >= 0, "the accept published no CTRL_ALLIANCE_ACCEPT");
    UT_ASSERT_MSG(tsFindSlot(cap, acc, 0) > acc && tsFindSlot(cap, acc, 1) > acc,
                  "the moved seats' slots did not follow the accept");
    UT_ASSERT_MSG(tsFindSlot(cap, 0, 2) < 0 && tsFindSlot(cap, 0, 3) < 0 &&
                  tsFindSlot(cap, 0, 4) < 0,
                  "a seat that did not move had its slot published");
    UT_ASSERT(cap->evts[tsFindSlot(cap, acc, 0)].u.lobbySlot.slot.teamNumber
              == 2);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    free(cap);
    return 0;
}

/* Seats 0 and 1 on no team and seat 2 on team 2. Seat 1 accepts seat 0: the
   accepter is given the lowest free team, and seat 0 joins it. */
int run_team_sides_accept_from_no_team(void) {
    const BYTE teams[3] = { 0, 0, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);

    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimAcceptAlliance(sim, 1, 0);
    UT_ASSERT_MSG(tsTeam(sim, 0) == 1 && tsTeam(sim, 1) == 1,
                  "after the accept seats 0 and 1 are on teams %u and %u",
                  tsTeam(sim, 0), tsTeam(sim, 1));
    UT_ASSERT(tsAllied(sim, 0, 1) && !tsAllied(sim, 0, 2));
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimDestroy(sim);
    return 0;
}

/* Two seats already on one side. An accept between them changes nothing and
   publishes nothing. */
int run_team_sides_accept_same_side_says_nothing(void) {
    const BYTE teams[2] = { 1, 1 };
    ServerSim *sim = tsRunningSim(teams, 2);
    TsEvents *cap = (TsEvents *)calloc(1, sizeof(TsEvents));
    SubscriberHandle h;

    UT_ASSERT(sim != NULL && cap != NULL);
    h = serverSimRegisterSubscriber(sim, tsCaptureAll, cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    memset(cap, 0, sizeof(*cap));

    serverSimAcceptAlliance(sim, 0, 1);
    UT_ASSERT_MSG(cap->count == 0, "the accept published %d events",
                  cap->count);
    UT_ASSERT(tsTeam(sim, 0) == 1 && tsTeam(sim, 1) == 1);
    UT_ASSERT(serverSimSidesAgree(sim));

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    free(cap);
    return 0;
}

/* Teams 1, 1 and 2. Seat 1 leaves and is put on team 3, a new team set up
   with D6's settings (D1). The leave goes out first, then the new team,
   then the slot. Seat 0, now alone on team 1, leaves too: the leave still
   goes out, and seat 0 keeps its team. */
int run_team_sides_leave_takes_free_team(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);
    TsEvents *cap = (TsEvents *)calloc(1, sizeof(TsEvents));
    SubscriberHandle h;
    int leave, meta, slot;

    UT_ASSERT(sim != NULL && cap != NULL);
    UT_ASSERT(!sim->teams[3].in_use);
    h = serverSimRegisterSubscriber(sim, tsCaptureAll, cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    memset(cap, 0, sizeof(*cap));

    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT_MSG(tsTeam(sim, 1) == 3, "the leaver is on team %u",
                  (unsigned)tsTeam(sim, 1));
    UT_ASSERT(tsTeam(sim, 0) == 1);
    UT_ASSERT(serverSimSidesAgree(sim));
    UT_ASSERT(sim->teams[3].in_use);
    UT_ASSERT(sim->teams[3].startSide == START_SIDE_ANY);
    UT_ASSERT(strcmp(sim->teams[3].name, "Team 3") == 0);

    leave = tsFind(cap, 0, CTRL_ALLIANCE_LEAVE);
    meta  = tsFind(cap, 0, CTRL_LOBBY_TEAM_META);
    slot  = tsFindSlot(cap, 0, 1);
    UT_ASSERT_MSG(leave >= 0 && meta > leave && slot > meta,
                  "the leave, team and slot went out at %d, %d and %d",
                  leave, meta, slot);
    UT_ASSERT(cap->evts[meta].u.lobbyTeamMeta.teamId == 3);
    UT_ASSERT(cap->evts[slot].u.lobbySlot.slot.teamNumber == 3);

    memset(cap, 0, sizeof(*cap));
    serverSimLeaveAlliance(sim, 0);
    UT_ASSERT(tsFind(cap, 0, CTRL_ALLIANCE_LEAVE) >= 0);
    UT_ASSERT_MSG(tsFindSlot(cap, 0, 0) < 0,
                  "a leaver alone on its team had its slot published");
    UT_ASSERT(tsTeam(sim, 0) == 1);
    UT_ASSERT(serverSimSidesAgree(sim));

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    free(cap);
    return 0;
}

/* Sixteen seats: seats 0 to 14 on teams 1 to 15, and seat 15 on team 1 with
   seat 0. Every team id has somebody, so seat 15 leaves onto no team, which
   is allied with nobody. */
int run_team_sides_leave_with_no_free_team(void) {
    BYTE teams[MAX_TANKS];
    ServerSim *sim;
    BYTE i;

    for (i = 0; i < MAX_TANKS - 1; i++) {
        teams[i] = (BYTE)(i + 1);
    }
    teams[MAX_TANKS - 1] = 1;
    sim = tsRunningSim(teams, MAX_TANKS);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(tsAllied(sim, 0, MAX_TANKS - 1));
    UT_ASSERT(serverSimFreeTeam(sim) == 0);

    serverSimLeaveAlliance(sim, MAX_TANKS - 1);
    UT_ASSERT_MSG(tsTeam(sim, MAX_TANKS - 1) == 0,
                  "with no free team the leaver is on team %u",
                  (unsigned)tsTeam(sim, MAX_TANKS - 1));
    UT_ASSERT(!tsAllied(sim, 0, MAX_TANKS - 1));
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimDestroy(sim);
    return 0;
}

/* serverSimFreeTeam in a lobby with ten seats, all on no team to begin.
   Each call answers the lowest id nobody is on, and a seat is put on it.
   Teams 1 and 2 keep the lobby's settings; each team set up after them
   takes the colour the fewest teams have, the lowest on a tie, so the first
   eight teams have eight colours and the ninth and tenth start the palette
   again. A seat that leaves the server frees its team. */
int run_team_sides_free_team_rules(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    bool seen[8];
    BYTE i;

    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    for (i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof(name), "P%u", (unsigned)i);
        serverSimAddPlayer(sim, i, name, false);
    }
    for (i = 0; i < 10; i++) {
        serverSimSetTeamBatch(sim, i, 0);
    }
    for (i = 0; i < 10; i++) {
        BYTE t = serverSimFreeTeam(sim);
        UT_ASSERT_MSG(t == i + 1, "call %u answered team %u", (unsigned)i,
                      (unsigned)t);
        UT_ASSERT(sim->teams[t].in_use);
        serverSimSetTeamBatch(sim, i, t);
    }
    UT_ASSERT(strcmp(sim->teams[9].name, "Team 9") == 0);
    UT_ASSERT(sim->teams[9].startSide == START_SIDE_ANY);

    memset(seen, 0, sizeof(seen));
    for (i = 1; i <= 8; i++) {
        BYTE c = (BYTE)(sim->teams[i].color & 7);
        UT_ASSERT_MSG(!seen[c], "teams 1 to 8 use colour %u twice",
                      (unsigned)c);
        seen[c] = true;
    }
    UT_ASSERT_MSG(sim->teams[9].color == 0 && sim->teams[10].color == 1,
                  "teams 9 and 10 have colours %u and %u",
                  (unsigned)sim->teams[9].color,
                  (unsigned)sim->teams[10].color);

    /* Seat 3 leaves the server; team 4 has nobody now. */
    serverSimRemovePlayer(sim, 3);
    UT_ASSERT(serverSimFreeTeam(sim) == 4);
    serverSimDestroy(sim);
    return 0;
}

/* Team chat follows the side. Teams 1, 1 and 2. Seat 0 accepts seat 2, who
   may then speak to team 1 and is on it; seat 2 leaves, and may no longer. */
static CmdResult tsTeamChat(ServerSim *sim, BYTE from, BYTE team) {
    ClientCommand cmd;
    CmdResult r;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_CHAT;
    cmd.cmdSeq = 1;
    cmd.u.chat.destPlayer = (uint8_t)(CHAT_DEST_TEAM_BASE + team);
    cmd.u.chat.bodyLen = 2;
    memcpy(cmd.u.chat.body, "hi", 2);
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, from, &cmd);
    threadsReleaseMutex();
    return r;
}

int run_team_sides_team_chat_follows_sides(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);

    UT_ASSERT(sim != NULL);
    UT_ASSERT(tsTeamChat(sim, 2, 1) == CMD_REJECT_INVALID);

    serverSimAcceptAlliance(sim, 0, 2);
    UT_ASSERT(tsTeam(sim, 2) == 1);
    UT_ASSERT_MSG(tsTeamChat(sim, 2, 1) == CMD_OK,
                  "the new member may not speak to its new team");
    UT_ASSERT(serverSimSidesAgree(sim));

    serverSimLeaveAlliance(sim, 2);
    UT_ASSERT(tsTeam(sim, 2) != 1);
    UT_ASSERT_MSG(tsTeamChat(sim, 2, 1) == CMD_REJECT_INVALID,
                  "the leaver may still speak to the team it left");
    UT_ASSERT(tsTeamChat(sim, 0, 1) == CMD_OK);
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimDestroy(sim);
    return 0;
}

/* Scenario panels follow a seat that changes team in a round. Panel 0 has
   an everyone list "E", a team 1 list "A" and a team 2 list "B"; panel 1
   only an everyone list, which no team change touches. Each move sends the
   seat one panel 0 list, addressed to it alone, and writes no store. */
static void tsStorePanel(ServerSim *sim, int panel, int target, char c) {
    ScnPanelStore *st = &sim->scenarioPanels[panel][target][0];
    st->valid    = true;
    st->tick     = 0;
    st->len      = 1;
    st->bytes[0] = (uint8_t)c;
}

/* The one panel event the last move sent seat slot: its list's first byte,
   or 0 for an empty list. -1 if it sent none, -2 if more than one. */
static int tsPanelSent(const TsEvents *c, BYTE slot) {
    int i, found = -1;
    for (i = 0; i < c->count && i < TS_MAX_EVENTS; i++) {
        const ControlEvent *e = &c->evts[i];
        if (e->type != CTRL_SCN_PANEL) continue;
        if (found != -1) return -2;
        if (e->u.scnPanel.panel != SCN_PANEL_WIRE(0, 0) ||
            e->u.scnPanel.destTeam != 0 || e->u.scnPanel.destPlayer != slot) {
            return -3;
        }
        found = (e->u.scnPanel.len > 0) ? (int)e->u.scnPanel.bytes[0] : 0;
    }
    return found;
}

int run_team_sides_panels_follow_team(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);
    TsEvents *cap = (TsEvents *)calloc(1, sizeof(TsEvents));
    SubscriberHandle h;
    int got;

    UT_ASSERT(sim != NULL && cap != NULL);
    tsStorePanel(sim, 0, 0, 'E');
    tsStorePanel(sim, 0, 1, 'A');
    tsStorePanel(sim, 0, 2, 'B');
    tsStorePanel(sim, 1, 0, 'X');
    h = serverSimRegisterSubscriber(sim, tsCaptureAll, cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Seat 1 leaves for team 3, which has no list: the everyone list. */
    memset(cap, 0, sizeof(*cap));
    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT(tsTeam(sim, 1) == 3);
    got = tsPanelSent(cap, 1);
    UT_ASSERT_MSG(got == 'E', "the leaver was sent %d", got);
    UT_ASSERT(tsFindSlot(cap, 0, 1) < tsFind(cap, 0, CTRL_SCN_PANEL));

    /* Seat 2 accepts it onto team 2: team 2's list. */
    memset(cap, 0, sizeof(*cap));
    serverSimAcceptAlliance(sim, 2, 1);
    UT_ASSERT(tsTeam(sim, 1) == 2);
    got = tsPanelSent(cap, 1);
    UT_ASSERT_MSG(got == 'B', "the new member was sent %d", got);

    /* The seat's own list wins over its team's. */
    tsStorePanel(sim, 0, MAX_TANKS + 1, 'S');
    memset(cap, 0, sizeof(*cap));
    serverSimLeaveAlliance(sim, 1);
    got = tsPanelSent(cap, 1);
    UT_ASSERT_MSG(got == 'S', "the seat was sent %d, not its own list", got);

    /* Nothing was written to the store. */
    UT_ASSERT(sim->scenarioPanels[0][3][0].valid == false);
    UT_ASSERT(sim->scenarioPanels[0][1][0].bytes[0] == 'A');
    UT_ASSERT(sim->scenarioPanels[0][2][0].bytes[0] == 'B');

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    free(cap);
    return 0;
}

/* Infection (Virus) keeps a seat on its side. Three players join a running
   round and the script puts them all on the survivors. Seat 1 leaves the
   alliance, which puts it on a team of its own; the script's
   on_team_changed puts it back. The two records agree on every tick. */
static bool tsCopyScript(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    FILE *out;
    char buf[4096];
    size_t n;
    if (in == NULL) return false;
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return true;
}

int run_team_sides_infection_puts_leaver_back(void) {
    static const char *const kMap = "ts_infection.map";
    static const char *const kScript = "ts_infection" SCN_SCRIPT_SUFFIX;
    BYTE emap[6000] = E_MAP;
    char err[512];
    ServerSim *sim;
    ScenarioHost *host;
    int t;
    BYTE i;

    UT_ASSERT_MSG(tsCopyScript("data/mods/Virus.scenario.lua", kScript),
                  "data/mods/Virus.scenario.lua could not be read");
    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    err[0] = '\0';
    host = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(host != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    for (i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Seat%u", (unsigned)i);
        serverSimAddPlayer(sim, i, name, false);
    }
    for (t = 0; t < 5; t++) {
        serverSimTick(sim);
    }
    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(tsTeam(sim, i) == 1, "seat %u is on team %u",
                      (unsigned)i, (unsigned)tsTeam(sim, i));
    }
    UT_ASSERT(serverSimSidesAgree(sim));

    threadsWaitForMutex();
    serverSimLeaveAlliance(sim, 1);
    threadsReleaseMutex();
    UT_ASSERT(tsTeam(sim, 1) != 1);
    UT_ASSERT(serverSimSidesAgree(sim));
    for (t = 0; t < 10; t++) {
        serverSimTick(sim);
        UT_ASSERT_MSG(serverSimSidesAgree(sim),
                      "the records differ after tick %d", t);
    }
    UT_ASSERT_MSG(tsTeam(sim, 1) == 1, "the script left seat 1 on team %u",
                  (unsigned)tsTeam(sim, 1));
    UT_ASSERT(tsAllied(sim, 0, 1) && tsAllied(sim, 1, 2));

    scenarioHostDetach(host);
    serverSimDestroy(sim);
    remove(kScript);
    return 0;
}

/* Seat 0 on team 1 and seat 1 alone on team 3, owning pills 1 and 2. Seat 1
   drops, which leaves both pills neutral, and seat 0 takes pill 2. Seat 1
   rejoins: it is a joiner (D11), so it gets the lowest free team, not team 3,
   and only the pill that is still neutral comes back. */
int run_team_sides_rejoin_is_a_joiner(void) {
    const BYTE teams[2] = { 1, 3 };
    ServerSim *sim = tsRunningSim(teams, 2);
    GameSim *gs;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    UT_ASSERT(pillsGetNumPills(&gs->pb) >= 2);
    gs->pb->item[0].owner = 1;
    gs->pb->item[1].owner = 1;

    serverSimRemovePlayer(sim, 1);
    UT_ASSERT_MSG(gs->pb->item[0].owner == NEUTRAL &&
                  gs->pb->item[1].owner == NEUTRAL,
                  "after the drop the pills are owned by %u and %u",
                  (unsigned)gs->pb->item[0].owner,
                  (unsigned)gs->pb->item[1].owner);
    gs->pb->item[1].owner = 0;

    serverSimAddPlayer(sim, 1, "P1", true);
    UT_ASSERT(serverSimIsPlayerConnected(sim, 1));
    UT_ASSERT_MSG(tsTeam(sim, 1) == 2, "the rejoiner is on team %u",
                  (unsigned)tsTeam(sim, 1));
    UT_ASSERT(!tsAllied(sim, 0, 1));
    UT_ASSERT(serverSimSidesAgree(sim));
    UT_ASSERT_MSG(gs->pb->item[0].owner == 1,
                  "the neutral pill went to %u",
                  (unsigned)gs->pb->item[0].owner);
    UT_ASSERT_MSG(gs->pb->item[1].owner == 0,
                  "the pill seat 0 took went to %u",
                  (unsigned)gs->pb->item[1].owner);
    serverSimDestroy(sim);
    return 0;
}

/* A joiner's lobby slot goes out before its join, so a script placing it in
   on_player_join already sees the team the server gave it. */
int run_team_sides_joiner_slot_before_join(void) {
    const BYTE teams[2] = { 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 2);
    TsEvents *cap = (TsEvents *)calloc(1, sizeof(TsEvents));
    SubscriberHandle h;
    int slot, join;

    UT_ASSERT(sim != NULL && cap != NULL);
    h = serverSimRegisterSubscriber(sim, tsCaptureAll, cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    memset(cap, 0, sizeof(*cap));

    serverSimAddPlayer(sim, 2, "Late", false);
    slot = tsFindSlot(cap, 0, 2);
    join = tsFind(cap, 0, CTRL_PLAYER_JOIN);
    UT_ASSERT_MSG(slot >= 0 && join > slot,
                  "the slot and the join went out at %d and %d", slot, join);
    UT_ASSERT(cap->evts[slot].u.lobbySlot.slot.teamNumber == 3);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    free(cap);
    return 0;
}

/* A seat a script holds for a bot, added to team 1 while the round runs, is
   allied with team 1 as it is added, so the two records agree. */
int run_team_sides_held_seat_allied_in_round(void) {
    const BYTE teams[2] = { 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 2);

    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, 2, "Held", 1));
    UT_ASSERT(tsTeam(sim, 2) == 1);
    UT_ASSERT_MSG(tsAllied(sim, 0, 2) && tsAllied(sim, 2, 0),
                  "the held seat is not allied with its team");
    UT_ASSERT(!tsAllied(sim, 1, 2));
    UT_ASSERT(tsTeam(sim, 0) == 1);
    UT_ASSERT(serverSimSidesAgree(sim));
    serverSimDestroy(sim);
    return 0;
}

/* Teams 1, 1 and 2, and a fourth seat that joins the round and so opens
   team 3. Seat 1 leaves for team 4, then seat 2 accepts it onto team 2, so
   team 4 is empty at the end. The next lobby lists teams 1, 2 and 3 and not
   team 4 (D6). */
int run_team_sides_lobby_drops_empty_teams(void) {
    const BYTE teams[3] = { 1, 1, 2 };
    ServerSim *sim = tsRunningSim(teams, 3);

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 3, "Late", false);
    UT_ASSERT(tsTeam(sim, 3) == 3);
    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT(tsTeam(sim, 1) == 4 && sim->teams[4].in_use);
    serverSimAcceptAlliance(sim, 2, 1);
    UT_ASSERT(tsTeam(sim, 1) == 2);
    UT_ASSERT(serverSimSidesAgree(sim));

    serverSimEnterGameOver(sim);
    serverSimReturnToLobby(sim);
    UT_ASSERT(sim->teams[1].in_use && sim->teams[2].in_use);
    UT_ASSERT_MSG(sim->teams[3].in_use, "team 3, which has a seat, went");
    UT_ASSERT_MSG(!sim->teams[4].in_use, "the empty team 4 is still listed");
    UT_ASSERT(tsTeam(sim, 0) == 1 && tsTeam(sim, 1) == 2 &&
              tsTeam(sim, 2) == 2 && tsTeam(sim, 3) == 3);
    serverSimDestroy(sim);
    return 0;
}

/* In the lobby a joiner keeps the smallest-team pick. */
int run_team_sides_lobby_joiner_keeps_rule(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);

    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    UT_ASSERT(tsTeam(sim, 0) == 1 && tsTeam(sim, 1) == 2);
    UT_ASSERT_MSG(tsTeam(sim, 2) == 1, "the lobby joiner is on team %u",
                  (unsigned)tsTeam(sim, 2));
    serverSimDestroy(sim);
    return 0;
}
