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
 *
 *   The triggers are here as well, as whole rows: adding one,
 *   dropping one, setting which hook it runs on, and the same
 *   three over the tests and actions inside it. A hook name is
 *   held against the function catalogue and a new test and a
 *   new action are made out of it, which this module reaches
 *   through mapeditor_scenario_fndesc.h and
 *   mapeditor_scenario_check.h.
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

/* What is wrong with a team's number, since the form lets the author type any
 * byte into it. */
typedef enum {
    ME_SCENARIO_TEAM_ID_OK = 0,
    ME_SCENARIO_TEAM_ID_RANGE, /* outside 1 to MAX_TANKS - 1 */
    ME_SCENARIO_TEAM_ID_TAKEN  /* a number a team above it already holds */
} MEScenarioTeamIdProblem;

/* Which of those the team at index has, so the lobby form can say it under the
 * field rather than leaving it to the validator at pack time. The two problems
 * are the ones scenario_validate.c reports for lobby.teams[].id, and a shared
 * number is reported the way it reports one: against the later of the two
 * teams, so one pair of numbers draws one hint. ME_SCENARIO_TEAM_ID_OK for an
 * index the template does not seat. */
MEScenarioTeamIdProblem meScenarioFormTeamIdProblem(const MEScenarioForm *f,
                                                    int index);

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

/* Appends a trigger set to run on the named hook. False when the table already
 * holds SCN_TRIGGERS_MAX of them, or the name is not one the catalogue carries
 * as a hook.
 *
 * A trigger is created already naming one, never blank: the validator refuses
 * a trigger that names no hook, so a blank one would be born broken. A policy
 * name is refused for the same reason — the host asks a policy a question and
 * a list of actions has none to give it. */
bool meScenarioFormAddTrigger(MEScenarioForm *f, const char *when);

/* Drops the trigger at index, keeping the rest of triggers[] packed. */
void meScenarioFormRemoveTrigger(MEScenarioForm *f, int index);

/* Changes which hook a trigger runs on. False for an index the table has not
 * got, and for a name the catalogue does not carry as a hook. */
bool meScenarioFormSetTriggerWhen(MEScenarioForm *f, int index,
                                  const char *when);

/* How many triggers the manifest carries. */
int meScenarioFormTriggerCount(const MEScenarioForm *f);

/* The operators a test may ask with: how many there are, and the one at an
 * index. The seven the surface has and never SCN_TRIG_CMP_UNKNOWN, which is
 * what a row naming no operator is left as and is no choice to offer;
 * SCN_TRIG_CMP_UNKNOWN is what an index off either end answers. */
int            meScenarioFormCompareCount(void);
ScnTrigCompare meScenarioFormCompareAt(int index);

/* The word a file writes one as — "eq", "in" — out of the table both readers
 * resolve a file's own word through, so the editor cannot come to offer a word
 * they do not know. "" for SCN_TRIG_CMP_UNKNOWN and for a value outside the
 * enum. */
const char *meScenarioFormCompareName(ScnTrigCompare op);

/* ── The tests and actions inside one trigger ────────────────────────
 *
 * Coarse on purpose: the view reads a row, edits a copy and writes the whole
 * row back. A setter per part would be ten functions and three more places to
 * spell a field name wrong.
 *
 * Every one of these refuses a trigger the table has not got, and the four
 * that take a row index refuse one the trigger has not got.
 *
 * What Set does not do is check the vocabulary. A field that is no field of
 * the hook, an op the game table has not got, an operator the field cannot
 * answer — each of those is scnCheckTriggers' to report, in a sentence the
 * author reads in the issues list, and refusing it here as well would be two
 * answers to one question. The form holds the shape; the validator holds the
 * meaning. */

/* How many tests the trigger carries, and 0 for a trigger the table has not
 * got. */
int meScenarioFormCondCount(const MEScenarioForm *f, int trigger);

/* Appends a test the hook can answer: the first field of the trigger's own
 * hook, an operator that field takes, and an empty value of the kind the
 * field holds for the author to fill in.
 *
 * Never blank, for the reason a trigger is never blank, read off what a blank
 * one would cost: the validator says nothing about a test naming no field, so
 * a blank one would be a fault nobody reports until the file is read back and
 * the reader refuses the row.
 *
 * False at SCN_TRIGGER_CONDS_MAX, and false for a trigger on a hook that is
 * handed nothing — there is no test to write against a payload with nothing
 * in it. */
bool meScenarioFormAddCond(MEScenarioForm *f, int trigger);

/* Drops the test at index, keeping the rest of the trigger's tests packed. */
void meScenarioFormRemoveCond(MEScenarioForm *f, int trigger, int index);

/* Writes a whole test over the one at index. */
bool meScenarioFormSetCond(MEScenarioForm *f, int trigger, int index,
                           const ScnTrigCond *cond);

/* How many actions the trigger carries, and 0 for a trigger the table has not
 * got. */
int meScenarioFormActionCount(const MEScenarioForm *f, int trigger);

/* Appends an action naming the first op the vocabulary offers that insists on
 * no arguments, so the row is one the validator passes as it stands and the
 * author changes an op rather than filling a blank in. The op is read off the
 * registry, so no name of one is written here.
 *
 * False at SCN_TRIGGER_ACTIONS_MAX. */
bool meScenarioFormAddAction(MEScenarioForm *f, int trigger);

/* Drops the action at index, keeping the rest of the trigger's actions
 * packed. */
void meScenarioFormRemoveAction(MEScenarioForm *f, int trigger, int index);

/* Writes a whole action over the one at index. False for one naming more than
 * SCN_TRIGGER_ARGS_MAX arguments, which is the one thing about an action that
 * is its shape rather than its meaning. */
bool meScenarioFormSetAction(MEScenarioForm *f, int trigger, int index,
                             const ScnTrigAct *act);

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
