/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* Set the Workshop item and author on a parsed doc. Every other key,
   known or not, is written back as it was. */
void scnManifestSetWorkshop(ScnManifestDoc *d, uint64_t id, uint64_t author);

/* A Workshop item id or a SteamID64 as the manifest and a script's table
   both write it: a non-empty string of ASCII digits that fits 64 bits. True
   with the value in *out; false with *out 0 for NULL and for anything else.
   Both readers take workshop_id and workshop_author through here, so a
   value one of them refuses the other refuses too. */
bool scnManifestParseId(const char *s, uint64_t *out);

/* What a reader found under author or updated: nothing, a string, or a
   value of some other type. */
typedef enum {
    scnIdentityAbsent,
    scnIdentityString,
    scnIdentityNotString
} ScnIdentitySeen;

/* author and updated into m, from what a reader found under the two keys.
   Both readers come through here, so a file reads the same in either form
   and warns the same way. The author is cleaned (scnIdentityCleanAuthor);
   an updated not in the one form is read as "". A key that is missing, of
   the wrong type, empty, cleaned or not in the form is a warning on
   rep->sink and nothing else: not an issue, and not a line on the console
   or in the soft buffer, because a script without them plays as it always
   did and a load has nothing to say about it. authorLen is the bytes at
   author, which may hold a NUL. */
void scnManifestTakeIdentity(ScenarioManifest *m,
                             ScnIdentitySeen authorSeen,
                             const char *author, size_t authorLen,
                             ScnIdentitySeen updatedSeen,
                             const char *updated,
                             ScnParseReport *rep);

/* Do a package's manifest and the table its script declared agree? True
   when they do. On a conflict, false with the disagreeing key written to
   key (for example "lobby.teams[0].brain" or "rules.tank_reload_ticks")
   and a sentence in err. */
bool scnManifestAgrees(const ScenarioManifest *fromJson,
                       const ScenarioManifest *fromLua,
                       char *key, size_t keyLen,
                       char *err, size_t errLen);

/* The seven operators a where-row tests with, as a name and back. A name
   this build does not know, and a NULL name, read as SCN_TRIG_CMP_UNKNOWN
   rather than being refused here: which operator suits which field is the
   catalogue's business and this library cannot see it. Keeping it apart
   from the seven is what leaves the check something to refuse. NULL for
   SCN_TRIG_CMP_UNKNOWN and for a value outside the enum.

   Both forms of a scenario spell these the same way, so the Lua reader in
   scenario_host.c resolves a script's "eq" through the same table the JSON
   decoder resolves the manifest's through. */
ScnTrigCompare scnManifestTrigOpFrom(const char *name);
const char    *scnManifestTrigOpName(ScnTrigCompare op);

/* The two words scenario.kind takes, as a name and back, and here for the
   same reason the operators are: a script writes the word in Lua and a
   package writes it in JSON, and the two have to spell it the same way.

   scnManifestKindFrom answers scnKindUnknown for a word this build does not
   know and for NULL. Both readers report that word and store scnKindScenario
   instead, so the author is told and a file nobody could classify keeps the
   meaning every file had before the key existed.

   scnManifestKindName never answers NULL. The operator table above keeps ""
   for a word it cannot name, because a row that names no operator has to come
   back as one; a kind has no third spelling to come back as, so a value from
   outside the enum is written as the word an absent key reads as, and a
   manifest that went out and came back says what it said. */
ScnManifestKind scnManifestKindFrom(const char *name);
const char     *scnManifestKindName(ScnManifestKind kind);

#endif /* SCENARIO_MANIFEST_JSON_H */
