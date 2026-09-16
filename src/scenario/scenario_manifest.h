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
 *  This is the library's header and the tests', not a
 *  frontend's. scenario_static publishes its own directory to
 *  whatever links it, so this file is reachable rather than
 *  sealed off: the unit tests include it to check what the
 *  parse produced. A frontend has no business here and works
 *  through scenario_host.h and the accessors on it.
 *
 *  A rule is held as a plain uint16_t index rather than as
 *  ScnRuleIndex, so the struct needs nothing from
 *  src/bolo/scenario_api/ and this header stays readable by
 *  anything in the library. scenario_host.c, which sees both,
 *  is where an index becomes an op.
 *
 *  triggers is deliberately not here. Its schema is not
 *  settled, so the parser reads nothing from it and a script
 *  that carries one is not refused for having it.
 *********************************************************/

#ifndef SCENARIO_MANIFEST_H
#define SCENARIO_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#include "scenario_host.h"  /* the sizes below, and the entity counts */
#include "scenario_table.h" /* ScnTable — a team's init block below */

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
 *********************************************************/
const ScenarioManifest *scenarioHostManifest(const ScenarioHost *h);

#endif /* SCENARIO_MANIFEST_H */
