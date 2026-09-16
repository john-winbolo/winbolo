/*
 * The binding table: the game table a script reads the sim through.
 *
 * Two ways in, because the rows answer two kinds of question. Most cases
 * build a Lua state of their own, install the table on it with a context of
 * their own and run a chunk, which is what lets a case compare what a row
 * marshalled against what the accessor behind it returned. The cases that
 * are about the host — the table on the round's own VM, and what the error
 * limit makes of a raise inside a row — attach a real script.
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
 * run_scenario_lua_rule_reads_the_table— game.rule answers what the script
 *                                        set, and a name that spells no
 *                                        rule raises
 * run_scenario_lua_tags_and_regions    — the tags and regions a script
 *                                        declares read back, a square
 *                                        inside a region and one outside
 *
 * and the writes:
 *
 * run_scenario_lua_refusals_in_round   — every refusal a row answers with
 *                                        while a round is running, one sim
 *                                        per row
 * run_scenario_lua_refusals_in_lobby   — and the rows whose state guard
 *                                        reads the other way
 * run_scenario_lua_op_index_reaches_the_payload
 *                                      — an op on item n reaches the
 *                                        payload as n - 1, read back off
 *                                        the accessor at n
 * run_scenario_lua_add_answers_its_index
 *                                      — an add answers the index it took
 *                                        and that index reads back
 * run_scenario_lua_queued_answers_queued
 *                                      — work that is under way answers
 *                                        true and "queued"
 * run_scenario_lua_shape_raises_refusal_does_not
 *                                      — a mistyped argument and an unknown
 *                                        word raise; a refusal answers
 * run_scenario_lua_detail_carries_the_number
 *                                      — the sentence a refusal carries
 *                                        holds the number that mattered
 * run_scenario_lua_teleport_start_refuses_bad_index
 *                                      — a start number no start could have
 *                                        is refused rather than read as
 *                                        "the engine picks"
 * run_scenario_lua_spawn_bot_refuses_bad_start
 *                                      — and the same numbers in a spawn's
 *                                        table, where zero keeps its meaning
 * run_scenario_lua_game_type_resolves_scripted
 *                                      — a scripted round reads back the base
 *                                        game it plays, never "scripted"
 *
 * and, run from the lobby case because it is the same subject: a row with a
 * state guard answers the state before its own arguments, while a row over
 * the map's own lists works in a lobby and in a round alike.
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

/* The timers every state this file builds is given. One set between them
 * rather than one per case, and emptied as each state is installed: the
 * references in it belong to whichever state was last installed, and that
 * state is closed before the next is built. Emptied without releasing, for
 * the same reason — there is nothing left to release them against.
 *
 * A case that wants to know what the timer rows did reads this; nothing
 * here fires one, because a timer runs from the host's tick and these cases
 * have no host. */
static ScnTimerSet slTimers;

/* The context outlives the state it is installed on: both are the caller's
 * locals and the state is closed first. The manifest is not const because
 * one row writes it — game.define_region names a rectangle for the round —
 * and the caller's own struct is what it writes into. */
static lua_State *slVm(ScnLuaCtx *ctx, ServerSim *sim, ScenarioManifest *m) {
    lua_State *L = luaL_newstate();

    if (L == NULL) {
        return NULL;
    }
    luaL_openlibs(L);
    scenarioLuaTimersReset(&slTimers);
    ctx->sim       = sim;
    ctx->manifest  = m;
    ctx->timers    = &slTimers;
    /* Running the file, not checking it: the write rows apply. */
    ctx->checkOnly = false;
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

/* A global the chunk left, as a boolean. Anything that is not one reads
 * false, so a case asserting true is asserting the boolean as well. */
static bool slGlobalBool(lua_State *L, const char *name) {
    bool v;

    lua_getglobal(L, name);
    v = lua_isboolean(L, -1) && lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

/* A global the chunk left, as a string. "" when it is not one. */
static void slGlobalStr(lua_State *L, const char *name, char *out,
                        size_t outLen) {
    out[0] = '\0';
    lua_getglobal(L, name);
    if (lua_type(L, -1) == LUA_TSTRING) {
        snprintf(out, outLen, "%s", lua_tostring(L, -1));
    }
    lua_pop(L, 1);
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

/* A lobby the ops can work in: taking part, in the lobby state, with a
 * person in seat 0 and a server that runs bots. scenarioRequireLobby wants
 * the first two together, and the sim below deliberately has neither — it is
 * built for the host cases, which start a round without a lobby.
 *
 * Modelled on the fixture in test_scenario_roster_arms.c, which is what the
 * lobby handlers are tested against there. */
static ServerSim *slSeatedLobbySim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    serverSimSetBotAiType(sim, aiFull);
    return sim;
}

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

/* ── Scripts, for the host-driven cases ───────────────────────────── */

static void slScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool slPut(const char *mapPath, const char *lua) {
    char  path[512];
    FILE *f;

    slScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void slDrop(const char *mapPath) {
    char path[512];
    slScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

/* A hook says what it read by printing a marked line. A scenario state has
 * no io, and its print goes to the server console, which the watcher below
 * catches. The mark is what tells a hook's line from the host's own: both
 * arrive on the same console. */
#define SL_NOTE_MARK "note:"

static char   slNote[8192];
static size_t slNoteLen;

static void slNoteReset(void) {
    slNote[0] = '\0';
    slNoteLen = 0;
}

/* One console line, kept if a hook wrote it. A record past the buffer is
   truncated rather than overrunning it. */
static void slNoteLine(const char *msg) {
    size_t mark = strlen(SL_NOTE_MARK);
    size_t room;
    size_t n;

    if (msg == NULL || strncmp(msg, SL_NOTE_MARK, mark) != 0) {
        return;
    }
    n    = strlen(msg + mark);
    room = sizeof(slNote) - 1 - slNoteLen;
    if (n > room) {
        n = room;
    }
    memcpy(slNote + slNoteLen, msg + mark, n);
    slNoteLen += n;
    slNote[slNoteLen] = '\0';
}

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static void (*slConsolePrev)(void *ctx, char *msg) = NULL;

static void slConsoleCb(void *ctx, char *msg) {
    if (slConsolePrev != NULL) {
        slConsolePrev(ctx, msg);
    }
    slNoteLine(msg);
}

static void slWatchConsole(ServerSim *sim) {
    slConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = slConsoleCb;
}

static void slUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = slConsolePrev;
    slConsolePrev = NULL;
}

static void slScript(char *out, size_t outLen, const char *body) {
    snprintf(out, outLen,
             "local function note(s)\n"
             "  print(\"" SL_NOTE_MARK "\" .. tostring(s))\n"
             "end\n"
             "%s", body);
}

static void slRead(char *out, size_t outLen) {
    snprintf(out, outLen, "%s", slNote);
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
    /* The three that change nothing on the sim. The timer sets one and
       cancels the id it was just given, so it answers the same whichever
       order this table happens to be walked in and leaves nothing waiting
       for a tick that never comes. */
    "  timer       = function() return game.timer(1, function() end) end,\n"
    "  cancel_timer = function()\n"
    "    return game.cancel_timer(game.timer(1, function() end))\n"
    "  end,\n"
    "  define_region = function()\n"
    "    return game.define_region(\"made_here\", 1, 2, 3, 4)\n"
    "  end,\n"
    /* The writes. Most are aimed at a seat nobody holds or an item the map
       does not have, so the case reaches every row without leaving the sim
       somewhere the next row cannot work in. */
    "  set_stocks  = function() return game.set_stocks(0, {shells=1}) end,\n"
    "  add_stocks  = function() return game.add_stocks(0, {shells=1}) end,\n"
    "  kill_tank   = function() return game.kill_tank(9) end,\n"
    "  teleport    = function() return game.teleport(9, 100, 100) end,\n"
    "  teleport_to_start = function() return game.teleport_to_start(9) end,\n"
    "  set_boat    = function() return game.set_boat(9, true) end,\n"
    "  give_pill   = function() return game.give_pill(9, 1) end,\n"
    "  drop_pill   = function() return game.drop_pill(9, 1) end,\n"
    "  set_modifiers = function() return game.set_modifiers(9, {speed=120}) end,\n"
    "  builder_order = function() return game.builder_order(9, \"road\", 100, 100) end,\n"
    "  builder_recall = function() return game.builder_recall(9) end,\n"
    "  kill_lgm    = function() return game.kill_lgm(9) end,\n"
    "  builder_parachute = function() return game.builder_parachute(9) end,\n"
    "  set_builder_carried = function() return game.set_builder_carried(9, 1, 1) end,\n"
    "  set_pill_owner  = function() return game.set_pill_owner(99, 0) end,\n"
    "  set_pill_armour = function() return game.set_pill_armour(99, 1) end,\n"
    "  set_pill_speed  = function() return game.set_pill_speed(99, 10) end,\n"
    "  move_pill   = function() return game.move_pill(99, 100, 100) end,\n"
    "  set_base_owner = function() return game.set_base_owner(99, 0) end,\n"
    "  set_base_stock = function() return game.set_base_stock(99, 1, 1, 1) end,\n"
    "  add_pill    = function() return game.add_pill(5, 5) end,\n"
    "  remove_pill = function() return game.remove_pill(99) end,\n"
    "  add_base    = function() return game.add_base(5, 5) end,\n"
    "  remove_base = function() return game.remove_base(99) end,\n"
    "  add_start   = function() return game.add_start(5, 5, 0) end,\n"
    "  remove_start = function() return game.remove_start(99) end,\n"
    "  set_tile    = function() return game.set_tile(5, 5, 4) end,\n"
    "  fill_rect   = function() return game.fill_rect(5, 5, 6, 6, 4) end,\n"
    "  place_mine  = function() return game.place_mine(5, 5) end,\n"
    "  remove_mine = function() return game.remove_mine(5, 5) end,\n"
    "  spawn_bot   = function() return game.spawn_bot({team=2}) end,\n"
    "  remove_bot  = function() return game.remove_bot(9) end,\n"
    "  set_team    = function() return game.set_team(9, 2) end,\n"
    "  lobby_add_bot = function() return game.lobby_add_bot({team=2}) end,\n"
    "  lobby_remove_bot = function() return game.lobby_remove_bot(9) end,\n"
    "  lobby_set_team = function() return game.lobby_set_team(9, 2) end,\n"
    "  message     = function() return game.message(\"hello\") end,\n"
    "  sound       = function() return game.sound(\"shoot_self\") end,\n"
    "  log         = function() return game.log(\"hello\") end,\n"
    "  set_game_time = function() return game.set_game_time(1000) end,\n"
    "  add_game_time = function() return game.add_game_time(10) end,\n"
    "  set_rule    = function() return game.set_rule(\"tank_reload_ticks\", 12) end,\n"
    /* The calls run in whatever order the table iterates in, which is not
       the same order under the two Lua builds, so no row here leans on
       another having run. This one ends the round the rest are working in,
       and a row that runs after it answers that the round is over — which
       is an answer, which is all this case asks for. */
    "  end_round   = function() return game.end_round(\"done\") end,\n"
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
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

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
 * script's own table set: the round start applies the table and the setup
 * call reads it back off the sim. */
int run_scenario_lua_rule_reads_the_table(void) {
    static const char *const kMap  = "scnlua_rule.map";
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

    slNoteReset();
    slScript(lua, sizeof(lua), kBody);
    UT_ASSERT(slPut(kMap, lua));
    sim = slLobbySim();
    UT_ASSERT(sim != NULL);
    /* Before the attach: a script that prints at its top level prints while
       the attach is still running. */
    slWatchConsole(sim);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    slRead(got, sizeof(got));

    UT_ASSERT_MSG(strncmp(got, "9|false|", 8) == 0,
                  "the setup read '%s', expected the script's 9 and a raise "
                  "for the name that spells no rule", got);
    UT_ASSERT_MSG(strstr(got, "no rule is named 'no_rule'") != NULL,
                  "the raise does not name the rule: %s", got);

    slUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    slDrop(kMap);
    slNoteReset();
    return 0;
}

/* ── 9. Tags and regions ──────────────────────────────────────────── */

/* The four manifest rows against a script that declares one tag and two
 * regions: the tag on the pill it names, the same tag found by name, the
 * rectangle read back whole, a square inside it and one outside, and the
 * region names in the order the row promises rather than the order the
 * script's table happened to iterate in. */
int run_scenario_lua_tags_and_regions(void) {
    static const char *const kMap  = "scnlua_tags.map";
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

    slNoteReset();
    slScript(lua, sizeof(lua), kBody);
    UT_ASSERT(slPut(kMap, lua));
    sim = slLobbySim();
    UT_ASSERT(sim != NULL);
    /* Before the attach: a script that prints at its top level prints while
       the attach is still running. */
    slWatchConsole(sim);
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    slRead(got, sizeof(got));

    UT_ASSERT_MSG(
        strcmp(got,
               "keep|pill2|2|10,12,4,6|true,false,false|zone_a>zone_b|nil|nil")
            == 0,
        "the setup read '%s'", got);

    slUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    slDrop(kMap);
    slNoteReset();
    return 0;
}

/* ── 10. Every refusal a row can answer ───────────────────────────── */

/* One row per refusal: what the script asks, and the code the funnel is
 * expected to answer. Each runs on a sim of its own, so nothing a row does
 * to the world reaches the row after it.
 *
 * The chunk ends by putting the three answers in res, code and detail, and
 * a chunk that needs the world arranged first arranges it in its own lines
 * above that. */
typedef struct {
    const char *what;
    const char *lua;
    ScnOpResult want;
} SlRefusal;

/* Lua that leaves fx, fy on a grass square with no pill or base on it: dry
 * ground a pill or a mine will go on, and ground a start will not. A search
 * rather than a coordinate, because where this map puts its land is not
 * something a case should have to know — and a search that finds nothing
 * says so rather than handing the row a nil. */
#define SL_FIND_FREE                                                        \
    "local taken = {}\n"                                                    \
    "for n = 1, game.num_pills() do\n"                                      \
    "  local p = game.pill(n)\n"                                            \
    "  if p then taken[p.y * 256 + p.x] = true end\n"                       \
    "end\n"                                                                 \
    "for n = 1, game.num_bases() do\n"                                      \
    "  local b = game.base(n)\n"                                            \
    "  if b then taken[b.y * 256 + b.x] = true end\n"                       \
    "end\n"                                                                 \
    "local fx, fy\n"                                                        \
    "for y = 30, 220 do\n"                                                  \
    "  for x = 30, 220 do\n"                                                \
    "    if game.map_tile(x, y) == game.TERRAIN.grass and\n"                \
    "       not taken[y * 256 + x] then fx, fy = x, y break end\n"          \
    "  end\n"                                                               \
    "  if fx then break end\n"                                              \
    "end\n"                                                                 \
    "if not fx then error(\"no free grass on this map\") end\n"

/* And sx, sy on open water with no mine on it, which is the one thing a
 * start will go on. Taken from a start the map already has, so the square
 * satisfies the test rather than being believed to. */
#define SL_FIND_SEA                                                         \
    "local sx, sy\n"                                                        \
    "for n = 1, game.num_starts() do\n"                                     \
    "  local s = game.start(n)\n"                                           \
    "  if s and game.map_tile(s.x, s.y) == game.TERRAIN.deep_sea and\n"     \
    "     not game.is_mine(s.x, s.y) then sx, sy = s.x, s.y break end\n"    \
    "end\n"                                                                 \
    "if not sx then error(\"no open water under this map's starts\") end\n"

/* And px, py on a live pill standing on ordinary ground — road, grass,
 * forest, rubble, swamp or crater. Two of the rows below are about the pill
 * being there, and their handlers test the terrain first, so the square has
 * to be one the terrain half of the test accepts or the answer comes back
 * for the wrong reason. Armoured, because a dead pill is ground a tank
 * drives over rather than something in its way. */
#define SL_FIND_PILL_ON_GROUND                                              \
    "local px, py\n"                                                        \
    "for n = 1, game.num_pills() do\n"                                      \
    "  local p = game.pill(n)\n"                                            \
    "  if p and p.armour > 0 then\n"                                        \
    "    local t = game.map_tile(p.x, p.y)\n"                               \
    "    if t == game.TERRAIN.road or t == game.TERRAIN.grass or\n"         \
    "       t == game.TERRAIN.forest or t == game.TERRAIN.rubble or\n"      \
    "       t == game.TERRAIN.swamp or t == game.TERRAIN.crater then\n"     \
    "      px, py = p.x, p.y break\n"                                       \
    "    end\n"                                                             \
    "  end\n"                                                               \
    "end\n"                                                                 \
    "if not px then error(\"no pill on ordinary ground\") end\n"

/* A rectangle far larger than one tick's budget of squares, over ground that
 * cannot already be what it is being painted. */
#define SL_BIG_FILL "game.fill_rect(30, 30, 220, 220, game.TERRAIN.swamp)"

static const SlRefusal kSlRunningRefusals[] = {
    { "stocks for a seat nobody holds",
      "res, code, detail = game.set_stocks(9, { shells = 1 })\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "stocks past what a tank holds",
      "res, code, detail = game.set_stocks(0, { shells = 250 })\n",
      SCN_OP_RANGE },
    { "stocks for a tank that is dead",
      "game.kill_tank(0)\n"
      "res, code, detail = game.set_stocks(0, { armour = 1 })\n",
      SCN_OP_TANK_DEAD },
    { "deltas for a seat nobody holds",
      "res, code, detail = game.add_stocks(9, { shells = 1 })\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "killing a seat nobody holds",
      "res, code, detail = game.kill_tank(9)\n", SCN_OP_NO_SUCH_PLAYER },
    { "killing a tank that is already dead",
      "game.kill_tank(0)\n"
      "res, code, detail = game.kill_tank(0)\n", SCN_OP_TANK_DEAD },
    /* The handler answers for the killer the way every other row answers for
       a slot naming nobody; SCN_OP_RANGE there is for a cause outside the
       four, which this row cannot send. */
    { "a killer who is not in the round",
      "res, code, detail = game.kill_tank(0, 9)\n", SCN_OP_NO_SUCH_PLAYER },
    { "teleporting a seat nobody holds",
      "res, code, detail = game.teleport(9, 100, 100)\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "teleporting into the border",
      "res, code, detail = game.teleport(0, 5, 5)\n", SCN_OP_BAD_SQUARE },
    { "teleporting past the end of the map",
      "res, code, detail = game.teleport(0, 300, 100)\n", SCN_OP_BAD_SQUARE },
    { "teleporting onto a pill",
      SL_FIND_PILL_ON_GROUND
      "res, code, detail = game.teleport(0, px, py)\n",
      SCN_OP_BAD_TERRAIN },
    { "a start the map does not have",
      "res, code, detail = game.teleport_to_start(0, 99)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a boat for a seat nobody holds",
      "res, code, detail = game.set_boat(9, true)\n", SCN_OP_NO_SUCH_PLAYER },
    /* A boat needs water under the tank, so the tank is put on dry ground
       first rather than the case hoping that is where it started. */
    { "a boat on dry ground",
      SL_FIND_FREE
      "game.teleport(0, fx, fy)\n"
      "res, code, detail = game.set_boat(0, true)\n", SCN_OP_BAD_TERRAIN },
    { "giving a pill the map does not have",
      "res, code, detail = game.give_pill(0, 99)\n", SCN_OP_NO_SUCH_ITEM },
    { "giving a pill that is already carried",
      "game.give_pill(0, 1)\n"
      "res, code, detail = game.give_pill(0, 1)\n", SCN_OP_CARRIED },
    { "dropping a pill the tank is not carrying",
      "res, code, detail = game.drop_pill(0, 1)\n", SCN_OP_NO_SUCH_ITEM },
    /* The square another pill is standing on is the first thing the drop
       test turns down, whatever the ground under it is. */
    { "dropping a pill onto another pill",
      "local other = game.pill(2)\n"
      "game.give_pill(0, 1)\n"
      "res, code, detail = game.drop_pill(0, 1, other.x, other.y)\n",
      SCN_OP_BAD_TERRAIN },
    { "modifiers for a seat nobody holds",
      "res, code, detail = game.set_modifiers(9, {})\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "a modifier past a percentage",
      "res, code, detail = game.set_modifiers(0, { speed = 300 })\n",
      SCN_OP_RANGE },
    { "an order for a seat nobody holds",
      "res, code, detail = game.builder_order(9, \"road\", 100, 100)\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "an order in the border",
      "res, code, detail = game.builder_order(0, \"road\", 5, 5)\n",
      SCN_OP_BAD_SQUARE },
    { "recalling a builder who is in the tank",
      "res, code, detail = game.builder_recall(0)\n", SCN_OP_ALREADY },
    { "killing a builder for a seat nobody holds",
      "res, code, detail = game.kill_lgm(9)\n", SCN_OP_NO_SUCH_PLAYER },
    { "parachuting a builder who is not dead",
      "res, code, detail = game.builder_parachute(0)\n", SCN_OP_ALREADY },
    { "a builder carrying more than he can",
      "res, code, detail = game.set_builder_carried(0, 300)\n",
      SCN_OP_RANGE },
    { "owning a pill the map does not have",
      "res, code, detail = game.set_pill_owner(99, 0)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a pill owner who is not a seat",
      "res, code, detail = game.set_pill_owner(1, 200)\n", SCN_OP_RANGE },
    { "owning a pill that is carried",
      "game.give_pill(0, 1)\n"
      "res, code, detail = game.set_pill_owner(1, 0)\n", SCN_OP_CARRIED },
    { "armour on a pill the map does not have",
      "res, code, detail = game.set_pill_armour(99, 1)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "armour past what a pill holds",
      "res, code, detail = game.set_pill_armour(1, 99)\n", SCN_OP_RANGE },
    { "a firing rate outside the pair",
      "res, code, detail = game.set_pill_speed(1, 0)\n", SCN_OP_RANGE },
    { "moving a pill into the border",
      "res, code, detail = game.move_pill(1, 5, 5)\n", SCN_OP_BAD_SQUARE },
    { "moving a pill onto another pill",
      "local p = game.pill(2)\n"
      "res, code, detail = game.move_pill(1, p.x, p.y)\n",
      SCN_OP_BAD_TERRAIN },
    { "moving a pill that is carried",
      "game.give_pill(0, 1)\n"
      "res, code, detail = game.move_pill(1, 100, 100)\n", SCN_OP_CARRIED },
    { "owning a base the map does not have",
      "res, code, detail = game.set_base_owner(99, 0)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a base owner who is not a seat",
      "res, code, detail = game.set_base_owner(1, 200)\n", SCN_OP_RANGE },
    { "stock for a base the map does not have",
      "res, code, detail = game.set_base_stock(99, 1, 1, 1)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "adding a pill in the border",
      "res, code, detail = game.add_pill(5, 5)\n", SCN_OP_BAD_SQUARE },
    { "adding a pill for an owner who is not a seat",
      "res, code, detail = game.add_pill(100, 100, 200)\n", SCN_OP_RANGE },
    { "adding a pill on top of one",
      "local p = game.pill(1)\n"
      "res, code, detail = game.add_pill(p.x, p.y)\n", SCN_OP_BAD_TERRAIN },
    { "a seventeenth pill",
      SL_FIND_FREE
      "res, code, detail = game.add_pill(fx, fy)\n", SCN_OP_FULL },
    { "removing a pill the map does not have",
      "res, code, detail = game.remove_pill(99)\n", SCN_OP_NO_SUCH_ITEM },
    { "removing a pill twice",
      "game.remove_pill(1)\n"
      "res, code, detail = game.remove_pill(1)\n", SCN_OP_NO_SUCH_ITEM },
    { "removing a pill that is carried",
      "game.give_pill(0, 1)\n"
      "res, code, detail = game.remove_pill(1)\n", SCN_OP_CARRIED },
    { "adding a base in the border",
      "res, code, detail = game.add_base(5, 5)\n", SCN_OP_BAD_SQUARE },
    { "adding a base for an owner who is not a seat",
      "res, code, detail = game.add_base(100, 100, 200)\n", SCN_OP_RANGE },
    { "removing a base the map does not have",
      "res, code, detail = game.remove_base(99)\n", SCN_OP_NO_SUCH_ITEM },
    /* A start goes on open water and nowhere else, so dry ground is the
       terrain it refuses rather than the terrain it wants. */
    { "a start on ground rather than open water",
      SL_FIND_FREE
      "res, code, detail = game.add_start(fx, fy, 0)\n",
      SCN_OP_BAD_TERRAIN },
    { "a seventeenth start",
      SL_FIND_SEA
      "res, code, detail = game.add_start(sx, sy, 0)\n", SCN_OP_FULL },
    { "removing a start the map does not have",
      "res, code, detail = game.remove_start(99)\n", SCN_OP_NO_SUCH_ITEM },
    { "writing a square in the border",
      "res, code, detail = game.set_tile(5, 5, game.TERRAIN.road)\n",
      SCN_OP_BAD_SQUARE },
    { "writing a terrain that is not one",
      "res, code, detail = game.set_tile(100, 100, 99)\n", SCN_OP_RANGE },
    { "a rectangle in the border",
      "res, code, detail = game.fill_rect(5, 5, 10, 10, game.TERRAIN.road)\n",
      SCN_OP_BAD_SQUARE },
    { "a second rectangle while one is running",
      SL_BIG_FILL "\n"
      "res, code, detail = game.fill_rect(150, 150, 160, 160,"
      " game.TERRAIN.road)\n", SCN_OP_RATE },
    { "a mine in the border",
      "res, code, detail = game.place_mine(5, 5)\n", SCN_OP_BAD_SQUARE },
    { "a mine for an owner who is not a seat",
      "res, code, detail = game.place_mine(100, 100, 200)\n", SCN_OP_RANGE },
    { "a mine under a pill",
      SL_FIND_PILL_ON_GROUND
      "res, code, detail = game.place_mine(px, py)\n",
      SCN_OP_BAD_TERRAIN },
    { "a second mine on one square",
      SL_FIND_FREE
      "game.place_mine(fx, fy)\n"
      "res, code, detail = game.place_mine(fx, fy)\n", SCN_OP_ALREADY },
    { "taking a mine off a square with none",
      SL_FIND_FREE
      "res, code, detail = game.remove_mine(fx, fy)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "taking a mine off the border",
      "res, code, detail = game.remove_mine(5, 5)\n", SCN_OP_BAD_SQUARE },
    { "removing a bot from a seat a person holds",
      "res, code, detail = game.remove_bot(0)\n", SCN_OP_IS_HUMAN },
    { "removing a bot from a seat nobody holds",
      "res, code, detail = game.remove_bot(9)\n", SCN_OP_NO_SUCH_PLAYER },
    { "moving a seat nobody holds",
      "res, code, detail = game.set_team(9, 2)\n", SCN_OP_NO_SUCH_PLAYER },
    { "moving a seat to a team the lobby has not got",
      "res, code, detail = game.set_team(0, 99)\n", SCN_OP_RANGE },
    { "seating a lobby bot mid-round",
      "res, code, detail = game.lobby_add_bot({ team = 2 })\n",
      SCN_OP_WRONG_STATE },
    { "taking a lobby bot out mid-round",
      "res, code, detail = game.lobby_remove_bot(0)\n", SCN_OP_WRONG_STATE },
    { "moving a lobby seat mid-round",
      "res, code, detail = game.lobby_set_team(0, 2)\n",
      SCN_OP_WRONG_STATE },
    { "a line longer than the buffer",
      "res, code, detail = game.message(string.rep(\"x\", 4000))\n",
      SCN_OP_TOO_BIG },
    { "a line to a seat nobody holds",
      "res, code, detail = game.message(\"hello\", 9)\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "a line to a team that is not one",
      "res, code, detail = game.message(\"hello\", { team = 99 })\n",
      SCN_OP_RANGE },
    { "a sound the server cannot aim",
      "res, code, detail = game.sound(\"bubbles\")\n", SCN_OP_RANGE },
    { "a sound in the border",
      "res, code, detail = game.sound(\"shoot_self\", 5, 5)\n",
      SCN_OP_BAD_SQUARE },
    { "a console line longer than the buffer",
      "res, code, detail = game.log(string.rep(\"x\", 4000))\n",
      SCN_OP_TOO_BIG },
    { "a win line longer than the buffer",
      "res, code, detail = game.end_round(string.rep(\"x\", 4000))\n",
      SCN_OP_TOO_BIG },
    { "a winner the lobby has no team for",
      "res, code, detail = game.end_round(\"done\", 99)\n", SCN_OP_RANGE },
    { "a round with no time left at all",
      "res, code, detail = game.set_game_time(0)\n", SCN_OP_RANGE },
    { "a rule value outside its row",
      "res, code, detail = game.set_rule(\"tank_reload_ticks\", 99999)\n",
      SCN_OP_RANGE },
    { "a rule value that breaks its pair",
      "res, code, detail = game.set_rule(\"pill_attack_min_ticks\", 200)\n",
      SCN_OP_PAIR },
};

/* The rows whose state guard is the other way round: the lobby is where they
 * work, or where they are refused. These run on a lobby that is taking part,
 * with a person in seat 0 — scenarioRequireLobby wants the lobby enabled as
 * well as the state, so a sim built with it switched off answers
 * SCN_OP_WRONG_STATE to every row here and says nothing about any of them. */
static const SlRefusal kSlLobbyRefusals[] = {
    { "moving a lobby seat nobody holds",
      "res, code, detail = game.lobby_set_team(9, 2)\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "moving a lobby seat to a team the lobby has not got",
      "res, code, detail = game.lobby_set_team(0, 99)\n", SCN_OP_RANGE },
    { "taking a lobby bot out of a seat a person holds",
      "res, code, detail = game.lobby_remove_bot(0)\n", SCN_OP_IS_HUMAN },
    { "taking a lobby bot out of an empty seat",
      "res, code, detail = game.lobby_remove_bot(9)\n",
      SCN_OP_NO_SUCH_PLAYER },
    { "a lobby bot on a team the lobby has not got",
      "res, code, detail = game.lobby_add_bot({ team = 99 })\n",
      SCN_OP_RANGE },
    { "spawning a bot into a round that is not running",
      "res, code, detail = game.spawn_bot({ team = 2 })\n",
      SCN_OP_WRONG_STATE },
    { "taking a bot out of a round that is not running",
      "res, code, detail = game.remove_bot(0)\n", SCN_OP_WRONG_STATE },
    { "moving a seat in a round that is not running",
      "res, code, detail = game.set_team(0, 2)\n", SCN_OP_WRONG_STATE },
    { "ending a round the lobby has not started",
      "res, code, detail = game.end_round(\"done\")\n", SCN_OP_WRONG_STATE },
    { "timing a round the lobby has not started",
      "res, code, detail = game.set_game_time(1000)\n",
      SCN_OP_WRONG_STATE },
};

/* Each row on a sim of its own: what one row does to the world is no
 * business of the next. */
static int slRefusals(const SlRefusal *rows, size_t count, bool running) {
    size_t i;

    for (i = 0; i < count; i++) {
        ServerSim       *sim;
        ScenarioManifest m;
        ScnLuaCtx        ctx;
        lua_State       *L;
        char             err[512];
        char             code[64];
        char             detail[256];
        const char      *want;
        bool             refused;

        sim = running ? ut_make_running_sim("Seat0") : slSeatedLobbySim();
        UT_ASSERT_MSG(sim != NULL, "%s: could not build a sim",
                      rows[i].what);
        memset(&m, 0, sizeof(m));
        L = slVm(&ctx, sim, &m);
        UT_ASSERT_MSG(L != NULL, "%s: no Lua state", rows[i].what);

        UT_ASSERT_MSG(slRun(L, rows[i].lua, err, sizeof(err)),
                      "%s: the chunk raised, and a refusal never raises: %s",
                      rows[i].what, err);

        refused = slGlobalIsNil(L, "res");
        slGlobalStr(L, "code", code, sizeof(code));
        slGlobalStr(L, "detail", detail, sizeof(detail));
        want = scenarioLuaResultName((int)rows[i].want);

        UT_ASSERT_MSG(refused,
                      "%s: answered something rather than nil, so it was not "
                      "refused at all", rows[i].what);
        UT_ASSERT_MSG(strcmp(code, want) == 0,
                      "%s: answered '%s', expected '%s'", rows[i].what, code,
                      want);
        UT_ASSERT_MSG(detail[0] != '\0',
                      "%s: the refusal says nothing about what it was about",
                      rows[i].what);

        lua_close(L);
        serverSimDestroy(sim);
    }
    return 0;
}

int run_scenario_lua_refusals_in_round(void) {
    return slRefusals(kSlRunningRefusals,
                      sizeof(kSlRunningRefusals) /
                          sizeof(kSlRunningRefusals[0]),
                      true);
}

/* At the foot of the file, beside the two sims it needs. */
static int slStatePrecedence(void);

/* Beside them, on the same lobby fixture: the one lobby row that is not a
 * refusal. */
static int slUnfieldedSeat(void);

int run_scenario_lua_refusals_in_lobby(void) {
    int r = slRefusals(kSlLobbyRefusals,
                       sizeof(kSlLobbyRefusals) / sizeof(kSlLobbyRefusals[0]),
                       false);
    if (r != 0) {
        return r;
    }
    r = slUnfieldedSeat();
    if (r != 0) {
        return r;
    }
    /* The rule the table above leans on, asserted rather than assumed. */
    return slStatePrecedence();
}

/* ── 11. An op index reaches the payload one lower ────────────────── */

/* Every pill on the map given an armour of its own through the op, then read
 * back through the accessor at the same number. An index that reached the
 * payload one either side of where it should would put each armour on the
 * wrong pill, which a case that only asked whether the call was made would
 * not see. */
int run_scenario_lua_op_index_reaches_the_payload(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    BYTE             count;
    int              n;

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    count = serverSimGetPillCount(sim);
    UT_ASSERT_MSG(count > 1, "the map has %u pills and this needs two",
                  (unsigned)count);

    /* Distinct per index and inside what a pill holds, counting down so the
       armour a pill ends with is nothing like its own number. */
    UT_ASSERT_MSG(slRun(L,
                        "ok = {}\n"
                        "for n = 1, game.num_pills() do\n"
                        "  ok[n] = game.set_pill_armour(n, 16 - n)\n"
                        "end\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    for (n = 1; n <= (int)count; n++) {
        ServerSimPillInfo info;
        UT_ASSERT(serverSimGetPillInfo(sim, (BYTE)n, &info));
        UT_ASSERT_MSG((int)info.armour == 16 - n,
                      "the script set pill %d to %d and pill %d holds %u: the "
                      "index reached the payload as something other than %d",
                      n, 16 - n, n, (unsigned)info.armour, n - 1);
    }

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 12. An add answers the index it took ─────────────────────────── */

/* And that index reads back as the thing that was added, through the read
 * row beside it: the op counts from zero and the script counts from one, so
 * an add that answered the payload's own number would name the pill below
 * the one it made. */
int run_scenario_lua_add_answers_its_index(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    long             got;
    long             x, y;
    ServerSimPillInfo info;

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    /* Everard fills its pill list, so a slot has to be freed before one can
       be added; the square the removed pill was on is one a pill fits. */
    UT_ASSERT_MSG(slRun(L,
                        "local gone = game.pill(2)\n"
                        "game.remove_pill(2)\n"
                        "took = game.add_pill(gone.x, gone.y, game.NEUTRAL, 5)\n"
                        "local back = game.pill(took)\n"
                        "bx = back and back.x or -1\n"
                        "by = back and back.y or -1\n"
                        "barmour = back and back.armour or -1\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    got = slGlobalInt(L, "took");
    UT_ASSERT_MSG(got >= 1 && got <= (long)serverSimGetPillCount(sim),
                  "the add answered %ld, which is no pill on the map", got);
    /* The slot the removal freed is the one the add took back. */
    UT_ASSERT_MSG(got == 2, "the add answered %ld, expected the freed slot 2",
                  got);

    x = slGlobalInt(L, "bx");
    y = slGlobalInt(L, "by");
    UT_ASSERT_MSG(slGlobalInt(L, "barmour") == 5,
                  "the pill read back at %ld holds %ld armour, expected the 5 "
                  "it was added with", got, slGlobalInt(L, "barmour"));
    UT_ASSERT(serverSimGetPillInfo(sim, (BYTE)got, &info));
    UT_ASSERT_MSG((long)info.x == x && (long)info.y == y,
                  "the read row says (%ld, %ld) and the accessor says (%u, %u)",
                  x, y, (unsigned)info.x, (unsigned)info.y);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 13. Work that is under way answers so ────────────────────────── */

/* A rectangle too big for one tick's budget is accepted and finished over
 * the ticks after it, which the row says with a second answer rather than a
 * different first one. */
int run_scenario_lua_queued_answers_queued(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    char             second[64];
    bool             first;

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    /* Wider than a tick's budget of squares, over ground that is not already
       what it is being painted. */
    UT_ASSERT_MSG(slRun(L,
                        "res, code = game.fill_rect(100, 100, 140, 140,"
                        " game.TERRAIN.road)\n"
                        "first = res\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    lua_getglobal(L, "first");
    first = lua_isboolean(L, -1) && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    slGlobalStr(L, "code", second, sizeof(second));

    UT_ASSERT_MSG(first,
                  "a fill that is under way answered something other than "
                  "true");
    UT_ASSERT_MSG(strcmp(second, "queued") == 0,
                  "the fill's second answer was '%s', expected \"queued\"",
                  second);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 14. A shape error raises; a refusal does not ─────────────────── */

/* The two live side by side here so the difference cannot drift: the same
 * row, asked once with an argument of the wrong type and once with a number
 * the world turns down. */
int run_scenario_lua_shape_raises_refusal_does_not(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    char             raised[256];
    char             code[64];

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L,
                        "shape_ok, shape_err = pcall(function()\n"
                        "  return game.set_pill_armour(\"one\", 1)\n"
                        "end)\n"
                        "word_ok, word_err = pcall(function()\n"
                        "  return game.builder_order(0, \"repair\", 100, 100)\n"
                        "end)\n"
                        "refuse_ok, res, code = pcall(function()\n"
                        "  return game.set_pill_armour(99, 1)\n"
                        "end)\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    /* A mistyped argument raises, and says which argument it was. */
    lua_getglobal(L, "shape_ok");
    UT_ASSERT_MSG(!lua_toboolean(L, -1),
                  "a pill index of \"one\" was taken rather than raised");
    lua_pop(L, 1);
    slGlobalStr(L, "shape_err", raised, sizeof(raised));
    UT_ASSERT_MSG(strstr(raised, "n must be a number") != NULL,
                  "the raise does not name the argument: %s", raised);

    /* And so does a word that names nothing: the builder has six actions and
       the engine decides repairing from the square. */
    lua_getglobal(L, "word_ok");
    UT_ASSERT_MSG(!lua_toboolean(L, -1),
                  "\"repair\" was taken as a builder action");
    lua_pop(L, 1);
    slGlobalStr(L, "word_err", raised, sizeof(raised));
    UT_ASSERT_MSG(strstr(raised, "not a builder action") != NULL,
                  "the raise does not say what the word should have been: %s",
                  raised);

    /* A refusal is an answer. The call returns, and the script reads why. */
    lua_getglobal(L, "refuse_ok");
    UT_ASSERT_MSG(lua_toboolean(L, -1),
                  "a pill the map does not have raised rather than answering");
    lua_pop(L, 1);
    UT_ASSERT_MSG(slGlobalIsNil(L, "res"),
                  "the refusal answered something rather than nil");
    slGlobalStr(L, "code", code, sizeof(code));
    UT_ASSERT_MSG(strcmp(code, scenarioLuaResultName(SCN_OP_NO_SUCH_ITEM)) == 0,
                  "the refusal answered '%s'", code);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 15. The detail carries the number that mattered ──────────────── */

int run_scenario_lua_detail_carries_the_number(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    char             detail[256];

    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L,
                        "local a, b, c = game.message(string.rep(\"x\", 4000))\n"
                        "size = c\n"
                        "local d, e, f = game.set_pill_armour(1, 99)\n"
                        "range = f\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    /* A size refusal says how long the text was and how long it may be. */
    slGlobalStr(L, "size", detail, sizeof(detail));
    UT_ASSERT_MSG(strstr(detail, "4000") != NULL,
                  "the size refusal does not say how long the text was: %s",
                  detail);
    UT_ASSERT_MSG(strstr(detail, "128") != NULL,
                  "the size refusal does not say what the limit is: %s",
                  detail);

    /* A range refusal says the number that was asked for. */
    slGlobalStr(L, "range", detail, sizeof(detail));
    UT_ASSERT_MSG(strstr(detail, "99") != NULL,
                  "the range refusal does not say what was asked for: %s",
                  detail);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── The state is asked before the row's own checks ───────────────── */

/* The rule that produced two wrong expectations in this file, written down.
 * A row with a state guard answers the state first, whatever its own
 * arguments would have said: a seat that really is taken, a team that really
 * exists, and the answer is still that the round is in the wrong place for
 * this row. A row that guards no state — the pill, base, entity and map
 * lists belong to the map rather than to the round — works in both.
 *
 * Run from the lobby refusal case below rather than registered as one of its
 * own, since it is the same subject and needs the same two sims. */
/* Asking for a seat without the bot in it, through the binding rather than
 * through the op underneath it.
 *
 * The seat is the one thing a script can ask for that it cannot see the
 * result of anywhere else: game.lobby_slot is the only read that carries
 * fielded, and until a seat could sit a round out that field answered true
 * for every seat that existed. This is what proves the value a script reads
 * is the seat's own.
 *
 * No brain path is set on the fixture and none is needed — a seat with
 * nobody in it loads none. */
static int slUnfieldedSeat(void) {
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    ServerSim       *lobby;
    char             err[512];
    long             seat;

    lobby = slSeatedLobbySim();
    if (lobby == NULL) UT_FAIL("could not build a lobby");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, lobby, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L,
                        "seat = game.lobby_add_bot({ team = 3,"
                        " fielded = false })\n"
                        "row = seat and game.lobby_slot(seat) or nil\n"
                        "row_connected = row ~= nil and row.connected\n"
                        "row_bot = row ~= nil and row.bot\n"
                        "row_fielded = row ~= nil and row.fielded\n"
                        "row_team = row ~= nil and row.team or -1\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    /* The seat number, not nil: this used to be a refusal. */
    UT_ASSERT_MSG(!slGlobalIsNil(L, "seat"),
                  "asking for a seat with nobody in it was refused");
    seat = slGlobalInt(L, "seat");
    UT_ASSERT_MSG(seat > 0 && seat < MAX_TANKS,
                  "the add answered seat %ld, and slot 0 holds the human",
                  seat);

    /* And it reads back as what it is. */
    UT_ASSERT_MSG(!slGlobalIsNil(L, "row"),
                  "seat %ld answered no lobby slot", seat);
    UT_ASSERT_MSG(slGlobalBool(L, "row_connected"),
                  "seat %ld does not read as connected", seat);
    UT_ASSERT_MSG(slGlobalBool(L, "row_bot"),
                  "seat %ld does not read as a bot", seat);
    UT_ASSERT_MSG(!slGlobalBool(L, "row_fielded"),
                  "seat %ld reads as fielded, so the read row is still "
                  "answering true for every seat", seat);
    UT_ASSERT_MSG(slGlobalInt(L, "row_team") == 3,
                  "seat %ld is on team %ld, expected 3", seat,
                  slGlobalInt(L, "row_team"));

    lua_close(L);
    serverSimDestroy(lobby);
    return 0;
}

static int slStatePrecedence(void) {
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    char             code[64];
    ServerSim       *lobby;
    ServerSim       *round;

    /* In a lobby: the in-round rows, asked about a seat a person is really
       sitting in, answer the state rather than the seat. */
    lobby = slSeatedLobbySim();
    if (lobby == NULL) UT_FAIL("could not build a lobby");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, lobby, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L,
                        "local a, kill = game.kill_tank(0)\n"
                        "local b, mods = game.set_modifiers(0, {})\n"
                        "local c, team = game.set_team(0, 2)\n"
                        "kill_code, mods_code, team_code = kill, mods, team\n"
                        /* The map's own lists answer for themselves in a
                           lobby, because no state guards them. */
                        "pill_ok = game.set_pill_armour(1, 3)\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    slGlobalStr(L, "kill_code", code, sizeof(code));
    UT_ASSERT_MSG(strcmp(code, scenarioLuaResultName(SCN_OP_WRONG_STATE)) == 0,
                  "killing a tank in a lobby answered '%s': the seat is taken, "
                  "so the state is what should have answered", code);
    slGlobalStr(L, "mods_code", code, sizeof(code));
    UT_ASSERT_MSG(strcmp(code, scenarioLuaResultName(SCN_OP_WRONG_STATE)) == 0,
                  "modifiers in a lobby answered '%s'", code);
    slGlobalStr(L, "team_code", code, sizeof(code));
    UT_ASSERT_MSG(strcmp(code, scenarioLuaResultName(SCN_OP_WRONG_STATE)) == 0,
                  "the in-round team row in a lobby answered '%s'", code);

    lua_getglobal(L, "pill_ok");
    UT_ASSERT_MSG(lua_isboolean(L, -1) && lua_toboolean(L, -1),
                  "a pill write in a lobby was refused: the pill list belongs "
                  "to the map, not to the round");
    lua_pop(L, 1);

    lua_close(L);
    serverSimDestroy(lobby);

    /* And the other way: the lobby rows in a running round, asked about the
       same kind of seat and team, answer the state rather than applying. */
    round = ut_make_running_sim("Seat0");
    if (round == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, round, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L,
                        "local a, seat = game.lobby_set_team(0, 2)\n"
                        "local b, gone = game.lobby_remove_bot(0)\n"
                        "seat_code, gone_code = seat, gone\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    slGlobalStr(L, "seat_code", code, sizeof(code));
    UT_ASSERT_MSG(strcmp(code, scenarioLuaResultName(SCN_OP_WRONG_STATE)) == 0,
                  "the lobby team row answered '%s' mid-round: the seat is "
                  "taken and the team is a real one, so the state is what "
                  "should have answered", code);
    slGlobalStr(L, "gone_code", code, sizeof(code));
    UT_ASSERT_MSG(strcmp(code, scenarioLuaResultName(SCN_OP_WRONG_STATE)) == 0,
                  "the lobby removal answered '%s' mid-round, where a person "
                  "in the seat would otherwise have answered", code);

    lua_close(L);
    serverSimDestroy(round);
    return 0;
}

/* ── 16. A start number no start could have ───────────────────────── */

/* The two rows that take a start read either a start the map could have or
 * the absence of one, and nothing in between. -1, 1e300, a fraction and a
 * NaN all used to reach the payload as "no start named", so a script asking
 * for a start that cannot exist was handed the one the engine would have
 * picked and told nothing.
 *
 * Zero is where the two rows differ. spawn_bot reads a start of zero as no
 * start named, which is what a table leaving the field out comes to;
 * teleport_to_start has the argument's own absence for that, so a zero
 * written there is a number that names no start. */
static const SlRefusal kSlTeleportStarts[] = {
    { "a start before the first",
      "res, code, detail = game.teleport_to_start(0, -1)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "start zero, which numbers no start",
      "res, code, detail = game.teleport_to_start(0, 0)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a start past every index",
      "res, code, detail = game.teleport_to_start(0, 1e300)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a start that is no number at all",
      "res, code, detail = game.teleport_to_start(0, 0/0)\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a start with a fraction in it",
      "res, code, detail = game.teleport_to_start(0, 1.5)\n",
      SCN_OP_NO_SUCH_ITEM },
};

int run_scenario_lua_teleport_start_refuses_bad_index(void) {
    ServerSim       *sim;
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    char             detail[256];
    char             code[64];
    int              r;

    r = slRefusals(kSlTeleportStarts,
                   sizeof(kSlTeleportStarts) / sizeof(kSlTeleportStarts[0]),
                   true);
    if (r != 0) {
        return r;
    }

    sim = ut_make_running_sim("Seat0");
    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L,
                        "local a, b, c = game.teleport_to_start(0, -1)\n"
                        "named = c\n"
                        /* The documented way of leaving the pick to the
                           engine, which is no n at all. */
                        "local d, e = game.teleport_to_start(0)\n"
                        "absent = e or \"\"\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    slGlobalStr(L, "named", detail, sizeof(detail));
    UT_ASSERT_MSG(strstr(detail, "-1") != NULL,
                  "the refusal does not say which start was asked for: %s",
                  detail);

    slGlobalStr(L, "absent", code, sizeof(code));
    UT_ASSERT_MSG(
        strcmp(code, scenarioLuaResultName(SCN_OP_NO_SUCH_ITEM)) != 0,
        "teleport_to_start with no n answered '%s': the absent argument is "
        "how a script asks the engine to pick", code);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 17. And the same number in a spawn's table ───────────────────── */

static const SlRefusal kSlSpawnStarts[] = {
    { "a bot on a start before the first",
      "res, code, detail = game.spawn_bot({ team = 2, start = -1 })\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a bot on a start past every index",
      "res, code, detail = game.spawn_bot({ team = 2, start = 1e300 })\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a bot on a start that is no number at all",
      "res, code, detail = game.spawn_bot({ team = 2, start = 0/0 })\n",
      SCN_OP_NO_SUCH_ITEM },
    { "a bot on a start with a fraction in it",
      "res, code, detail = game.spawn_bot({ team = 2, start = 1.5 })\n",
      SCN_OP_NO_SUCH_ITEM },
};

int run_scenario_lua_spawn_bot_refuses_bad_start(void) {
    ServerSim       *sim;
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    char             detail[256];
    char             code[64];
    int              r;

    r = slRefusals(kSlSpawnStarts,
                   sizeof(kSlSpawnStarts) / sizeof(kSlSpawnStarts[0]), true);
    if (r != 0) {
        return r;
    }

    sim = ut_make_running_sim("Seat0");
    if (sim == NULL) UT_FAIL("could not build a running sim");
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L,
                        "local a, b, c = game.spawn_bot({ team = 2,"
                        " start = -1 })\n"
                        "named = c\n"
                        /* The two documented ways of leaving the pick to the
                           engine: the field at zero, and no field. */
                        "local d, e = game.spawn_bot({ team = 2, start = 0 })\n"
                        "zero = e or \"\"\n"
                        "local f, g = game.spawn_bot({ team = 2 })\n"
                        "absent = g or \"\"\n",
                        err, sizeof(err)),
                  "the chunk would not run: %s", err);

    slGlobalStr(L, "named", detail, sizeof(detail));
    UT_ASSERT_MSG(strstr(detail, "-1") != NULL,
                  "the refusal does not say which start was asked for: %s",
                  detail);

    slGlobalStr(L, "zero", code, sizeof(code));
    UT_ASSERT_MSG(
        strcmp(code, scenarioLuaResultName(SCN_OP_NO_SUCH_ITEM)) != 0,
        "a spawn with start = 0 answered '%s': zero is how a table asks the "
        "engine to pick", code);
    slGlobalStr(L, "absent", code, sizeof(code));
    UT_ASSERT_MSG(
        strcmp(code, scenarioLuaResultName(SCN_OP_NO_SUCH_ITEM)) != 0,
        "a spawn with no start answered '%s'", code);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}

/* ── 18. game_type answers a game the loadout table holds ─────────── */

/* A scripted round's own game type is gameScripted, and a script that feeds
   game.game_type() into a spawn's loadout would have that word refused: the
   loadout table holds "open", "tournament" and "strict" and nothing else. So
   the row resolves, and a round that declared no base game reads "strict",
   which is what such a round plays. With a base game declared the row reads
   that instead. */
int run_scenario_lua_game_type_resolves_scripted(void) {
    ServerSim       *sim = ut_make_running_sim("Seat0");
    ScenarioManifest m;
    ScnLuaCtx        ctx;
    lua_State       *L;
    char             err[512];
    char             word[64];

    if (sim == NULL) UT_FAIL("could not build a running sim");
    serverSimSetGameType(sim, gameScripted);
    memset(&m, 0, sizeof(m));
    L = slVm(&ctx, sim, &m);
    UT_ASSERT(L != NULL);

    UT_ASSERT_MSG(slRun(L, "word = game.game_type()\n", err, sizeof(err)),
                  "the fixture would not run: %s", err);
    slGlobalStr(L, "word", word, sizeof(word));
    UT_ASSERT_MSG(strcmp(word, "strict") == 0,
                  "a scripted round declaring no game read '%s'", word);

    /* The base game the lobby template carried reaches the row the same way
       it reaches the loadout and the start picker. */
    serverSimGetGameSim(sim)->scenarioBaseGame = gameStrictTournament;
    UT_ASSERT_MSG(slRun(L, "word = game.game_type()\n", err, sizeof(err)),
                  "the fixture would not run: %s", err);
    slGlobalStr(L, "word", word, sizeof(word));
    UT_ASSERT_MSG(strcmp(word, "strict") == 0,
                  "a scripted round declaring strict read '%s'", word);

    /* And a table that names one of its own is still what the script reads,
       resolve or no resolve. */
    snprintf(m.game, sizeof(m.game), "tournament");
    UT_ASSERT_MSG(slRun(L, "word = game.game_type()\n", err, sizeof(err)),
                  "the fixture would not run: %s", err);
    slGlobalStr(L, "word", word, sizeof(word));
    UT_ASSERT_MSG(strcmp(word, "tournament") == 0,
                  "a table naming tournament read '%s'", word);

    lua_close(L);
    serverSimDestroy(sim);
    return 0;
}
