/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario.h
 * Purpose:
 *   The scenario script the editor edits: the buffer, the
 *   path it came from, and reading and writing the loose
 *   X.scenario.lua that sits beside X.map. Drawing is in
 *   mapeditor_scenario_imgui.cpp; this half owns the text.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_H
#define MAPEDITOR_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>

#ifndef ME_PATH_MAX
#define ME_PATH_MAX 1024
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   scriptPath[ME_PATH_MAX];  /* "" when the map has no file yet */
    char  *script;                   /* NUL-terminated; NULL means empty */
    size_t scriptLen;
    bool   dirty;                    /* buffer differs from what is on disk */
    bool   fileOnDisk;               /* a script file was found for this map */
    bool   readRefused;              /* a file is there and was not read, so
                                        writing over it would destroy it */
    bool   pushToWidget;             /* the view must re-seed itself */
    char   status[256];              /* last load/save result, shown in the pane */
} MEScenarioState;

/* An empty state with no script and no path. */
void meScenarioInit(MEScenarioState *st);

/* Releases the buffer and returns the state to empty. */
void meScenarioFree(MEScenarioState *st);

/* Where the script for a map lives: .../X.map is accompanied by
 * .../X.scenario.lua. False when mapPath is empty or the result would not
 * fit in outLen. */
bool meScenarioScriptPathForMap(const char *mapPath, char *out, size_t outLen);

/* Points the state at a map: derives the script path, reads the script if
 * one is there and leaves the buffer empty if it is not, clears dirty and
 * asks the view to re-seed. An empty mapPath clears the state. */
void meScenarioSetMap(MEScenarioState *st, const char *mapPath);

/* Writes the buffer to the script path for mapPath and adopts that path.
 * False with status set when the write fails, and false without writing
 * anything when the target is a script that is there but would not open:
 * the buffer is empty in that case, and writing it would destroy the file
 * the editor declined to show. Saving under a different name is a different
 * file and goes ahead. */
bool meScenarioSaveForMap(MEScenarioState *st, const char *mapPath);

/* Points the state at the script for mapPath without reading or writing
 * anything, keeping the buffer and its dirty flag. This is what a map saved
 * under a new name needs when the script has not been edited: the state
 * follows the map to its new name. */
void meScenarioAdoptPath(MEScenarioState *st, const char *mapPath);

/* Re-reads scriptPath, discarding edits, and asks the view to re-seed. */
bool meScenarioReload(MEScenarioState *st);

/* Replaces the buffer with len bytes of text and marks it dirty. */
void meScenarioSetText(MEScenarioState *st, const char *text, size_t len);

/* True when the buffer differs from what is on disk. */
bool meScenarioDirty(const MEScenarioState *st);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_H */
