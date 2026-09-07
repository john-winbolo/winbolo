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
