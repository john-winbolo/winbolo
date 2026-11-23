/*
 * The brain's C functions, given values no working brain sends.
 *
 * A brain is Lua the host may not trust, and a scenario script can set many of
 * its numbers through cfg= tokens, so what these functions take from Lua must
 * not reach memory outside their arrays. The cases that index past an array do
 * not always fail in a plain build; the ASan build is what shows them.
 *
 * run_brain_bounds_tail_bad_steps      - a negative or huge water step acts like
 *                                        the nearest step that works
 * run_brain_bounds_tail_bad_neutral    - the same for the neutral zone step
 * run_brain_bounds_deserialize_open    - a blob's open_count never reaches the
 *                                        heap, which is not in a blob
 * run_brain_bounds_next_step_bad_start - a start off the map is no step
 * run_brain_bounds_shells_at_bad_tile  - a tile off the map is nil
 * run_brain_bounds_push_replaced_info  - the info tables a brain replaced are
 *                                        tables again after the next push
 * run_brain_bounds_stamps_bad_tables   - gh_attack.init_stamps takes non-tables
 *                                        where it wants tables
 * run_brain_bounds_threat_range        - a PILL_RANGE_MAP out of range acts like
 *                                        the nearest range the arrays hold
 * run_brain_bounds_think_errors        - a metamethod that raises in the host's
 *                                        own reads and writes is a failed think,
 *                                        not the end of the program
 * run_brain_bounds_wrapper_called_from_lua - the host wrappers refuse a call from
 *                                        Lua with anything but their own argument
 * run_brain_bounds_kill_outside_think  - a kill outside think is still a kill
 * run_brain_bounds_host_globals_raw    - host global access runs no metatable
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "test_harness.h"
#include "brain.h"
#include "braincore.h"
#include "brain_pathfinder.h"

void naThreatRegister(lua_State *L);
void naAttackRegister(lua_State *L);

#define BB_MAPSZ 256
#define BB_TT_BUILDING 0
#define BB_TT_RIVER 1
#define BB_TT_GRASS 7

static BYTE bbMap[BB_MAPSZ * BB_MAPSZ];

static BrainPathfinder *bbFreshPathfinder(void) {
    BrainPathfinder *pf = brainPathfinderCreate();
    if (pf == NULL) return NULL;
    memset(bbMap, BB_TT_GRASS, sizeof(bbMap));
    brainPathfinderSetMap(pf, bbMap);
    brainPathfinderClearInfluence(pf);
    brainPathfinderClearNeutralZones(pf);
    return pf;
}

/* A VM as luaBrainInstanceCreate starts one: the bindings read a BotContext
 * pointer out of the extra space, which Lua does not zero. */
static lua_State *bbNewState(void) {
    lua_State *L = luaL_newstate();
    if (L == NULL) return NULL;
    *(void **)lua_getextraspace(L) = NULL;
    luaL_openlibs(L);
    return L;
}

/* A core at (100,100) on grass with a river beside it. */
static void bbSeedRiverCore(BrainPathfinder *pf) {
    int y;
    for (y = 0; y < BB_MAPSZ; y++) {
        bbMap[y * BB_MAPSZ + 102] = BB_TT_RIVER;
    }
    pf->influence_grid[100 * BB_MAPSZ + 100] = 40;
}

int run_brain_bounds_tail_bad_steps(void) {
    BrainPathfinder *pf = bbFreshPathfinder();
    int16_t *zero, *one;
    static const int low[] = { -1, -500, INT_MIN };
    static const int high[] = { 65, 5000, INT_MAX };
    size_t i;

    UT_ASSERT(pf != NULL);
    bbSeedRiverCore(pf);
    zero = (int16_t *)malloc(sizeof(pf->expand_grid));
    one = (int16_t *)malloc(sizeof(pf->expand_grid));
    UT_ASSERT(zero != NULL && one != NULL);

    brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, 3, 0, 0, 1);
    memcpy(zero, pf->expand_grid, sizeof(pf->expand_grid));
    for (i = 0; i < sizeof(low) / sizeof(low[0]); i++) {
        brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, 3, low[i], 0, 1);
        UT_ASSERT_MSG(memcmp(zero, pf->expand_grid, sizeof(pf->expand_grid)) == 0,
                      "water step %d did not act like step 0", low[i]);
    }

    /* A step past the longest reach crosses nothing, however large. */
    brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, 3, 64, 0, 1);
    memcpy(one, pf->expand_grid, sizeof(pf->expand_grid));
    for (i = 0; i < sizeof(high) / sizeof(high[0]); i++) {
        brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, 3, high[i], 0, 1);
        UT_ASSERT_MSG(memcmp(one, pf->expand_grid, sizeof(pf->expand_grid)) == 0,
                      "water step %d did not act like a step past the reach",
                      high[i]);
    }
    free(zero);
    free(one);
    brainPathfinderDestroy(pf);
    return 0;
}

int run_brain_bounds_tail_bad_neutral(void) {
    BrainPathfinder *pf = bbFreshPathfinder();
    int16_t *zero, *past, *working;
    static const int low[] = { -1, -500, INT_MIN };
    static const int high[] = { 65, 5000, INT_MAX };
    size_t i;

    UT_ASSERT(pf != NULL);
    pf->influence_grid[100 * BB_MAPSZ + 100] = 40;
    /* A live neutral pill beside the core, so this step has ground to act on. */
    brainPathfinderStampNeutralZone(pf, 104, 100, 8);
    zero = (int16_t *)malloc(sizeof(pf->expand_grid));
    past = (int16_t *)malloc(sizeof(pf->expand_grid));
    working = (int16_t *)malloc(sizeof(pf->expand_grid));
    UT_ASSERT(zero != NULL && past != NULL && working != NULL);

    brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, 0, 2, 0, 1);
    memcpy(zero, pf->expand_grid, sizeof(pf->expand_grid));
    brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, 3, 2, 0, 1);
    memcpy(working, pf->expand_grid, sizeof(pf->expand_grid));
    brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, 64, 2, 0, 1);
    memcpy(past, pf->expand_grid, sizeof(pf->expand_grid));
    /* If the zone did nothing, the comparisons below would prove nothing. */
    UT_ASSERT_MSG(memcmp(zero, working, sizeof(pf->expand_grid)) != 0,
                  "neutral step 3 acted like step 0, so the zone has no effect");
    UT_ASSERT_MSG(memcmp(past, working, sizeof(pf->expand_grid)) != 0,
                  "neutral step 64 acted like step 3, so the zone has no effect");

    for (i = 0; i < sizeof(low) / sizeof(low[0]); i++) {
        brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, low[i], 2, 0, 1);
        UT_ASSERT_MSG(memcmp(zero, pf->expand_grid, sizeof(pf->expand_grid)) == 0,
                      "neutral step %d did not act like step 0", low[i]);
    }
    for (i = 0; i < sizeof(high) / sizeof(high[0]); i++) {
        brainPathfinderRebuildInfluenceTail(pf, 20, 10, 15, high[i], 2, 0, 1);
        UT_ASSERT_MSG(memcmp(past, pf->expand_grid, sizeof(pf->expand_grid)) == 0,
                      "neutral step %d did not act like a step past the reach",
                      high[i]);
    }
    free(zero);
    free(past);
    free(working);
    brainPathfinderDestroy(pf);
    return 0;
}

int run_brain_bounds_deserialize_open(void) {
    BrainPathfinder *pf = bbFreshPathfinder();
    BrainPathfinder *back = bbFreshPathfinder();
    unsigned char *blob;
    size_t size = 0;
    int ok;

    UT_ASSERT(pf != NULL && back != NULL);
    /* A slate with no data carries its header alone, as an edited blob can. */
    pf->dij_slates[1].open_count = 123456;
    blob = brainPathfinderSerialize(pf, &size);
    UT_ASSERT(blob != NULL);
    ok = brainPathfinderDeserialize(back, blob, size);
    free(blob);
    UT_ASSERT_MSG(ok == 1, "the blob did not load");
    UT_ASSERT_MSG(back->dij_slates[1].open_count == 0,
                  "open_count from the blob is %d, not 0",
                  back->dij_slates[1].open_count);
    pf->dij_slates[1].open_count = 0;
    brainPathfinderDestroy(pf);
    brainPathfinderDestroy(back);
    return 0;
}

int run_brain_bounds_next_step_bad_start(void) {
    BrainPathfinder *pf = bbFreshPathfinder();
    int x = 0, y = 0;

    UT_ASSERT(pf != NULL);
    /* A finished slate, so the search goes on to read the start's cost. */
    brainPathfinderDijkstraStart(pf, 0, 1, 10, 10, 0, 40, 40, 40, 40,
                                 100.0f, 0, 0.0f, 0, 0);
    while (!brainPathfinderDijkstraStep(pf, 0, 1, 100000)) {
    }
    UT_ASSERT_MSG(brainPathfinderDijkstraNextStep(pf, 0, -1000000, 10, 20, 20,
                                                  NULL, 0, 0.0f, 0, &x, &y) == 0,
                  "a start west of the map gave a step");
    UT_ASSERT_MSG(brainPathfinderDijkstraNextStep(pf, 0, 10, 1000000, 20, 20,
                                                  NULL, 0, 0.0f, 0, &x, &y) == 0,
                  "a start south of the map gave a step");
    UT_ASSERT_MSG(brainPathfinderDijkstraNextStep(pf, 0, 256, 10, 20, 20,
                                                  NULL, 0, 0.0f, 0, &x, &y) == 0,
                  "a start one past the map gave a step");
    brainPathfinderDestroy(pf);
    return 0;
}

int run_brain_bounds_shells_at_bad_tile(void) {
    BrainPathfinder *pf = bbFreshPathfinder();
    lua_State *L = bbNewState();

    UT_ASSERT(pf != NULL && L != NULL);
    brainCoreRegisterPathfinder(L, &pf);
    UT_ASSERT_MSG(luaL_dostring(L,
                  "return cpf_astar_shells_at(1000000, 1000000),"
                  "       cpf_astar_shells_at(-1, 5),"
                  "       cpf_astar_shells_at(5, 256),"
                  "       cpf_astar_shells_at(5, 5)") == 0,
                  "the calls raised: %s", lua_tostring(L, -1));
    UT_ASSERT_MSG(lua_isnil(L, -4), "a tile far off the map was not nil");
    UT_ASSERT_MSG(lua_isnil(L, -3), "a tile west of the map was not nil");
    UT_ASSERT_MSG(lua_isnil(L, -2), "a tile south of the map was not nil");
    UT_ASSERT_MSG(lua_isnumber(L, -1), "a tile on the map was not a number");
    lua_close(L);
    brainPathfinderDestroy(pf);
    return 0;
}

int run_brain_bounds_push_replaced_info(void) {
    static BrainInfo info;
    static ObjectInfo objects[1];
    static GameEvent events[1];
    static const char *const names[] = {
        "player_names", "gameinfo", "objects", "messages", "events", "rules",
        "mods"
    };
    lua_State *L = bbNewState();
    size_t i;

    UT_ASSERT(L != NULL);
    memset(&info, 0, sizeof(info));
    memset(objects, 0, sizeof(objects));
    memset(events, 0, sizeof(events));
    info.objects = objects;
    info.num_objects = 1;
    info.events = events;
    info.num_events = 1;

    brainCorePushInfo(L, &info);
    /* Each table replaced with a string, which raw writes would not refuse. */
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        lua_pushstring(L, "not a table");
        lua_setfield(L, -2, names[i]);
    }
    lua_pop(L, 1);

    /* The rules are written only when they change, so change them. */
    info.rules.pill_range = 2048;
    brainCorePushInfo(L, &info);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        lua_getfield(L, -1, names[i]);
        UT_ASSERT_MSG(lua_istable(L, -1), "info.%s is not a table after a push",
                      names[i]);
        lua_pop(L, 1);
    }

    /* An event's data array, one level further in. */
    lua_getfield(L, -1, "events");
    lua_rawgeti(L, -1, 1);
    lua_pushinteger(L, 5);
    lua_setfield(L, -2, "data");
    lua_pop(L, 3);
    brainCorePushInfo(L, &info);
    lua_getfield(L, -1, "events");
    lua_rawgeti(L, -1, 1);
    lua_getfield(L, -1, "data");
    UT_ASSERT_MSG(lua_istable(L, -1), "an event's data is not a table after a push");
    lua_close(L);
    return 0;
}

int run_brain_bounds_stamps_bad_tables(void) {
    lua_State *L = bbNewState();

    UT_ASSERT(L != NULL);
    naAttackRegister(L);
    UT_ASSERT_MSG(luaL_dostring(L,
                  "gh_attack.init_stamps({ [0] = { aims = 5 }, [5] = 'x' },"
                  "                      { [0] = 'not a table', [5] = 7 })\n"
                  "gh_attack.init_stamps({ [0] = { aims = 'not a table' } }, {})\n"
                  "gh_attack.init_stamps({ [0] = {} }, {})") == 0,
                  "the calls raised: %s", lua_tostring(L, -1));
    lua_close(L);
    return 0;
}

int run_brain_bounds_threat_range(void) {
    lua_State *L = bbNewState();
    static const BYTE *world = bbMap;
    int x;

    UT_ASSERT(L != NULL);
    memset(bbMap, BB_TT_GRASS, sizeof(bbMap));
    /* A wall beside the pill, so that occlusion changes the grid. */
    for (x = 95; x <= 105; x++) {
        bbMap[103 * BB_MAPSZ + x] = BB_TT_BUILDING;
    }
    /* gh_threat reads the map through this, as a real brain's does. */
    brainCoreRegisterGetTerrain(L, &world);
    naThreatRegister(L);
    /* An out-of-range PILL_RANGE_MAP must give what the nearest range in the
     * arrays gives. Unclamped, the occlusion sweep decodes its slots with the
     * wrong size and reads outside the arrays, which changes the grid. */
    UT_ASSERT_MSG(luaL_dostring(L,
                  "local function grid(range)\n"
                  "  gh_threat.configure({ PILL_RANGE_MAP = range, T_BUILDING = 0 })\n"
                  "  gh_threat.terrain_rebuild()\n"
                  "  gh_threat.pill_rebuild_begin()\n"
                  "  gh_threat.stamp_pill(100, 100, 1, 8)\n"
                  "  gh_threat.apply_occlusion_all({}, {}, {}, 0, { 100 }, { 100 }, 1)\n"
                  "  local out = {}\n"
                  "  for y = 80, 120 do for x = 80, 120 do\n"
                  "    out[#out + 1] = gh_threat.pill_grid_at(y * 256 + x)\n"
                  "  end end\n"
                  "  return out\n"
                  "end\n"
                  "local function same(a, b)\n"
                  "  for i = 1, #a do if a[i] ~= b[i] then return false end end\n"
                  "  return true\n"
                  "end\n"
                  "local g = grid(15)\n"
                  "local any = false\n"
                  "for i = 1, #g do if g[i] > 0 then any = true end end\n"
                  "return any, same(g, grid(100000)), same(grid(0), grid(-100000))") == 0,
                  "the calls raised: %s", lua_tostring(L, -1));
    UT_ASSERT_MSG(lua_toboolean(L, -3), "the pill stamped nothing to compare");
    UT_ASSERT_MSG(lua_toboolean(L, -2), "a huge range did not act like range 15");
    UT_ASSERT_MSG(lua_toboolean(L, -1), "a negative range did not act like range 0");
    lua_close(L);
    return 0;
}

/* A fresh VM for each case: a table one case poisons would otherwise fail the
 * next case's push, and that case would never reach what it is there to test. */
static lua_State *bbStateWith(const char *chunk) {
    lua_State *L = bbNewState();
    if (L != NULL && luaL_dostring(L, chunk) != 0) {
        lua_close(L);
        return NULL;
    }
    return L;
}

static bool bbThink(lua_State *L, BrainInfo *info) {
    bool killed = false;
    return brainCoreCallThink(L, info, &killed);
}

/* Whether the global a chunk's metamethod sets was set, read past any metatable
 * on _G. Tells a call that failed because of the raise from one that fails
 * anyway, since both answer false. */
static bool bbFlagSet(lua_State *L, const char *name) {
    bool set;
    brainCoreGetGlobalRaw(L, name);
    set = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return set;
}

int run_brain_bounds_think_errors(void) {
    static BrainInfo info;
    lua_State *L;
    int top;

    memset(&info, 0, sizeof(info));

    /* The info table the host writes through on the next push. */
    L = bbStateWith(
        "brain = { think = function(info)\n"
        "  info.mods = setmetatable({}, { __newindex = function()\n"
        "    raised = true; error('mods') end })\n"
        "  return {}\n"
        "end }\n");
    UT_ASSERT(L != NULL);
    UT_ASSERT_MSG(bbThink(L, &info), "the first think failed");
    top = lua_gettop(L);
    UT_ASSERT_MSG(!bbThink(L, &info), "a push into a raising table succeeded");
    UT_ASSERT_MSG(bbFlagSet(L, "raised"), "the push did not reach the metamethod");
    UT_ASSERT_MSG(lua_gettop(L) == top, "a failed think left the stack changed");
    lua_close(L);

    /* The table the think returned, which the host reads. */
    L = bbStateWith(
        "brain = { think = function(info)\n"
        "  return setmetatable({}, { __index = function()\n"
        "    raised = true; error('output') end })\n"
        "end }\n");
    UT_ASSERT(L != NULL);
    UT_ASSERT_MSG(!bbThink(L, &info), "reading a raising output table succeeded");
    UT_ASSERT_MSG(bbFlagSet(L, "raised"), "the read did not reach the metamethod");
    lua_close(L);

    /* The lookup of a method on the brain's own table. */
    L = bbStateWith(
        "brain = setmetatable({}, { __index = function()\n"
        "  raised = true; error('brain') end })\n");
    UT_ASSERT(L != NULL);
    UT_ASSERT_MSG(!bbThink(L, &info), "a raising brain table gave a think");
    UT_ASSERT_MSG(bbFlagSet(L, "raised"), "the lookup did not reach the metamethod");
    UT_ASSERT_MSG(!brainCoreCallMethod(L, &info, "open"),
                  "a raising brain table gave a method");
    lua_close(L);
    return 0;
}

/* The wrappers are on the stack below think. Without the sandbox, getinfo gives
 * a brain the deepest one, and it calls it with what would be read as the host's
 * arguments. */
int run_brain_bounds_wrapper_called_from_lua(void) {
    static BrainInfo info;
    lua_State *L = bbStateWith(
        "local function deepest()\n"
        "  local f, i = nil, 1\n"
        "  while true do\n"
        "    local ar = debug.getinfo(i, 'f')\n"
        "    if not ar then return f end\n"
        "    f = ar.func; i = i + 1\n"
        "  end\n"
        "end\n"
        "local function poke(flag)\n"
        "  local f = deepest()\n"
        "  if type(f) == 'function' and debug.getinfo(f, 'S').what == 'C' then\n"
        "    rawset(_G, flag, true)\n"
        "  end\n"
        "  pcall(f)\n"
        "  pcall(f, 5)\n"
        "  pcall(f, io.stdout)\n"
        "end\n"
        "brain = {\n"
        "  think = function() poke('found_think'); return {} end,\n"
        "  open = function() poke('found_open') end,\n"
        "}\n");

    UT_ASSERT(L != NULL);
    memset(&info, 0, sizeof(info));
    UT_ASSERT_MSG(bbThink(L, &info), "the think failed");
    UT_ASSERT_MSG(bbFlagSet(L, "found_think"), "think did not find a wrapper");
    UT_ASSERT_MSG(brainCoreCallMethod(L, &info, "open"), "the open failed");
    UT_ASSERT_MSG(bbFlagSet(L, "found_open"), "open did not find a wrapper");
    lua_close(L);
    return 0;
}

static void bbNoHook(lua_State *L, lua_Debug *ar) {
    (void)L;
    (void)ar;
}

/* A budget kill raised outside the brain's own pcall is still a kill, its log
 * lines are still flushed, and the armed budget hook is taken off before they
 * are. */
int run_brain_bounds_kill_outside_think(void) {
    static BrainInfo info;
    bool killed = false;
    lua_State *L = bbStateWith(
        "function brain_flush_killed() flushed = true end\n"
        "brain = { think = function()\n"
        "  return setmetatable({}, { __index = function()\n"
        "    error('tick_budget_exceeded') end })\n"
        "end }\n");

    UT_ASSERT(L != NULL);
    memset(&info, 0, sizeof(info));
    lua_sethook(L, bbNoHook, LUA_MASKCOUNT, 1000000);
    UT_ASSERT_MSG(!brainCoreCallThink(L, &info, &killed), "the think succeeded");
    UT_ASSERT_MSG(killed, "the kill was not reported");
    UT_ASSERT_MSG(bbFlagSet(L, "flushed"), "the killed tick's logs were not flushed");
    UT_ASSERT_MSG(lua_gethook(L) == NULL, "the budget hook is still armed");
    lua_close(L);
    return 0;
}

/* Host code that reads and writes globals outside any pcall, with a _G whose
 * metatable raises on everything. Reaching the end at all is most of the test. */
int run_brain_bounds_host_globals_raw(void) {
    static BrainInfo info;
    ScnTable table;
    char why[128];
    lua_State *L = bbStateWith(
        "brain = nil\n"
        "local function hit() rawset(_G, 'raised', true); error('global') end\n"
        "setmetatable(_G, { __index = hit, __newindex = hit })\n");

    UT_ASSERT(L != NULL);
    memset(&info, 0, sizeof(info));
    memset(&table, 0, sizeof(table));
    UT_ASSERT_MSG(!bbThink(L, &info), "a think with no brain succeeded");
    UT_ASSERT_MSG(!brainCoreCallMethod(L, &info, "open"), "a method with no brain succeeded");
    brainCoreSetInitTable(L, &table);
    UT_ASSERT_MSG(!brainCoreUpdateInitTable(L, &table, why, sizeof(why)),
                  "an init update with no brain succeeded");
    UT_ASSERT_MSG(brainCoreCallScenarioHint(L, &table, why, sizeof(why)) ==
                  BRAIN_HINT_NO_HANDLER, "a hint with no handler was delivered");
    UT_ASSERT_MSG(!bbFlagSet(L, "raised"), "host code ran the metatable on _G");
    lua_close(L);
    return 0;
}

/* on_init through a metatable: one a brain inherits is still found and called,
 * and an __index that raises only fails the update. */
int run_brain_bounds_on_init_metatable(void) {
    ScnTable table;
    char why[128];
    lua_State *L = bbStateWith(
        "brain = setmetatable({}, { __index = {\n"
        "  on_init = function() called = true end } })\n");

    UT_ASSERT(L != NULL);
    memset(&table, 0, sizeof(table));
    UT_ASSERT_MSG(brainCoreUpdateInitTable(L, &table, why, sizeof(why)),
                  "an inherited on_init was not called");
    UT_ASSERT_MSG(bbFlagSet(L, "called"), "an inherited on_init did not run");
    lua_close(L);

    L = bbStateWith(
        "brain = setmetatable({}, { __index = function()\n"
        "  raised = true; error('on_init lookup') end })\n");
    UT_ASSERT(L != NULL);
    UT_ASSERT_MSG(!brainCoreUpdateInitTable(L, &table, why, sizeof(why)),
                  "an on_init lookup that raised succeeded");
    UT_ASSERT_MSG(bbFlagSet(L, "raised"), "the lookup did not reach the metamethod");
    UT_ASSERT_MSG(strstr(why, "on_init lookup") != NULL,
                  "the reason was not passed on: %s", why);
    lua_close(L);
    return 0;
}
