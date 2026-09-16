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
 *  triggers is deliberately not here. Its schema is not
 *  settled, so the parser reads nothing from it and a script
 *  that carries one is not refused for having it.
 *********************************************************/

#ifndef SCENARIO_MANIFEST_H
#define SCENARIO_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#include "server_sim.h"     /* MAX_TANKS / MAX_PILLS / MAX_BASES / MAX_STARTS
                            * through global.h and types.h */
#include "scenario_table.h" /* ScnTable — a team's init block below */

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

/* Room for every rule the table can name. scenario_host.c checks this
 * covers the rule list, so a rule added to the list cannot overflow it. */
#define SCN_MANIFEST_RULES_MAX 128

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

/* A named rectangle of map squares: an inclusive top-left and a size. */
typedef struct {
    char    name[SCN_REGION_NAME_LEN];
    uint8_t x, y;
    uint8_t w, h;
} ScnManifestRegion;

typedef struct {
    char name[SCN_SCENARIO_NAME_LEN];
    char description[SCN_SCENARIO_DESC_LEN];
    int  api;                       /* the version the file was written to */
    char game[SCN_GAME_NAME_LEN];   /* the game type it asks for; "" = none */
    bool bound;                     /* true = tied to its map; false = a mod */

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
} ScenarioManifest;

/*********************************************************
 *NAME:          scenarioHostManifest
 *PURPOSE:
 *  The table the host last read, for the library's own code
 *  and for the tests that check the parse. NULL for a NULL
 *  host.
 *
 *  Defined in scenario_host.c, which is the library above
 *  this one. The host is named rather than included: nothing
 *  else here needs scenario_host.h, and this file is on the
 *  include path of targets that link no scenario runtime.
 *********************************************************/
struct ScenarioHost;

const ScenarioManifest *scenarioHostManifest(const struct ScenarioHost *h);

#endif /* SCENARIO_MANIFEST_H */
