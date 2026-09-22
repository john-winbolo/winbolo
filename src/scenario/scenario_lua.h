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
 * Whoever takes the entry out owns the reference and has to release it.
 *
 * owner is the table of the script that set the timer, read off the context
 * at the moment it was set. The set is one id space for the whole list, so
 * without this the tick that runs a timer would not know whose function it
 * is calling, and the row that has to know — see running below — would have
 * no answer at all. NULL where the timer was set from a file's top level, or
 * from a state that has no list behind it. */
typedef struct {
    uint32_t                id;
    uint32_t                dueTick;
    int                     ref;
    const ScenarioManifest *owner;
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
 * running is the table of the one script whose own code is on the stack, and
 * it is a different question from manifest. manifest is the composite the
 * whole list adds up to; running is the entry the host is inside a call into.
 * A state runs several scripts and the two differ whenever the caller is not
 * the base: the composite's kind is the base's, so a row that has to know
 * whether a mod is calling it has to read this one. The host writes it around
 * every call it makes into a script's code and puts back what was there
 * before, so a hook that leads to another hook leaves it where it found it.
 *
 * There is no falling back to manifest. The composite says what the round
 * is, not who is calling it, and on any list holding a scenario it says
 * "scenario" for every mod on that list — which is the whole of the defect
 * this field was added for.
 *
 * NULL means no script's own code is running. That is a file's top level:
 * the chunk runs before scnReadManifest has read its table out of its
 * globals, so the kind is not known and cannot be. A row that has to know
 * which script is calling has no answer at that moment, and each such row
 * decides for itself what to make of that. The win-condition guard refuses
 * there rather than guessing, which is scnNotTheDecider in
 * src/scenario/scenario_lua.c. Refusing is not the only right answer: a row
 * that filters by the calling script may read NULL as no filter at all.
 *
 * runningOwner is which entry of the list that same script is, in the
 * encoding SCN_OWNER_OF_ENTRY spells. It answers the question running
 * answers, but as a position rather than as a table, and the region rows are
 * what want it that way: a composed round may hold two regions under one
 * name, one per script, and the rectangle a lookup means is the asking
 * script's own. scenarioLuaRegionFind below is the whole rule.
 *
 * It is written wider than running is, and deliberately so. The host sets it
 * around every call it makes into a script, the way it sets running, and it
 * also sets it across a file's own top level, where running is NULL and has
 * to be: the chunk runs before scnReadManifest has read the table out of its
 * globals, so what kind of file it is is not known yet, while which entry of
 * the list it is has been known since the host picked the list. That is what
 * lets game.define_region called at a top level land on the right owner
 * rather than on nobody. No row reads this field as "a script is running";
 * the win-condition guard reads running for that and still refuses at a top
 * level.
 *
 * SCN_OWNER_NONE means the asker is not one of the round's scripts — a call
 * the engine made rather than a script, or a state with no list behind it.
 * A region lookup made with it takes the first region of that name whoever
 * owns it.
 *
 * runningFile is the same script said a third way: where its bytes came
 * from, as the host holds the path. It is set and put back in step with
 * runningOwner, in the same places and across the same calls, so a row that
 * has one has the other.
 *
 * Only one row reads it, and it is the reason the field exists at all:
 * game.define_region needs the key a region's bit is worked out from, and
 * that key is the naming file's own name rather than its position on the
 * list — a position moves when a host reorders the list and a file name
 * does not. runningOwner cannot stand in for it, because it is exactly the
 * position. The path is what is carried and not the name, because the host
 * holds the path; scenarioLuaRegionBit takes the name off the end of it, so
 * two servers that keep their scenarios in different directories work out
 * the same bit for the same script.
 *
 * NULL is a call from no script of the round's, and hashes as an empty
 * name. Nothing else reads it, so a caller that has a file and no use for
 * one may leave it NULL.
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
    ServerSim              *sim;
    ScenarioManifest       *manifest;
    const ScenarioManifest *running;
    uint8_t                 runningOwner;
    const char             *runningFile;
    ScnTimerSet            *timers;
    bool                    checkOnly;
} ScnLuaCtx;

/* What one parameter holds, beside the name the author writes it under. A
 * name says what a parameter is called and nothing about what may be put in
 * it, so the type is what lets a caller tell a seat from a pillbox number
 * and work out which fields a value of that kind carries with it.
 *
 * Both catalogues below are typed from this one list: the ops a script
 * calls, and the functions an author writes.
 *
 * SCN_PARAM_SLOT and SCN_PARAM_OWNER are both a player slot; the difference
 * is that an owner may be NEUTRAL, so anything reading one has nobody to
 * ask about as well as a seat.
 *
 * The last two are what an op takes and a hook never does: an argument with
 * a shape of its own, rather than the one value a form could hold. */
typedef enum {
    SCN_PARAM_NONE,      /* the terminator of a row's array, and nothing
                            else */
    SCN_PARAM_SLOT,      /* a player slot, 0-based, always a real seat */
    SCN_PARAM_OWNER,     /* a player slot or NEUTRAL: a seat, or nobody */
    SCN_PARAM_TEAM,      /* a team number */
    SCN_PARAM_PILL,      /* a pillbox index, 1-based as a script counts */
    SCN_PARAM_BASE,      /* a base index, 1-based as a script counts */
    SCN_PARAM_ITEM,      /* a pill, base or start index, which of the three
                            decided by a kind argument beside it */
    SCN_PARAM_SQUARE_X,  /* a map square's x */
    SCN_PARAM_SQUARE_Y,  /* a map square's y */
    SCN_PARAM_WORD,      /* one of a fixed set of strings the surface names */
    SCN_PARAM_TARGET,    /* who a line is addressed to: a seat number, or one
                            of the words the surface names for a wider
                            audience. A number or a word and nothing else —
                            the third form a binding takes, { team = t }, is
                            a table, and this type does not promise it. A
                            trigger's argument cannot be a table at all, so
                            an action addresses a seat or everyone. */
    SCN_PARAM_COLOUR,    /* the palette's word, or the number behind it.
                            scnArgColour is the reader: a string goes to the
                            palette's word set and anything else is read as
                            the index, so both spellings of red are the same
                            argument. Left out, it takes the row's own
                            default, which is why every colour is optional.
                            SCN_PARAM_WORD is the narrower one beside it —
                            a word and nothing else, which is what every
                            other word argument on the surface takes. */
    SCN_PARAM_NUMBER,
    SCN_PARAM_STRING,    /* free text */
    SCN_PARAM_BOOL,
    SCN_PARAM_TAG,       /* a scenario tag; a derived field's type */
    SCN_PARAM_REGION,    /* a declared region's name */
    SCN_PARAM_TABLE,     /* a Lua table of named fields, or a list */
    SCN_PARAM_FUNCTION   /* a Lua function the host holds and calls later */
} ScnLuaParamType;

/* One argument an op takes: the name the document writes it under, what may
 * be put in it, and whether a call may leave it out. An optional argument is
 * one the doc string writes inside brackets. */
typedef struct {
    const char     *name;
    ScnLuaParamType type;
    bool            optional;
} ScnLuaOpParam;

/* One script-visible function: what it is called, what runs it, the one line
 * a document says about it, and the arguments it takes.
 *
 * The doc opens with the call's own signature and params says the same thing
 * as data, name for name and bracket for bracket, so a caller that has to
 * count arguments or tell one kind from another reads the row rather than
 * the sentence. */
typedef struct {
    const char          *name;
    lua_CFunction        fn;
    const char          *doc;
    const ScnLuaOpParam *params;
    size_t               paramCount;
    /* Whether the op changes something the round can observe, as against a
       read accessor that answers a value and leaves the round where it was.
       The engine draws the same line itself: a state that is checking a file
       rather than running one turns every acting op down and lets every read
       answer, so the check-only refusal is what this column is written
       from. */
    bool                 acts;
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

/* One parameter of one function: the name the author writes and what may be
 * put in it. */
typedef struct {
    const char     *name;
    ScnLuaParamType type;
} ScnLuaFnParam;

/* One function the author writes and the host calls. params are the
 * author's own parameter names in order, each with the type of what the
 * host puts in it, and without the trailing scripted flag an event hook
 * carries — kind is what says whether there is one. returns says what a
 * policy answers, and is NULL for a hook, which answers nothing. */
typedef struct {
    const char          *name;
    ScnLuaFnKind         kind;
    const ScnLuaFnParam *params;
    size_t               paramCount;
    const char          *returns;
} ScnLuaFnRow;

/* The parameters of one row, as a list below writes them: a name and a type
 * for each. Each of these leaves its pairs followed by a comma, so the array
 * built from a row closes on the terminator that ends it however many pairs
 * there are, none included. Five is as many as any function on the surface
 * takes. */
#define SCN_FN_ARGS0()
#define SCN_FN_ARGS1(n1, t1) { n1, t1 },
#define SCN_FN_ARGS2(n1, t1, n2, t2) { n1, t1 }, { n2, t2 },
#define SCN_FN_ARGS3(n1, t1, n2, t2, n3, t3)                                 \
    { n1, t1 }, { n2, t2 }, { n3, t3 },
#define SCN_FN_ARGS4(n1, t1, n2, t2, n3, t3, n4, t4)                         \
    { n1, t1 }, { n2, t2 }, { n3, t3 }, { n4, t4 },
#define SCN_FN_ARGS5(n1, t1, n2, t2, n3, t3, n4, t4, n5, t5)                 \
    { n1, t1 }, { n2, t2 }, { n3, t3 }, { n4, t4 }, { n5, t5 },

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
      SCN_FN_ARGS1("tick", SCN_PARAM_NUMBER))                                \
    X(END,              "on_end",              SCN_FN_LIFECYCLE_HOOK,        \
      SCN_FN_ARGS0())                                                        \
    X(LOBBY,            "on_lobby",            SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS1("p", SCN_PARAM_SLOT))                                     \
    X(PLAYER_JOIN,      "on_player_join",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS1("p", SCN_PARAM_SLOT))                                     \
    X(PLAYER_LEAVE,     "on_player_leave",     SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS1("p", SCN_PARAM_SLOT))                                     \
    X(TEAM_CHANGED,     "on_team_changed",     SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("p", SCN_PARAM_SLOT, "team", SCN_PARAM_TEAM))             \
    X(CHAT,             "on_chat",             SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("p", SCN_PARAM_SLOT, "text", SCN_PARAM_STRING))           \
    X(PING,             "on_ping",             SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", SCN_PARAM_SLOT, "kind", SCN_PARAM_NUMBER,            \
                   "mx", SCN_PARAM_SQUARE_X, "my", SCN_PARAM_SQUARE_Y))      \
    X(TANK_SPAWNED,     "on_tank_spawned",     SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", SCN_PARAM_SLOT, "mx", SCN_PARAM_SQUARE_X,            \
                   "my", SCN_PARAM_SQUARE_Y, "respawn", SCN_PARAM_BOOL))     \
    X(TANK_KILLED,      "on_tank_killed",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("victim", SCN_PARAM_SLOT, "killer", SCN_PARAM_OWNER,      \
                   "cause", SCN_PARAM_WORD))                                 \
    X(LGM_DIED,         "on_lgm_died",         SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", SCN_PARAM_SLOT, "killer", SCN_PARAM_OWNER,           \
                   "mx", SCN_PARAM_SQUARE_X, "my", SCN_PARAM_SQUARE_Y))      \
    X(LGM_LANDED,       "on_lgm_landed",       SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("p", SCN_PARAM_SLOT, "mx", SCN_PARAM_SQUARE_X,            \
                   "my", SCN_PARAM_SQUARE_Y))                                \
    X(BASE_CAPTURED,    "on_base_captured",    SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("n", SCN_PARAM_BASE, "old", SCN_PARAM_OWNER,              \
                   "new", SCN_PARAM_OWNER))                                  \
    X(BASE_NEUTRALIZED, "on_base_neutralized", SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("n", SCN_PARAM_BASE, "old", SCN_PARAM_OWNER))             \
    X(PILL_CAPTURED,    "on_pill_captured",    SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("n", SCN_PARAM_PILL, "old", SCN_PARAM_OWNER,              \
                   "new", SCN_PARAM_OWNER))                                  \
    X(PILL_PLACED,      "on_pill_placed",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("n", SCN_PARAM_PILL, "p", SCN_PARAM_SLOT,                 \
                   "armour", SCN_PARAM_NUMBER))                              \
    X(PILL_PICKED_UP,   "on_pill_picked_up",   SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("n", SCN_PARAM_PILL, "p", SCN_PARAM_SLOT))                \
    X(PILL_KILLED,      "on_pill_killed",      SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS2("n", SCN_PARAM_PILL, "by", SCN_PARAM_OWNER))              \
    X(BUILT,            "on_built",            SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS4("p", SCN_PARAM_SLOT, "action", SCN_PARAM_WORD,            \
                   "x", SCN_PARAM_SQUARE_X, "y", SCN_PARAM_SQUARE_Y))        \
    X(MINE_LAID,        "on_mine_laid",        SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("p", SCN_PARAM_SLOT, "mx", SCN_PARAM_SQUARE_X,            \
                   "my", SCN_PARAM_SQUARE_Y))                                \
    X(MINE_EXPLOSION,   "on_mine_explosion",   SCN_FN_EVENT_HOOK,            \
      SCN_FN_ARGS3("mx", SCN_PARAM_SQUARE_X, "my", SCN_PARAM_SQUARE_Y,       \
                   "layer", SCN_PARAM_OWNER))                                \
    X(ENTER_REGION,     "on_enter_region",     SCN_FN_REGION_HOOK,           \
      SCN_FN_ARGS2("p", SCN_PARAM_SLOT, "name", SCN_PARAM_REGION))           \
    X(LEAVE_REGION,     "on_leave_region",     SCN_FN_REGION_HOOK,           \
      SCN_FN_ARGS2("p", SCN_PARAM_SLOT, "name", SCN_PARAM_REGION))

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
    X(CAN_RESPAWN,       "can_respawn",                                      \
      SCN_FN_ARGS1("p", SCN_PARAM_SLOT),                                     \
      "false to hold the tank where it is")                                  \
    X(CAN_BUILD,         "can_build",                                        \
      SCN_FN_ARGS5("p", SCN_PARAM_SLOT, "action", SCN_PARAM_WORD,            \
                   "x", SCN_PARAM_SQUARE_X, "y", SCN_PARAM_SQUARE_Y,         \
                   "n", SCN_PARAM_ITEM),                                     \
      "false to refuse the order")                                           \
    X(CAN_CAPTURE,       "can_capture",                                      \
      SCN_FN_ARGS3("kind", SCN_PARAM_WORD, "n", SCN_PARAM_ITEM,              \
                   "p", SCN_PARAM_SLOT),                                     \
      "false to leave the objective where it is")                            \
    X(ANNOUNCE,          "announce",                                         \
      SCN_FN_ARGS3("kind", SCN_PARAM_WORD, "subject", SCN_PARAM_ITEM,        \
                   "actor", SCN_PARAM_OWNER),                                \
      "false to keep the line off every newswire")                           \
    X(CAN_DIE,           "can_die",                                          \
      SCN_FN_ARGS4("kind", SCN_PARAM_WORD, "n", SCN_PARAM_ITEM,              \
                   "killer", SCN_PARAM_OWNER, "cause", SCN_PARAM_WORD),      \
      "false to leave what the blow landed on standing")                     \
    X(ON_CHOOSE_START,   "on_choose_start",                                  \
      SCN_FN_ARGS1("p", SCN_PARAM_SLOT),                                     \
      "a start number, counted from 1 as game.start counts")                 \
    X(SPAWN_LOADOUT,     "spawn_loadout",                                    \
      SCN_FN_ARGS1("p", SCN_PARAM_SLOT),                                     \
      "a loadout word, or a table of shells, mines, armour and trees")       \
    X(DAMAGE_SCALE,      "damage_scale",                                     \
      SCN_FN_ARGS3("attacker", SCN_PARAM_OWNER, "victim", SCN_PARAM_SLOT,    \
                   "cause", SCN_PARAM_WORD),                                 \
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
 *NAME:          scenarioLuaOpIsScalar
 *PURPOSE:
 *  Whether every argument this op takes is a flat scalar,
 *  so a trigger's action list can express a call to it. An
 *  op taking a table or a function reaches an author
 *  through a scenario's own Lua instead.
 *
 *  Read off the argument types, so an op that grows a table
 *  argument drops out of the list without anything being
 *  told about it.
 *********************************************************/
bool scenarioLuaOpIsScalar(const ScnLuaRow *row);

/* Whether a trigger's action list has any use for this op: it is expressible
 * as flat arguments and it changes something. A read accessor is neither
 * refused nor useful — it runs and answers a value nobody is there to
 * read. */
bool scenarioLuaOpIsAction(const ScnLuaRow *row);

/* Whether this op decides the round: which side wins it, or when it is over.
 * A file whose manifest says kind = "mod" leaves the win condition alone, so
 * an op this answers true for raises when a mod calls it.
 *
 * The list behind it is in scenario_lua.c, in one place because three
 * readers want the same answer: the op itself raises, the host refuses to
 * start a round for a mod whose declared trigger action names one, and the
 * map editor leaves them off the action list it offers while the form says
 * mod. Taking a name rather than a row is what lets the last two ask, since
 * a trigger's action states the op it calls as text.
 *
 * The name is the op's own, as the registry spells it — "end_round", not
 * "game.end_round". NULL answers false. */
bool scenarioLuaOpDecidesRound(const char *name);

/* The same list to walk rather than to ask of: the index'th name, and NULL
 * once there are no more, so a caller building a sentence or a menu needs no
 * count of its own. */
const char *scenarioLuaRoundDeciderAt(size_t index);

/* Whether the round reads this function's answer to decide an ending, so a
 * file whose manifest says kind = "mod" is not offered it and is told when it
 * writes one anyway. One function answers true — allow_base_win, the only
 * policy whose answer takes an ending out of a round — and every hook answers
 * false, a hook returning nothing the host reads.
 *
 * The name is the one an author writes the function under, as SCN_POLICY_LIST
 * spells it. NULL answers false.
 *
 * Both readers are the map editor's: the catalogue of functions leaves this
 * row out while the form says mod, and the editor's check flags a mod that
 * defines it. The host makes the same decision at the one call site instead,
 * where it knows which policy it is asking; see scnAllowBaseWin. */
bool scenarioLuaFnDecidesRound(const char *name);

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

/* Room for a field name and its terminator. The longest a derived name
 * reaches is a parameter's own plus _team, and no parameter in the
 * catalogue is anywhere near long enough to need this much. */
#define SCN_FN_FIELD_NAME_LEN 32

/* One name a trigger's where-row may test on a given catalogue row: either
 * one of the function's own parameters or a field derived from one. The name
 * is copied rather than pointed at, because a derived name is built and has
 * nowhere of its own to live. */
typedef struct {
    char            name[SCN_FN_FIELD_NAME_LEN];
    ScnLuaParamType type;
    size_t          from;     /* the parameter index the field reads */
    bool            derived;  /* false for the parameter itself */
} ScnLuaFnField;

/*********************************************************
 *NAME:          scenarioLuaFnFields
 *PURPOSE:
 *  Every field a trigger may test on one catalogue row:
 *  the function's own parameters, in order, then the
 *  fields their types earn — a team for a seat or an
 *  owner, a tag for a pillbox or a base, a region for a
 *  square pair.
 *
 *  Answers how many the row has and writes at most outMax
 *  of them, so a caller sizing an array asks with an
 *  outMax of 0 and a NULL out first. row is an index into
 *  what scenarioLuaFunctions answers; a row past the end
 *  answers 0.
 *********************************************************/
size_t scenarioLuaFnFields(size_t row, ScnLuaFnField *out, size_t outMax);

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
 *
 *  asking is the script doing the looking, in the encoding
 *  SCN_OWNER_OF_ENTRY spells, and SCN_OWNER_NONE for a
 *  caller that is no script of the round's.
 *
 *  A composed round may hold two regions under one name,
 *  because two scripts on the list may each name one and
 *  neither is refused any more. The rule that picks between
 *  them: the asking script's own region of that name where
 *  it has one, and otherwise the last one on the list
 *  whoever owns it.
 *
 *  Both halves of that are what makes a list of scripts
 *  play together. A script that names its own "spawn"
 *  always gets its own rectangle and no mod can reach into
 *  it. A script that only reads "spawn" and never names one
 *  still finds the rectangle that exists, so a mod written
 *  to read a scenario's regions goes on working. The
 *  fallback takes the first rather than the last for the
 *  reason the rules do: the top of the list is the
 *  precedence end, and a host orders the list deliberately.
 *********************************************************/
const ScnManifestRegion *scenarioLuaRegionFind(const ScenarioManifest *m,
                                               const char *name,
                                               uint8_t asking);

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

/*********************************************************
 *NAME:          scenarioLuaRegionBit
 *PURPOSE:
 *  Which bit of the round's region mask a region belongs
 *  on, worked out from the file that named it and the name
 *  it was given. The one place a bit is handed out: the
 *  compose calls it for every region it appends and
 *  game.define_region calls it for every region a script
 *  names while the round runs.
 *
 *  path is where the naming script's bytes came from and
 *  name is the region's own. Only the last segment of the
 *  path is hashed, so a server that keeps its scenarios in
 *  one directory and a server that keeps them in another
 *  work out the same bit for the same script — which they
 *  must, because a headless test game is expected to play
 *  the same round on two machines. NULL for either is
 *  hashed as an empty string rather than refused.
 *
 *  The file has to be in the key. Two scripts on one list
 *  may each name a region "spawn" and the compose keeps
 *  both, so the name alone does not tell one rectangle from
 *  the other.
 *
 *  The hash is FNV-1a over those bytes, folded into the
 *  0..SCN_REGIONS_MAX-1 the mask has room for, and two keys
 *  that want one bit are separated by walking forward to
 *  the next bit no region of m is already on. m is read for
 *  that and for nothing else, so a caller passing the table
 *  it is about to append to gets a bit nothing in that
 *  table holds.
 *
 *  The walk is bounded by SCN_REGIONS_MAX and cannot spin:
 *  a table with every bit taken is a table already holding
 *  SCN_REGIONS_MAX regions, which both callers refuse
 *  before they ask. Reached anyway, it answers the bit the
 *  hash named and lets the caller's own limit be what says
 *  no.
 *********************************************************/
uint8_t scenarioLuaRegionBit(const ScenarioManifest *m, const char *path,
                             const char *name);

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
 *  owners takes the table of the script that set each one,
 *  in the same order, so the caller can say which script is
 *  running across the call it is about to make. It may be
 *  NULL where the caller does not need that, and an entry
 *  of it is NULL where the timer was set outside any
 *  script's own code.
 *
 *  Returns how many were taken. The caller owns each
 *  reference and must release it.
 *********************************************************/
int scenarioLuaTimersTakeDue(ScnTimerSet *t, uint32_t now, int *out,
                             const ScenarioManifest **owners, int outMax);

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
