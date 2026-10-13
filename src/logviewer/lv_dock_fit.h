/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_dock_fit.h - how the log viewer's window is shared between the map and
 * the sidebars (the docks down the right and left and across the top and
 * bottom).
 *
 * Header only and free of SDL and ImGui, so the sums can be tested on their
 * own (tests/unit/test_lv_dock_fit.c). Every length is in window points, the
 * unit SDL_GetWindowSize reports and ImGui lays out in.
 */

#ifndef LV_DOCK_FIT_H
#define LV_DOCK_FIT_H

/* The narrowest the dock gets, and the width it has before the user drags its
 * edge. The default leaves the Controls panel's button row room on one line. */
#define LV_DOCK_WIDTH_MIN     220
#define LV_DOCK_WIDTH_DEFAULT 420

/* The viewport is at least one map square across and at most the whole map. */
#define LV_FIT_TILES_MIN 1
#define LV_FIT_TILES_MAX 255

static inline int lvFitClampTiles(int tiles) {
    if (tiles < LV_FIT_TILES_MIN) tiles = LV_FIT_TILES_MIN;
    if (tiles > LV_FIT_TILES_MAX) tiles = LV_FIT_TILES_MAX;
    return tiles;
}

/* Squares across an area areaPx wide at tilePx a square, to the nearest whole
 * square. This is the sum the window sizing has always used: the window is
 * then snapped to that many squares, so nearest moves it the least. */
static inline int lvFitTilesNearest(int areaPx, float tilePx) {
    if (tilePx <= 0.0f) tilePx = 1.0f;
    if (areaPx < 0) areaPx = 0;
    return lvFitClampTiles((int)(((float)areaPx + tilePx * 0.5f) / tilePx));
}

/* Squares needed to reach the far edge of the area: a last part square counts
 * as a whole one. Used beside the dock, where the window is not snapped to the
 * map: a map that stops short would leave a black strip against the dock, and
 * one that runs on puts less than a square of it under the dock's edge. */
static inline int lvFitTilesCover(int areaPx, float tilePx) {
    int tiles;
    if (tilePx <= 0.0f) tilePx = 1.0f;
    if (areaPx < 0) areaPx = 0;
    tiles = (int)((float)areaPx / tilePx);
    if ((float)tiles * tilePx < (float)areaPx) tiles++;
    return lvFitClampTiles(tiles);
}

/* The width the dock takes of a window windowW wide when the user asked for
 * wanted. It never takes more than two thirds of the window, so some map is
 * always left, and never less than its smallest width unless the window is
 * narrower than that. */
static inline int lvDockClampWidth(int wanted, int windowW) {
    int maxW;
    if (windowW < 1) return 0;
    maxW = (windowW * 2) / 3;
    if (maxW < LV_DOCK_WIDTH_MIN) maxW = LV_DOCK_WIDTH_MIN;
    if (maxW > windowW) maxW = windowW;
    if (wanted < LV_DOCK_WIDTH_MIN) wanted = LV_DOCK_WIDTH_MIN;
    if (wanted > maxW) wanted = maxW;
    return wanted;
}

/* The four sidebars. The right one is first because it was the only one for
 * a time, and its saved settings are still read. */
enum {
    LV_DOCK_RIGHT = 0,
    LV_DOCK_LEFT,
    LV_DOCK_TOP,
    LV_DOCK_BOTTOM,
    LV_DOCK_SIDES
};

/* The top and bottom sidebars: the least height (a tab and a row or two) and
 * the height each has before the user drags its edge. */
#define LV_DOCK_HEIGHT_MIN     100
#define LV_DOCK_HEIGHT_DEFAULT 220

/* True for the sidebars whose size is a width. */
static inline int lvDockSideIsWide(int side) {
    return side == LV_DOCK_RIGHT || side == LV_DOCK_LEFT;
}

static inline int lvDockSideMin(int side) {
    return lvDockSideIsWide(side) ? LV_DOCK_WIDTH_MIN : LV_DOCK_HEIGHT_MIN;
}

static inline int lvDockSideDefault(int side) {
    return lvDockSideIsWide(side) ? LV_DOCK_WIDTH_DEFAULT : LV_DOCK_HEIGHT_DEFAULT;
}

/* lvDockClampWidth for the top and bottom sidebars: areaH is the window's
 * height under the menu bar. */
static inline int lvDockClampHeight(int wanted, int areaH) {
    int maxH;
    if (areaH < 1) return 0;
    maxH = (areaH * 2) / 3;
    if (maxH < LV_DOCK_HEIGHT_MIN) maxH = LV_DOCK_HEIGHT_MIN;
    if (maxH > areaH) maxH = areaH;
    if (wanted < LV_DOCK_HEIGHT_MIN) wanted = LV_DOCK_HEIGHT_MIN;
    if (wanted > maxH) wanted = maxH;
    return wanted;
}

/* Two sidebars that face each other share one length of the window. Each has
 * been clamped on its own; together they still take no more than two thirds
 * of that length, so a third is always the map's. When they ask for more,
 * each gives up in proportion to its size, which in a small window takes
 * them under their smallest size rather than leaving no map. */
static inline void lvDockFitPair(int *a, int *b, int extent) {
    int budget, sum;
    if (extent < 0) extent = 0;
    budget = (extent * 2) / 3;
    sum = *a + *b;
    if (*a <= 0 || *b <= 0 || sum <= budget) return;
    *a = (int)(((long long)*a * budget) / sum);
    *b = budget - *a;
}

/* The part of the window each sidebar takes, in window points. */
typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} LvDockMargins;

/* The margins for a window windowW wide and areaH high under its menu bar.
 * wanted and showing are indexed by LV_DOCK_*; a sidebar that is not showing
 * takes nothing.
 *
 * The corners: the left and right sidebars run the full height under the
 * menu bar, and the top and bottom ones sit between them. So left and right
 * are fitted to the window's width, top and bottom to the height, and
 * neither pair changes the other's size. */
static inline LvDockMargins lvDockFitMargins(const int *wanted, const int *showing,
                                             int windowW, int areaH) {
    LvDockMargins m;
    m.right  = showing[LV_DOCK_RIGHT]  ? lvDockClampWidth(wanted[LV_DOCK_RIGHT], windowW) : 0;
    m.left   = showing[LV_DOCK_LEFT]   ? lvDockClampWidth(wanted[LV_DOCK_LEFT], windowW) : 0;
    m.top    = showing[LV_DOCK_TOP]    ? lvDockClampHeight(wanted[LV_DOCK_TOP], areaH) : 0;
    m.bottom = showing[LV_DOCK_BOTTOM] ? lvDockClampHeight(wanted[LV_DOCK_BOTTOM], areaH) : 0;
    lvDockFitPair(&m.left, &m.right, windowW);
    lvDockFitPair(&m.top, &m.bottom, areaH);
    return m;
}

/* The size a sidebar gets when the user drags its edge to ask for wanted,
 * with other the size of the sidebar facing it (0 when that one is off) and
 * extent the length they share. Held between the smallest size and what the
 * facing sidebar leaves of the two thirds, so the dragged edge stops where
 * the fitted margin would stop and the facing sidebar does not move. */
static inline int lvDockDragSize(int side, int wanted, int other, int extent) {
    int lo = lvDockSideMin(side);
    int hi;
    if (extent < 1) return lo;
    hi = (extent * 2) / 3 - (other > 0 ? other : 0);
    if (wanted > hi) wanted = hi;
    if (wanted < lo) wanted = lo;
    return wanted;
}

/* The map's part of the window: what the margins leave under the menu bar.
 * Never less than one point each way, so the sums that divide by it hold. */
static inline void lvDockMapRect(const LvDockMargins *m, int windowW, int windowH,
                                 int menuH, int *x, int *y, int *w, int *h) {
    int mw = windowW - m->left - m->right;
    int mh = windowH - menuH - m->top - m->bottom;
    if (mw < 1) mw = 1;
    if (mh < 1) mh = 1;
    if (x) *x = m->left;
    if (y) *y = menuH + m->top;
    if (w) *w = mw;
    if (h) *h = mh;
}

/* True when the window point (px, py) is on the map and not on a sidebar or
 * the menu bar, even where the map's last square runs a little way under a
 * sidebar's edge. */
static inline int lvDockPointOnMap(const LvDockMargins *m, int windowW, int windowH,
                                   int menuH, float px, float py) {
    if (px < (float)m->left || px >= (float)(windowW - m->right)) return 0;
    if (py < (float)(menuH + m->top) || py >= (float)(windowH - m->bottom)) return 0;
    return 1;
}

/* Squares across the map's part of a window windowW wide with reservedRight
 * points kept for the dock. With nothing reserved this is the nearest-square
 * sum, unchanged. The same sum fits the squares down, given the height under
 * the menu bar and the points the top and bottom sidebars keep. */
static inline int lvFitTilesBesideDock(int windowW, int reservedRight,
                                       float tilePx) {
    if (reservedRight <= 0) return lvFitTilesNearest(windowW, tilePx);
    return lvFitTilesCover(windowW - reservedRight, tilePx);
}

#endif /* LV_DOCK_FIT_H */
