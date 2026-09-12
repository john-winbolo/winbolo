/*
 * The six roster ops a scenario changes who is playing with.
 *
 * Three of them change a running round — spawn a bot, remove one, move a
 * player to a team — and three do the same to a lobby. Building a bot means
 * a Lua VM and a ClientSim and removing one tears both down, so the two
 * in-round arms queue and the sim makes one change a tick; the lobby arms
 * apply where they stand.
 *
 * Each case asks three things of its arm: does every refusal in its contract
 * come back under its own code, does the change land on the sim, and does a
 * refusal leave the roster exactly as it found it.
 *
 * The unit binary has no Lua brain — luabrainshandler.c stays out of it for
 * its dependency closure — so luaBrainInstanceCreate is the fixture brain in
 * test_stubs.c. Armed, it reports success and writes down the init table its
 * bot was handed, which is the last thing the sim does with that table
 * before a real VM would read it out as BRAIN_INIT.
 *
 * run_scenario_roster_spawn_refusals  — every refusal the spawn arm answers
 * run_scenario_roster_spawn_lands     — the bot, its team and its init table
 * run_scenario_roster_spawn_paced     — ten asked for at once, one a tick
 * run_scenario_roster_team_during_add — the team is in place as the add
 *                                       picks the slot's start
 * run_scenario_roster_remove_bot      — humans refused, the removal paced
 * run_scenario_roster_set_team        — the write, and a team off the end
 * run_scenario_lobby_add_bot          — the lobby add and its refusals
 * run_scenario_lobby_remove_bot       — the lobby removal and its refusals
 * run_scenario_lobby_set_team         — the lobby write and its refusals
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, lobbyPlayers, the roster queue */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "server_sim_scenario.h"
#include "server_sim_join.h"       /* serverSimFindFreeSlot */
#include "game_sim.h"
#include "tank.h"                  /* tankGetWorld */
#include "starts.h"                /* startsGetNumStarts */
#include "everard_map.h"
#include "test_harness.h"

/* A file for the brain path to name. The fixture brain never opens it — the
 * arm reads the path to refuse one that names nothing, which is the check
 * being satisfied here.
 *
 * The name carries the case's own name, because ctest runs the cases as
 * concurrent processes in one working directory. With one name between them,
 * the first case to reach its drop removes the file the others are still
 * naming, scenarioBrainPath then answers SCN_OP_NOT_FOUND, and the case that
 * lost the file fails on an assertion about its arm instead. The tag is a
 * required argument so a case added later cannot quietly share a name. */
static char raBrainPath[128];

static bool raMakeBrainFile(const char *tag) {
    FILE *f;

    SDL_snprintf(raBrainPath, sizeof(raBrainPath),
                 "test_scenario_roster_brain_%s.lua", tag);
    f = fopen(raBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static void raDropBrainFile(void) {
    remove(raBrainPath);
}

/* A running round with one human in slot 0 and a server configured to run
 * bots, which is what every in-round arm needs before it looks at a
 * payload. */
static ServerSim *raRunningSim(void) {
    ServerSim *sim = ut_make_running_sim("Human");
    if (sim == NULL) return NULL;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, raBrainPath);
    return sim;
}

/* A lobby taking part, with one ready human in slot 0. */
static ServerSim *raLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    sim->lobbyPlayers[0].ready = true;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, raBrainPath);
    return sim;
}

static void raSpawnOp(ScenarioOp *op, BYTE slot, BYTE team,
                      const char *name, const char *brain) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ROSTER_SPAWN_BOT;
    op->u.rosterSpawnBot.slot  = slot;
    op->u.rosterSpawnBot.team  = team;
    op->u.rosterSpawnBot.start = SCN_NONE;
    if (name != NULL) {
        SDL_strlcpy(op->u.rosterSpawnBot.name, name, PLAYER_NAME_LEN);
    }
    if (brain != NULL) {
        SDL_strlcpy(op->u.rosterSpawnBot.brain, brain, SCN_PATH_MAX);
    }
}

static void raLobbyAddOp(ScenarioOp *op, BYTE team, bool fielded,
                         const char *name, const char *brain) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_LOBBY_ADD_BOT;
    op->u.lobbyAddBot.slot    = SCN_NONE;
    op->u.lobbyAddBot.team    = team;
    op->u.lobbyAddBot.fielded = fielded;
    if (name != NULL) {
        SDL_strlcpy(op->u.lobbyAddBot.name, name, PLAYER_NAME_LEN);
    }
    if (brain != NULL) {
        SDL_strlcpy(op->u.lobbyAddBot.brain, brain, SCN_PATH_MAX);
    }
}

static void raSlotOp(ScenarioOp *op, ScenarioOpType type, BYTE slot) {
    memset(op, 0, sizeof(*op));
    op->type = type;
    /* Every one-slot payload here starts with its slot byte, but they are
       different structs, so each is written through its own member. */
    if (type == SCN_OP_ROSTER_REMOVE_BOT) {
        op->u.rosterRemoveBot.slot = slot;
    } else {
        op->u.lobbyRemoveBot.slot = slot;
    }
}

static void raTeamOp(ScenarioOp *op, ScenarioOpType type, BYTE slot,
                     BYTE team) {
    memset(op, 0, sizeof(*op));
    op->type = type;
    if (type == SCN_OP_ROSTER_SET_TEAM) {
        op->u.rosterSetTeam.slot = slot;
        op->u.rosterSetTeam.team = team;
    } else {
        op->u.lobbySetTeam.slot = slot;
        op->u.lobbySetTeam.team = team;
    }
}

/* How many slots hold a bot. */
static int raBotCount(ServerSim *sim) {
    int n = 0, i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) n++;
    }
    return n;
}

/* ── Spawn ───────────────────────────────────────────────────────── */

int run_scenario_roster_spawn_refusals(void) {
    ServerSim *sim;
    ScenarioOp op;
    int i;

    UT_ASSERT(raMakeBrainFile("scenario_roster_spawn_refusals"));
    ut_brain_stub_arm(false);

    /* An in-round arm in a lobby is refused on the state. */
    sim = raLobbySim();
    UT_ASSERT(sim != NULL);
    raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE,
                  "a spawn in the lobby must be refused on the state");
    serverSimDestroy(sim);

    sim = raRunningSim();
    UT_ASSERT(sim != NULL);

    /* A server that runs no bots answers the way its lobby's Add Bot does. */
    serverSimSetBotAiType(sim, aiNone);
    raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    serverSimSetBotAiType(sim, aiFull);

    /* A team off the end of the table, which the batch setter would quietly
       coerce to team 1 if the arm let it through. */
    raSpawnOp(&op, SCN_NONE, MAX_TANKS, NULL, NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE,
                  "a team at MAX_TANKS must be refused, not coerced");

    /* A seat that is not a seat, and one that is already fielded. */
    raSpawnOp(&op, MAX_TANKS, 0, NULL, NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    raSpawnOp(&op, 0, 0, NULL, NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY,
                  "slot 0 holds the human, so a spawn into it is ALREADY");

    /* A brain nothing resolves: a package name, and a path to no file. */
    raSpawnOp(&op, SCN_NONE, 0, NULL, "package:Hunter");
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NOT_FOUND,
                  "a package brain has nothing to resolve it yet");
    raSpawnOp(&op, SCN_NONE, 0, NULL, "no_such_brain_at_all.lua");
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NOT_FOUND);

    /* A name field with no terminator in it. */
    raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
    memset(op.u.rosterSpawnBot.name, 'n', PLAYER_NAME_LEN);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG);

    /* Nothing above was queued. */
    UT_ASSERT_MSG(sim->scenarioRosterCount == 0,
                  "a refused spawn must not queue: %d outstanding",
                  (int)sim->scenarioRosterCount);

    /* The queue takes SCN_ROSTER_QUEUE_MAX and refuses the one past it. */
    for (i = 0; i < SCN_ROSTER_QUEUE_MAX; i++) {
        raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
        UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED,
                      "spawn %d of %d should have queued", i, SCN_ROSTER_QUEUE_MAX);
    }
    raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_FULL,
                  "the queue is full and must refuse rather than displace");
    serverSimScenarioResetRoster(sim);

    /* Every seat taken, so there is nowhere to put one. */
    for (i = 1; i < MAX_TANKS; i++) {
        serverSimAddPlayer(sim, (BYTE)i, "Filler", false);
    }
    raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_FULL,
                  "a full roster has no seat for a spawn");

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

int run_scenario_roster_spawn_lands(void) {
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;
    const ScnTable *seen;
    int i;
    BYTE slot = SCN_NONE;

    UT_ASSERT(raMakeBrainFile("scenario_roster_spawn_lands"));
    ut_brain_stub_arm(true);
    sim = raRunningSim();
    UT_ASSERT(sim != NULL);

    memset(&out, 0, sizeof(out));
    raSpawnOp(&op, SCN_NONE, 3, "Scout", NULL);
    op.u.rosterSpawnBot.init.count = 1;
    SDL_strlcpy(op.u.rosterSpawnBot.init.kv[0].key, "role",
                SCN_TABLE_KEY_LEN);
    SDL_strlcpy(op.u.rosterSpawnBot.init.kv[0].value, "scout",
                SCN_TABLE_VALUE_LEN);

    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_QUEUED);
    UT_ASSERT_MSG(raBotCount(sim) == 0,
                  "a queued spawn must not have landed yet");

    serverSimTick(sim);
    UT_ASSERT_MSG(raBotCount(sim) == 1, "the spawn should have landed");
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) { slot = (BYTE)i; break; }
    }
    UT_ASSERT(slot != SCN_NONE);

    /* The team the op asked for, written whole rather than coerced. */
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == 3,
                  "slot %d is on team %d, expected 3",
                  (int)slot, (int)sim->lobbyPlayers[slot].teamNumber);

    /* The init table reached the call that makes the brain. */
    UT_ASSERT_MSG(ut_brain_stub_made(slot),
                  "no brain was made for slot %d", (int)slot);
    seen = ut_brain_stub_init(slot);
    UT_ASSERT(seen != NULL);
    UT_ASSERT_MSG(seen->count == 1, "the brain was handed %d pairs, expected 1",
                  (int)seen->count);
    UT_ASSERT(strcmp(seen->kv[0].key, "role") == 0);
    UT_ASSERT_MSG(strcmp(seen->kv[0].value, "scout") == 0,
                  "the brain was handed role='%s'", seen->kv[0].value);

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

/* Ten asked for in one tick arrive over ten ticks, each with its own seat
 * and its own init table. This is the whole point of the queue: a script
 * names the bots it wants and the sim decides how fast to build them. */
int run_scenario_roster_spawn_paced(void) {
    ServerSim *sim;
    ScenarioOp op;
    int i;

    UT_ASSERT(raMakeBrainFile("scenario_roster_spawn_paced"));
    ut_brain_stub_arm(true);
    sim = raRunningSim();
    UT_ASSERT(sim != NULL);

    for (i = 0; i < 10; i++) {
        char value[SCN_TABLE_VALUE_LEN];
        raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
        op.u.rosterSpawnBot.init.count = 1;
        SDL_strlcpy(op.u.rosterSpawnBot.init.kv[0].key, "n", SCN_TABLE_KEY_LEN);
        SDL_snprintf(value, sizeof(value), "%d", i);
        SDL_strlcpy(op.u.rosterSpawnBot.init.kv[0].value, value,
                    SCN_TABLE_VALUE_LEN);
        UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    }
    UT_ASSERT_MSG(sim->scenarioRosterCount == 10,
                  "%d queued, expected 10", (int)sim->scenarioRosterCount);
    UT_ASSERT(raBotCount(sim) == 0);

    for (i = 1; i <= 10; i++) {
        serverSimTick(sim);
        UT_ASSERT_MSG(raBotCount(sim) == i,
                      "after %d ticks there are %d bots, expected %d",
                      i, raBotCount(sim), i);
        UT_ASSERT_MSG(sim->scenarioRosterCount == (uint8_t)(10 - i),
                      "after %d ticks %d are still queued, expected %d",
                      i, (int)sim->scenarioRosterCount, 10 - i);
    }

    /* Ten seats, ten tables: each bot kept the one its own spawn carried. */
    {
        int found = 0;
        for (i = 0; i < MAX_TANKS; i++) {
            const ScnTable *t;
            if (!serverSimIsBot(sim, (BYTE)i)) continue;
            t = ut_brain_stub_init(i);
            UT_ASSERT(t != NULL && t->count == 1);
            found++;
        }
        UT_ASSERT_MSG(found == 10, "%d bots carried a table, expected 10", found);
    }

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

/* The team is on the slot while the add runs, rather than written onto it
 * afterwards.
 *
 * serverSimAddBot writes the team out of the config it is handed and then
 * picks the slot's lobby start from it, clustering a new bot near the
 * reservations its team already holds. A caller that adds the bot and then
 * moves it onto its team has missed that pick: the start was chosen for a
 * slot that had no team at all.
 *
 * What the pick saw is what this reads, through the team the fixture brain
 * records as it is made — the brain is created inside the same add, a few
 * steps after the team write, so the team it sees is the team the start
 * pick saw. The reservation itself does not tell the two orderings apart:
 * serverSimSetTeam re-runs the same pick with the right team and
 * startsPickIncremental is deterministic, so a bot moved onto its team
 * afterwards ends on the same start. What differs is whether the add ever
 * had the team. */
int run_scenario_roster_team_during_add(void) {
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;
    BYTE slot = SCN_NONE;
    int i;

    UT_ASSERT(raMakeBrainFile("scenario_roster_team_during_add"));
    ut_brain_stub_arm(true);

    /* A lobby whose one human is on team 2 and holds a start, so the team
       a bot joins has a reservation to be clustered toward. */
    sim = raLobbySim();
    UT_ASSERT(sim != NULL);
    serverSimSetTeam(sim, 0, 2);
    serverSimSetLobbyStartIdx(sim, 0, 1);
    UT_ASSERT(sim->lobbyPlayers[0].teamNumber == 2);

    memset(&out, 0, sizeof(out));
    raLobbyAddOp(&op, 2, true, "Wing", NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK);
    UT_ASSERT_MSG(ut_brain_stub_team(out.slot) == 2,
                  "the add saw team %d on slot %d, expected 2 — the team "
                  "reached the slot only after its start was picked",
                  ut_brain_stub_team(out.slot), (int)out.slot);
    UT_ASSERT(sim->lobbyPlayers[out.slot].teamNumber == 2);
    UT_ASSERT_MSG(sim->lobbyPlayers[out.slot].startIdx != 0xFF,
                  "the bot came out of the add with no reservation");
    serverSimDestroy(sim);

    /* The in-round spawn reaches the add the same way. */
    sim = raRunningSim();
    UT_ASSERT(sim != NULL);
    raSpawnOp(&op, SCN_NONE, 3, NULL, NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) { slot = (BYTE)i; break; }
    }
    UT_ASSERT(slot != SCN_NONE);
    UT_ASSERT_MSG(ut_brain_stub_team(slot) == 3,
                  "the add saw team %d on slot %d, expected 3",
                  ut_brain_stub_team(slot), (int)slot);
    UT_ASSERT(sim->lobbyPlayers[slot].teamNumber == 3);

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

/* ── Remove ──────────────────────────────────────────────────────── */

int run_scenario_roster_remove_bot(void) {
    ServerSim *sim;
    ScenarioOp op;
    BYTE slot = SCN_NONE;
    int i;

    UT_ASSERT(raMakeBrainFile("scenario_roster_remove_bot"));
    ut_brain_stub_arm(true);

    /* In a lobby the in-round arm is refused on the state. */
    sim = raLobbySim();
    UT_ASSERT(sim != NULL);
    raSlotOp(&op, SCN_OP_ROSTER_REMOVE_BOT, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    serverSimDestroy(sim);

    sim = raRunningSim();
    UT_ASSERT(sim != NULL);

    /* An empty seat, a seat off the end, and the human in slot 0. */
    raSlotOp(&op, SCN_OP_ROSTER_REMOVE_BOT, 5);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    raSlotOp(&op, SCN_OP_ROSTER_REMOVE_BOT, MAX_TANKS);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    raSlotOp(&op, SCN_OP_ROSTER_REMOVE_BOT, 0);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_IS_HUMAN,
                  "a remove aimed at a human must be refused");
    UT_ASSERT(sim->scenarioRosterCount == 0);
    UT_ASSERT(serverSimIsPlayerConnected(sim, 0));

    /* Put a bot in, then take it out again. */
    raSpawnOp(&op, SCN_NONE, 0, NULL, NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) { slot = (BYTE)i; break; }
    }
    UT_ASSERT(slot != SCN_NONE);

    raSlotOp(&op, SCN_OP_ROSTER_REMOVE_BOT, slot);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    UT_ASSERT_MSG(serverSimIsBot(sim, slot),
                  "a queued removal must not have landed yet");
    serverSimTick(sim);
    UT_ASSERT_MSG(!serverSimIsBot(sim, slot), "the removal should have landed");
    UT_ASSERT(!serverSimIsPlayerConnected(sim, slot));

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

/* ── Set team ────────────────────────────────────────────────────── */

int run_scenario_roster_set_team(void) {
    ServerSim *sim;
    ScenarioOp op;

    UT_ASSERT(raMakeBrainFile("scenario_roster_set_team"));
    ut_brain_stub_arm(false);

    sim = raLobbySim();
    UT_ASSERT(sim != NULL);
    raTeamOp(&op, SCN_OP_ROSTER_SET_TEAM, 0, 2);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE,
                  "the in-round team arm is refused in a lobby");
    serverSimDestroy(sim);

    sim = raRunningSim();
    UT_ASSERT(sim != NULL);

    raTeamOp(&op, SCN_OP_ROSTER_SET_TEAM, 5, 2);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    raTeamOp(&op, SCN_OP_ROSTER_SET_TEAM, MAX_TANKS, 2);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    /* Move the slot onto team 3 before asking for a refusal. Two different
       things put team 1 on a slot — the team a join hands a fresh slot
       (addPlayerInternal) and the team serverSimSetTeamBatch coerces an
       out-of-range one to — so a refusal that wrote anyway is invisible
       against either 0 or 1. Against 3 it is a change. */
    raTeamOp(&op, SCN_OP_ROSTER_SET_TEAM, 0, 3);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(sim->lobbyPlayers[0].teamNumber == 3);

    raTeamOp(&op, SCN_OP_ROSTER_SET_TEAM, 0, MAX_TANKS);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE,
                  "a team at MAX_TANKS must be refused, not coerced to 1");
    UT_ASSERT_MSG(sim->lobbyPlayers[0].teamNumber == 3,
                  "the refused team must not have been written: team %d",
                  (int)sim->lobbyPlayers[0].teamNumber);

    raTeamOp(&op, SCN_OP_ROSTER_SET_TEAM, 0, 4);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(sim->lobbyPlayers[0].teamNumber == 4,
                  "slot 0 is on team %d, expected 4",
                  (int)sim->lobbyPlayers[0].teamNumber);

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

/* ── The lobby three ─────────────────────────────────────────────── */

int run_scenario_lobby_add_bot(void) {
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;

    UT_ASSERT(raMakeBrainFile("scenario_lobby_add_bot"));
    ut_brain_stub_arm(true);

    /* A lobby arm in a running round is refused on the state. */
    sim = raRunningSim();
    UT_ASSERT(sim != NULL);
    raLobbyAddOp(&op, 0, true, NULL, NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE,
                  "a lobby add in a running round must be refused");
    serverSimDestroy(sim);

    sim = raLobbySim();
    UT_ASSERT(sim != NULL);

    /* A seat with nobody fielded in it has no entry point on the sim, so it
       is refused rather than quietly given a fielded bot instead. */
    raLobbyAddOp(&op, 0, false, NULL, NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);

    raLobbyAddOp(&op, MAX_TANKS, true, NULL, NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);

    raLobbyAddOp(&op, 0, true, NULL, "package:Hunter");
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NOT_FOUND);
    raLobbyAddOp(&op, 0, true, NULL, "no_such_brain_at_all.lua");
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NOT_FOUND);

    /* The operator's cap, which the lobby's own Add Bot enforces. Written
       straight onto the sim: -maxbots is read at startup and there is no
       setter for it. */
    sim->maxBots = 1;
    UT_ASSERT(raBotCount(sim) == 0);
    memset(&out, 0, sizeof(out));
    raLobbyAddOp(&op, 2, true, "Helper", NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK,
                  "the first bot is inside the cap");
    UT_ASSERT_MSG(raBotCount(sim) == 1, "the bot should be in the lobby");
    UT_ASSERT_MSG(serverSimIsBot(sim, out.slot),
                  "out.slot %d does not hold the bot", (int)out.slot);
    UT_ASSERT_MSG(sim->lobbyPlayers[out.slot].teamNumber == 2,
                  "the bot is on team %d, expected 2",
                  (int)sim->lobbyPlayers[out.slot].teamNumber);
    /* A lobby add carries no configuration, so its brain gets an empty
       table rather than somebody else's. */
    UT_ASSERT(ut_brain_stub_made(out.slot));
    UT_ASSERT(ut_brain_stub_init(out.slot)->count == 0);

    raLobbyAddOp(&op, 0, true, "Second", NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_FULL,
                  "the second bot is past the cap");
    UT_ASSERT(raBotCount(sim) == 1);

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

int run_scenario_lobby_remove_bot(void) {
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;

    UT_ASSERT(raMakeBrainFile("scenario_lobby_remove_bot"));
    ut_brain_stub_arm(true);

    sim = raRunningSim();
    UT_ASSERT(sim != NULL);
    raSlotOp(&op, SCN_OP_LOBBY_REMOVE_BOT, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    serverSimDestroy(sim);

    sim = raLobbySim();
    UT_ASSERT(sim != NULL);

    raSlotOp(&op, SCN_OP_LOBBY_REMOVE_BOT, 5);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    raSlotOp(&op, SCN_OP_LOBBY_REMOVE_BOT, MAX_TANKS);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    raSlotOp(&op, SCN_OP_LOBBY_REMOVE_BOT, 0);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_IS_HUMAN,
                  "a lobby remove aimed at a human must be refused");
    UT_ASSERT(serverSimIsPlayerConnected(sim, 0));

    memset(&out, 0, sizeof(out));
    raLobbyAddOp(&op, 0, true, "Helper", NULL);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK);
    UT_ASSERT(serverSimIsBot(sim, out.slot));

    raSlotOp(&op, SCN_OP_LOBBY_REMOVE_BOT, out.slot);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "a lobby removal applies where it stands");
    UT_ASSERT(!serverSimIsBot(sim, out.slot));
    UT_ASSERT(!serverSimIsPlayerConnected(sim, out.slot));

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

int run_scenario_lobby_set_team(void) {
    ServerSim *sim;
    ScenarioOp op;

    UT_ASSERT(raMakeBrainFile("scenario_lobby_set_team"));
    ut_brain_stub_arm(false);

    sim = raRunningSim();
    UT_ASSERT(sim != NULL);
    raTeamOp(&op, SCN_OP_LOBBY_SET_TEAM, 0, 2);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    serverSimDestroy(sim);

    sim = raLobbySim();
    UT_ASSERT(sim != NULL);

    raTeamOp(&op, SCN_OP_LOBBY_SET_TEAM, 5, 2);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER,
                  "an empty slot has no team to set");

    /* Move the slot onto team 3 before asking for a refusal. Two different
       things put team 1 on a slot — the team a join hands a fresh slot
       (addPlayerInternal) and the team serverSimSetTeamBatch coerces an
       out-of-range one to — so a refusal that wrote anyway is invisible
       against either 0 or 1. Against 3 it is a change. */
    raTeamOp(&op, SCN_OP_LOBBY_SET_TEAM, 0, 3);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(sim->lobbyPlayers[0].teamNumber == 3);

    raTeamOp(&op, SCN_OP_LOBBY_SET_TEAM, 0, MAX_TANKS);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE,
                  "a team at MAX_TANKS must be refused, not coerced to 1");
    UT_ASSERT_MSG(sim->lobbyPlayers[0].teamNumber == 3,
                  "the refused team must not have been written: team %d",
                  (int)sim->lobbyPlayers[0].teamNumber);

    /* The write, and the auto-unready the lobby's own team command makes:
       a roster change puts the lobby back to gathering. The successful
       write above already cleared the flag, so arm it again to watch this
       one clear it. */
    sim->lobbyPlayers[0].ready = true;
    raTeamOp(&op, SCN_OP_LOBBY_SET_TEAM, 0, 5);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(sim->lobbyPlayers[0].teamNumber == 5,
                  "slot 0 is on team %d, expected 5",
                  (int)sim->lobbyPlayers[0].teamNumber);
    UT_ASSERT_MSG(!sim->lobbyPlayers[0].ready,
                  "the team change should have unreadied the lobby");

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

/* ── Seats ───────────────────────────────────────────────────────── */

/* The operator's -maxplayers caps humans, not bots: with a cap of one and
 * the one human seated, a scripted spawn still lands, in the first seat
 * past the cap, and the lobby add takes the seat it names. */
int run_scenario_roster_bots_seat_past_the_human_cap(void) {
    ServerSim *sim;
    ScenarioOp op;
    ScnOpOut out;

    UT_ASSERT(raMakeBrainFile("scenario_roster_bots_seat_past_the_human_cap"));
    ut_brain_stub_arm(true);
    sim = raRunningSim();
    UT_ASSERT(sim != NULL);
    sim->maxPlayers = 1;

    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) == -1,
                  "setup: with a cap of one and a human seated, no human seat");
    raSpawnOp(&op, SCN_NONE, 2, "Wave", NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED,
                  "a spawn was refused by the human cap");
    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimIsBot(sim, 1),
                  "the bot did not land in the first seat past the cap");
    serverSimDestroy(sim);

    sim = raLobbySim();
    UT_ASSERT(sim != NULL);
    sim->maxPlayers = 1;
    memset(&out, 0, sizeof(out));
    raLobbyAddOp(&op, 0, true, "Named", NULL);
    op.u.lobbyAddBot.slot = 5;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK,
                  "a lobby add naming seat 5 was refused");
    UT_ASSERT_MSG(out.slot == 5 && serverSimIsBot(sim, 5),
                  "the lobby add took seat %u, wanted the named 5",
                  (unsigned)out.slot);
    raLobbyAddOp(&op, 0, true, "Second", NULL);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_OK,
                  "a lobby add for the first free seat was refused by the cap");
    UT_ASSERT_MSG(out.slot == 1, "the first free seat was %u, wanted 1",
                  (unsigned)out.slot);
    /* And the same seat again is refused rather than doubled up. */
    raLobbyAddOp(&op, 0, true, "Again", NULL);
    op.u.lobbyAddBot.slot = 5;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, &out) == SCN_OP_ALREADY);

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}

/* A spawn that names a start lands there, and a start the map does not have
 * is refused when the op is made rather than dropped when it lands. */
int run_scenario_roster_spawn_named_start(void) {
    ServerSim *sim;
    ScenarioOp op;
    GameSim *gs;
    BYTE numStarts;
    BYTE slot = SCN_NONE;
    int i;

    UT_ASSERT(raMakeBrainFile("scenario_roster_spawn_named_start"));
    ut_brain_stub_arm(true);
    sim = raRunningSim();
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    numStarts = startsGetNumStarts(&gs->ss);
    UT_ASSERT(numStarts >= 2);

    raSpawnOp(&op, SCN_NONE, 2, "Placed", NULL);
    op.u.rosterSpawnBot.start = numStarts;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE,
                  "a start past the end was not refused");
    op.u.rosterSpawnBot.start = (BYTE)(numStarts - 1);
    op.u.rosterSpawnBot.loadout = 1;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE,
                  "a loadout override was accepted before one exists");
    op.u.rosterSpawnBot.loadout = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) { slot = (BYTE)i; break; }
    }
    UT_ASSERT_MSG(slot != SCN_NONE, "the spawn did not land");
    UT_ASSERT(gs->tanks[slot] != NULL);
    {
        WORLD wx, wy;
        int mx, my, best = -1, bestDist = 0;
        tankGetWorld(&gs->tanks[slot], &wx, &wy);
        mx = (int)(wx >> M_W_SHIFT_SIZE);
        my = (int)(wy >> M_W_SHIFT_SIZE);
        for (i = 0; i < (int)numStarts; i++) {
            int dx = (int)(*gs->ss).item[i].x - mx;
            int dy = (int)(*gs->ss).item[i].y - my;
            int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (best < 0 || dist < bestDist) { best = i; bestDist = dist; }
        }
        UT_ASSERT_MSG(best == (int)(numStarts - 1),
                      "the bot landed nearest start %d, wanted the named %u",
                      best, (unsigned)(numStarts - 1));
    }
    UT_ASSERT_MSG(gs->scenarioStartIdx[slot] == MAX_STARTS,
                  "the spawn left its start slot set");

    serverSimDestroy(sim);
    raDropBrainFile();
    return 0;
}
