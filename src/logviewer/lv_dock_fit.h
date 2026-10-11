/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_dock_fit.h - how the log viewer's window is shared between the map and
 * the right-side dock.
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

/* Squares across the map's part of a window windowW wide with reservedRight
 * points kept for the dock. With nothing reserved this is the nearest-square
 * sum, unchanged. */
static inline int lvFitTilesBesideDock(int windowW, int reservedRight,
                                       float tilePx) {
    if (reservedRight <= 0) return lvFitTilesNearest(windowW, tilePx);
    return lvFitTilesCover(windowW - reservedRight, tilePx);
}

#endif /* LV_DOCK_FIT_H */
