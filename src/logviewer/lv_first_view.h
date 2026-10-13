/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_first_view.h - where the log viewer's map is when a log is first shown.
 *
 * Most of a map is deep sea. The part worth a first look is the box round
 * everything that is not: each square of land, river, road or building, and
 * each pillbox, base and start. The first view puts the middle of that box
 * in the middle of the map that shows (lvViewCentreScrollPx in
 * lv_event_flash.h does that sum), and zooms out when the box does not go in
 * at the zoom the viewer has.
 *
 * Header only, with no includes but the C library's, so the viewer and the
 * unit tests use the same sums.
 */

#ifndef LV_FIRST_VIEW_H
#define LV_FIRST_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

/* How many squares a map is, each way. */
#define LV_LAND_MAP_SIZE 256

/* The room kept round the box when a zoom is chosen for it, in squares on
   each side. */
#define LV_FIRST_VIEW_MARGIN 1

/* The box, in squares, both ends in it. has is 0 while nothing is in it. */
typedef struct LvLandBox {
    int has;
    int x0, y0, x1, y1;
} LvLandBox;

static inline void lvLandBoxClear(LvLandBox *box) {
    box->has = 0;
    box->x0 = box->y0 = box->x1 = box->y1 = 0;
}

/* Take one square in. A square that is not on the map is passed over. */
static inline void lvLandBoxAdd(LvLandBox *box, int x, int y) {
    if (x < 0 || y < 0 || x >= LV_LAND_MAP_SIZE || y >= LV_LAND_MAP_SIZE) return;
    if (!box->has) {
        box->has = 1;
        box->x0 = box->x1 = x;
        box->y0 = box->y1 = y;
        return;
    }
    if (x < box->x0) box->x0 = x;
    if (x > box->x1) box->x1 = x;
    if (y < box->y0) box->y0 = y;
    if (y > box->y1) box->y1 = y;
}

/* Take in every square of a map that is not deep sea. terrain holds the map
   as the game keeps it: LV_LAND_MAP_SIZE columns of LV_LAND_MAP_SIZE squares,
   the square x, y at terrain[x * LV_LAND_MAP_SIZE + y]. deepSea is the value
   of a deep sea square. The mined border of a map is deep sea, so it is
   never taken in; a mined square of land is land. */
static inline void lvLandBoxAddTerrain(LvLandBox *box, const unsigned char *terrain,
                                       unsigned char deepSea) {
    int x, y;
    if (terrain == 0) return;
    for (x = 0; x < LV_LAND_MAP_SIZE; x++) {
        const unsigned char *col = terrain + x * LV_LAND_MAP_SIZE;
        for (y = 0; y < LV_LAND_MAP_SIZE; y++) {
            if (col[y] != deepSea) lvLandBoxAdd(box, x, y);
        }
    }
}

/* How many squares the box is across and down; 0 for an empty one. */
static inline int lvLandBoxWidth(const LvLandBox *box) {
    return box->has ? box->x1 - box->x0 + 1 : 0;
}

static inline int lvLandBoxHeight(const LvLandBox *box) {
    return box->has ? box->y1 - box->y0 + 1 : 0;
}

/* The middle of the squares lo..hi, in map pixels at zoom 1. With an even
   number of squares it is the line between two of them. */
static inline float lvLandBoxMidPx(int lo, int hi, int tilePx) {
    return (float)(lo + hi + 1) * (float)tilePx * 0.5f;
}

/* 1 when `squares` squares, with `margin` more on each side, go in visLenPt
   points of window at this zoom. */
static inline int lvFirstViewFits(int squares, int margin, int tilePx, float zoom,
                                  float visLenPt) {
    if (zoom <= 0.0f) zoom = 1.0f;
    return (float)((squares + 2 * margin) * tilePx) * zoom <= visLenPt;
}

/* The zoom step the first view of a log has. steps holds the viewer's zoom
   steps, smallest first, and current is the one it has now. boxW and boxH
   are the size of the box in squares; visWPt and visHPt the size of the map
   that shows, in points.

   The zoom is kept when the whole box goes in at it. Otherwise the answer is
   the largest step under it at which the box and LV_FIRST_VIEW_MARGIN
   squares round it go in; when there is none, the smallest step. The zoom
   never goes up, and no step is made that the viewer does not have. */
static inline int lvFirstViewZoomStep(const float *steps, int count, int current,
                                      int boxW, int boxH, float visWPt, float visHPt,
                                      int tilePx) {
    int s;
    if (steps == 0 || count <= 0) return 0;
    if (current < 0) current = 0;
    if (current >= count) current = count - 1;
    if (boxW <= 0 || boxH <= 0) return current;
    if (lvFirstViewFits(boxW, 0, tilePx, steps[current], visWPt) &&
        lvFirstViewFits(boxH, 0, tilePx, steps[current], visHPt)) {
        return current;
    }
    for (s = current - 1; s >= 0; s--) {
        if (lvFirstViewFits(boxW, LV_FIRST_VIEW_MARGIN, tilePx, steps[s], visWPt) &&
            lvFirstViewFits(boxH, LV_FIRST_VIEW_MARGIN, tilePx, steps[s], visHPt)) {
            return s;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* LV_FIRST_VIEW_H */
