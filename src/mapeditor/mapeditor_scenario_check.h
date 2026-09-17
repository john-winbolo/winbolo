/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_check.h
 * Purpose:
 *   The editor's reach into the scenario runtime: the
 *   validator run over the text in the script pane, and the
 *   game.* rows the completion list is built from.
 *
 *   Both calls live behind this header so the panel code
 *   names no scenario header. The check runs no sim
 *   and boots no host — it loads the chunk once in a Lua
 *   state of its own and reads the table it declared, which
 *   is what keeps the editor's privileged exception in
 *   docs/ARCHITECTURE.md intact.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_CHECK_H
#define MAPEDITOR_SCENARIO_CHECK_H

#include <stdbool.h>
#include <stddef.h>

#include "scenario_issues.h" /* ScnValidateResult, ScnValidateIssue */
#include "../scenario_io/scenario_manifest.h" /* ScenarioManifest — the table
                                               * the check pushes for the
                                               * script to be read against */

/* What a script with no file yet is called in a Lua error. Not translated: it
 * stands where a path stands, and Lua writes it ahead of the line number in a
 * message the author reads as the compiler's own words. */
#define ME_SCENARIO_CHECK_UNNAMED "untitled.scenario.lua"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MEScenarioCheck {
    ScnValidateResult result;
    bool              hasRun;       /* false until the first check, which is
                                     * what tells "no problems" from "not
                                     * looked at" */
    bool              pushToWidget; /* the view must re-apply its markers, the
                                     * way MEScenarioState re-seeds its text */
    bool              stale;        /* the script has been edited since the
                                     * check ran, so the lines the issues name
                                     * are the lines the text had then. Set by
                                     * the view, which is where an edit is
                                     * seen; the issues themselves are kept,
                                     * because a problem the author has not
                                     * reached yet is still a problem. */
} MEScenarioCheck;

/* An empty check that has not run. */
void meScenarioCheckInit(MEScenarioCheck *c);

/* Forgets the last run: no issues, and hasRun false again. What a reloaded
 * script or a new map leaves behind. */
void meScenarioCheckClear(MEScenarioCheck *c);

/* Checks len bytes of script text and keeps the result. name is what the chunk
 * is called in a Lua error — the script's path, or ME_SCENARIO_CHECK_UNNAMED
 * where the map has no file yet.
 *
 * manifest goes on as the scenario global before the chunk runs, the way a
 * server does it for a script that came out of a package, and is NULL for
 * none. The editor passes the manifest it is about to write, so a script that
 * leaves the table to the forms checks clean here exactly as it loads there.
 * A script that declares its own table overwrites the global, so the two are
 * still compared afterwards and a conflict is still reported.
 *
 * The sim handed to the validator is NULL, because the editor has none and
 * must not make one. That leaves out the tags against the entity lists, which
 * is the one check that reads a map; everything the table says about itself is
 * still checked, the rules included — a rule's bounds and the pairs it sits in
 * belong to the rules table rather than to a map. */
void meScenarioCheckRun(MEScenarioCheck *c, const char *text, size_t len,
                        const char *name, const ScenarioManifest *manifest);

/* How many rows the scenario surface has, and the name and the one-line
 * document of one of them. The list is the registry itself rather than a copy,
 * so a row added to the surface is in the editor's completion list with no
 * second edit. False for an index past the end, which leaves both out
 * parameters alone. */
size_t meScenarioCompletionCount(void);
bool   meScenarioCompletionAt(size_t index, const char **name,
                              const char **doc);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_CHECK_H */
