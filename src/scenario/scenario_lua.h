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
#include <stdint.h>

#include <lua.h>

#include "server_sim.h"       /* ServerSim, BYTE */
#include "scenario_manifest.h"

/* One timer a script is waiting on: the tick it comes due, the function to
 * call, and the id the script cancels it by.
 *
 * ref is LUA_NOREF for a free entry and is the only thing that says an entry
 * is taken. It is a registry reference because the function has to survive
 * every collection between the call that set it and the tick it runs on.
 * Whoever takes the entry out owns the reference and has to release it. */
typedef struct {
    uint32_t id;
    uint32_t dueTick;
    int      ref;
} ScnTimer;

/* The timers of one round.
 *
 * nextId only ever rises, and it rises for the life of the host rather than
 * of a round. An entry is reused; an id is not. That is what makes a stale
 * id safe to cancel: it matches nothing, rather than matching whatever has
 * since moved into the entry it used to hold. */
typedef struct {
    ScnTimer entries[SCN_TIMERS_MAX];
    uint32_t nextId;
} ScnTimerSet;

/* What a row reads. The host holds one of these for as long as the VM it
 * installed the table on, and every row reads through it rather than
 * through a copy, so a round start that reads a new manifest into the same
 * struct is the table the next call sees.
 *
 * The manifest is not const: define_region writes a region into it, which is
 * the one row that writes here rather than through the op funnel. It is the
 * round's own copy of the table — a round start reads the sidecar's bytes
 * over it — so what a script defines lasts the round and no longer.
 *
 * timers may be NULL, which leaves a state with no timers: the row refuses
 * rather than reaching through nothing.
 *
 * checkOnly says this state is reading a file to see whether it can be used,
 * not running it. The reload sets it: the edited file's top level runs in a
 * state of its own, and a top level that writes would otherwise apply to the
 * round that is playing, against the one thing a reload promises — that
 * nothing changes until the next round start. Every row that reaches the op
 * funnel refuses while it is set, and the rows that read answer as they
 * always do, which is what the check is for. */
typedef struct {
    ServerSim        *sim;
    ScenarioManifest *manifest;
    ScnTimerSet      *timers;
    bool              checkOnly;
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

/* ── Regions ────────────────────────────────────────────────────────── */

/*********************************************************
 *NAME:          scenarioLuaRegionFind
 *PURPOSE:
 *  The region of that name, or NULL for a name nothing
 *  carries. Declared and defined regions are one list, so
 *  this finds either.
 *********************************************************/
const ScnManifestRegion *scenarioLuaRegionFind(const ScenarioManifest *m,
                                               const char *name);

/*********************************************************
 *NAME:          scenarioLuaRegionHolds
 *PURPOSE:
 *  Whether a square is inside a region. Half-open on both
 *  axes — x to x + w - 1 — and a rectangle with no width or
 *  no height holds nothing.
 *
 *  The one place the test is written. game.in_region answers
 *  from it and the host's per-tick scan decides from it, so
 *  a script cannot be told it is inside a region the hook
 *  disagrees about.
 *********************************************************/
bool scenarioLuaRegionHolds(const ScnManifestRegion *r, int mx, int my);

/* ── Timers ─────────────────────────────────────────────────────────── */

/*********************************************************
 *NAME:          scenarioLuaTimersReset
 *PURPOSE:
 *  Empties the set without touching Lua. For a state that
 *  has already been closed, where every reference in it died
 *  with the state and there is nothing left to release.
 *********************************************************/
void scenarioLuaTimersReset(ScnTimerSet *t);

/*********************************************************
 *NAME:          scenarioLuaTimersDrop
 *PURPOSE:
 *  Releases every waiting timer's function against the state
 *  that holds it, then empties the set. Called before a
 *  state is closed — a round start swapping VMs, or a
 *  detach — so nothing is left holding a reference into a
 *  state that is going away.
 *********************************************************/
void scenarioLuaTimersDrop(lua_State *L, ScnTimerSet *t);

/*********************************************************
 *NAME:          scenarioLuaTimersTakeDue
 *PURPOSE:
 *  Takes every timer due at or before now out of the set and
 *  writes their functions' references into out, oldest first
 *  — the order they were set in, which is the order their
 *  ids run in.
 *
 *  The due set is read once, so a timer one of them sets
 *  while running waits for the next tick however short its
 *  delay: a run that feeds itself moves one call per tick
 *  and cannot spin inside one, which is the rule the event
 *  drain follows.
 *
 *  Returns how many were taken. The caller owns each
 *  reference and must release it.
 *********************************************************/
int scenarioLuaTimersTakeDue(ScnTimerSet *t, uint32_t now, int *out,
                             int outMax);

/* ── The words an event's payload reads as ──────────────────────────── */

/*********************************************************
 *NAME:          scenarioLuaBuiltActionWord
 *PURPOSE:
 *  What the action byte of a build event is called, as the
 *  word a hook is handed. The builder's six request codes,
 *  with one difference from the word an order takes: a build
 *  of kind pill on this event is always a repair, because a
 *  new pillbox going down is its own event. NULL for a
 *  number that names no action.
 *********************************************************/
const char *scenarioLuaBuiltActionWord(int action);

/*********************************************************
 *NAME:          scenarioLuaDeathCauseWord
 *PURPOSE:
 *  What a tank died of, as the word a hook is handed: the
 *  four LAST_DEATH_BY_* values the engine writes. NULL for a
 *  number that names none of them.
 *********************************************************/
const char *scenarioLuaDeathCauseWord(int cause);

/* ── The words a policy question reads as ───────────────────────────── */

/*********************************************************
 *NAME:          scenarioLuaBuildOrderWord
 *PURPOSE:
 *  What a build order is called, as the word can_build is
 *  handed. The same six request codes the event word above
 *  takes, spelled as the order rather than as the job: a
 *  pill order is "pill", because the engine has not yet
 *  decided whether the square makes it a repair. NULL for a
 *  number that names no order.
 *********************************************************/
const char *scenarioLuaBuildOrderWord(int action);

/*********************************************************
 *NAME:          scenarioLuaCaptureKindWord
 *PURPOSE:
 *  What is being taken, as the word can_capture is handed.
 *  NULL for a kind the surface does not name.
 *********************************************************/
const char *scenarioLuaCaptureKindWord(int kind);

/*********************************************************
 *NAME:          scenarioLuaDieKindWord
 *PURPOSE:
 *  What the blow would destroy, as the word can_die is
 *  handed. NULL for a kind the surface does not name.
 *********************************************************/
const char *scenarioLuaDieKindWord(int kind);

/*********************************************************
 *NAME:          scenarioLuaDamageSourceWord
 *PURPOSE:
 *  What inflicted a hit, as the word can_die is handed for a
 *  builder or a pill — a tank's cause reads through
 *  scenarioLuaDeathCauseWord instead, and the two vocabularies
 *  spell a shell and a mine the same way. NULL for a source
 *  the site could not name, DMG_SRC_UNKNOWN included.
 *********************************************************/
const char *scenarioLuaDamageSourceWord(int source);

/*********************************************************
 *NAME:          scenarioLuaAnnounceKindWord
 *PURPOSE:
 *  Which newswire-worthy fact is being put to the announce
 *  policy, as the word the script is handed. NULL for a kind
 *  the surface does not name.
 *********************************************************/
const char *scenarioLuaAnnounceKindWord(int kind);

/*********************************************************
 *NAME:          scenarioLuaLoadoutFromWord
 *PURPOSE:
 *  The game type a loadout word names — "open",
 *  "tournament" or "strict" — for a spawn_loadout answer
 *  handed back as a word rather than as four amounts. The
 *  same three words a spawn op takes, read from the one
 *  table. False for a word the set does not hold, which the
 *  host answers classic for.
 *********************************************************/
bool scenarioLuaLoadoutFromWord(const char *word, int *out);

/* ── The rule names ─────────────────────────────────────────────────── */

/*********************************************************
 *NAME:          scenarioLuaResultName
 *PURPOSE:
 *  What an op answered, as the word a refusal hands a
 *  script: the enumerator's own spelling, "SCN_OP_RANGE" and
 *  its neighbours. One table builds these, so the string a
 *  script reads and the string a case asserts are the same
 *  one. "" for a number that names no result.
 *********************************************************/
const char *scenarioLuaResultName(int result);

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
