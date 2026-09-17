/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_form.h
 * Purpose:
 *   The manifest behind the scenario panel's metadata,
 *   lobby and rules forms: one ScenarioManifest held in
 *   editor memory, with the adds and removes those forms
 *   need. Plain C, so the drawing in
 *   mapeditor_scenario_imgui.cpp is a view over it the way
 *   the script pane is a view over MEScenarioState.
 *
 *   Nothing here reads or writes anything. There is no
 *   container to put a manifest in yet, so the form is
 *   emptied whenever the map changes and is deliberately
 *   left out of the editor's unsaved-changes checks:
 *   prompting to save what cannot be saved is worse than
 *   not prompting at all.
 *
 *   The manifest records what the author set and nothing
 *   else. A rule typed back to its classic value stays in
 *   the list; only Remove takes a row out.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_FORM_H
#define MAPEDITOR_SCENARIO_FORM_H

#include <stdbool.h>

#include "../scenario_io/scenario_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Named, so a header that only passes one of these about can say
 * "struct MEScenarioForm *" and leave the manifest's layout out of its
 * includers. */
typedef struct MEScenarioForm {
    ScenarioManifest manifest;
    bool             dirty;
} MEScenarioForm;

/* An empty manifest: the API version this build writes, bound to its map,
 * everything else zeroed, and not dirty. */
void meScenarioFormInit(MEScenarioForm *f);

/* The same empty manifest again, for a map change. */
void meScenarioFormReset(MEScenarioForm *f);

/* Where a rule sits in rules[], or -1 when the author has not set it. */
int meScenarioFormFindRule(const MEScenarioForm *f, int rule);

/* Sets a rule to value: in place when the rule is already in the list, and
 * appended when it is not. False when there is no room for another row, so
 * one rule never has two of them. */
bool meScenarioFormSetRule(MEScenarioForm *f, int rule, double value);

/* Drops the row at index, keeping the rest of rules[] packed. */
void meScenarioFormRemoveRule(MEScenarioForm *f, int index);

/* Appends a team with the lowest team number not already in the template and
 * no bots, no brain and an empty init table. False once the template holds
 * MAX_TANKS - 1 of them, which is as many team numbers as a game will
 * seat. */
bool meScenarioFormAddTeam(MEScenarioForm *f);

/* Drops the team at index, keeping the rest of teams[] packed. */
void meScenarioFormRemoveTeam(MEScenarioForm *f, int index);

/* True when the manifest has been edited since it was last emptied. */
bool meScenarioFormDirty(const MEScenarioForm *f);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_FORM_H */
