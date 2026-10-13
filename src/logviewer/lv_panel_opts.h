/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer panel options sums
 *Filename:      lv_panel_opts.h
 *Purpose:
 *  Every panel of the Log Viewer has a small gear button
 *  that opens the panel's options (imgui_lv_panel.cpp). This
 *  is the arithmetic and the names behind it:
 *
 *   - the name of a panel's option in the preferences;
 *   - where the gear goes: in the title bar beside the close
 *     button, or in the bottom right corner of the panel
 *     when there is no title bar to put it in;
 *   - for a panel whose unused parts are transparent, which
 *     parts are used: the boxes of everything the panel drew
 *     are gathered into rows and the rows into blocks, and a
 *     backing is drawn behind each block so the text can be
 *     read over the map.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_PANEL_OPTS_H
#define LV_PANEL_OPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The panels that have options: the part of the window's title that makes
 * its ID, and the panel's part of its preference names (the same part the
 * "Window.<Panel>.Visible" names have). */
typedef struct LvPanelOptName {
    const char *id;
    const char *key;
} LvPanelOptName;

#define LV_PANEL_OPT_COUNT 9

static const LvPanelOptName lvPanelOptNames[LV_PANEL_OPT_COUNT] = {
    { "###controls",   "Controls" },
    { "###hud",        "HUD" },
    { "###overview",   "Overview" },
    { "###tank",       "Tank" },
    { "###gameinfo",   "GameInformation" },
    { "###lvscnpanel", "ScenarioPanel" },
    { "###iteminfo",   "ItemInformation" },
    { "###events",     "Events" },
    { "###comments",   "Comments" },
};

/* The index of a panel in lvPanelOptNames, -1 for a window that is not one
 * of the panels. */
static inline int lvPanelOptIndex(const char *id) {
    int i;
    if (id == NULL) {
        return -1;
    }
    for (i = 0; i < LV_PANEL_OPT_COUNT; i++) {
        if (strcmp(lvPanelOptNames[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* The preference name of a panel's "unused parts are transparent" option:
 * "Window.<Panel>.Transparent". Returns false, with an empty name, for an
 * index that is no panel or a buffer too small. */
static inline bool lvPanelOptKey(int index, char *out, size_t size) {
    int n;
    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';
    if (index < 0 || index >= LV_PANEL_OPT_COUNT) {
        return false;
    }
    n = snprintf(out, size, "Window.%s.Transparent", lvPanelOptNames[index].key);
    if (n < 0 || (size_t)n >= size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* ---- where the gear goes ---- */

typedef struct LvPanelGear {
    float x, y; /* the top left of the gear's square */
    float side; /* the length of the square */
} LvPanelGear;

/* The gear in a title bar that starts at (barMinX, barMinY) and ends at
 * barMaxX. ImGui puts the close button a frame padding in from the right
 * end, as tall as the font; the gear is the same size, one inner spacing to
 * its left, or in its place when there is no close button. border is the
 * width of the border the bar is drawn inside (the tab bar of a dock node;
 * 0 for a window's own title bar). In a bar too narrow the gear stays inside
 * the bar's left end. */
static inline LvPanelGear lvPanelGearInBar(float barMinX, float barMinY, float barMaxX,
                                           float fontSize, float padX, float padY,
                                           float innerSpacing, float border, bool hasClose) {
    LvPanelGear g;
    float right = barMaxX - border - padX;
    if (hasClose) {
        right -= fontSize + innerSpacing;
    }
    g.side = fontSize;
    g.x = right - fontSize;
    if (g.x < barMinX + border) {
        g.x = barMinX + border;
    }
    g.y = barMinY + padY;
    return g;
}

/* The gear of a panel with no title bar to put it in: the bottom right
 * corner of the panel's inside (innerMinX..innerMaxX, down to innerMaxY),
 * margin in from both edges. The bottom, because the top right corner is
 * where the Tank and HUD panels have the X of their name line; a panel with
 * no title bar is docked, and so has no resize grip in that corner. In a
 * panel too small the gear stays inside the panel's top left. */
static inline LvPanelGear lvPanelGearInCorner(float innerMinX, float innerMinY, float innerMaxX,
                                              float innerMaxY, float fontSize, float margin) {
    LvPanelGear g;
    g.side = fontSize;
    g.x = innerMaxX - margin - fontSize;
    if (g.x < innerMinX) {
        g.x = innerMinX;
    }
    g.y = innerMaxY - margin - fontSize;
    if (g.y < innerMinY) {
        g.y = innerMinY;
    }
    return g;
}

/* ---- which parts of a panel are used ---- */

/* The panel's height is cut into rows; each row keeps the leftmost and the
 * rightmost point anything was drawn at. */
#define LV_BACKING_ROWS 1024
#define LV_BACKING_ROW_H 2.0f

typedef struct LvBacking {
    float top;  /* the y of row 0 */
    float rowH; /* the height of a row */
    int   rows;
    float minX[LV_BACKING_ROWS];
    float maxX[LV_BACKING_ROWS];
} LvBacking;

typedef struct LvBackingRect {
    float x0, y0, x1, y1;
} LvBackingRect;

/* Start over for a panel whose inside is from top down for height. Rows are
 * 2 high; a panel taller than 1024 rows of that has taller rows. */
static inline void lvBackingReset(LvBacking *b, float top, float height) {
    int i;
    b->top = top;
    b->rowH = LV_BACKING_ROW_H;
    if (height < 0.0f) {
        height = 0.0f;
    }
    b->rows = (int)(height / b->rowH);
    if ((float)b->rows * b->rowH < height) {
        b->rows++;
    }
    if (b->rows > LV_BACKING_ROWS) {
        b->rows = LV_BACKING_ROWS;
        b->rowH = height / (float)LV_BACKING_ROWS;
    }
    for (i = 0; i < b->rows; i++) {
        b->minX[i] = 1.0e30f;
        b->maxX[i] = -1.0e30f;
    }
}

/* Something was drawn in the box x0..x1 by y0..y1. A box with no area, or
 * outside the panel, adds nothing. */
static inline void lvBackingAdd(LvBacking *b, float x0, float y0, float x1, float y1) {
    int r0, r1, r;
    if (b->rows <= 0 || !(x1 > x0) || !(y1 > y0)) {
        return;
    }
    if (y1 <= b->top || y0 >= b->top + (float)b->rows * b->rowH) {
        return;
    }
    r0 = (int)((y0 - b->top) / b->rowH);
    if (y0 < b->top) {
        r0 = 0;
    }
    /* The last row the box reaches into: a box that ends exactly on a row's
     * top edge does not touch that row. */
    r1 = (int)((y1 - b->top) / b->rowH);
    if ((float)r1 * b->rowH >= y1 - b->top) {
        r1--;
    }
    if (r1 >= b->rows) {
        r1 = b->rows - 1;
    }
    for (r = r0; r <= r1; r++) {
        if (x0 < b->minX[r]) {
            b->minX[r] = x0;
        }
        if (x1 > b->maxX[r]) {
            b->maxX[r] = x1;
        }
    }
}

/* The blocks: rows with something in them that are no more than 2 * pad
 * apart are one block, as wide as its widest row, with pad added on every
 * side. Up to max blocks are written to out, from the top; when there are
 * more, the last one takes all that is left. Returns how many. */
static inline int lvBackingBlocks(const LvBacking *b, float pad, LvBackingRect *out, int max) {
    int   count = 0;
    int   first = -1, last = -1;
    int   gapRows;
    int   r;
    float lo = 0.0f, hi = 0.0f;

    if (out == NULL || max <= 0 || b->rows <= 0) {
        return 0;
    }
    if (pad < 0.0f) {
        pad = 0.0f;
    }
    gapRows = (int)((2.0f * pad) / b->rowH);

    for (r = 0; r <= b->rows; r++) {
        bool used = r < b->rows && b->maxX[r] > b->minX[r];
        if (used) {
            if (first < 0) {
                first = r;
                lo = b->minX[r];
                hi = b->maxX[r];
            } else {
                if (b->minX[r] < lo) {
                    lo = b->minX[r];
                }
                if (b->maxX[r] > hi) {
                    hi = b->maxX[r];
                }
            }
            last = r;
            continue;
        }
        if (first < 0) {
            continue;
        }
        /* An empty row: the block goes on if it is the last block there is
         * room for, or the gap is still small. */
        if (r < b->rows && (count == max - 1 || r - last <= gapRows)) {
            continue;
        }
        out[count].x0 = lo - pad;
        out[count].x1 = hi + pad;
        out[count].y0 = b->top + (float)first * b->rowH - pad;
        out[count].y1 = b->top + (float)(last + 1) * b->rowH + pad;
        count++;
        first = -1;
        if (count >= max) {
            break;
        }
    }
    return count;
}

#endif /* LV_PANEL_OPTS_H */
