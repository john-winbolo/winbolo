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
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
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
    serverSimLeaveAlliance(sim, 1);
    serverSimEnterGameOver(sim);
    serverSimReturnToLobby(sim);
    for (i = 0; i < 4; i++) {
        got[i] = tsTeam(sim, i);
    }
    /* Today: the fold puts the group 0, 2 and 3 on the lowest team any of
       them was on, and the leaver keeps the team it had. */
    UT_ASSERT_MSG(got[0] == 1 && got[1] == 1 && got[2] == 1 && got[3] == 1,
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

    serverSimGameVoteToggle(sim, 0, GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);
    UT_ASSERT(serverSimGetGameVoteSnapshot(sim, GAME_VOTE_KIND_SURRENDER,
                                           &snap));
    UT_ASSERT_MSG(snap.active == GAME_VOTE_ACTIVE_RUNNING,
                  "the surrender vote did not open (active %u)",
                  (unsigned)snap.active);
    /* Today: seat 1 still votes with the team it left. */
    UT_ASSERT_MSG(snap.eligibleCount == 2,
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
    /* Today: the old team still carries it. */
    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 1, 0),
                  "a ping from 0 no longer reaches the player who left");
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
    serverSimEnterGameOver(sim);
    /* Today: the team it left. */
    UT_ASSERT_MSG(sim->trackIdentity[1].team == 1,
                  "the round log names seat 1 on team %u",
                  (unsigned)sim->trackIdentity[1].team);
    UT_ASSERT(sim->trackIdentity[2].team == 2);
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
    /* Today: the smallest team, which it is not allied with. */
    UT_ASSERT_MSG(tsTeam(sim, 2) == 1, "the joiner is on team %u",
                  (unsigned)tsTeam(sim, 2));
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
    /* Today: the rebuild from the teams takes the alliance 0 and 1 made. */
    UT_ASSERT_MSG(!tsAllied(sim, 0, 1),
                  "the move of seat 3 left 0 and 1 allied");
    serverSimDestroy(sim);
    return 0;
}
