/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_pack.h
 * Purpose:
 *   The scenario container as the editor sees it: the
 *   manifest and script already packed into a map, the chunk
 *   written back on to that map, and the standalone
 *   .scenario file a mod is.
 *
 *   Every call the editor makes into scenario_io is here,
 *   the way mapeditor_scenario_check.c holds its calls into
 *   the validator, so the rest of the editor names no
 *   container header. Nothing here starts a round, builds a
 *   ServerSim or reads a brain entry: the server's -pack
 *   writes none and neither does this.
 *
 *   The manifest the forms hold is never modified. Both
 *   writes take a const manifest and copy what they have to
 *   change.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_PACK_H
#define MAPEDITOR_SCENARIO_PACK_H

#include <stdbool.h>
#include <stddef.h>

#include "../scenario_io/scenario_manifest.h" /* ScenarioManifest */

/* Room for the sentence a refusal gives back. Long enough for a path and a
 * reason, which is what the container's own messages are. */
#define ME_SCENARIO_PACK_ERR_LEN 256

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
 *NAME:          meScenarioReadFromMap
 *PURPOSE:
 *  The scenario packed into a map file, if there is one:
 *  the manifest into *outManifest and the script into a
 *  buffer of its own.
 *
 *  A map with nothing on the end of it leaves *outFound
 *  false with no error — a plain map is the ordinary case
 *  and not a fault. True with *outFound true is a container
 *  that opened and a manifest that parsed; false is a
 *  container that is there and could not be read, with err
 *  saying why.
 *
 *  *outScript is malloc'd and the caller owns it. It is NULL
 *  for a package that carries no script, which a rules-only
 *  scenario is, and is always NUL-terminated where it is
 *  not. The entry read is whichever one the manifest names,
 *  and main.lua when it names none.
 *********************************************************/
bool meScenarioReadFromMap(const char *mapPath, ScenarioManifest *outManifest,
                           char **outScript, size_t *outScriptLen,
                           bool *outFound, char *err, size_t errLen);

/*********************************************************
 *NAME:          MEScenarioPacked
 *PURPOSE:
 *  The scenario a map already carried when it was opened,
 *  kept so a map save can put it back.
 *
 *  mapWrite opens the file with "wb" and writes the map, so
 *  it takes the container off the end with it. Saving terrain
 *  on a packed map would otherwise drop the scenario from the
 *  file without saying so. The pane cannot stand in for this:
 *  where a loose script exists the pane holds that one, and
 *  the packed bytes are nowhere else in memory.
 *
 *  These are the bytes that were on the file, so they are
 *  written back as they are. What the forms hold may have
 *  been edited and has not been checked; Pack into Map is
 *  what writes those, with the checks that go with them.
 *********************************************************/
typedef struct MEScenarioPacked {
    ScenarioManifest manifest;
    char            *script;    /* NUL-terminated; NULL when there was none */
    size_t           scriptLen;
    bool             present;   /* the map carried a container when it opened */
} MEScenarioPacked;

/* An empty store: no manifest, no script, nothing to put back. */
void meScenarioPackedInit(MEScenarioPacked *p);

/* Releases the script and returns the store to empty. Called wherever the
 * rest of the editor's scenario state is cleared, so a save can never write
 * one map's scenario on to another. */
void meScenarioPackedClear(MEScenarioPacked *p);

/* Takes a copy of the manifest and the script for putting back later. False
 * when the copy could not be made, leaving the store empty rather than half
 * filled. A NULL script is a package that carries none. */
bool meScenarioPackedSet(MEScenarioPacked *p, const ScenarioManifest *m,
                         const char *script, size_t scriptLen);

/* Writes the kept chunk back on to mapPath. True and nothing written when the
 * store is empty, which is every map that opened without a container. False
 * with err set when there was one and it did not go back. */
bool meScenarioPackedRestore(const MEScenarioPacked *p, const char *mapPath,
                             char *err, size_t errLen);

/*********************************************************
 *NAME:          meScenarioPackIntoMap
 *PURPOSE:
 *  Writes the manifest and the script on to the map file as
 *  a WBSC container, replacing whatever container was on it.
 *  The map is left as it was on any failure.
 *
 *  A manifest with bound false is refused: a chunk on a map
 *  is that map's scenario by definition, and a scenario that
 *  plays over any map is saved as a .scenario file instead.
 *********************************************************/
bool meScenarioPackIntoMap(const ScenarioManifest *m, const char *script,
                           size_t scriptLen, const char *mapPath,
                           char *err, size_t errLen);

/*********************************************************
 *NAME:          meScenarioWriteMod
 *PURPOSE:
 *  Writes the same container as a file of its own, which is
 *  what a mod is: the manifest and the script, stored rather
 *  than deflated because both are small text.
 *
 *  The manifest written is meScenarioModManifest's copy —
 *  bound false, no tags, no regions — whatever the form
 *  holds, because those are properties of a map a mod has
 *  never seen.
 *********************************************************/
bool meScenarioWriteMod(const ScenarioManifest *m, const char *script,
                        size_t scriptLen, const char *modPath,
                        char *err, size_t errLen);

/*********************************************************
 *NAME:          meScenarioModManifest
 *PURPOSE:
 *  The manifest a mod is written from: a copy of the form's
 *  with bound false and the tags and regions cleared. The
 *  validator refuses a bound: false package that fills
 *  either, and a script is compared against this copy rather
 *  than against the form, so a table that says bound = true
 *  is caught before anything is written.
 *********************************************************/
void meScenarioModManifest(const ScenarioManifest *in, ScenarioManifest *out);

/*********************************************************
 *NAME:          meScenarioManifestAgrees
 *PURPOSE:
 *  Does the manifest about to be written agree with the
 *  table the script declared? The host fills the scenario
 *  global from the manifest, runs the chunk and refuses the
 *  load on a conflict, so a package written without this
 *  check is one the server turns down when a round starts on
 *  it.
 *
 *  False writes the disagreeing key into key and a sentence
 *  into err. A script that declares no table has nothing to
 *  compare and is not asked about here.
 *********************************************************/
bool meScenarioManifestAgrees(const ScenarioManifest *fromForm,
                              const ScenarioManifest *fromScript,
                              char *key, size_t keyLen,
                              char *err, size_t errLen);

/*********************************************************
 *NAME:          meScenarioModFileName
 *PURPOSE:
 *  The name a mod is offered under in the save dialog: the
 *  manifest's name with the .scenario extension, with
 *  anything a file name cannot carry replaced. A manifest
 *  with no name gives a stand-in rather than a bare
 *  extension. False when the result would not fit.
 *********************************************************/
bool meScenarioModFileName(const ScenarioManifest *m, char *out, size_t outLen);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_PACK_H */
