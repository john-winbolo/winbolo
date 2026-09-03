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

#include "types.h"          /* MAP_ARRAY_SIZE, MAX_PILLS */
#include "viewport_types.h" /* MAIN_SCREEN_SIZE_X */

#define OVERVIEW_UNSEEN      0xFF   /* tile[] sentinel; the per-square calculator
                                       never produces TANK_TRANSPARENT (255) */
#define OVERVIEW_F_MINE      0x01   /* mine was visible on the square when last seen */
#define OVERVIEW_F_LIVE      0x02   /* inside a live region on the latest update */

/* The tank region is the full scroll envelope of the main view; the pill
 * region is what pill view can reach. */
#define OVERVIEW_TANK_HALF   (MAIN_SCREEN_SIZE_X - 1)   /* 14 -> 29x29 */
#define OVERVIEW_PILL_HALF   (MAIN_SCREEN_SIZE_X / 2)   /* 7  -> 15x15 */
#define OVERVIEW_MAX_REGIONS (1 + MAX_PILLS)

/* Inclusive on all four edges, clamped to 0..255. */
typedef struct OverviewRect { int left, top, right, bottom; } OverviewRect;

typedef struct OverviewMap {
    BYTE         tile[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];   /* [x][y] tilenum index as last seen */
    BYTE         flags[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];  /* [x][y] OVERVIEW_F_* */
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
    bool         pillWasLive[MAX_PILLS];                 /* pill i had a region last update */
    bool         pillWasInTank[MAX_PILLS];               /* pill i was being carried last update */
    unsigned     generation;                             /* +1 per update that changed anything */
    unsigned     seenCount;                              /* squares with tile != OVERVIEW_UNSEEN */
} OverviewMap;

#endif /* OVERVIEW_TYPES_H */
