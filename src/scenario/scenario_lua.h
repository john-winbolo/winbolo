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
#include "scenario_table.h"   /* ScnTable — what the reader below fills */
#include "scenario_host.h"     /* SCN_TIMERS_MAX — the timer table below */
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
 * round's own copy of the table — a round start reads the script's bytes
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

/* One word a script may write for a small argument, and the number the
 * payload carries for it. */
typedef struct {
    const char *word;
    int         value;
} ScnLuaWord;

/* One of the word tables the game table carries beside TERRAIN — COLOUR,
 * SIZE, ALIGN and TIMER_MODE — under the name a script indexes it by.
 *
 * The members are the same rows the argument readers match a word against,
 * so a word a panel primitive takes is a name the table holds and a document
 * written from here names every word the surface accepts. */
typedef struct {
    const char       *name;
    const ScnLuaWord *words;
    size_t            count;
} ScnLuaWordTable;

/* ── The functions a scenario author writes ─────────────────────────── */

/* What kind of function this is, which decides what the host does with it
 * and what a stub for it looks like. */
typedef enum {
    SCN_FN_LIFECYCLE_HOOK, /* the round reaching a point of its own; comes
                              off no queue and takes no scripted flag */
    SCN_FN_EVENT_HOOK,     /* off the event queue; takes the trailing
                              scripted flag */
    SCN_FN_REGION_HOOK,    /* derived from tank positions once a tick; no
                              scripted flag */
    SCN_FN_POLICY          /* the host asks it a question and reads the
                              answer */
} ScnLuaFnKind;

/* One function the author writes and the host calls. params are the
 * author's own parameter names in order, without the trailing scripted flag
 * an event hook carries — kind is what says whether there is one. returns
 * says what a policy answers, and is NULL for a hook, which answers
 * nothing. */
typedef struct {
    const char        *name;
    ScnLuaFnKind       kind;
    const char *const *params;
    size_t             paramCount;
    const char        *returns;
} ScnLuaFnRow;

/* The parameter names of one row, as a list below writes them. Each of
 * these leaves its names followed by a comma, so the array built from a row
 * closes on the NULL that ends it however many names there are, none
 * included. Five is as many as any function on the surface takes. */
#define SCN_FN_ARGS0()
#define SCN_FN_ARGS1(a)             a,
#define SCN_FN_ARGS2(a, b)          a, b,
#define SCN_FN_ARGS3(a, b, c)       a, b, c,
#define SCN_FN_ARGS4(a, b, c, d)    a, b, c, d,
#define SCN_FN_ARGS5(a, b, c, d, e) a, b, c, d, e,

/* Every function a round may call, in one list: the four lifecycle calls
 * and one per event the host turns into a hook. The name a script writes it
 * under is here and nowhere else, so the name the boot resolves, the name a
 * case defines and the name a document is written from cannot drift apart.
 *
 * The four at the top are not events. They are made where the round reaches
 * the point each of them names, and they take no trailing scripted boolean
 * because there is no event behind them to have caused.
 *
 * Nor do the last two, and for the same reason read the other way round.
 * Every hook between them comes off the queue, where the entry carries the
 * mark saying whether the scenario caused the fact. The two region hooks
 * come off no queue at all: the host samples where the tanks are once a
 * tick and compares that against the sample before it, so what it has is a
 * difference between two pictures and not an action anybody took. A
 * boolean there could only be a guess about which of the things that moved
 * a tank since the last sample mattered, and a script reading it would be
 * reading a guess. See scnScanRegions for what bounds the loop instead.
 *
 * The parameter names are the author's own, in order, and stop short of the
 * scripted flag an event hook carries — the kind beside them is what says
 * there is one. scenario_host.c expands this list for the ScnHookId enum it
 * dispatches by, so the order of the rows is the order of that enum and a
 * row moved here moves a value the host indexes with. */
#define SCN_HOOK_LIST(X)                                                     \
    X(SETUP,            "on_setup",            SCN_FN_LIFECYCLE_HOOK,        \
      SCN_FN_ARGS0())                                                        \
    X(START,            "on_start",            SCN_FN_LIFECYCLE_HOOK,        \
      SCN_FN_ARGS0())                                                        \
    X(TICK,             "on_tick",             SCN_FN_LIFECYCLE_HOOK,        \
      SCN_FN_ARGS1("tick"))                                                  \
    X(END,              "on_end",              SCN_FN_LIFECYCLE_HOOK,        \
      SCN_FN_ARGS0())                                                        \
    X(LOBBY,            "on_lobby",            SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS1("p"))                                                     \
    X(PLAYER_JOIN,      "on_player_join",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS1("p"))                                                     \
    X(PLAYER_LEAVE,     "on_player_leave",     SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS1("p"))                                                     \
    X(TEAM_CHANGED,     "on_team_changed",     SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("p", "team"))                                             \
    X(CHAT,             "on_chat",             SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("p", "text"))                                             \
    X(PING,             "on_ping",             SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", "kind", "mx", "my"))                                 \
    X(TANK_SPAWNED,     "on_tank_spawned",     SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", "mx", "my", "respawn"))                              \
    X(TANK_KILLED,      "on_tank_killed",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("victim", "killer", "cause"))                             \
    X(LGM_DIED,         "on_lgm_died",         SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", "killer", "mx", "my"))                               \
    X(LGM_LANDED,       "on_lgm_landed",       SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("p", "mx", "my"))                                         \
    X(BASE_CAPTURED,    "on_base_captured",    SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("n", "old", "new"))                                       \
    X(BASE_NEUTRALIZED, "on_base_neutralized", SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("n", "old"))                                              \
    X(PILL_CAPTURED,    "on_pill_captured",    SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("n", "old", "new"))                                       \
    X(PILL_PLACED,      "on_pill_placed",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("n", "p", "armour"))                                      \
    X(PILL_PICKED_UP,   "on_pill_picked_up",   SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("n", "p"))                                                \
    X(PILL_KILLED,      "on_pill_killed",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("n", "by"))                                               \
    X(BUILT,            "on_built",            SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", "action", "x", "y"))                                 \
    X(MINE_LAID,        "on_mine_laid",        SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("p", "mx", "my"))                                         \
    X(MINE_EXPLOSION,   "on_mine_explosion",   SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("mx", "my", "layer"))                                     \
    X(ENTER_REGION,     "on_enter_region",     SCN_FN_REGION_HOOK,           \
      SCN_FN_ARGS2("p", "name"))                                             \
    X(LEAVE_REGION,     "on_leave_region",     SCN_FN_REGION_HOOK,           \
      SCN_FN_ARGS2("p", "name"))

/* Every question the host puts to a round, in one list, in the order the
 * engine reaches them: the two the lobby and the round's endings ask, then
 * the ones asked in the middle of doing something.
 *
 * A policy is looked up by name at each call rather than resolved once, so
 * unlike a hook it has no enum and no index — the name in the row is the
 * whole of what the host needs, and the string at the call site is the same
 * string this list holds. The id column is the name in upper case and names
 * nothing but the tables an expansion of this list generates, which is why
 * it can carry the on_ a hook id drops.
 *
 * The last column is what the answer means. Every policy also answers
 * nothing, by not being declared or by returning nil, and that is the
 * ordinary rule rather than a value; see docs/SCENARIO_API.md for which
 * rule each one falls back to. */
#define SCN_POLICY_LIST(X)                                                   \
    X(ALLOW_EXTRA_TEAMS, "allow_extra_teams", SCN_FN_ARGS0(),                \
      "true to let the seat sit on a team of its own")                       \
    X(ALLOW_BASE_WIN,    "allow_base_win",    SCN_FN_ARGS0(),                \
      "false to take that ending out of the round")                          \
    X(CAN_RESPAWN,       "can_respawn",       SCN_FN_ARGS1("p"),             \
      "false to hold the tank where it is")                                  \
    X(CAN_BUILD,         "can_build",                                        \
      SCN_FN_ARGS5("p", "action", "x", "y", "n"),                            \
      "false to refuse the order")                                           \
    X(CAN_CAPTURE,       "can_capture",                                      \
      SCN_FN_ARGS3("kind", "n", "p"),                                        \
      "false to leave the objective where it is")                            \
    X(ANNOUNCE,          "announce",                                         \
      SCN_FN_ARGS3("kind", "subject", "actor"),                              \
      "false to keep the line off every newswire")                           \
    X(CAN_DIE,           "can_die",                                          \
      SCN_FN_ARGS4("kind", "n", "killer", "cause"),                          \
      "false to leave what the blow landed on standing")                     \
    X(ON_CHOOSE_START,   "on_choose_start",   SCN_FN_ARGS1("p"),             \
      "a start number, counted from 1 as game.start counts")                 \
    X(SPAWN_LOADOUT,     "spawn_loadout",     SCN_FN_ARGS1("p"),             \
      "a loadout word, or a table of shells, mines, armour and trees")       \
    X(DAMAGE_SCALE,      "damage_scale",                                     \
      SCN_FN_ARGS3("attacker", "victim", "cause"),                           \
      "a percent from 0 to 10000, where 100 is the ordinary amount")

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

/*********************************************************
 *NAME:          scenarioLuaWordTables
 *PURPOSE:
 *  The word tables the game table carries beside TERRAIN,
 *  in the order it names them.
 *********************************************************/
const ScnLuaWordTable *scenarioLuaWordTables(size_t *count);

/*********************************************************
 *NAME:          scenarioLuaFunctions
 *PURPOSE:
 *  Every hook and every policy a scenario author writes,
 *  as one list: the hooks first, in the order the host
 *  dispatches them, then the policies. *count is how many.
 *
 *  The rows are built from SCN_HOOK_LIST and
 *  SCN_POLICY_LIST above, which are the same two lists the
 *  host resolves its hooks from and names its policies at,
 *  so what this answers is what the host actually calls.
 *********************************************************/
const ScnLuaFnRow *scenarioLuaFunctions(size_t *count);

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

/* ── The flat table a bot is built with ─────────────────────────────── */

/* Why a read of one of these tables stopped. */
typedef enum {
    SCN_TABLE_READ_OK = 0,
    SCN_TABLE_READ_NOT_TABLE,   /* the field is there and is not a table */
    SCN_TABLE_READ_BAD_KEY,     /* a key that is not a name */
    SCN_TABLE_READ_BAD_VALUE,   /* a value that is not a string or a number */
    SCN_TABLE_READ_NO_ROOM      /* a 17th pair, or a key or value too long */
} ScnTableRead;

/* Room for the sentence a stopped read writes. */
#define SCN_TABLE_WHY_LEN 128

/*********************************************************
 *NAME:          scenarioLuaReadTable
 *PURPOSE:
 *  Reads the flat key/value table under `key` off the Lua
 *  table at `idx`. Keys are names and values are text, with
 *  a number written out as the text of itself, because that
 *  is what a brain reads.
 *
 *  The one walk, for the two things that read one of these
 *  off Lua: spawn_bot's own init argument, and the team
 *  block of the scenario table. spawn_bot refuses a bad
 *  argument by raising; the manifest read runs outside a
 *  protected call and cannot raise at all. So this answers
 *  rather than raising, and each caller does what it has to
 *  with the answer — which is what keeps one reader behind
 *  both.
 *
 *  out is cleared first, so a missing field leaves an empty
 *  table and SCN_TABLE_READ_OK. A read that stops leaves the
 *  pairs taken before the stop in out; it never empties what
 *  it had already read.
 *
 *  badKey, when given, is the pair the read stopped on, and
 *  "" when nothing stopped it or the field was not a table
 *  at all. A key that is not a name is written as its own
 *  text where it has any, and as the name of its Lua type
 *  where it has none.
 *
 *  why, when given, is the reason, written to follow the
 *  field's own name: printing "t.%s%s" with the key and this
 *  reads as one sentence. "" when nothing stopped the read.
 *
 *ARGUMENTS:
 *  L      - The Lua state
 *  idx    - Stack index of the table to read the field off
 *  key    - The field's name
 *  out    - The table to fill
 *  badKey - Buffer for the pair that stopped it, or NULL
 *  badCap - Its size
 *  why    - Buffer for the reason, or NULL
 *  whyCap - Its size
 *********************************************************/
ScnTableRead scenarioLuaReadTable(lua_State *L, int idx, const char *key,
                                  ScnTable *out, char *badKey, size_t badCap,
                                  char *why, size_t whyCap);

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

#endif /* SCENARIO_LUA_H */
