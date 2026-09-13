/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Lua
 *Filename:      scenario_lua.c
 *Author:        John Morrison
 *Purpose:
 *  The marshalling between a script's tables and the typed
 *  reads the sim publishes, the way transport_control_codec.c
 *  marshals between the wire and the same structs. One
 *  table-driven registry, one row per script-visible
 *  function, and the row carries the line a document is
 *  written from.
 *
 *  What every row holds to:
 *
 *  A read answers a table, or nil for an entity that is not
 *  there, and never raises. An index outside the list, an
 *  empty seat and a slot whose item has been removed all
 *  answer nil, so a script walks a list with
 *  `for n = 1, game.num_pills()` and skips the nils: the
 *  counts are slot counts rather than live ones, and a
 *  removed pill keeps its index so the ones above it keep
 *  their numbers.
 *
 *  A shape error raises. A missing or mistyped argument, or
 *  a word that names no kind, is a luaL_error naming the
 *  argument; the host's pcall around every hook and policy
 *  call catches it and counts it toward SCN_ERROR_LIMIT.
 *
 *  The two Lua builds must answer the same thing. The host
 *  links LuaJIT or PUC 5.4 depending on the build and they
 *  differ in number model, so this file converts its own
 *  numbers rather than leaning on lua_tointeger, and the
 *  one row that answers an array of names orders it here
 *  rather than passing on the order a table iterated in.
 *
 *  This file compiles under the scenario_host profile: it
 *  sees src/bolo/public/ and src/bolo/scenario_api/, and
 *  nothing under src/bolo/internal/.
 *********************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#include "platform_types.h"       /* BOLO_STATIC_ASSERT */
#include "global.h"               /* the terrain codes, NEUTRAL, MAX_TANKS */
#include "gametype.h"             /* gameOpen and its siblings */
#include "server_sim.h"           /* the read accessors and their structs */
#include "scenario_defs.h"        /* SCN_RULE_LIST, SCN_RULE_COUNT */
#include "server_sim_scenario.h"  /* serverSimGetScenarioRule */

#include "scenario_host.h"
#include "scenario_manifest.h"
#include "scenario_lua.h"

/* The window an argument is read in. A number outside it cannot be
 * converted to an integer at all — that conversion is undefined rather than
 * wrong — so both ends are the answer for anything past them. */
#define SCN_LUA_INT_MIN (-2147483647 - 1)
#define SCN_LUA_INT_MAX 2147483647

/* What an item index becomes when it is one no list could hold. Past every
 * entity list a map has, so an op built from it is refused rather than
 * naming an item that does exist. */
#define SCN_LUA_NO_ITEM 255

/* ── The rule names ───────────────────────────────────────────────── */

/* A rule's name in a script is its name in the rule list, so the list is the
 * only place the spelling exists. A rule added there is resolvable here with
 * nothing to update. */
static const char *const kScnRuleNames[] = {
#define SCN_RULE_NAME_ROW(name) #name,
    SCN_RULE_LIST(SCN_RULE_NAME_ROW)
#undef SCN_RULE_NAME_ROW
};

BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnRuleNames) / sizeof(kScnRuleNames[0])) == (int)SCN_RULE_COUNT,
    rule_name_table_is_the_whole_rule_list);

int scenarioLuaRuleIndex(const char *name) {
    int i;

    if (name == NULL) {
        return -1;
    }
    for (i = 0; i < (int)SCN_RULE_COUNT; i++) {
        if (strcmp(kScnRuleNames[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

const char *scenarioLuaRuleName(int rule) {
    if (rule < 0 || rule >= (int)SCN_RULE_COUNT) {
        return "";
    }
    return kScnRuleNames[rule];
}

/* ── The three index rules ────────────────────────────────────────── */

BYTE scenarioLuaIndexToRead(lua_Integer n) {
    if (n < 1 || n > 255) {
        return 0;
    }
    return (BYTE)n;
}

BYTE scenarioLuaIndexToOp(lua_Integer n) {
    if (n < 1 || n > 255) {
        return SCN_LUA_NO_ITEM;
    }
    return (BYTE)(n - 1);
}

lua_Integer scenarioLuaIndexToScript(int n) {
    return (lua_Integer)n + 1;
}

/* ── Reading an argument ──────────────────────────────────────────── */

/* What the rows read through. The context is the upvalue every row is
 * closed over, so a row needs nothing from the registry and two VMs on one
 * sim keep their own. */
static const ScnLuaCtx *scnCtx(lua_State *L) {
    return (const ScnLuaCtx *)lua_touserdata(L, lua_upvalueindex(1));
}

/* A number argument, as a whole number. A missing or mistyped one raises
 * naming the argument. A value past either end of the window, and a NaN,
 * come back as that end, so the range test at the call site turns them down
 * rather than a cast wrapping them onto a square or an index that exists.
 *
 * The truncation is this file's own rather than lua_tointeger's: PUC 5.4
 * reads 1.5 as no integer at all and LuaJIT reads it as 1, and a row answers
 * the same under both builds. */
static lua_Integer scnArgInt(lua_State *L, int idx, const char *name) {
    lua_Number v;

    if (lua_type(L, idx) != LUA_TNUMBER) {
        return (lua_Integer)luaL_argerror(
            L, idx,
            lua_pushfstring(L, "%s must be a number, got %s", name,
                            luaL_typename(L, idx)));
    }
    v = lua_tonumber(L, idx);
    /* Written as a negated in-range test, so a NaN takes the first branch
       rather than falling through both. */
    if (!(v >= (lua_Number)SCN_LUA_INT_MIN)) {
        return (lua_Integer)SCN_LUA_INT_MIN;
    }
    if (v > (lua_Number)SCN_LUA_INT_MAX) {
        return (lua_Integer)SCN_LUA_INT_MAX;
    }
    return (lua_Integer)v;
}

/* A string argument. A number is not one: Lua would coerce it, the two
 * builds spell a coerced number differently, and no row here wants a number
 * where it asks for a name. */
static const char *scnArgStr(lua_State *L, int idx, const char *name) {
    if (lua_type(L, idx) != LUA_TSTRING) {
        luaL_argerror(L, idx,
                      lua_pushfstring(L, "%s must be a string, got %s", name,
                                      luaL_typename(L, idx)));
        return NULL;   /* luaL_argerror does not return */
    }
    return lua_tostring(L, idx);
}

/* ── Pushing ──────────────────────────────────────────────────────── */

static void scnSetInt(lua_State *L, const char *key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

static void scnSetBool(lua_State *L, const char *key, bool v) {
    lua_pushboolean(L, v ? 1 : 0);
    lua_setfield(L, -2, key);
}

static void scnSetStr(lua_State *L, const char *key, const char *v) {
    lua_pushstring(L, (v != NULL) ? v : "");
    lua_setfield(L, -2, key);
}

/* A value that may or may not be whole. A whole one is pushed as an integer
 * so the two builds write it the same way: PUC 5.4 writes the float 1.0 as
 * "1.0" and LuaJIT writes it as "1", and a message a script builds out of a
 * rule should not depend on which one the server was built against. */
static void scnPushNumber(lua_State *L, double v) {
    if (v >= (double)SCN_LUA_INT_MIN && v <= (double)SCN_LUA_INT_MAX &&
        v == (double)(lua_Integer)v) {
        lua_pushinteger(L, (lua_Integer)v);
        return;
    }
    lua_pushnumber(L, (lua_Number)v);
}

/* ── Words for the enumerations ───────────────────────────────────── */

static const char *scnGameTypeWord(gameType g) {
    switch (g) {
        case gameOpen:             return "open";
        case gameTournament:       return "tournament";
        case gameStrictTournament: return "strict";
    }
    return "open";
}

static const char *scnBuilderStateWord(BuilderState s) {
    switch (s) {
        case builderStateInTank:      return "in_tank";
        case builderStateGoing:       return "going";
        case builderStateReturning:   return "returning";
        case builderStateParachuting: return "parachuting";
        case builderStateDead:        return "dead";
    }
    return "in_tank";
}

/* The job a builder is on, or NULL for one on none. The words are the
 * builder actions an order is given in, so a job read off a builder is a
 * word an order takes. */
static const char *scnBuilderJobWord(BuilderJob j) {
    switch (j) {
        case builderJobTrees:    return "trees";
        case builderJobRoad:     return "road";
        case builderJobBuilding: return "building";
        case builderJobPill:     return "pill";
        case builderJobMine:     return "mine";
        case builderJobBoat:     return "boat";
        case builderJobNone:     return NULL;
    }
    return NULL;
}

/* ── The counts and the clock ─────────────────────────────────────── */

static int scnLuaTick(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)serverSimGetTick(scnCtx(L)->sim));
    return 1;
}

static int scnLuaMaxTanks(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)MAX_TANKS);
    return 1;
}

static int scnLuaNumPlayers(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)serverSimGetNumFielded(scnCtx(L)->sim));
    return 1;
}

static int scnLuaNumHumans(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)serverSimGetNumHumans(scnCtx(L)->sim));
    return 1;
}

/* Seats rather than tanks: a seat sitting the round out still belongs to its
 * team, which is what a script asking how big the other side is wants. */
static int scnLuaTeamSize(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);
    lua_Integer      t = scnArgInt(L, 1, "t");
    lua_Integer      count = 0;
    BYTE             i;

    for (i = 0; i < MAX_TANKS; i++) {
        ServerSimRosterSlot slot;
        if (serverSimGetRosterSlot(c->sim, i, &slot) &&
            (lua_Integer)slot.team == t) {
            count++;
        }
    }
    lua_pushinteger(L, count);
    return 1;
}

/* The game type the humans are playing under. A scenario that asks for one
 * in its table is what the round runs, so that is what a script reads back;
 * a scenario that asks for none reads the sim's own. */
static int scnLuaGameType(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);

    if (c->manifest != NULL && c->manifest->game[0] != '\0') {
        lua_pushstring(L, c->manifest->game);
        return 1;
    }
    lua_pushstring(L, scnGameTypeWord(serverSimGetGameType(c->sim)));
    return 1;
}

/* ── The map ──────────────────────────────────────────────────────── */

static int scnLuaMapName(lua_State *L) {
    lua_pushstring(L, serverSimGetMapName(scnCtx(L)->sim));
    return 1;
}

/* A square the map does not have is nil rather than a code: every square
 * from 0 to 255 is on the map, so a number outside that is a square the
 * caller worked out wrongly rather than an empty one. */
static int scnLuaMapTile(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);
    lua_Integer      x = scnArgInt(L, 1, "x");
    lua_Integer      y = scnArgInt(L, 2, "y");

    if (x < 0 || x > 255 || y < 0 || y > 255) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L,
                    (lua_Integer)serverSimGetMapTerrain(c->sim, (BYTE)x,
                                                        (BYTE)y));
    return 1;
}

/* A square off the map holds no mine, so this answers false rather than nil:
 * the row is a question with a yes and a no. */
static int scnLuaIsMine(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);
    lua_Integer      x = scnArgInt(L, 1, "x");
    lua_Integer      y = scnArgInt(L, 2, "y");

    if (x < 0 || x > 255 || y < 0 || y > 255) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, serverSimMapIsMine(c->sim, (BYTE)x, (BYTE)y) ? 1 : 0);
    return 1;
}

/* Every square in one string, for a script that reads the whole map at setup
 * rather than making 65,536 calls. The buffer is Lua's own, so the string
 * push cannot leave it behind. */
static int scnLuaTerrain(lua_State *L) {
    const ScnLuaCtx *c   = scnCtx(L);
    BYTE            *buf = (BYTE *)lua_newuserdata(L, SERVER_SIM_TERRAIN_BYTES);

    if (!serverSimGetMapTerrainBuffer(c->sim, buf, SERVER_SIM_TERRAIN_BYTES)) {
        /* The copy turns down a buffer shorter than the map, which this one
           is not. A read that answers nothing answers nil rather than a
           half-filled string. */
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buf, SERVER_SIM_TERRAIN_BYTES);
    return 1;
}

/* ── Pills, bases and starts ──────────────────────────────────────── */

static int scnLuaNumPills(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)serverSimGetPillCount(scnCtx(L)->sim));
    return 1;
}

static int scnLuaPill(lua_State *L) {
    const ScnLuaCtx  *c = scnCtx(L);
    ServerSimPillInfo info;
    BYTE              n = scenarioLuaIndexToRead(scnArgInt(L, 1, "n"));

    if (n == 0 || !serverSimGetPillInfo(c->sim, n, &info) || !info.active) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    scnSetInt(L, "x", info.x);
    scnSetInt(L, "y", info.y);
    scnSetInt(L, "owner", info.owner);
    scnSetInt(L, "armour", info.armour);
    scnSetInt(L, "speed", info.speed);
    scnSetBool(L, "in_tank", info.in_tank);
    return 1;
}

static int scnLuaNumBases(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)serverSimGetBaseCount(scnCtx(L)->sim));
    return 1;
}

static int scnLuaBase(lua_State *L) {
    const ScnLuaCtx  *c = scnCtx(L);
    ServerSimBaseInfo info;
    BYTE              n = scenarioLuaIndexToRead(scnArgInt(L, 1, "n"));

    if (n == 0 || !serverSimGetBaseInfo(c->sim, n, &info) || !info.active) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    scnSetInt(L, "x", info.x);
    scnSetInt(L, "y", info.y);
    scnSetInt(L, "owner", info.owner);
    scnSetInt(L, "armour", info.armour);
    scnSetInt(L, "shells", info.shells);
    scnSetInt(L, "mines", info.mines);
    return 1;
}

static int scnLuaNumStarts(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)serverSimGetStartCount(scnCtx(L)->sim));
    return 1;
}

/* serverSimGetStartInfo rather than serverSimGetStart, which Appendix E
 * names: the two read the same start and only the first carries the flag
 * that tells a live slot from one a removal left behind, which is what the
 * nil above is. */
static int scnLuaStart(lua_State *L) {
    const ScnLuaCtx   *c = scnCtx(L);
    ServerSimStartInfo info;
    BYTE               n = scenarioLuaIndexToRead(scnArgInt(L, 1, "n"));

    if (n == 0 || !serverSimGetStartInfo(c->sim, n, &info) || !info.active) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    scnSetInt(L, "x", info.x);
    scnSetInt(L, "y", info.y);
    scnSetInt(L, "dir", info.dir);
    return 1;
}

/* ── The players ──────────────────────────────────────────────────── */

/* Players are 0-based slots and convert nowhere, so a slot is range-checked
 * here rather than run through one of the index helpers. */
static bool scnSlotOf(lua_State *L, int idx, const char *name, BYTE *out) {
    lua_Integer p = scnArgInt(L, idx, name);

    if (p < 0 || p >= MAX_TANKS) {
        return false;
    }
    *out = (BYTE)p;
    return true;
}

/* A seat with nobody in it, and one whose tank is not in the world — the
 * countdown before a round and the wait after a death — both answer nil. A
 * tank that is in the world and dying answers a table with dead true. */
static int scnLuaTank(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);
    TankInfo         info;
    BYTE             p;

    if (!scnSlotOf(L, 1, "p", &p) ||
        !serverSimGetTankInfo(c->sim, p, &info) || !info.has_tank) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    scnSetInt(L, "mx", info.map_x);
    scnSetInt(L, "my", info.map_y);
    scnSetInt(L, "wx", info.world_x);
    scnSetInt(L, "wy", info.world_y);
    scnSetInt(L, "armour", info.armour);
    scnSetInt(L, "shells", info.shells);
    scnSetInt(L, "mines", info.mines);
    scnSetInt(L, "trees", info.trees);
    scnSetInt(L, "pills", info.pills);
    /* The full 0-255 facing, which is the one a teleport takes back. */
    scnSetInt(L, "dir", info.dir256);
    scnSetBool(L, "boat", info.on_boat);
    scnSetBool(L, "dead", !info.alive);
    scnSetStr(L, "name", info.name);
    scnSetBool(L, "bot", info.is_bot);
    scnSetInt(L, "kills", info.kills);
    scnSetInt(L, "deaths", info.deaths);

    lua_newtable(L);
    scnSetInt(L, "speed", info.mods.speed);
    scnSetInt(L, "accel", info.mods.accel);
    scnSetInt(L, "turn", info.mods.turn);
    scnSetInt(L, "reload", info.mods.reload);
    scnSetInt(L, "dealt", info.mods.dealt);
    scnSetInt(L, "taken", info.mods.taken);
    lua_setfield(L, -2, "mods");
    return 1;
}

/* The builder's square is tracked while he is out of the tank; in_tank he is
 * wherever his tank is and the engine leaves the fields at zero, which is
 * why the state is the first thing the table carries. */
static int scnLuaBuilder(lua_State *L) {
    const ScnLuaCtx     *c = scnCtx(L);
    ServerSimBuilderInfo info;
    const char          *job;
    BYTE                 p;

    if (!scnSlotOf(L, 1, "p", &p) ||
        !serverSimGetBuilderInfo(c->sim, p, &info)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    scnSetStr(L, "state", scnBuilderStateWord(info.state));
    scnSetInt(L, "mx", info.map_x);
    scnSetInt(L, "my", info.map_y);
    scnSetInt(L, "wx", info.world_x);
    scnSetInt(L, "wy", info.world_y);
    scnSetInt(L, "trees", info.trees);
    scnSetInt(L, "mines", info.mines);
    job = scnBuilderJobWord(info.job);
    if (job != NULL) {
        scnSetStr(L, "job", job);
    }
    return 1;
}

static int scnLuaLobbySlot(lua_State *L) {
    const ScnLuaCtx    *c = scnCtx(L);
    ServerSimRosterSlot slot;
    BYTE                p;

    if (!scnSlotOf(L, 1, "p", &p) ||
        !serverSimGetRosterSlot(c->sim, p, &slot)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    scnSetBool(L, "connected", slot.connected);
    scnSetBool(L, "bot", slot.is_bot);
    scnSetInt(L, "team", slot.team);
    scnSetStr(L, "name", slot.name);
    scnSetBool(L, "ready", slot.ready);
    scnSetBool(L, "fielded", slot.fielded);
    scnSetBool(L, "alive", slot.alive);
    return 1;
}

/* ── The rules ────────────────────────────────────────────────────── */

/* A name that spells no rule raises rather than answering nil: every other
 * nil here is an entity that is not there, and a rule that is not in the
 * table is a misspelling in the script. */
static int scnLuaRule(lua_State *L) {
    const ScnLuaCtx *c    = scnCtx(L);
    const char      *name = scnArgStr(L, 1, "name");
    int              rule = scenarioLuaRuleIndex(name);
    double           v    = 0.0;

    if (rule < 0) {
        return luaL_error(L, "no rule is named '%s'", name);
    }
    if (!serverSimGetScenarioRule(c->sim, (uint16_t)rule, &v)) {
        /* The index came out of the list the sim reads by, so this is the
           sim answering that it has no table rather than a bad name. */
        lua_pushnil(L);
        return 1;
    }
    scnPushNumber(L, v);
    return 1;
}

/* ── The manifest: tags and regions ───────────────────────────────── */

/* The kinds a tag can sit on, in the order the rows walk them. */
typedef struct {
    const char *word;
    size_t      offset;    /* the tag array on ScenarioManifest */
    int         maxEntity; /* the highest index that array holds */
} ScnLuaTagKind;

static const ScnLuaTagKind kTagKinds[] = {
    { "pill",  offsetof(ScenarioManifest, pillTags),  MAX_PILLS  },
    { "base",  offsetof(ScenarioManifest, baseTags),  MAX_BASES  },
    { "start", offsetof(ScenarioManifest, startTags), MAX_STARTS },
};

static const ScnManifestTags *scnTagArray(const ScenarioManifest *m,
                                          const ScnLuaTagKind *kind) {
    return (const ScnManifestTags *)(const void *)((const char *)m +
                                                   kind->offset);
}

static const ScnLuaTagKind *scnTagKindOf(const char *word) {
    size_t i;

    for (i = 0; i < sizeof(kTagKinds) / sizeof(kTagKinds[0]); i++) {
        if (strcmp(kTagKinds[i].word, word) == 0) {
            return &kTagKinds[i];
        }
    }
    return NULL;
}

static bool scnHasTag(const ScnManifestTags *t, const char *tag) {
    int i;

    for (i = 0; i < (int)t->count; i++) {
        if (strcmp(t->tag[i], tag) == 0) {
            return true;
        }
    }
    return false;
}

/* The tags on one entity. An index the map cannot hold answers nil, as every
 * other read of an entity that is not there does; an entity that is there
 * and carries none answers an empty array. */
static int scnLuaTags(lua_State *L) {
    const ScnLuaCtx     *c    = scnCtx(L);
    const char          *word = scnArgStr(L, 1, "kind");
    const ScnLuaTagKind *kind = scnTagKindOf(word);
    lua_Integer          n;
    const ScnManifestTags *arr;
    int                  i;

    if (kind == NULL) {
        return luaL_error(L, "kind is '%s' and not \"pill\", \"base\" or "
                             "\"start\"", word);
    }
    n = scnArgInt(L, 2, "n");
    if (c->manifest == NULL || n < 1 || n > kind->maxEntity) {
        lua_pushnil(L);
        return 1;
    }
    arr = scnTagArray(c->manifest, kind);
    lua_newtable(L);
    for (i = 0; i < (int)arr[n].count; i++) {
        lua_pushstring(L, arr[n].tag[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* Everything carrying a tag. The order is pills, then bases, then starts,
 * each by ascending index — an order stated here rather than taken from a
 * table's own, which the two Lua builds iterate differently. */
static int scnLuaTagged(lua_State *L) {
    const ScnLuaCtx     *c   = scnCtx(L);
    const char          *tag = scnArgStr(L, 1, "tag");
    const ScnLuaTagKind *only = NULL;
    int                  out = 0;
    size_t               k;

    if (!lua_isnoneornil(L, 2)) {
        const char *word = scnArgStr(L, 2, "kind");
        only = scnTagKindOf(word);
        if (only == NULL) {
            return luaL_error(L, "kind is '%s' and not \"pill\", \"base\" or "
                                 "\"start\"", word);
        }
    }

    lua_newtable(L);
    if (c->manifest == NULL) {
        return 1;
    }
    for (k = 0; k < sizeof(kTagKinds) / sizeof(kTagKinds[0]); k++) {
        const ScnLuaTagKind   *kind = &kTagKinds[k];
        const ScnManifestTags *arr;
        int                    n;

        if (only != NULL && only != kind) {
            continue;
        }
        arr = scnTagArray(c->manifest, kind);
        for (n = 1; n <= kind->maxEntity; n++) {
            if (!scnHasTag(&arr[n], tag)) {
                continue;
            }
            out++;
            if (only != NULL) {
                lua_pushinteger(L, (lua_Integer)n);
            } else {
                lua_newtable(L);
                scnSetStr(L, "kind", kind->word);
                scnSetInt(L, "n", n);
            }
            lua_rawseti(L, -2, out);
        }
    }
    return 1;
}

static const ScnManifestRegion *scnRegionOf(const ScenarioManifest *m,
                                            const char *name) {
    int i;

    if (m == NULL) {
        return NULL;
    }
    for (i = 0; i < (int)m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) {
            return &m->regions[i];
        }
    }
    return NULL;
}

static int scnLuaRegion(lua_State *L) {
    const ScnLuaCtx         *c    = scnCtx(L);
    const char              *name = scnArgStr(L, 1, "name");
    const ScnManifestRegion *r    = scnRegionOf(c->manifest, name);

    if (r == NULL) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    scnSetInt(L, "x", r->x);
    scnSetInt(L, "y", r->y);
    scnSetInt(L, "w", r->w);
    scnSetInt(L, "h", r->h);
    return 1;
}

/* The names, in name order. The manifest holds regions in whatever order the
 * sidecar's table iterated in, which is not the same order under the two Lua
 * builds, so the array a script walks is sorted here and a script reading it
 * gets the same round twice. */
static int scnLuaRegions(lua_State *L) {
    const ScnLuaCtx        *c = scnCtx(L);
    const ScenarioManifest *m = c->manifest;
    uint8_t                 order[SCN_REGIONS_MAX];
    int                     n = (m != NULL) ? (int)m->numRegions : 0;
    int                     i, j;

    for (i = 0; i < n; i++) {
        order[i] = (uint8_t)i;
    }
    for (i = 1; i < n; i++) {
        uint8_t take = order[i];
        j = i - 1;
        while (j >= 0 && strcmp(m->regions[order[j]].name,
                                m->regions[take].name) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = take;
    }

    lua_newtable(L);
    for (i = 0; i < n; i++) {
        lua_pushstring(L, m->regions[order[i]].name);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* A square inside a named rectangle. A name no region carries answers false:
 * a square is not inside a rectangle that was never declared, and the row is
 * a question rather than a read of an entity. */
static int scnLuaInRegion(lua_State *L) {
    const ScnLuaCtx         *c    = scnCtx(L);
    const char              *name = scnArgStr(L, 1, "name");
    lua_Integer              mx   = scnArgInt(L, 2, "mx");
    lua_Integer              my   = scnArgInt(L, 3, "my");
    const ScnManifestRegion *r    = scnRegionOf(c->manifest, name);
    bool                     in;

    if (r == NULL || r->w == 0 || r->h == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    in = mx >= (lua_Integer)r->x && mx < (lua_Integer)r->x + (lua_Integer)r->w &&
         my >= (lua_Integer)r->y && my < (lua_Integer)r->y + (lua_Integer)r->h;
    lua_pushboolean(L, in ? 1 : 0);
    return 1;
}

/* ── The registry ─────────────────────────────────────────────────── */

/* One row per script-visible function. The doc column is what
 * docs/SCENARIO_API.md and the editor's completion list are written from, so
 * a row added without one is a row nothing can describe. */
static const ScnLuaRow kScnLuaRows[] = {
    { "tick", scnLuaTick,
      "tick() — the tick the round is on." },
    { "max_tanks", scnLuaMaxTanks,
      "max_tanks() — how many seats a game has." },
    { "num_players", scnLuaNumPlayers,
      "num_players() — how many seats are playing the round, bots included." },
    { "num_humans", scnLuaNumHumans,
      "num_humans() — how many of the players are people." },
    { "team_size", scnLuaTeamSize,
      "team_size(t) — how many seats sit on team t, playing the round or "
      "not." },
    { "game_type", scnLuaGameType,
      "game_type() — the rules the humans play under: \"open\", "
      "\"tournament\" or \"strict\"." },
    { "map_name", scnLuaMapName,
      "map_name() — what the map is called." },
    { "map_tile", scnLuaMapTile,
      "map_tile(x, y) — the terrain code at a square, or nil for a square "
      "off the map." },
    { "is_mine", scnLuaIsMine,
      "is_mine(x, y) — whether a square holds a mine." },
    { "terrain", scnLuaTerrain,
      "terrain() — every square as one 65,536-byte string, the square at "
      "(x, y) at byte y * 256 + x + 1." },
    { "num_pills", scnLuaNumPills,
      "num_pills() — how many pill slots the map has, live or not." },
    { "pill", scnLuaPill,
      "pill(n) — pill n as { x, y, owner, armour, speed, in_tank }, or nil "
      "for a slot no live pill holds." },
    { "num_bases", scnLuaNumBases,
      "num_bases() — how many base slots the map has, live or not." },
    { "base", scnLuaBase,
      "base(n) — base n as { x, y, owner, armour, shells, mines }, or nil "
      "for a slot no live base holds." },
    { "num_starts", scnLuaNumStarts,
      "num_starts() — how many start slots the map has, live or not." },
    { "start", scnLuaStart,
      "start(n) — start n as { x, y, dir }, or nil for a slot no live start "
      "holds." },
    { "tank", scnLuaTank,
      "tank(p) — player p's tank as { mx, my, wx, wy, dir, armour, shells, "
      "mines, trees, pills, boat, dead, name, bot, kills, deaths, mods }, "
      "or nil when the seat is empty or has no tank." },
    { "builder", scnLuaBuilder,
      "builder(p) — player p's builder as { state, mx, my, wx, wy, job, "
      "trees, mines }, or nil when the seat has none." },
    { "lobby_slot", scnLuaLobbySlot,
      "lobby_slot(p) — seat p as { connected, bot, team, name, ready, "
      "fielded, alive }, or nil for an empty seat." },
    { "rule", scnLuaRule,
      "rule(name) — what a gameplay rule is set to; a name that spells no "
      "rule raises." },
    { "tags", scnLuaTags,
      "tags(kind, n) — the tags the scenario put on a \"pill\", \"base\" or "
      "\"start\", as an array of strings." },
    { "tagged", scnLuaTagged,
      "tagged(tag[, kind]) — everything carrying a tag as an array of "
      "{ kind, n }, or of n when a kind is named; pills, then bases, then "
      "starts." },
    { "region", scnLuaRegion,
      "region(name) — a declared rectangle as { x, y, w, h }, or nil when "
      "nothing is declared by that name." },
    { "regions", scnLuaRegions,
      "regions() — the name of every declared region, in name order." },
    { "in_region", scnLuaInRegion,
      "in_region(name, mx, my) — whether a square is inside a declared "
      "region." },
};

static const ScnLuaConst kScnLuaConsts[] = {
    { "api_version", SCENARIO_API_VERSION,
      "api_version — the surface this server implements; a script states "
      "the one it was written against as scenario.api." },
    { "NEUTRAL", NEUTRAL,
      "NEUTRAL — the owner a pill or base carries when no player holds it." },
};

/* The terrain codes by name, so a script compares what map_tile answered
 * against a word rather than against a number of its own. */
static const ScnLuaTerrain kScnLuaTerrain[] = {
    { "deep_sea",      DEEP_SEA     },
    { "building",      BUILDING     },
    { "river",         RIVER        },
    { "swamp",         SWAMP        },
    { "crater",        CRATER       },
    { "road",          ROAD         },
    { "forest",        FOREST       },
    { "rubble",        RUBBLE       },
    { "grass",         GRASS        },
    { "half_building", HALFBUILDING },
    { "boat",          BOAT         },
    { "mine_swamp",    MINE_SWAMP   },
    { "mine_crater",   MINE_CRATER  },
    { "mine_road",     MINE_ROAD    },
    { "mine_forest",   MINE_FOREST  },
    { "mine_rubble",   MINE_RUBBLE  },
    { "mine_grass",    MINE_GRASS   },
};

const ScnLuaRow *scenarioLuaRows(size_t *count) {
    if (count != NULL) {
        *count = sizeof(kScnLuaRows) / sizeof(kScnLuaRows[0]);
    }
    return kScnLuaRows;
}

const ScnLuaConst *scenarioLuaConsts(size_t *count) {
    if (count != NULL) {
        *count = sizeof(kScnLuaConsts) / sizeof(kScnLuaConsts[0]);
    }
    return kScnLuaConsts;
}

const ScnLuaTerrain *scenarioLuaTerrain(size_t *count) {
    if (count != NULL) {
        *count = sizeof(kScnLuaTerrain) / sizeof(kScnLuaTerrain[0]);
    }
    return kScnLuaTerrain;
}

void scenarioLuaInstall(lua_State *L, const ScnLuaCtx *ctx) {
    /* The context reaches a row as its one upvalue rather than through the
       registry: a row then needs no lookup to run, and nothing a script
       leaves in the registry can be taken for it. */
    void  *box = (void *)(uintptr_t)ctx;
    size_t i;

    lua_newtable(L);

    for (i = 0; i < sizeof(kScnLuaRows) / sizeof(kScnLuaRows[0]); i++) {
        lua_pushlightuserdata(L, box);
        lua_pushcclosure(L, kScnLuaRows[i].fn, 1);
        lua_setfield(L, -2, kScnLuaRows[i].name);
    }
    for (i = 0; i < sizeof(kScnLuaConsts) / sizeof(kScnLuaConsts[0]); i++) {
        lua_pushinteger(L, (lua_Integer)kScnLuaConsts[i].value);
        lua_setfield(L, -2, kScnLuaConsts[i].name);
    }

    lua_newtable(L);
    for (i = 0; i < sizeof(kScnLuaTerrain) / sizeof(kScnLuaTerrain[0]); i++) {
        lua_pushinteger(L, (lua_Integer)kScnLuaTerrain[i].code);
        lua_setfield(L, -2, kScnLuaTerrain[i].name);
    }
    lua_setfield(L, -2, "TERRAIN");

    lua_setglobal(L, "game");
}
