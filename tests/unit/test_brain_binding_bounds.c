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
