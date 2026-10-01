/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Visibility presets
 *Filename:      visibility_presets.c
 *Purpose:
 *  The preset table and the match over it - see
 *  visibility_presets.h for what a preset is and why the
 *  match has to be exact.
 *********************************************************/

#include "visibility_presets.h"

#include <string.h>

#include "lang.h"

/* The table. One row per preset, in lobby order, spelled out rather than
 * derived from the row above it: each row is a statement about what a
 * player can see, and a later change to one of them must not quietly
 * move the others.
 *
 * Classic mode is the first column because it owns the rest: while it is
 * on the server writes the other seven itself and refuses an edit to any of
 * them, so the Classic row's remaining values are the ones classic mode
 * forces, and every other row has it off.
 *
 * Allies in trees rides with the bases-and-allies step rather than with
 * the window: "visibility provided by allies" is exactly what it adds, so
 * it goes on where the ally view does.
 *
 * Positional sound goes on with the expanded window: a panned sound
 * tells a player which side an off-screen tank is on, which is the same
 * kind of reach the wider window gives. */
typedef struct {
    VisibilityPreset preset;
    bool             classicMode;
    uint8_t          overviewWindow;
    uint8_t          pill;
    uint8_t          base;
    uint8_t          ally;
    uint8_t          lineOfSight;
    bool             alliesInTrees;
    bool             positionalSound;
    const char      *prefWord;
    int              nameId;
    int              descId;
} VisibilityPresetRow;

static const VisibilityPresetRow kPresets[VISIBILITY_PRESET_COUNT] = {
    { visibilityPresetClassic, true, (uint8_t)overviewWindowNone,
      (uint8_t)viewPolicyKey, (uint8_t)viewPolicyOff, (uint8_t)viewPolicyOff,
      (uint8_t)lineOfSightOff, false, false, "Classic",
      STR_DLGLOBBY_PRESET_CLASSIC, STR_DLGLOBBY_PRESET_CLASSIC_DESC },

    { visibilityPresetClassicOverview, false, (uint8_t)overviewWindowClassic,
      (uint8_t)viewPolicyKey, (uint8_t)viewPolicyOff, (uint8_t)viewPolicyOff,
      (uint8_t)lineOfSightOff, false, false, "ClassicOverview",
      STR_DLGLOBBY_PRESET_CLASSIC_OVERVIEW,
      STR_DLGLOBBY_PRESET_CLASSIC_OVERVIEW_DESC },

    { visibilityPresetExpanded, false, (uint8_t)overviewWindowExpanded,
      (uint8_t)viewPolicyAlways, (uint8_t)viewPolicyOff, (uint8_t)viewPolicyOff,
      (uint8_t)lineOfSightOff, false, true, "Expanded",
      STR_DLGLOBBY_PRESET_EXPANDED, STR_DLGLOBBY_PRESET_EXPANDED_DESC },

    { visibilityPresetMaxView, false, (uint8_t)overviewWindowExpanded,
      (uint8_t)viewPolicyAlways, (uint8_t)viewPolicyAlways,
      (uint8_t)viewPolicyAlways,
      (uint8_t)lineOfSightOff, true, true, "MaxView",
      STR_DLGLOBBY_PRESET_MAXVIEW, STR_DLGLOBBY_PRESET_MAXVIEW_DESC },

    { visibilityPresetSight, false, (uint8_t)overviewWindowExpanded,
      (uint8_t)viewPolicyAlways, (uint8_t)viewPolicyAlways,
      (uint8_t)viewPolicyAlways,
      (uint8_t)lineOfSightBuildingsAndTrees, true, true, "LineOfSight",
      STR_DLGLOBBY_PRESET_SIGHT, STR_DLGLOBBY_PRESET_SIGHT_DESC },
};

/* Writes a row into the settings shape everything else works in. */
static void rowToSettings(const VisibilityPresetRow *row,
                          VisibilitySettings *out) {
    int i; /* Looping variable */

    memset(out, 0, sizeof(*out));
    out->policy[viewCategoryPill] = row->pill;
    out->policy[viewCategoryBase] = row->base;
    out->policy[viewCategoryAlly] = row->ally;
    for (i = 0; i < VIEW_CATEGORY_COUNT; i++) {
        out->decaySecs[i] = (uint16_t)VIEW_DECAY_DEFAULT_SECS;
    }
    out->classicMode    = row->classicMode;
    out->overviewWindow = row->overviewWindow;
    out->lineOfSight    = row->lineOfSight;
    out->alliesInTrees  = row->alliesInTrees;
    out->positionalSound = row->positionalSound;
}

bool visibilityPresetSettings(VisibilityPreset preset,
                              VisibilitySettings *out) {
    if (out == NULL) return false;
    if ((int)preset < 0 || (int)preset >= VISIBILITY_PRESET_COUNT) return false;
    rowToSettings(&kPresets[(int)preset], out);
    return true;
}

VisibilityPreset visibilityPresetMatch(const VisibilitySettings *v) {
    int p; /* Looping variable */

    if (v == NULL) return visibilityPresetCustom;
    for (p = 0; p < VISIBILITY_PRESET_COUNT; p++) {
        const VisibilityPresetRow *row = &kPresets[p];
        if (row->classicMode != v->classicMode) continue;
        if (row->overviewWindow != v->overviewWindow) continue;
        if (row->pill != v->policy[viewCategoryPill]) continue;
        if (row->base != v->policy[viewCategoryBase]) continue;
        if (row->ally != v->policy[viewCategoryAlly]) continue;
        if (row->lineOfSight != v->lineOfSight) continue;
        if (row->alliesInTrees != v->alliesInTrees) continue;
        if (row->positionalSound != v->positionalSound) continue;
        return row->preset;
    }
    return visibilityPresetCustom;
}

bool visibilitySettingsEqual(const VisibilitySettings *a,
                             const VisibilitySettings *b) {
    int i; /* Looping variable */

    if (a == NULL || b == NULL) return (a == b);
    for (i = 0; i < VIEW_CATEGORY_COUNT; i++) {
        if (a->policy[i] != b->policy[i]) return false;
        if (a->decaySecs[i] != b->decaySecs[i]) return false;
    }
    return (a->classicMode == b->classicMode &&
            a->overviewWindow == b->overviewWindow &&
            a->lineOfSight == b->lineOfSight &&
            a->alliesInTrees == b->alliesInTrees &&
            a->positionalSound == b->positionalSound);
}

int visibilityPresetNameId(VisibilityPreset preset) {
    if ((int)preset < 0 || (int)preset >= VISIBILITY_PRESET_COUNT) {
        return STR_DLGLOBBY_PRESET_CUSTOM;
    }
    return kPresets[(int)preset].nameId;
}

int visibilityPresetDescId(VisibilityPreset preset) {
    if ((int)preset < 0 || (int)preset >= VISIBILITY_PRESET_COUNT) {
        return STR_DLGLOBBY_PRESET_CUSTOM_DESC;
    }
    return kPresets[(int)preset].descId;
}

const char *visibilityPresetPrefWord(VisibilityPreset preset) {
    if ((int)preset < 0 || (int)preset >= VISIBILITY_PRESET_COUNT) {
        return "Custom";
    }
    return kPresets[(int)preset].prefWord;
}

VisibilityPreset visibilityPresetFromPrefWord(const char *word,
                                              VisibilityPreset fallback) {
    int p; /* Looping variable */

    if (word == NULL) return fallback;
    if (strcmp(word, "Custom") == 0) return visibilityPresetCustom;
    for (p = 0; p < VISIBILITY_PRESET_COUNT; p++) {
        if (strcmp(word, kPresets[p].prefWord) == 0) return kPresets[p].preset;
    }
    return fallback;
}
