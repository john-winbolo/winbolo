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

/* The first texel of a square's row band, so a per-square value can be written
 * across the OVERVIEW_FOG_SUB texels the square covers. */
static BYTE *overviewFogTexelRow(BYTE *mask, int mx, int my, int ty) {
    return mask +
           ((size_t)my * OVERVIEW_FOG_SUB + (size_t)ty) *
               (size_t)OVERVIEW_FOG_MASK_SIDE +
           (size_t)mx * OVERVIEW_FOG_SUB;
}

/* Every texel of one map square lit to `v` where the mask has it darker. The
 * regions and the lift both answer per square, so a square's texels all take
 * the same byte and the edges land on square boundaries as they always did. */
static void overviewFogSquareLift(BYTE *mask, int mx, int my, BYTE v) {
    for (int ty = 0; ty < OVERVIEW_FOG_SUB; ty++) {
        BYTE *row = overviewFogTexelRow(mask, mx, my, ty);
        for (int tx = 0; tx < OVERVIEW_FOG_SUB; tx++) {
            if (v < row[tx]) row[tx] = v;
        }
    }
}

/* The same square, taken down to `v` where the mask has it brighter — which is
 * what the per-square dark pass does. */
static void overviewFogSquareDarken(BYTE *mask, int mx, int my, BYTE v) {
    for (int ty = 0; ty < OVERVIEW_FOG_SUB; ty++) {
        BYTE *row = overviewFogTexelRow(mask, mx, my, ty);
        for (int tx = 0; tx < OVERVIEW_FOG_SUB; tx++) {
            if (v > row[tx]) row[tx] = v;
        }
    }
}

/* Where a texel's centre sits inside its own square, in squares and measured
 * from the square's centre: -0.375 through +0.375 at a sub of 4. Adding it to
 * the whole-square offset is what asks the beam about a point inside a square
 * rather than about the square. */
static float overviewFogTexelOffset(int t) {
    return ((float)t + 0.5f) / (float)OVERVIEW_FOG_SUB - 0.5f;
}

/* Fog carried at `d` squares from a region, 0 at the region's edge and full
 * fog at OVERVIEW_FOG_RAMP. Smoothstepped rather than linear so a ramp longer
 * than a square has no visible kink where it flattens out into the fog beyond
 * it. The caller only ever asks about a square inside the ramp, and with a ramp
 * of 0 that is a square the region covers: d is 0, the smoothstep is 0, and the
 * square carries none of the fog. The 1 stands in for the span in that build so
 * the division is still sound. */
static BYTE overviewFogRampValue(float d) {
    float span = (float)(OVERVIEW_FOG_RAMP > 0 ? OVERVIEW_FOG_RAMP : 1);
    float t = d / span;
    float s = t * t * (3.0f - 2.0f * t);
    return (BYTE)lroundf((float)OVERVIEW_FOG_ALPHA * s);
}

void overviewFogBuildMask(const OverviewRect *live, int liveCount,
                          const BYTE *lift, const BYTE *dark,
                          const OverviewBeam *beam, BYTE *mask) {
    int i; /* Looping variable */

    if (mask == NULL) return;

    /* Everything the walk below does not reach is outside every region, and
     * beyond every region's ramp where there is one, so it carries full fog. */
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

            for (int x = left; x <= right; x++) {
                int dx = 0;
                if (x < rect->left) {
                    dx = rect->left - x;
                } else if (x > rect->right) {
                    dx = x - rect->right;
                }

                /* Distance to the rect itself, so a ramp bends round a corner
                 * instead of squaring off at it. A square the region covers is
                 * at distance 0 and is never skipped, which with a ramp of 0
                 * leaves the region's own squares lit and every other square
                 * in full fog. */
                float d = sqrtf((float)(dx * dx + dy * dy));
                if (d > 0.0f && d >= (float)OVERVIEW_FOG_RAMP) continue;

                /* alpha scales how far this region lifts the square out of
                 * full fog: at 255 the lift is the whole way and the value is
                 * the ramp's, and as the region fades every square it covers
                 * darkens towards the fog. Rounded to nearest, the way the
                 * ramp itself is. */
                int regionLift =
                    ((OVERVIEW_FOG_ALPHA - (int)overviewFogRampValue(d))
                     * (int)rect->alpha + 127) / 255;

                /* Regions overlap — the brightest answer wins, or a pill's
                 * faded block would darken ground the tank's block has live. */
                BYTE v = (BYTE)(OVERVIEW_FOG_ALPHA - regionLift);
                overviewFogSquareLift(mask, x, y, v);
            }
        }
    }

    /* The per-square pass, which is ground no region covers any more but that
     * has not gone yet: 255 leaves the square as clear as a live one, 0 leaves
     * it as the regions left it. Brightest wins here too, so a square a region
     * already holds is never darkened by one of these. A lift of 0 works out to
     * full fog, which can never be brighter than what the mask already carries,
     * so those squares are stepped over rather than written back unchanged. */
    if (lift != NULL) {
        for (i = 0; i < OVERVIEW_FOG_SQUARE_BYTES; i++) {
            if (lift[i] == 0) continue;
            BYTE v = (BYTE)(OVERVIEW_FOG_ALPHA -
                            (OVERVIEW_FOG_ALPHA * (int)lift[i]) / 255);
            overviewFogSquareLift(mask, i % MAP_ARRAY_SIZE, i / MAP_ARRAY_SIZE,
                                  v);
        }
    }

    /* Ground the player cannot see into, which is the one pass that darkens
     * rather than lights: a square behind a building sits inside the region the
     * block covers, so the walk above has already cleared it, and nothing a
     * region or a fading square says about it may put it back. After both, so
     * it has the final word over them. A 0 darkens nothing, so those squares
     * are stepped over the way a lift of 0 is. */
    if (dark != NULL) {
        for (i = 0; i < OVERVIEW_FOG_SQUARE_BYTES; i++) {
            if (dark[i] == 0) continue;
            overviewFogSquareDarken(mask, i % MAP_ARRAY_SIZE,
                                    i / MAP_ARRAY_SIZE, dark[i]);
        }
    }

    /* The Headlights beam, and the one thing here worked out texel by texel:
     * the wedge's edge crosses a square at an angle, so asking about it once a
     * square is what drew it as a staircase. Every texel of the block the beam
     * does not reach goes to full fog, which is what the dark pass above does
     * for a square, so the beam only ever darkens and composes with the rest
     * the same way.
     *
     * The block alone: outside it every square is already uniform and the beam
     * has nothing to say about it, so this is at most 29x29 squares of work
     * rather than the map's 256x256. */
    if (beam == NULL || !beam->active) return;

    int bLeft   = overviewFogClampSquare(beam->block.left);
    int bTop    = overviewFogClampSquare(beam->block.top);
    int bRight  = overviewFogClampSquare(beam->block.right);
    int bBottom = overviewFogClampSquare(beam->block.bottom);

    for (int y = bTop; y <= bBottom; y++) {
        for (int ty = 0; ty < OVERVIEW_FOG_SUB; ty++) {
            float dy = (float)(y - (int)beam->originY) +
                       overviewFogTexelOffset(ty);
            for (int x = bLeft; x <= bRight; x++) {
                BYTE *row = overviewFogTexelRow(mask, x, y, ty);
                for (int tx = 0; tx < OVERVIEW_FOG_SUB; tx++) {
                    float dx = (float)(x - (int)beam->originX) +
                               overviewFogTexelOffset(tx);
                    if (overviewBeamLights(beam, dx, dy)) continue;
                    if (row[tx] < (BYTE)OVERVIEW_FOG_ALPHA) {
                        row[tx] = (BYTE)OVERVIEW_FOG_ALPHA;
                    }
                }
            }
        }
    }
}
