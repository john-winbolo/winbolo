/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Lua
 *Filename:      scenario_lua.c
 *Author:        John Morrison
 *Purpose:
 *  The marshalling between a script's tables and the typed
 *  reads the sim publishes and the typed ops it takes, the
 *  way transport_control_codec.c marshals between the wire
 *  and the same structs. One table-driven registry, one row
 *  per script-visible function, and the row carries the line
 *  a document is written from.
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
 *  A write fills one typed payload and hands it to the
 *  funnel. It answers true, or true and "queued" while the
 *  work is still going on, or nil with the refusal's own
 *  name and one sentence carrying the number that mattered.
 *  A refusal is an answer a script tests rather than an
 *  error, so no refusal raises.
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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#include "platform_types.h"       /* BOLO_STATIC_ASSERT */
#include "brain_list.h"           /* brainListResolve — the brain a roster row
                                   * names, against this server's own brains
                                   * directory */
#include "global.h"               /* the terrain codes, NEUTRAL, MAX_TANKS */
#include "gametype.h"             /* gameOpen and its siblings */
#include "client_enums.h"         /* sndEffects — the sound row's words */
#include "server_sim.h"           /* the accessors, and the rule read */
#include "scenario_defs.h"        /* the op payloads */
#include "scenario_panel.h"       /* the display list's primitives, and the
                                   * one writer that turns them into bytes */
#include "sim_rules_names.h"      /* simRulesRuleIndex — a rule by name */
#include "server_sim_scenario.h"  /* serverSimApplyScenarioOp */

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

/* ── What an op answered ──────────────────────────────────────────── */

/* One line per result code. The word a script reads off a refusal and the
 * word a case asserts come from this one list, so the two cannot drift. */
#define SCN_LUA_RESULT_LIST(X)                                               \
    X(SCN_OP_OK)             X(SCN_OP_QUEUED)      X(SCN_OP_UNSUPPORTED)     \
    X(SCN_OP_IN_POLICY)      X(SCN_OP_WRONG_STATE) X(SCN_OP_NO_SUCH_PLAYER)  \
    X(SCN_OP_TANK_DEAD)      X(SCN_OP_IS_HUMAN)    X(SCN_OP_NO_SUCH_ITEM)    \
    X(SCN_OP_BAD_SQUARE)     X(SCN_OP_BAD_TERRAIN) X(SCN_OP_RANGE)           \
    X(SCN_OP_PAIR)           X(SCN_OP_CARRIED)     X(SCN_OP_FULL)            \
    X(SCN_OP_ALREADY)        X(SCN_OP_TOO_BIG)     X(SCN_OP_RATE)            \
    X(SCN_OP_NOT_FOUND)      X(SCN_OP_NO_STOCK)    X(SCN_OP_NO_RUNNER)       \
    X(SCN_OP_BAD_CALL)

static const struct {
    int         result;
    const char *name;
} kScnLuaResults[] = {
#define SCN_LUA_RESULT_ROW(r) { (int)(r), #r },
    SCN_LUA_RESULT_LIST(SCN_LUA_RESULT_ROW)
#undef SCN_LUA_RESULT_ROW
};

/* The codes run from zero with no gaps, so the last one plus one is how many
 * there are. A code added to the enum without a line above does not compile
 * rather than reaching a script as "". */
BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnLuaResults) / sizeof(kScnLuaResults[0])) ==
        (int)SCN_OP_BAD_CALL + 1,
    result_name_table_is_the_whole_result_enum);

const char *scenarioLuaResultName(int result) {
    size_t i;

    for (i = 0; i < sizeof(kScnLuaResults) / sizeof(kScnLuaResults[0]); i++) {
        if (kScnLuaResults[i].result == result) {
            return kScnLuaResults[i].name;
        }
    }
    return "";
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
static lua_Integer scnWhole(lua_Number v) {
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

static lua_Integer scnArgInt(lua_State *L, int idx, const char *name) {
    if (lua_type(L, idx) != LUA_TNUMBER) {
        return (lua_Integer)luaL_argerror(
            L, idx,
            lua_pushfstring(L, "%s must be a number, got %s", name,
                            luaL_typename(L, idx)));
    }
    return scnWhole(lua_tonumber(L, idx));
}

/* The number an argument arrived as, untouched, with the same raise for a
 * mistyped one that scnArgInt makes. For a row whose own test has to see the
 * number before the conversion above folds the ends of the range and a NaN
 * onto values that mean something else. */
static lua_Number scnArgNumber(lua_State *L, int idx, const char *name) {
    if (lua_type(L, idx) != LUA_TNUMBER) {
        return (lua_Number)luaL_argerror(
            L, idx,
            lua_pushfstring(L, "%s must be a number, got %s", name,
                            luaL_typename(L, idx)));
    }
    return lua_tonumber(L, idx);
}

/* An argument a row may be called without. Absent and nil are the same
 * thing: a script writing nil for a field it does not care about means what
 * a script leaving the field off means. */
static lua_Integer scnOptInt(lua_State *L, int idx, const char *name,
                             lua_Integer def) {
    if (lua_isnoneornil(L, idx)) {
        return def;
    }
    return scnArgInt(L, idx, name);
}

/* Whether a number arrived as one a payload's byte can carry. Every row that
 * fills a byte asks this first: a Lua 256 cast into one would arrive as seat
 * 0 or as the square at the top corner. */
static bool scnFitsByte(lua_Integer v) {
    return v >= 0 && v <= 255;
}

/* A start a script named, as the number the payload carries. True with the
 * index written to out; false for a number no start could ever have.
 *
 * The raw number is what is tested. Every other argument goes through the
 * whole-number conversion above, which answers the bottom of the range for a
 * NaN and the top of it for 1e300, and the index rule turns both of those
 * into SCN_LUA_NO_ITEM — which is the very value the two rows that take a
 * start read as "no start named, let the engine pick". A start asked for by
 * a number that names none is a refusal, not a free choice, so the test
 * comes before the conversion rather than after it.
 *
 * The caller has already dealt with its own way of naming no start: an
 * argument that is absent, or a spawn's zero. */
static bool scnStartIndex(lua_Number v, BYTE *out) {
    if (!(v >= 1.0 && v <= 255.0) || v != (lua_Number)(lua_Integer)v) {
        return false;
    }
    *out = scenarioLuaIndexToOp((lua_Integer)v);
    return true;
}

/* A true or false argument. A number is not one, because 0 is true in Lua
 * and a script writing 0 for false would otherwise set the flag. */
static bool scnArgBool(lua_State *L, int idx, const char *name) {
    if (!lua_isboolean(L, idx)) {
        luaL_argerror(L, idx,
                      lua_pushfstring(L, "%s must be true or false, got %s",
                                      name, luaL_typename(L, idx)));
    }
    return lua_toboolean(L, idx) != 0;
}

static bool scnOptBool(lua_State *L, int idx, const char *name, bool def) {
    if (lua_isnoneornil(L, idx)) {
        return def;
    }
    return scnArgBool(L, idx, name);
}

static void scnArgTable(lua_State *L, int idx, const char *name) {
    if (!lua_istable(L, idx)) {
        luaL_argerror(L, idx,
                      lua_pushfstring(L, "%s must be a table, got %s", name,
                                      luaL_typename(L, idx)));
    }
}

/* A text argument and how many bytes of it there are. The length is the
 * row's business: text is refused for being too long, never cut, so the row
 * holds it against its own field and says what it found. */
static const char *scnArgText(lua_State *L, int idx, const char *name,
                              size_t *len) {
    if (lua_type(L, idx) != LUA_TSTRING) {
        luaL_argerror(L, idx,
                      lua_pushfstring(L, "%s must be a string, got %s", name,
                                      luaL_typename(L, idx)));
        return NULL;   /* luaL_argerror does not return */
    }
    return lua_tolstring(L, idx, len);
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
        /* The one caller resolves gameScripted before it gets here, so this
           case is only what keeps the switch covering the enumeration. A
           scripted round plays under the base game it declared, and "strict"
           is what an undeclared one plays. */
        case gameScripted:         return "strict";
    }
    return "strict";
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
 * word an order takes — which is why this and the order word below are one
 * table. builderJobNone names no order and falls out of it. */
static const char *scnBuilderJobWord(BuilderJob j) {
    return scenarioLuaBuildOrderWord((int)j);
}

const char *scenarioLuaBuiltActionWord(int action) {
    switch (action) {
        case builderJobTrees:    return "trees";
        case builderJobRoad:     return "road";
        case builderJobBuilding: return "building";
        /* Not "pill". input_packet.h says so where the event is defined: a
           new pillbox going down is EVENT_PILL_PLACED, so the one thing a
           build of kind pill can be on EVENT_BUILT is a repair of one that
           was already there. An order is still given as "pill" — the engine
           has six request codes and decides repairing from what is on the
           square — so this is the one direction the two spellings differ,
           and it differs because the fact does. */
        case builderJobPill:     return "repair";
        /* Nor does a mine reach this event: laying one is EVENT_MINE_PLACED.
           Spelled anyway, so the answer does not depend on which facts the
           engine happens to route here today. */
        case builderJobMine:     return "mine";
        case builderJobBoat:     return "boat";
        default:                 return NULL;
    }
}

const char *scenarioLuaDeathCauseWord(int cause) {
    switch (cause) {
        case LAST_DEATH_BY_MINES:   return "mine";
        case LAST_DEATH_BY_DEEPSEA: return "deep_sea";
        case LAST_DEATH_BY_SHELL:   return "shell";
        case LAST_DEATH_BY_SCRIPT:  return "script";
        default:                    return NULL;
    }
}

/* ── The words a policy question reads as ─────────────────────────── */

/* The order, not the job. The event word above reads a fact the engine has
   already settled and spells a pill build "repair", because that is the only
   thing it can have been; this reads what the player asked for, before the
   engine substitutes a harvest for a road on forest or a repair for a
   placement, so a pill order is "pill". */
const char *scenarioLuaBuildOrderWord(int action) {
    switch (action) {
        case builderJobTrees:    return "trees";
        case builderJobRoad:     return "road";
        case builderJobBuilding: return "building";
        case builderJobPill:     return "pill";
        case builderJobMine:     return "mine";
        case builderJobBoat:     return "boat";
        default:                 return NULL;
    }
}

const char *scenarioLuaCaptureKindWord(int kind) {
    switch (kind) {
        case CAPTURE_KIND_PILL: return "pill";
        case CAPTURE_KIND_BASE: return "base";
        default:                return NULL;
    }
}

const char *scenarioLuaHitKindWord(int kind) {
    switch (kind) {
        case HIT_KIND_TANK: return "tank";
        case HIT_KIND_PILL: return "pill";
        default:            return NULL;
    }
}

const char *scenarioLuaDieKindWord(int kind) {
    switch (kind) {
        case DIE_KIND_TANK:    return "tank";
        case DIE_KIND_BUILDER: return "builder";
        case DIE_KIND_PILL:    return "pill";
        default:               return NULL;
    }
}

/* A shell and a mine are spelled here as a tank's cause spells them, so a
   script reads one vocabulary across the three kinds a death comes in. A
   dying tank's blast is "explosion", a word only a builder or a pill is
   handed: a tank takes no damage from one.
   DMG_SRC_UNKNOWN has no word on purpose: what the script is handed for it is
   nil, which says what the value says. */
const char *scenarioLuaDamageSourceWord(int source) {
    switch (source) {
        case DMG_SRC_SHELL:     return "shell";
        case DMG_SRC_MINE:      return "mine";
        case DMG_SRC_EXPLOSION: return "explosion";
        default:                return NULL;
    }
}

const char *scenarioLuaAnnounceKindWord(int kind) {
    switch (kind) {
        case ANNOUNCE_KIND_JOINED:        return "joined";
        case ANNOUNCE_KIND_LEFT:          return "left";
        case ANNOUNCE_KIND_BASE_CAPTURED: return "base_captured";
        case ANNOUNCE_KIND_PILL_CAPTURED: return "pill_captured";
        case ANNOUNCE_KIND_BUILDER_LOST:  return "builder_lost";
        case ANNOUNCE_KIND_NAME_CHANGED:  return "name_changed";
        case ANNOUNCE_KIND_ALLIANCE:      return "alliance";
        case ANNOUNCE_KIND_VOTE:          return "vote";
        default:                          return NULL;
    }
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
 * a scenario that asks for none reads the sim's own, resolved the way every
 * other site resolves it. The resolve is what keeps "scripted" off this row:
 * a scripted round's own game type is gameScripted, and the word a script
 * wants back is the base game it plays — the same three words the loadout
 * table in a spawn_bot call holds.
 *
 * The table's word is checked against that same set before it is handed
 * back, because a word the set does not hold never reached the lobby
 * template: the round plays strict, and this row says strict rather than
 * repeating what the author typed. */
static int scnLuaGameType(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);
    int              declared = 0;

    if (c->manifest != NULL && c->manifest->game[0] != '\0' &&
        scenarioLuaLoadoutFromWord(c->manifest->game, &declared)) {
        lua_pushstring(L, c->manifest->game);
        return 1;
    }
    lua_pushstring(L, scnGameTypeWord(
        gameTypeResolve(serverSimGetGameSim(c->sim),
                        serverSimGetGameType(c->sim))));
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
    if (slot.team_pool[0] != '\0') {
        scnSetStr(L, "team_pool", slot.team_pool);
    }
    return 1;
}

/* The sim's own alliance table, which is what every game rule reads. It is
 * not worked out from the two seats' teams: players ally and split in play,
 * and the table is where that shows. A seat nobody holds answers nil, as
 * lobby_slot does for it, and a seat is always on its own side. */
static int scnLuaAllied(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);
    BYTE             a;
    BYTE             b;

    if (!scnSlotOf(L, 1, "a", &a) || !scnSlotOf(L, 2, "b", &b) ||
        !serverSimIsPlayerConnected(c->sim, a) ||
        !serverSimIsPlayerConnected(c->sim, b)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, serverSimIsAllied(c->sim, a, b));
    return 1;
}

/* ── The rules ────────────────────────────────────────────────────── */

/* A name that spells no rule raises rather than answering nil: every other
 * nil here is an entity that is not there, and a rule that is not in the
 * table is a misspelling in the script. */
static int scnLuaRule(lua_State *L) {
    const ScnLuaCtx *c    = scnCtx(L);
    const char      *name = scnArgStr(L, 1, "name");
    int              rule = simRulesRuleIndex(name);
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

/* ── Settings ─────────────────────────────────────────────────────── */

/* One line about a settings block, through the caller's reporter. */
static void scnSettingsSay(ScnSettingsReportFn report, void *ud,
                           const char *key, const char *fmt, ...) {
    char    line[256];
    va_list ap;

    if (report == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    report(ud, key, line);
}

/* A field of the row at index row as a whole number that fits an int32.
 * Absent answers true with *had false and leaves *out alone. Anything else
 * that is not such a number answers false. */
static bool scnSettingsInt(lua_State *L, int row, const char *field,
                           int32_t *out, bool *had) {
    lua_Number v;
    bool       ok = true;

    *had = false;
    lua_pushstring(L, field);
    lua_rawget(L, row);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    if (lua_type(L, -1) != LUA_TNUMBER) {
        ok = false;
    } else {
        v = lua_tonumber(L, -1);
        if (v != v || v < -2147483648.0 || v > 2147483647.0 ||
            v != (lua_Number)(int64_t)v) {
            ok = false;
        } else {
            *out = (int32_t)(int64_t)v;
            *had = true;
        }
    }
    lua_pop(L, 1);
    return ok;
}

/* Whether the row gives field at all, whatever its value. */
static bool scnSettingsHas(lua_State *L, int row, const char *field) {
    bool has;

    lua_pushstring(L, field);
    lua_rawget(L, row);
    has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has;
}

/* A bool row's default, true or false, into *out as 1 or 0. False for a
 * default that is absent or is not a Lua boolean. */
static bool scnSettingsBool(lua_State *L, int row, int32_t *out) {
    bool ok;

    lua_pushstring(L, "default");
    lua_rawget(L, row);
    ok = lua_isboolean(L, -1);
    if (ok) {
        *out = lua_toboolean(L, -1) ? 1 : 0;
    }
    lua_pop(L, 1);
    return ok;
}

/* A choice row's words and its default, into s: choices must be a list of
 * strings and default one of them. NULL when they read, else the reason for
 * the report. The count and the words are checked again, with the range, by
 * scnSettingProblem. */
static const char *scnSettingsChoices(lua_State *L, int row, ScnSetting *s) {
    const char *why = NULL;
    int         total;
    int         i;

    lua_pushstring(L, "choices");
    lua_rawget(L, row);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return "needs a list of words for choices";
    }
    total = (int)lua_rawlen(L, -1);
    if (total < SCN_SETTING_CHOICES_MIN ||
        total > SCN_SETTING_CHOICES_WORDS_MAX) {
        lua_pop(L, 1);
        return "a choice setting needs 2 to 8 choices";
    }
    for (i = 1; i <= total && why == NULL; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_type(L, -1) != LUA_TSTRING) {
            why = "every choice must be a string";
        } else {
            size_t      n;
            const char *w = lua_tolstring(L, -1, &n);
            if (n == 0 || n >= SCN_SETTING_CHOICE_LEN || strlen(w) != n) {
                why = "each choice must be 1 to 31 characters";
            } else {
                memcpy(s->choices[i - 1], w, n + 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* choices */
    if (why != NULL) {
        return why;
    }
    s->numChoices = (uint8_t)total;
    s->min        = 0;
    s->max        = total - 1;
    s->step       = 1;

    lua_pushstring(L, "default");
    lua_rawget(L, row);
    if (lua_type(L, -1) != LUA_TSTRING) {
        why = "needs one of its choices, as a string, for default";
    } else {
        int at = scnSettingChoiceIndex(s, lua_tostring(L, -1));
        if (at < 0) {
            why = "default is not one of its choices";
        } else {
            s->def = at;
        }
    }
    lua_pop(L, 1);
    return why;
}

/* A string field of the row into dst. False for a value that is there but is
 * not a string, or does not fit. Absent answers true and leaves dst "". */
static bool scnSettingsStr(lua_State *L, int row, const char *field,
                           char *dst, size_t dstLen) {
    bool ok = true;

    dst[0] = '\0';
    lua_pushstring(L, field);
    lua_rawget(L, row);
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t      n;
        const char *str = lua_tolstring(L, -1, &n);
        if (n >= dstLen || strlen(str) != n) {
            ok = false;
        } else {
            memcpy(dst, str, n + 1);
        }
    } else if (!lua_isnil(L, -1)) {
        ok = false;
    }
    lua_pop(L, 1);
    return ok;
}

int scenarioLuaReadSettings(lua_State *L, int tbl, ScnSetting *out, int max,
                            const char *path, ScnSettingsReportFn report,
                            void *ud) {
    int  list;
    int  n = 0;
    int  total;
    int  i;
    char key[48];

    if (L == NULL || out == NULL || max <= 0) {
        return 0;
    }
    if (path == NULL) {
        path = "script";
    }
    if (tbl < 0) {
        tbl = lua_gettop(L) + tbl + 1;
    }
    lua_pushstring(L, "settings");
    lua_rawget(L, tbl);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_istable(L, -1)) {
        scnSettingsSay(report, ud, "settings",
                       "scenario: %s: settings is not a list of settings",
                       path);
        lua_pop(L, 1);
        return 0;
    }
    list  = lua_gettop(L);
    total = (int)lua_rawlen(L, list);
    for (i = 1; i <= total; i++) {
        ScnSetting  s;
        char        type[16];
        bool        hadStep;
        bool        hadMin;
        bool        hadMax;
        bool        hadDef;
        const char *why;
        int         row;

        snprintf(key, sizeof(key), "settings[%d]", i);
        lua_rawgeti(L, list, i);
        if (!lua_istable(L, -1)) {
            scnSettingsSay(report, ud, key,
                           "scenario: %s: %s is not a table; dropped", path,
                           key);
            lua_pop(L, 1);
            continue;
        }
        row = lua_gettop(L);
        memset(&s, 0, sizeof(s));
        s.step = 1;
        if (!scnSettingsStr(L, row, "id", s.id, sizeof(s.id)) ||
            s.id[0] == '\0') {
            scnSettingsSay(report, ud, key,
                           "scenario: %s: %s has no id of up to %d "
                           "characters; dropped", path, key,
                           SCN_SETTING_ID_LEN - 1);
            lua_pop(L, 1);
            continue;
        }
        snprintf(key, sizeof(key), "settings.%s", s.id);
        if (!scnSettingsStr(L, row, "label", s.label, sizeof(s.label)) ||
            s.label[0] == '\0') {
            scnSettingsSay(report, ud, key,
                           "scenario: %s: %s has no label of up to %d "
                           "characters; dropped", path, key,
                           SCN_SETTING_LABEL_LEN - 1);
            lua_pop(L, 1);
            continue;
        }
        if (!scnSettingsStr(L, row, "type", type, sizeof(type))) {
            type[0] = '?';
            type[1] = '\0';
        }
        if (type[0] == '\0' || strcmp(type, "int") == 0) {
            s.type = SCN_SETTING_TYPE_INT;
        } else if (strcmp(type, "bool") == 0) {
            s.type = SCN_SETTING_TYPE_BOOL;
        } else if (strcmp(type, "choice") == 0) {
            s.type = SCN_SETTING_TYPE_CHOICE;
        } else {
            scnSettingsSay(report, ud, key,
                           "scenario: %s: %s has type '%s'; only \"int\", "
                           "\"bool\" and \"choice\" are supported; dropped",
                           path, key, type);
            lua_pop(L, 1);
            continue;
        }
        if (s.type == SCN_SETTING_TYPE_CHOICE) {
            /* The words are the range, so a min, max or step on a choice
               row is a mistake in the row, as it is on a bool row. */
            const char *bad = NULL;

            if (scnSettingsHas(L, row, "min") ||
                scnSettingsHas(L, row, "max") ||
                scnSettingsHas(L, row, "step")) {
                bad = "is a choice setting and takes no min, max or step";
            } else {
                bad = scnSettingsChoices(L, row, &s);
            }
            if (bad != NULL) {
                scnSettingsSay(report, ud, key, "scenario: %s: %s %s; dropped",
                               path, key, bad);
                lua_pop(L, 1);
                continue;
            }
        } else if (s.type == SCN_SETTING_TYPE_BOOL) {
            /* On or off has a fixed range, so a min, max or step on a
               bool row is a mistake in the row. */
            if (scnSettingsHas(L, row, "min") ||
                scnSettingsHas(L, row, "max") ||
                scnSettingsHas(L, row, "step")) {
                scnSettingsSay(report, ud, key,
                               "scenario: %s: %s is a bool setting and takes "
                               "no min, max or step; dropped", path, key);
                lua_pop(L, 1);
                continue;
            }
            if (!scnSettingsBool(L, row, &s.def)) {
                scnSettingsSay(report, ud, key,
                               "scenario: %s: %s needs true or false for "
                               "default; dropped", path, key);
                lua_pop(L, 1);
                continue;
            }
            s.min  = 0;
            s.max  = 1;
            s.step = 1;
        } else if (!scnSettingsInt(L, row, "min", &s.min, &hadMin) ||
                   !scnSettingsInt(L, row, "max", &s.max, &hadMax) ||
                   !scnSettingsInt(L, row, "step", &s.step, &hadStep) ||
                   !scnSettingsInt(L, row, "default", &s.def, &hadDef) ||
                   !hadMin || !hadMax || !hadDef) {
            scnSettingsSay(report, ud, key,
                           "scenario: %s: %s needs whole numbers for min, "
                           "max and default (and step, if given); dropped",
                           path, key);
            lua_pop(L, 1);
            continue;
        }
        why = scnSettingProblem(&s);
        if (why != NULL) {
            scnSettingsSay(report, ud, key, "scenario: %s: %s: %s; dropped",
                           path, key, why);
            lua_pop(L, 1);
            continue;
        }
        if (scnSettingFind(out, n, s.id) != NULL) {
            scnSettingsSay(report, ud, key,
                           "scenario: %s: %s is declared twice; the second "
                           "is dropped", path, key);
            lua_pop(L, 1);
            continue;
        }
        if (n >= max) {
            scnSettingsSay(report, ud, key,
                           "scenario: %s: more than %d settings; %s dropped",
                           path, max, key);
            lua_pop(L, 1);
            continue;
        }
        out[n++] = s;
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* settings */
    return n;
}

void scenarioLuaPushSetting(lua_State *L, const ScnSetting *s, int32_t v) {
    if (s != NULL && s->type == SCN_SETTING_TYPE_BOOL) {
        lua_pushboolean(L, v != 0);
    } else if (s != NULL && s->type == SCN_SETTING_TYPE_CHOICE) {
        /* The word, never the index: the script compares against the words
           it wrote. A value that names no word is the default word. */
        const char *w = scnSettingChoiceText(s, v);
        if (w == NULL) {
            w = scnSettingChoiceText(s, s->def);
        }
        lua_pushstring(L, w != NULL ? w : "");
    } else {
        lua_pushinteger(L, (lua_Integer)v);
    }
}

/* The declared settings of the script that is calling, into rows. While a
 * hook or a policy runs, that is the table the host read. At the file's top
 * level the host has not read the table yet, so it is read from the file's
 * own globals. -1 when no script is running. */
static int scnLuaSettingsOfCaller(lua_State *L, const ScnLuaCtx *c,
                                  ScnSetting *rows) {
    int n;

    if (c->running != NULL) {
        n = (int)c->running->numSettings;
        if (n > SCN_SETTINGS_MAX) {
            n = SCN_SETTINGS_MAX;
        }
        memcpy(rows, c->running->settings, (size_t)n * sizeof(rows[0]));
        return n;
    }
    if (c->runningEnv <= 0) {
        return -1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->runningEnv);
    lua_pushstring(L, "scenario");
    lua_rawget(L, -2);
    n = 0;
    if (lua_istable(L, -1)) {
        /* Quiet: the manifest read after the chunk reports the block once. */
        n = scenarioLuaReadSettings(L, -1, rows, SCN_SETTINGS_MAX, NULL,
                                    NULL, NULL);
    }
    lua_pop(L, 2);
    return n;
}

/* The file name at the end of a path: the key the server keeps the host's
 * choices under, the same on every machine. */
static const char *scnLuaSettingFile(const char *path) {
    const char *last = path;
    const char *p;

    if (path == NULL) {
        return "";
    }
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }
    return last;
}

/* game.setting(id): what the host chose for one of this script's own
 * settings, or its declared default when the host chose nothing or chose a
 * value the declaration does not allow. An id the script never declared
 * raises, as a rule name that spells no rule does: it is a misspelling, and
 * a nil would carry it on into arithmetic. */
static int scnLuaSetting(lua_State *L) {
    const ScnLuaCtx  *c  = scnCtx(L);
    const char       *id = scnArgStr(L, 1, "id");
    ScnSetting        rows[SCN_SETTINGS_MAX];
    const ScnSetting *s;
    int               n;
    int32_t           v      = 0;
    bool              chosen = false;

    n = scnLuaSettingsOfCaller(L, c, rows);
    if (n < 0) {
        return luaL_error(L, "setting('%s') has no script to answer for",
                          id);
    }
    s = scnSettingFind(rows, n, id);
    if (s == NULL) {
        return luaL_error(L, "no setting is named '%s'", id);
    }
    if (c->sim != NULL && c->runningFile != NULL) {
        chosen = serverSimGetScriptSetting(
            c->sim, scnLuaSettingFile(c->runningFile), id, &v);
    }
    scenarioLuaPushSetting(L, s, scnSettingResolve(s, chosen, v));
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

/* Where this script's own region of that name sits in the table, and -1 for
 * a script that has not named one. The one place the "its own" half of the
 * lookup rule is written: the read below answers from it, and so does
 * game.define_region, which has to replace the asker's own rectangle rather
 * than another script's.
 *
 * SCN_OWNER_NONE never matches. A caller that is no script of the round's
 * owns nothing, and the regions a manifest carries that nobody owns — an
 * entry's own table straight out of its file, which was never composed —
 * are not its to claim either. */
static int scnRegionOwnIndex(const ScenarioManifest *m, const char *name,
                             uint8_t asking) {
    int i;

    if (m == NULL || name == NULL || asking == SCN_OWNER_NONE) {
        return -1;
    }
    for (i = 0; i < (int)m->numRegions; i++) {
        if (m->regions[i].owner == asking &&
            strcmp(m->regions[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const ScnManifestRegion *scenarioLuaRegionFind(const ScenarioManifest *m,
                                               const char *name,
                                               uint8_t asking) {
    int own;
    int i;

    if (m == NULL || name == NULL) {
        return NULL;
    }
    own = scnRegionOwnIndex(m, name, asking);
    if (own >= 0) {
        return &m->regions[own];
    }
    /* Nothing of the asker's under that name, so the list decides. The first
       match and not the last: the scripts are appended in list order, so the
       first one is the furthest up the list, which is the one a host who
       ordered the list meant to have the say. A round whose scripts name no
       region twice has one match either way, which is every round written
       before two of them were allowed to. */
    for (i = 0; i < (int)m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) {
            return &m->regions[i];
        }
    }
    return NULL;
}

bool scenarioLuaRegionHolds(const ScnManifestRegion *r, int mx, int my) {
    if (r == NULL || r->w == 0 || r->h == 0) {
        return false;
    }
    return mx >= (int)r->x && mx < (int)r->x + (int)r->w &&
           my >= (int)r->y && my < (int)r->y + (int)r->h;
}

/* The last segment of a path, which is the half of it that is the same on
 * every machine. A server started from a checkout, one started from an
 * install and one started from a test's temporary directory all read the
 * same script under three different paths, and a bit worked out from the
 * whole of one of them would differ between the three. Both separators are
 * cut on, because a path on Windows may hold either.
 *
 * scnFileNameOf in src/scenario/scenario_host.c is the same walk, written
 * for the lines that name a file to an operator. It is written twice rather
 * than shared because that one is static to a file this one cannot see, and
 * the two answer the same question for different readers. */
static const char *scnRegionKeyFile(const char *path) {
    const char *at = path;
    const char *s;

    if (path == NULL) {
        return "";
    }
    for (s = path; *s != '\0'; s++) {
        if (*s == '/' || *s == '\\') {
            at = s + 1;
        }
    }
    return at;
}

/* FNV-1a over one string, continued from a hash already running, so that the
 * key below is hashed as its parts rather than pasted into a buffer first.
 *
 * FNV-1a, and written out here rather than taken from somewhere: the two
 * numbers are the published ones, the arithmetic is unsigned 32-bit and
 * therefore wraps the same way under every compiler this builds with, and
 * nothing in it reads a pointer, a clock or a seed. That is the whole of
 * what is wanted — two machines composing one list have to agree on the
 * bits, because a headless game is replayed from its list and its seed and
 * is expected to play out the same both times. A collision costs one step
 * of the walk below and nothing else, so there is no reason to pay for a
 * wider hash. */
static uint32_t scnRegionKeyHash(uint32_t hash, const char *s) {
    if (s == NULL) {
        return hash;
    }
    for (; *s != '\0'; s++) {
        hash ^= (uint32_t)(unsigned char)*s;
        hash *= 16777619u;
    }
    return hash;
}

uint8_t scenarioLuaRegionBit(const ScenarioManifest *m, const char *path,
                             const char *name) {
    uint32_t hash  = 2166136261u;   /* FNV-1a's offset basis */
    uint64_t taken = 0;
    int      start;
    int      i;

    hash = scnRegionKeyHash(hash, scnRegionKeyFile(path));
    /* One zero byte hashed between the two halves, so that a file called
       "ab" naming "c" and a file called "a" naming "bc" are two keys and not
       one. Only the multiply is written: the exclusive-or half of an FNV-1a
       step against a zero byte leaves the hash where it was, and a line
       saying so would be a line that does nothing. Neither a file name nor a
       region name can hold a zero byte, so no key reaches this the other way
       round and loses its separator. */
    hash *= 16777619u;
    hash = scnRegionKeyHash(hash, name);

    /* Every bit the table has already handed out. A region whose bit was
       never assigned reads as zero, which makes bit zero look taken and
       costs one step of the walk; it cannot put two regions on one bit,
       because all of them read as the same zero. Nothing composed is in
       that state — both callers of this went through it — and the tables
       that are, the map editor's form and the one the JSON reader fills,
       have no round behind them to hold a bit for.

       A bit outside the mask is not one, and is stepped over rather than
       shifted by: the field is a byte and the mask is sixty-four bits wide,
       so a table filled by hand with a number this never handed out would
       otherwise shift past the end of a uint64_t. */
    if (m != NULL) {
        for (i = 0; i < (int)m->numRegions; i++) {
            if (m->regions[i].bit < SCN_REGIONS_MAX) {
                taken |= (uint64_t)1 << m->regions[i].bit;
            }
        }
    }
    start = (int)(hash % (uint32_t)SCN_REGIONS_MAX);
    for (i = 0; i < SCN_REGIONS_MAX; i++) {
        int at = (start + i) % SCN_REGIONS_MAX;

        if ((taken & ((uint64_t)1 << at)) == 0) {
            return (uint8_t)at;
        }
    }
    /* Every bit taken, which is a table already holding the SCN_REGIONS_MAX
       regions a round may name. Both callers refuse at that count before
       they ask, so this is unreachable; it answers the bit the hash named
       rather than looping, so that a caller that one day forgets the limit
       gets a wrong bit instead of a hung tick. */
    return (uint8_t)start;
}

static int scnLuaRegion(lua_State *L) {
    const ScnLuaCtx         *c    = scnCtx(L);
    const char              *name = scnArgStr(L, 1, "name");
    const ScnManifestRegion *r    = scenarioLuaRegionFind(c->manifest, name,
                                                          c->runningOwner);

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
 * script's table iterated in, which is not the same order under the two Lua
 * builds, so the array a script walks is sorted here and a script reading it
 * gets the same round twice.
 *
 * One entry per name and not per region. Two scripts on the list may each
 * name "spawn", so the composite may hold two rectangles under it, and a
 * name listed twice would have a script that walks the list and reads each
 * name do the same work twice on the same rectangle — game.region answers
 * one rectangle for a name however many carry it. The sort is what makes the
 * duplicates adjacent, so the skip below is a comparison with the name
 * before it. */
static int scnLuaRegions(lua_State *L) {
    const ScnLuaCtx        *c = scnCtx(L);
    const ScenarioManifest *m = c->manifest;
    uint8_t                 order[SCN_REGIONS_MAX];
    int                     n = (m != NULL) ? (int)m->numRegions : 0;
    int                     out = 0;
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
        const char *name = m->regions[order[i]].name;

        if (i > 0 && strcmp(m->regions[order[i - 1]].name, name) == 0) {
            continue;
        }
        out++;
        lua_pushstring(L, name);
        lua_rawseti(L, -2, out);
    }
    return 1;
}

/* A square inside a named rectangle. A name no region carries answers false:
 * a square is not inside a rectangle that was never named, and the row is a
 * question rather than a read of an entity.
 *
 * The square is compared as a plain int rather than as a byte, so a
 * coordinate off the map answers false instead of wrapping onto one that is
 * inside. */
static int scnLuaInRegion(lua_State *L) {
    const ScnLuaCtx         *c    = scnCtx(L);
    const char              *name = scnArgStr(L, 1, "name");
    lua_Integer              mx   = scnArgInt(L, 2, "mx");
    lua_Integer              my   = scnArgInt(L, 3, "my");
    const ScnManifestRegion *r    = scenarioLuaRegionFind(c->manifest, name,
                                                          c->runningOwner);

    lua_pushboolean(L, scenarioLuaRegionHolds(r, (int)mx, (int)my) ? 1 : 0);
    return 1;
}

/* ══ The ops ══════════════════════════════════════════════════════════
 *
 * A row fills one typed payload, hands it to the funnel and answers what
 * came back: true, true and "queued" while the work is still going on, or
 * nil, the refusal's own name and one sentence with the number that
 * mattered. A refusal is an answer rather than an error — a script tests it
 * — and only a mistyped argument or a word that names nothing raises.
 *
 * A number that no payload byte could carry is refused here rather than
 * cast, with the code the funnel would have answered for a value it could
 * see: a seat past the roster is no such player, a square past the map is a
 * bad square. Everything else the funnel decides.
 *
 * Item indices go through scenarioLuaIndexToOp, which is the one place the
 * script's 1-based numbering becomes the op's 0-based one. Player slots are
 * 0-based on both sides and convert nowhere. */

/* One word a script may write, and what the payload carries for it, is
 * ScnLuaWord in the header: four of the sets below are also the tables the
 * game table carries, so a document reads the rows an argument is matched
 * against rather than a second copy of them. */

/* A set of them, and what a message calls the set when a script writes a
 * word that is not in it. */
typedef struct {
    const ScnLuaWord *rows;
    size_t            count;
    const char       *what;
} ScnLuaWordSet;

/* The builder's jobs. Six, because the engine has six request codes: it
 * decides repairing from what is already on the square rather than from a
 * word of its own, so these are also the six game.builder(p).job answers. */
static const ScnLuaWord kScnActionWords[] = {
    { "trees",    (int)builderJobTrees    },
    { "road",     (int)builderJobRoad     },
    { "building", (int)builderJobBuilding },
    { "pill",     (int)builderJobPill     },
    { "mine",     (int)builderJobMine     },
    { "boat",     (int)builderJobBoat     },
};
static const ScnLuaWordSet kScnActions = {
    kScnActionWords, sizeof(kScnActionWords) / sizeof(kScnActionWords[0]),
    "a builder action"
};

/* What a spawning bot is handed, by the name of the rules it plays under. */
static const ScnLuaWord kScnLoadoutWords[] = {
    { "open",       (int)gameOpen             },
    { "tournament", (int)gameTournament       },
    { "strict",     (int)gameStrictTournament },
};
static const ScnLuaWordSet kScnLoadouts = {
    kScnLoadoutWords, sizeof(kScnLoadoutWords) / sizeof(kScnLoadoutWords[0]),
    "a loadout"
};

/* The sounds the server plays to the round. The list stops where the engine
 * stops taking one from a scenario: the lobby's own sounds are the last it
 * carries, and the ping sounds past them are the client's own. */
static const ScnLuaWord kScnSoundWords[] = {
    { "shoot_self",           (int)shootSelf          },
    { "shoot_near",           (int)shootNear          },
    { "shoot_far",            (int)shootFar           },
    { "shot_tree_near",       (int)shotTreeNear       },
    { "shot_tree_far",        (int)shotTreeFar        },
    { "shot_building_near",   (int)shotBuildingNear   },
    { "shot_building_far",    (int)shotBuildingFar    },
    { "hit_tank_near",        (int)hitTankNear        },
    { "hit_tank_far",         (int)hitTankFar         },
    { "hit_tank_self",        (int)hitTankSelf        },
    { "bubbles",              (int)bubbles            },
    { "tank_sink_near",       (int)tankSinkNear       },
    { "tank_sink_far",        (int)tankSinkFar        },
    { "big_explosion_near",   (int)bigExplosionNear   },
    { "big_explosion_far",    (int)bigExplosionFar    },
    { "farming_tree_near",    (int)farmingTreeNear    },
    { "farming_tree_far",     (int)farmingTreeFar     },
    { "man_building_near",    (int)manBuildingNear    },
    { "man_building_far",     (int)manBuildingFar     },
    { "man_dying_near",       (int)manDyingNear       },
    { "man_dying_far",        (int)manDyingFar        },
    { "man_laying_mine_near", (int)manLayingMineNear  },
    { "mine_explosion_near",  (int)mineExplosionNear  },
    { "mine_explosion_far",   (int)mineExplosionFar   },
    { "lobby_chat_received",  (int)lobbyChatReceived  },
    { "lobby_ready",          (int)lobbyReady         },
    { "lobby_unready",        (int)lobbyUnready       },
    { "lobby_countdown",      (int)lobbyCountdown     },
    { "lobby_game_start",     (int)lobbyGameStart     },
    { "lobby_player_join",    (int)lobbyPlayerJoin    },
    { "lobby_player_leave",   (int)lobbyPlayerLeave   },
};
static const ScnLuaWordSet kScnSounds = {
    kScnSoundWords, sizeof(kScnSoundWords) / sizeof(kScnSoundWords[0]),
    "a sound"
};

/* The seven panel primitives, by the word a list entry names one with. */
static const ScnLuaWord kScnPanelOpWords[] = {
    { "rect",   (int)SCN_PANEL_OP_RECT   },
    { "line",   (int)SCN_PANEL_OP_LINE   },
    { "text",   (int)SCN_PANEL_OP_TEXT   },
    { "name",   (int)SCN_PANEL_OP_NAME   },
    { "sprite", (int)SCN_PANEL_OP_SPRITE },
    { "bar",    (int)SCN_PANEL_OP_BAR    },
    { "timer",  (int)SCN_PANEL_OP_TIMER  },
};
static const ScnLuaWordSet kScnPanelOps = {
    kScnPanelOpWords, sizeof(kScnPanelOpWords) / sizeof(kScnPanelOpWords[0]),
    "a panel primitive"
};

/* The palette a panel and a marker draw from. A script writes the word or
 * the number itself, so a hand-written list reads as colours and one a
 * script computes still reaches the same byte.
 *
 * Index 0 draws nothing, and the last four carry names that say they are
 * reserved: they parse and draw as nothing until a skin gives them a colour,
 * so a script naming one is asking for that rather than for a colour. */
static const ScnLuaWord kScnColourWords[] = {
    { "none",        (int)SCN_PANEL_COLOUR_NONE        },
    { "black",       (int)SCN_PANEL_COLOUR_BLACK       },
    { "white",       (int)SCN_PANEL_COLOUR_WHITE       },
    { "grey",        (int)SCN_PANEL_COLOUR_GREY        },
    { "grey_dark",   (int)SCN_PANEL_COLOUR_GREY_DARK   },
    { "red",         (int)SCN_PANEL_COLOUR_RED         },
    { "green",       (int)SCN_PANEL_COLOUR_GREEN       },
    { "blue",        (int)SCN_PANEL_COLOUR_BLUE        },
    { "yellow",      (int)SCN_PANEL_COLOUR_YELLOW      },
    { "orange",      (int)SCN_PANEL_COLOUR_ORANGE      },
    { "cyan",        (int)SCN_PANEL_COLOUR_CYAN        },
    { "magenta",     (int)SCN_PANEL_COLOUR_MAGENTA     },
    { "reserved_12", (int)SCN_PANEL_COLOUR_RESERVED_12 },
    { "reserved_13", (int)SCN_PANEL_COLOUR_RESERVED_13 },
    { "reserved_14", (int)SCN_PANEL_COLOUR_RESERVED_14 },
    { "reserved_15", (int)SCN_PANEL_COLOUR_RESERVED_15 },
};
static const ScnLuaWordSet kScnColours = {
    kScnColourWords, sizeof(kScnColourWords) / sizeof(kScnColourWords[0]),
    "a panel colour"
};

/* Text height, in the frontend's own font. */
static const ScnLuaWord kScnSizeWords[] = {
    { "small",  (int)SCN_PANEL_SIZE_SMALL  },
    { "normal", (int)SCN_PANEL_SIZE_NORMAL },
    { "large",  (int)SCN_PANEL_SIZE_LARGE  },
};
static const ScnLuaWordSet kScnSizes = {
    kScnSizeWords, sizeof(kScnSizeWords) / sizeof(kScnSizeWords[0]),
    "a text size"
};

/* Which way text and timers sit about their x. */
static const ScnLuaWord kScnAlignWords[] = {
    { "left",   (int)SCN_PANEL_ALIGN_LEFT   },
    { "centre", (int)SCN_PANEL_ALIGN_CENTRE },
    { "right",  (int)SCN_PANEL_ALIGN_RIGHT  },
};
static const ScnLuaWordSet kScnAligns = {
    kScnAlignWords, sizeof(kScnAlignWords) / sizeof(kScnAlignWords[0]),
    "an alignment"
};

/* Whether a timer counts down to its tick or up from it. */
static const ScnLuaWord kScnTimerModeWords[] = {
    { "down", (int)SCN_PANEL_TIMER_DOWN },
    { "up",   (int)SCN_PANEL_TIMER_UP   },
};
static const ScnLuaWordSet kScnTimerModes = {
    kScnTimerModeWords,
    sizeof(kScnTimerModeWords) / sizeof(kScnTimerModeWords[0]),
    "a timer mode"
};

/* Every one of the four sets above names every value its field can carry,
 * counting from zero, so a number a set does not reach is a number the panel
 * parser would turn down. The readers lean on that rather than carrying a
 * ceiling of their own. */
BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnColourWords) / sizeof(kScnColourWords[0])) ==
        (int)SCN_PANEL_COLOURS,
    colour_words_are_the_whole_palette);
BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnSizeWords) / sizeof(kScnSizeWords[0])) ==
        (int)SCN_PANEL_SIZE_LARGE + 1,
    size_words_are_every_text_size);
BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnAlignWords) / sizeof(kScnAlignWords[0])) ==
        (int)SCN_PANEL_ALIGN_RIGHT + 1,
    align_words_are_every_alignment);
BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnTimerModeWords) / sizeof(kScnTimerModeWords[0])) ==
        (int)SCN_PANEL_TIMER_UP + 1,
    timer_mode_words_are_every_mode);

/* What a word stands for in its set, or false for one the set does not hold.
 * The lookup behind both the word arguments below and the panel list's own
 * readers, so a word is matched the same way wherever one is written. */
static bool scnWordValue(const ScnLuaWordSet *set, const char *word,
                         lua_Integer *out) {
    size_t i;

    for (i = 0; i < set->count; i++) {
        if (strcmp(set->rows[i].word, word) == 0) {
            *out = set->rows[i].value;
            return true;
        }
    }
    return false;
}

/* A word argument. One that names nothing in its set is a shape error, so a
 * misspelling stops the script rather than doing something else quietly. */
static int scnArgWord(lua_State *L, int idx, const char *name,
                      const ScnLuaWordSet *set) {
    const char *word = scnArgStr(L, idx, name);
    lua_Integer value;

    if (scnWordValue(set, word, &value)) {
        return (int)value;
    }
    return luaL_argerror(L, idx,
                         lua_pushfstring(L, "%s is '%s', which is not %s",
                                         name, word, set->what));
}

/* The same set, read by a caller with no lua_State argument to raise on: the
   host asks the loadout policy and has to answer classic for a word the set
   does not hold rather than stopping the script. One table serves both, so a
   word a spawn op takes is a word the policy takes. */
bool scenarioLuaLoadoutFromWord(const char *word, int *out) {
    size_t i;

    if (word == NULL || out == NULL) {
        return false;
    }
    for (i = 0; i < kScnLoadouts.count; i++) {
        if (strcmp(kScnLoadouts.rows[i].word, word) == 0) {
            *out = kScnLoadouts.rows[i].value;
            return true;
        }
    }
    return false;
}

/* Who a line is addressed to. */
typedef enum {
    SCN_LUA_TO_ALL,
    SCN_LUA_TO_PLAYER,
    SCN_LUA_TO_TEAM
} ScnLuaTargetKind;

/* nil or "all" for the whole game, a number for one seat, { team = t } for
 * one team. Anything else is a shape error. */
static ScnLuaTargetKind scnArgTarget(lua_State *L, int idx,
                                     lua_Integer *value) {
    *value = 0;

    if (lua_isnoneornil(L, idx)) {
        return SCN_LUA_TO_ALL;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char *word = lua_tostring(L, idx);
        if (strcmp(word, "all") == 0) {
            return SCN_LUA_TO_ALL;
        }
        luaL_argerror(L, idx,
                      lua_pushfstring(L, "target is '%s' and the only word a "
                                         "target takes is \"all\"", word));
    }
    if (lua_type(L, idx) == LUA_TNUMBER) {
        *value = scnWhole(lua_tonumber(L, idx));
        return SCN_LUA_TO_PLAYER;
    }
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "team");
        if (lua_type(L, -1) != LUA_TNUMBER) {
            lua_pop(L, 1);
            luaL_argerror(L, idx, "target is a table and has no team in it");
        }
        *value = scnWhole(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return SCN_LUA_TO_TEAM;
    }
    luaL_argerror(L, idx,
                  lua_pushfstring(L, "target must be nil, \"all\", a player "
                                     "or { team = t }, got %s",
                                  luaL_typename(L, idx)));
    return SCN_LUA_TO_ALL;   /* luaL_argerror does not return */
}

/* nil, the refusal's name, and one sentence with the number that mattered. */
static int scnRefusedV(lua_State *L, ScnOpResult r, const char *fmt,
                       va_list ap) {
    lua_pushnil(L);
    lua_pushstring(L, scenarioLuaResultName((int)r));
    lua_pushvfstring(L, fmt, ap);
    return 3;
}

/* Whether this state is checking a file rather than running one. A reload
 * loads the edited script in a state of its own to find out whether it can
 * be used, and that state runs the file's top level like any other — so a
 * top level that writes would write into the round that is playing.
 *
 * Asked by every row that reaches the op funnel, and by no row that reads:
 * what the check is for is finding out whether the file loads, and a file
 * that reads the sim while it loads has to be able to. */
static bool scnCheckingOnly(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);

    return c != NULL && c->checkOnly;
}

/* ── What a mod may not do ────────────────────────────────────────── */

/* The rows that decide the round: which side wins it, and when it is over.
 * A file that declared scenario.kind = "mod" said it changes how the game
 * plays and leaves winning and losing where the map and the server's rules
 * left them, so these are the rows it is held back from.
 *
 * One list in one place, because three readers want it. This file raises
 * when a mod calls one of these rows. scenario_host.c walks the triggers a
 * file declares and refuses to start a round for a mod whose action names
 * one of them, which is the same refusal made early, because a declared
 * action is visible before anything runs. The map editor leaves these off
 * the action list it offers while the form says mod, so an author is never
 * shown a thing the round will not take.
 *
 * Kept as names rather than as a column on the rows further down, because
 * the two readers outside this file have a name in hand and no row: a
 * trigger's action states the row it calls as text. */
static const char *const kScnRoundDeciders[] = {
    "end_round",     /* ends the round outright */
    "set_game_time", /* sets what is left of the clock, and a clock that runs
                      * out ends the round */
    "add_game_time", /* the same clock, moved rather than set */
    "score"          /* the scenario's own score for a seat or a team, which
                      * is the number a player reads as who is winning */
};

bool scenarioLuaOpDecidesRound(const char *name) {
    size_t i;

    if (name == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(kScnRoundDeciders) / sizeof(kScnRoundDeciders[0]);
         i++) {
        if (strcmp(name, kScnRoundDeciders[i]) == 0) {
            return true;
        }
    }
    return false;
}

const char *scenarioLuaRoundDeciderAt(size_t index) {
    if (index >= sizeof(kScnRoundDeciders) / sizeof(kScnRoundDeciders[0])) {
        return NULL;
    }
    return kScnRoundDeciders[index];
}

/* The other half of the same rule, on the functions a file writes rather than
 * the rows it calls: allow_base_win.
 *
 * One name and not a list, because one is all SCN_POLICY_LIST holds. Every
 * other policy answers a question about how the game plays — whether a seat
 * may sit on a team of its own, whether a dead tank comes back, what a hit
 * costs — and how the game plays is what a mod is for. A hook answers
 * nothing at all: the host reads a return value off a policy and off no hook,
 * which is why only the policy half of the catalogue was worth reading
 * through. A second policy whose answer ends a round is a second name here,
 * and this becomes a list the way the ops above are one.
 *
 * Spelled here and held to the row by a test rather than pasted out of the
 * list, because picking one row out of an X-macro takes more machinery than
 * the one name is worth. editor_form_mod_hides_base_win walks the catalogue
 * and fails if no row answers to this, so a rename in SCN_POLICY_LIST cannot
 * leave this pointing at a policy that is gone.
 *
 * The host does not ask this. scnAllowBaseWin knows which policy it is about
 * to ask and makes the check at the call site, and it ignores the answer
 * rather than raising: the script did not call anything, the sim called the
 * script. The readers are the editor's — the catalogue does not offer the
 * stub while the form says mod, and the check flags a mod that wrote the
 * function anyway. */
static const char *const kScnRoundDecidingFn = "allow_base_win";

bool scenarioLuaFnDecidesRound(const char *name) {
    return name != NULL && strcmp(name, kScnRoundDecidingFn) == 0;
}

/* Why the file that is calling may not decide the round, or NULL when it
 * may. The tail of a sentence the row's own name goes on the front of, so
 * the three rows that ask say the same thing in the same words.
 *
 * The tables the context points at are the round's own, read again at every
 * round start, so this answers for the files that are playing now. A file
 * that declared no kind answers NULL, which is what every scenario written
 * before the key existed needs it to answer.
 *
 * The calling file, and never the round. A round runs several scripts in one
 * state, and c->manifest is the composite: scnComposeInto in
 * src/scenario/scenario_host.c copies the declarative half from the base
 * entry, kind with it, so on any list holding a scenario the composite says
 * "scenario" for every mod on that list. Reading the composite is what this
 * used to do, and it let a mod call game.end_round for as long as a scenario
 * was listed with it. The composite is not read here at all, not even as a
 * fallback: it answers what the round is, which is a different question from
 * who is calling.
 *
 * c->running is the entry whose own code is on the stack. The host writes it
 * around every call it makes into a script — scnHookCall, scnPolicyBool,
 * scnPolicyAnswer and scnRunTimers in src/scenario/scenario_host.c are all
 * of them — and puts back what was there before, so a call that leads to
 * another call leaves it where it found it.
 *
 * NULL means no script's own code is running, and the three rows are refused
 * there. That is a file's top level: scnReadManifest reads the script's
 * table out of its globals after the chunk has returned, so while the chunk
 * runs the kind is not known and cannot be. Refused rather than guessed at,
 * and refused rather than allowed, because a top level has no round to end,
 * no score to keep and no clock to move — the boot running the chunk has not
 * started one. Nothing in data/ calls one of the three from a top level;
 * every call to them is from inside a function. A state with no context at
 * all answers the same way, for the same reason: nothing has said which file
 * is calling. */
static const char *scnNotTheDecider(lua_State *L) {
    const ScnLuaCtx *c = scnCtx(L);

    if (c == NULL || c->running == NULL) {
        return "decides the round, and no script is running that could "
               "decide one: a file's own top level is read before the round "
               "starts. Call it from a hook";
    }
    if (scnManifestKeepsWinCondition(c->running)) {
        return "decides the round and this file is a mod, which leaves the "
               "win condition alone; write scenario.kind = \"scenario\" if "
               "it is meant to decide rounds";
    }
    return NULL;
}

/* What a round-deciding row does when its caller may not call it. A raise
 * rather than a refusal, because the call is wrong rather than the moment:
 * the file said it does not decide rounds and then asked to, and an author
 * wants that on the error log with a line number, the way a misspelled rule
 * name is. The host counts it with every other raise, so a mod that keeps
 * asking is switched off for the round at SCN_ERROR_LIMIT the way any other
 * repeatedly raising script is.
 *
 * The sentence names the row and then says why, which scnNotTheDecider above
 * wrote. For a mod that is what a mod is and the one line to write if
 * deciding the round is what the file actually means to do. */
static int scnDecidesRefusal(lua_State *L, const char *op, const char *why) {
    return luaL_error(L, "%s %s", op, why);
}

/* What such a row answers, spelled once so every one of them says the same
 * thing. A wrong state rather than a refusal of its arguments: the row is
 * fine and the moment is not. */
static int scnCheckOnlyRefusal(lua_State *L) {
    lua_pushnil(L);
    lua_pushstring(L, scenarioLuaResultName((int)SCN_OP_WRONG_STATE));
    lua_pushliteral(L, "a reload checks the file and applies nothing");
    return 3;
}

/* A refusal the row itself makes, for a number the payload could not carry
 * at all. The code is the one the funnel would have answered had it been
 * able to see the value. */
static int scnRefused(lua_State *L, ScnOpResult r, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = scnRefusedV(L, r, fmt, ap);
    va_end(ap);
    return n;
}

/* Hand the op to the funnel and answer for it: true when it applied, true
 * and "queued" when the work is under way, and the refusal otherwise, whose
 * sentence the row writes from the numbers it was given.
 *
 * The rows that answer with an index or a seat of their own pass an out
 * struct and read it themselves. */
static int scnDone(lua_State *L, ScenarioOp *op, const char *fmt, ...) {
    ScnOpResult r;
    va_list     ap;
    int         n;

    if (scnCheckingOnly(L)) {
        return scnCheckOnlyRefusal(L);
    }
    r = serverSimApplyScenarioOp(scnCtx(L)->sim, op, NULL);
    if (r == SCN_OP_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (r == SCN_OP_QUEUED) {
        lua_pushboolean(L, 1);
        lua_pushliteral(L, "queued");
        return 2;
    }
    va_start(ap, fmt);
    n = scnRefusedV(L, r, fmt, ap);
    va_end(ap);
    return n;
}

/* ── Tank ─────────────────────────────────────────────────────────── */

/* One field out of a stocks table. Absent leaves that stock alone, which the
 * payload spells -1 where the numbers are absolute and 0 where they are
 * deltas. */
static int16_t scnStockField(lua_State *L, int idx, const char *key,
                             int16_t absent) {
    int16_t v = absent;

    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        lua_Integer n;
        if (lua_type(L, -1) != LUA_TNUMBER) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s must be a number, got %s",
                                          key, luaL_typename(L, -1)));
        }
        n = scnWhole(lua_tonumber(L, -1));
        /* Past what the field holds is past every cap the funnel checks
           against, so the clamp changes no answer: the op refuses it. */
        if (n < -32768) n = -32768;
        if (n > 32767)  n = 32767;
        v = (int16_t)n;
    }
    lua_pop(L, 1);
    return v;
}

static int scnStocks(lua_State *L, BYTE mode) {
    ScenarioOp  op;
    int16_t     absent = (mode == SCN_STOCK_ABSOLUTE) ? -1 : 0;
    lua_Integer p = scnArgInt(L, 1, "p");

    scnArgTable(L, 2, "t");
    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_TANK_SET_STOCKS;
    op.u.tankSetStocks.slot    = (BYTE)p;
    op.u.tankSetStocks.mode    = mode;
    op.u.tankSetStocks.shells  = scnStockField(L, 2, "shells", absent);
    op.u.tankSetStocks.mines   = scnStockField(L, 2, "mines", absent);
    op.u.tankSetStocks.armour  = scnStockField(L, 2, "armour", absent);
    op.u.tankSetStocks.trees   = scnStockField(L, 2, "trees", absent);
    return scnDone(L, &op,
                   "player %d, shells %d, mines %d, armour %d, trees %d",
                   (int)p, (int)op.u.tankSetStocks.shells,
                   (int)op.u.tankSetStocks.mines,
                   (int)op.u.tankSetStocks.armour,
                   (int)op.u.tankSetStocks.trees);
}

static int scnLuaSetStocks(lua_State *L) {
    return scnStocks(L, SCN_STOCK_ABSOLUTE);
}

static int scnLuaAddStocks(lua_State *L) {
    return scnStocks(L, SCN_STOCK_DELTA);
}

static int scnLuaKillTank(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p      = scnArgInt(L, 1, "p");
    lua_Integer killer = scnOptInt(L, 2, "killer", SCN_NONE);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(killer)) {
        return scnRefused(L, SCN_OP_RANGE, "killer %d is not a seat",
                          (int)killer);
    }
    memset(&op, 0, sizeof(op));
    op.type              = SCN_OP_TANK_KILL;
    op.u.tankKill.slot   = (BYTE)p;
    op.u.tankKill.killer = (BYTE)killer;
    /* A death the script caused, whether or not it named who did it. */
    op.u.tankKill.cause  = LAST_DEATH_BY_SCRIPT;
    return scnDone(L, &op, "player %d, killer %d", (int)p, (int)killer);
}

static int scnLuaTeleport(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p   = scnArgInt(L, 1, "p");
    lua_Integer x   = scnArgInt(L, 2, "x");
    lua_Integer y   = scnArgInt(L, 3, "y");
    lua_Integer dir = scnOptInt(L, 4, "dir", SCN_NONE);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (!scnFitsByte(dir)) {
        return scnRefused(L, SCN_OP_RANGE, "dir is %d, outside 0 to 255",
                          (int)dir);
    }
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_TANK_TELEPORT;
    op.u.tankTeleport.slot  = (BYTE)p;
    op.u.tankTeleport.mode  = SCN_TELEPORT_SQUARE;
    op.u.tankTeleport.x     = (BYTE)x;
    op.u.tankTeleport.y     = (BYTE)y;
    op.u.tankTeleport.start = SCN_NONE;
    op.u.tankTeleport.dir   = (BYTE)dir;
    return scnDone(L, &op, "player %d to (%d, %d)", (int)p, (int)x, (int)y);
}

static int scnLuaTeleportToStart(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p     = scnArgInt(L, 1, "p");
    BYTE        start = SCN_NONE;   /* no n: the engine picks */
    lua_Number  n     = 0;

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!lua_isnoneornil(L, 2)) {
        n = scnArgNumber(L, 2, "n");
        if (!scnStartIndex(n, &start)) {
            return scnRefused(L, SCN_OP_NO_SUCH_ITEM,
                              "start %f is no start: they are numbered 1 to "
                              "255", n);
        }
    }
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_TANK_TELEPORT;
    op.u.tankTeleport.slot  = (BYTE)p;
    op.u.tankTeleport.mode  = SCN_TELEPORT_START;
    op.u.tankTeleport.dir   = SCN_NONE;
    op.u.tankTeleport.start = start;
    /* Whole by now, or the row has already answered, so the sentence a
       refusal from the funnel carries reads as a start number. */
    return scnDone(L, &op, "player %d to start %d", (int)p, (int)n);
}

static int scnLuaSetBoat(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p  = scnArgInt(L, 1, "p");
    bool        on = scnArgBool(L, 2, "on");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_TANK_SET_BOAT;
    op.u.tankSetBoat.slot   = (BYTE)p;
    op.u.tankSetBoat.onBoat = on;
    return scnDone(L, &op, "player %d", (int)p);
}

static int scnLuaGivePill(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");
    lua_Integer n = scnArgInt(L, 2, "n");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                = SCN_OP_TANK_GIVE_PILL;
    op.u.tankGivePill.slot = (BYTE)p;
    op.u.tankGivePill.pill = scenarioLuaIndexToOp(n);
    return scnDone(L, &op, "player %d, pill %d", (int)p, (int)n);
}

static int scnLuaDropPill(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");
    lua_Integer n = scnArgInt(L, 2, "n");
    lua_Integer x = scnOptInt(L, 3, "x", SCN_NONE);
    lua_Integer y = scnOptInt(L, 4, "y", SCN_NONE);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    memset(&op, 0, sizeof(op));
    op.type                = SCN_OP_TANK_DROP_PILL;
    op.u.tankDropPill.slot = (BYTE)p;
    op.u.tankDropPill.pill = scenarioLuaIndexToOp(n);
    op.u.tankDropPill.x    = (BYTE)x;
    op.u.tankDropPill.y    = (BYTE)y;
    return scnDone(L, &op, "player %d, pill %d", (int)p, (int)n);
}

/* One percentage out of a modifier table. Absent is the classic tank, which
 * the payload spells 0. */
static bool scnModField(lua_State *L, int idx, const char *key, uint8_t *out,
                        lua_Integer *bad) {
    lua_Integer v = 0;

    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s must be a number, got %s",
                                          key, luaL_typename(L, -1)));
        }
        v = scnWhole(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    if (!scnFitsByte(v)) {
        *bad = v;
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static int scnLuaSetModifiers(lua_State *L) {
    ScenarioOp     op;
    TankModifiers *m;
    lua_Integer    bad = 0;
    lua_Integer    p   = scnArgInt(L, 1, "p");

    scnArgTable(L, 2, "t");
    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                     = SCN_OP_TANK_SET_MODIFIERS;
    op.u.tankSetModifiers.slot  = (BYTE)p;
    m = &op.u.tankSetModifiers.mods;
    /* The whole set is replaced, so a field the table leaves out goes back to
       the classic tank rather than keeping what it had. */
    if (!scnModField(L, 2, "speed", &m->speed, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "speed is %d", (int)bad);
    }
    if (!scnModField(L, 2, "accel", &m->accel, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "accel is %d", (int)bad);
    }
    if (!scnModField(L, 2, "turn", &m->turn, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "turn is %d", (int)bad);
    }
    if (!scnModField(L, 2, "reload", &m->reload, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "reload is %d", (int)bad);
    }
    if (!scnModField(L, 2, "dealt", &m->dealt, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "dealt is %d", (int)bad);
    }
    if (!scnModField(L, 2, "taken", &m->taken, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "taken is %d", (int)bad);
    }
    return scnDone(L, &op, "player %d", (int)p);
}

/* ── Builder ──────────────────────────────────────────────────────── */

static int scnLuaBuilderOrder(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p      = scnArgInt(L, 1, "p");
    int         action = scnArgWord(L, 2, "action", &kScnActions);
    lua_Integer x      = scnArgInt(L, 3, "x");
    lua_Integer y      = scnArgInt(L, 4, "y");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    memset(&op, 0, sizeof(op));
    op.type                  = SCN_OP_LGM_DISPATCH;
    op.u.lgmDispatch.slot    = (BYTE)p;
    op.u.lgmDispatch.action  = (BYTE)action;
    op.u.lgmDispatch.x       = (BYTE)x;
    op.u.lgmDispatch.y       = (BYTE)y;
    return scnDone(L, &op, "player %d at (%d, %d)", (int)p, (int)x, (int)y);
}

static int scnLuaBuilderRecall(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type              = SCN_OP_LGM_RECALL;
    op.u.lgmRecall.slot  = (BYTE)p;
    return scnDone(L, &op, "player %d", (int)p);
}

static int scnLuaKillLgm(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p      = scnArgInt(L, 1, "p");
    lua_Integer killer = scnOptInt(L, 2, "killer", SCN_NONE);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(killer)) {
        return scnRefused(L, SCN_OP_RANGE, "killer %d is not a seat",
                          (int)killer);
    }
    memset(&op, 0, sizeof(op));
    op.type             = SCN_OP_LGM_KILL;
    op.u.lgmKill.slot   = (BYTE)p;
    op.u.lgmKill.killer = (BYTE)killer;
    return scnDone(L, &op, "player %d, killer %d", (int)p, (int)killer);
}

static int scnLuaBuilderParachute(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");
    lua_Integer x = scnOptInt(L, 2, "x", SCN_NONE);
    lua_Integer y = scnOptInt(L, 3, "y", SCN_NONE);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_LGM_PARACHUTE;
    op.u.lgmParachute.slot  = (BYTE)p;
    op.u.lgmParachute.x     = (BYTE)x;
    op.u.lgmParachute.y     = (BYTE)y;
    return scnDone(L, &op, "player %d", (int)p);
}

static int scnLuaSetBuilderCarried(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p     = scnArgInt(L, 1, "p");
    lua_Integer trees = scnOptInt(L, 2, "trees", SCN_NONE);
    lua_Integer mines = scnOptInt(L, 3, "mines", SCN_NONE);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(trees)) {
        return scnRefused(L, SCN_OP_RANGE, "trees is %d", (int)trees);
    }
    if (!scnFitsByte(mines)) {
        return scnRefused(L, SCN_OP_RANGE, "mines is %d", (int)mines);
    }
    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_LGM_SET_CARRIED;
    op.u.lgmSetCarried.slot    = (BYTE)p;
    op.u.lgmSetCarried.trees   = (BYTE)trees;
    op.u.lgmSetCarried.mines   = (BYTE)mines;
    return scnDone(L, &op, "player %d", (int)p);
}

/* ── Pill and base ────────────────────────────────────────────────── */

/* An owner argument: a seat, game.NEUTRAL, or nothing at all, which is the
 * neutral owner as well. */
static lua_Integer scnArgOwner(lua_State *L, int idx) {
    return scnOptInt(L, idx, "owner", NEUTRAL);
}

static int scnLuaSetPillOwner(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n = scnArgInt(L, 1, "n");
    lua_Integer p = scnOptInt(L, 2, "p", NEUTRAL);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_RANGE, "owner %d is not a seat", (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_PILL_SET_OWNER;
    op.u.pillSetOwner.pill  = scenarioLuaIndexToOp(n);
    op.u.pillSetOwner.owner = (BYTE)p;
    return scnDone(L, &op, "pill %d, owner %d", (int)n, (int)p);
}

static int scnLuaSetPillArmour(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n = scnArgInt(L, 1, "n");
    lua_Integer a = scnArgInt(L, 2, "a");

    if (!scnFitsByte(a)) {
        return scnRefused(L, SCN_OP_RANGE, "armour is %d", (int)a);
    }
    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_PILL_SET_ARMOUR;
    op.u.pillSetArmour.pill    = scenarioLuaIndexToOp(n);
    op.u.pillSetArmour.armour  = (BYTE)a;
    return scnDone(L, &op, "pill %d, armour %d", (int)n, (int)a);
}

static int scnLuaSetPillSpeed(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n = scnArgInt(L, 1, "n");
    lua_Integer s = scnArgInt(L, 2, "s");

    if (!scnFitsByte(s)) {
        return scnRefused(L, SCN_OP_RANGE, "speed is %d", (int)s);
    }
    memset(&op, 0, sizeof(op));
    op.type                  = SCN_OP_PILL_SET_SPEED;
    op.u.pillSetSpeed.pill   = scenarioLuaIndexToOp(n);
    op.u.pillSetSpeed.speed  = (BYTE)s;
    return scnDone(L, &op, "pill %d, speed %d", (int)n, (int)s);
}

static int scnLuaMovePill(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n = scnArgInt(L, 1, "n");
    lua_Integer x = scnArgInt(L, 2, "x");
    lua_Integer y = scnArgInt(L, 3, "y");

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    memset(&op, 0, sizeof(op));
    op.type             = SCN_OP_PILL_MOVE;
    op.u.pillMove.pill  = scenarioLuaIndexToOp(n);
    op.u.pillMove.x     = (BYTE)x;
    op.u.pillMove.y     = (BYTE)y;
    return scnDone(L, &op, "pill %d to (%d, %d)", (int)n, (int)x, (int)y);
}

static int scnLuaSetBaseOwner(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n    = scnArgInt(L, 1, "n");
    lua_Integer p    = scnOptInt(L, 2, "p", NEUTRAL);
    bool        keep = scnOptBool(L, 3, "keep_stock", false);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_RANGE, "owner %d is not a seat", (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                     = SCN_OP_BASE_SET_OWNER;
    op.u.baseSetOwner.base      = scenarioLuaIndexToOp(n);
    op.u.baseSetOwner.owner     = (BYTE)p;
    op.u.baseSetOwner.keepStock = keep;
    return scnDone(L, &op, "base %d, owner %d", (int)n, (int)p);
}

/* One stock a base is being set to. Absent keeps what is there, which the
 * payload spells -1. */
static bool scnBaseStock(lua_State *L, int idx, const char *name,
                         int16_t *out, lua_Integer *bad) {
    lua_Integer v;

    if (lua_isnoneornil(L, idx)) {
        *out = -1;
        return true;
    }
    v = scnArgInt(L, idx, name);
    if (v < 0 || v > 32767) {
        *bad = v;
        return false;
    }
    *out = (int16_t)v;
    return true;
}

static int scnLuaSetBaseStock(lua_State *L) {
    ScenarioOp  op;
    lua_Integer bad = 0;
    lua_Integer n   = scnArgInt(L, 1, "n");

    memset(&op, 0, sizeof(op));
    op.type                = SCN_OP_BASE_SET_STOCK;
    op.u.baseSetStock.base = scenarioLuaIndexToOp(n);
    if (!scnBaseStock(L, 2, "armour", &op.u.baseSetStock.armour, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "armour is %d", (int)bad);
    }
    if (!scnBaseStock(L, 3, "shells", &op.u.baseSetStock.shells, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "shells is %d", (int)bad);
    }
    if (!scnBaseStock(L, 4, "mines", &op.u.baseSetStock.mines, &bad)) {
        return scnRefused(L, SCN_OP_RANGE, "mines is %d", (int)bad);
    }
    return scnDone(L, &op, "base %d", (int)n);
}

/* ── Entities ─────────────────────────────────────────────────────── */

/* What the index an add chose is called in the script: the op counts from
 * zero and the script counts from one. */
static int scnAdded(lua_State *L, ScenarioOp *op, const char *fmt, ...) {
    ScnOpOut    out;
    ScnOpResult r;
    va_list     ap;
    int         n;

    if (scnCheckingOnly(L)) {
        return scnCheckOnlyRefusal(L);
    }
    memset(&out, 0, sizeof(out));
    r = serverSimApplyScenarioOp(scnCtx(L)->sim, op, &out);
    if (r == SCN_OP_OK) {
        lua_pushinteger(L, scenarioLuaIndexToScript((int)out.index));
        return 1;
    }
    va_start(ap, fmt);
    n = scnRefusedV(L, r, fmt, ap);
    va_end(ap);
    return n;
}

/* The interval an untouched pill fires at, which is the rate a pill added
 * without one takes. Read off the round's own table, so a scenario that
 * changed the rule adds pills at the rate it is playing with. */
static BYTE scnRoundPillSpeed(lua_State *L) {
    double v = 0.0;

    if (serverSimGetScenarioRule(scnCtx(L)->sim,
                                 (uint16_t)SCN_RULE_pill_attack_ticks, &v) &&
        v >= 0.0 && v <= 255.0) {
        return (BYTE)v;
    }
    return 0;
}

static int scnLuaAddPill(lua_State *L) {
    ScenarioOp  op;
    lua_Integer x     = scnArgInt(L, 1, "x");
    lua_Integer y     = scnArgInt(L, 2, "y");
    lua_Integer owner = scnArgOwner(L, 3);
    lua_Integer armour;
    lua_Integer speed;

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (!scnFitsByte(owner)) {
        return scnRefused(L, SCN_OP_RANGE, "owner %d is not a seat",
                          (int)owner);
    }
    /* A pill nobody holds, lying dead on the ground, firing at the rate the
       round's table says: what a script asks for when it says only where. */
    armour = scnOptInt(L, 4, "armour", 0);
    speed  = scnOptInt(L, 5, "speed", scnRoundPillSpeed(L));
    if (!scnFitsByte(armour)) {
        return scnRefused(L, SCN_OP_RANGE, "armour is %d", (int)armour);
    }
    if (!scnFitsByte(speed)) {
        return scnRefused(L, SCN_OP_RANGE, "speed is %d", (int)speed);
    }
    memset(&op, 0, sizeof(op));
    op.type                   = SCN_OP_ENTITY_ADD_PILL;
    op.u.entityAddPill.x      = (BYTE)x;
    op.u.entityAddPill.y      = (BYTE)y;
    op.u.entityAddPill.owner  = (BYTE)owner;
    op.u.entityAddPill.armour = (BYTE)armour;
    op.u.entityAddPill.speed  = (BYTE)speed;
    return scnAdded(L, &op, "at (%d, %d)", (int)x, (int)y);
}

static int scnLuaRemovePill(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n = scnArgInt(L, 1, "n");

    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_ENTITY_REMOVE_PILL;
    op.u.entityRemovePill.pill = scenarioLuaIndexToOp(n);
    return scnDone(L, &op, "pill %d", (int)n);
}

static int scnLuaAddBase(lua_State *L) {
    ScenarioOp  op;
    lua_Integer x      = scnArgInt(L, 1, "x");
    lua_Integer y      = scnArgInt(L, 2, "y");
    lua_Integer owner  = scnArgOwner(L, 3);
    lua_Integer armour = scnOptInt(L, 4, "armour", 0);
    lua_Integer shells = scnOptInt(L, 5, "shells", 0);
    lua_Integer mines  = scnOptInt(L, 6, "mines", 0);

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (!scnFitsByte(owner)) {
        return scnRefused(L, SCN_OP_RANGE, "owner %d is not a seat",
                          (int)owner);
    }
    if (!scnFitsByte(armour) || !scnFitsByte(shells) || !scnFitsByte(mines)) {
        return scnRefused(L, SCN_OP_RANGE,
                          "armour %d, shells %d, mines %d", (int)armour,
                          (int)shells, (int)mines);
    }
    memset(&op, 0, sizeof(op));
    op.type                   = SCN_OP_ENTITY_ADD_BASE;
    op.u.entityAddBase.x      = (BYTE)x;
    op.u.entityAddBase.y      = (BYTE)y;
    op.u.entityAddBase.owner  = (BYTE)owner;
    op.u.entityAddBase.armour = (BYTE)armour;
    op.u.entityAddBase.shells = (BYTE)shells;
    op.u.entityAddBase.mines  = (BYTE)mines;
    return scnAdded(L, &op, "at (%d, %d)", (int)x, (int)y);
}

static int scnLuaRemoveBase(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n = scnArgInt(L, 1, "n");

    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_ENTITY_REMOVE_BASE;
    op.u.entityRemoveBase.base = scenarioLuaIndexToOp(n);
    return scnDone(L, &op, "base %d", (int)n);
}

static int scnLuaAddStart(lua_State *L) {
    ScenarioOp  op;
    lua_Integer x   = scnArgInt(L, 1, "x");
    lua_Integer y   = scnArgInt(L, 2, "y");
    lua_Integer dir = scnArgInt(L, 3, "dir");

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (!scnFitsByte(dir)) {
        return scnRefused(L, SCN_OP_RANGE, "dir is %d", (int)dir);
    }
    memset(&op, 0, sizeof(op));
    op.type                = SCN_OP_ENTITY_ADD_START;
    op.u.entityAddStart.x  = (BYTE)x;
    op.u.entityAddStart.y  = (BYTE)y;
    op.u.entityAddStart.dir = (BYTE)dir;
    return scnAdded(L, &op, "at (%d, %d) facing %d", (int)x, (int)y, (int)dir);
}

static int scnLuaRemoveStart(lua_State *L) {
    ScenarioOp  op;
    lua_Integer n = scnArgInt(L, 1, "n");

    memset(&op, 0, sizeof(op));
    op.type                      = SCN_OP_ENTITY_REMOVE_START;
    op.u.entityRemoveStart.start = scenarioLuaIndexToOp(n);
    return scnDone(L, &op, "start %d", (int)n);
}

/* ── Map ──────────────────────────────────────────────────────────── */

static int scnLuaSetTile(lua_State *L) {
    ScenarioOp  op;
    lua_Integer x = scnArgInt(L, 1, "x");
    lua_Integer y = scnArgInt(L, 2, "y");
    lua_Integer t = scnArgInt(L, 3, "t");

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (!scnFitsByte(t)) {
        return scnRefused(L, SCN_OP_RANGE, "terrain is %d", (int)t);
    }
    memset(&op, 0, sizeof(op));
    op.type                  = SCN_OP_MAP_SET_TILE;
    op.u.mapSetTile.x        = (BYTE)x;
    op.u.mapSetTile.y        = (BYTE)y;
    op.u.mapSetTile.terrain  = (BYTE)t;
    return scnDone(L, &op, "square (%d, %d), terrain %d", (int)x, (int)y,
                   (int)t);
}

static int scnLuaFillRect(lua_State *L) {
    ScenarioOp  op;
    lua_Integer x0 = scnArgInt(L, 1, "x0");
    lua_Integer y0 = scnArgInt(L, 2, "y0");
    lua_Integer x1 = scnArgInt(L, 3, "x1");
    lua_Integer y1 = scnArgInt(L, 4, "y1");
    lua_Integer t  = scnArgInt(L, 5, "t");

    if (!scnFitsByte(x0) || !scnFitsByte(y0) || !scnFitsByte(x1) ||
        !scnFitsByte(y1)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE,
                          "rectangle (%d, %d) to (%d, %d) is off the map",
                          (int)x0, (int)y0, (int)x1, (int)y1);
    }
    if (!scnFitsByte(t)) {
        return scnRefused(L, SCN_OP_RANGE, "terrain is %d", (int)t);
    }
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_MAP_FILL_RECT;
    op.u.mapFillRect.x0     = (BYTE)x0;
    op.u.mapFillRect.y0     = (BYTE)y0;
    op.u.mapFillRect.x1     = (BYTE)x1;
    op.u.mapFillRect.y1     = (BYTE)y1;
    op.u.mapFillRect.terrain = (BYTE)t;
    return scnDone(L, &op, "rectangle (%d, %d) to (%d, %d), terrain %d",
                   (int)x0, (int)y0, (int)x1, (int)y1, (int)t);
}

static int scnLuaPlaceMine(lua_State *L) {
    ScenarioOp  op;
    lua_Integer x       = scnArgInt(L, 1, "x");
    lua_Integer y       = scnArgInt(L, 2, "y");
    lua_Integer owner   = scnArgOwner(L, 3);
    bool        visible = scnOptBool(L, 4, "visible", false);

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (!scnFitsByte(owner)) {
        return scnRefused(L, SCN_OP_RANGE, "owner %d is not a seat",
                          (int)owner);
    }
    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_MAP_PLACE_MINE;
    op.u.mapPlaceMine.x        = (BYTE)x;
    op.u.mapPlaceMine.y        = (BYTE)y;
    op.u.mapPlaceMine.owner    = (BYTE)owner;
    op.u.mapPlaceMine.visible  = visible;
    return scnDone(L, &op, "square (%d, %d)", (int)x, (int)y);
}

static int scnLuaRemoveMine(lua_State *L) {
    ScenarioOp  op;
    lua_Integer x = scnArgInt(L, 1, "x");
    lua_Integer y = scnArgInt(L, 2, "y");

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    memset(&op, 0, sizeof(op));
    op.type                = SCN_OP_MAP_REMOVE_MINE;
    op.u.mapRemoveMine.x   = (BYTE)x;
    op.u.mapRemoveMine.y   = (BYTE)y;
    return scnDone(L, &op, "square (%d, %d)", (int)x, (int)y);
}

/* ── The roster ───────────────────────────────────────────────────── */

/* Text out of a table's field, into a fixed buffer. Absent leaves the buffer
 * empty, which every field here reads as the server's own choice. False when
 * the text would not fit, with what it measured in *len: text is refused for
 * being too long, never cut. */
static bool scnFieldText(lua_State *L, int idx, const char *key, char *out,
                         size_t cap, size_t *len) {
    *len = 0;
    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        const char *s;
        size_t      n = 0;

        if (lua_type(L, -1) != LUA_TSTRING) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s must be a string, got %s",
                                          key, luaL_typename(L, -1)));
        }
        s    = lua_tolstring(L, -1, &n);
        *len = n;
        if (n >= cap) {
            lua_pop(L, 1);
            return false;
        }
        memcpy(out, s, n + 1);
    }
    lua_pop(L, 1);
    return true;
}

static lua_Integer scnFieldInt(lua_State *L, int idx, const char *key,
                               lua_Integer def) {
    lua_Integer v = def;

    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s must be a number, got %s",
                                          key, luaL_typename(L, -1)));
        }
        v = scnWhole(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    return v;
}

/* The number a field arrived as, untouched. The field form of scnArgNumber,
 * and there for the same reason. */
static lua_Number scnFieldNumber(lua_State *L, int idx, const char *key,
                                 lua_Number def) {
    lua_Number v = def;

    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s must be a number, got %s",
                                          key, luaL_typename(L, -1)));
        }
        v = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

static bool scnFieldBool(lua_State *L, int idx, const char *key, bool def) {
    bool v = def;

    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s must be true or false, "
                                             "got %s", key,
                                          luaL_typename(L, -1)));
        }
        v = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    return v;
}

static int scnFieldWord(lua_State *L, int idx, const char *key,
                        const ScnLuaWordSet *set, int def) {
    int v = def;

    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        const char *word;
        size_t      i;
        bool        found = false;

        if (lua_type(L, -1) != LUA_TSTRING) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s must be a string, got %s",
                                          key, luaL_typename(L, -1)));
        }
        word = lua_tostring(L, -1);
        for (i = 0; i < set->count && !found; i++) {
            if (strcmp(set->rows[i].word, word) == 0) {
                v     = set->rows[i].value;
                found = true;
            }
        }
        if (!found) {
            luaL_argerror(L, idx,
                          lua_pushfstring(L, "t.%s is '%s', which is not %s",
                                          key, word, set->what));
        }
    }
    lua_pop(L, 1);
    return v;
}

/* The key a walk stopped on, as text for a line about it. A name is itself, a
 * number renders, and anything else has no text of its own and is named by its
 * type. What is converted is a copy, so the key the walk is standing on is
 * left exactly as it is and the next step still finds it. */
static void scnTableBadKey(lua_State *L, char *out, size_t outCap) {
    const char *s;

    if (out == NULL || outCap == 0) return;
    lua_pushvalue(L, -2);
    s = lua_tostring(L, -1);
    snprintf(out, outCap, "%s", (s != NULL) ? s : luaL_typename(L, -1));
    lua_pop(L, 1);
}

/* One value of a flat table as the text that is stored for it. A name is
 * itself and a number renders; a boolean has no text of its own, so it is
 * written out as the word Lua's own tostring would give it. allowBool says
 * whether the caller takes one at all — the init table does not, because its
 * values follow the -bot-init text form where a bare flag is "1", and two
 * spellings of yes in one table would be worse than turning the second down.
 *
 * Reading a number here rewrites it in place, which is safe: the walk is
 * already past the value. */
static const char *scnTableValueText(lua_State *L, int idx, bool allowBool) {
    if (allowBool && lua_type(L, idx) == LUA_TBOOLEAN) {
        return lua_toboolean(L, idx) ? "true" : "false";
    }
    return lua_tostring(L, idx);
}

static bool scnTableValueTakeable(lua_State *L, int idx, bool allowBool) {
    int t = lua_type(L, idx);
    return t == LUA_TSTRING || t == LUA_TNUMBER ||
           (allowBool && t == LUA_TBOOLEAN);
}

/* Walk a table already on the stack at tblIdx into out. The two readers below
 * differ only in how they get to the table and in what they take for a value,
 * so the walk itself is written once. */
static ScnTableRead scnTableWalk(lua_State *L, int tblIdx, ScnTable *out,
                                 char *badKey, size_t badCap,
                                 char *why, size_t whyCap, bool allowBool) {
    ScnTableRead r = SCN_TABLE_READ_OK;
    const char  *takes = allowBool ? "a string, a number or true/false"
                                   : "a string or a number";

    lua_pushnil(L);
    while (r == SCN_TABLE_READ_OK && lua_next(L, tblIdx) != 0) {
        /* The key is tested rather than read as a string: lua_tostring on a
           number key would rewrite it in place and break the walk. The value
           is safe to convert, since the walk is past it. */
        if (lua_type(L, -2) != LUA_TSTRING) {
            scnTableBadKey(L, badKey, badCap);
            if (why != NULL && whyCap > 0) {
                snprintf(why, whyCap, " has a key that is not a name");
            }
            r = SCN_TABLE_READ_BAD_KEY;
        } else if (!scnTableValueTakeable(L, -1, allowBool)) {
            if (badKey != NULL && badCap > 0) {
                snprintf(badKey, badCap, "%s", lua_tostring(L, -2));
            }
            if (why != NULL && whyCap > 0) {
                snprintf(why, whyCap, ".%s must be %s, got %s",
                         lua_tostring(L, -2), takes, luaL_typename(L, -1));
            }
            r = SCN_TABLE_READ_BAD_VALUE;
        } else if (!scnTableSet(out, lua_tostring(L, -2),
                                scnTableValueText(L, -1, allowBool))) {
            if (badKey != NULL && badCap > 0) {
                snprintf(badKey, badCap, "%s", lua_tostring(L, -2));
            }
            if (why != NULL && whyCap > 0) {
                snprintf(why, whyCap, ".%s does not fit", lua_tostring(L, -2));
            }
            r = SCN_TABLE_READ_NO_ROOM;
        }
        lua_pop(L, 1);   /* the value; the key stays for the next step */
    }
    if (r != SCN_TABLE_READ_OK) {
        lua_pop(L, 1);   /* the key the walk stopped on */
    }
    return r;
}

ScnTableRead scenarioLuaReadTable(lua_State *L, int idx, const char *key,
                                  ScnTable *out, char *badKey, size_t badCap,
                                  char *why, size_t whyCap) {
    ScnTableRead r;

    scnTableClear(out);
    if (badKey != NULL && badCap > 0) badKey[0] = '\0';
    if (why != NULL && whyCap > 0) why[0] = '\0';

    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return SCN_TABLE_READ_OK;
    }
    if (!lua_istable(L, -1)) {
        if (why != NULL && whyCap > 0) {
            snprintf(why, whyCap, " must be a table, got %s",
                     luaL_typename(L, -1));
        }
        lua_pop(L, 1);
        return SCN_TABLE_READ_NOT_TABLE;
    }

    r = scnTableWalk(L, lua_gettop(L), out, badKey, badCap, why, whyCap,
                     false);
    lua_pop(L, 1);       /* the table */
    return r;
}

/* The hint table, which is the argument itself rather than a field of one,
 * and which takes true and false beside strings and numbers. */
static ScnTableRead scnLuaReadArgTable(lua_State *L, int idx, ScnTable *out,
                                       char *badKey, size_t badCap,
                                       char *why, size_t whyCap) {
    scnTableClear(out);
    if (badKey != NULL && badCap > 0) badKey[0] = '\0';
    if (why != NULL && whyCap > 0) why[0] = '\0';
    return scnTableWalk(L, idx, out, badKey, badCap, why, whyCap, true);
}

/* The same read as a Lua argument. What did not fit is answered false, with
 * the key it stopped on in badKey, because the caller says how big a table a
 * roster row takes; everything else is refused the way any bad argument is.
 * Either way out holds the pairs read before the stop. */
static bool scnFieldTable(lua_State *L, int idx, const char *key,
                          ScnTable *out, char *badKey, size_t badCap) {
    char         why[SCN_TABLE_WHY_LEN];
    ScnTableRead r = scenarioLuaReadTable(L, idx, key, out, badKey, badCap,
                                          why, sizeof(why));

    if (r == SCN_TABLE_READ_OK) return true;
    if (r == SCN_TABLE_READ_NO_ROOM) return false;
    luaL_argerror(L, idx, lua_pushfstring(L, "t.%s%s", key, why));
    return false;   /* luaL_argerror does not come back */
}

/* What every roster row says when a pair of the table would not fit. */
static int scnTableTooBig(lua_State *L, const char *field, const char *key) {
    return scnRefused(L, SCN_OP_TOO_BIG,
                      "%s '%s' does not fit: at most %d pairs, a name of %d "
                      "bytes and a value of %d", field, key, SCN_TABLE_MAX,
                      (int)SCN_TABLE_KEY_LEN - 1,
                      (int)SCN_TABLE_VALUE_LEN - 1);
}

/* The brain a roster row names, turned into the path the bot loader opens.
 *
 * A scenario names a brain — the directory under the server's own brains/ —
 * rather than pathing to one, because a scenario shared with a server does not
 * know that server's layout. Both rows that carry a brain come here, so a
 * script writes a name wherever it writes a brain.
 *
 * A value with a path separator in it is refused, and told what to write
 * instead, so an author who wrote the old form learns it here rather than from
 * a seat that fields nothing. A name this server does not have is refused too:
 * left as the script wrote it, it reaches the sim as a path relative to
 * wherever the server was started, so spawn_bot{brain="init.lua"} would open
 * whatever file that name happens to hit. An empty brain is the seat's own, or
 * failing that the server's, and is left alone.
 *
 * Answers 0 for a row that should carry on, and otherwise the number of values
 * the refusal pushed, which is the row's own answer to the script. */
static int scnResolveOpBrain(lua_State *L, char *brain, size_t brainLen) {
    char path[SCN_PATH_MAX];

    if (brain[0] == '\0') {
        return 0;
    }
    if (strpbrk(brain, "/\\") != NULL) {
        return scnRefused(L, SCN_OP_NOT_FOUND,
                          "brain '%s' is a path; a scenario names a brain, "
                          "which is the directory under the server's brains/ "
                          "— 'GoalHunter_1.7', not a path to it", brain);
    }
    if (!brainListResolve(brain, path, sizeof(path))) {
        return scnRefused(L, SCN_OP_NOT_FOUND,
                          "brain '%s' names no brain this server has", brain);
    }
    snprintf(brain, brainLen, "%s", path);
    return 0;
}

static int scnLuaSpawnBot(lua_State *L) {
    ScenarioOp  op;
    ScnOpOut    out;
    ScnOpResult r;
    char        badKey[SCN_TABLE_KEY_LEN + 1];
    size_t      len   = 0;
    BYTE        start = SCN_NONE;   /* no start named: the engine picks */
    lua_Number  n;
    lua_Integer team, slot;
    int         loadout;
    int         refused;

    if (scnCheckingOnly(L)) {
        return scnCheckOnlyRefusal(L);
    }
    scnArgTable(L, 1, "t");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_SPAWN_BOT;

    if (!scnFieldText(L, 1, "name", op.u.rosterSpawnBot.name,
                      sizeof(op.u.rosterSpawnBot.name), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG, "name is %d bytes, limit %d",
                          (int)len,
                          (int)sizeof(op.u.rosterSpawnBot.name) - 1);
    }
    if (!scnFieldText(L, 1, "brain", op.u.rosterSpawnBot.brain,
                      sizeof(op.u.rosterSpawnBot.brain), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG, "brain is %d bytes, limit %d",
                          (int)len,
                          (int)sizeof(op.u.rosterSpawnBot.brain) - 1);
    }
    refused = scnResolveOpBrain(L, op.u.rosterSpawnBot.brain,
                                sizeof(op.u.rosterSpawnBot.brain));
    if (refused != 0) {
        return refused;
    }
    /* The brain mode and the level inside it, by the keys the brain's own
       modes.txt lists. Taken as text and matched by the sim, which is the
       side that has the brain and can say what keys it has. */
    if (!scnFieldText(L, 1, "mode", op.u.rosterSpawnBot.mode,
                      sizeof(op.u.rosterSpawnBot.mode), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG, "mode is %d bytes, limit %d",
                          (int)len,
                          (int)sizeof(op.u.rosterSpawnBot.mode) - 1);
    }
    if (!scnFieldText(L, 1, "difficulty", op.u.rosterSpawnBot.difficulty,
                      sizeof(op.u.rosterSpawnBot.difficulty), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG,
                          "difficulty is %d bytes, limit %d", (int)len,
                          (int)sizeof(op.u.rosterSpawnBot.difficulty) - 1);
    }
    team = scnFieldInt(L, 1, "team", 0);
    if (!scnFitsByte(team)) {
        return scnRefused(L, SCN_OP_RANGE, "team is %d", (int)team);
    }
    slot = scnFieldInt(L, 1, "slot", SCN_NONE);
    if (!scnFitsByte(slot)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "seat %d is not a seat",
                          (int)slot);
    }
    /* A start of nothing is the engine's own pick, which is also what the
       script's own numbering makes of zero. Any other number has to name a
       start that could exist; one that could not is refused here rather than
       reaching the op as another way of saying "you pick". */
    n = scnFieldNumber(L, 1, "start", 0);
    if (n != 0 && !scnStartIndex(n, &start)) {
        return scnRefused(L, SCN_OP_NO_SUCH_ITEM,
                          "start %f is no start: they are numbered 1 to 255, "
                          "and 0 leaves the pick to the engine", n);
    }
    loadout = scnFieldWord(L, 1, "loadout", &kScnLoadouts, 0);

    op.u.rosterSpawnBot.team    = (BYTE)team;
    op.u.rosterSpawnBot.slot    = (BYTE)slot;
    op.u.rosterSpawnBot.start   = start;
    op.u.rosterSpawnBot.loadout = (BYTE)loadout;
    if (!scnFieldTable(L, 1, "init", &op.u.rosterSpawnBot.init, badKey,
                       sizeof(badKey))) {
        return scnTableTooBig(L, "init", badKey);
    }

    memset(&out, 0, sizeof(out));
    r = serverSimApplyScenarioOp(scnCtx(L)->sim, &op, &out);
    if (r == SCN_OP_OK || r == SCN_OP_QUEUED) {
        /* The seat, when the op was told which one. A spawn that asked for
           the first free seat is not promised one until it lands. */
        if (out.slot == SCN_NONE) {
            lua_pushboolean(L, 1);
        } else {
            lua_pushinteger(L, (lua_Integer)out.slot);
        }
        if (r == SCN_OP_QUEUED) {
            lua_pushliteral(L, "queued");
            return 2;
        }
        return 1;
    }
    return scnRefused(L, r, "seat %d, team %d", (int)slot, (int)team);
}

static int scnLuaRemoveBot(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_ROSTER_REMOVE_BOT;
    op.u.rosterRemoveBot.slot  = (BYTE)p;
    return scnDone(L, &op, "player %d", (int)p);
}

static int scnLuaSetTeam(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");
    lua_Integer t = scnArgInt(L, 2, "t");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(t)) {
        return scnRefused(L, SCN_OP_RANGE, "team is %d", (int)t);
    }
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_ROSTER_SET_TEAM;
    op.u.rosterSetTeam.slot = (BYTE)p;
    op.u.rosterSetTeam.team = (BYTE)t;
    return scnDone(L, &op, "player %d to team %d", (int)p, (int)t);
}

/* New data for a bot already in the round. The table is the one a spawn's
 * init is: flat, names to text, and no larger than a spawn's, because it is
 * the same table on the other side — the bot's BRAIN_INIT is rebuilt from
 * it whole, so what is not in the table is not in the bot's any more.
 *
 * The seat has to hold a bot this server runs. A human is refused, an empty
 * seat is refused, and both say so under the codes the removal row already
 * answers with, because they are the same two questions asked of the same
 * kind of seat.
 *
 * It rides the roster queue with the spawns and the removals, so the write
 * into the brain's own Lua state happens where every other change to a bot
 * happens: on the producer thread, between ticks, never while the brain is
 * thinking. */
static int scnLuaBotInit(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");
    char        badKey[SCN_TABLE_KEY_LEN + 1];

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    scnArgTable(L, 2, "t");
    memset(&op, 0, sizeof(op));
    op.type                 = SCN_OP_ROSTER_BOT_INIT;
    op.u.rosterBotInit.slot = (BYTE)p;
    badKey[0]               = '\0';
    /* The reader takes a field of a table, so the argument is set as the
       one field of a table made for it: the same walk, the same limits and
       the same wording as spawn_bot's init field. */
    {
        char         why[SCN_TABLE_WHY_LEN];
        ScnTableRead r;

        lua_createtable(L, 0, 1);
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "t");
        r = scenarioLuaReadTable(L, lua_gettop(L), "t",
                                 &op.u.rosterBotInit.init, badKey,
                                 sizeof(badKey), why, sizeof(why));
        lua_pop(L, 1);
        if (r == SCN_TABLE_READ_NO_ROOM) {
            return scnTableTooBig(L, "init", badKey);
        }
        if (r != SCN_TABLE_READ_OK) {
            luaL_argerror(L, 2, lua_pushfstring(L, "t%s", why));
        }
    }
    return scnDone(L, &op, "player %d", (int)p);
}

static int scnLuaLobbyAddBot(lua_State *L) {
    ScenarioOp  op;
    ScnOpOut    out;
    ScnOpResult r;
    size_t      len = 0;
    lua_Integer team, slot;
    int         refused;

    if (scnCheckingOnly(L)) {
        return scnCheckOnlyRefusal(L);
    }
    scnArgTable(L, 1, "t");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LOBBY_ADD_BOT;

    if (!scnFieldText(L, 1, "name", op.u.lobbyAddBot.name,
                      sizeof(op.u.lobbyAddBot.name), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG, "name is %d bytes, limit %d",
                          (int)len, (int)sizeof(op.u.lobbyAddBot.name) - 1);
    }
    if (!scnFieldText(L, 1, "brain", op.u.lobbyAddBot.brain,
                      sizeof(op.u.lobbyAddBot.brain), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG, "brain is %d bytes, limit %d",
                          (int)len, (int)sizeof(op.u.lobbyAddBot.brain) - 1);
    }
    refused = scnResolveOpBrain(L, op.u.lobbyAddBot.brain,
                                sizeof(op.u.lobbyAddBot.brain));
    if (refused != 0) {
        return refused;
    }
    /* The brain mode and the level inside it, as spawn_bot takes them. */
    if (!scnFieldText(L, 1, "mode", op.u.lobbyAddBot.mode,
                      sizeof(op.u.lobbyAddBot.mode), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG, "mode is %d bytes, limit %d",
                          (int)len, (int)sizeof(op.u.lobbyAddBot.mode) - 1);
    }
    if (!scnFieldText(L, 1, "difficulty", op.u.lobbyAddBot.difficulty,
                      sizeof(op.u.lobbyAddBot.difficulty), &len)) {
        return scnRefused(L, SCN_OP_TOO_BIG,
                          "difficulty is %d bytes, limit %d", (int)len,
                          (int)sizeof(op.u.lobbyAddBot.difficulty) - 1);
    }
    team = scnFieldInt(L, 1, "team", 0);
    if (!scnFitsByte(team)) {
        return scnRefused(L, SCN_OP_RANGE, "team is %d", (int)team);
    }
    slot = scnFieldInt(L, 1, "slot", SCN_NONE);
    if (!scnFitsByte(slot)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "seat %d is not a seat",
                          (int)slot);
    }
    op.u.lobbyAddBot.team    = (BYTE)team;
    op.u.lobbyAddBot.slot    = (BYTE)slot;
    /* False asks for the seat without the bot: it is held in the roster and
       loads no brain until a spawn names it. */
    op.u.lobbyAddBot.fielded = scnFieldBool(L, 1, "fielded", true);

    memset(&out, 0, sizeof(out));
    r = serverSimApplyScenarioOp(scnCtx(L)->sim, &op, &out);
    if (r == SCN_OP_OK) {
        lua_pushinteger(L, (lua_Integer)out.slot);
        return 1;
    }
    return scnRefused(L, r, "seat %d, team %d", (int)slot, (int)team);
}

static int scnLuaLobbyRemoveBot(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    memset(&op, 0, sizeof(op));
    op.type                   = SCN_OP_LOBBY_REMOVE_BOT;
    op.u.lobbyRemoveBot.slot  = (BYTE)p;
    return scnDone(L, &op, "player %d", (int)p);
}

static int scnLuaLobbySetTeam(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p = scnArgInt(L, 1, "p");
    lua_Integer t = scnArgInt(L, 2, "t");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(t)) {
        return scnRefused(L, SCN_OP_RANGE, "team is %d", (int)t);
    }
    memset(&op, 0, sizeof(op));
    op.type                = SCN_OP_LOBBY_SET_TEAM;
    op.u.lobbySetTeam.slot = (BYTE)p;
    op.u.lobbySetTeam.team = (BYTE)t;
    return scnDone(L, &op, "player %d to team %d", (int)p, (int)t);
}

/* ── Bots ─────────────────────────────────────────────────────────── */

/* An order for one bot's brain: a flat table with a verb in it, and whatever
 * else the brain it was written for reads. Nothing here knows what any of it
 * means — the engine marshals the table and the brain is the only thing that
 * reads it — so every pair but `verb` is carried through untouched.
 *
 * A missing verb RAISES rather than being refused or passed on. Every other
 * shape mistake in this file raises, because the shape of an argument is the
 * author's business and knowable without the round; and a hint with no verb
 * is the one mistake that would otherwise be invisible, since a brain quietly
 * ignores a table it cannot read. A refusal would be wrong for the same
 * reason: there is no state of the round that makes a verb appear. */
static int scnLuaHint(lua_State *L) {
    ScenarioOp   op;
    ScnTableRead tr;
    char         badKey[SCN_TABLE_KEY_LEN + 1];
    char         why[SCN_TABLE_WHY_LEN];
    const char  *verb;
    lua_Integer  p = scnArgInt(L, 1, "p");

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    scnArgTable(L, 2, "t");
    memset(&op, 0, sizeof(op));
    op.type              = SCN_OP_BOT_HINT;
    op.u.botHint.slot    = (BYTE)p;

    tr = scnLuaReadArgTable(L, 2, &op.u.botHint.hint, badKey, sizeof(badKey),
                            why, sizeof(why));
    if (tr == SCN_TABLE_READ_NO_ROOM) {
        return scnTableTooBig(L, "hint", badKey);
    }
    if (tr != SCN_TABLE_READ_OK) {
        return luaL_argerror(L, 2, lua_pushfstring(L, "t%s", why));
    }

    verb = scnTableGet(&op.u.botHint.hint, "verb");
    if (verb == NULL || verb[0] == '\0') {
        return luaL_argerror(L, 2,
                             "t.verb is the word that says what the order "
                             "is, and a hint has to carry one");
    }
    return scnDone(L, &op, "player %d, verb '%s'", (int)p, verb);
}

/* ── Comms ────────────────────────────────────────────────────────── */

/* One line, to everyone, to a team or to a seat. Three ops rather than one
 * with a target byte, which is how the funnel spells the three. */
static int scnLuaMessage(lua_State *L) {
    ScenarioOp       op;
    size_t           len = 0;
    const char      *text = scnArgText(L, 1, "text", &len);
    lua_Integer      to   = 0;
    ScnLuaTargetKind kind = scnArgTarget(L, 2, &to);

    memset(&op, 0, sizeof(op));
    if (len >= SCN_TEXT_MAX) {
        return scnRefused(L, SCN_OP_TOO_BIG, "text is %d bytes, limit %d",
                          (int)len, (int)SCN_TEXT_MAX - 1);
    }
    if (!scnFitsByte(to)) {
        return scnRefused(L,
                          (kind == SCN_LUA_TO_TEAM) ? SCN_OP_RANGE
                                                    : SCN_OP_NO_SUCH_PLAYER,
                          "target is %d", (int)to);
    }
    switch (kind) {
        case SCN_LUA_TO_PLAYER:
            op.type                = SCN_OP_MSG_PLAYER;
            op.u.msgPlayer.slot    = (BYTE)to;
            memcpy(op.u.msgPlayer.text, text, len + 1);
            break;
        case SCN_LUA_TO_TEAM:
            op.type            = SCN_OP_MSG_TEAM;
            op.u.msgTeam.team  = (BYTE)to;
            memcpy(op.u.msgTeam.text, text, len + 1);
            break;
        case SCN_LUA_TO_ALL:
        default:
            op.type = SCN_OP_MSG_ALL;
            memcpy(op.u.msgAll.text, text, len + 1);
            break;
    }
    return scnDone(L, &op, "%d bytes to %d", (int)len, (int)to);
}

/* A line one seat says.
 *
 * The line game.message writes is the server's, and a server line never
 * enters a brain's inbox: a brain reads chat. This is a seat's own chat
 * line, so a scripted round can hand a bot exactly what a human ally typing
 * would hand it, from a seat nobody is sitting in. It fires on_chat too,
 * with the sender named and scripted true.
 *
 * Who hears it is the three a player has: their own team with no target,
 * the whole game with "all", and one seat with a seat number. A team the
 * sender is not on is not among them, because the chat path refuses a line
 * addressed to one whoever sends it.
 */
static int scnLuaSay(lua_State *L) {
    ScenarioOp  op;
    size_t      len  = 0;
    lua_Integer p    = scnArgInt(L, 1, "p");
    const char *text = scnArgText(L, 2, "text", &len);
    BYTE        mode = SCN_SAY_TEAM;
    lua_Integer to   = 0;

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (len >= SCN_TEXT_MAX) {
        return scnRefused(L, SCN_OP_TOO_BIG, "text is %d bytes, limit %d",
                          (int)len, (int)SCN_TEXT_MAX - 1);
    }
    /* A line with nothing in it arrives nowhere: every receiver drops a chat
       body of no length. Refused here rather than accepted and lost. */
    if (len == 0) {
        return scnRefused(L, SCN_OP_BAD_CALL, "the line is empty");
    }
    /* No target is the seat's own team, which is what a scenario handing a
       bot an order almost always wants. */
    if (!lua_isnoneornil(L, 3)) {
        if (lua_type(L, 3) == LUA_TSTRING) {
            const char *word = lua_tostring(L, 3);
            if (strcmp(word, "all") != 0 && strcmp(word, "team") != 0) {
                return scnRefused(L, SCN_OP_BAD_CALL,
                                  "target is \"%s\", not \"all\" or \"team\"",
                                  word);
            }
            mode = (strcmp(word, "all") == 0) ? SCN_SAY_ALL : SCN_SAY_TEAM;
        } else {
            to = scnArgInt(L, 3, "target");
            if (!scnFitsByte(to)) {
                return scnRefused(L, SCN_OP_NO_SUCH_PLAYER,
                                  "target %d is not a seat", (int)to);
            }
            mode = SCN_SAY_PLAYER;
        }
    }
    memset(&op, 0, sizeof(op));
    op.type            = SCN_OP_MSG_SAY;
    op.u.msgSay.slot   = (BYTE)p;
    op.u.msgSay.mode   = mode;
    op.u.msgSay.target = (BYTE)to;
    memcpy(op.u.msgSay.text, text, len + 1);
    return scnDone(L, &op, "%d bytes from player %d, mode %d", (int)len,
                   (int)p, (int)mode);
}

static int scnLuaSound(lua_State *L) {
    ScenarioOp  op;
    int         sound = scnArgWord(L, 1, "name", &kScnSounds);
    lua_Integer x     = scnOptInt(L, 2, "x", SCN_NONE);
    lua_Integer y     = scnOptInt(L, 3, "y", SCN_NONE);

    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    memset(&op, 0, sizeof(op));
    op.type          = SCN_OP_SOUND;
    op.u.sound.sound = (BYTE)sound;
    op.u.sound.x     = (BYTE)x;
    op.u.sound.y     = (BYTE)y;
    return scnDone(L, &op, "sound %d at (%d, %d)", sound, (int)x, (int)y);
}

static int scnLuaLog(lua_State *L) {
    ScenarioOp  op;
    size_t      len  = 0;
    const char *text = scnArgText(L, 1, "text", &len);

    if (len >= SCN_TEXT_MAX) {
        return scnRefused(L, SCN_OP_TOO_BIG, "text is %d bytes, limit %d",
                          (int)len, (int)SCN_TEXT_MAX - 1);
    }
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LOG;
    memcpy(op.u.log.text, text, len + 1);
    return scnDone(L, &op, "%d bytes", (int)len);
}

/* ── Presentation ─────────────────────────────────────────────────── */

/* The player bit in a presentation op's target byte. The layout is written
 * down beside the payloads in scenario_defs.h — 0 for everyone,
 * 1..MAX_TANKS-1 for a team, and this bit over a 0-based slot for one seat —
 * and the arm unpacks with the same bit. */
#define SCN_LUA_TARGET_PLAYER 0x80

/* The colour a marker takes when a script names none. It has to be
 * something: index 0 is the palette's "draw nothing", so a marker left at
 * whatever the memset put there would not appear at all. */
#define SCN_LUA_MARKER_COLOUR ((lua_Integer)SCN_PANEL_COLOUR_YELLOW)

/* Who a presentation op is addressed to, as the one byte its payload
 * carries. The three forms are the ones every other row takes, read by the
 * same helper, so "to a team" is written one way across the surface.
 *
 * False for a target that will not pack: a seat or a team outside the
 * roster, and team 0, which is the team a seat sits on before it is given
 * one rather than a destination — the arm reads a 0 as everyone, so a script
 * asking for team 0 is refused here rather than told the whole game. asked
 * is the number the script wrote, for the sentence the row answers with. */
static bool scnPresentationTarget(lua_State *L, int idx, BYTE *out,
                                  lua_Integer *asked) {
    ScnLuaTargetKind kind = scnArgTarget(L, idx, asked);

    switch (kind) {
        case SCN_LUA_TO_PLAYER:
            if (*asked < 0 || *asked >= MAX_TANKS) {
                return false;
            }
            *out = (BYTE)(SCN_LUA_TARGET_PLAYER | (BYTE)*asked);
            return true;
        case SCN_LUA_TO_TEAM:
            if (*asked <= 0 || *asked >= MAX_TANKS) {
                return false;
            }
            *out = (BYTE)*asked;
            return true;
        case SCN_LUA_TO_ALL:
        default:
            *out = 0;
            return true;
    }
}

/* A colour argument: the palette's word, the number itself, or nothing at
 * all, which takes the row's own default. A name the palette does not hold
 * raises, the way every other word argument does; a number outside it is a
 * value the parser would turn down, so it comes back false and the row
 * refuses it. */
static bool scnArgColour(lua_State *L, int idx, lua_Integer def,
                         lua_Integer *out) {
    if (lua_isnoneornil(L, idx)) {
        *out = def;
        return true;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        *out = scnArgWord(L, idx, "colour", &kScnColours);
        return true;
    }
    *out = scnArgInt(L, idx, "colour");
    return *out >= 0 && *out < (lua_Integer)SCN_PANEL_COLOURS;
}

/* ── The panel's display list ─────────────────────────────────────── */

/* A list is an array of primitives and a primitive is an array whose first
 * element is the opcode word, so an operand is read by position, in the
 * order the panel's own byte layout gives — with a text primitive's string
 * standing where the layout writes a length.
 *
 * A malformed entry raises, naming the entry and the operand: an author who
 * wrote the wrong shape has a bug rather than a refusal waiting. A value the
 * sim would turn down is answered as a refusal the script reads, the way
 * every other row answers one.
 *
 * Every operand rule scnPanelParse holds a list to is answered here, entry
 * by entry. That is what lets the row read scnPanelWrite's one 0 as "the
 * bytes did not fit": nothing else it refuses an item for is left. */

/* What kind of operand sits at one position, so the order is written once
 * as data rather than seven times as code. */
typedef enum {
    SCN_ARG_END = 0,
    SCN_ARG_BYTE,     /* a coordinate, a width, a height, a tile id */
    SCN_ARG_COLOUR,
    SCN_ARG_SIZE,
    SCN_ARG_ALIGN,
    SCN_ARG_MODE,
    SCN_ARG_FILL,     /* true or false, or the 0 or 1 the byte carries */
    SCN_ARG_SLOT,     /* the seat a name primitive names */
    SCN_ARG_U16,      /* a bar's value and max */
    SCN_ARG_TICK,     /* a timer's game tick */
    SCN_ARG_TEXT      /* a text primitive's bytes */
} ScnPanelArg;

/* Room for one primitive's operands and the SCN_ARG_END that closes its row.
 * Seven operands is the most any of them takes — the bar's and the timer's —
 * so eight covers the widest row and its end. */
#define SCN_PANEL_ARGS_MAX 8

/* The operands of each primitive, in the order the list writes them. Indexed
 * by opcode, so the row a primitive reads is the row its opcode names. */
static const struct {
    ScnPanelArg kind;
    const char *name;
} kScnPanelArgs[SCN_PANEL_OP_TIMER + 1][SCN_PANEL_ARGS_MAX] = {
    /* 0 is not a primitive */
    { { SCN_ARG_END, NULL } },
    /* rect */
    { { SCN_ARG_BYTE, "x" }, { SCN_ARG_BYTE, "y" }, { SCN_ARG_BYTE, "w" },
      { SCN_ARG_BYTE, "h" }, { SCN_ARG_COLOUR, "colour" },
      { SCN_ARG_FILL, "fill" }, { SCN_ARG_END, NULL } },
    /* line */
    { { SCN_ARG_BYTE, "x0" }, { SCN_ARG_BYTE, "y0" }, { SCN_ARG_BYTE, "x1" },
      { SCN_ARG_BYTE, "y1" }, { SCN_ARG_COLOUR, "colour" },
      { SCN_ARG_END, NULL } },
    /* text */
    { { SCN_ARG_BYTE, "x" }, { SCN_ARG_BYTE, "y" },
      { SCN_ARG_COLOUR, "colour" }, { SCN_ARG_SIZE, "size" },
      { SCN_ARG_ALIGN, "align" }, { SCN_ARG_TEXT, "text" },
      { SCN_ARG_END, NULL } },
    /* name */
    { { SCN_ARG_BYTE, "x" }, { SCN_ARG_BYTE, "y" },
      { SCN_ARG_COLOUR, "colour" }, { SCN_ARG_SIZE, "size" },
      { SCN_ARG_ALIGN, "align" }, { SCN_ARG_SLOT, "p" },
      { SCN_ARG_END, NULL } },
    /* sprite */
    { { SCN_ARG_BYTE, "x" }, { SCN_ARG_BYTE, "y" }, { SCN_ARG_BYTE, "tile" },
      { SCN_ARG_END, NULL } },
    /* bar */
    { { SCN_ARG_BYTE, "x" }, { SCN_ARG_BYTE, "y" }, { SCN_ARG_BYTE, "w" },
      { SCN_ARG_BYTE, "h" }, { SCN_ARG_COLOUR, "colour" },
      { SCN_ARG_U16, "value" }, { SCN_ARG_U16, "max" }, { SCN_ARG_END, NULL } },
    /* timer */
    { { SCN_ARG_BYTE, "x" }, { SCN_ARG_BYTE, "y" },
      { SCN_ARG_COLOUR, "colour" }, { SCN_ARG_SIZE, "size" },
      { SCN_ARG_ALIGN, "align" }, { SCN_ARG_MODE, "mode" },
      { SCN_ARG_TICK, "tick" }, { SCN_ARG_END, NULL } },
};

/* The word a primitive begins with, as the opcode byte it names. A word the
 * set does not hold raises, the way every other word argument does. */
static int scnPanelOpcode(lua_State *L, int entry, int n) {
    const char *word;
    lua_Integer op = 0;

    lua_rawgeti(L, entry, 1);
    if (lua_type(L, -1) != LUA_TSTRING) {
        return luaL_error(L,
                          "list entry %d begins with %s, and a primitive "
                          "begins with the word that names it", n,
                          luaL_typename(L, -1));
    }
    word = lua_tostring(L, -1);
    if (!scnWordValue(&kScnPanelOps, word, &op)) {
        return luaL_error(L, "list entry %d is '%s', which is not %s", n, word,
                          kScnPanelOps.what);
    }
    lua_pop(L, 1);
    return (int)op;
}

/* One operand, as the whole number it arrived as. A missing one and one that
 * is not a number both raise, naming the entry and the operand. */
static lua_Integer scnPanelOperand(lua_State *L, int entry, int n, int pos,
                                   const char *name) {
    lua_Integer v;

    lua_rawgeti(L, entry, pos);
    if (lua_type(L, -1) != LUA_TNUMBER) {
        if (lua_isnoneornil(L, -1)) {
            return (lua_Integer)luaL_error(L, "list entry %d has no %s", n,
                                           name);
        }
        return (lua_Integer)luaL_error(
            L, "list entry %d: %s is %s, not a number", n, name,
            luaL_typename(L, -1));
    }
    v = scnWhole(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return v;
}

/* What a primitive's own operand is refused with. */
static int scnPanelRange(lua_State *L, int n, const char *name,
                         lua_Integer got, lua_Integer lo, lua_Integer hi) {
    return scnRefused(L, SCN_OP_RANGE,
                      "list entry %d: %s is %d, and it runs %d to %d", n, name,
                      (int)got, (int)lo, (int)hi);
}

/* An operand that fills one byte. */
static bool scnPanelByte(lua_State *L, int entry, int n, int pos,
                         const char *name, lua_Integer *out) {
    *out = scnPanelOperand(L, entry, n, pos, name);
    return scnFitsByte(*out);
}

/* An operand a script may write as a word or as the number itself. A word
 * the set does not hold raises; a number the set does not reach is refused,
 * because each of these sets names every value its field carries, counting
 * from zero. */
static bool scnPanelWord(lua_State *L, int entry, int n, int pos,
                         const char *name, const ScnLuaWordSet *set,
                         lua_Integer *out) {
    lua_rawgeti(L, entry, pos);
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *word = lua_tostring(L, -1);

        if (scnWordValue(set, word, out)) {
            lua_pop(L, 1);
            return true;
        }
        luaL_error(L, "list entry %d: %s is '%s', which is not %s", n, name,
                   word, set->what);
        return false;   /* luaL_error does not return */
    }
    lua_pop(L, 1);
    *out = scnPanelOperand(L, entry, n, pos, name);
    return *out >= 0 && *out < (lua_Integer)set->count;
}

/* A rect's fill. A script writes true or false for it and the byte carries 0
 * or 1, so both are taken. */
static bool scnPanelFill(lua_State *L, int entry, int n, int pos,
                         lua_Integer *out) {
    lua_rawgeti(L, entry, pos);
    if (lua_isboolean(L, -1)) {
        *out = lua_toboolean(L, -1) ? 1 : 0;
        lua_pop(L, 1);
        return true;
    }
    lua_pop(L, 1);
    *out = scnPanelOperand(L, entry, n, pos, "fill");
    return *out == 0 || *out == 1;
}

/* The bytes of a text primitive, into the item's own buffer. False for a
 * string longer than one primitive carries, and for one holding a byte no
 * panel draws — both are lists the parser turns down, so both are refusals
 * rather than raises. bad is the 1-based position of the offending byte, and
 * 0 when it was the length. */
static bool scnPanelText(lua_State *L, int entry, int n, int pos,
                         ScnPanelItem *item, size_t *len, int *bad) {
    const char *s;
    size_t      i;

    *len = 0;
    *bad = 0;
    lua_rawgeti(L, entry, pos);
    if (lua_type(L, -1) != LUA_TSTRING) {
        if (lua_isnoneornil(L, -1)) {
            luaL_error(L, "list entry %d has no text", n);
        }
        luaL_error(L, "list entry %d: text is %s, not a string", n,
                   luaL_typename(L, -1));
        return false;   /* luaL_error does not return */
    }
    s = lua_tolstring(L, -1, len);
    if (*len > SCN_PANEL_TEXT_MAX) {
        lua_pop(L, 1);
        return false;
    }
    for (i = 0; i < *len; i++) {
        unsigned char c = (unsigned char)s[i];
        /* The parser's own rule: a control byte or DEL is refused so a script
           cannot write a newline or an escape into anyone's draw. */
        if (c < 0x20 || c == 0x7F) {
            *bad = (int)i + 1;
            lua_pop(L, 1);
            return false;
        }
    }
    memcpy(item->u.text.text, s, *len);
    item->u.text.text[*len] = '\0';
    item->u.text.len        = (uint8_t)*len;
    lua_pop(L, 1);
    return true;
}

/* One primitive off the list, into the item. Answers 0 when the item is
 * filled, and the number of results it pushed when the sim would turn the
 * primitive down — the row hands that straight back, so a refusal from
 * inside a list reads like any other. A malformed entry raises and does not
 * return.
 *
 * The entry table is dropped before a filled item is answered for, so a list
 * of 128 primitives does not need a stack 128 deep. A refusal leaves it
 * where it is: the three results it pushed sit above it and are what the row
 * hands back. */
static int scnPanelItem(lua_State *L, int listIdx, int n, ScnPanelItem *item) {
    lua_Integer v[SCN_PANEL_ARGS_MAX];
    size_t      len = 0;
    int         bad = 0;
    int         entry;
    int         i;

    lua_rawgeti(L, listIdx, n);
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "list entry %d is %s, and a primitive is a table",
                          n, luaL_typename(L, -1));
    }
    entry    = lua_gettop(L);
    item->op = (uint8_t)scnPanelOpcode(L, entry, n);

    memset(v, 0, sizeof(v));
    for (i = 0; kScnPanelArgs[item->op][i].kind != SCN_ARG_END; i++) {
        const char *name = kScnPanelArgs[item->op][i].name;
        int         pos  = i + 2;   /* the name is at 1, so operands start at 2 */

        switch (kScnPanelArgs[item->op][i].kind) {
            case SCN_ARG_BYTE:
                if (!scnPanelByte(L, entry, n, pos, name, &v[i])) {
                    return scnPanelRange(L, n, name, v[i], 0, 255);
                }
                break;
            case SCN_ARG_COLOUR:
                if (!scnPanelWord(L, entry, n, pos, name, &kScnColours,
                                  &v[i])) {
                    return scnPanelRange(L, n, name, v[i], 0,
                                         SCN_PANEL_COLOURS - 1);
                }
                break;
            case SCN_ARG_SIZE:
                if (!scnPanelWord(L, entry, n, pos, name, &kScnSizes, &v[i])) {
                    return scnPanelRange(L, n, name, v[i], 0,
                                         SCN_PANEL_SIZE_LARGE);
                }
                break;
            case SCN_ARG_ALIGN:
                if (!scnPanelWord(L, entry, n, pos, name, &kScnAligns, &v[i])) {
                    return scnPanelRange(L, n, name, v[i], 0,
                                         SCN_PANEL_ALIGN_RIGHT);
                }
                break;
            case SCN_ARG_MODE:
                if (!scnPanelWord(L, entry, n, pos, name, &kScnTimerModes,
                                  &v[i])) {
                    return scnPanelRange(L, n, name, v[i], 0,
                                         SCN_PANEL_TIMER_UP);
                }
                break;
            case SCN_ARG_FILL:
                if (!scnPanelFill(L, entry, n, pos, &v[i])) {
                    return scnPanelRange(L, n, name, v[i], 0, 1);
                }
                break;
            case SCN_ARG_SLOT:
                v[i] = scnPanelOperand(L, entry, n, pos, name);
                if (v[i] < 0 || v[i] >= MAX_TANKS) {
                    return scnPanelRange(L, n, name, v[i], 0, MAX_TANKS - 1);
                }
                break;
            case SCN_ARG_U16:
                v[i] = scnPanelOperand(L, entry, n, pos, name);
                if (v[i] < 0 || v[i] > 0xFFFF) {
                    return scnPanelRange(L, n, name, v[i], 0, 0xFFFF);
                }
                break;
            case SCN_ARG_TICK:
                /* A tick is four bytes on the wire and an argument is read in
                   the int window, so the window's own top is the ceiling. At
                   a hundred ticks a second it is months of round. */
                v[i] = scnPanelOperand(L, entry, n, pos, name);
                if (v[i] < 0) {
                    return scnPanelRange(L, n, name, v[i], 0, SCN_LUA_INT_MAX);
                }
                break;
            case SCN_ARG_TEXT:
            default:
                if (!scnPanelText(L, entry, n, pos, item, &len, &bad)) {
                    if (bad > 0) {
                        return scnRefused(
                            L, SCN_OP_RANGE,
                            "list entry %d: byte %d of the text is one no "
                            "panel draws", n, bad);
                    }
                    return scnRefused(L, SCN_OP_TOO_BIG,
                                      "list entry %d: the text is %d bytes, "
                                      "limit %d", n, (int)len,
                                      SCN_PANEL_TEXT_MAX);
                }
                break;
        }
    }

    switch (item->op) {
        case SCN_PANEL_OP_RECT:
            item->u.rect.x      = (uint8_t)v[0];
            item->u.rect.y      = (uint8_t)v[1];
            item->u.rect.w      = (uint8_t)v[2];
            item->u.rect.h      = (uint8_t)v[3];
            item->u.rect.colour = (uint8_t)v[4];
            item->u.rect.fill   = (uint8_t)v[5];
            /* The one rect the parser turns down: on the wire it is the mark
               that makes the item before it large. It draws nothing anyway. */
            if (scnPanelIsSizeMark(item)) {
                return scnRefused(L, SCN_OP_RANGE,
                                  "list entry %d: a colourless empty rect at "
                                  "x %d, y 0 is reserved", n,
                                  (int)SCN_PANEL_SIZE_LARGE);
            }
            break;
        case SCN_PANEL_OP_LINE:
            item->u.line.x0     = (uint8_t)v[0];
            item->u.line.y0     = (uint8_t)v[1];
            item->u.line.x1     = (uint8_t)v[2];
            item->u.line.y1     = (uint8_t)v[3];
            item->u.line.colour = (uint8_t)v[4];
            break;
        case SCN_PANEL_OP_TEXT:
            /* The bytes and the length are the text reader's: it copied them
               into the item while the string was still on the stack. */
            item->u.text.x      = (uint8_t)v[0];
            item->u.text.y      = (uint8_t)v[1];
            item->u.text.colour = (uint8_t)v[2];
            item->u.text.size   = (uint8_t)v[3];
            item->u.text.align  = (uint8_t)v[4];
            break;
        case SCN_PANEL_OP_NAME:
            item->u.name.x      = (uint8_t)v[0];
            item->u.name.y      = (uint8_t)v[1];
            item->u.name.colour = (uint8_t)v[2];
            item->u.name.size   = (uint8_t)v[3];
            item->u.name.align  = (uint8_t)v[4];
            item->u.name.slot   = (uint8_t)v[5];
            break;
        case SCN_PANEL_OP_SPRITE:
            item->u.sprite.x    = (uint8_t)v[0];
            item->u.sprite.y    = (uint8_t)v[1];
            item->u.sprite.tile = (uint8_t)v[2];
            break;
        case SCN_PANEL_OP_BAR:
            item->u.bar.x      = (uint8_t)v[0];
            item->u.bar.y      = (uint8_t)v[1];
            item->u.bar.w      = (uint8_t)v[2];
            item->u.bar.h      = (uint8_t)v[3];
            item->u.bar.colour = (uint8_t)v[4];
            item->u.bar.value  = (uint16_t)v[5];
            item->u.bar.max    = (uint16_t)v[6];
            break;
        case SCN_PANEL_OP_TIMER:
        default:
            item->u.timer.x      = (uint8_t)v[0];
            item->u.timer.y      = (uint8_t)v[1];
            item->u.timer.colour = (uint8_t)v[2];
            item->u.timer.size   = (uint8_t)v[3];
            item->u.timer.align  = (uint8_t)v[4];
            item->u.timer.mode   = (uint8_t)v[5];
            item->u.timer.tick   = (uint32_t)v[6];
            break;
    }
    lua_pop(L, 1);   /* the entry */
    return 0;
}

static int scnLuaPanel(lua_State *L) {
    ScenarioOp   op;
    ScnPanelList list;
    lua_Integer  id     = scnArgInt(L, 1, "id");
    lua_Integer  asked  = 0;
    BYTE         target = 0;
    size_t       count;
    size_t       n;

    scnArgTable(L, 2, "list");
    if (!scnFitsByte(id)) {
        return scnRefused(L, SCN_OP_RANGE, "panel %d is not a panel", (int)id);
    }
    if (!scnPresentationTarget(L, 3, &target, &asked)) {
        return scnRefused(L, SCN_OP_RANGE, "target is %d", (int)asked);
    }
    count = lua_rawlen(L, 2);
    if (count > SCN_PANEL_ITEMS_MAX) {
        return scnRefused(L, SCN_OP_TOO_BIG,
                          "the list has %d primitives, limit %d", (int)count,
                          SCN_PANEL_ITEMS_MAX);
    }

    memset(&op, 0, sizeof(op));
    memset(&list, 0, sizeof(list));
    list.count = (uint8_t)count;
    for (n = 1; n <= count; n++) {
        int refused = scnPanelItem(L, 2, (int)n, &list.items[n - 1]);
        if (refused != 0) {
            return refused;
        }
    }
    /* Each large item costs a second primitive on the wire, and the limit is
       on those, since an older client counts them. */
    if (scnPanelWireCount(&list) > SCN_PANEL_ITEMS_MAX) {
        return scnRefused(L, SCN_OP_TOO_BIG,
                          "the list comes to %d primitives with each large "
                          "item counted twice, limit %d",
                          (int)scnPanelWireCount(&list), SCN_PANEL_ITEMS_MAX);
    }
    op.type           = SCN_OP_PANEL;
    op.u.panel.target = target;
    op.u.panel.panel  = (BYTE)id;
    /* Which script this list is from, so each script keeps a panel of its
       own on the client instead of overwriting the others'. runningOwner is
       that script's position on the list plus one, and SCN_OWNER_NONE a call
       no script of the round's made; both of the latter read as owner 0. */
    {
        const ScnLuaCtx *c = scnCtx(L);
        if (c != NULL && c->runningOwner != SCN_OWNER_NONE &&
            c->runningOwner <= SCN_PANEL_OWNERS) {
            op.u.panel.owner = (BYTE)(c->runningOwner - 1);
        }
    }
    /* An empty list is the clear, and it never reaches the writer: 0 is the
       writer's answer both for a list it would not take and for one with
       nothing in it, and the count is what tells those apart. Every other
       reason it has to refuse an item has been answered above, item by item,
       so what a 0 means here is that the bytes did not fit. */
    if (count > 0) {
        uint16_t len = scnPanelWrite(&list, op.u.panel.bytes, SCN_PANEL_MAX);
        if (len == 0) {
            return scnRefused(L, SCN_OP_TOO_BIG,
                              "the list does not fit the %d bytes a panel "
                              "carries", SCN_PANEL_MAX);
        }
        op.u.panel.len = len;
    }
    return scnDone(L, &op, "panel %d, %d primitives, %d bytes", (int)id,
                   (int)count, (int)op.u.panel.len);
}

/* ── The other three ──────────────────────────────────────────────── */

static int scnLuaScore(lua_State *L) {
    ScenarioOp       op;
    lua_Integer      to    = 0;
    ScnLuaTargetKind kind;
    lua_Number       value;
    const char      *label = "";
    const char      *why;
    size_t           len   = 0;

    /* The scenario's own score is the number a player reads as who is
       winning, and a scenario that keeps one usually ends the round on it,
       so a mod does not get to write one. */
    why = scnNotTheDecider(L);
    if (why != NULL) {
        return scnDecidesRefusal(L, "game.score", why);
    }
    kind  = scnArgTarget(L, 1, &to);
    value = scnArgNumber(L, 2, "value");

    /* A score is one seat's or one team's. There is no everyone's, so the
       two forms scnArgTarget reads as "the whole game" are the call being
       written wrong rather than a destination the sim would refuse. */
    if (kind == SCN_LUA_TO_ALL) {
        return luaL_argerror(L, 1,
                             "target must be a player or { team = t }: a score "
                             "is one seat's or one team's");
    }
    if (!lua_isnoneornil(L, 3)) {
        label = scnArgText(L, 3, "label", &len);
    }
    /* Negated, so a NaN takes the branch rather than falling through it. */
    if (!(value >= (lua_Number)SCN_LUA_INT_MIN &&
          value <= (lua_Number)SCN_LUA_INT_MAX)) {
        return scnRefused(L, SCN_OP_RANGE, "a score runs %d to %d",
                          SCN_LUA_INT_MIN, SCN_LUA_INT_MAX);
    }
    memset(&op, 0, sizeof(op));
    /* The arm refuses a label with no terminator inside its own field rather
       than reading past the end of one, so the row holds the text against
       the field less the byte the terminator wants. */
    if (len >= sizeof(op.u.score.label)) {
        return scnRefused(L, SCN_OP_TOO_BIG, "label is %d bytes, limit %d",
                          (int)len, (int)sizeof(op.u.score.label) - 1);
    }
    if (kind == SCN_LUA_TO_PLAYER) {
        if (!scnFitsByte(to)) {
            return scnRefused(L, SCN_OP_NO_SUCH_PLAYER,
                              "player %d is not a seat", (int)to);
        }
        op.u.score.kind = SCN_SCORE_KIND_PLAYER;
    } else {
        if (!scnFitsByte(to)) {
            return scnRefused(L, SCN_OP_RANGE, "team %d is not a team",
                              (int)to);
        }
        op.u.score.kind = SCN_SCORE_KIND_TEAM;
    }
    op.type            = SCN_OP_SCORE;
    op.u.score.target  = (BYTE)to;
    op.u.score.score   = (int32_t)scnWhole(value);
    memcpy(op.u.score.label, label, len + 1);
    return scnDone(L, &op, "%s %d, score %d",
                   (kind == SCN_LUA_TO_TEAM) ? "team" : "player", (int)to,
                   (int)op.u.score.score);
}

static int scnLuaAnnounce(lua_State *L) {
    ScenarioOp  op;
    size_t      len     = 0;
    const char *text    = scnArgText(L, 1, "text", &len);
    lua_Number  seconds = 0.0;
    lua_Number  ticks;
    lua_Integer asked  = 0;
    BYTE        target = 0;

    /* A line to be held up has to say how long for, and a missing argument
       is the call written wrong rather than a refusal waiting. The clear is
       the one call that may leave it out: an empty line is taken away rather
       than put up, so there is nothing to time. */
    if (len > 0 || !lua_isnoneornil(L, 2)) {
        seconds = scnArgNumber(L, 2, "seconds");
    }
    if (len >= SCN_TEXT_MAX) {
        return scnRefused(L, SCN_OP_TOO_BIG, "text is %d bytes, limit %d",
                          (int)len, (int)SCN_TEXT_MAX - 1);
    }
    if (!scnPresentationTarget(L, 3, &target, &asked)) {
        return scnRefused(L, SCN_OP_RANGE, "target is %d", (int)asked);
    }
    /* Negated, so a NaN is refused rather than converting to something. */
    if (!(seconds >= 0)) {
        return scnRefused(L, SCN_OP_RANGE,
                          "seconds is negative and an announcement runs "
                          "forwards");
    }
    /* Seconds are the script's unit and the payload's is the server's own
       tick, which is the clock game.tick() answers on and the one the client
       measures the announcement against. game.timer converts the same way,
       so the two ways a script counts a stretch of round agree. */
    ticks = seconds * (lua_Number)GAME_NUMTOTALTICKS_SEC;
    if (ticks > (lua_Number)0xFFFF) {
        return scnRefused(L, SCN_OP_RANGE,
                          "an announcement holds for at most %d ticks, which "
                          "is %d seconds", 0xFFFF,
                          0xFFFF / GAME_NUMTOTALTICKS_SEC);
    }
    /* An empty line is the clear, and the arm does not read its ticks: there
       is nothing to hold up. A line with something in it and no time to be up
       in is a mistake rather than a clear, so the arm refuses it and the row
       says so in the same terms. */
    if (len > 0 && (uint16_t)ticks == 0) {
        return scnRefused(L, SCN_OP_RANGE,
                          "seconds is under the one tick a line has to stay "
                          "up for");
    }
    memset(&op, 0, sizeof(op));
    op.type              = SCN_OP_ANNOUNCE;
    op.u.announce.target = target;
    op.u.announce.ticks  = (uint16_t)ticks;
    memcpy(op.u.announce.text, text, len + 1);
    return scnDone(L, &op, "%d bytes for %d ticks", (int)len,
                   (int)op.u.announce.ticks);
}

/* The three marker rows are one op with the kind changed, so they are one
 * function: the id, the colour and the target are read the same way for all
 * three, and what a kind does not use is left as the memset put it. The
 * clear reads no square, no seat and no colour, which is what lets a script
 * clear an id without remembering what it put there. */
static int scnMarker(lua_State *L, BYTE kind) {
    ScenarioOp  op;
    lua_Integer id     = scnArgInt(L, 1, "id");
    lua_Integer x      = 0;
    lua_Integer y      = 0;
    lua_Integer p      = 0;
    lua_Integer colour = SCN_LUA_MARKER_COLOUR;
    lua_Integer asked  = 0;
    BYTE        target = 0;
    int         colourIdx = 0;   /* 0 for the kind that takes none */
    int         targetIdx = 2;

    switch (kind) {
        case SCN_MARKER_KIND_SQUARE:
            x         = scnArgInt(L, 2, "x");
            y         = scnArgInt(L, 3, "y");
            colourIdx = 4;
            targetIdx = 5;
            break;
        case SCN_MARKER_KIND_FOLLOW:
            p         = scnArgInt(L, 2, "p");
            colourIdx = 3;
            targetIdx = 4;
            break;
        case SCN_MARKER_KIND_CLEAR:
        default:
            break;
    }
    if (!scnFitsByte(id)) {
        return scnRefused(L, SCN_OP_RANGE, "marker %d is not a marker",
                          (int)id);
    }
    if (colourIdx != 0 &&
        !scnArgColour(L, colourIdx, SCN_LUA_MARKER_COLOUR, &colour)) {
        return scnRefused(L, SCN_OP_RANGE,
                          "colour is %d, and the palette runs 0 to %d",
                          (int)colour, SCN_PANEL_COLOURS - 1);
    }
    if (!scnPresentationTarget(L, targetIdx, &target, &asked)) {
        return scnRefused(L, SCN_OP_RANGE, "target is %d", (int)asked);
    }
    if (kind == SCN_MARKER_KIND_SQUARE && (!scnFitsByte(x) || !scnFitsByte(y))) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (kind == SCN_MARKER_KIND_FOLLOW && !scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }

    memset(&op, 0, sizeof(op));
    op.type            = SCN_OP_MARKER;
    op.u.marker.target = target;
    op.u.marker.id     = (BYTE)id;
    op.u.marker.kind   = kind;
    if (kind != SCN_MARKER_KIND_CLEAR) {
        op.u.marker.x      = (BYTE)x;
        op.u.marker.y      = (BYTE)y;
        op.u.marker.slot   = (BYTE)p;
        op.u.marker.colour = (BYTE)colour;
    }
    return scnDone(L, &op, "marker %d, %s", (int)id,
                   (kind == SCN_MARKER_KIND_SQUARE)   ? "on a square"
                   : (kind == SCN_MARKER_KIND_FOLLOW) ? "on a seat"
                                                      : "cleared");
}

static int scnLuaMarker(lua_State *L) {
    return scnMarker(L, SCN_MARKER_KIND_SQUARE);
}

static int scnLuaMarkerFollow(lua_State *L) {
    return scnMarker(L, SCN_MARKER_KIND_FOLLOW);
}

static int scnLuaClearMarker(lua_State *L) {
    return scnMarker(L, SCN_MARKER_KIND_CLEAR);
}

/* ── Flow ─────────────────────────────────────────────────────────── */

static int scnLuaEndRound(lua_State *L) {
    ScenarioOp  op;
    const char *why;
    size_t      len = 0;
    lua_Integer team;

    /* Asked before the arguments are read, so a mod is told the row is not
       its to call rather than told its winner_team is out of range. */
    why = scnNotTheDecider(L);
    if (why != NULL) {
        return scnDecidesRefusal(L, "game.end_round", why);
    }
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_END_ROUND;
    if (!lua_isnoneornil(L, 1)) {
        const char *text = scnArgText(L, 1, "text", &len);
        if (len >= SCN_TEXT_MAX) {
            return scnRefused(L, SCN_OP_TOO_BIG, "text is %d bytes, limit %d",
                              (int)len, (int)SCN_TEXT_MAX - 1);
        }
        memcpy(op.u.endRound.text, text, len + 1);
    }
    team = scnOptInt(L, 2, "winner_team", 0);
    if (!scnFitsByte(team)) {
        return scnRefused(L, SCN_OP_RANGE, "winner_team is %d", (int)team);
    }
    op.u.endRound.winnerTeam = (BYTE)team;
    return scnDone(L, &op, "winner team %d", (int)team);
}

static int scnGameTime(lua_State *L, bool relative) {
    ScenarioOp  op;
    const char *why;
    lua_Integer ticks;

    /* The clock decides when the round is over, so both rows that move it
       are the scenario's alone. Named for the row the script actually
       called, because a sentence naming the other one would send an author
       looking in the wrong place. */
    why = scnNotTheDecider(L);
    if (why != NULL) {
        return scnDecidesRefusal(L, relative ? "game.add_game_time"
                                             : "game.set_game_time", why);
    }
    ticks = scnArgInt(L, 1, "ticks");
    memset(&op, 0, sizeof(op));
    op.type                    = SCN_OP_SET_GAME_TIME;
    op.u.setGameTime.ticks     = (int32_t)ticks;
    op.u.setGameTime.relative  = relative;
    return scnDone(L, &op, "ticks %d", (int)ticks);
}

static int scnLuaSetGameTime(lua_State *L) {
    return scnGameTime(L, false);
}

static int scnLuaAddGameTime(lua_State *L) {
    return scnGameTime(L, true);
}

static int scnLuaSetRule(lua_State *L) {
    ScenarioOp  op;
    const char *name = scnArgStr(L, 1, "name");
    int         rule = simRulesRuleIndex(name);

    if (rule < 0) {
        return luaL_error(L, "no rule is named '%s'", name);
    }
    if (lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_argerror(L, 2,
                             lua_pushfstring(L, "value must be a number, got "
                                                "%s", luaL_typename(L, 2)));
    }
    memset(&op, 0, sizeof(op));
    op.type             = SCN_OP_SET_RULE;
    op.u.setRule.rule   = (uint16_t)rule;
    op.u.setRule.value  = (double)lua_tonumber(L, 2);
    /* The value is stated to the whole part, which is what the sentence has
       room for and what an integer rule ends up holding anyway. */
    return scnDone(L, &op, "rule '%s' to %d", name,
                   (int)scnWhole(lua_tonumber(L, 2)));
}

/* ── Test hooks ────────────────────────────────────────────────────
 *
 * shell_expired(p, x, y [, fire_tick]): one of seat p's shells ran its full
 * range and died over square (x, y) with nothing hit. It exists because a
 * script cannot make a seat fire, and the three-shot order — three
 * full-range shells on one open square inside two seconds, with a quiet
 * second either side of them — has no other way of being put to a round.
 * Nothing is simulated but the notice itself.
 *
 * fire_tick is the SERVER tick the shell LEFT THE GUN, and every timing rule
 * in the detector is on that tick rather than on the landing. A real
 * full-range shell is 104 server ticks in the air (shells.c shellsAddItem
 * works the number out), which is longer than either quiet second, so a
 * script that wants to place a shot inside or outside one of them has to say
 * when it was fired. Left out, the shell counts as fired now.
 */
static int scnLuaShellExpired(lua_State *L) {
    ScenarioOp  op;
    lua_Integer p    = scnArgInt(L, 1, "p");
    lua_Integer x    = scnArgInt(L, 2, "x");
    lua_Integer y    = scnArgInt(L, 3, "y");
    lua_Integer fire = scnOptInt(L, 4, "fire_tick", -1);

    if (!scnFitsByte(p)) {
        return scnRefused(L, SCN_OP_NO_SUCH_PLAYER, "player %d is not a seat",
                          (int)p);
    }
    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE, "square (%d, %d) is off the map",
                          (int)x, (int)y);
    }
    if (fire < -1) {
        return scnRefused(L, SCN_OP_RANGE, "fire_tick %d is before the round "
                          "started", (int)fire);
    }
    memset(&op, 0, sizeof(op));
    op.type                        = SCN_OP_SHELL_EXPIRED;
    op.u.shellExpired.slot         = (BYTE)p;
    op.u.shellExpired.x            = (BYTE)x;
    op.u.shellExpired.y            = (BYTE)y;
    op.u.shellExpired.haveFireTick = (fire >= 0);
    op.u.shellExpired.fireTick     = (fire >= 0) ? (uint32_t)fire : 0u;
    if (fire >= 0) {
        return scnDone(L, &op, "player %d over (%d, %d), fired on %d",
                       (int)p, (int)x, (int)y, (int)fire);
    }
    return scnDone(L, &op, "player %d over (%d, %d)", (int)p, (int)x, (int)y);
}

/* ══ The rows that reach no op ════════════════════════════════════════
 *
 * Three of them, and they are the only rows that change something without
 * the funnel seeing it: two name a call to make later and one names a
 * rectangle. Nothing on the sim is touched by any of them, which is why
 * there is no op to carry them — what they write is the host's own, and it
 * lasts exactly as long as the round does.
 *
 * They answer the way every other row answers: the value asked for, or nil
 * with the refusal's own name and one sentence carrying the number that
 * mattered. */

/* One region named for the rest of the round.
 *
 * A name this script already has in the list is replaced where it stands,
 * which costs no room and keeps the bit a region answers to: the host's
 * per-tick scan remembers who is inside which region by that bit, so
 * replacing a rectangle moves the tanks in and out of the new one rather
 * than making a second region nobody was ever in.
 *
 * A name it has not got is appended, and takes a bit of its own from
 * scenarioLuaRegionBit — the same call the compose makes, keyed the same
 * way, on this script's own file and the name it has just been given. The
 * bit is worked out before the count goes up, so the region being added is
 * not counted among the ones whose bits are already spoken for.
 *
 * This script's own and not any of that name, which is the half that had to
 * change when two scripts were allowed to name one region. A mod that
 * redefines "spawn" now moves its own rectangle and leaves the scenario's
 * where it is; before, it would have moved a rectangle belonging to a file
 * its author never read.
 *
 * The owner is read off the context, which the host sets across a file's own
 * top level as well as across every call it makes into one — see
 * runningOwner in scenario_lua.h. A call that somehow reaches here with no
 * owner at all writes SCN_OWNER_NONE, and a region owned by nobody behaves
 * predictably rather than oddly: no script's lookup ever prefers it, every
 * script's lookup can still reach it through the last-on-the-list fallback,
 * and a second define of the same name from the same nowhere appends a
 * second region rather than replacing the first. Nothing under data/ is in
 * that state and nothing the host runs can be.
 *
 * Declared and defined regions share the one list and the one limit. What a
 * script defines lasts the round: the next round start reads the file's
 * regions over the whole list again. */
static int scnLuaDefineRegion(lua_State *L) {
    const ScnLuaCtx   *c    = scnCtx(L);
    const char        *name = scnArgStr(L, 1, "name");
    lua_Integer        x    = scnArgInt(L, 2, "x");
    lua_Integer        y    = scnArgInt(L, 3, "y");
    lua_Integer        w    = scnArgInt(L, 4, "w");
    lua_Integer        h    = scnArgInt(L, 5, "h");
    ScenarioManifest  *m    = c->manifest;
    /* reg and not slot, because slot is a player's seat everywhere else in
       the round and a region's own number is its bit. */
    ScnManifestRegion *reg  = NULL;
    size_t             len;
    int                i;

    if (m == NULL) {
        return scnRefused(L, SCN_OP_UNSUPPORTED, "this round has no table to "
                                                 "define a region in");
    }
    len = strlen(name);
    if (len == 0 || len >= SCN_REGION_NAME_LEN) {
        return scnRefused(L, SCN_OP_TOO_BIG,
                          "the name is %d bytes and a region's name runs 1 to "
                          "%d", (int)len, SCN_REGION_NAME_LEN - 1);
    }
    if (!scnFitsByte(x) || !scnFitsByte(y)) {
        return scnRefused(L, SCN_OP_BAD_SQUARE,
                          "the corner is %d,%d and a square runs 0 to 255",
                          (int)x, (int)y);
    }
    /* A side is a byte here because it is a byte in the table a script
       declares, and the two are one list. A region that wants the whole of
       an axis is 255 wide and one column short of it, which is what a
       declared region has always been. */
    if (!scnFitsByte(w) || !scnFitsByte(h)) {
        return scnRefused(L, SCN_OP_RANGE,
                          "the size is %dx%d and a side runs 0 to 255",
                          (int)w, (int)h);
    }

    i = scnRegionOwnIndex(m, name, c->runningOwner);
    if (i >= 0) {
        reg = &m->regions[i];
    } else {
        uint8_t at;

        if (m->numRegions >= SCN_REGIONS_MAX) {
            return scnRefused(L, SCN_OP_FULL,
                              "the round already names %d regions, which is "
                              "the limit", (int)m->numRegions);
        }
        /* Before the count goes up, so that the row about to be filled is
           not read as a region already holding a bit: the array is not
           cleared behind the count, and the bytes sitting there are
           whatever the table was copied from. */
        at  = scenarioLuaRegionBit(m, c->runningFile, name);
        reg = &m->regions[m->numRegions];
        m->numRegions++;
        memcpy(reg->name, name, len);
        reg->name[len] = '\0';
        reg->owner = c->runningOwner;
        reg->bit   = at;
    }
    reg->x = (uint8_t)x;
    reg->y = (uint8_t)y;
    reg->w = (uint8_t)w;
    reg->h = (uint8_t)h;

    lua_pushboolean(L, 1);
    return 1;
}

/* ── Timers ───────────────────────────────────────────────────────────
 *
 * A timer is a function the round holds until a tick it names, and the id a
 * script cancels it by. The set lives on the host beside the VM, because
 * what a timer holds has to be released when that VM goes.
 *
 * Seconds become ticks at GAME_NUMTOTALTICKS_SEC, which is what the sim's
 * own seconds-to-ticks conversions use. A running frame advances the clock
 * by two half-steps and a lobby frame by one, so a timer set while the round
 * is in the lobby comes due at half the wall-clock rate — it counts the
 * ticks a script reads off game.tick(), which is the only clock this surface
 * has. */

void scenarioLuaTimersReset(ScnTimerSet *t) {
    int i;

    if (t == NULL) {
        return;
    }
    for (i = 0; i < SCN_TIMERS_MAX; i++) {
        t->entries[i].id      = 0;
        t->entries[i].dueTick = 0;
        t->entries[i].ref     = LUA_NOREF;
        t->entries[i].owner   = NULL;
    }
    /* nextId is left where it is. It rises for the life of the host, so an
       id from the round just finished matches nothing in this one. */
}

void scenarioLuaTimersDrop(lua_State *L, ScnTimerSet *t) {
    int i;

    if (t == NULL) {
        return;
    }
    for (i = 0; i < SCN_TIMERS_MAX; i++) {
        if (t->entries[i].ref != LUA_NOREF && L != NULL) {
            luaL_unref(L, LUA_REGISTRYINDEX, t->entries[i].ref);
        }
        t->entries[i].id      = 0;
        t->entries[i].dueTick = 0;
        t->entries[i].ref     = LUA_NOREF;
        t->entries[i].owner   = NULL;
    }
}

int scenarioLuaTimersDue(const ScnTimerSet *t, uint32_t now, uint32_t *ids,
                         int idsMax) {
    int n = 0;
    int i, j;

    if (t == NULL || ids == NULL || idsMax <= 0) {
        return 0;
    }
    /* The due set, read once. */
    for (i = 0; i < SCN_TIMERS_MAX && n < idsMax; i++) {
        if (t->entries[i].ref != LUA_NOREF && t->entries[i].dueTick <= now) {
            ids[n] = t->entries[i].id;
            n++;
        }
    }
    /* Oldest first. An entry is reused and an id is not, so the id is what
       says which of two timers due on the same tick was set first. Sorted
       here rather than left as entry order, which a script cannot see and
       could not predict. */
    for (i = 1; i < n; i++) {
        uint32_t take = ids[i];
        j = i - 1;
        while (j >= 0 && ids[j] > take) {
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = take;
    }
    return n;
}

/* By id rather than by entry, because the entry may have changed hands since
 * the due set was read: a timer earlier in the run can cancel this one and set
 * another that lands in the same entry. The id is never reused, so it finds
 * the timer that was due or nothing. */
bool scenarioLuaTimersTake(ScnTimerSet *t, uint32_t id, int *ref,
                           const ScenarioManifest **owner) {
    int i;

    if (t == NULL || ref == NULL || id == 0) {
        return false;
    }
    for (i = 0; i < SCN_TIMERS_MAX; i++) {
        ScnTimer *e = &t->entries[i];

        if (e->ref == LUA_NOREF || e->id != id) {
            continue;
        }
        *ref = e->ref;
        if (owner != NULL) {
            *owner = e->owner;
        }
        e->ref     = LUA_NOREF;
        e->id      = 0;
        e->dueTick = 0;
        e->owner   = NULL;
        return true;
    }
    return false;
}

static int scnLuaTimer(lua_State *L) {
    const ScnLuaCtx *c       = scnCtx(L);
    lua_Number       seconds;
    ScnTimerSet     *t       = (c != NULL) ? c->timers : NULL;
    int              free_at = -1;
    int              i;
    int64_t          due;

    if (lua_type(L, 1) != LUA_TNUMBER) {
        return luaL_argerror(
            L, 1, lua_pushfstring(L, "seconds must be a number, got %s",
                                  luaL_typename(L, 1)));
    }
    if (!lua_isfunction(L, 2)) {
        return luaL_argerror(
            L, 2, lua_pushfstring(L, "fn must be a function, got %s",
                                  luaL_typename(L, 2)));
    }
    seconds = lua_tonumber(L, 1);
    if (!(seconds >= 0)) {   /* negated, so a NaN takes this branch */
        return scnRefused(L, SCN_OP_RANGE,
                          "seconds is negative and a timer runs forwards");
    }
    if (t == NULL) {
        return scnRefused(L, SCN_OP_UNSUPPORTED,
                          "this round keeps no timers");
    }

    for (i = 0; i < SCN_TIMERS_MAX; i++) {
        if (t->entries[i].ref == LUA_NOREF) {
            free_at = i;
            break;
        }
    }
    if (free_at < 0) {
        return scnRefused(L, SCN_OP_FULL,
                          "%d timers are already waiting, which is the limit",
                          SCN_TIMERS_MAX);
    }

    /* The tick it comes due on. The delay is held against the width of the
       clock before it is made a whole number, because a double past what an
       integer can hold does not convert to a large number — it does not
       convert at all. A script asking for longer than the clock can count
       gets the last tick there is rather than one in the past. */
    {
        lua_Number ticks = seconds * (lua_Number)GAME_NUMTOTALTICKS_SEC;
        if (!(ticks < (lua_Number)UINT32_MAX)) {
            ticks = (lua_Number)UINT32_MAX;
        }
        due = (int64_t)serverSimGetTick(c->sim) + (int64_t)ticks;
        if (due > (int64_t)UINT32_MAX) {
            due = (int64_t)UINT32_MAX;
        }
    }

    /* The function off the top of the stack, held against collection until
       it runs or is cancelled. */
    lua_pushvalue(L, 2);
    t->entries[free_at].ref     = luaL_ref(L, LUA_REGISTRYINDEX);
    t->entries[free_at].dueTick = (uint32_t)due;
    /* And whose it is. Read here, where the script that is asking is on the
       stack; by the tick it runs on there is nothing left to read it from,
       because the set is one id space for the whole list. Without it a
       scenario's own timer would run as no script in particular, and the
       rows a scenario may call and a mod may not would be closed to it —
       see scnNotTheDecider above. */
    t->entries[free_at].owner   = c->running;
    t->nextId++;
    t->entries[free_at].id      = t->nextId;

    lua_pushinteger(L, (lua_Integer)t->nextId);
    return 1;
}

/* Stop one before it runs. An id that named a timer which has already run,
 * or one that was cancelled, or one from a round that is over, all answer
 * false: ids are never reused, so a stale one matches nothing rather than
 * matching whatever has since taken its place. */
static int scnLuaCancelTimer(lua_State *L) {
    const ScnLuaCtx *c  = scnCtx(L);
    lua_Integer      id = scnArgInt(L, 1, "id");
    ScnTimerSet     *t  = (c != NULL) ? c->timers : NULL;
    int              i;

    if (t != NULL && id > 0) {
        for (i = 0; i < SCN_TIMERS_MAX; i++) {
            if (t->entries[i].ref != LUA_NOREF &&
                (lua_Integer)t->entries[i].id == id) {
                luaL_unref(L, LUA_REGISTRYINDEX, t->entries[i].ref);
                t->entries[i].ref     = LUA_NOREF;
                t->entries[i].id      = 0;
                t->entries[i].dueTick = 0;
                lua_pushboolean(L, 1);
                return 1;
            }
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

/* ── The registry ─────────────────────────────────────────────────── */

/* One argument array per row of the registry below, in the order the rows
 * name them. The name and whether a call may leave it out are the doc
 * string's own — a case holds the two against each other, name for name and
 * bracket for bracket — and the type is what the row's reader does with the
 * argument.
 *
 * An array ends on a terminator naming nothing, so a row's count is the
 * array's length less that terminator and the two cannot disagree. An op
 * that takes no arguments holds only the terminator. */
#define SCN_OP_ARG_END { NULL, SCN_PARAM_NONE, false }

/* The array a row points at and the count derived from it. */
#define SCN_OP_PARAMS(id)                                                    \
    kScnOpArgs_##id,                                                         \
    sizeof(kScnOpArgs_##id) / sizeof(kScnOpArgs_##id[0]) - 1

/* The last cell of a row, written as the word for what it says rather than
 * as a bare true or false. An op acts when it changes something the round
 * can observe, which is the line a check-only state draws for itself: it
 * refuses every acting op and lets every read answer. */
#define SCN_OP_ACTS  true
#define SCN_OP_READS false

static const ScnLuaOpParam kScnOpArgs_tick[]        = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_max_tanks[]   = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_num_players[] = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_num_humans[]  = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_team_size[] = {
    { "t", SCN_PARAM_TEAM, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_game_type[] = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_map_name[]  = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_map_tile[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_is_mine[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_terrain[]    = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_num_pills[]  = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_pill[] = {
    { "n", SCN_PARAM_PILL, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_num_bases[]  = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_base[] = {
    { "n", SCN_PARAM_BASE, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_num_starts[] = { SCN_OP_ARG_END };
/* A start index is an item rather than a kind of its own: the catalogue
   splits pills and bases out because a hook's payload names one or the
   other, and no derived field is built off a start. */
static const ScnLuaOpParam kScnOpArgs_start[] = {
    { "n", SCN_PARAM_ITEM, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_tank[] = {
    { "p", SCN_PARAM_SLOT, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_builder[] = {
    { "p", SCN_PARAM_SLOT, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_lobby_slot[] = {
    { "p", SCN_PARAM_SLOT, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_allied[] = {
    { "a", SCN_PARAM_SLOT, false }, { "b", SCN_PARAM_SLOT, false },
    SCN_OP_ARG_END
};
/* A rule name is matched against the rules table and a name that spells none
   raises, so it is a word out of a fixed set rather than free text. */
static const ScnLuaOpParam kScnOpArgs_rule[] = {
    { "name", SCN_PARAM_WORD, false }, SCN_OP_ARG_END
};
/* An id is matched against the calling script's own settings block and an
   id that names none raises, so it is a word out of a fixed set. */
static const ScnLuaOpParam kScnOpArgs_setting[] = {
    { "id", SCN_PARAM_WORD, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_tags[] = {
    { "kind", SCN_PARAM_WORD, false }, { "n", SCN_PARAM_ITEM, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_tagged[] = {
    { "tag", SCN_PARAM_TAG, false }, { "kind", SCN_PARAM_WORD, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_region[] = {
    { "name", SCN_PARAM_REGION, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_regions[] = { SCN_OP_ARG_END };
static const ScnLuaOpParam kScnOpArgs_in_region[] = {
    { "name", SCN_PARAM_REGION, false }, { "mx", SCN_PARAM_SQUARE_X, false },
    { "my", SCN_PARAM_SQUARE_Y, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_timer[] = {
    { "seconds", SCN_PARAM_NUMBER, false },
    { "fn", SCN_PARAM_FUNCTION, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_cancel_timer[] = {
    { "id", SCN_PARAM_NUMBER, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_define_region[] = {
    { "name", SCN_PARAM_REGION, false }, { "x", SCN_PARAM_SQUARE_X, false },
    { "y", SCN_PARAM_SQUARE_Y, false }, { "w", SCN_PARAM_NUMBER, false },
    { "h", SCN_PARAM_NUMBER, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_stocks[] = {
    { "p", SCN_PARAM_SLOT, false }, { "t", SCN_PARAM_TABLE, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_add_stocks[] = {
    { "p", SCN_PARAM_SLOT, false }, { "t", SCN_PARAM_TABLE, false },
    SCN_OP_ARG_END
};
/* A killer left out arrives at the payload as the byte NEUTRAL, which is
   what a script writing game.NEUTRAL sends, so the argument is an owner. */
static const ScnLuaOpParam kScnOpArgs_kill_tank[] = {
    { "p", SCN_PARAM_SLOT, false }, { "killer", SCN_PARAM_OWNER, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_teleport[] = {
    { "p", SCN_PARAM_SLOT, false }, { "x", SCN_PARAM_SQUARE_X, false },
    { "y", SCN_PARAM_SQUARE_Y, false }, { "dir", SCN_PARAM_NUMBER, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_teleport_to_start[] = {
    { "p", SCN_PARAM_SLOT, false }, { "n", SCN_PARAM_ITEM, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_boat[] = {
    { "p", SCN_PARAM_SLOT, false }, { "on", SCN_PARAM_BOOL, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_give_pill[] = {
    { "p", SCN_PARAM_SLOT, false }, { "n", SCN_PARAM_PILL, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_drop_pill[] = {
    { "p", SCN_PARAM_SLOT, false }, { "n", SCN_PARAM_PILL, false },
    { "x", SCN_PARAM_SQUARE_X, true }, { "y", SCN_PARAM_SQUARE_Y, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_modifiers[] = {
    { "p", SCN_PARAM_SLOT, false }, { "t", SCN_PARAM_TABLE, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_builder_order[] = {
    { "p", SCN_PARAM_SLOT, false }, { "action", SCN_PARAM_WORD, false },
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_builder_recall[] = {
    { "p", SCN_PARAM_SLOT, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_kill_lgm[] = {
    { "p", SCN_PARAM_SLOT, false }, { "killer", SCN_PARAM_OWNER, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_builder_parachute[] = {
    { "p", SCN_PARAM_SLOT, false }, { "x", SCN_PARAM_SQUARE_X, true },
    { "y", SCN_PARAM_SQUARE_Y, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_builder_carried[] = {
    { "p", SCN_PARAM_SLOT, false }, { "trees", SCN_PARAM_NUMBER, true },
    { "mines", SCN_PARAM_NUMBER, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_pill_owner[] = {
    { "n", SCN_PARAM_PILL, false }, { "p", SCN_PARAM_OWNER, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_pill_armour[] = {
    { "n", SCN_PARAM_PILL, false }, { "a", SCN_PARAM_NUMBER, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_pill_speed[] = {
    { "n", SCN_PARAM_PILL, false }, { "s", SCN_PARAM_NUMBER, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_move_pill[] = {
    { "n", SCN_PARAM_PILL, false }, { "x", SCN_PARAM_SQUARE_X, false },
    { "y", SCN_PARAM_SQUARE_Y, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_base_owner[] = {
    { "n", SCN_PARAM_BASE, false }, { "p", SCN_PARAM_OWNER, true },
    { "keep_stock", SCN_PARAM_BOOL, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_base_stock[] = {
    { "n", SCN_PARAM_BASE, false }, { "armour", SCN_PARAM_NUMBER, true },
    { "shells", SCN_PARAM_NUMBER, true }, { "mines", SCN_PARAM_NUMBER, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_add_pill[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    { "owner", SCN_PARAM_OWNER, true }, { "armour", SCN_PARAM_NUMBER, true },
    { "speed", SCN_PARAM_NUMBER, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_remove_pill[] = {
    { "n", SCN_PARAM_PILL, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_add_base[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    { "owner", SCN_PARAM_OWNER, true }, { "armour", SCN_PARAM_NUMBER, true },
    { "shells", SCN_PARAM_NUMBER, true }, { "mines", SCN_PARAM_NUMBER, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_remove_base[] = {
    { "n", SCN_PARAM_BASE, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_add_start[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    { "dir", SCN_PARAM_NUMBER, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_remove_start[] = {
    { "n", SCN_PARAM_ITEM, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_tile[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    { "t", SCN_PARAM_NUMBER, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_fill_rect[] = {
    { "x0", SCN_PARAM_SQUARE_X, false }, { "y0", SCN_PARAM_SQUARE_Y, false },
    { "x1", SCN_PARAM_SQUARE_X, false }, { "y1", SCN_PARAM_SQUARE_Y, false },
    { "t", SCN_PARAM_NUMBER, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_place_mine[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    { "owner", SCN_PARAM_OWNER, true }, { "visible", SCN_PARAM_BOOL, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_remove_mine[] = {
    { "x", SCN_PARAM_SQUARE_X, false }, { "y", SCN_PARAM_SQUARE_Y, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_spawn_bot[] = {
    { "t", SCN_PARAM_TABLE, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_remove_bot[] = {
    { "p", SCN_PARAM_SLOT, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_team[] = {
    { "p", SCN_PARAM_SLOT, false }, { "t", SCN_PARAM_TEAM, false },
    SCN_OP_ARG_END
};
/* The seat, and the table that replaces what the bot was spawned with. The
   same pair hint takes, and for the same reason: a flat table is the one
   shape a brain is handed anything in. */
static const ScnLuaOpParam kScnOpArgs_bot_init[] = {
    { "p", SCN_PARAM_SLOT, false }, { "t", SCN_PARAM_TABLE, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_lobby_add_bot[] = {
    { "t", SCN_PARAM_TABLE, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_lobby_remove_bot[] = {
    { "p", SCN_PARAM_SLOT, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_lobby_set_team[] = {
    { "p", SCN_PARAM_SLOT, false }, { "t", SCN_PARAM_TEAM, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_hint[] = {
    { "p", SCN_PARAM_SLOT, false }, { "t", SCN_PARAM_TABLE, false },
    SCN_OP_ARG_END
};
/* A target is a seat or one of the words the surface names, which is what
   SCN_PARAM_TARGET says. The third form a binding takes, { team = t }, is a
   table: a call can write one and a trigger's argument cannot, so an action
   addresses a seat or whatever wider audience a word names. */
static const ScnLuaOpParam kScnOpArgs_message[] = {
    { "text", SCN_PARAM_STRING, false }, { "target", SCN_PARAM_TARGET, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_say[] = {
    { "p", SCN_PARAM_SLOT, false }, { "text", SCN_PARAM_STRING, false },
    { "target", SCN_PARAM_TARGET, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_sound[] = {
    { "name", SCN_PARAM_WORD, false }, { "x", SCN_PARAM_SQUARE_X, true },
    { "y", SCN_PARAM_SQUARE_Y, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_log[] = {
    { "text", SCN_PARAM_STRING, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_panel[] = {
    { "id", SCN_PARAM_NUMBER, false }, { "list", SCN_PARAM_TABLE, false },
    { "target", SCN_PARAM_TARGET, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_score[] = {
    { "target", SCN_PARAM_TARGET, false },
    { "value", SCN_PARAM_NUMBER, false },
    { "label", SCN_PARAM_STRING, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_announce[] = {
    { "text", SCN_PARAM_STRING, false },
    /* The clear leaves it out: empty text takes a line away rather than
       putting one up, so there is nothing to time. scnLuaAnnounce reads it
       only where the text has something in it. */
    { "seconds", SCN_PARAM_NUMBER, true },
    { "target", SCN_PARAM_TARGET, true }, SCN_OP_ARG_END
};
/* A colour is the palette's word or the number behind it, which is what
   SCN_PARAM_COLOUR says and what scnArgColour reads. Not SCN_PARAM_WORD:
   that one is a word and nothing else, and a colour written as its number
   is a call the binding takes. */
static const ScnLuaOpParam kScnOpArgs_marker[] = {
    { "id", SCN_PARAM_NUMBER, false }, { "x", SCN_PARAM_SQUARE_X, false },
    { "y", SCN_PARAM_SQUARE_Y, false },
    { "colour", SCN_PARAM_COLOUR, true },
    { "target", SCN_PARAM_TARGET, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_marker_follow[] = {
    { "id", SCN_PARAM_NUMBER, false }, { "p", SCN_PARAM_SLOT, false },
    { "colour", SCN_PARAM_COLOUR, true },
    { "target", SCN_PARAM_TARGET, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_clear_marker[] = {
    { "id", SCN_PARAM_NUMBER, false }, { "target", SCN_PARAM_TARGET, true },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_end_round[] = {
    { "text", SCN_PARAM_STRING, true },
    { "winner_team", SCN_PARAM_TEAM, true }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_game_time[] = {
    { "ticks", SCN_PARAM_NUMBER, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_add_game_time[] = {
    { "ticks", SCN_PARAM_NUMBER, false }, SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_set_rule[] = {
    { "name", SCN_PARAM_WORD, false }, { "value", SCN_PARAM_NUMBER, false },
    SCN_OP_ARG_END
};
static const ScnLuaOpParam kScnOpArgs_shell_expired[] = {
    { "p", SCN_PARAM_SLOT, false }, { "x", SCN_PARAM_SQUARE_X, false },
    { "y", SCN_PARAM_SQUARE_Y, false },
    { "fire_tick", SCN_PARAM_NUMBER, true }, SCN_OP_ARG_END
};

/* One row per script-visible function. The doc column is what
 * docs/SCENARIO_API.md and the editor's completion list are written from, so
 * a row added without one is a row nothing can describe; the arguments
 * beside it are that sentence's own signature as data, for whatever has to
 * count a call's arguments rather than read about them. */
static const ScnLuaRow kScnLuaRows[] = {
    { "tick", scnLuaTick,
      "tick() — the tick the round is on.",
      SCN_OP_PARAMS(tick), SCN_OP_READS },
    { "max_tanks", scnLuaMaxTanks,
      "max_tanks() — how many seats a game has.",
      SCN_OP_PARAMS(max_tanks), SCN_OP_READS },
    { "num_players", scnLuaNumPlayers,
      "num_players() — how many seats are playing the round, bots included.",
      SCN_OP_PARAMS(num_players), SCN_OP_READS },
    { "num_humans", scnLuaNumHumans,
      "num_humans() — how many of the players are people.",
      SCN_OP_PARAMS(num_humans), SCN_OP_READS },
    { "team_size", scnLuaTeamSize,
      "team_size(t) — how many seats sit on team t, playing the round or "
      "not.",
      SCN_OP_PARAMS(team_size), SCN_OP_READS },
    { "game_type", scnLuaGameType,
      "game_type() — the rules the humans play under, as one of \"open\", "
      "\"tournament\" and \"strict\": the game type the table names, or the "
      "one the round is playing when it names none.",
      SCN_OP_PARAMS(game_type), SCN_OP_READS },
    { "map_name", scnLuaMapName,
      "map_name() — what the map is called.",
      SCN_OP_PARAMS(map_name), SCN_OP_READS },
    { "map_tile", scnLuaMapTile,
      "map_tile(x, y) — the terrain code at a square, or nil for a square "
      "off the map.",
      SCN_OP_PARAMS(map_tile), SCN_OP_READS },
    { "is_mine", scnLuaIsMine,
      "is_mine(x, y) — whether a square holds a mine.",
      SCN_OP_PARAMS(is_mine), SCN_OP_READS },
    { "terrain", scnLuaTerrain,
      "terrain() — every square as one 65,536-byte string, the square at "
      "(x, y) at byte y * 256 + x + 1.",
      SCN_OP_PARAMS(terrain), SCN_OP_READS },
    { "num_pills", scnLuaNumPills,
      "num_pills() — how many pill slots the map has, live or not.",
      SCN_OP_PARAMS(num_pills), SCN_OP_READS },
    { "pill", scnLuaPill,
      "pill(n) — pill n as { x, y, owner, armour, speed, in_tank }, or nil "
      "for a slot no live pill holds.",
      SCN_OP_PARAMS(pill), SCN_OP_READS },
    { "num_bases", scnLuaNumBases,
      "num_bases() — how many base slots the map has, live or not.",
      SCN_OP_PARAMS(num_bases), SCN_OP_READS },
    { "base", scnLuaBase,
      "base(n) — base n as { x, y, owner, armour, shells, mines }, or nil "
      "for a slot no live base holds.",
      SCN_OP_PARAMS(base), SCN_OP_READS },
    { "num_starts", scnLuaNumStarts,
      "num_starts() — how many start slots the map has, live or not.",
      SCN_OP_PARAMS(num_starts), SCN_OP_READS },
    { "start", scnLuaStart,
      "start(n) — start n as { x, y, dir }, or nil for a slot no live start "
      "holds.",
      SCN_OP_PARAMS(start), SCN_OP_READS },
    { "tank", scnLuaTank,
      "tank(p) — player p's tank as { mx, my, wx, wy, dir, armour, shells, "
      "mines, trees, pills, boat, dead, name, bot, kills, deaths, mods }, "
      "or nil when the seat is empty or has no tank.",
      SCN_OP_PARAMS(tank), SCN_OP_READS },
    { "builder", scnLuaBuilder,
      "builder(p) — player p's builder as { state, mx, my, wx, wy, job, "
      "trees, mines }, or nil when the seat has none.",
      SCN_OP_PARAMS(builder), SCN_OP_READS },
    { "lobby_slot", scnLuaLobbySlot,
      "lobby_slot(p) — seat p as { connected, bot, team, name, ready, "
      "fielded, alive }, or nil for an empty seat.",
      SCN_OP_PARAMS(lobby_slot), SCN_OP_READS },
    { "allied", scnLuaAllied,
      "allied(a, b) — whether seats a and b are on the same side in the "
      "game, including alliances players made in play, which can differ "
      "from their lobby teams; true for a seat and itself, nil when either "
      "seat is empty.",
      SCN_OP_PARAMS(allied), SCN_OP_READS },
    { "rule", scnLuaRule,
      "rule(name) — what a gameplay rule is set to; a name that spells no "
      "rule raises.",
      SCN_OP_PARAMS(rule), SCN_OP_READS },
    { "setting", scnLuaSetting,
      "setting(id) — the value the host chose in the lobby for one of "
      "this script's own settings, or its declared default: a number, "
      "true or false for a bool setting, or the chosen word for a choice "
      "setting; an id the script never declared raises.",
      SCN_OP_PARAMS(setting), SCN_OP_READS },
    { "tags", scnLuaTags,
      "tags(kind, n) — the tags the scenario put on a \"pill\", \"base\" or "
      "\"start\", as an array of strings.",
      SCN_OP_PARAMS(tags), SCN_OP_READS },
    { "tagged", scnLuaTagged,
      "tagged(tag[, kind]) — everything carrying a tag as an array of "
      "{ kind, n }, or of n when a kind is named; pills, then bases, then "
      "starts.",
      SCN_OP_PARAMS(tagged), SCN_OP_READS },
    { "region", scnLuaRegion,
      "region(name) — a declared rectangle as { x, y, w, h }, or nil when "
      "nothing is declared by that name.",
      SCN_OP_PARAMS(region), SCN_OP_READS },
    { "regions", scnLuaRegions,
      "regions() — the name of every declared region, in name order.",
      SCN_OP_PARAMS(regions), SCN_OP_READS },
    { "in_region", scnLuaInRegion,
      "in_region(name, mx, my) — whether a square is inside a named "
      "region.",
      SCN_OP_PARAMS(in_region), SCN_OP_READS },

    /* The three that change nothing on the sim: what the host holds for the
       rest of the round. */
    { "timer", scnLuaTimer,
      "timer(seconds, fn) → id — run fn once, on the first tick at or after "
      "seconds from now; cancel it with the id. At most 64 wait at a time, "
      "and none outlives its round.",
      SCN_OP_PARAMS(timer), SCN_OP_ACTS },
    { "cancel_timer", scnLuaCancelTimer,
      "cancel_timer(id) — stop a timer that has not run yet; false when the "
      "id names none, which is what an id that has already run names.",
      SCN_OP_PARAMS(cancel_timer), SCN_OP_ACTS },
    { "define_region", scnLuaDefineRegion,
      "define_region(name, x, y, w, h) — name a rectangle for the rest of "
      "the round, replacing one of that name; it shares the 64 the scenario "
      "table's own regions come out of.",
      SCN_OP_PARAMS(define_region), SCN_OP_ACTS },

    /* The writes. Each answers true, or nil with the refusal's name and one
       sentence saying what it was about. */
    { "set_stocks", scnLuaSetStocks,
      "set_stocks(p, t) — set any of t.shells, t.mines, t.armour and "
      "t.trees on player p's tank; a stock the table leaves out is left "
      "alone.",
      SCN_OP_PARAMS(set_stocks), SCN_OP_ACTS },
    { "add_stocks", scnLuaAddStocks,
      "add_stocks(p, t) — the same four as amounts to add, negative to take "
      "away; each is held at the cap and at zero rather than refused.",
      SCN_OP_PARAMS(add_stocks), SCN_OP_ACTS },
    { "kill_tank", scnLuaKillTank,
      "kill_tank(p[, killer]) — kill player p's tank; killer is a seat, and "
      "without one the death is the scenario's own.",
      SCN_OP_PARAMS(kill_tank), SCN_OP_ACTS },
    { "teleport", scnLuaTeleport,
      "teleport(p, x, y[, dir]) — put player p's tank on a square, facing "
      "dir from 0 to 255; without dir it keeps the way it faces.",
      SCN_OP_PARAMS(teleport), SCN_OP_ACTS },
    { "teleport_to_start", scnLuaTeleportToStart,
      "teleport_to_start(p[, n]) — put player p's tank on start n, or on "
      "the one the engine would have chosen.",
      SCN_OP_PARAMS(teleport_to_start), SCN_OP_ACTS },
    { "set_boat", scnLuaSetBoat,
      "set_boat(p, on) — put player p's tank on a boat or take it off one; "
      "the square under it has to be water.",
      SCN_OP_PARAMS(set_boat), SCN_OP_ACTS },
    { "give_pill", scnLuaGivePill,
      "give_pill(p, n) — put pill n into player p's tank, however armoured "
      "and whoever held it.",
      SCN_OP_PARAMS(give_pill), SCN_OP_ACTS },
    { "drop_pill", scnLuaDropPill,
      "drop_pill(p, n[, x, y]) — put a pill player p is carrying back on "
      "the map, on a square or under the tank.",
      SCN_OP_PARAMS(drop_pill), SCN_OP_ACTS },
    { "set_modifiers", scnLuaSetModifiers,
      "set_modifiers(p, t) — replace player p's speed, accel, turn, reload, "
      "dealt and taken percentages; a field the table leaves out goes back "
      "to the classic tank.",
      SCN_OP_PARAMS(set_modifiers), SCN_OP_ACTS },
    { "builder_order", scnLuaBuilderOrder,
      "builder_order(p, action, x, y) — send player p's builder out to do "
      "one of \"trees\", \"road\", \"building\", \"pill\", \"mine\" or "
      "\"boat\" on a square; the engine repairs rather than builds where "
      "the square already holds one.",
      SCN_OP_PARAMS(builder_order), SCN_OP_ACTS },
    { "builder_recall", scnLuaBuilderRecall,
      "builder_recall(p) — call player p's builder back to the tank.",
      SCN_OP_PARAMS(builder_recall), SCN_OP_ACTS },
    { "kill_lgm", scnLuaKillLgm,
      "kill_lgm(p[, killer]) — kill player p's builder; killer is a seat.",
      SCN_OP_PARAMS(kill_lgm), SCN_OP_ACTS },
    { "builder_parachute", scnLuaBuilderParachute,
      "builder_parachute(p[, x, y]) — drop a dead builder back in, on a "
      "square or at the tank.",
      SCN_OP_PARAMS(builder_parachute), SCN_OP_ACTS },
    { "set_builder_carried", scnLuaSetBuilderCarried,
      "set_builder_carried(p[, trees[, mines]]) — what player p's builder "
      "is carrying; a count left out is left alone.",
      SCN_OP_PARAMS(set_builder_carried), SCN_OP_ACTS },
    { "set_pill_owner", scnLuaSetPillOwner,
      "set_pill_owner(n[, p]) — hand pill n to a seat, or to nobody with "
      "game.NEUTRAL or with no seat named.",
      SCN_OP_PARAMS(set_pill_owner), SCN_OP_ACTS },
    { "set_pill_armour", scnLuaSetPillArmour,
      "set_pill_armour(n, a) — how much pill n has left; 0 is a dead pill "
      "on the ground.",
      SCN_OP_PARAMS(set_pill_armour), SCN_OP_ACTS },
    { "set_pill_speed", scnLuaSetPillSpeed,
      "set_pill_speed(n, s) — the ticks between pill n's shots.",
      SCN_OP_PARAMS(set_pill_speed), SCN_OP_ACTS },
    { "move_pill", scnLuaMovePill,
      "move_pill(n, x, y) — put pill n on another square.",
      SCN_OP_PARAMS(move_pill), SCN_OP_ACTS },
    { "set_base_owner", scnLuaSetBaseOwner,
      "set_base_owner(n[, p[, keep_stock]]) — hand base n to a seat, or to "
      "nobody with game.NEUTRAL or with no seat named; keep_stock leaves "
      "what it holds.",
      SCN_OP_PARAMS(set_base_owner), SCN_OP_ACTS },
    { "set_base_stock", scnLuaSetBaseStock,
      "set_base_stock(n[, armour[, shells[, mines]]]) — what base n holds; a "
      "stock left out is left alone and one past the cap is held there.",
      SCN_OP_PARAMS(set_base_stock), SCN_OP_ACTS },
    { "add_pill", scnLuaAddPill,
      "add_pill(x, y[, owner[, armour[, speed]]]) → n — put a new pill on "
      "the map and answer which one it is; nobody's, dead, and firing at "
      "the round's own rate unless told otherwise.",
      SCN_OP_PARAMS(add_pill), SCN_OP_ACTS },
    { "remove_pill", scnLuaRemovePill,
      "remove_pill(n) — take pill n off the map; the slot stays, so the "
      "pills above it keep their numbers.",
      SCN_OP_PARAMS(remove_pill), SCN_OP_ACTS },
    { "add_base", scnLuaAddBase,
      "add_base(x, y[, owner[, armour, shells, mines]]) → n — put a new "
      "base on the map and answer which one it is; nobody's and empty "
      "unless told otherwise.",
      SCN_OP_PARAMS(add_base), SCN_OP_ACTS },
    { "remove_base", scnLuaRemoveBase,
      "remove_base(n) — take base n off the map; the slot stays.",
      SCN_OP_PARAMS(remove_base), SCN_OP_ACTS },
    { "add_start", scnLuaAddStart,
      "add_start(x, y, dir) → n — put a new start on a deep-sea square, "
      "facing dir from 0 to 15.",
      SCN_OP_PARAMS(add_start), SCN_OP_ACTS },
    { "remove_start", scnLuaRemoveStart,
      "remove_start(n) — take start n off the map; the last one is "
      "refused.",
      SCN_OP_PARAMS(remove_start), SCN_OP_ACTS },
    { "set_tile", scnLuaSetTile,
      "set_tile(x, y, t) — write one square's terrain, by a game.TERRAIN "
      "code.",
      SCN_OP_PARAMS(set_tile), SCN_OP_ACTS },
    { "fill_rect", scnLuaFillRect,
      "fill_rect(x0, y0, x1, y1, t) — write a rectangle of terrain; one "
      "too big for a tick's budget answers true and \"queued\" and finishes "
      "over the ticks after it.",
      SCN_OP_PARAMS(fill_rect), SCN_OP_ACTS },
    { "place_mine", scnLuaPlaceMine,
      "place_mine(x, y[, owner[, visible]]) — lay a mine on a square; "
      "visible shows it to everyone rather than to its owner's side.",
      SCN_OP_PARAMS(place_mine), SCN_OP_ACTS },
    { "remove_mine", scnLuaRemoveMine,
      "remove_mine(x, y) — take a mine off a square without setting it "
      "off.",
      SCN_OP_PARAMS(remove_mine), SCN_OP_ACTS },
    { "spawn_bot", scnLuaSpawnBot,
      "spawn_bot(t) → p, \"queued\" — put a bot into the running round; t "
      "takes name, brain, team, slot, start, loadout, mode, difficulty and "
      "a flat init table, all of them optional.",
      SCN_OP_PARAMS(spawn_bot), SCN_OP_ACTS },
    { "remove_bot", scnLuaRemoveBot,
      "remove_bot(p) → true, \"queued\" — take a bot out of the running "
      "round; a human seat is refused.",
      SCN_OP_PARAMS(remove_bot), SCN_OP_ACTS },
    { "set_team", scnLuaSetTeam,
      "set_team(p, t) — move a seat to another team mid-round.",
      SCN_OP_PARAMS(set_team), SCN_OP_ACTS },
    { "bot_init", scnLuaBotInit,
      "bot_init(p, t) — hand a bot already in the round a new init table; "
      "it replaces the one the bot was spawned with and the brain is told "
      "about it.",
      SCN_OP_PARAMS(bot_init), SCN_OP_ACTS },
    { "lobby_add_bot", scnLuaLobbyAddBot,
      "lobby_add_bot(t) → p — seat a bot in the lobby and answer which seat "
      "it took; t takes name, brain, team, slot, fielded, mode and "
      "difficulty.",
      SCN_OP_PARAMS(lobby_add_bot), SCN_OP_ACTS },
    { "lobby_remove_bot", scnLuaLobbyRemoveBot,
      "lobby_remove_bot(p) — take a bot out of the lobby; a human seat is "
      "refused.",
      SCN_OP_PARAMS(lobby_remove_bot), SCN_OP_ACTS },
    { "lobby_set_team", scnLuaLobbySetTeam,
      "lobby_set_team(p, t) — move a lobby seat to another team.",
      SCN_OP_PARAMS(lobby_set_team), SCN_OP_ACTS },
    { "hint", scnLuaHint,
      "hint(p, t) — hand bot p's brain an order: a flat table with a verb "
      "and whatever else the brain reads. Values may be strings, numbers or "
      "true/false and all reach the brain as text. A brain that takes no "
      "hints ignores it.",
      SCN_OP_PARAMS(hint), SCN_OP_ACTS },
    { "message", scnLuaMessage,
      "message(text[, target]) — a line to everyone, to one seat with a "
      "number, or to a team with { team = t }.",
      SCN_OP_PARAMS(message), SCN_OP_ACTS },
    { "say", scnLuaSay,
      "say(p, text[, target]) — a chat line seat p says: to its own team "
      "with no target, to everyone with \"all\", or to one seat with a "
      "number. Unlike message, which is the server talking, this reaches a "
      "bot's inbox and fires on_chat.",
      SCN_OP_PARAMS(say), SCN_OP_ACTS },
    { "sound", scnLuaSound,
      "sound(name[, x, y]) — play one of the server's sounds, at a square "
      "or everywhere.",
      SCN_OP_PARAMS(sound), SCN_OP_ACTS },
    { "log", scnLuaLog,
      "log(text) — write a line to the server's console; no player sees "
      "it.",
      SCN_OP_PARAMS(log), SCN_OP_ACTS },
    { "panel", scnLuaPanel,
      "panel(id, list[, target]) — draw a panel from a list of primitives, "
      "each an array with its name first: { \"rect\", x, y, w, h, colour, "
      "fill }, { \"text\", x, y, colour, size, align, s } and so on. An "
      "empty list clears the panel, and one update per panel per audience "
      "per tick is taken.",
      SCN_OP_PARAMS(panel), SCN_OP_ACTS },
    { "score", scnLuaScore,
      "score(target, value[, label]) — the scenario's own score for one "
      "seat with a number, or for a team with { team = t }; label is the "
      "short word shown beside it.",
      SCN_OP_PARAMS(score), SCN_OP_ACTS },
    { "announce", scnLuaAnnounce,
      "announce(text[, seconds[, target]]) — a line across the centre of the "
      "screen for that many seconds; empty text takes the line away.",
      SCN_OP_PARAMS(announce), SCN_OP_ACTS },
    { "marker", scnLuaMarker,
      "marker(id, x, y[, colour[, target]]) — put mark id on a map square; "
      "a second marker on the same id replaces the first.",
      SCN_OP_PARAMS(marker), SCN_OP_ACTS },
    { "marker_follow", scnLuaMarkerFollow,
      "marker_follow(id, p[, colour[, target]]) — put mark id on seat p, "
      "where it rides the tank rather than a square.",
      SCN_OP_PARAMS(marker_follow), SCN_OP_ACTS },
    { "clear_marker", scnLuaClearMarker,
      "clear_marker(id[, target]) — take mark id off the map.",
      SCN_OP_PARAMS(clear_marker), SCN_OP_ACTS },
    { "end_round", scnLuaEndRound,
      "end_round([text[, winner_team]]) — end the round now, with the line "
      "the lobby shows and the team that won it.",
      SCN_OP_PARAMS(end_round), SCN_OP_ACTS },
    { "set_game_time", scnLuaSetGameTime,
      "set_game_time(ticks) — how long the round has left.",
      SCN_OP_PARAMS(set_game_time), SCN_OP_ACTS },
    { "add_game_time", scnLuaAddGameTime,
      "add_game_time(ticks) — add to what the round has left, or take away "
      "with a negative.",
      SCN_OP_PARAMS(add_game_time), SCN_OP_ACTS },
    { "set_rule", scnLuaSetRule,
      "set_rule(name, value) — write one of the gameplay rules; a name that "
      "spells no rule raises, and a value the table will not take is "
      "refused.",
      SCN_OP_PARAMS(set_rule), SCN_OP_ACTS },
    { "shell_expired", scnLuaShellExpired,
      "shell_expired(p, x, y [, fire_tick]) — post one of seat p's shells as "
      "having run its full range and died over square (x, y) with nothing "
      "hit; three on one open square inside two seconds, with a quiet second "
      "either side, are the three-shot order the bots read. fire_tick is "
      "when the shell left the gun, which is the tick every timing rule "
      "reads; left out, it counts as fired now. A test hook: nothing else "
      "about the shell happens.",
      SCN_OP_PARAMS(shell_expired), SCN_OP_ACTS },
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

/* The word tables the game table carries beside TERRAIN. The rows are the
 * ones the argument readers match a word against, so a name a script may
 * write is a name -validate's stub table and the document both hold. */
static const ScnLuaWordTable kScnLuaWordTables[] = {
    { "COLOUR",     kScnColourWords,
      sizeof(kScnColourWords) / sizeof(kScnColourWords[0])       },
    { "SIZE",       kScnSizeWords,
      sizeof(kScnSizeWords) / sizeof(kScnSizeWords[0])           },
    { "ALIGN",      kScnAlignWords,
      sizeof(kScnAlignWords) / sizeof(kScnAlignWords[0])         },
    { "TIMER_MODE", kScnTimerModeWords,
      sizeof(kScnTimerModeWords) / sizeof(kScnTimerModeWords[0]) },
};

/* ── The functions the author writes ──────────────────────────────── */

/* One parameter array per row, built from the two lists in scenario_lua.h:
 * a name and the type of what the host puts in it, for each. Each array ends
 * on a terminator naming nothing, so a row's count below is the array's
 * length less that terminator and the two cannot disagree. */
#define SCN_FN_HOOK_PARAMS(id, name, kind, params)                            \
    static const ScnLuaFnParam kScnFnHook_##id[] = {                          \
        params { NULL, SCN_PARAM_NONE }                                       \
    };
SCN_HOOK_LIST(SCN_FN_HOOK_PARAMS)
#undef SCN_FN_HOOK_PARAMS

#define SCN_FN_POLICY_PARAMS(id, name, params, returns)                       \
    static const ScnLuaFnParam kScnFnPolicy_##id[] = {                        \
        params { NULL, SCN_PARAM_NONE }                                       \
    };
SCN_POLICY_LIST(SCN_FN_POLICY_PARAMS)
#undef SCN_FN_POLICY_PARAMS

/* The hooks first, in the order scenario_host.c dispatches them, then the
 * policies. Both halves are the same row, so whatever reads this — a
 * document, the editor's list of what a scenario may define — walks one
 * table and reads the kind to tell a question from a fact. */
static const ScnLuaFnRow kScnLuaFunctions[] = {
#define SCN_FN_HOOK_ROW(id, name, kind, params)                               \
    { name, kind, kScnFnHook_##id,                                            \
      sizeof(kScnFnHook_##id) / sizeof(kScnFnHook_##id[0]) - 1, NULL },
    SCN_HOOK_LIST(SCN_FN_HOOK_ROW)
#undef SCN_FN_HOOK_ROW
#define SCN_FN_POLICY_ROW(id, name, params, returns)                          \
    { name, SCN_FN_POLICY, kScnFnPolicy_##id,                                 \
      sizeof(kScnFnPolicy_##id) / sizeof(kScnFnPolicy_##id[0]) - 1, returns },
    SCN_POLICY_LIST(SCN_FN_POLICY_ROW)
#undef SCN_FN_POLICY_ROW
};

/* The table is the two lists and nothing else. The hook half is the tally
 * ScnHookId ends on — scenario_host.c makes SCN_HOOK_COUNT from this same
 * list — so a hook added there arrives here with its parameters or does not
 * compile at all, the row macro having too few arguments. This holds the
 * table against a row written in by hand underneath them, which would be
 * the one way to have a function here the lists do not name.
 *
 * SCN_HOOK_COUNT itself belongs to the enum in scenario_host.c and is not
 * visible from this file; scenario_functions_table is where the two are
 * held together by name at run time. */
#define SCN_FN_HOOK_TALLY(id, name, kind, params) +1
#define SCN_FN_POLICY_TALLY(id, name, params, returns) +1
BOLO_STATIC_ASSERT(
    (int)(sizeof(kScnLuaFunctions) / sizeof(kScnLuaFunctions[0])) ==
        (0 SCN_HOOK_LIST(SCN_FN_HOOK_TALLY)) +
        (0 SCN_POLICY_LIST(SCN_FN_POLICY_TALLY)),
    function_table_is_the_hook_list_and_the_policy_list);
#undef SCN_FN_HOOK_TALLY
#undef SCN_FN_POLICY_TALLY

const ScnLuaFnRow *scenarioLuaFunctions(size_t *count) {
    if (count != NULL) {
        *count = sizeof(kScnLuaFunctions) / sizeof(kScnLuaFunctions[0]);
    }
    return kScnLuaFunctions;
}

uint8_t scenarioLuaFnCallbackType(const char *name, bool byTrigger) {
    size_t i;

    for (i = 0; name != NULL &&
                i < sizeof(kScnLuaFunctions) / sizeof(kScnLuaFunctions[0]);
         i++) {
        if (strcmp(kScnLuaFunctions[i].name, name) == 0) {
            if (kScnLuaFunctions[i].kind == SCN_FN_POLICY) {
                return SCN_CB_TYPE_QUERY;
            }
            break;
        }
    }
    return byTrigger ? SCN_CB_TYPE_TRIGGER : SCN_CB_TYPE_EVENT;
}

/* One field on to the end of what has been counted, written only where the
 * caller left room for it. The tally rises either way, so a caller that
 * asked for none still learns how many there are. A name too long for the
 * field is cut rather than written past the end of it. */
static void scnFnFieldAdd(ScnLuaFnField *out, size_t outMax, size_t *count,
                          const char *name, ScnLuaParamType type, size_t from,
                          bool derived) {
    if (out != NULL && *count < outMax) {
        ScnLuaFnField *f = &out[*count];

        memset(f, 0, sizeof(*f));
        snprintf(f->name, sizeof(f->name), "%s", name);
        f->type    = type;
        f->from    = from;
        f->derived = derived;
    }
    (*count)++;
}

/* The parameters of a row, and then what their types are worth on top of
 * them: a seat and an owner each carry the team they are on, a pillbox and a
 * base each carry the tag the scenario put on them, and a square's two
 * halves together carry whichever region holds them.
 *
 * The derived names follow the parameters rather than sitting beside them,
 * so the first paramCount fields are always the function's own arguments in
 * the order it takes them. _team is prefixed with the parameter's name
 * because a function may take more than one seat. region is bare because no
 * function in the catalogue takes two squares, and tag is bare for the first
 * pillbox or base a row takes; pill_damage_scale takes two pillboxes, the one
 * hit and the one whose shell hit it, and the second's is prefixed with its
 * name the way a team is. */
size_t scenarioLuaFnFields(size_t row, ScnLuaFnField *out, size_t outMax) {
    const ScnLuaFnRow *r;
    size_t             count  = 0;
    size_t             i;
    bool               tagged = false;

    if (row >= sizeof(kScnLuaFunctions) / sizeof(kScnLuaFunctions[0])) {
        return 0;
    }
    r = &kScnLuaFunctions[row];

    for (i = 0; i < r->paramCount; i++) {
        scnFnFieldAdd(out, outMax, &count, r->params[i].name,
                      r->params[i].type, i, false);
    }
    for (i = 0; i < r->paramCount; i++) {
        char derived[SCN_FN_FIELD_NAME_LEN];

        switch (r->params[i].type) {
            case SCN_PARAM_SLOT:
            case SCN_PARAM_OWNER:
                snprintf(derived, sizeof(derived), "%s_team",
                         r->params[i].name);
                scnFnFieldAdd(out, outMax, &count, derived, SCN_PARAM_TEAM, i,
                              true);
                break;

            case SCN_PARAM_PILL:
            case SCN_PARAM_BASE:
                if (tagged) {
                    snprintf(derived, sizeof(derived), "%s_tag",
                             r->params[i].name);
                    scnFnFieldAdd(out, outMax, &count, derived,
                                  SCN_PARAM_TAG, i, true);
                } else {
                    scnFnFieldAdd(out, outMax, &count, "tag", SCN_PARAM_TAG,
                                  i, true);
                    tagged = true;
                }
                break;

            case SCN_PARAM_SQUARE_X:
                /* The pair, not the half: an x with no y behind it names no
                   square and so holds no region. */
                if (i + 1 < r->paramCount &&
                    r->params[i + 1].type == SCN_PARAM_SQUARE_Y) {
                    scnFnFieldAdd(out, outMax, &count, "region",
                                  SCN_PARAM_REGION, i, true);
                }
                break;

            default:
                break;
        }
    }
    return count;
}

const ScnLuaRow *scenarioLuaRows(size_t *count) {
    if (count != NULL) {
        *count = sizeof(kScnLuaRows) / sizeof(kScnLuaRows[0]);
    }
    return kScnLuaRows;
}

/* Read off the types rather than off a list of names, so an op that grows a
 * table argument leaves the list a trigger may call by that alone. A row
 * with no arguments at all is a call anything can write, so it answers
 * true. */
bool scenarioLuaOpIsScalar(const ScnLuaRow *row) {
    size_t i;

    if (row == NULL || row->params == NULL) {
        return false;
    }
    for (i = 0; i < row->paramCount; i++) {
        if (row->params[i].type == SCN_PARAM_TABLE ||
            row->params[i].type == SCN_PARAM_FUNCTION) {
            return false;
        }
    }
    return true;
}

/* The two flags and nothing else, so a row that grows a table argument and a
 * row that stops changing anything each leave the list without a second
 * edit. */
bool scenarioLuaOpIsAction(const ScnLuaRow *row) {
    return row != NULL && row->acts && scenarioLuaOpIsScalar(row);
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

const ScnLuaWordTable *scenarioLuaWordTables(size_t *count) {
    if (count != NULL) {
        *count = sizeof(kScnLuaWordTables) / sizeof(kScnLuaWordTables[0]);
    }
    return kScnLuaWordTables;
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

    /* The word tables, each under its own name. A script writes the word
       itself wherever one of these is taken, so these are for a script that
       computes a value and for the completion list rather than for the
       common case. */
    for (i = 0; i < sizeof(kScnLuaWordTables) / sizeof(kScnLuaWordTables[0]);
         i++) {
        size_t w;
        lua_newtable(L);
        for (w = 0; w < kScnLuaWordTables[i].count; w++) {
            lua_pushinteger(L,
                            (lua_Integer)kScnLuaWordTables[i].words[w].value);
            lua_setfield(L, -2, kScnLuaWordTables[i].words[w].word);
        }
        lua_setfield(L, -2, kScnLuaWordTables[i].name);
    }

    lua_setglobal(L, "game");
}
