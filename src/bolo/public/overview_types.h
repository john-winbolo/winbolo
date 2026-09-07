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
 *Name:          Overview Types
 *Filename:      overview_types.h
 *Purpose:
 *  Tier-T3 header: the map memory a client keeps of what it
 *  has seen. Squares inside a live region are recalculated
 *  every display tick; squares outside one hold the tile
 *  they carried when they were last live. The memory is
 *  seeded from the map when it lands, so a square the player
 *  has never reached shows the ground as it was at the start;
 *  OVERVIEW_UNSEEN survives only between a reset and the next
 *  map install.
 *
 *  Read-only for frontends: the sim owns every byte here
 *  and rewrites it from overview_map.c.
 *********************************************************/

#ifndef OVERVIEW_TYPES_H
#define OVERVIEW_TYPES_H

#include "types.h"          /* MAP_ARRAY_SIZE, MAX_PILLS, MAX_BASES, MAX_TANKS */
#include "viewport_types.h" /* MAIN_SCREEN_SIZE_X */

#define OVERVIEW_UNSEEN      0xFF   /* tile[] sentinel; the per-square calculator
                                       never produces TANK_TRANSPARENT (255) */
#define OVERVIEW_F_MINE      0x01   /* mine was visible on the square when last seen */
#define OVERVIEW_F_LIVE      0x02   /* inside a live region on the latest update */
/* Close enough to see things moving on the square, not just the ground under
 * them. Every region but a terrain-only one grants it, so under every
 * experiment but Halo it is set wherever OVERVIEW_F_LIVE is. */
#define OVERVIEW_F_SIGHT     0x04
/* Inside a live region and not seen from the tank all the same - behind a
 * building, or outside the Headlights beam. The square keeps the tile it last
 * showed rather than being rewritten, nothing moving on it is drawn, and it is
 * left in full fog. Only set while something is masking the blocks round the
 * tank. */
#define OVERVIEW_F_HIDDEN    0x08

/* The tank region is the full scroll envelope of the main view. An item view
 * cannot be scrolled, so it shows one fixed block round what it watches: a
 * pillbox, a base and an allied tank all take the pill block. */
#define OVERVIEW_TANK_HALF   (MAIN_SCREEN_SIZE_X - 1)   /* 14 -> 29x29 */
#define OVERVIEW_PILL_HALF   (MAIN_SCREEN_SIZE_X / 2)   /* 7  -> 15x15 */
/* The classic view's own visible block, which is what the fog experiments
 * narrow the tank's region down to. Same number as the item block and a
 * different thing, so tuning one never moves the other. */
#define OVERVIEW_LENS_HALF   (MAIN_SCREEN_SIZE_X / 2)   /* 7  -> 15x15 */
/* Halo's outer block of ground: the same width as the scroll envelope and a
 * different thing, so tuning one never moves the other. The alpha is what puts
 * that block at half fog, through the rect's own alpha, so the fog builder
 * needs nothing added to it. */
#define OVERVIEW_HALO_HALF   (MAIN_SCREEN_SIZE_X - 1)   /* 14 -> 29x29 */
#define OVERVIEW_HALO_ALPHA  128

/* How far round the tank the Headlights blocks stay live whichever way it is
 * pointing, as a half-width: 2 is the 5x5 the tank sits in the middle of. */
#define OVERVIEW_HEADLIGHT_NEAR 2

/* Half the angle of the Headlights beam, so the beam itself is twice this
 * across. It is held as the square of the cosine rather than as the angle:
 * the test compares a dot product against the lengths it came from, and
 * squaring both sides is what removes the square root. 9330 out of 10000
 * is cos(15 degrees) squared, which makes the beam 30 degrees wide, and it is
 * the one number to change to widen or narrow it - cos(half-angle) squared,
 * scaled by OVERVIEW_HEADLIGHT_COS2_ONE and rounded. */
#define OVERVIEW_HEADLIGHT_COS2_ONE 10000
#define OVERVIEW_HEADLIGHT_COS2     9330

/* Two round the player's own tank, because Halo puts a block of ground round
 * the block they can see things moving on; every other experiment builds one
 * and leaves the second slot empty. */
#define OVERVIEW_MAX_REGIONS (2 + MAX_PILLS + MAX_BASES + MAX_TANKS)

/* Inclusive on all four edges, clamped to 0..255. alpha is 255 for a region
 * the player holds outright and ramps down over the last VIEW_DECAY_FADE_SECS
 * of a decay window, reaching 0 as the window runs out. terrainOnly is a
 * region that stamps the ground and grants no sight, so what is moving on it
 * is not drawn.
 *
 * The frontend compares stored rects byte for byte to decide it can reuse a
 * fog mask, so every rect has to be zeroed whole when it is built rather than
 * filled field by field - which is what overviewRectAround does. */
typedef struct OverviewRect {
    int  left, top, right, bottom;
    BYTE alpha;
    BYTE terrainOnly;
} OverviewRect;

/* The Headlights beam the latest update built, or active false when it built
 * none. It is the record of what the map did rather than of which toggle is
 * on, the same way fadeSpan and hiddenActive are, so a frontend reads the beam
 * the squares it was handed came from and cannot be caught out by an
 * experiment changed between the update and the drawing of it.
 *
 * The facing vector is in 256ths of a square, which is how the facing table
 * holds it, and block is the extent the beam was applied over. Zeroed whole
 * when it is built, so it can be compared byte for byte the way the rects
 * are. */
typedef struct OverviewBeam {
    bool         active;              /* a Headlights experiment built this */
    BYTE         originX, originY;    /* the tank square the beam comes from */
    int          dirX, dirY;          /* the facing vector, 256ths of a square */
    OverviewRect block;               /* how far it reaches */
} OverviewBeam;

/* Whether the beam lights a point (dx, dy) squares from the square it comes
 * from, measured centre to centre. Whole numbers ask about a square's own
 * centre, which is what the map memory does; fractions ask about a point
 * inside a square, which is what the fog mask does when it samples the beam's
 * edge several times across each square and gets a straight line instead of a
 * staircase.
 *
 * Floats because the answer decides only what the local client draws: it never
 * crosses the wire and the simulation never reads it, so nothing depends on
 * two machines agreeing about a boundary point. The arithmetic runs in double,
 * which holds every product below exactly at whole-square offsets, so a square
 * asked about here answers exactly as the whole-number test always did.
 *
 * active and block are the caller's to read; this answers about the shape
 * alone. */
static inline bool overviewBeamLights(const OverviewBeam *beam,
                                      float dx, float dy) {
    double nearHalf; /* The near block's reach, half a square past its squares */
    double x;        /* The point asked about, in squares */
    double y;
    double fx;       /* Where the tank points, in 256ths of a square */
    double fy;
    double dot;      /* How much of the way to the point runs along the facing */
    double lhs;      /* That, squared and scaled to compare against the cosine */
    double rhs;      /* The cosine's share of the two lengths it came from */

    if (beam == NULL) return false;

    /* The near squares, read as a square block rather than a circle, so there
     * is no rounding to argue about at the diagonals. The tank's own square is
     * inside it, so the reticle is never dropped. The half square either side
     * is what puts every point of a near square in the block; at whole-square
     * offsets it picks out the same 5x5 it always has. */
    nearHalf = (double)OVERVIEW_HEADLIGHT_NEAR + 0.5;
    x = (double)dx;
    y = (double)dy;
    if (x >= -nearHalf && x <= nearHalf && y >= -nearHalf && y <= nearHalf) {
        return true;
    }

    fx = (double)beam->dirX;
    fy = (double)beam->dirY;
    dot = x * fx + y * fy;
    if (dot <= 0.0) {
        return false; /* the rear half, whatever the angle works out to */
    }

    /* The angle between the point and the facing, as a dot product against the
     * lengths it came from and squared so there is no square root:
     *
     *   dot^2 * ONE >= COS2 * |d|^2 * |f|^2
     */
    lhs = dot * dot * (double)OVERVIEW_HEADLIGHT_COS2_ONE;
    rhs = (double)OVERVIEW_HEADLIGHT_COS2 * (x * x + y * y) *
          (fx * fx + fy * fy);
    return lhs >= rhs;
}

typedef struct OverviewMap {
    BYTE         tile[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];   /* [x][y] tilenum index as last seen */
    BYTE         flags[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];  /* [x][y] OVERVIEW_F_* */
    /* Afterimage's countdown: how long a square that has dropped out of the
     * live set has left before it is back in full fog. Only Afterimage writes
     * these, and fadeSpan is what says so - it is the value fade counts down
     * from, and 0 under every other experiment, so nothing else has to know
     * which experiment is running to read them. Brightness is worked out where
     * it is drawn, as fade * 255 / fadeSpan. Another 64 KB on a struct that
     * already carries two arrays this size, and ClientSim holds one of these by
     * value. */
    BYTE         fade[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];   /* [x][y] ticks left before full fog */
    BYTE         fadeSpan;                               /* 0 when nothing is fading */
    /* Whether the latest update masked the blocks round the tank, so some of
     * their squares are hidden rather than live - by line of sight, by the
     * Headlights beam, or by both. It is the record of what the map did rather
     * than of which toggle is on, the same way fadeSpan is, so a frontend reads
     * the squares it was handed and cannot be caught out by a toggle flipped
     * between the update and the drawing of it. */
    bool         hiddenActive;
    /* The Headlights beam the latest update masked those blocks with, and
     * active false when no experiment built one. The fog mask samples it
     * several times across each square, so it needs the beam itself rather
     * than the per-square answer the flags carry. */
    OverviewBeam beam;
    OverviewRect live[OVERVIEW_MAX_REGIONS];             /* regions used by the latest update */
    int          liveCount;
    OverviewRect prevLive[OVERVIEW_MAX_REGIONS];         /* regions used by the update before it */
    int          prevLiveCount;
    bool         haloWasLive;                            /* the halo region existed last update */
    bool         tankWasLive;                            /* the tank region existed last update */
    /* A tank waiting to respawn reports the map origin instead of the square
     * it died on, so the square it last had a block on has to be kept here:
     * without it there is no position to hold the block still at. */
    BYTE         lastTankMX, lastTankMY;                 /* where the tank last had a live block */
    bool         haveLastTank;                           /* whether the two above mean anything yet */
    BYTE         lastViewLeft, lastViewTop;              /* where the classic view was when the tank last had a block */
    bool         haveLastView;                           /* whether the two above mean anything yet */
    bool         pillWasLive[MAX_PILLS];                 /* pill i had a region last update */
    bool         baseWasLive[MAX_BASES];                 /* base i had a region last update */
    bool         allyWasLive[MAX_TANKS];                 /* player i's tank had a region last update */
    bool         pillWasInTank[MAX_PILLS];               /* pill i was being carried last update */
    unsigned     generation;                             /* +1 per update that changed anything */
    unsigned     seenCount;                              /* squares with tile != OVERVIEW_UNSEEN */
} OverviewMap;

#endif /* OVERVIEW_TYPES_H */
