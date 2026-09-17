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
 *   Nothing here reads or writes anything. Pack into Map
 *   is what puts the manifest in a container, so the form
 *   is emptied whenever the map changes and its dirty flag
 *   counts as unsaved work until that pack has run.
 *
 *   The manifest records what the author set and nothing
 *   else. A rule typed back to its classic value stays in
 *   the list; only Remove takes a row out.
 *
 *   Tags and regions live here too, and the tag accessors are
 *   the one place the editor's entity numbering meets the
 *   manifest's. The editor counts its pills, bases and starts
 *   from 0 and the file counts from 1, so editor entity i is
 *   manifest entry i + 1 and that sum is written once, in
 *   meScnTagsAt below.
 *********************************************************/

#ifndef MAPEDITOR_SCENARIO_FORM_H
#define MAPEDITOR_SCENARIO_FORM_H

#include <stdbool.h>
#include <stdint.h>

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

/* Which of the three entity lists a tag is on. The manifest holds one array
 * per kind and the panel draws them in this order. */
typedef enum {
    ME_SCENARIO_TAG_PILL = 0,
    ME_SCENARIO_TAG_BASE,
    ME_SCENARIO_TAG_START
} MEScenarioTagKind;

/* How many entities of a kind a map can hold, which is one less than the
 * length of the manifest's array for it: MAX_PILLS, MAX_BASES, MAX_STARTS.
 * Editor indices run 0 to this less one. 0 for a kind that is none of the
 * three. */
int meScenarioFormEntityCap(MEScenarioTagKind kind);

/* The tags on one editor entity, or NULL for a kind or an index outside the
 * range above.
 *
 * editorIndex is 0-based, the way the editor's own pill, base and start lists
 * are; the manifest's arrays are 1-based, the way the file writes them. The
 * conversion between the two lives here and nowhere else. */
const ScnManifestTags *meScenarioFormTags(const MEScenarioForm *f,
                                          MEScenarioTagKind     kind,
                                          int                   editorIndex);

/* Puts another tag on an editor entity, cut to fit SCN_TAG_LEN rather than
 * written past it. False for an entity that already carries
 * SCN_TAGS_PER_ENTITY of them, for an empty name, and for an index outside the
 * range meScenarioFormEntityCap gives. editorIndex is 0-based, as above. */
bool meScenarioFormAddTag(MEScenarioForm *f, MEScenarioTagKind kind,
                          int editorIndex, const char *tag);

/* Drops tag number at from an editor entity, keeping the rest packed and the
 * count right. An index off either end changes nothing. editorIndex is
 * 0-based, as above. */
void meScenarioFormRemoveTag(MEScenarioForm *f, MEScenarioTagKind kind,
                             int editorIndex, int at);

/* Appends a named rectangle of map squares: an inclusive top-left at x,y and a
 * size of w by h. The rectangle is clamped — at least one square each way, and
 * the whole of it on the map. False for an empty name and once the manifest
 * holds SCN_REGIONS_MAX of them. */
bool meScenarioFormAddRegion(MEScenarioForm *f, const char *name, int x, int y,
                             int w, int h);

/* Moves the region at index over a different rectangle, clamped the same way.
 * False for an index off the end. */
bool meScenarioFormSetRegionRect(MEScenarioForm *f, int index, int x, int y,
                                 int w, int h);

/* Drops the region at index, keeping the rest of regions[] packed. */
void meScenarioFormRemoveRegion(MEScenarioForm *f, int index);

/* How many tags the manifest carries, over all three kinds, and how many
 * regions. What Save as Mod says will be left out, since a mod is written from
 * a copy with both cleared. */
int meScenarioFormTagCount(const MEScenarioForm *f);
int meScenarioFormRegionCount(const MEScenarioForm *f);

/* What the tags view needs to know about the open map: how many of each entity
 * it holds, where each one sits, and the rectangle the selection tool is
 * holding, which is where a region's bounds come from.
 *
 * The panel cannot ask the map for any of it — pillsGetNumPills and its two
 * neighbours are bolo internals and the panel's translation unit has no reach
 * into them — so mapeditor.c, which has, fills this in and hands it over.
 * Indices here are the editor's own, counting from 0. */
typedef struct MEScenarioMapInfo {
    int     numPills;
    int     numBases;
    int     numStarts;
    uint8_t pillX[MAX_PILLS];
    uint8_t pillY[MAX_PILLS];
    uint8_t baseX[MAX_BASES];
    uint8_t baseY[MAX_BASES];
    uint8_t startX[MAX_STARTS];
    uint8_t startY[MAX_STARTS];

    bool    hasSelection;        /* the selection tool is holding a rectangle */
    int     selX, selY;          /* its top-left square, inclusive */
    int     selW, selH;          /* and its size, at least 1 by 1 */
} MEScenarioMapInfo;

/* True when the manifest has been edited since it was last emptied. */
bool meScenarioFormDirty(const MEScenarioForm *f);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SCENARIO_FORM_H */
