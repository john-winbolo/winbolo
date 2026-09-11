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
 * them. Every region grants it, so it is set wherever OVERVIEW_F_LIVE is. Kept
 * as its own bit because sight and liveness are separate questions to a
 * frontend, and a region that stamps ground without granting sight is the
 * obvious next thing to want. */
#define OVERVIEW_F_SIGHT     0x04
/* Inside a live region and not seen from the tank all the same - behind a
 * building. The square keeps the tile it last showed rather than being
 * rewritten, nothing moving on it is drawn, and it is left in full fog. Only
 * set while line of sight is masking the blocks round the tank. */
#define OVERVIEW_F_HIDDEN    0x08

/* The tank region is the full scroll envelope of the main view. An item view
 * cannot be scrolled, so it shows one fixed block round what it watches: a
 * pillbox, a base and an allied tank all take the pill block. */
#define OVERVIEW_TANK_HALF   (MAIN_SCREEN_SIZE_X - 1)   /* 14 -> 29x29 */
#define OVERVIEW_PILL_HALF   (MAIN_SCREEN_SIZE_X / 2)   /* 7  -> 15x15 */
/* The classic view's own visible block, which is what the Classic fog mode
 * narrows the tank's region down to. Same number as the item block and a
 * different thing, so tuning one never moves the other. */
#define OVERVIEW_LENS_HALF   (MAIN_SCREEN_SIZE_X / 2)   /* 7  -> 15x15 */

/* One round the player's own tank, and one for each item a view policy grants
 * a block to. */
#define OVERVIEW_MAX_REGIONS (1 + MAX_PILLS + MAX_BASES + MAX_TANKS)

/* Inclusive on all four edges, clamped to 0..255. alpha is 255 for a region
 * the player holds outright and ramps down over the last VIEW_DECAY_FADE_SECS
 * of a decay window, reaching 0 as the window runs out.
 *
 * The frontend compares stored rects byte for byte to decide it can reuse a
 * fog mask, so every rect has to be zeroed whole when it is built rather than
 * filled field by field - which is what overviewRectAround does. */
typedef struct OverviewRect {
    int  left, top, right, bottom;
    BYTE alpha;
} OverviewRect;

typedef struct OverviewMap {
    BYTE         tile[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];   /* [x][y] tilenum index as last seen */
    BYTE         flags[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];  /* [x][y] OVERVIEW_F_* */
    /* Whether the latest update masked the blocks round the tank by line of
     * sight, so some of their squares are hidden rather than live. It is the
     * record of what the map did rather than of which mode is on, so a frontend
     * reads the squares it was handed and cannot be caught out by a mode
     * changed between the update and the drawing of it. */
    bool         hiddenActive;
    OverviewRect live[OVERVIEW_MAX_REGIONS];             /* regions used by the latest update */
    int          liveCount;
    OverviewRect prevLive[OVERVIEW_MAX_REGIONS];         /* regions used by the update before it */
    int          prevLiveCount;
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
