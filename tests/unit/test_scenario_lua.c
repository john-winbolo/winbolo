/*
 * The binding table: the game table a script reads the sim through.
 *
 * Two ways in, because the rows answer two kinds of question. Most cases
 * build a Lua state of their own, install the table on it with a context of
 * their own and run a chunk, which is what lets a case compare what a row
 * marshalled against what the accessor behind it returned. The cases that
 * are about the host — the table on the round's own VM, and what the error
 * limit makes of a raise inside a row — attach a real sidecar.
 *
 * run_scenario_lua_every_row_answers   — a script calls every row in the
 *                                        registry once and none of them
 *                                        raises or is missing
 * run_scenario_lua_read_index_passes_through
 *                                      — the Lua index of a read is the
 *                                        accessor's index, for every pill
 *                                        on the map
 * run_scenario_lua_op_index_subtracts_one
 *                                      — and an op payload's is one less
 * run_scenario_lua_script_index_adds_one
 *                                      — and an index arriving from an
 *                                        event is one more
 * run_scenario_lua_absent_reads_are_nil— an index outside the list, an
 *                                        empty seat and a removed pill all
 *                                        answer nil, and the pills above
 *                                        the removed one keep their numbers
 * run_scenario_lua_terrain_is_the_whole_map
 *                                      — 65,536 bytes, the square at
 *                                        (x, y) at byte y * 256 + x + 1
 * run_scenario_lua_shape_error_counts  — a mistyped argument raises naming
 *                                        the argument, and each raise
 *                                        counts one toward SCN_ERROR_LIMIT
 * run_scenario_lua_rule_reads_the_table— game.rule answers what the sidecar
 *                                        set, and a name that spells no
 *                                        rule raises
 * run_scenario_lua_tags_and_regions    — the tags and regions a sidecar
 *                                        declares read back, a square
 *                                        inside a region and one outside
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* the policy vtable and its depth bracket */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"   /* the funnel, for the removal case */
#include "everard_map.h"
#include "scenario_host.h"
#include "scenario_manifest.h"
#include "scenario_lua.h"
#include "test_harness.h"

/* ── A state with the table on it ─────────────────────────────────── */

/* The context outlives the state it is installed on: both are the caller's
 * locals and the state is closed first. */
static lua_State *slVm(ScnLuaCtx *ctx, ServerSim *sim,
                       const ScenarioManifest *m) {
    lua_State *L = luaL_newstate();

    if (L == NULL) {
        return NULL;
    }
    luaL_openlibs(L);
    ctx->sim      = sim;
    ctx->manifest = m;
    scenarioLuaInstall(L, ctx);
    return L;
}

/* Run a chunk and leave nothing on the stack. The error, when there is one,
 * is the whole message Lua produced. */
static bool slRun(lua_State *L, const char *src, char *err, size_t errLen) {
    err[0] = '\0';
    if (luaL_loadstring(L, src) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        const char *msg = lua_tostring(L, -1);
        snprintf(err, errLen, "%s", (msg != NULL) ? msg : "unknown error");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

/* A global the chunk left, as a number. SL_NONE for one that is absent or
 * is not a number, which is what a case asserting a number checks against. */
#define SL_NONE (-1L)

static long slGlobalInt(lua_State *L, const char *name) {
    long v = SL_NONE;

    lua_getglobal(L, name);
    if (lua_isnumber(L, -1)) {
        v = (long)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

static bool slGlobalIsNil(lua_State *L, const char *name) {
    bool nil;

    lua_getglobal(L, name);
    nil = lua_isnil(L, -1);
    lua_pop(L, 1);
    return nil;
}

/* One entry of a global array, as a number. */
static long slArrayInt(lua_State *L, const char *name, int n) {
    long v = SL_NONE;

    lua_getglobal(L, name);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, n);
        if (lua_isnumber(L, -1)) {
            v = (long)lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return v;
}

/* ── A sim to read ────────────────────────────────────────────────── */

/* A sim that has not started, for the cases that attach a host and start a
 * round themselves. ut_make_running_sim is the one for the rest. */
static ServerSim *slLobbySim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    return sim;
}

/* ── Sidecars, for the host-driven cases ──────────────────────────── */

static void slSidecarFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SIDECAR_SUFFIX);
}

static bool slPut(const char *mapPath, const char *lua) {
    char  side[512];
    FILE *f;

    slSidecarFor(mapPath, side, sizeof(side));
    f = fopen(side, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void slDrop(const char *mapPath) {
    char side[512];
    slSidecarFor(mapPath, side, sizeof(side));
    remove(side);
}

/* A hook says what it read by writing a line to this case's own file. The
 * names are per-case on purpose: CTest runs cases as separate processes in
 * one directory, so a shared name is a race rather than a fixture. */
static void slScript(char *out, size_t outLen, const char *record,
                     const char *body) {
    snprintf(out, outLen,
             "local function note(s)\n"
             "  local f = io.open(\"%s\", \"a\")\n"
             "  if f then f:write(s) f:close() end\n"
             "end\n"
             "%s", record, body);
}

static void slRead(const char *record, char *out, size_t outLen) {
    FILE  *f = fopen(record, "rb");
    size_t n;

    out[0] = '\0';
    if (f == NULL) {
        return;
    }
    n = fread(out, 1, outLen - 1, f);
    fclose(f);
    out[n] = '\0';
}

/* The one question the sim asks, asked the way the lobby asks it: through
 * the registered vtable with the policy depth held across the call. */
static bool slAskExtraTeams(ServerSim *sim) {
    bool allow;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->allowExtraTeams == NULL) {
        return true;
    }
    serverSimScenarioPolicyEnter(sim);
    allow = sim->scenarioPolicy->allowExtraTeams(sim->scenarioPolicy->ctx);
    serverSimScenarioPolicyLeave(sim);
    return allow;
}

/* ── 1. Every row answers ─────────────────────────────────────────── */

/* One call per row, by the row's own name, so a row that is in the registry
 * and cannot marshal is caught here whether or not a case below aims at it.
 * The keys are the registry's names: a row added without a line here reads
 * back as nothing at all and fails. */
static const char *const kSlEveryRow =
    "local calls = {\n"
    "  tick        = function() return game.tick() end,\n"
    "  max_tanks   = function() return game.max_tanks() end,\n"
    "  num_players = function() return game.num_players() end,\n"
    "  num_humans  = function() return game.num_humans() end,\n"
    "  team_size   = function() return game.team_size(1) end,\n"
    "  game_type   = function() return game.game_type() end,\n"
    "  map_name    = function() return game.map_name() end,\n"
    "  map_tile    = function() return game.map_tile(10, 10) end,\n"
    "  is_mine     = function() return game.is_mine(10, 10) end,\n"
    "  terrain     = function() return game.terrain() end,\n"
    "  num_pills   = function() return game.num_pills() end,\n"
    "  pill        = function() return game.pill(1) end,\n"
    "  num_bases   = function() return game.num_bases() end,\n"
    "  base        = function() return game.base(1) end,\n"
    "  num_starts  = function() return game.num_starts() end,\n"
    "  start       = function() return game.start(1) end,\n"
    "  tank        = function() return game.tank(0) end,\n"
    "  builder     = function() return game.builder(0) end,\n"
    "  lobby_slot  = function() return game.lobby_slot(0) end,\n"
    "  rule        = function() return game.rule(\"tank_reload_ticks\") end,\n"
    "  tags        = function() return game.tags(\"pill\", 1) end,\n"
    "  tagged      = function() return game.tagged(\"keep\") end,\n"
    "  region      = function() return game.region(\"keep\") end,\n"
    "  regions     = function() return game.regions() end,\n"
    "  in_region   = function() return game.in_region(\"keep\", 10, 10) end,\n"
    "}\n"
    "results = {}\n"
    "for name, f in pairs(calls) do\n"
    "  local ok, v = pcall(f)\n"
    "  if ok then results[name] = type(v)\n"
    "  else results[name] = \"!\" .. tostring(v) end\n"
    "end\n"
    "constants = tostring(game.api_version) .. \",\" ..\n"
    "            tostring(game.NEUTRAL) .. \",\" ..\n"
    "            tostring(game.TERRAIN.road) .. \",\" ..\n"
    "            tostring(game.TERRAIN.deep_sea)\n";

/* What a handful of the rows answer with, so the case says more than that
 * each of them returned. */
static const struct {
    const char *row;
    const char *type;
} kSlRowTypes[] = {
    { "tick",     "number" },
    { "map_name", "string" },
    { "terrain",  "string" },
    { "is_mine",  "boolean" },
    { "pill",     "table"  },
    { "regions",  "table"  },
};

int run_scenario_lua_every_row_answers(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    const ScnLuaRow *rows;
    size_t           count = 0;
    size_t           i;
    char             err[512];
    char             want[64];

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L, kSlEveryRow, err, sizeof(err)),
                  "the fixture would not run: %s", err);

    rows = scenarioLuaRows(&count);
    UT_ASSERT_MSG(count > 0, "the registry is empty");
    for (i = 0; i < count; i++) {
        char got[256];

        UT_ASSERT_MSG(rows[i].name != NULL && rows[i].name[0] != '\0',
                      "row %u has no name", (unsigned)i);
        UT_ASSERT_MSG(rows[i].fn != NULL, "row '%s' has no function",
                      rows[i].name);
        /* A row nothing describes is a row no document can be written
           from. */
        UT_ASSERT_MSG(rows[i].doc != NULL && rows[i].doc[0] != '\0',
                      "row '%s' carries no description", rows[i].name);

        got[0] = '\0';
        lua_getglobal(L, "results");
        lua_getfield(L, -1, rows[i].name);
        if (lua_type(L, -1) == LUA_TSTRING) {
            snprintf(got, sizeof(got), "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 2);

        UT_ASSERT_MSG(got[0] != '\0',
                      "the fixture never calls game.%s, so nothing here "
                      "exercises it", rows[i].name);
        UT_ASSERT_MSG(got[0] != '!', "game.%s raised: %s", rows[i].name,
                      got + 1);
    }

    for (i = 0; i < sizeof(kSlRowTypes) / sizeof(kSlRowTypes[0]); i++) {
        char got[256];
        got[0] = '\0';
        lua_getglobal(L, "results");
        lua_getfield(L, -1, kSlRowTypes[i].row);
        if (lua_type(L, -1) == LUA_TSTRING) {
            snprintf(got, sizeof(got), "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 2);
        UT_ASSERT_MSG(strcmp(got, kSlRowTypes[i].type) == 0,
                      "game.%s answered a %s, expected a %s",
                      kSlRowTypes[i].row, got, kSlRowTypes[i].type);
    }

    /* The numbers beside the calls, and the terrain table. */
    {
        char got[128];
        got[0] = '\0';
        lua_getglobal(L, "constants");
        if (lua_type(L, -1) == LUA_TSTRING) {
            snprintf(got, sizeof(got), "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        snprintf(want, sizeof(want), "%d,%d,%d,%d", SCENARIO_API_VERSION,
                 NEUTRAL, ROAD, DEEP_SEA);
        UT_ASSERT_MSG(strcmp(got, want) == 0,
                      "the constants read '%s', expected '%s'", got, want);
    }

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 2. A read index passes straight through ──────────────────────── */

/* The helper on its own, and then every pill on the map through it: the Lua
 * index and the accessor's are the same number, which a case that only
 * asked whether the call happened would not tell from an index one either
 * side of it. */
int run_scenario_lua_read_index_passes_through(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    BYTE             count;
    int              n;

    UT_ASSERT(scenarioLuaIndexToRead(1) == 1);
    UT_ASSERT(scenarioLuaIndexToRead(2) == 2);
    UT_ASSERT(scenarioLuaIndexToRead(16) == 16);
    UT_ASSERT(scenarioLuaIndexToRead(255) == 255);
    /* Nothing a 1-based list holds, so the index every accessor turns
       down. */
    UT_ASSERT(scenarioLuaIndexToRead(0) == 0);
    UT_ASSERT(scenarioLuaIndexToRead(-1) == 0);
    UT_ASSERT(scenarioLuaIndexToRead(256) == 0);

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    count = serverSimGetPillCount(sim);
    UT_ASSERT_MSG(count > 1, "the map has %u pills and this needs two",
                  (unsigned)count);

    UT_ASSERT_MSG(slRun(L,
                        "px = {} py = {}\n"
                        "for n = 1, game.num_pills() do\n"
                        "  local p = game.pill(n)\n"
                        "  if p then px[n] = p.x py[n] = p.y end\n"
                        "end\n"
                        "pills = game.num_pills()\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    UT_ASSERT_MSG(slGlobalInt(L, "pills") == (long)count,
                  "game.num_pills() answered %ld, the map has %u",
                  slGlobalInt(L, "pills"), (unsigned)count);

    for (n = 1; n <= (int)count; n++) {
        ServerSimPillInfo info;
        UT_ASSERT(serverSimGetPillInfo(sim, (BYTE)n, &info));
        UT_ASSERT_MSG(slArrayInt(L, "px", n) == (long)info.x &&
                      slArrayInt(L, "py", n) == (long)info.y,
                      "game.pill(%d) answered (%ld, %ld) and "
                      "serverSimGetPillInfo(%d) says (%u, %u)",
                      n, slArrayInt(L, "px", n), slArrayInt(L, "py", n), n,
                      (unsigned)info.x, (unsigned)info.y);
    }

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 3. An op index is one less ───────────────────────────────────── */

/* The ops are 0-based. No row issues one yet, so this is the helper itself:
 * the conversion, and what it does with a number no list holds. */
int run_scenario_lua_op_index_subtracts_one(void) {
    UT_ASSERT(scenarioLuaIndexToOp(1) == 0);
    UT_ASSERT(scenarioLuaIndexToOp(2) == 1);
    UT_ASSERT(scenarioLuaIndexToOp(16) == 15);
    UT_ASSERT(scenarioLuaIndexToOp(255) == 254);
    /* Past every entity list a map has, so the op is refused rather than
       naming an item that does exist. */
    UT_ASSERT(scenarioLuaIndexToOp(0) == 255);
    UT_ASSERT(scenarioLuaIndexToOp(-1) == 255);
    UT_ASSERT(scenarioLuaIndexToOp(256) == 255);
    return 0;
}

/* ── 4. An index into a script is one more ────────────────────────── */

/* Event payloads and policy questions are 0-based on the C side. */
int run_scenario_lua_script_index_adds_one(void) {
    UT_ASSERT(scenarioLuaIndexToScript(0) == 1);
    UT_ASSERT(scenarioLuaIndexToScript(1) == 2);
    UT_ASSERT(scenarioLuaIndexToScript(15) == 16);
    UT_ASSERT(scenarioLuaIndexToScript(254) == 255);
    return 0;
}

/* ── 5. What is not there reads nil ───────────────────────────────── */

int run_scenario_lua_absent_reads_are_nil(void) {
    ServerSim        *sim = ut_make_running_sim("Seat0");
    ScenarioManifest  m;
    ScnLuaCtx         ctx;
    lua_State        *L;
    ScenarioOp        op;
    ServerSimPillInfo before3;
    char              err[512];
    char              chunk[512];
    BYTE              count;

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    count = serverSimGetPillCount(sim);
    UT_ASSERT_MSG(count >= 3, "the map has %u pills and this needs three",
                  (unsigned)count);

    /* An index outside the list, on both sides of it. */
    snprintf(chunk, sizeof(chunk),
             "low = game.pill(0)\n"
             "high = game.pill(%d)\n"
             "way_high = game.pill(300)\n",
             (int)count + 1);
    UT_ASSERT_MSG(slRun(L, chunk, err, sizeof(err)),
                  "the chunk would not run: %s", err);
    UT_ASSERT_MSG(slGlobalIsNil(L, "low"), "game.pill(0) answered a table");
    UT_ASSERT_MSG(slGlobalIsNil(L, "high"),
                  "game.pill(%d) answered a table and the map has %u pills",
                  (int)count + 1, (unsigned)count);
    UT_ASSERT_MSG(slGlobalIsNil(L, "way_high"),
                  "game.pill(300) answered a table");

    /* An empty seat. Slot 0 is the one ut_make_running_sim fills. */
    UT_ASSERT_MSG(slRun(L,
                        "seat_tank = game.tank(1)\n"
                        "seat_slot = game.lobby_slot(1)\n"
                        "seat_lgm = game.builder(1)\n"
                        "past_end = game.tank(16)\n"
                        "below = game.lobby_slot(-1)\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);
    UT_ASSERT_MSG(slGlobalIsNil(L, "seat_tank"),
                  "an empty seat answered a tank");
    UT_ASSERT_MSG(slGlobalIsNil(L, "seat_slot"),
                  "an empty seat answered a lobby slot");
    UT_ASSERT_MSG(slGlobalIsNil(L, "seat_lgm"),
                  "an empty seat answered a builder");
    UT_ASSERT_MSG(slGlobalIsNil(L, "past_end"),
                  "a slot past the roster answered a tank");
    UT_ASSERT_MSG(slGlobalIsNil(L, "below"),
                  "a negative slot answered a lobby slot");
    /* And the seat that is filled still reads. */
    UT_ASSERT_MSG(slRun(L, "seat0 = game.lobby_slot(0)\n"
                           "seat0_name = seat0 and seat0.name or \"\"\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);
    {
        char got[64];
        got[0] = '\0';
        lua_getglobal(L, "seat0_name");
        if (lua_type(L, -1) == LUA_TSTRING) {
            snprintf(got, sizeof(got), "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        UT_ASSERT_MSG(strcmp(got, "Seat0") == 0,
                      "the filled seat read as '%s'", got);
    }

    /* An item that has been taken off the map. Pill 2 in the script's
       numbering is index 1 on the op, which is the conversion the funnel
       takes, and the pills above it keep their numbers. */
    UT_ASSERT(serverSimGetPillInfo(sim, 3, &before3));
    memset(&op, 0, sizeof(op));
    op.type                  = SCN_OP_ENTITY_REMOVE_PILL;
    op.u.entityRemovePill.pill = scenarioLuaIndexToOp(2);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the removal was refused");

    UT_ASSERT_MSG(slRun(L,
                        "gone = game.pill(2)\n"
                        "still1 = game.pill(1)\n"
                        "still3 = game.pill(3)\n"
                        "left = game.num_pills()\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);
    UT_ASSERT_MSG(slGlobalIsNil(L, "gone"),
                  "the removed pill still answers a table");
    UT_ASSERT_MSG(slGlobalInt(L, "left") == (long)count,
                  "the count fell to %ld from %u: a removed pill keeps its "
                  "slot so the ones above it keep their numbers",
                  slGlobalInt(L, "left"), (unsigned)count);
    {
        long x3, y3;
        lua_getglobal(L, "still3");
        UT_ASSERT_MSG(lua_istable(L, -1),
                      "pill 3 went with the removal of pill 2");
        lua_getfield(L, -1, "x");
        x3 = (long)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "y");
        y3 = (long)lua_tonumber(L, -1);
        lua_pop(L, 2);
        UT_ASSERT_MSG(x3 == (long)before3.x && y3 == (long)before3.y,
                      "pill 3 moved to (%ld, %ld) from (%u, %u)", x3, y3,
                      (unsigned)before3.x, (unsigned)before3.y);
    }
    {
        bool table1;
        lua_getglobal(L, "still1");
        table1 = lua_istable(L, -1);
        lua_pop(L, 1);
        UT_ASSERT_MSG(table1, "pill 1 went with the removal of pill 2");
    }

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 6. The whole map in one string ───────────────────────────────── */

int run_scenario_lua_terrain_is_the_whole_map(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    const char      *buf;
    size_t           len = 0;
    int              x, y;
    int              kinds = 0;
    bool             seen[256];

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L, "whole = game.terrain()\n", err, sizeof(err)),
                  "the chunk would not run: %s", err);

    lua_getglobal(L, "whole");
    UT_ASSERT_MSG(lua_type(L, -1) == LUA_TSTRING,
                  "game.terrain() answered a %s", luaL_typename(L, -1));
    buf = lua_tolstring(L, -1, &len);
    UT_ASSERT_MSG(len == (size_t)SERVER_SIM_TERRAIN_BYTES,
                  "game.terrain() is %u bytes, expected %u", (unsigned)len,
                  (unsigned)SERVER_SIM_TERRAIN_BYTES);

    /* Every square against the accessor for the same square. The whole
       buffer rather than a sample of it: an offset out by a byte or by a row
       has nowhere in 65,536 comparisons to hide, and the case depends on
       nothing about where this map happens to put its land. */
    memset(seen, 0, sizeof(seen));
    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            BYTE got  = (BYTE)buf[y * MAP_ARRAY_SIZE + x];
            BYTE want = serverSimGetMapTerrain(sim, (BYTE)x, (BYTE)y);
            UT_ASSERT_MSG(got == want,
                          "(%d, %d) reads %u in the string and %u from the "
                          "map", x, y, (unsigned)got, (unsigned)want);
            if (!seen[got]) {
                seen[got] = true;
                kinds++;
            }
        }
    }
    /* A map of one terrain everywhere — a buffer nothing filled — would pass
       the walk above whatever the offset was. */
    UT_ASSERT_MSG(kinds >= 2,
                  "the whole map reads as %d terrain, so the walk above "
                  "proves nothing about where a square sits", kinds);
    lua_pop(L, 1);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 7. A shape error raises, and counts ──────────────────────────── */

/* A mistyped argument is a shape error: it raises, it names the argument,
 * and the host counts it like any other raise. The count is walked to the
 * boundary rather than read, so a raise that counted twice or not at all
 * moves the line the players are told. */
int run_scenario_lua_shape_error_counts(void) {
    static const char *const kMap = "scnlua_shape.map";
    static const char *const kLua =
        "scenario = { name = \"Shapes\", api = 1 }\n"
        "function allow_extra_teams()\n"
        "  return game.pill(\"two\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(slPut(kMap, kLua));
    sim = slLobbySim();
    UT_ASSERT(sim != NULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);

    serverSimStartGame(sim);

    UT_ASSERT_MSG(slAskExtraTeams(sim),
                  "a policy that raised answered anything but the classic "
                  "yes");
    UT_ASSERT_MSG(strstr(scenarioHostLastError(h), "n must be a number") !=
                      NULL,
                  "the raise does not name the argument: %s",
                  scenarioHostLastError(h));

    for (i = 2; i < SCN_ERROR_LIMIT; i++) {
        UT_ASSERT(slAskExtraTeams(sim));
    }
    UT_ASSERT_MSG(strstr(scenarioHostLastError(h),
                         "off for the rest of the round") == NULL,
                  "%d shape errors switched the scenario off and the limit "
                  "is %d: %s", SCN_ERROR_LIMIT - 1, SCN_ERROR_LIMIT,
                  scenarioHostLastError(h));

    UT_ASSERT(slAskExtraTeams(sim));
    UT_ASSERT_MSG(strstr(scenarioHostLastError(h),
                         "off for the rest of the round") != NULL,
                  "%d shape errors in a row left the scenario running: %s",
                  SCN_ERROR_LIMIT, scenarioHostLastError(h));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    slDrop(kMap);
    return 0;
}

/* ── 8. A rule reads back ─────────────────────────────────────────── */

/* Through the host, because the value the row answers is the one the
 * sidecar's own table set: the round start applies the table and the setup
 * call reads it back off the sim. */
int run_scenario_lua_rule_reads_the_table(void) {
    static const char *const kMap    = "scnlua_rule.map";
    static const char *const kRecord = "scnlua_rule.record";
    static const char *const kBody =
        "scenario = {\n"
        "  name = \"Rules\",\n"
        "  api = 1,\n"
        "  rules = { tank_reload_ticks = 9 },\n"
        "}\n"
        "function on_setup()\n"
        "  local v = game.rule(\"tank_reload_ticks\")\n"
        "  local ok, e = pcall(function() return game.rule(\"no_rule\") end)\n"
        "  note(tostring(v) .. \"|\" .. tostring(ok) .. \"|\" .. tostring(e))\n"
        "end\n";
    char          lua[2048];
    char          got[512];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    remove(kRecord);
    slScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(slPut(kMap, lua));
    sim = slLobbySim();
    UT_ASSERT(sim != NULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);

    serverSimStartGame(sim);
    slRead(kRecord, got, sizeof(got));

    UT_ASSERT_MSG(strncmp(got, "9|false|", 8) == 0,
                  "the setup read '%s', expected the sidecar's 9 and a raise "
                  "for the name that spells no rule", got);
    UT_ASSERT_MSG(strstr(got, "no rule is named 'no_rule'") != NULL,
                  "the raise does not name the rule: %s", got);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    slDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 9. Tags and regions ──────────────────────────────────────────── */

/* The four manifest rows against a sidecar that declares one tag and two
 * regions: the tag on the pill it names, the same tag found by name, the
 * rectangle read back whole, a square inside it and one outside, and the
 * region names in the order the row promises rather than the order the
 * sidecar's table happened to iterate in. */
int run_scenario_lua_tags_and_regions(void) {
    static const char *const kMap    = "scnlua_tags.map";
    static const char *const kRecord = "scnlua_tags.record";
    static const char *const kBody =
        "scenario = {\n"
        "  name = \"Tagged\",\n"
        "  api = 1,\n"
        "  tags = { pills = { [2] = \"keep\" } },\n"
        "  regions = {\n"
        "    zone_b = { x = 40, y = 40, w = 2, h = 2 },\n"
        "    zone_a = { x = 10, y = 12, w = 4, h = 6 },\n"
        "  },\n"
        "}\n"
        "function on_setup()\n"
        "  local t = game.tags(\"pill\", 2)\n"
        "  local g = game.tagged(\"keep\")\n"
        "  local n = game.tagged(\"keep\", \"pill\")\n"
        "  local r = game.region(\"zone_a\")\n"
        "  note(table.concat(t, \",\") .. \"|\" ..\n"
        "       g[1].kind .. g[1].n .. \"|\" .. tostring(n[1]) .. \"|\" ..\n"
        "       r.x .. \",\" .. r.y .. \",\" .. r.w .. \",\" .. r.h .. \"|\" ..\n"
        "       tostring(game.in_region(\"zone_a\", 11, 13)) .. \",\" ..\n"
        "       tostring(game.in_region(\"zone_a\", 14, 13)) .. \",\" ..\n"
        "       tostring(game.in_region(\"nowhere\", 11, 13)) .. \"|\" ..\n"
        "       table.concat(game.regions(), \">\") .. \"|\" ..\n"
        "       tostring(game.tags(\"pill\", 1)[1]) .. \"|\" ..\n"
        "       tostring(game.region(\"nowhere\")))\n"
        "end\n";
    char          lua[4096];
    char          got[512];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    remove(kRecord);
    slScript(lua, sizeof(lua), kRecord, kBody);
    UT_ASSERT(slPut(kMap, lua));
    sim = slLobbySim();
    UT_ASSERT(sim != NULL);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);

    serverSimStartGame(sim);
    slRead(kRecord, got, sizeof(got));

    UT_ASSERT_MSG(
        strcmp(got,
               "keep|pill2|2|10,12,4,6|true,false,false|zone_a>zone_b|nil|nil")
            == 0,
        "the setup read '%s'", got);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    slDrop(kMap);
    remove(kRecord);
    return 0;
}
