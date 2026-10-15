/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_dock_default.h - the arrangement of the log viewer's sidebars that a
 * new user starts with, and that "Reset Window Positions into Dock" puts
 * back.
 *
 *  - the left sidebar, 209 points wide: the HUD panel, the Tank panel under
 *    it. Its unused part is clear, so the map shows below the Tank panel;
 *  - the right sidebar, 407 points wide: Game Information, the Scenario
 *    panel, Overview, Item Information, Events and Comments, top to bottom.
 *    Events takes what the others leave, now and whenever the window
 *    changes height;
 *  - the bottom sidebar, 161 points high: the Controls panel;
 *  - no top sidebar.
 *
 * Every length here is in window points and is the size of a panel's
 * content, not a place on the screen, so the arrangement is the same in any
 * window: the sidebars are fitted to the window (lv_dock_fit.h) and the
 * panel that takes the slack takes it. A panel that is hidden is left out
 * and gets no room.
 *
 * Header only and free of SDL and ImGui, so it can be tested on its own
 * (tests/unit/test_lv_dock_clear.c).
 */

#ifndef LV_DOCK_DEFAULT_H
#define LV_DOCK_DEFAULT_H

#include <stddef.h>

#include "lv_dock_fit.h"
#include "lv_dock_layout.h"
#include "lv_dock_clear.h"

/* One panel's place in a sidebar of the default arrangement. len is its
 * length along the sidebar; 0 says the panel's own (its height for the
 * sidebar's width, or a share of what is left). The anchor is the panel
 * that takes the slack. */
typedef struct {
    const char *id;
    int         len;
    int         anchor;
} LvDockDefaultSlot;

/* The panels of one sidebar, in order from its start. */
typedef struct {
    int                      count;
    const LvDockDefaultSlot *slot;
} LvDockDefaultStack;

static const LvDockDefaultSlot lvDockDefaultRight[] = {
    { "###gameinfo",   159, 0 },
    { "###lvscnpanel", 176, 0 },
    { "###overview",     0, 0 },
    { "###iteminfo",     0, 0 },
    { "###events",       0, 1 },
    { "###comments",   158, 0 },
};

static const LvDockDefaultSlot lvDockDefaultLeft[] = {
    { "###hud",  545, 1 },
    { "###tank", 223, 0 },
};

static const LvDockDefaultSlot lvDockDefaultBottom[] = {
    { "###controls", 0, 1 },
};

/* The panels the default arrangement puts in a sidebar. A sidebar it does
 * not use has none. */
static inline LvDockDefaultStack lvDockDefaultStackOf(int side) {
    LvDockDefaultStack s;
    s.count = 0;
    s.slot = NULL;
    switch (side) {
        case LV_DOCK_RIGHT:
            s.slot = lvDockDefaultRight;
            s.count = (int)(sizeof(lvDockDefaultRight) / sizeof(lvDockDefaultRight[0]));
            break;
        case LV_DOCK_LEFT:
            s.slot = lvDockDefaultLeft;
            s.count = (int)(sizeof(lvDockDefaultLeft) / sizeof(lvDockDefaultLeft[0]));
            break;
        case LV_DOCK_BOTTOM:
            s.slot = lvDockDefaultBottom;
            s.count = (int)(sizeof(lvDockDefaultBottom) / sizeof(lvDockDefaultBottom[0]));
            break;
        default:
            break;
    }
    return s;
}

/* Which sidebars are on and how big each is. The right sidebar counts as
 * laid out: it must not have every panel stacked in it the first time it
 * shows. */
static inline void lvDockDefaultSides(LvDockSides *s) {
    lvDockSidesDefault(s);
    s->enabled[LV_DOCK_RIGHT] = 1;
    s->size[LV_DOCK_RIGHT] = 407;
    s->enabled[LV_DOCK_LEFT] = 1;
    s->size[LV_DOCK_LEFT] = 209;
    s->enabled[LV_DOCK_BOTTOM] = 1;
    s->size[LV_DOCK_BOTTOM] = 161;
    s->rightLaidOut = 1;
}

/* Which sidebars have their unused part clear: the left one. */
static inline int lvDockDefaultClear(int side) {
    return side == LV_DOCK_LEFT;
}

/* The length of each panel in a sidebar whose unused part is clear, indexed
 * as lvDockClearPanels: the stack's lengths, 0 for the rest. */
static inline void lvDockDefaultLens(int side, int *lens) {
    LvDockDefaultStack s = lvDockDefaultStackOf(side);
    int i;
    for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) lens[i] = 0;
    if (!lvDockDefaultClear(side)) return;
    for (i = 0; i < s.count; i++) {
        int p = lvDockClearPanelIndex(s.slot[i].id);
        if (p >= 0) lens[p] = s.slot[i].len;
    }
}

/* ---- Sharing a sidebar out among the panels in it ---------------------------
 *
 * count panels one after the other along a sidebar extent long. fixed[i] is
 * a panel's own length, 0 for one that has none; weight[i] its share of
 * what the fixed ones leave. Each fixed panel gets its length and the others
 * share the rest by weight. When the fixed lengths do not fit, every panel
 * gets an equal part. The anchor's length is not used to split the sidebar
 * (it takes whatever is left) but is worked out the same way. */
static inline void lvDockStackLens(const float *fixed, const float *weight, int count,
                                   float extent, float *out) {
    float fixedSum = 0.0f, weightSum = 0.0f, spare;
    int i;
    if (count <= 0) return;
    for (i = 0; i < count; i++) {
        fixedSum += fixed[i] > 0.0f ? fixed[i] : 0.0f;
        weightSum += weight[i] > 0.0f ? weight[i] : 0.0f;
    }
    spare = extent - fixedSum;
    for (i = 0; i < count; i++) {
        if (spare > 0.0f && fixed[i] > 0.0f) {
            out[i] = fixed[i];
        } else if (spare > 0.0f && weightSum > 0.0f) {
            out[i] = spare * (weight[i] > 0.0f ? weight[i] : 0.0f) / weightSum;
        } else {
            out[i] = extent / (float)count;
        }
    }
}

#endif /* LV_DOCK_DEFAULT_H */
