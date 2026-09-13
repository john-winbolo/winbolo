/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Lua
 *Filename:      scenario_lua.h
 *Author:        John Morrison
 *Purpose:
 *  The game table a script reads the sim through: one row
 *  per script-visible function, the three index rules the
 *  rows convert by, and the name a script spells a rule
 *  with.
 *
 *  The rows are the surface. docs/SCENARIO_API.md and the
 *  editor's completion list are written from the tables
 *  below, so a row carries the line that describes it and a
 *  row without one cannot be documented.
 *
 *  This is the library's header and the tests', not a
 *  frontend's. It names Lua types, so whatever includes it
 *  links lua_static for the headers; scenario_host.h, which
 *  is what a frontend includes, names none.
 *********************************************************/

#ifndef SCENARIO_LUA_H
#define SCENARIO_LUA_H

#include <stdbool.h>
#include <stddef.h>

#include <lua.h>

#include "server_sim.h"       /* ServerSim, BYTE */
#include "scenario_manifest.h"

/* What a row reads. The host holds one of these for as long as the VM it
 * installed the table on, and every row reads through it rather than
 * through a copy, so a round start that reads a new manifest into the same
 * struct is the table the next call sees. */
typedef struct {
    ServerSim              *sim;
    const ScenarioManifest *manifest;
} ScnLuaCtx;

/* One script-visible function: what it is called, what runs it, and the one
 * line a document says about it. */
typedef struct {
    const char   *name;
    lua_CFunction fn;
    const char   *doc;
} ScnLuaRow;

/* One script-visible number that is not a call. */
typedef struct {
    const char *name;
    int         value;
    const char *doc;
} ScnLuaConst;

/* One member of game.TERRAIN: the name a script writes and the code the map
 * holds. */
typedef struct {
    const char *name;
    BYTE        code;
} ScnLuaTerrain;

/*********************************************************
 *NAME:          scenarioLuaInstall
 *PURPOSE:
 *  Builds the game table from the rows and sets it as the
 *  global. Called at every VM boot, before the chunk runs,
 *  because a script may read the game at chunk scope.
 *
 *  ctx is not copied. It must outlive the state.
 *********************************************************/
void scenarioLuaInstall(lua_State *L, const ScnLuaCtx *ctx);

/*********************************************************
 *NAME:          scenarioLuaRows
 *PURPOSE:
 *  The registry, for a document, a completion list or a
 *  test that wants to reach every row. *count is how many.
 *********************************************************/
const ScnLuaRow *scenarioLuaRows(size_t *count);

/*********************************************************
 *NAME:          scenarioLuaConsts
 *PURPOSE:
 *  The numbers the table carries beside the calls.
 *********************************************************/
const ScnLuaConst *scenarioLuaConsts(size_t *count);

/*********************************************************
 *NAME:          scenarioLuaTerrain
 *PURPOSE:
 *  The members of game.TERRAIN, in the order it names them.
 *********************************************************/
const ScnLuaTerrain *scenarioLuaTerrain(size_t *count);

/* ── The three index rules ──────────────────────────────────────────
 *
 * Players are 0-based slots in Lua and in C, and convert nowhere. Pills,
 * bases and starts are 1-based in Lua, and the C side is not uniform: the
 * read accessors are 1-based and the ops are 0-based. One helper per
 * direction, and the three of them are the only place an item index
 * changes. */

/*********************************************************
 *NAME:          scenarioLuaIndexToRead
 *PURPOSE:
 *  A Lua item index as a read accessor takes it. Both are
 *  1-based, so this is the direction that passes straight
 *  through. An index no list could hold — zero, negative,
 *  or past a byte — comes back as 0, which every 1-based
 *  accessor refuses.
 *********************************************************/
BYTE scenarioLuaIndexToRead(lua_Integer n);

/*********************************************************
 *NAME:          scenarioLuaIndexToOp
 *PURPOSE:
 *  A Lua item index as an op payload carries it. The ops
 *  are 0-based, so this is the direction that subtracts.
 *  An index no list could hold comes back as 255, which is
 *  past every entity list a map has and is refused as no
 *  such item.
 *********************************************************/
BYTE scenarioLuaIndexToOp(lua_Integer n);

/*********************************************************
 *NAME:          scenarioLuaIndexToScript
 *PURPOSE:
 *  An item index from an event payload or a policy question
 *  as Lua reads it. Both of those are 0-based, so this is
 *  the direction that adds.
 *********************************************************/
lua_Integer scenarioLuaIndexToScript(int n);

/* ── The rule names ─────────────────────────────────────────────────── */

/*********************************************************
 *NAME:          scenarioLuaRuleIndex
 *PURPOSE:
 *  The index of the rule a name spells, or -1 for a name
 *  that spells none. A rule's name in a script is its name
 *  in the rule list, so the list is the only place the
 *  spelling exists: the sidecar's rules table and the
 *  game.rule row resolve a name through this one lookup.
 *********************************************************/
int scenarioLuaRuleIndex(const char *name);

/*********************************************************
 *NAME:          scenarioLuaRuleName
 *PURPOSE:
 *  What a rule is called, for an operator line that has an
 *  index and needs to say which rule it was. "" for an
 *  index that names no rule.
 *********************************************************/
const char *scenarioLuaRuleName(int rule);

#endif /* SCENARIO_LUA_H */
