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
 * Name:          overview_fog.cpp
 * Purpose:       Implementation of the overview's fog mask —
 *                see overview_fog.h. Arithmetic only: no SDL,
 *                and nothing kept between calls.
 *********************************************************/

#include <cmath>
#include <cstring>

#include "overview_fog.h"

/* A square index brought onto the map. */
static int overviewFogClampSquare(int v) {
    if (v < 0) return 0;
    if (v > MAP_ARRAY_SIZE - 1) return MAP_ARRAY_SIZE - 1;
    return v;
}

/* Fog carried at `d` squares from a region, 0 at the region's edge and full
 * fog at OVERVIEW_FOG_RAMP. Smoothstepped rather than linear so the ramp has
 * no visible kink where it flattens out into the fog beyond it. */
static BYTE overviewFogRampValue(float d) {
    float t = d / (float)OVERVIEW_FOG_RAMP;
    float s = t * t * (3.0f - 2.0f * t);
    return (BYTE)lroundf((float)OVERVIEW_FOG_ALPHA * s);
}

void overviewFogBuildMask(const OverviewRect *live, int liveCount,
                          const BYTE *lift, const BYTE *dark, BYTE *mask) {
    int i; /* Looping variable */

    if (mask == NULL) return;

    /* Everything the walk below does not reach is beyond every region's ramp
     * and so carries full fog. */
    memset(mask, OVERVIEW_FOG_ALPHA, (size_t)OVERVIEW_FOG_MASK_BYTES);

    /* No list is no regions rather than nothing to do: a per-square lift with
     * no region to go with it is a map that is all afterimage. */
    if (live == NULL) liveCount = 0;

    for (i = 0; i < liveCount; i++) {
        const OverviewRect *rect = &live[i];

        /* A region faded right out lifts no square out of the fog, so the
         * walk below could only ever write back what is already there. */
        if (rect->alpha == 0) continue;

        /* The region plus its ramp, trimmed to the map. Squares off the map
         * are not drawn and the texture clamps at its edge, so a region that
         * runs into the map border simply keeps its brightness to the border
         * rather than fading against nothing. */
        int left   = overviewFogClampSquare(rect->left   - OVERVIEW_FOG_RAMP);
        int top    = overviewFogClampSquare(rect->top    - OVERVIEW_FOG_RAMP);
        int right  = overviewFogClampSquare(rect->right  + OVERVIEW_FOG_RAMP);
        int bottom = overviewFogClampSquare(rect->bottom + OVERVIEW_FOG_RAMP);

        for (int y = top; y <= bottom; y++) {
            int dy = 0;
            if (y < rect->top) {
                dy = rect->top - y;
            } else if (y > rect->bottom) {
                dy = y - rect->bottom;
            }

            BYTE *row = mask + (size_t)y * MAP_ARRAY_SIZE;
            for (int x = left; x <= right; x++) {
                int dx = 0;
                if (x < rect->left) {
                    dx = rect->left - x;
                } else if (x > rect->right) {
                    dx = x - rect->right;
                }

                /* Distance to the rect itself, so the ramp bends round a
                 * corner instead of squaring off at it. */
                float d = sqrtf((float)(dx * dx + dy * dy));
                if (d >= (float)OVERVIEW_FOG_RAMP) continue;

                /* alpha scales how far this region lifts the square out of
                 * full fog: at 255 the lift is the whole way and the value is
                 * the ramp's, and as the region fades every square it covers
                 * darkens towards the fog while the ramp keeps its shape at
                 * the edges. Rounded to nearest, the way the ramp itself is. */
                int regionLift =
                    ((OVERVIEW_FOG_ALPHA - (int)overviewFogRampValue(d))
                     * (int)rect->alpha + 127) / 255;

                /* Regions overlap — the brightest answer wins, or a pill's
                 * ramp would darken ground the tank's block has live. */
                BYTE v = (BYTE)(OVERVIEW_FOG_ALPHA - regionLift);
                if (v < row[x]) row[x] = v;
            }
        }
    }

    /* The per-square pass, which is ground no region covers any more but that
     * has not gone yet: 255 leaves the square as clear as a live one, 0 leaves
     * it as the regions left it. Brightest wins here too, so a square a region
     * already holds is never darkened by one of these. */
    if (lift != NULL) {
        for (i = 0; i < OVERVIEW_FOG_MASK_BYTES; i++) {
            BYTE v = (BYTE)(OVERVIEW_FOG_ALPHA -
                            (OVERVIEW_FOG_ALPHA * (int)lift[i]) / 255);
            if (v < mask[i]) mask[i] = v;
        }
    }

    /* Ground the player cannot see into, which is the one pass that darkens
     * rather than lights: a square behind a building sits inside the region the
     * block covers, so the walk above has already cleared it, and nothing a
     * region or a fading square says about it may put it back. Last, so it has
     * the final word over both. */
    if (dark == NULL) return;

    for (i = 0; i < OVERVIEW_FOG_MASK_BYTES; i++) {
        if (dark[i] > mask[i]) mask[i] = dark[i];
    }
}
