/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer near base sums
 *Filename:      lv_base_near.h
 *Purpose:
 *  The base the Tank panel (imgui/imgui_tank.cpp) shows under
 *  the selected tank's gauges, and how long each of its three
 *  bars is drawn.
 *
 *  The pick is the game's own (basesGetClosestForPlayer in
 *  bolo/bases.c, which the client's status panel calls every
 *  tick through basesGetClosest): of the bases on the map that
 *  are neutral or allied to the player, the nearest one whose
 *  middle is strictly nearer to the tank than the rule
 *  base_status_range (BASE_STATUS_RANGE, 1792 world units, 7
 *  map squares, in a game that does not change it). The
 *  distance is a straight line, and of two at the same
 *  distance the one with the lower number wins. The viewer has
 *  no game sim to call it on, so the same sum is here.
 *
 *  The bars are the game's (sdl3RenderBaseBarsTex): across,
 *  BAR_BASE_FULL_PIXELS for a base that holds its cap and in
 *  proportion under that.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer (C and C++) and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_BASE_NEAR_H
#define LV_BASE_NEAR_H

#include <math.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No base is near enough. */
#define LV_BASE_NEAR_NONE (-1)

/* The world units of one map square, and the middle of one (MAP_SQUARE_MIDDLE). */
#define LV_BASE_NEAR_SQUARE 256
#define LV_BASE_NEAR_MIDDLE 128

/* One base slot, as the pick needs it. */
typedef struct LvBaseNearItem {
    bool active;    /* the base is on the map */
    bool friendly;  /* neutral, or its owner is allied to the player */
    int  mapX, mapY;
} LvBaseNearItem;

/* The index (0 up) of the base the game shows for a tank at the world place
 * tankX, tankY, or LV_BASE_NEAR_NONE. range is base_status_range. */
static inline int lvBaseNearPick(const LvBaseNearItem *items, int count, long tankX,
                                 long tankY, long range) {
    double best = (double)range;
    int pick = LV_BASE_NEAR_NONE;
    int i;
    for (i = 0; i < count; i++) {
        long x, y, gapX, gapY;
        double d;
        if (!items[i].active || !items[i].friendly) {
            continue;
        }
        x = (long)items[i].mapX * LV_BASE_NEAR_SQUARE + LV_BASE_NEAR_MIDDLE;
        y = (long)items[i].mapY * LV_BASE_NEAR_SQUARE + LV_BASE_NEAR_MIDDLE;
        gapX = (tankX > x) ? (tankX - x) : (x - tankX);
        gapY = (tankY > y) ? (tankY - y) : (y - tankY);
        d = sqrt((double)(gapX * gapX + gapY * gapY));
        if (d < best) {
            best = d;
            pick = i;
        }
    }
    return pick;
}

/* The length of a bar in the game's pixels, as the game works it out: a cap
 * of 0 cannot be divided by and draws empty. A value over its cap is cut off
 * at maxPixels, the end of the bar's well, where the game's drawing is cut
 * off too. */
static inline float lvBaseBarWidth(int value, int full, int fullPixels, int maxPixels) {
    float w;
    if (full <= 0 || value <= 0) return 0.0f;
    w = (float)fullPixels / (float)full * (float)value;
    if (w > (float)maxPixels) w = (float)maxPixels;
    return w;
}

#ifdef __cplusplus
}
#endif

#endif /* LV_BASE_NEAR_H */
