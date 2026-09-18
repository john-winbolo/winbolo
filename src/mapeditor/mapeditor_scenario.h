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

/* Makes the state follow a map that has just been written to mapPath, with
 * the buffer and its dirty flag kept. This is what a save needs when the
 * script has not been edited, and what it does depends on where the buffer
 * came from:
 *
 *  - the script path for mapPath is the one the state already holds: nothing
 *    moves, and only "is there a file at this name" is asked again;
 *  - a different path, and the buffer is a loose script that was read
 *    (fileOnDisk, not readRefused): the buffer is written there the way
 *    meScenarioSaveForMap writes it, and that path is adopted as a file on
 *    disk, so the copy carries the script the original had. A write that
 *    fails leaves the status line saying so and the state pointed at the new
 *    path with no file behind it;
 *  - a different path, and the buffer stands for a file that would not open
 *    (readRefused), or came out of the map's own package and has no loose
 *    file behind it at all: nothing is written, since there is no loose
 *    script to carry and a packed one travels inside the map. The path is
 *    adopted and checked for a file of its own. */
void meScenarioAdoptPath(MEScenarioState *st, const char *mapPath);

/* Shows the script that came out of the map's own package. There is no loose
 * file behind it, so the buffer is not dirty and not on disk: Save writes the
 * loose script beside the map, which is the file a server reads in preference
 * to the packed one. The status line says where the text came from.
 *
 * Only for a map with no loose script beside it — a loose one overrides the
 * packed script when a round starts on it, and overrides it here too. */
void meScenarioSetPackedScript(MEScenarioState *st, const char *text,
                               size_t len);

/* Re-reads scriptPath, discarding edits, and asks the view to re-seed. False
 * with the status line set and the buffer kept when there is nothing to read,
 * which is the path a script written beside an already-open map takes on its
 * first press and any press before it. */
bool meScenarioReload(MEScenarioState *st);

/* Replaces the buffer with len bytes of text and marks it dirty. */
void meScenarioSetText(MEScenarioState *st, const char *text, size_t len);

/* Puts one line on the panel's status line: what the last read or write did.
 * The script view draws it in its toolbar and the metadata view under its
 * save buttons, so the panel says one thing at a time. */
void meScenarioSetStatus(MEScenarioState *st, const char *text);

/* True when the buffer differs from what is on disk. */
bool meScenarioDirty(const MEScenarioState *st);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_H */
