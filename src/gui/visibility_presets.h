/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Visibility presets
 *Filename:      visibility_presets.h
 *Purpose:
 *  The five named visibility sets the lobby offers, and the
 *  match that reads a live set of settings back as one of
 *  them.
 *
 *  There is nothing on the wire for a preset. Every client
 *  already has all seven values, so each one works out the
 *  preset for itself and they all arrive at the same answer;
 *  picking one in the lobby just sends the values it stands
 *  for. That is also why the match has to be exact rather
 *  than nearest: two clients must never disagree about which
 *  row is ticked.
 *
 *  Decay seconds are outside the match. No preset uses the
 *  Decay policy, so the seconds are whatever the host last
 *  typed, and a preset leaves them where they are.
 *********************************************************/

#ifndef VISIBILITY_PRESETS_H
#define VISIBILITY_PRESETS_H

#include <stdbool.h>
#include <stdint.h>

#include "view_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every visibility value a lobby carries, in one place, so a preset, the
 * live settings and the remembered custom set are all the same thing and
 * can be compared with one call. */
typedef struct VisibilitySettings {
    uint8_t  policy[VIEW_CATEGORY_COUNT];    /* ViewPolicy per category */
    uint16_t decaySecs[VIEW_CATEGORY_COUNT]; /* only read under Decay */
    bool     classicMode;
    uint8_t  overviewWindow;                 /* OverviewWindow */
    uint8_t  lineOfSight;                    /* LineOfSightMode */
    bool     alliesInTrees;
} VisibilitySettings;

/* Ordered the way the lobby lists them, least sight to most, so the
 * dropdown and the details table both just count up. Custom is not a
 * preset — it is the answer the match gives when no preset fits — so it
 * sits past the count rather than inside it. */
typedef enum {
    visibilityPresetClassic         = 0,
    visibilityPresetClassicOverview = 1,
    visibilityPresetExpanded        = 2,
    visibilityPresetMaxView         = 3,
    visibilityPresetSight           = 4,
    VISIBILITY_PRESET_COUNT         = 5,
    visibilityPresetCustom          = 5
} VisibilityPreset;

/* Fills out with the preset's values. The decay seconds come back on
 * VIEW_DECAY_DEFAULT_SECS because a preset has nothing to say about
 * them; a caller applying one to a live lobby writes the lobby's own
 * seconds over them first. Returns false — and leaves out alone — for
 * Custom and for anything outside the enum. */
bool visibilityPresetSettings(VisibilityPreset preset,
                              VisibilitySettings *out);

/* The first preset whose values are exactly v, or visibilityPresetCustom
 * when none of them is. Decay seconds take no part: see the header note.
 * A NULL v reads as Custom. */
VisibilityPreset visibilityPresetMatch(const VisibilitySettings *v);

/* Whether two sets are the same edit-for-edit, decay seconds included.
 * This is the "has the remembered custom set changed" test, which is a
 * different question from the match above and so counts the seconds. */
bool visibilitySettingsEqual(const VisibilitySettings *a,
                             const VisibilitySettings *b);

/* Lang ids for the row's name and the sentence under it. Custom has a
 * name and a description too — its row carries the controls instead of a
 * value, and the description says so. An id outside the enum comes back
 * as the Custom row's, so a caller can never hand langGetText a zero. */
int visibilityPresetNameId(VisibilityPreset preset);
int visibilityPresetDescId(VisibilityPreset preset);

/* The word the [GAME OPTIONS] INI stores the preset under, and the read
 * back. One token per preset so a hand-edited INI reads clearly, the same
 * as the Pill View / Overview Window words next to it. A word that is
 * none of them returns the caller's fallback. */
const char      *visibilityPresetPrefWord(VisibilityPreset preset);
VisibilityPreset visibilityPresetFromPrefWord(const char *word,
                                              VisibilityPreset fallback);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* VISIBILITY_PRESETS_H */
