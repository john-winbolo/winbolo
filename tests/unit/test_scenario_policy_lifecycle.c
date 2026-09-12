/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * The six lifecycle and lobby policy pointers:
 *
 *   run_scenario_policy_choose_start       — a named start places every tank
 *       there, whether the tank comes from a lobby batch or a mid-round join
 *   run_scenario_policy_choose_start_out_of_range — an index off the end is
 *       refused and the engine's own pick stands
 *   run_scenario_policy_allow_base_win     — a swept board ends the round
 *       under true and NULL, and does not under false
 *   run_scenario_policy_allow_extra_teams  — the lobby's add-bot team write
 *       under each answer
 *   run_scenario_policy_team_set_extra_teams — the team picker: an empty
 *       team refused, a populated one allowed, and the slot's own team
 *   run_scenario_policy_max_players        — a person refused past the cap, a
 *       bot seated above it
 *   run_scenario_policy_spawn_loadout      — both forms of the answer at the
 *       three places the engine hands out a loadout, the respawn included
 *   run_scenario_policy_can_respawn        — a hundred ticks held dead, then
 *       back the tick the answer changes
 *   run_scenario_policy_null_is_classic    — all six decisions with nothing
 *       registered
 *
 * Three of the six are asked through GameSim callbacks and three are read off
 * the sim directly, so the cases drive the real call sites rather than the
 * policy: a start selection, the win sweep, the add-bot command, the free-slot
 * search, a respawn and the death wait.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_ADD_BOT, CmdResult */
#include "server_sim.h"
#include "server_sim_internal.h"   /* scenarioPolicy, playerConnected, lobbyPlayers */
#include "server_sim_join.h"       /* serverSimFindFreeSlot */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled / BotAiType / BrainPath */
#include "server_sim_scenario.h"   /* serverSimSetScenarioPolicy */
#include "game_sim.h"
#include "bases.h"                 /* basesGetNumBases */
#include "starts.h"                /* startsGetNumStarts, startsGetStart */
#include "tank.h"
#include "gametype.h"              /* TANK_FULL_* — the classic amounts */
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* Armour comfortably above MIN_ARMOUR_CAPTURE — a held base. */
#define PL_ARMOUR_HELD 50

/* The seats the cap case fills before it asks for one more. */
#define PL_CAP 6

/* The explicit loadout, picked so no amount is a classic one: strict
   tournament hands out nothing and open hands out forty of everything. */
#define PL_SHELLS 7
#define PL_MINES  6
#define PL_ARMOUR 33
#define PL_TREES  5

/* ── The test policy ──────────────────────────────────────────────────────
 *
 * One vtable with all six filled and one context holding what each should
 * answer, so a case changes an answer between two runs of the same call site
 * and nothing else moves. The two counters are what prove a question is asked
 * once per decision rather than once per branch. */
typedef struct {
    int        startAsks;
    bool       startAnswers;   /* false: the policy declines to name one */
    BYTE       startNamed;
    bool       baseWin;
    bool       extraTeams;
    int        maxPlayers;
    bool       loadoutAnswers; /* false: the sim's game type decides */
    ScnLoadout loadout;
    int        respawnAsks;
    bool       respawn;
    bool       startWritesNothing; /* answer true and leave *startIdx alone */
    BYTE       lastPlayer;         /* the player the last question named */
} PlCtx;

static bool plChooseStart(void *ctx, BYTE player, BYTE *startIdx) {
    PlCtx *p = (PlCtx *)ctx;
    p->lastPlayer = player;
    p->startAsks++;
    if (!p->startAnswers) return FALSE;
    if (p->startWritesNothing) return TRUE;
    *startIdx = p->startNamed;
    return TRUE;
}

static bool plAllowBaseWin(void *ctx) {
    return ((PlCtx *)ctx)->baseWin;
}

static bool plAllowExtraTeams(void *ctx) {
    return ((PlCtx *)ctx)->extraTeams;
}

static int plMaxPlayers(void *ctx) {
    return ((PlCtx *)ctx)->maxPlayers;
}

static bool plSpawnLoadout(void *ctx, BYTE player, ScnLoadout *out) {
    PlCtx *p = (PlCtx *)ctx;
    p->lastPlayer = player;
    if (!p->loadoutAnswers) return FALSE;
    *out = p->loadout;
    return TRUE;
}

static bool plCanRespawn(void *ctx, BYTE player) {
    PlCtx *p = (PlCtx *)ctx;
    p->lastPlayer = player;
    p->respawnAsks++;
    return p->respawn;
}

/* Every answer starts classic, so a case sets only the one it is about. */
static void plFillPolicy(ScenarioPolicy *pol, PlCtx *pc) {
    memset(pol, 0, sizeof(*pol));
    memset(pc, 0, sizeof(*pc));
    pc->baseWin    = true;
    pc->extraTeams = true;
    pc->maxPlayers = 0;
    pc->respawn    = true;
    pol->chooseStart     = plChooseStart;
    pol->allowBaseWin    = plAllowBaseWin;
    pol->allowExtraTeams = plAllowExtraTeams;
    pol->maxPlayers      = plMaxPlayers;
    pol->spawnLoadout    = plSpawnLoadout;
    pol->canRespawn      = plCanRespawn;
    pol->ctx             = pc;
}

/* ── Fixtures ─────────────────────────────────────────────────────────── */

/* A running round with nobody in it: each case seats what it needs, after
   registering its policy, so the policy is in place for the tank create. */
static ServerSim *plRunningSim(gameType gt) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gt, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    return sim;
}

/* A file for the brain path to name. The stub brain never opens it — the
   add-bot arm only needs the path to be non-empty.

   The name carries the case's own name, because ctest runs the cases as
   concurrent processes in one working directory: with one name between them
   the first case to reach its drop takes the file the others are still
   naming. The tag is a required argument so a case added later cannot
   quietly share a name. */
static char plBrainPath[128];

static bool plMakeBrainFile(const char *tag) {
    FILE *f;

    SDL_snprintf(plBrainPath, sizeof(plBrainPath),
                 "test_scenario_policy_brain_%s.lua", tag);
    f = fopen(plBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static void plDropBrainFile(void) {
    remove(plBrainPath);
}

/* A lobby with a host in slot 0 and nothing else arranged. The team-set arm
   needs no bot support, so this keeps the brain fixture out of its way. */
static ServerSim *plPlainLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    return sim;
}

/* The same lobby, with a server that will run bots — what the add-bot arm
   wants before it reads a team number. */
static ServerSim *plLobbySim(void) {
    ServerSim *sim = plPlainLobbySim();
    if (sim == NULL) return NULL;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, plBrainPath);
    return sim;
}

/* Hand every base to one owner so the sweep has a winner to name. */
static void plSweepBases(ServerSim *sim, BYTE owner) {
    GameSim *gs = serverSimGetGameSim(sim);
    BYTE n, i;
    if (gs == NULL) return;
    n = basesGetNumBases(&gs->bs);
    for (i = 0; i < n; i++) {
        (*gs->bs).item[i].owner  = owner;
        (*gs->bs).item[i].armour = PL_ARMOUR_HELD;
    }
}

/* ── Reading the world back ───────────────────────────────────────────── */

/* Which start a map square is closest to. The scatter search nudges a tank
   off the start itself to keep spawns apart, so "at start N" is read as
   "nearer start N than any other" rather than as an exact square. */
static int plNearestStart(GameSim *gs, int mx, int my) {
    BYTE n = startsGetNumStarts(&gs->ss);
    int best = -1;
    int bestDist = 0;
    BYTE i;

    for (i = 0; i < n; i++) {
        int dx = (int)(*gs->ss).item[i].x - mx;
        int dy = (int)(*gs->ss).item[i].y - my;
        int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (best < 0 || dist < bestDist) {
            best = (int)i;
            bestDist = dist;
        }
    }
    return best;
}

static int plTankNearestStart(GameSim *gs, BYTE slot) {
    WORLD wx, wy;
    if (gs->tanks[slot] == NULL) return -1;
    tankGetWorld(&gs->tanks[slot], &wx, &wy);
    return plNearestStart(gs, (int)(wx >> M_W_SHIFT_SIZE),
                          (int)(wy >> M_W_SHIFT_SIZE));
}

/* One start selection, through the same fence the engine's callers put it
   behind. */
static int plSelectStart(GameSim *gs, BYTE player) {
    BYTE x = 0;
    BYTE y = 0;
    TURNTYPE dir = 0;

    gs->inStartFind = TRUE;
    startsGetStart(gs, &gs->ss, &x, &y, &dir, player);
    gs->inStartFind = FALSE;
    return plNearestStart(gs, (int)x, (int)y);
}

/* Kill a tank and take it through the server's respawn, which is the third
   of the three places a loadout is handed out. */
static void plKillAndRespawn(GameSim *gs, BYTE slot) {
    tankSetArmour(&gs->tanks[slot], 0);
    tankSetDestroyed(&gs->tanks[slot], TRUE);
    tankDeath(gs, &gs->tanks[slot]);
}

/* The lobby's team picker, sent by the host so it may move any slot. */
static CmdResult plSetTeam(ServerSim *sim, BYTE slot, BYTE team) {
    ClientCommand cmd;
    CmdResult r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_TEAM_SET;
    cmd.cmdSeq = 1;
    cmd.u.teamSet.slot = slot;
    cmd.u.teamSet.team = team;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

/* The lobby's Add Bot, with a team number on it. Returns the seat the bot
   took, or -1 if the command was refused. */
static int plAddBotOnTeam(ServerSim *sim, BYTE team) {
    ClientCommand cmd;
    bool before[MAX_TANKS];
    CmdResult r;
    int i;

    for (i = 0; i < MAX_TANKS; i++) {
        before[i] = serverSimIsPlayerConnected(sim, (BYTE)i);
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_ADD_BOT;
    cmd.cmdSeq = 1;
    cmd.u.lobbyAddBot.teamNumber = team;
    cmd.u.lobbyAddBot.nameLen    = 0;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    if (r != CMD_OK) return -1;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!before[i] && serverSimIsPlayerConnected(sim, (BYTE)i)) return i;
    }
    return -1;
}

/* ================================================================
 * 1. A named start places every tank there.
 *
 * The first seat carries a pre-computed batch slot naming a different start,
 * so the named one has that to beat as well as the per-player picker the
 * other two reach. Three placements ask three times, which is what "one
 * decision site" means here.
 * ================================================================ */
int run_scenario_policy_choose_start(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    ScenarioPolicy pol;
    PlCtx pc;
    GameSim *gs;
    BYTE numStarts;
    BYTE slot;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    gs = serverSimGetGameSim(sim);
    numStarts = startsGetNumStarts(&gs->ss);
    UT_ASSERT_MSG(numStarts >= 3,
                  "setup: the map must carry at least three starts, has %u",
                  (unsigned)numStarts);

    plFillPolicy(&pol, &pc);
    pc.startAnswers = true;
    pc.startNamed   = (BYTE)(numStarts - 1);
    serverSimSetScenarioPolicy(sim, &pol);

    /* A batch slot on the first seat, so the named start has something to
       beat rather than only a bare per-player pick. */
    gs->pendingStartIdx[0] = 0;

    for (slot = 0; slot < 3; slot++) {
        char name[16];
        SDL_snprintf(name, sizeof(name), "P%u", (unsigned)slot);
        serverSimAddPlayer(sim, slot, name, false);
        UT_ASSERT_MSG(gs->tanks[slot] != NULL,
                      "setup: slot %u should have a tank", (unsigned)slot);
        UT_ASSERT_MSG(plTankNearestStart(gs, slot) == (int)pc.startNamed,
                      "slot %u landed nearest start %d, expected the named %u",
                      (unsigned)slot, plTankNearestStart(gs, slot),
                      (unsigned)pc.startNamed);
    }

    UT_ASSERT_MSG(pc.startAsks == 3,
                  "three placements should ask three times, asked %d",
                  pc.startAsks);
    UT_ASSERT_MSG(pc.lastPlayer == 2,
                  "the last placement asked about player %u, wanted 2",
                  (unsigned)pc.lastPlayer);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. An index off the end falls back to the engine's pick.
 *
 * The batch slot is the engine's pick for this selection and it names a
 * start that is not 0, so a fallback that landed on 0 by accident — reading
 * the refused index as "the first one" — fails here rather than passing by
 * coincidence.
 * ================================================================ */
int run_scenario_policy_choose_start_out_of_range(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    ScenarioPolicy pol;
    PlCtx pc;
    GameSim *gs;
    BYTE numStarts;
    BYTE enginePick;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    gs = serverSimGetGameSim(sim);
    numStarts = startsGetNumStarts(&gs->ss);
    UT_ASSERT_MSG(numStarts >= 2,
                  "setup: the map must carry at least two starts, has %u",
                  (unsigned)numStarts);
    enginePick = (BYTE)(numStarts - 1);
    UT_ASSERT_MSG(enginePick != 0,
                  "setup: the engine's pick must not be start 0, or the case "
                  "proves nothing");

    plFillPolicy(&pol, &pc);
    pc.startAnswers = true;
    pc.startNamed   = numStarts;    /* one past the last live start */
    serverSimSetScenarioPolicy(sim, &pol);

    gs->pendingStartIdx[0] = enginePick;
    UT_ASSERT_MSG(plSelectStart(gs, 0) == (int)enginePick,
                  "a refused index must leave the engine's pick standing");
    UT_ASSERT_MSG(pc.startAsks == 1,
                  "one placement should ask once, asked %d", pc.startAsks);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. The base sweep is the scenario's to turn off.
 * ================================================================ */
int run_scenario_policy_allow_base_win(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    ScenarioPolicy pol;
    PlCtx pc;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    serverSimAddPlayer(sim, 0, "Sweeper", false);
    plSweepBases(sim, 0);

    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "setup: a full-ownership board must be a win with no policy");

    plFillPolicy(&pol, &pc);
    pc.baseWin = false;
    serverSimSetScenarioPolicy(sim, &pol);
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == FALSE,
                  "the same board must not be a win under a policy that "
                  "refuses the sweep");

    pc.baseWin = true;
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "the sweep must come back the moment the policy allows it");

    serverSimSetScenarioPolicy(sim, NULL);
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "clearing the policy must leave the classic sweep");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. The add-bot arm's team write.
 *
 * The bot is seated either way — it is in its seat before the team is
 * written — so the refusal shows up as a seat with no team on it.
 *
 * The legs use two team numbers because the first add populates the one it
 * names: a second bot onto that team is joining a team that exists, which
 * is not an extra team and is never asked about. Team 4 stays empty until
 * something is allowed onto it, so it is the one the refusals use.
 * ================================================================ */
int run_scenario_policy_allow_extra_teams(void) {
    ServerSim *sim;
    ScenarioPolicy pol;
    PlCtx pc;
    int slot;

    UT_ASSERT(plMakeBrainFile("allow_extra_teams"));
    ut_brain_stub_arm(true);

    sim = plLobbySim();
    UT_ASSERT_MSG(sim != NULL, "plLobbySim returned NULL");

    slot = plAddBotOnTeam(sim, 3);
    UT_ASSERT_MSG(slot >= 0, "setup: the add must be accepted with no policy");
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == 3,
                  "with no policy the bot should be on team 3, is on %u",
                  (unsigned)sim->lobbyPlayers[slot].teamNumber);

    plFillPolicy(&pol, &pc);
    pc.extraTeams = false;
    serverSimSetScenarioPolicy(sim, &pol);

    /* Team 4 has nobody on it, so this add is what would make it. */
    slot = plAddBotOnTeam(sim, 4);
    UT_ASSERT_MSG(slot >= 0, "the add itself is not what is refused");
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == 0,
                  "a refused team must leave the seat unassigned, is on %u",
                  (unsigned)sim->lobbyPlayers[slot].teamNumber);

    /* Team 3 has the first bot on it, so this add joins a team rather than
       making one: allowed under the same policy, without asking it. */
    slot = plAddBotOnTeam(sim, 3);
    UT_ASSERT_MSG(slot >= 0, "the add must be accepted");
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == 3,
                  "joining a team that already has members must be allowed "
                  "under a policy that refuses extra teams, seat is on %u",
                  (unsigned)sim->lobbyPlayers[slot].teamNumber);

    pc.extraTeams = true;
    slot = plAddBotOnTeam(sim, 4);
    UT_ASSERT_MSG(slot >= 0, "the add must still be accepted");
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == 4,
                  "team 4 must be allowed the moment the policy allows it, "
                  "seat is on %u",
                  (unsigned)sim->lobbyPlayers[slot].teamNumber);

    serverSimDestroy(sim);
    plDropBrainFile();
    return 0;
}

/* ================================================================
 * 4b. The team-set arm.
 *
 * The ordinary way a team comes into existence: a player moves themselves,
 * or the host moves anyone, onto a team number nobody is using. Nothing has
 * been written when the question is asked, so a refusal is a return code
 * and the roster does not move.
 *
 * The roster is set up by hand rather than left to the join defaults: team
 * 1 holds two slots and team 2 holds one, so the case has both a populated
 * team to move onto and a sole member to move away.
 * ================================================================ */
int run_scenario_policy_team_set_extra_teams(void) {
    ServerSim *sim = plPlainLobbySim();
    ScenarioPolicy pol;
    PlCtx pc;

    UT_ASSERT_MSG(sim != NULL, "plPlainLobbySim returned NULL");
    serverSimAddPlayer(sim, 1, "Second", false);
    serverSimAddPlayer(sim, 2, "Third", false);

    UT_ASSERT(plSetTeam(sim, 0, 1) == CMD_OK);
    UT_ASSERT(plSetTeam(sim, 1, 1) == CMD_OK);
    UT_ASSERT(plSetTeam(sim, 2, 2) == CMD_OK);
    UT_ASSERT_MSG(sim->lobbyPlayers[2].teamNumber == 2,
                  "setup: slot 2 should be the only member of team 2");

    /* With no policy an empty team is there for the taking. */
    UT_ASSERT_MSG(plSetTeam(sim, 2, 5) == CMD_OK,
                  "setup: a move onto an empty team must be accepted with no "
                  "policy");
    UT_ASSERT(sim->lobbyPlayers[2].teamNumber == 5);
    UT_ASSERT(plSetTeam(sim, 2, 2) == CMD_OK);

    plFillPolicy(&pol, &pc);
    pc.extraTeams = false;
    serverSimSetScenarioPolicy(sim, &pol);

    /* The team a slot already holds is not a team being made, even when the
       slot is the only one on it. Without that test the count below would
       see an empty team 2 and refuse a move that changes nothing. */
    UT_ASSERT_MSG(plSetTeam(sim, 2, 2) == CMD_OK,
                  "re-sending the team a slot already holds must be accepted");
    UT_ASSERT(sim->lobbyPlayers[2].teamNumber == 2);

    /* An empty team: refused, and the roster does not move. */
    UT_ASSERT_MSG(plSetTeam(sim, 2, 5) == CMD_REJECT_INVALID,
                  "a move onto an empty team must be refused");
    UT_ASSERT_MSG(sim->lobbyPlayers[2].teamNumber == 2,
                  "a refused move must leave the slot where it was, is on %u",
                  (unsigned)sim->lobbyPlayers[2].teamNumber);

    /* A team two other slots are on: joining it makes nothing, so it is
       allowed under the same policy. */
    UT_ASSERT_MSG(plSetTeam(sim, 2, 1) == CMD_OK,
                  "a move onto a team that already has members must be "
                  "allowed");
    UT_ASSERT(sim->lobbyPlayers[2].teamNumber == 1);

    /* Team 2 is empty now that its only member has left, so going back to
       it is making a team again and is refused. The number of teams in play
       would not rise, which is exactly the edge the slot-excluded count
       decides: the team being moved onto is the one under test. */
    UT_ASSERT_MSG(plSetTeam(sim, 2, 2) == CMD_REJECT_INVALID,
                  "a team emptied by the move away must be refused on the "
                  "way back");
    UT_ASSERT(sim->lobbyPlayers[2].teamNumber == 1);

    pc.extraTeams = true;
    UT_ASSERT_MSG(plSetTeam(sim, 2, 5) == CMD_OK,
                  "an empty team must be allowed the moment the policy "
                  "allows it");
    UT_ASSERT(sim->lobbyPlayers[2].teamNumber == 5);

    serverSimSetScenarioPolicy(sim, NULL);
    UT_ASSERT_MSG(plSetTeam(sim, 2, 6) == CMD_OK,
                  "clearing the policy must leave the arm as it was");
    UT_ASSERT(sim->lobbyPlayers[2].teamNumber == 6);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. The player cap counts people, not bots.
 * ================================================================ */
int run_scenario_policy_max_players(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    ScenarioPolicy pol;
    PlCtx pc;
    int i;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");

    /* Six seats taken. Written straight onto the sim: the search reads the
       connected flags and nothing else about the seats matters here. */
    for (i = 0; i < PL_CAP; i++) {
        sim->playerConnected[i] = TRUE;
    }

    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == PL_CAP,
                  "setup: with no policy the seventh seat is free");

    plFillPolicy(&pol, &pc);
    pc.maxPlayers = PL_CAP;
    serverSimSetScenarioPolicy(sim, &pol);

    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == -1,
                  "the seventh person must be refused at a cap of %d", PL_CAP);
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, true) == PL_CAP,
                  "the seventh bot must seat above the cap");

    /* Zero is the manifest's way of saying no cap. */
    pc.maxPlayers = 0;
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == PL_CAP,
                  "a cap of zero must leave the search where it was");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 6. What a spawning tank is handed.
 *
 * The round is a strict tournament, which hands out nothing, so an answer
 * that says "open" is unmistakable. All three places the engine fuels a tank
 * are driven: the respawn first, because that is the one a per-slot array
 * missed and every wave bot's second life arrived empty.
 * ================================================================ */
int run_scenario_policy_spawn_loadout(void) {
    ServerSim *sim = plRunningSim(gameStrictTournament);
    ScenarioPolicy pol;
    PlCtx pc;
    GameSim *gs;
    WORLD wx, wy;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    serverSimAddPlayer(sim, 0, "Tester", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs->tanks[0] != NULL);
    UT_ASSERT_MSG(tankGetShells(&gs->tanks[0]) == 0,
                  "setup: a strict tournament tank starts empty, has %u shells",
                  (unsigned)tankGetShells(&gs->tanks[0]));

    plFillPolicy(&pol, &pc);
    pc.loadoutAnswers       = true;
    pc.loadout.useGameType  = 1;
    pc.loadout.gameType     = (uint8_t)gameOpen;
    serverSimSetScenarioPolicy(sim, &pol);

    /* The respawn. */
    plKillAndRespawn(gs, 0);
    UT_ASSERT_MSG(tankGetShells(&gs->tanks[0]) == TANK_FULL_SHELLS,
                  "a respawn under an open loadout should carry %d shells, "
                  "carries %u", TANK_FULL_SHELLS,
                  (unsigned)tankGetShells(&gs->tanks[0]));
    UT_ASSERT(tankGetMines(&gs->tanks[0]) == TANK_FULL_MINES);
    UT_ASSERT(tankGetTrees(&gs->tanks[0]) == TANK_FULL_TREES);
    UT_ASSERT_MSG(pc.lastPlayer == 0,
                  "the loadout question named player %u, wanted 0",
                  (unsigned)pc.lastPlayer);

    /* The four amounts, at the same site. */
    pc.loadout.useGameType = 0;
    pc.loadout.shells = PL_SHELLS;
    pc.loadout.mines  = PL_MINES;
    pc.loadout.armour = PL_ARMOUR;
    pc.loadout.trees  = PL_TREES;
    plKillAndRespawn(gs, 0);
    UT_ASSERT_MSG(tankGetShells(&gs->tanks[0]) == PL_SHELLS &&
                  tankGetMines(&gs->tanks[0])  == PL_MINES &&
                  tankGetArmour(&gs->tanks[0]) == PL_ARMOUR &&
                  tankGetTrees(&gs->tanks[0])  == PL_TREES,
                  "the four amounts should have been handed over exactly, got "
                  "%u/%u/%u/%u",
                  (unsigned)tankGetShells(&gs->tanks[0]),
                  (unsigned)tankGetMines(&gs->tanks[0]),
                  (unsigned)tankGetArmour(&gs->tanks[0]),
                  (unsigned)tankGetTrees(&gs->tanks[0]));

    /* The create. */
    tankDestroy(gs, &gs->tanks[0]);
    gs->tanks[0] = NULL;
    tankCreate(gs, &gs->tanks[0]);
    UT_ASSERT(gs->tanks[0] != NULL);
    UT_ASSERT_MSG(tankGetShells(&gs->tanks[0]) == PL_SHELLS &&
                  tankGetTrees(&gs->tanks[0])  == PL_TREES,
                  "a fresh tank should have been fuelled by the policy too");

    /* The re-fuel, which is the same question asked from tankSetWorld. */
    tankGetWorld(&gs->tanks[0], &wx, &wy);
    tankSetShells(&gs->tanks[0], 0);
    tankSetWorld(gs, &gs->tanks[0], wx, wy, 0, TRUE);
    UT_ASSERT_MSG(tankGetShells(&gs->tanks[0]) == PL_SHELLS,
                  "a re-fuel should have been answered by the policy, got %u",
                  (unsigned)tankGetShells(&gs->tanks[0]));

    /* A policy with no opinion leaves the sim's game type deciding. */
    pc.loadoutAnswers = false;
    plKillAndRespawn(gs, 0);
    UT_ASSERT_MSG(tankGetShells(&gs->tanks[0]) == 0,
                  "a declined answer must leave the strict-tournament "
                  "loadout, got %u shells",
                  (unsigned)tankGetShells(&gs->tanks[0]));

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 7. A dead tank stays dead until the policy says otherwise.
 *
 * The wait is held at one rather than run down to zero, so the tank is still
 * dead and the question is put again every tick.
 * ================================================================ */
int run_scenario_policy_can_respawn(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    ScenarioPolicy pol;
    PlCtx pc;
    GameSim *gs;
    int i;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    serverSimAddPlayer(sim, 0, "Tester", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs->tanks[0] != NULL);

    plFillPolicy(&pol, &pc);
    pc.respawn = false;
    serverSimSetScenarioPolicy(sim, &pol);

    tankSetArmour(&gs->tanks[0], 0);
    tankSetDestroyed(&gs->tanks[0], TRUE);
    tankSetDeathWait(&gs->tanks[0], 1);

    for (i = 0; i < 100; i++) {
        tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
    }
    UT_ASSERT_MSG(tankGetDeathWait(&gs->tanks[0]) == 1,
                  "the wait must be held at one, is %d",
                  tankGetDeathWait(&gs->tanks[0]));
    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[0]),
                  "the tank must still be dead after a hundred ticks");
    UT_ASSERT_MSG(pc.respawnAsks == 100,
                  "each held tick must ask again, asked %d times",
                  pc.respawnAsks);
    UT_ASSERT_MSG(pc.lastPlayer == 0,
                  "the respawn question named player %u, wanted 0",
                  (unsigned)pc.lastPlayer);

    pc.respawn = true;
    tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
    UT_ASSERT_MSG(tankGetDeathWait(&gs->tanks[0]) == 0,
                  "the wait must run out the tick the answer changes, is %d",
                  tankGetDeathWait(&gs->tanks[0]));
    UT_ASSERT_MSG(!tankIsDestroyed(&gs->tanks[0]),
                  "the tank must be back");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 8. With nothing registered every decision is the classic one.
 * ================================================================ */
int run_scenario_policy_null_is_classic(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    GameSim *gs;
    BYTE numStarts;
    BYTE enginePick;
    int i;
    int slot;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    UT_ASSERT_MSG(sim->scenarioPolicy == NULL,
                  "setup: a fresh sim must carry no policy");
    gs = serverSimGetGameSim(sim);

    /* The start selection is the engine's, and the batch slot is spent. */
    numStarts = startsGetNumStarts(&gs->ss);
    UT_ASSERT(numStarts >= 2);
    enginePick = (BYTE)(numStarts - 1);
    gs->pendingStartIdx[0] = enginePick;
    UT_ASSERT_MSG(plSelectStart(gs, 0) == (int)enginePick,
                  "the batch slot must still be what a selection uses");
    UT_ASSERT_MSG(gs->pendingStartIdx[0] == MAX_STARTS,
                  "the batch slot must still be consumed on use");

    /* The base sweep still ends the round. */
    serverSimAddPlayer(sim, 0, "Sweeper", false);
    plSweepBases(sim, 0);
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, FALSE) == TRUE,
                  "a full-ownership board must still be a win");

    /* The free-slot search is still uncapped. */
    for (i = 0; i < PL_CAP; i++) {
        sim->playerConnected[i] = TRUE;
    }
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == PL_CAP,
                  "a person must still take the seventh seat");
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, true) == PL_CAP,
                  "and so must a bot");
    serverSimDestroy(sim);

    /* The loadout is the sim's game type, and a dead tank comes back. */
    sim = plRunningSim(gameStrictTournament);
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Tester", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs->tanks[0] != NULL);
    plKillAndRespawn(gs, 0);
    UT_ASSERT_MSG(tankGetShells(&gs->tanks[0]) == 0 &&
                  tankGetArmour(&gs->tanks[0]) == TANK_FULL_ARMOUR,
                  "a respawn must still be a strict-tournament one, got "
                  "%u shells and %u armour",
                  (unsigned)tankGetShells(&gs->tanks[0]),
                  (unsigned)tankGetArmour(&gs->tanks[0]));

    tankSetArmour(&gs->tanks[0], 0);
    tankSetDestroyed(&gs->tanks[0], TRUE);
    tankSetDeathWait(&gs->tanks[0], 1);
    tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
    UT_ASSERT_MSG(tankGetDeathWait(&gs->tanks[0]) == 0 &&
                  !tankIsDestroyed(&gs->tanks[0]),
                  "the wait must still run out on its own");
    serverSimDestroy(sim);

    /* And the lobby may still make a team. */
    UT_ASSERT(plMakeBrainFile("null_is_classic"));
    ut_brain_stub_arm(true);
    sim = plLobbySim();
    UT_ASSERT(sim != NULL);
    slot = plAddBotOnTeam(sim, 3);
    UT_ASSERT_MSG(slot >= 0, "the add must be accepted");
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == 3,
                  "the bot must still land on team 3, is on %u",
                  (unsigned)sim->lobbyPlayers[slot].teamNumber);
    serverSimDestroy(sim);
    plDropBrainFile();
    return 0;
}

/* ================================================================
 * 9. Answers a careless script can give, none of which may hurt the sim.
 *
 * chooseStart returning true without writing leaves the engine's pick; a
 * negative or absurd maxPlayers leaves the seat search where it was.
 * ================================================================ */
int run_scenario_policy_hostile_answers(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    ScenarioPolicy pol;
    PlCtx pc;
    GameSim *gs;
    BYTE numStarts;
    BYTE enginePick;
    int i;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    gs = serverSimGetGameSim(sim);
    numStarts = startsGetNumStarts(&gs->ss);
    UT_ASSERT(numStarts >= 2);
    enginePick = (BYTE)(numStarts - 1);

    plFillPolicy(&pol, &pc);
    pc.startAnswers = true;
    pc.startWritesNothing = true;
    serverSimSetScenarioPolicy(sim, &pol);

    /* The out-parameter starts as MAX_STARTS in the caller, so an answer that
       writes nothing reads as out of range and the engine's pick stands. The
       reservation is consumed first, so the pick is the one below. */
    gs->pendingStartIdx[0] = MAX_STARTS;
    for (i = 0; i < 10; i++) {
        int chosen = plSelectStart(gs, 0);
        UT_ASSERT_MSG(chosen >= 0 && chosen < (int)numStarts,
                      "a policy that wrote nothing placed the tank at %d",
                      chosen);
    }
    gs->pendingStartIdx[0] = enginePick;
    UT_ASSERT_MSG(plSelectStart(gs, 0) == (int)enginePick,
                  "a reservation must be honoured whatever the policy answers");

    for (i = 0; i < PL_CAP; i++) {
        sim->playerConnected[i] = TRUE;
    }
    pc.maxPlayers = -5;
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == PL_CAP,
                  "a negative cap must leave the search where it was");
    pc.maxPlayers = 1000000;
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == PL_CAP,
                  "an absurd cap must leave the search where it was");
    pc.maxPlayers = 1;
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == -1,
                  "a cap of one with six seated must refuse");
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, true) == PL_CAP,
                  "a bot seats past every human cap");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 10. A start named by an op outranks the placement policy.
 *
 * The policy names the last start for everyone. A teleport to start 0 must
 * land at start 0: the op is the scenario choosing, and the policy is asked
 * only when the engine is choosing.
 * ================================================================ */
int run_scenario_policy_named_start_outranks_choose_start(void) {
    ServerSim *sim = plRunningSim(gameOpen);
    ScenarioPolicy pol;
    PlCtx pc;
    GameSim *gs;
    ScenarioOp op;
    BYTE numStarts;

    UT_ASSERT_MSG(sim != NULL, "plRunningSim returned NULL");
    gs = serverSimGetGameSim(sim);
    numStarts = startsGetNumStarts(&gs->ss);
    UT_ASSERT(numStarts >= 3);

    plFillPolicy(&pol, &pc);
    pc.startAnswers = true;
    pc.startNamed   = (BYTE)(numStarts - 1);
    serverSimSetScenarioPolicy(sim, &pol);

    serverSimAddPlayer(sim, 0, "Tester", false);
    UT_ASSERT(gs->tanks[0] != NULL);
    UT_ASSERT_MSG(plTankNearestStart(gs, 0) == (int)pc.startNamed,
                  "setup: the policy should have placed the tank at its start");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_TELEPORT;
    op.u.tankTeleport.slot  = 0;
    op.u.tankTeleport.mode  = SCN_TELEPORT_START;
    op.u.tankTeleport.start = 0;
    op.u.tankTeleport.dir   = SCN_NONE;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the teleport to a named start was refused");
    UT_ASSERT_MSG(plTankNearestStart(gs, 0) == 0,
                  "the tank landed nearest start %d after a teleport to start 0 "
                  "under a policy naming %u: the policy pre-empted the op",
                  plTankNearestStart(gs, 0), (unsigned)pc.startNamed);
    UT_ASSERT_MSG(gs->scenarioStartIdx[0] == MAX_STARTS,
                  "the op left its start slot set: %u",
                  (unsigned)gs->scenarioStartIdx[0]);

    /* And a start the op does not name is still the policy's to choose. */
    op.u.tankTeleport.start = SCN_NONE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(plTankNearestStart(gs, 0) == (int)pc.startNamed,
                  "with no start named the policy must still decide");

    serverSimDestroy(sim);
    return 0;
}
