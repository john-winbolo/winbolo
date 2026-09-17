/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Manifest JSON
 *Filename:      scenario_manifest_json.h
 *Author:        John Morrison
 *Purpose:
 *  manifest.json, read into and written out of the same
 *  ScenarioManifest the Lua reader fills. The two producers
 *  of that struct are scnReadManifest, which reads a
 *  script's scenario table, and this file, which reads a
 *  package's manifest; they agree field for field and
 *  default for default.
 *
 *  A doc holds the parsed JSON as well as the decoded
 *  values, so a key this build does not know is still in
 *  the text the doc writes back out. That is what lets the
 *  schema grow without an older build throwing away what it
 *  could not read.
 *
 *  triggers is decoded like the rest, and unlike the rest it
 *  is written back from the struct rather than from the tree:
 *  a trigger goes out whole or not at all, so the array a
 *  file gets is the array the struct holds.
 *
 *  scnManifestAgrees is the check that keeps the two forms
 *  honest where a package carries both: the manifest is
 *  what the package is, the script's table is how its
 *  author wrote it down, and a disagreement names the key.
 *********************************************************/

#ifndef SCENARIO_MANIFEST_JSON_H
#define SCENARIO_MANIFEST_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scenario_manifest.h" /* ScenarioManifest */
#include "scenario_issues.h"   /* ScnParseReport */

/* The schema version this build writes and the only one it reads. The
 * "manifest" key, which moves independently of "api". */
#define SCN_MANIFEST_SCHEMA_VERSION 1

/* An entry name inside the container, as the manifest names it. */
#define SCN_MANIFEST_ENTRY_LEN 256

/* The entry the script is looked for in when the manifest does not say. */
#define SCN_MANIFEST_SCRIPT_DEFAULT "main.lua"

/* How many packaged brains one manifest may list. A name past this is
 * dropped and reported; it is not a refusal. */
#define SCN_MANIFEST_BRAINS_MAX 32

typedef struct ScnManifestDoc ScnManifestDoc;

/* Parse manifest.json. Soft problems go to rep; a refusal returns NULL
   with err set. */
ScnManifestDoc *scnManifestParse(const uint8_t *json, size_t len,
                                 ScnParseReport *rep,
                                 char *err, size_t errLen);

/* A doc built from the struct alone — no JSON behind it, so no unknown
   keys. This is what a script's scenario table becomes when it is packed. */
ScnManifestDoc *scnManifestFromValues(const ScenarioManifest *m,
                                      char *err, size_t errLen);

void scnManifestFree(ScnManifestDoc *d);

/* The decoded values. NULL for a NULL doc. */
const ScenarioManifest *scnManifestValues(const ScnManifestDoc *d);

/* The schema version the file declared — the "manifest" key, not "api". */
int scnManifestSchemaVersion(const ScnManifestDoc *d);

/* The entry holding the Lua, and the packaged brain names the manifest
   lists. The container is what actually holds them; these are what the
   manifest claims, so a later caller can hold the two against each other. */
const char *scnManifestScriptEntry(const ScnManifestDoc *d);
int         scnManifestBrainCount(const ScnManifestDoc *d);
const char *scnManifestBrainName(const ScnManifestDoc *d, int i);

/* The doc as JSON text, keys this build does not know included. The caller
   frees the returned string. */
char *scnManifestWrite(const ScnManifestDoc *d, char *err, size_t errLen);

/* Do a package's manifest and the table its script declared agree? True
   when they do. On a conflict, false with the disagreeing key written to
   key (for example "lobby.teams[0].brain" or "rules.tank_reload_ticks")
   and a sentence in err. */
bool scnManifestAgrees(const ScenarioManifest *fromJson,
                       const ScenarioManifest *fromLua,
                       char *key, size_t keyLen,
                       char *err, size_t errLen);

/* The seven operators a where-row tests with, as a name and back. A name
   this build does not know reads as SCN_TRIG_CMP_EQ rather than being
   refused: which operator suits which field is the catalogue's business and
   this library cannot see it. NULL for an op outside the enum.

   Both forms of a scenario spell these the same way, so the Lua reader in
   scenario_host.c resolves a script's "eq" through the same table the JSON
   decoder resolves the manifest's through. */
ScnTrigCompare scnManifestTrigOpFrom(const char *name);
const char    *scnManifestTrigOpName(ScnTrigCompare op);

#endif /* SCENARIO_MANIFEST_JSON_H */
