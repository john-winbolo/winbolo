/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_form.c
 * Purpose:
 *   The manifest the scenario panel's forms edit, and the
 *   list handling those forms need: setting a rule without
 *   ever giving one rule two rows, removing a row without
 *   leaving a hole, and adding a team with a team number
 *   nobody else has.
 *
 *   The struct is the whole state. Every mutator marks the
 *   form dirty and none of them touches a file.
 *
 *   The tag accessors carry the editor's one index
 *   conversion: entity i on the map is entry i + 1 in the
 *   manifest, because the file counts entities from 1 and
 *   entry 0 of each array is unused. meScnTagsAt is the only
 *   place that sum is written.
 *********************************************************/

#include "mapeditor_scenario_form.h"

#include <stdint.h>
#include <string.h>

/* The function catalogue, for what the triggers below ask of it: whether a
 * name is a hook, and the fields a hook's payload carries. It arrives through
 * this header rather than through scenario_lua.h because that one names Lua
 * types, and this module is compiled into the embedded editor as well as the
 * standalone one. */
#include "mapeditor_scenario_fndesc.h"

/* scnManifestTrigOpName: the seven operator words, out of the table both
 * readers resolve a file's own word through. The editor offers what they
 * accept because it is the same list, not a copy of it. */
#include "../scenario_io/scenario_manifest_json.h"

/* SCENARIO_API_VERSION: the version of the scenario surface this build
 * writes, so a new manifest states the one it was authored against. Only the
 * constant is taken from there: the editor links the scenario runtime and the
 * container library, but this file calls nothing declared on that header or
 * anything else in either of them. gamefront.c takes the same header the same
 * way. */
#include "../scenario/scenario_host.h"

void meScenarioFormReset(MEScenarioForm *f) {
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->manifest.api = SCENARIO_API_VERSION;
    /* A scenario is written for the map it is being edited beside until the
     * author says otherwise. */
    f->manifest.bound = true;
    f->dirty          = false;
}

void meScenarioFormInit(MEScenarioForm *f) { meScenarioFormReset(f); }

ScnManifestKind meScenarioFormKind(const MEScenarioForm *f) {
    return (f == NULL) ? scnKindScenario : f->manifest.kind;
}

void meScenarioFormSetKind(MEScenarioForm *f, ScnManifestKind kind) {
    if (f == NULL || kind == scnKindUnknown || f->manifest.kind == kind) {
        return;
    }
    /* Only the kind. The game type and anything else a mod may not use stay
       in the manifest: the author typed them, and a form that emptied a
       field because a combo moved would lose work on a mis-click. They stop
       being offered and the check refuses the file while they are set, which
       says the same thing without throwing anything away. */
    f->manifest.kind = kind;
    f->dirty         = true;
    f->edits++;
}

bool meScenarioFormKeepsWinCondition(const MEScenarioForm *f) {
    return f != NULL && scnManifestKeepsWinCondition(&f->manifest);
}

int meScenarioFormFindRule(const MEScenarioForm *f, int rule) {
    uint16_t i;

    if (f == NULL || rule < 0 || rule > UINT16_MAX) {
        return -1;
    }
    for (i = 0; i < f->manifest.numRules; i++) {
        if (f->manifest.rules[i].rule == (uint16_t)rule) {
            return (int)i;
        }
    }
    return -1;
}

bool meScenarioFormSetRule(MEScenarioForm *f, int rule, double value) {
    int at;

    if (f == NULL || rule < 0 || rule > UINT16_MAX) {
        return false;
    }

    at = meScenarioFormFindRule(f, rule);
    if (at >= 0) {
        f->manifest.rules[at].value = value;
        f->dirty                    = true;
        f->edits++;
        return true;
    }

    if (f->manifest.numRules >= SCN_MANIFEST_RULES_MAX) {
        return false;
    }
    f->manifest.rules[f->manifest.numRules].rule  = (uint16_t)rule;
    f->manifest.rules[f->manifest.numRules].value = value;
    f->manifest.numRules++;
    f->dirty = true;
    f->edits++;
    return true;
}

void meScenarioFormRemoveRule(MEScenarioForm *f, int index) {
    if (f == NULL || index < 0 || index >= (int)f->manifest.numRules) {
        return;
    }
    /* The rows after it close up, so the list stays the list the author sees
     * and numRules still counts it. */
    memmove(&f->manifest.rules[index], &f->manifest.rules[index + 1],
            (size_t)((int)f->manifest.numRules - index - 1) *
                sizeof(f->manifest.rules[0]));
    f->manifest.numRules--;
    memset(&f->manifest.rules[f->manifest.numRules], 0,
           sizeof(f->manifest.rules[0]));
    f->dirty = true;
    f->edits++;
}

bool meScenarioFormAddTeam(MEScenarioForm *f) {
    ScnManifestLobby *lob;
    ScnManifestTeam  *team;
    unsigned          id;

    if (f == NULL) {
        return false;
    }
    lob = &f->manifest.lobby;
    /* A team number runs 1 to MAX_TANKS - 1, the range scenario_validate.c
     * holds an id to: the engine drops a team at MAX_TANKS or above without
     * saying so. Team numbers are unique and start at 1, so one team past
     * that count would have to take a number no game will seat, and the form
     * does not offer what is always refused. teams[] therefore ends with a
     * slot the template never fills. */
    if (lob->numTeams >= MAX_TANKS - 1) {
        return false;
    }

    /* The lowest team number the template does not already seat, counting
     * from the first one a game has. */
    for (id = 1; id <= MAX_TANKS - 1; id++) {
        uint8_t i;
        bool    taken = false;
        for (i = 0; i < lob->numTeams; i++) {
            if (lob->teams[i].id == (uint8_t)id) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            break;
        }
    }

    team = &lob->teams[lob->numTeams];
    memset(team, 0, sizeof(*team));
    team->id      = (uint8_t)id;
    team->bots    = 0;
    team->maxBots = 0;
    team->fielded = false;
    lob->numTeams++;
    f->dirty = true;
    f->edits++;
    return true;
}

void meScenarioFormRemoveTeam(MEScenarioForm *f, int index) {
    ScnManifestLobby *lob;

    if (f == NULL) {
        return;
    }
    lob = &f->manifest.lobby;
    if (index < 0 || index >= (int)lob->numTeams) {
        return;
    }
    memmove(&lob->teams[index], &lob->teams[index + 1],
            (size_t)((int)lob->numTeams - index - 1) * sizeof(lob->teams[0]));
    lob->numTeams--;
    memset(&lob->teams[lob->numTeams], 0, sizeof(lob->teams[0]));
    f->dirty = true;
    f->edits++;
}

MEScenarioTeamIdProblem meScenarioFormTeamIdProblem(const MEScenarioForm *f,
                                                    int index) {
    const ScnManifestLobby *lob;
    uint8_t                 id;
    int                     i;

    if (f == NULL) {
        return ME_SCENARIO_TEAM_ID_OK;
    }
    lob = &f->manifest.lobby;
    if (index < 0 || index >= (int)lob->numTeams) {
        return ME_SCENARIO_TEAM_ID_OK;
    }
    id = lob->teams[index].id;

    /* The range Add Team picks from, and the one scenario_validate.c holds an
       id to. Add Team never leaves this range; the field the author types into
       takes any byte, which is what these two answers are for. */
    if (id < 1 || id >= MAX_TANKS) {
        return ME_SCENARIO_TEAM_ID_RANGE;
    }
    /* Only the teams above it, so a shared number marks the later of the two
       rows — the same team the validator names it against. */
    for (i = 0; i < index; i++) {
        if (lob->teams[i].id == id) {
            return ME_SCENARIO_TEAM_ID_TAKEN;
        }
    }
    return ME_SCENARIO_TEAM_ID_OK;
}

/* ── Tags ─────────────────────────────────────────────────────────── */

int meScenarioFormEntityCap(MEScenarioTagKind kind) {
    switch (kind) {
        case ME_SCENARIO_TAG_PILL:
            return MAX_PILLS;
        case ME_SCENARIO_TAG_BASE:
            return MAX_BASES;
        case ME_SCENARIO_TAG_START:
            return MAX_STARTS;
        default:
            return 0;
    }
}

/* The manifest entry holding the tags on editor entity editorIndex.
 *
 * This is the conversion, and the only copy of it. The editor's pill, base and
 * start lists count from 0; the manifest's arrays are 1-based because Lua
 * writes tags.pills[1] for the first pill, so entry 0 of each array is unused
 * and the arrays are one longer than the entity cap. Editor entity i is
 * therefore manifest entry i + 1, and the last entity a map can hold, cap - 1,
 * is entry cap — the last one the array has.
 *
 * NULL for a kind that is none of the three and for an index outside 0 to
 * cap - 1. */
static ScnManifestTags *meScnTagsAt(ScenarioManifest *m, MEScenarioTagKind kind,
                                    int editorIndex) {
    const int cap = meScenarioFormEntityCap(kind);

    if (m == NULL || cap == 0 || editorIndex < 0 || editorIndex >= cap) {
        return NULL;
    }
    switch (kind) {
        case ME_SCENARIO_TAG_PILL:
            return &m->pillTags[editorIndex + 1];
        case ME_SCENARIO_TAG_BASE:
            return &m->baseTags[editorIndex + 1];
        case ME_SCENARIO_TAG_START:
            return &m->startTags[editorIndex + 1];
        default:
            return NULL;
    }
}

const ScnManifestTags *meScenarioFormTags(const MEScenarioForm *f,
                                          MEScenarioTagKind     kind,
                                          int                   editorIndex) {
    if (f == NULL) {
        return NULL;
    }
    /* The cast is over the const on the form, not over the manifest's shape:
     * one conversion serves the readers and the two writers below. */
    return meScnTagsAt((ScenarioManifest *)&f->manifest, kind, editorIndex);
}

bool meScenarioFormAddTag(MEScenarioForm *f, MEScenarioTagKind kind,
                          int editorIndex, const char *tag) {
    ScnManifestTags *tags;
    size_t           n;

    if (f == NULL || tag == NULL || tag[0] == '\0') {
        return false;
    }
    tags = meScnTagsAt(&f->manifest, kind, editorIndex);
    if (tags == NULL || tags->count >= SCN_TAGS_PER_ENTITY) {
        return false;
    }

    /* Cut rather than written past: the field holds SCN_TAG_LEN bytes with the
     * terminator among them, and a long name is the author's to shorten. */
    n = strlen(tag);
    if (n >= SCN_TAG_LEN) {
        n = SCN_TAG_LEN - 1;
    }
    memcpy(tags->tag[tags->count], tag, n);
    tags->tag[tags->count][n] = '\0';
    tags->count++;
    f->dirty = true;
    f->edits++;
    return true;
}

void meScenarioFormRemoveTag(MEScenarioForm *f, MEScenarioTagKind kind,
                             int editorIndex, int at) {
    ScnManifestTags *tags;

    if (f == NULL) {
        return;
    }
    tags = meScnTagsAt(&f->manifest, kind, editorIndex);
    if (tags == NULL || at < 0 || at >= (int)tags->count) {
        return;
    }
    memmove(tags->tag[at], tags->tag[at + 1],
            (size_t)((int)tags->count - at - 1) * sizeof(tags->tag[0]));
    tags->count--;
    memset(tags->tag[tags->count], 0, sizeof(tags->tag[0]));
    f->dirty = true;
    f->edits++;
}

int meScenarioFormTagCount(const MEScenarioForm *f) {
    static const MEScenarioTagKind kKinds[] = {
        ME_SCENARIO_TAG_PILL, ME_SCENARIO_TAG_BASE, ME_SCENARIO_TAG_START
    };
    int n = 0;
    int k;

    if (f == NULL) {
        return 0;
    }
    for (k = 0; k < (int)(sizeof(kKinds) / sizeof(kKinds[0])); k++) {
        const int cap = meScenarioFormEntityCap(kKinds[k]);
        int       e;
        for (e = 0; e < cap; e++) {
            const ScnManifestTags *tags = meScenarioFormTags(f, kKinds[k], e);
            if (tags != NULL) {
                n += (int)tags->count;
            }
        }
    }
    return n;
}

/* ── Regions ──────────────────────────────────────────────────────── */

/* A rectangle the manifest can hold and the validator will not complain
 * about: at least one square each way, and every square of it on the map.
 *
 * The four numbers are stored as bytes, so a size of 256 cannot be written at
 * all; a rectangle drawn over the whole map gives up its last column and row
 * rather than wrapping to zero. */
static void meScnClampRect(int *x, int *y, int *w, int *h) {
    if (*x < 0) {
        *x = 0;
    }
    if (*y < 0) {
        *y = 0;
    }
    if (*x > MAP_ARRAY_SIZE - 1) {
        *x = MAP_ARRAY_SIZE - 1;
    }
    if (*y > MAP_ARRAY_SIZE - 1) {
        *y = MAP_ARRAY_SIZE - 1;
    }
    if (*w < 1) {
        *w = 1;
    }
    if (*h < 1) {
        *h = 1;
    }
    if (*x + *w > MAP_ARRAY_SIZE) {
        *w = MAP_ARRAY_SIZE - *x;
    }
    if (*y + *h > MAP_ARRAY_SIZE) {
        *h = MAP_ARRAY_SIZE - *y;
    }
    if (*w > UINT8_MAX) {
        *w = UINT8_MAX;
    }
    if (*h > UINT8_MAX) {
        *h = UINT8_MAX;
    }
}

bool meScenarioFormAddRegion(MEScenarioForm *f, const char *name, int x, int y,
                             int w, int h) {
    ScnManifestRegion *r;
    size_t             n;

    if (f == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    if (f->manifest.numRegions >= SCN_REGIONS_MAX) {
        return false;
    }
    meScnClampRect(&x, &y, &w, &h);

    r = &f->manifest.regions[f->manifest.numRegions];
    memset(r, 0, sizeof(*r));
    n = strlen(name);
    if (n >= SCN_REGION_NAME_LEN) {
        n = SCN_REGION_NAME_LEN - 1;
    }
    memcpy(r->name, name, n);
    r->name[n] = '\0';
    r->x       = (uint8_t)x;
    r->y       = (uint8_t)y;
    r->w       = (uint8_t)w;
    r->h       = (uint8_t)h;
    f->manifest.numRegions++;
    f->dirty = true;
    f->edits++;
    return true;
}

bool meScenarioFormSetRegionRect(MEScenarioForm *f, int index, int x, int y,
                                 int w, int h) {
    ScnManifestRegion *r;

    if (f == NULL || index < 0 || index >= (int)f->manifest.numRegions) {
        return false;
    }
    meScnClampRect(&x, &y, &w, &h);

    r        = &f->manifest.regions[index];
    r->x     = (uint8_t)x;
    r->y     = (uint8_t)y;
    r->w     = (uint8_t)w;
    r->h     = (uint8_t)h;
    f->dirty = true;
    f->edits++;
    return true;
}

void meScenarioFormRemoveRegion(MEScenarioForm *f, int index) {
    if (f == NULL || index < 0 || index >= (int)f->manifest.numRegions) {
        return;
    }
    memmove(&f->manifest.regions[index], &f->manifest.regions[index + 1],
            (size_t)((int)f->manifest.numRegions - index - 1) *
                sizeof(f->manifest.regions[0]));
    f->manifest.numRegions--;
    memset(&f->manifest.regions[f->manifest.numRegions], 0,
           sizeof(f->manifest.regions[0]));
    f->dirty = true;
    f->edits++;
}

int meScenarioFormRegionCount(const MEScenarioForm *f) {
    return (f == NULL) ? 0 : (int)f->manifest.numRegions;
}

/* ── Triggers ─────────────────────────────────────────────────────── */

/* Whether the catalogue carries that name as a hook, and whether the field a
 * trigger holds it in has room for it. *row comes back as the catalogue index
 * the name answers to, which is the index a hook's fields are read at, and is
 * left alone for a name that is no row at all.
 *
 * Both halves refuse rather than repair. A name the catalogue has not got is
 * a hook nothing will ever dispatch, and a policy name is a question a list of
 * actions cannot answer — scenario_validate.c refuses each of those with a
 * reason of its own. A name too long for the field would be cut, and a cut
 * hook name is no longer a hook name, so the length is checked here rather
 * than truncated at the copy. Every hook the catalogue carries fits with room
 * over. */
static bool meScnHookRow(const char *when, size_t *row) {
    const size_t count = meScnFnCount();
    size_t       i;

    if (when == NULL || when[0] == '\0' ||
        strlen(when) >= SCN_TRIGGER_NAME_LEN) {
        return false;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(meScnFnName(i), when) == 0) {
            if (row != NULL) {
                *row = i;
            }
            return meScnFnIsHook(i);
        }
    }
    return false;
}

static bool meScnHookName(const char *when) {
    return meScnHookRow(when, NULL);
}

/* A name into one of the manifest's name fields, cut to fit rather than
 * written past it. The whole field is cleared first, so nothing of what it
 * held is left past the terminator for a byte-for-byte comparison of two
 * manifests to read as a difference. */
static void meScnCopyName(char *out, size_t outLen, const char *name) {
    const size_t n = strlen(name);

    memset(out, 0, outLen);
    memcpy(out, name, (n < outLen) ? n : outLen - 1);
}

/* The hook a trigger runs on, written over whatever the field held. */
static void meScnSetWhen(ScnTrigger *t, const char *when) {
    meScnCopyName(t->when, sizeof(t->when), when);
}

bool meScenarioFormAddTrigger(MEScenarioForm *f, const char *when) {
    ScnTrigger *t;

    if (f == NULL || !meScnHookName(when)) {
        return false;
    }
    if (f->manifest.numTriggers >= SCN_TRIGGERS_MAX) {
        return false;
    }

    t = &f->manifest.triggers[f->manifest.numTriggers];
    memset(t, 0, sizeof(*t));
    meScnSetWhen(t, when);
    f->manifest.numTriggers++;
    f->dirty = true;
    f->edits++;
    return true;
}

void meScenarioFormRemoveTrigger(MEScenarioForm *f, int index) {
    if (f == NULL || index < 0 || index >= (int)f->manifest.numTriggers) {
        return;
    }
    memmove(&f->manifest.triggers[index], &f->manifest.triggers[index + 1],
            (size_t)((int)f->manifest.numTriggers - index - 1) *
                sizeof(f->manifest.triggers[0]));
    f->manifest.numTriggers--;
    memset(&f->manifest.triggers[f->manifest.numTriggers], 0,
           sizeof(f->manifest.triggers[0]));
    f->dirty = true;
    f->edits++;
}

bool meScenarioFormSetTriggerWhen(MEScenarioForm *f, int index,
                                  const char *when) {
    if (f == NULL || index < 0 || index >= (int)f->manifest.numTriggers) {
        return false;
    }
    if (!meScnHookName(when)) {
        return false;
    }
    /* The tests and the actions are left alone: which hook a trigger listens
       on is the author's to change without losing what they wrote. A test that
       named a field of the old hook is what the validator reports. */
    meScnSetWhen(&f->manifest.triggers[index], when);
    f->dirty = true;
    f->edits++;
    return true;
}

int meScenarioFormTriggerCount(const MEScenarioForm *f) {
    return (f == NULL) ? 0 : (int)f->manifest.numTriggers;
}

/* ── The operators a test asks with ─────────────────────────────────── */

int meScenarioFormCompareCount(void) {
    /* Counted off the enum rather than written down: EQ is the first of the
       seven and IN the last, and SCN_TRIG_CMP_UNKNOWN sits below EQ so it is
       never one of them. */
    return (int)SCN_TRIG_CMP_IN - (int)SCN_TRIG_CMP_EQ + 1;
}

ScnTrigCompare meScenarioFormCompareAt(int index) {
    if (index < 0 || index >= meScenarioFormCompareCount()) {
        return SCN_TRIG_CMP_UNKNOWN;
    }
    return (ScnTrigCompare)((int)SCN_TRIG_CMP_EQ + index);
}

const char *meScenarioFormCompareName(ScnTrigCompare op) {
    const char *name = scnManifestTrigOpName(op);

    return (name != NULL) ? name : "";
}

/* ── The tests and actions inside a trigger ─────────────────────────── */

/* The trigger at that index, or NULL for one the table has not got. Every
 * accessor below starts here, so an index off the end is refused in one place
 * rather than in eight. */
static ScnTrigger *meScnTriggerAt(ScenarioManifest *m, int trigger) {
    if (m == NULL || trigger < 0 || trigger >= (int)m->numTriggers) {
        return NULL;
    }
    return &m->triggers[trigger];
}

/* The same, off a form the caller is only reading. The cast is over the const
 * on the form and not over the manifest's shape, the way meScenarioFormTags
 * does it. */
static const ScnTrigger *meScnTriggerOf(const MEScenarioForm *f, int trigger) {
    if (f == NULL) {
        return NULL;
    }
    return meScnTriggerAt((ScenarioManifest *)&f->manifest, trigger);
}

bool meScenarioFormFieldIsSet(MEScnParamType type, bool derived) {
    return derived &&
           (type == ME_SCN_PARAM_TAG || type == ME_SCN_PARAM_REGION);
}

/* The kind of value a field is tested against, so a new test carries an empty
 * value of the shape the author is about to fill in rather than a zero that
 * means a different thing on either side of the comparison. */
static ScnTrigValueKind meScnFieldValueKind(MEScnParamType type) {
    switch (type) {
        case ME_SCN_PARAM_BOOL:
            return SCN_TRIG_VAL_BOOL;
        case ME_SCN_PARAM_WORD:
        case ME_SCN_PARAM_STRING:
        case ME_SCN_PARAM_TAG:
        case ME_SCN_PARAM_REGION:
            return SCN_TRIG_VAL_STRING;
        default:
            return SCN_TRIG_VAL_NUMBER;
    }
}

int meScenarioFormCondCount(const MEScenarioForm *f, int trigger) {
    const ScnTrigger *t = meScnTriggerOf(f, trigger);

    return (t != NULL) ? (int)t->numWhere : 0;
}

bool meScenarioFormAddCond(MEScenarioForm *f, int trigger) {
    ScnTrigger    *t = (f == NULL) ? NULL
                                   : meScnTriggerAt(&f->manifest, trigger);
    ScnTrigCond   *c;
    char           field[ME_SCN_FIELD_NAME_LEN];
    MEScnParamType type    = ME_SCN_PARAM_NONE;
    bool           derived = false;
    size_t         row     = 0;

    if (t == NULL || t->numWhere >= SCN_TRIGGER_CONDS_MAX) {
        return false;
    }
    if (!meScnHookRow(t->when, &row) ||
        !meScnFnFieldAt(row, 0, field, sizeof(field), &type, &derived)) {
        return false;
    }

    c = &t->where[t->numWhere];
    memset(c, 0, sizeof(*c));
    meScnCopyName(c->field, sizeof(c->field), field);
    /* in for a field that answers a set of names and eq for one that answers a
       value, which is what scnCheckTrigCond holds a test to either way. */
    c->op         = meScenarioFormFieldIsSet(type, derived) ? SCN_TRIG_CMP_IN
                                                           : SCN_TRIG_CMP_EQ;
    c->value.kind = meScnFieldValueKind(type);
    t->numWhere++;
    f->dirty = true;
    f->edits++;
    return true;
}

void meScenarioFormRemoveCond(MEScenarioForm *f, int trigger, int index) {
    ScnTrigger *t = (f == NULL) ? NULL
                                : meScnTriggerAt(&f->manifest, trigger);

    if (t == NULL || index < 0 || index >= (int)t->numWhere) {
        return;
    }
    memmove(&t->where[index], &t->where[index + 1],
            (size_t)((int)t->numWhere - index - 1) * sizeof(t->where[0]));
    t->numWhere--;
    memset(&t->where[t->numWhere], 0, sizeof(t->where[0]));
    f->dirty = true;
    f->edits++;
}

bool meScenarioFormSetCond(MEScenarioForm *f, int trigger, int index,
                           const ScnTrigCond *cond) {
    ScnTrigger *t = (f == NULL) ? NULL
                                : meScnTriggerAt(&f->manifest, trigger);

    if (t == NULL || cond == NULL || index < 0 || index >= (int)t->numWhere) {
        return false;
    }
    t->where[index] = *cond;
    f->dirty        = true;
    f->edits++;
    return true;
}

int meScenarioFormActionCount(const MEScenarioForm *f, int trigger) {
    const ScnTrigger *t = meScnTriggerOf(f, trigger);

    return (t != NULL) ? (int)t->numActions : 0;
}

bool meScenarioFormAddAction(MEScenarioForm *f, int trigger) {
    ScnTrigger *t = (f == NULL) ? NULL
                                : meScnTriggerAt(&f->manifest, trigger);
    ScnTrigAct *a;

    if (t == NULL || t->numActions >= SCN_TRIGGER_ACTIONS_MAX) {
        return false;
    }

    /* Naming nothing, for the author to name. An op chosen here is one they
       did not choose and need not notice: an action born on end_round ends
       the round on the first thing its hook hears. Both readers report an
       action that names no op, so the row stands in the issues list until it
       has been filled in. */
    a = &t->actions[t->numActions];
    memset(a, 0, sizeof(*a));
    t->numActions++;
    f->dirty = true;
    f->edits++;
    return true;
}

void meScenarioFormRemoveAction(MEScenarioForm *f, int trigger, int index) {
    ScnTrigger *t = (f == NULL) ? NULL
                                : meScnTriggerAt(&f->manifest, trigger);

    if (t == NULL || index < 0 || index >= (int)t->numActions) {
        return;
    }
    memmove(&t->actions[index], &t->actions[index + 1],
            (size_t)((int)t->numActions - index - 1) * sizeof(t->actions[0]));
    t->numActions--;
    memset(&t->actions[t->numActions], 0, sizeof(t->actions[0]));
    f->dirty = true;
    f->edits++;
}

bool meScenarioFormSetAction(MEScenarioForm *f, int trigger, int index,
                             const ScnTrigAct *act) {
    ScnTrigger *t = (f == NULL) ? NULL
                                : meScnTriggerAt(&f->manifest, trigger);

    if (t == NULL || act == NULL || index < 0 ||
        index >= (int)t->numActions) {
        return false;
    }
    /* The one refusal here, and it is the shape rather than the meaning: the
       array holds SCN_TRIGGER_ARGS_MAX of them and a count above that names
       arguments there is nowhere to keep. */
    if (act->numArgs > SCN_TRIGGER_ARGS_MAX) {
        return false;
    }
    t->actions[index] = *act;
    f->dirty          = true;
    f->edits++;
    return true;
}

bool meScenarioFormDirty(const MEScenarioForm *f) {
    return f != NULL && f->dirty;
}
