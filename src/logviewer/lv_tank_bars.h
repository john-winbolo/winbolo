/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer tank bars sums
 *Filename:      lv_tank_bars.h
 *Purpose:
 *  The two small bars the Log Viewer can draw over each tank
 *  on the map (Options > Tank Bars; draw.c draws them): the
 *  tank's armour on top and its shells under it.
 *
 *  This is the arithmetic: where the two bars go for a tank
 *  drawn at a place on the screen with squares of a size, how
 *  much of a bar is filled, and the colour of the fill.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_TANK_BARS_H
#define LV_TANK_BARS_H

#include <math.h>
#include <stdbool.h>

/* Sizes here are in points, not in framebuffer pixels. On a display with a
 * pixel density of 2 (a Retina Mac) a point is two pixels, so each size is
 * multiplied by the density before it is drawn: a bar is as thick to the eye
 * on every display. The width is the exception. It is one map square, and
 * the square's size on the screen already holds the density.
 *
 * Under this many points to a map square no bars are drawn: a bar would be
 * too short to read, and the zoomed-out view has enough on it. The viewer's
 * zoom steps give squares of 8, 9.6, 11.2, 12.8, 14.4, 16, 32, 48 and 64
 * points, so the bars come in at 0.8x, on every display. */
#define LV_TANK_BARS_MIN_TILE 12.0f

/* A bar is an eighth of a square high, but never under 2 points (it would
 * not be seen) nor over 5 (it would hide the map at a large zoom). */
#define LV_TANK_BARS_MIN_H 2.0f
#define LV_TANK_BARS_MAX_H 5.0f

/* The space between the two bars, and between the lower one and the top of
 * the tank, in points. */
#define LV_TANK_BARS_GAP 1.0f

/* "25% transparent": the fill and the empty part are both drawn 75% solid. */
#define LV_TANK_BARS_ALPHA 0.75f

/* The colours. The empty part of a bar is near black. Armour goes from
 * green through yellow to red as it runs out; shells are one light blue, so
 * the two bars cannot be taken for each other. */
#define LV_TANK_BARS_TRACK_R 24
#define LV_TANK_BARS_TRACK_G 24
#define LV_TANK_BARS_TRACK_B 24
#define LV_TANK_BARS_SHELLS_R 110
#define LV_TANK_BARS_SHELLS_G 200
#define LV_TANK_BARS_SHELLS_B 255

/* Armour is "high" (green) over half, "low" (red) at a quarter or under,
 * and "medium" (yellow) between. */
#define LV_TANK_BARS_ARMOUR_HIGH 0.5f
#define LV_TANK_BARS_ARMOUR_LOW  0.25f

typedef struct LvTankBarRect {
    float x, y, w, h;
} LvTankBarRect;

typedef struct LvTankBarsLayout {
    bool visible;           /* false: the squares are too small, draw none */
    LvTankBarRect armour;   /* the upper bar */
    LvTankBarRect shells;   /* the lower bar */
} LvTankBarsLayout;

/* A length in points as whole framebuffer pixels, never under one. */
static inline float lvTankBarPointsToPixels(float points, float pxScale) {
    float px;
    if (pxScale <= 0.0f) pxScale = 1.0f;
    px = floorf(points * pxScale + 0.5f);
    return (px < 1.0f) ? 1.0f : px;
}

/* The height of one bar, in framebuffer pixels, for squares tilePx
 * framebuffer pixels across on a display of pixel density pxScale. */
static inline float lvTankBarThickness(float tilePx, float pxScale) {
    float t;
    if (pxScale <= 0.0f) pxScale = 1.0f;
    t = floorf(tilePx / pxScale / 8.0f);
    if (t < LV_TANK_BARS_MIN_H) t = LV_TANK_BARS_MIN_H;
    if (t > LV_TANK_BARS_MAX_H) t = LV_TANK_BARS_MAX_H;
    return lvTankBarPointsToPixels(t, pxScale);
}

/* Whether squares tilePx framebuffer pixels across are large enough for
 * bars on a display of pixel density pxScale. */
static inline bool lvTankBarsVisible(float tilePx, float pxScale) {
    if (pxScale <= 0.0f) pxScale = 1.0f;
    return tilePx >= LV_TANK_BARS_MIN_TILE * pxScale;
}

/* Where the two bars go. tankLeft and tankTop are the top left of the tank's
 * picture in the framebuffer, in pixels, tilePx is the size of a map square
 * (and of the picture) there, and pxScale is the display's pixel density (1
 * on most, 2 on a Retina Mac). Both bars are exactly as wide as one square,
 * start at the picture's left edge and sit on whole pixels: the shells bar
 * one gap above the picture, the armour bar one gap above that. */
static inline LvTankBarsLayout lvTankBarsLayout(float tankLeft, float tankTop,
                                                float tilePx, float pxScale) {
    LvTankBarsLayout out;
    float t = lvTankBarThickness(tilePx, pxScale);
    float gap = lvTankBarPointsToPixels(LV_TANK_BARS_GAP, pxScale);
    float x = floorf(tankLeft);
    float top = floorf(tankTop);
    float w = floorf(tilePx + 0.5f);

    out.visible = lvTankBarsVisible(tilePx, pxScale);
    out.shells.x = x;
    out.shells.w = w;
    out.shells.h = t;
    out.shells.y = top - gap - t;
    out.armour.x = x;
    out.armour.w = w;
    out.armour.h = t;
    out.armour.y = out.shells.y - gap - t;
    return out;
}

/* How full a bar is, 0..1: what the tank holds over the most it can hold
 * (the cap out of the recorded game's rules). A cap of 0 is empty. */
static inline float lvTankBarFraction(int value, int full) {
    if (full <= 0 || value <= 0) return 0.0f;
    if (value >= full) return 1.0f;
    return (float)value / (float)full;
}

/* The filled part of a bar barW pixels wide, in whole pixels. A tank that
 * holds anything shows at least one pixel, and only a full tank fills the
 * whole bar. */
static inline float lvTankBarFillWidth(float barW, int value, int full) {
    float frac = lvTankBarFraction(value, full);
    float w;
    if (frac <= 0.0f || barW <= 0.0f) return 0.0f;
    if (frac >= 1.0f) return barW;
    w = floorf(barW * frac + 0.5f);
    if (w < 1.0f) w = 1.0f;
    if (w > barW - 1.0f && barW > 1.0f) w = barW - 1.0f;
    return w;
}

/* The colour of the armour bar's fill for how full it is. */
static inline void lvTankBarArmourColour(float frac, unsigned char *r,
                                         unsigned char *g, unsigned char *b) {
    if (frac > LV_TANK_BARS_ARMOUR_HIGH) {
        *r = 60;  *g = 220; *b = 60;    /* green */
    } else if (frac > LV_TANK_BARS_ARMOUR_LOW) {
        *r = 240; *g = 210; *b = 40;    /* yellow */
    } else {
        *r = 235; *g = 50;  *b = 50;    /* red */
    }
}

#endif /* LV_TANK_BARS_H */
