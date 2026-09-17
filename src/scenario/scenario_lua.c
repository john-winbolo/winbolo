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
    X(SCN_OP_NOT_FOUND)      X(SCN_OP_NO_STOCK)    X(SCN_OP_BAD_CALL)

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

const char *scenarioLuaDieKindWord(int kind) {
    switch (kind) {
        case DIE_KIND_TANK:    return "tank";
        case DIE_KIND_BUILDER: return "builder";
        case DIE_KIND_PILL:    return "pill";
        default:               return NULL;
    }
}

/* A shell and a mine are spelled here as a tank's cause spells them, so a
   script reads one vocabulary across the three kinds a death comes in.
   DMG_SRC_UNKNOWN has no word on purpose: what the script is handed for it is
   nil, which says what the value says. */
const char *scenarioLuaDamageSourceWord(int source) {
    switch (source) {
        case DMG_SRC_SHELL: return "shell";
        case DMG_SRC_MINE:  return "mine";
        default:            return NULL;
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

const ScnManifestRegion *scenarioLuaRegionFind(const ScenarioManifest *m,
                                               const char *name) {
    int i;

    if (m == NULL || name == NULL) {
        return NULL;
    }
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

static int scnLuaRegion(lua_State *L) {
    const ScnLuaCtx         *c    = scnCtx(L);
    const char              *name = scnArgStr(L, 1, "name");
    const ScnManifestRegion *r    = scenarioLuaRegionFind(c->manifest, name);

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
    const ScnManifestRegion *r    = scenarioLuaRegionFind(c->manifest, name);

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
        (int)SCN_PANEL_SIZE_NORMAL + 1,
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
                                         SCN_PANEL_SIZE_NORMAL);
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
    op.type           = SCN_OP_PANEL;
    op.u.panel.target = target;
    op.u.panel.panel  = (BYTE)id;
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
    ScnLuaTargetKind kind  = scnArgTarget(L, 1, &to);
    lua_Number       value = scnArgNumber(L, 2, "value");
    const char      *label = "";
    size_t           len   = 0;

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
    size_t      len = 0;
    lua_Integer team;

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
    lua_Integer ticks = scnArgInt(L, 1, "ticks");

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
 * A name already in the list is replaced where it stands, which costs no
 * room and keeps the index a region sits at: the host's per-tick scan
 * remembers who is inside which region by that index, so replacing a
 * rectangle moves the tanks in and out of the new one rather than making a
 * second region nobody was ever in.
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
    ScnManifestRegion *slot = NULL;
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

    for (i = 0; i < (int)m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) {
            slot = &m->regions[i];
            break;
        }
    }
    if (slot == NULL) {
        if (m->numRegions >= SCN_REGIONS_MAX) {
            return scnRefused(L, SCN_OP_FULL,
                              "the round already names %d regions, which is "
                              "the limit", (int)m->numRegions);
        }
        slot = &m->regions[m->numRegions];
        m->numRegions++;
        memcpy(slot->name, name, len);
        slot->name[len] = '\0';
    }
    slot->x = (uint8_t)x;
    slot->y = (uint8_t)y;
    slot->w = (uint8_t)w;
    slot->h = (uint8_t)h;

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
    }
}

int scenarioLuaTimersTakeDue(ScnTimerSet *t, uint32_t now, int *out,
                             int outMax) {
    int taken[SCN_TIMERS_MAX];
    int n = 0;
    int i, j;

    if (t == NULL || out == NULL || outMax <= 0) {
        return 0;
    }
    /* Read the due set once, before any of it runs. */
    for (i = 0; i < SCN_TIMERS_MAX && n < outMax; i++) {
        if (t->entries[i].ref != LUA_NOREF && t->entries[i].dueTick <= now) {
            taken[n] = i;
            n++;
        }
    }
    /* Oldest first. An entry is reused and an id is not, so the id is what
       says which of two timers due on the same tick was set first. Sorted
       here rather than left as entry order, which a script cannot see and
       could not predict. */
    for (i = 1; i < n; i++) {
        int take = taken[i];
        j = i - 1;
        while (j >= 0 && t->entries[taken[j]].id > t->entries[take].id) {
            taken[j + 1] = taken[j];
            j--;
        }
        taken[j + 1] = take;
    }
    for (i = 0; i < n; i++) {
        ScnTimer *e = &t->entries[taken[i]];
        out[i]      = e->ref;
        e->ref      = LUA_NOREF;
        e->id       = 0;
        e->dueTick  = 0;
    }
    return n;
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
      "game_type() — the rules the humans play under, as one of \"open\", "
      "\"tournament\" and \"strict\": the game type the table names, or the "
      "one the round is playing when it names none." },
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
      "in_region(name, mx, my) — whether a square is inside a named "
      "region." },

    /* The three that change nothing on the sim: what the host holds for the
       rest of the round. */
    { "timer", scnLuaTimer,
      "timer(seconds, fn) → id — run fn once, on the first tick at or after "
      "seconds from now; cancel it with the id. At most 64 wait at a time, "
      "and none outlives its round." },
    { "cancel_timer", scnLuaCancelTimer,
      "cancel_timer(id) — stop a timer that has not run yet; false when the "
      "id names none, which is what an id that has already run names." },
    { "define_region", scnLuaDefineRegion,
      "define_region(name, x, y, w, h) — name a rectangle for the rest of "
      "the round, replacing one of that name; it shares the 64 the scenario "
      "table's own regions come out of." },

    /* The writes. Each answers true, or nil with the refusal's name and one
       sentence saying what it was about. */
    { "set_stocks", scnLuaSetStocks,
      "set_stocks(p, t) — set any of t.shells, t.mines, t.armour and "
      "t.trees on player p's tank; a stock the table leaves out is left "
      "alone." },
    { "add_stocks", scnLuaAddStocks,
      "add_stocks(p, t) — the same four as amounts to add, negative to take "
      "away; each is held at the cap and at zero rather than refused." },
    { "kill_tank", scnLuaKillTank,
      "kill_tank(p[, killer]) — kill player p's tank; killer is a seat, and "
      "without one the death is the scenario's own." },
    { "teleport", scnLuaTeleport,
      "teleport(p, x, y[, dir]) — put player p's tank on a square, facing "
      "dir from 0 to 255; without dir it keeps the way it faces." },
    { "teleport_to_start", scnLuaTeleportToStart,
      "teleport_to_start(p[, n]) — put player p's tank on start n, or on "
      "the one the engine would have chosen." },
    { "set_boat", scnLuaSetBoat,
      "set_boat(p, on) — put player p's tank on a boat or take it off one; "
      "the square under it has to be water." },
    { "give_pill", scnLuaGivePill,
      "give_pill(p, n) — put pill n into player p's tank, however armoured "
      "and whoever held it." },
    { "drop_pill", scnLuaDropPill,
      "drop_pill(p, n[, x, y]) — put a pill player p is carrying back on "
      "the map, on a square or under the tank." },
    { "set_modifiers", scnLuaSetModifiers,
      "set_modifiers(p, t) — replace player p's speed, accel, turn, reload, "
      "dealt and taken percentages; a field the table leaves out goes back "
      "to the classic tank." },
    { "builder_order", scnLuaBuilderOrder,
      "builder_order(p, action, x, y) — send player p's builder out to do "
      "one of \"trees\", \"road\", \"building\", \"pill\", \"mine\" or "
      "\"boat\" on a square; the engine repairs rather than builds where "
      "the square already holds one." },
    { "builder_recall", scnLuaBuilderRecall,
      "builder_recall(p) — call player p's builder back to the tank." },
    { "kill_lgm", scnLuaKillLgm,
      "kill_lgm(p[, killer]) — kill player p's builder; killer is a seat." },
    { "builder_parachute", scnLuaBuilderParachute,
      "builder_parachute(p[, x, y]) — drop a dead builder back in, on a "
      "square or at the tank." },
    { "set_builder_carried", scnLuaSetBuilderCarried,
      "set_builder_carried(p[, trees[, mines]]) — what player p's builder "
      "is carrying; a count left out is left alone." },
    { "set_pill_owner", scnLuaSetPillOwner,
      "set_pill_owner(n, p) — hand pill n to a seat, or to nobody with "
      "game.NEUTRAL." },
    { "set_pill_armour", scnLuaSetPillArmour,
      "set_pill_armour(n, a) — how much pill n has left; 0 is a dead pill "
      "on the ground." },
    { "set_pill_speed", scnLuaSetPillSpeed,
      "set_pill_speed(n, s) — the ticks between pill n's shots." },
    { "move_pill", scnLuaMovePill,
      "move_pill(n, x, y) — put pill n on another square." },
    { "set_base_owner", scnLuaSetBaseOwner,
      "set_base_owner(n, p[, keep_stock]) — hand base n to a seat, or to "
      "nobody with game.NEUTRAL; keep_stock leaves what it holds." },
    { "set_base_stock", scnLuaSetBaseStock,
      "set_base_stock(n, armour, shells, mines) — what base n holds; a "
      "stock left out is left alone and one past the cap is held there." },
    { "add_pill", scnLuaAddPill,
      "add_pill(x, y[, owner[, armour[, speed]]]) → n — put a new pill on "
      "the map and answer which one it is; nobody's, dead, and firing at "
      "the round's own rate unless told otherwise." },
    { "remove_pill", scnLuaRemovePill,
      "remove_pill(n) — take pill n off the map; the slot stays, so the "
      "pills above it keep their numbers." },
    { "add_base", scnLuaAddBase,
      "add_base(x, y[, owner[, armour, shells, mines]]) → n — put a new "
      "base on the map and answer which one it is; nobody's and empty "
      "unless told otherwise." },
    { "remove_base", scnLuaRemoveBase,
      "remove_base(n) — take base n off the map; the slot stays." },
    { "add_start", scnLuaAddStart,
      "add_start(x, y, dir) → n — put a new start on a deep-sea square, "
      "facing dir from 0 to 15." },
    { "remove_start", scnLuaRemoveStart,
      "remove_start(n) — take start n off the map; the last one is "
      "refused." },
    { "set_tile", scnLuaSetTile,
      "set_tile(x, y, t) — write one square's terrain, by a game.TERRAIN "
      "code." },
    { "fill_rect", scnLuaFillRect,
      "fill_rect(x0, y0, x1, y1, t) — write a rectangle of terrain; one "
      "too big for a tick's budget answers true and \"queued\" and finishes "
      "over the ticks after it." },
    { "place_mine", scnLuaPlaceMine,
      "place_mine(x, y[, owner[, visible]]) — lay a mine on a square; "
      "visible shows it to everyone rather than to its owner's side." },
    { "remove_mine", scnLuaRemoveMine,
      "remove_mine(x, y) — take a mine off a square without setting it "
      "off." },
    { "spawn_bot", scnLuaSpawnBot,
      "spawn_bot(t) → p, \"queued\" — put a bot into the running round; t "
      "takes name, brain, team, slot, start, loadout and a flat init "
      "table, all of them optional." },
    { "remove_bot", scnLuaRemoveBot,
      "remove_bot(p) → true, \"queued\" — take a bot out of the running "
      "round; a human seat is refused." },
    { "set_team", scnLuaSetTeam,
      "set_team(p, t) — move a seat to another team mid-round." },
    { "lobby_add_bot", scnLuaLobbyAddBot,
      "lobby_add_bot(t) → p — seat a bot in the lobby and answer which seat "
      "it took; t takes name, brain, team, slot and fielded." },
    { "lobby_remove_bot", scnLuaLobbyRemoveBot,
      "lobby_remove_bot(p) — take a bot out of the lobby; a human seat is "
      "refused." },
    { "lobby_set_team", scnLuaLobbySetTeam,
      "lobby_set_team(p, t) — move a lobby seat to another team." },
    { "hint", scnLuaHint,
      "hint(p, t) — hand bot p's brain an order: a flat table with a verb "
      "and whatever else the brain reads. Values may be strings, numbers or "
      "true/false and all reach the brain as text. A brain that takes no "
      "hints ignores it." },
    { "message", scnLuaMessage,
      "message(text[, target]) — a line to everyone, to one seat with a "
      "number, or to a team with { team = t }." },
    { "say", scnLuaSay,
      "say(p, text[, target]) — a chat line seat p says: to its own team "
      "with no target, to everyone with \"all\", or to one seat with a "
      "number. Unlike message, which is the server talking, this reaches a "
      "bot's inbox and fires on_chat." },
    { "sound", scnLuaSound,
      "sound(name[, x, y]) — play one of the server's sounds, at a square "
      "or everywhere." },
    { "log", scnLuaLog,
      "log(text) — write a line to the server's console; no player sees "
      "it." },
    { "panel", scnLuaPanel,
      "panel(id, list[, target]) — draw a panel from a list of primitives, "
      "each an array with its name first: { \"rect\", x, y, w, h, colour, "
      "fill }, { \"text\", x, y, colour, size, align, s } and so on. An "
      "empty list clears the panel, and one update per panel per audience "
      "per tick is taken." },
    { "score", scnLuaScore,
      "score(target, value[, label]) — the scenario's own score for one "
      "seat with a number, or for a team with { team = t }; label is the "
      "short word shown beside it." },
    { "announce", scnLuaAnnounce,
      "announce(text, seconds[, target]) — a line across the centre of the "
      "screen for that many seconds; empty text takes the line away." },
    { "marker", scnLuaMarker,
      "marker(id, x, y[, colour[, target]]) — put mark id on a map square; "
      "a second marker on the same id replaces the first." },
    { "marker_follow", scnLuaMarkerFollow,
      "marker_follow(id, p[, colour[, target]]) — put mark id on seat p, "
      "where it rides the tank rather than a square." },
    { "clear_marker", scnLuaClearMarker,
      "clear_marker(id[, target]) — take mark id off the map." },
    { "end_round", scnLuaEndRound,
      "end_round([text[, winner_team]]) — end the round now, with the line "
      "the lobby shows and the team that won it." },
    { "set_game_time", scnLuaSetGameTime,
      "set_game_time(ticks) — how long the round has left." },
    { "add_game_time", scnLuaAddGameTime,
      "add_game_time(ticks) — add to what the round has left, or take away "
      "with a negative." },
    { "set_rule", scnLuaSetRule,
      "set_rule(name, value) — write one of the gameplay rules; a name that "
      "spells no rule raises, and a value the table will not take is "
      "refused." },
    { "shell_expired", scnLuaShellExpired,
      "shell_expired(p, x, y [, fire_tick]) — post one of seat p's shells as "
      "having run its full range and died over square (x, y) with nothing "
      "hit; three on one open square inside two seconds, with a quiet second "
      "either side, are the three-shot order the bots read. fire_tick is "
      "when the shell left the gun, which is the tick every timing rule "
      "reads; left out, it counts as fired now. A test hook: nothing else "
      "about the shell happens." },
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

/* One parameter-name array per row, built from the two lists in
 * scenario_lua.h. Each array ends on a NULL, so a row's count below is the
 * array's length less that terminator and the two cannot disagree. */
#define SCN_FN_HOOK_PARAMS(id, name, kind, params)                            \
    static const char *const kScnFnHook_##id[] = { params NULL };
SCN_HOOK_LIST(SCN_FN_HOOK_PARAMS)
#undef SCN_FN_HOOK_PARAMS

#define SCN_FN_POLICY_PARAMS(id, name, params, returns)                       \
    static const char *const kScnFnPolicy_##id[] = { params NULL };
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
