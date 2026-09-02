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
 * Name:          overview_hud_layout.h
 * Purpose:       Where the status panels and the newswire go
 *                when the map overview owns the whole game
 *                window: one scaled column of classic-chrome
 *                slices down the right edge, and the newswire
 *                on a strip across the bottom.
 *
 *                The pieces are cut out of a frame of the
 *                classic 515x325 chrome, so each one carries
 *                both where it is in that layout and where it
 *                lands on screen. Sizes come from positions.h,
 *                which is what the classic frame draws them at.
 *
 *                Pure arithmetic — no SDL, no ImGui, no
 *                renderer — so it compiles into the unit-test
 *                binary and the drawing code stays a thin
 *                consumer of the numbers.
 *********************************************************/

#ifndef OVERVIEW_HUD_LAYOUT_H
#define OVERVIEW_HUD_LAYOUT_H

#include <stdbool.h>

#include "../positions.h"

/* The classic chrome layout the slices are cut from, in source pixels.
 * SDL3_SCREEN_W / SDL3_SCREEN_H in sdl3draw.h hold the same two numbers, but
 * that header pulls in SDL3 and this one must stay renderer-free. */
#define OVERVIEW_HUD_SRC_W 515
#define OVERVIEW_HUD_SRC_H 325

#ifdef __cplusplus
extern "C" {
#endif

/* One piece of the classic 515x325 chrome, copied into the HUD column: where
   it is in the source layout and where it lands on screen. */
typedef struct OverviewHudElement {
    int   srcX, srcY, srcW, srcH;   /* source pixels in the 515x325 layout */
    float dstX, dstY, dstW, dstH;   /* window pixels, relative to the map rect */
} OverviewHudElement;

typedef enum {
    OVERVIEW_HUD_MANSTATUS = 0,
    OVERVIEW_HUD_KILLSDEATHS,
    OVERVIEW_HUD_TANKS,
    OVERVIEW_HUD_PILLS,
    OVERVIEW_HUD_BASES,
    OVERVIEW_HUD_BASEBARS,
    OVERVIEW_HUD_TANKBARS,
    OVERVIEW_HUD_BUILDSELECT,
    OVERVIEW_HUD_NEWSWIRE,
    OVERVIEW_HUD_COUNT
} OverviewHudElementId;

typedef struct OverviewHudLayout {
    float              scale;                        /* source pixel -> window pixel */
    OverviewHudElement el[OVERVIEW_HUD_COUNT];
    float              columnX, columnY, columnW, columnH;   /* translucent backing */
    float              newswireX, newswireY, newswireW, newswireH;
} OverviewHudLayout;

/* Lay the column out for a map rect of viewW x viewH window pixels. Returns
   false and leaves *out untouched when the rect is too small to hold a
   legible column, in which case the caller draws no HUD. */
bool overviewHudLayout(int viewW, int viewH, OverviewHudLayout *out);

#ifdef __cplusplus
}
#endif

#endif /* OVERVIEW_HUD_LAYOUT_H */
