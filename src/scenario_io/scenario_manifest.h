/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Manifest
 *Filename:      scenario_manifest.h
 *Author:        John Morrison
 *Purpose:
 *  The scenario table a script declares, as one POD struct.
 *  Lua is read into this at attach and again at each round
 *  start, both times from the bytes the host cached; every-
 *  thing past the read works on the struct and never on the
 *  VM, which is what keeps the marshalling in one place.
 *
 *  scenario_io_static publishes its own directory to whatever
 *  links it, and scenario_static links it publicly, so this
 *  file is reachable rather than sealed off: the unit tests
 *  include it to check what the parse produced, and a reader
 *  or writer of a container has the shape without the
 *  runtime. A frontend that wants a round works through
 *  scenario_host.h and the accessors on it.
 *
 *  A rule is held as a plain uint16_t index rather than as
 *  ScnRuleIndex, so the struct needs nothing from
 *  src/bolo/scenario_api/, which this file cannot see.
 *  scenario_host.c, which sees both, is where an index
 *  becomes an op.
 *
 *  triggers are carried here and both readers fill them:
 *  scnReadManifest out of a script's own table and the JSON
 *  decoder out of manifest.json. A package stating them in
 *  both forms is held to stating the same thing in each.
 *********************************************************/

#ifndef SCENARIO_MANIFEST_H
#define SCENARIO_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

/* The four counts the arrays below are sized by, taken from the headers that
 * define them rather than through server_sim.h. That header declares a bot
 * config whose gameType member is named after its own type, which a C++
 * translation unit refuses to compile, and the map editor's forms read this
 * manifest from C++. Nothing here names a ServerSim type. */
#include "global.h"         /* MAX_TANKS */
#include "types.h"          /* MAX_PILLS / MAX_BASES / MAX_STARTS */
#include "scenario_table.h" /* ScnTable — a team's init block below */
#include "scenario_callbacks.h" /* SCN_CALLBACK_NAME_LEN / _TEXT_LEN */

/* Tags and regions, at the sizes the scenario table is specified with. A
 * tag names a pill, base or start; a region names a rectangle of map
 * squares. Both are read into the manifest here and mean nothing to the
 * engine yet.
 *
 * SCN_TIMERS_MAX in scenario_host.h bounds the regions a round holds and
 * is stated against SCN_REGIONS_MAX there. */
#define SCN_TAG_LEN          32
#define SCN_TAGS_PER_ENTITY   4
#define SCN_REGIONS_MAX      64
#define SCN_REGION_NAME_LEN  32

/* The text fields of the scenario table. The name is what a lobby row
 * shows and the description what a tooltip or an info line shows, so they
 * are sized for a line rather than for prose.
 *
 * SCN_DIR_NAME_LEN and SCN_DIR_DESC_LEN in scenario_api/scenario_defs.h are
 * the same two lengths on the sim's side of the fence, where a scenario the
 * server offers is described. They are stated twice because this header sees
 * public/ alone and that one is not on its include path. scenario_dir.c sees
 * both and holds them against each other. */
#define SCN_SCENARIO_NAME_LEN 64
#define SCN_SCENARIO_DESC_LEN 256

/* scenario.game names a game type ("open", "tournament", "strict"), kept
 * as the text the file gave. */
#define SCN_GAME_NAME_LEN 24

/* A brain named by a lobby team: the directory under the server's own
 * brains/, as the file wrote it. The same length the spawn op carries a brain
 * in, because that op carries the path this name resolves to; scenario_host.c
 * holds the two against each other where it can see both. */
#define SCN_BRAIN_LEN 256

/* A bot mode key or a level key, as a team template or a bot op names it.
 * These are matched against the keys in the brain's own modes.txt, so the
 * room is that file's: BRAIN_MODE_KEY_LEN in public/brain_list.h. Written
 * out rather than included, because this header sees public/ alone and that
 * one is not on its include path; SCN_BOT_KEY_MAX in
 * scenario_api/scenario_defs.h holds the same number for the ops, and
 * server_sim_lobby.c is where all three meet. */
#define SCN_BOT_KEY_LEN 16

/* Room for every rule the table can name. scenario_host.c checks this
 * covers the rule list, so a rule added to the list cannot overflow it.
 * Held well clear of the list's own length rather than trimmed to it: the
 * check is a static assertion, so a run of rules that outgrew this number
 * would stop the build rather than fail anything at runtime, and the array
 * it sizes is sixteen bytes a row. */
#define SCN_MANIFEST_RULES_MAX 256

/* One rule the table sets: which rule, and what it was set to. The value
 * is a double because sixteen of the rules are float-valued and the rest
 * are integers a double carries exactly. */
typedef struct {
    uint16_t rule;  /* a ScnRuleIndex, resolved from the Lua key at parse */
    double   value;
} ScnManifestRule;

/* One team the lobby template seats. bots is how many the template asks
 * for, max_bots the ceiling a host may raise it to, fielded whether those
 * seats start with a tank or sit as roster entries.
 *
 * mode and difficulty are the brain mode this team's bots play in and the
 * level inside it, by the keys the brain's own modes.txt lists. "" for
 * either leaves that one as the lobby would have had it. A key the brain
 * does not list is refused: the validator names it, and the seating leaves
 * the seat's config alone and says so in the server log.
 *
 * init is the table the team's bots are built with, read once when a VM is
 * built, empty for none. initBadKey names the pair the read of it stopped on
 * and is "" when the whole table was taken: the pairs read before a bad one
 * stay in init, the way scnReadRules keeps the rest of a rules table past a
 * key that names no rule, and the validator is what reports the key. */
typedef struct {
    uint8_t  id;                     /* team number */
    uint8_t  bots;
    uint8_t  maxBots;
    bool     fielded;
    char     brain[SCN_BRAIN_LEN];   /* the brain's name, as the file wrote
                                      * it; "" = the server's own */
    char     mode[SCN_BOT_KEY_LEN];        /* "" = leave the lobby's */
    char     difficulty[SCN_BOT_KEY_LEN];  /* "" = leave the lobby's */
    ScnTable init;
    char     initBadKey[SCN_TABLE_KEY_LEN];
} ScnManifestTeam;

typedef struct {
    uint8_t         maxPlayers;     /* 0 = the server's own cap */
    bool            extraTeams;     /* may a host add teams beyond these */
    uint8_t         numTeams;
    ScnManifestTeam teams[MAX_TANKS];
} ScnManifestLobby;

/* The tags on one entity: up to SCN_TAGS_PER_ENTITY of them, each a short
 * name. Held per kind and per 1-based entity index, which is how Lua
 * writes them, so index 1 in the file is index 1 here. */
typedef struct {
    uint8_t count;
    char    tag[SCN_TAGS_PER_ENTITY][SCN_TAG_LEN];
} ScnManifestTags;

/* Which script of a composed list named a thing, written as that script's
 * position on the list plus one. Zero is nobody.
 *
 * Plus one rather than the bare index, so that the value a memset leaves
 * means nobody rather than the first script. A manifest that was never
 * composed — one the JSON reader filled, one the map editor's form holds,
 * one entry's own table straight out of its file — carries no owner at all,
 * and none of its regions can be read as the first script's.
 *
 * The same encoding is what a caller says it is with, so an owner and an
 * asker compare directly. SCN_SCRIPTS_MAX in scenario_host.h bounds the
 * list, and this header does not see it: the values here run 1 to however
 * many scripts that number allows. */
#define SCN_OWNER_NONE 0
#define SCN_OWNER_OF_ENTRY(i) ((uint8_t)((i) + 1))

/* A named rectangle of map squares: an inclusive top-left and a size.
 *
 * owner is which script of the composed list named it. Two scripts may now
 * both name "spawn" — the composite holds both rectangles and this is what
 * tells them apart — so a lookup by name is answered with the asking
 * script's own where it has one. See scenarioLuaRegionFind in
 * src/scenario/scenario_lua.h for the whole rule.
 *
 * bit is which bit of the round's region mask this rectangle answers to, and
 * it is the region's identity for as long as the round lasts. The host
 * remembers who is standing inside what as one uint64_t a seat, and it is
 * this number that indexes those bits rather than the region's place in the
 * array below. The two used to be the same number, which is what tied a
 * region's identity to where its script sat on the host's list and stopped
 * the map's own script from being anywhere but the front of it. It is worked
 * out from the file that named the region and the region's own name, so
 * reordering the list moves the rectangles about in the array and leaves
 * every bit meaning what it meant. scenarioLuaRegionBit in
 * src/scenario/scenario_lua.h is where the number comes from, and says how
 * two keys that want one bit are told apart.
 *
 * It is a number in 0..SCN_REGIONS_MAX-1 rather than the mask value, so that
 * a reader shifting by it is doing the same thing everywhere it is read.
 *
 * Both are runtime only. scnComposeInto writes them as it builds a round's
 * table and game.define_region writes them for a region a script names
 * while the round runs; nothing else does. manifest.json neither reads nor
 * writes either one, so a package is the same bytes on disk and on the wire
 * whether or not the server that wrote it knew about owners or bits — and a
 * manifest that was never composed carries a bit of zero for every region,
 * which means nothing and is never shifted by. */
typedef struct {
    char    name[SCN_REGION_NAME_LEN];
    uint8_t x, y;
    uint8_t w, h;
    uint8_t owner;
    uint8_t bit;
} ScnManifestRegion;

/* ── Triggers ──────────────────────────────────────────────────────
 *
 * One trigger is a hook to listen on, a list of tests against that hook's
 * payload, and a list of actions to run when every test holds.
 *
 * Names are held as text rather than as resolved indices. The catalogue that
 * would resolve them is src/scenario/scenario_lua.h, which this library is
 * not built to see: scenario_io_static uses the runtime_only include profile
 * and scenario_static links this one rather than the other way round. rules[]
 * above holds a resolved index only because simRulesRuleIndex is in public/.
 *
 * Nothing here checks that a name exists, that an operator suits the field it
 * tests, or that an action was given the arguments its op takes. This is the
 * shape a file states, not a statement that the file is playable. */

/* How many triggers one scenario may declare, and how many tests and actions
 * one of them carries. Sized against what the whole struct costs, which at
 * this cap is some 180 KB: the host, the editor's form and a validate result
 * each hold one by value, and every one of those lives on the heap. A frame
 * that needs a manifest of its own allocates it rather than declaring it. */
#define SCN_TRIGGERS_MAX         64
#define SCN_TRIGGER_CONDS_MAX     4
#define SCN_TRIGGER_ACTIONS_MAX   4

/* add_base(x, y, owner, armour, shells, mines) is the widest op the
 * vocabulary has. */
#define SCN_TRIGGER_ARGS_MAX      6

/* A hook name, an op name, a field name, or a value's own string — a tag, a
 * region, or the name of a payload field a value refers to. Every one of
 * those is a short name and 32 covers the longest the catalogue holds with
 * room over. */
#define SCN_TRIGGER_NAME_LEN     32

/* The one long string an action may carry: the line announce, message, log
 * and end_round take. This restates SCN_TEXT_MAX from
 * src/bolo/scenario_api/scenario_defs.h, which this header cannot see —
 * the same reason SCN_SCENARIO_NAME_LEN restates SCN_DIR_NAME_LEN above. */
#define SCN_TRIGGER_TEXT_LEN    129

typedef enum {
    SCN_TRIG_VAL_NONE,
    SCN_TRIG_VAL_NUMBER,
    SCN_TRIG_VAL_STRING,
    SCN_TRIG_VAL_BOOL,
    SCN_TRIG_VAL_FIELD   /* {"field": "<name>"}: read off the hook's payload
                            when the trigger fires, not at authoring time */
} ScnTrigValueKind;

/* One value: a literal, or a reference to a payload field. num carries a
 * NUMBER, and a BOOL as 0 or 1. text carries a STRING and a FIELD's name.
 *
 * A STRING too long for text lives on the action's own text instead, and
 * inText is what says so: text is then "" and means nothing. Only an action's
 * argument can be in that state, because only an action has a text to put it
 * on — a condition's value is never inText. Reading text for a string value
 * without checking inText gets "" for the longest strings a file can state,
 * which is why the flag is its own field rather than something inferred from
 * an empty text. */
typedef struct {
    ScnTrigValueKind kind;
    bool             inText;  /* STRING only: the bytes are on the action's
                                 text, this slot being too small to hold
                                 them */
    double           num;
    char             text[SCN_TRIGGER_NAME_LEN];
} ScnTrigValue;

typedef enum {
    SCN_TRIG_CMP_UNKNOWN,   /* a word that is none of the seven, and what a
                               row that names no operator is left as: a test
                               nothing can evaluate, refused before a round
                               starts and never holding if one is reached.
                               First, so a condition nobody filled in reads
                               as this rather than as eq. */
    SCN_TRIG_CMP_EQ, SCN_TRIG_CMP_NE, SCN_TRIG_CMP_LT,
    SCN_TRIG_CMP_LTE, SCN_TRIG_CMP_GT, SCN_TRIG_CMP_GTE, SCN_TRIG_CMP_IN
} ScnTrigCompare;

/* One test: a field of the hook's payload against a value. */
typedef struct {
    char           field[SCN_TRIGGER_NAME_LEN];
    ScnTrigCompare op;
    ScnTrigValue   value;
} ScnTrigCond;

/* One action: an op and its positional arguments. text is the one long
 * string an op may take and is "" for an op that takes none. */
typedef struct {
    char         op[SCN_TRIGGER_NAME_LEN];
    uint8_t      numArgs;
    ScnTrigValue args[SCN_TRIGGER_ARGS_MAX];
    char         text[SCN_TRIGGER_TEXT_LEN];
} ScnTrigAct;

typedef struct {
    char        when[SCN_TRIGGER_NAME_LEN];
    uint8_t     numWhere;
    uint8_t     numActions;
    ScnTrigCond where[SCN_TRIGGER_CONDS_MAX];
    ScnTrigAct  actions[SCN_TRIGGER_ACTIONS_MAX];
} ScnTrigger;

/* ── What each callback does ───────────────────────────────────────
 *
 * scenario.callbacks maps an engine callback the script uses to a sentence
 * saying what it does in this script, for the lobby's details dialog:
 *
 *   callbacks = {
 *     on_start       = "Sets base recharge to 250 ticks per player.",
 *     on_player_join = "Works the base recharge out again.",
 *   }
 *
 * A rules table says what a file sets; this says what it does while the
 * round runs, which a table cannot — a mod that writes a rule from
 * on_player_join depending on how many are playing has an empty rules block.
 *
 * Optional. A file without it loads as it always did. The Lua read checks
 * the names against the function catalogue in src/scenario/scenario_lua.h
 * and against what the chunk defined, and warns about the three ways they
 * can disagree; the names that pass are kept in the catalogue's order, so
 * the dialog lists them the same way whatever order the file wrote them in.
 * manifest.json carries the block too, as an object of the same pairs, and
 * that reader, which cannot see the catalogue, keeps them as written.
 *
 * SCN_CALLBACKS_MAX holds every function the catalogue has, which
 * scenario_host.c asserts, so the count never truncates: a name is kept at
 * most once and a name the catalogue lacks is not kept at all. What bounds a
 * block is its packed size, SCN_CALLBACKS_BLOB_MAX in scenario_callbacks.h,
 * because that is what has to fit the lobby's wire. */
#define SCN_CALLBACKS_MAX 40

typedef struct {
    char name[SCN_CALLBACK_NAME_LEN];   /* the catalogue's, e.g. "on_start" */
    char text[SCN_CALLBACK_TEXT_LEN];   /* the author's sentence */
    /* No function by this name; a declared trigger's `when` is what defines
       it. The details dialog's Type column says "Trigger" for it. The Lua
       read knows; manifest.json does not say, so its reader sets this for a
       name any trigger it declares hooks, function or no. */
    bool byTrigger;
} ScnManifestCallback;

/* ── What kind of file this is ─────────────────────────────────────
 *
 * scenario.kind says whether the file may decide the win condition. The
 * author states it; nothing here works it out. A static read of a script
 * cannot tell the two apart, because game.end_round is an ordinary call a
 * script makes from whatever hook it likes: Survival ends its round from
 * on_tick, so a scan of the triggers it declares would read it as the kind
 * that never ends one.
 *
 * The words are the author's, and the identifiers are deliberately not:
 * lobbyScenarioMod in src/bolo/public/control_event.h already means
 * something else — where a script was loaded from — and the two would be
 * read for each other on sight. Everything in C is named for the thing this
 * field actually decides, which is whether the round is the script's to
 * end.
 *
 * scnKindScenario is zero so a manifest nobody filled in is a scenario,
 * which is what every file written before this key existed is. That is the
 * other way round from ScnTrigCompare above, whose unknown is first for the
 * opposite reason: an unfilled comparison must not read as eq, while an
 * unfilled kind must read as the kind that keeps today's meaning.
 *
 * scnKindUnknown is what a lookup answers for a word that is neither, and
 * is never stored: both readers report the word and keep the scenario kind
 * rather than leaving a manifest in a state nothing can act on. */
typedef enum {
    scnKindScenario = 0,      /* kind = "scenario": the round is its to end */
    scnKindKeepsWinCondition, /* kind = "mod": it changes how the game plays
                               * and leaves winning and losing alone */
    scnKindUnknown            /* a word that is neither; a lookup's answer
                               * and never a manifest's value */
} ScnManifestKind;

typedef struct {
    char name[SCN_SCENARIO_NAME_LEN];
    char description[SCN_SCENARIO_DESC_LEN];
    ScnManifestKind kind;           /* what the file is allowed to decide */
    int  api;                       /* the version the file was written to */
    char game[SCN_GAME_NAME_LEN];   /* the game type it asks for; "" = none */

    /* True when the file was written for one map and false when it plays
     * over any of them. Read together with kind above and the two are easy
     * to confuse, so: this one is about which map, and kind is about the win
     * condition. A file can be either of these and either of those. */
    bool bound;

    /* Start the map's pills and bases at the caps this table sets rather
     * than at the numbers the map file holds. A map file states a number
     * and cannot state "full", so a scenario that raises a cap would
     * otherwise leave every base short of it and every pill under it. Off
     * unless the file asks for it, which leaves a map's own numbers alone. */
    bool fillToCaps;

    ScnManifestLobby lobby;

    uint16_t        numRules;
    ScnManifestRule rules[SCN_MANIFEST_RULES_MAX];

    /* Tag arrays are 1-based to match the file: entry 0 is unused and the
     * highest entry is the highest entity index the map can hold. */
    ScnManifestTags pillTags[MAX_PILLS + 1];
    ScnManifestTags baseTags[MAX_BASES + 1];
    ScnManifestTags startTags[MAX_STARTS + 1];

    uint8_t           numRegions;
    ScnManifestRegion regions[SCN_REGIONS_MAX];

    uint8_t    numTriggers;
    ScnTrigger triggers[SCN_TRIGGERS_MAX];

    /* What each callback does, in the catalogue's order. See above. */
    uint8_t             numCallbacks;
    ScnManifestCallback callbacks[SCN_CALLBACKS_MAX];
} ScenarioManifest;

/* Does this file leave the win condition alone? True only for one that
 * declared kind = "mod", which is the file every win-deciding thing is held
 * back from: the ops that end or time a round raise when it calls them, a
 * declared trigger that names one of those ops is refused before the round
 * starts, the game type its table asks for is not applied, and an
 * allow_base_win it answers is not read.
 *
 * Written the way round, rather than as "may end the round", so that a
 * manifest nobody filled in and a NULL one both answer false and nothing is
 * held back from them. A caller with no manifest is a caller with no script,
 * and a round with no script has always been the sim's own to end.
 *
 * Inline rather than a call, because this is read on the op path and the
 * struct is already in the caller's hands: a target holding a manifest can
 * ask without linking anything. */
static inline bool scnManifestKeepsWinCondition(const ScenarioManifest *m) {
    return m != NULL && m->kind == scnKindKeepsWinCondition;
}

/* Nothing here reaches up into the runtime. The table a host last read is
 * scenarioHostManifest, declared in src/scenario/scenario_validate.h with
 * the rest of what the host and the validator share: a target that links
 * this library alone has no host to ask. */

#endif /* SCENARIO_MANIFEST_H */
