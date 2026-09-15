/*
 * game.bot_init — new data for a bot that is already playing.
 *
 * A spawn hands a bot its init table once, at its first breath. This op
 * hands one to a bot in the middle of a round: the bot's own copy is
 * replaced, its BRAIN_INIT global is rebuilt, and the on_init off its own
 * table (the 'brain' global) is called with the same table when the brain
 * has written one.
 *
 * The two halves are tested apart, because the unit binary has no Lua brain
 * — luabrainshandler.c stays out of its dependency closure, so
 * luaBrainInstanceCreate is the fixture in test_stubs.c and a bot here has
 * no lua_State at all.
 *
 *   - the op half is driven through serverSimApplyScenarioOp on a running
 *     sim with a stub-brained bot in a seat, and read back off the bot with
 *     botManagerGetBotInitTable;
 *   - the Lua half is driven straight at brainCoreUpdateInitTable on a VM
 *     the test makes itself, with a fixture brain standing in for a real
 *     one — the same shape test_bot_init_table.c uses for the spawn path.
 *
 * run_scenario_bot_init_lands     — the table replaces the spawn's, a tick
 *                                   after the op, and replaces it WHOLE
 * run_scenario_bot_init_refusals  — a human seat, an empty seat, a seat off
 *                                   the end, a table too big, and a setup
 *                                   window; and none of them writes
 * run_brain_on_init_update        — the global is rebuilt either way, and
 *                                   on_init is called when it is there and
 *                                   its error is survived when it raises
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, lobbyPlayers, the roster queue */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "server_sim_scenario.h"
#include "bot_manager.h"           /* botManagerGetBotInitTable */
#include "braincore.h"             /* brainCoreUpdateInitTable */
#include "scenario_table.h"
#include "game_sim.h"
#include "everard_map.h"
#include "test_harness.h"

/* A file for the spawn's brain path to name; the fixture brain never opens
 * it. Named for this file because ctest runs cases as concurrent processes
 * in one working directory, and two cases sharing a name race each other's
 * drop. */
static char biBrainPath[128];

static bool biMakeBrainFile(const char *tag) {
    FILE *f;

    SDL_snprintf(biBrainPath, sizeof(biBrainPath),
                 "test_scenario_bot_init_brain_%s.lua", tag);
    f = fopen(biBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static void biDropBrainFile(void) {
    remove(biBrainPath);
}

static ServerSim *biRunningSim(void) {
    ServerSim *sim = ut_make_running_sim("Human");
    if (sim == NULL) return NULL;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, biBrainPath);
    return sim;
}

/* One pair onto an op's table, written the way the Lua row writes it. */
static void biPair(ScnTable *t, const char *key, const char *value) {
    SDL_strlcpy(t->kv[t->count].key, key, SCN_TABLE_KEY_LEN);
    SDL_strlcpy(t->kv[t->count].value, value, SCN_TABLE_VALUE_LEN);
    t->count++;
}

/* Spawn one bot and let it land. Answers its seat, or SCN_NONE. */
static BYTE biSpawnBot(ServerSim *sim) {
    ScenarioOp op;
    ScnOpOut   out;
    int        i;

    memset(&op, 0, sizeof(op));
    memset(&out, 0, sizeof(out));
    op.type                    = SCN_OP_ROSTER_SPAWN_BOT;
    op.u.rosterSpawnBot.slot   = SCN_NONE;
    op.u.rosterSpawnBot.team   = 2;
    op.u.rosterSpawnBot.start  = SCN_NONE;
    SDL_strlcpy(op.u.rosterSpawnBot.name, "Scout", PLAYER_NAME_LEN);
    biPair(&op.u.rosterSpawnBot.init, "role", "scout");
    biPair(&op.u.rosterSpawnBot.init, "deprive", "100");

    if (serverSimApplyScenarioOp(sim, &op, &out) != SCN_OP_QUEUED) {
        return SCN_NONE;
    }
    serverSimTick(sim);
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) return (BYTE)i;
    }
    return SCN_NONE;
}

static void biInitOp(ScenarioOp *op, BYTE slot) {
    memset(op, 0, sizeof(*op));
    op->type                 = SCN_OP_ROSTER_BOT_INIT;
    op->u.rosterBotInit.slot = slot;
}

/* The op lands a tick after it is asked for, like every other roster
 * change, and what it lands is the whole table: a key the bot was spawned
 * with and the new table does not name is gone, because a brain reads
 * BRAIN_INIT and BRAIN_INIT is rebuilt rather than merged into. */
int run_scenario_bot_init_lands(void) {
    ServerSim      *sim;
    ScenarioOp      op;
    const ScnTable *held;
    BYTE            slot;

    UT_ASSERT(biMakeBrainFile("lands"));
    ut_brain_stub_arm(true);
    sim = biRunningSim();
    UT_ASSERT(sim != NULL);

    slot = biSpawnBot(sim);
    UT_ASSERT_MSG(slot != SCN_NONE, "the spawn never landed");

    held = botManagerGetBotInitTable(sim, slot);
    UT_ASSERT(held != NULL);
    UT_ASSERT_MSG(held->count == 2, "the spawn left %d pairs, expected 2",
                  (int)held->count);

    biInitOp(&op, slot);
    biPair(&op.u.rosterBotInit.init, "noblitz", "1");
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED,
                  "bot_init should queue like the other roster ops");

    /* Not yet: the queue is drained on the tick, on the producer thread,
       which is the whole reason the op queues rather than writing where it
       stands. */
    held = botManagerGetBotInitTable(sim, slot);
    UT_ASSERT(held != NULL);
    UT_ASSERT_MSG(held->count == 2,
                  "a queued bot_init must not have landed yet, %d pairs",
                  (int)held->count);

    serverSimTick(sim);

    held = botManagerGetBotInitTable(sim, slot);
    UT_ASSERT(held != NULL);
    UT_ASSERT_MSG(held->count == 1,
                  "the new table should have replaced the old one whole, "
                  "%d pairs", (int)held->count);
    UT_ASSERT(strcmp(held->kv[0].key, "noblitz") == 0);
    UT_ASSERT_MSG(strcmp(held->kv[0].value, "1") == 0,
                  "the bot holds noblitz='%s'", held->kv[0].value);
    UT_ASSERT_MSG(scnTableGet(held, "role") == NULL,
                  "the spawn's 'role' should be gone");

    serverSimDestroy(sim);
    biDropBrainFile();
    return 0;
}

/* Every refusal in the row's contract, under its own code, and none of
 * them writing anything. */
int run_scenario_bot_init_refusals(void) {
    ServerSim      *sim;
    ScenarioOp      op;
    const ScnTable *held;
    BYTE            slot;
    int             i;

    UT_ASSERT(biMakeBrainFile("refusals"));
    ut_brain_stub_arm(true);
    sim = biRunningSim();
    UT_ASSERT(sim != NULL);

    slot = biSpawnBot(sim);
    UT_ASSERT_MSG(slot != SCN_NONE, "the spawn never landed");

    /* Slot 0 is the human this sim was built around. */
    biInitOp(&op, 0);
    biPair(&op.u.rosterBotInit.init, "noblitz", "1");
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_IS_HUMAN,
                  "a human seat should be refused as a human");

    /* An empty seat: the one after the bot, which nothing has taken. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) break;
    }
    UT_ASSERT_MSG(i < MAX_TANKS, "no empty seat to ask about");
    biInitOp(&op, (BYTE)i);
    biPair(&op.u.rosterBotInit.init, "noblitz", "1");
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) ==
                      SCN_OP_NO_SUCH_PLAYER,
                  "an empty seat should be refused as no such player");

    /* A seat off the end of the table. */
    biInitOp(&op, (BYTE)MAX_TANKS);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) ==
              SCN_OP_NO_SUCH_PLAYER);

    /* A table with more pairs than the payload carries. The Lua row cannot
       build one — scnTableSet refuses the pair that will not fit — so this
       is the funnel's own guard against a caller that is not the row. */
    biInitOp(&op, slot);
    op.u.rosterBotInit.init.count = SCN_TABLE_MAX + 1;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG,
                  "a table past the cap should be refused as too big");

    /* A value with no terminator inside its own field: it would be read
       past the end of on the way into a Lua state. */
    biInitOp(&op, slot);
    op.u.rosterBotInit.init.count = 1;
    SDL_strlcpy(op.u.rosterBotInit.init.kv[0].key, "role", SCN_TABLE_KEY_LEN);
    memset(op.u.rosterBotInit.init.kv[0].value, 'x', SCN_TABLE_VALUE_LEN);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TOO_BIG);

    /* And a setup window, where the roster rows all answer alike. */
    sim->startInProgress      = true;
    sim->scenarioSetupWindow  = true;
    biInitOp(&op, slot);
    biPair(&op.u.rosterBotInit.init, "noblitz", "1");
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) ==
                      SCN_OP_WRONG_STATE,
                  "bot_init inside a setup should answer wrong state");
    sim->startInProgress     = false;
    sim->scenarioSetupWindow = false;

    /* Nothing above wrote: the bot still holds what its spawn gave it. */
    serverSimTick(sim);
    held = botManagerGetBotInitTable(sim, slot);
    UT_ASSERT(held != NULL);
    UT_ASSERT_MSG(held->count == 2,
                  "a refused bot_init wrote something: %d pairs",
                  (int)held->count);
    UT_ASSERT(scnTableGet(held, "role") != NULL);

    serverSimDestroy(sim);
    biDropBrainFile();
    return 0;
}

/* The Lua half, on a VM of the test's own.
 *
 * The fixture brain writes down what it was handed, so the case can say
 * that on_init saw the same pairs the global holds rather than only that
 * something was called. */
static const char *BI_FIXTURE_WITH_ON_INIT =
    "brain = {}\n"
    "ON_INIT_CALLS = 0\n"
    "ON_INIT_SEEN = ''\n"
    "function brain.on_init(t)\n"
    "  ON_INIT_CALLS = ON_INIT_CALLS + 1\n"
    "  local keys = {}\n"
    "  for k in pairs(t) do keys[#keys + 1] = k end\n"
    "  table.sort(keys)\n"
    "  local out = {}\n"
    "  for _, k in ipairs(keys) do out[#out + 1] = k .. '=' .. tostring(t[k]) end\n"
    "  ON_INIT_SEEN = table.concat(out, ';')\n"
    "  ON_INIT_SAME = rawequal(t, rawget(_G, 'BRAIN_INIT'))\n"
    "end\n";

/* Reads one global as text; an absent one reads as "". */
static void biGlobalText(lua_State *L, const char *name, char *out,
                         size_t cap) {
    const char *s;

    out[0] = '\0';
    lua_getglobal(L, name);
    s = lua_tostring(L, -1);
    if (s != NULL) snprintf(out, cap, "%s", s);
    lua_pop(L, 1);
}

int run_brain_on_init_update(void) {
    lua_State *L;
    ScnTable   first;
    ScnTable   second;
    char       seen[256];
    char       why[256];

    scnTableClear(&first);
    scnTableClear(&second);
    UT_ASSERT(scnTableSet(&first, "role", "scout"));
    UT_ASSERT(scnTableSet(&second, "noblitz", "1"));
    UT_ASSERT(scnTableSet(&second, "cfg", "ORDER_NEARBY_TILES=3"));

    L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);

    /* A brain with no table of its own at all: the global is still written, and
       the call answers false because nothing was told. */
    brainCoreSetInitTable(L, &first);
    why[0] = '\0';
    UT_ASSERT_MSG(!brainCoreUpdateInitTable(L, &second, why, sizeof(why)),
                  "a brain with no on_init cannot have run one");
    UT_ASSERT_MSG(why[0] == '\0', "nothing raised, but why says '%s'", why);
    UT_ASSERT(luaL_dostring(L,
                  "local t = rawget(_G, 'BRAIN_INIT')\n"
                  "SEEN = tostring(t.noblitz) .. ',' .. tostring(t.role)\n") == 0);
    biGlobalText(L, "SEEN", seen, sizeof(seen));
    UT_ASSERT_MSG(strcmp(seen, "1,nil") == 0,
                  "the global reads '%s', expected the new table whole",
                  seen);

    /* Now a brain that has written one. */
    UT_ASSERT(luaL_dostring(L, BI_FIXTURE_WITH_ON_INIT) == 0);
    UT_ASSERT_MSG(brainCoreUpdateInitTable(L, &first, why, sizeof(why)),
                  "on_init did not run: %s", why);
    biGlobalText(L, "ON_INIT_SEEN", seen, sizeof(seen));
    UT_ASSERT_MSG(strcmp(seen, "role=scout") == 0,
                  "on_init saw '%s'", seen);
    lua_getglobal(L, "ON_INIT_SAME");
    UT_ASSERT_MSG(lua_toboolean(L, -1),
                  "on_init's argument is not the BRAIN_INIT global");
    lua_pop(L, 1);
    biGlobalText(L, "ON_INIT_CALLS", seen, sizeof(seen));
    UT_ASSERT_MSG(strcmp(seen, "1") == 0, "on_init ran %s times", seen);

    /* Twice in a row is twice: the call is not consumed. */
    UT_ASSERT(brainCoreUpdateInitTable(L, &second, why, sizeof(why)));
    biGlobalText(L, "ON_INIT_CALLS", seen, sizeof(seen));
    UT_ASSERT_MSG(strcmp(seen, "2") == 0, "on_init ran %s times", seen);
    biGlobalText(L, "ON_INIT_SEEN", seen, sizeof(seen));
    UT_ASSERT_MSG(strcmp(seen, "cfg=ORDER_NEARBY_TILES=3;noblitz=1") == 0,
                  "on_init saw '%s'", seen);

    /* An on_init that raises is reported and survived, and the global it
       was called about is written all the same. */
    UT_ASSERT(luaL_dostring(L,
                  "function brain.on_init(t) error('no thanks') end\n") == 0);
    why[0] = '\0';
    UT_ASSERT(!brainCoreUpdateInitTable(L, &first, why, sizeof(why)));
    UT_ASSERT_MSG(strstr(why, "no thanks") != NULL,
                  "the error was not reported: '%s'", why);
    UT_ASSERT(luaL_dostring(L,
                  "SEEN = tostring(rawget(_G, 'BRAIN_INIT').role)\n") == 0);
    biGlobalText(L, "SEEN", seen, sizeof(seen));
    UT_ASSERT_MSG(strcmp(seen, "scout") == 0,
                  "the global reads '%s' after a raising on_init", seen);

    /* And the VM is left as it was found: the stack the call borrowed is
       given back, error or not. */
    UT_ASSERT_MSG(lua_gettop(L) == 0, "the stack holds %d values",
                  lua_gettop(L));

    lua_close(L);
    return 0;
}
