/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Record JSON
 *Filename:      scenario_record_json.h
 *Author:        John Morrison
 *Purpose:
 *  The scripts.json text a round's recording carries
 *  (src/bolo/public/scripts_record.h), written from a plain
 *  description the scenario host fills at round boot. Here
 *  rather than in the host because this library is the one
 *  that holds cJSON; the host names no cJSON type and this
 *  header names none either.
 *
 *  The shape is docs/replay-format.md, "scripts.json":
 *
 *    { "version": 1, "map": ..., "mods_enabled": ...,
 *      "rules":   { name: value, ... },
 *      "regions": [ { name, x, y, w, h, file }, ... ],
 *      "scripts": [ { file, source, kind, manifest }, ... ] }
 *
 *  Every string the description points at is the caller's
 *  and is only read for the length of the call.
 *********************************************************/

#ifndef SCENARIO_RECORD_JSON_H
#define SCENARIO_RECORD_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scenario_manifest.h" /* SCN_MANIFEST_RULES_MAX, SCN_REGIONS_MAX */

/* The version the "version" key carries. */
#define SCN_RECORD_VERSION 1

/* How many script rows a description holds. Equal to SCN_SCRIPTS_MAX in
 * src/scenario/scenario_host.h, which this library cannot see; the host
 * holds the two against each other. */
#define SCN_RECORD_SCRIPTS_MAX 10

typedef struct {
    const char *name;   /* the rule's name, as simRulesRuleName gives it */
    double      value;
} ScnRecordRule;

typedef struct {
    const char *name;
    uint8_t     x, y;
    uint8_t     w, h;
    const char *file;   /* the file name of the script that named it */
} ScnRecordRegion;

typedef struct {
    const char *file;          /* the file name, not the path */
    const char *source;        /* "map" or "server" */
    const char *kind;          /* "scenario" or "mod" */
    const char *manifestJson;  /* the script's manifest as JSON text */
} ScnRecordScript;

typedef struct {
    const char      *map;
    bool             modsEnabled;
    int              numRules;
    ScnRecordRule    rules[SCN_MANIFEST_RULES_MAX];
    int              numRegions;
    ScnRecordRegion  regions[SCN_REGIONS_MAX];
    int              numScripts;
    ScnRecordScript  scripts[SCN_RECORD_SCRIPTS_MAX];
} ScnRecordDesc;

/* The description as unformatted JSON. Each manifestJson is parsed and put
 * in as an object, and a rule value that is a whole number is written as an
 * integer. The caller frees the returned string. NULL with err set when a
 * count is out of range, a manifest will not parse, or memory runs out. */
char *scnRecordJsonWrite(const ScnRecordDesc *d, char *err, size_t errLen);

#endif /* SCENARIO_RECORD_JSON_H */
