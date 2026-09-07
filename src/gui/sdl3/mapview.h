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
 * Name:          mapview.h
 * Purpose:
 *   Reusable map view renderer. Draws tiles, shells,
 *   tanks, and LGMs given an SDL renderer, tile atlas,
 *   and either pre-built screen* structs (client) or
 *   a ServerSim* (bg_game welcome screen / braintest).
 *********************************************************/

#ifndef MAPVIEW_H
#define MAPVIEW_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "global.h"
#include "viewport_types.h"  /* screen, screenMines, screenGunsight */
#include "screenbullet.h"
#include "screentank.h"
#include "screenlgm.h"
#include "server_sim.h"
#include "sprite_positions.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SDL_Renderer *renderer;
    SDL_Texture  *tilesTex;
    int           zoomFactor;
    int           sheetScale;  /* atlas scale: source coords *= sheetScale */
} MapViewCtx;

/* Source rect for a cell of the tile atlas, inset by a whisker on every
 * side. The sheet packs sprites edge-to-edge — the row above the tanks is
 * water, the shells and LGMs sit 3px apart — so a NEAREST sample that lands
 * exactly on a rect edge and rounds the wrong way shows a line of whatever
 * is packed next door. On the main view's integer zoom that edge case never
 * comes up (pixel centres map to texel centres), but a fractional scale —
 * the overview's zoom rungs, a preview scaled to fit — puts pixel centres
 * arbitrarily close to the edges. The inset must stay well under 0.5: at
 * integer zoom the sample nearest each edge sits about 0.5 - inset/cellSize
 * texels inside, so 0.05 leaves 1:1 rendering picking every texel exactly
 * while absorbing float error at any scale. Every draw from the atlas goes
 * through here; a raw SDL_FRect against the sheet is the bug returning. */
#define MAPVIEW_ATLAS_INSET 0.05f

static inline SDL_FRect mapViewAtlasSrc(int x, int y, int w, int h, int ss) {
    SDL_FRect r;
    r.x = (float)(x * ss) + MAPVIEW_ATLAS_INSET;
    r.y = (float)(y * ss) + MAPVIEW_ATLAS_INSET;
    r.w = (float)(w * ss) - 2.0f * MAPVIEW_ATLAS_INSET;
    r.h = (float)(h * ss) - 2.0f * MAPVIEW_ATLAS_INSET;
    return r;
}

/* Draw pre-built tile buffer. hiddenView marks the squares the player cannot
   see into; those are drawn dimmed, because the tile they carry is what was
   last seen there rather than what is there now. NULL draws every square at
   full brightness. */
void mapViewDrawTiles(MapViewCtx *ctx, screen *value, screenMines *mineView,
                      screenHidden *hiddenView,
                      int originX, int originY, int tileW, int tileH,
                      int edgeX, int edgeY);

/* How much of a hidden square's tile is drawn, out of 255. */
#define MAPVIEW_HIDDEN_ALPHA 110

/* Draw pre-built sprite lists. */
void mapViewDrawShells(MapViewCtx *ctx, screenBullets *sBullets,
                       int originX, int originY, int tileW, int tileH,
                       int edgeX, int edgeY);

void mapViewDrawTanks(MapViewCtx *ctx, screenTanks *tks,
                      int originX, int originY, int tileW, int tileH,
                      int edgeX, int edgeY);

void mapViewDrawLGMs(MapViewCtx *ctx, screenLgm *lgms,
                     int originX, int originY, int tileW, int tileH,
                     int edgeX, int edgeY);

/* Adjacency-aware tile calculation from a ServerSim (no module-static state).
   selfPlayer = NEUTRAL (0xFF) for "no self" (bg_game case). */
BYTE mapViewCalcSquare(ServerSim *sim, BYTE mapX, BYTE mapY, bool *outMine, BYTE selfPlayer);

/* Max tile buffer dimensions for mapViewRenderCentered */
#define MAPVIEW_MAX_TILES_W 256
#define MAPVIEW_MAX_TILES_H 256

/* All-in-one: compute camera from world center, build tile+sprite buffers
   from ServerSim, render everything. selfPlayer=NEUTRAL for bg_game. */
void mapViewRenderCentered(MapViewCtx *ctx, ServerSim *sim,
                           WORLD centerWX, WORLD centerWY,
                           int originX, int originY,
                           int viewW, int viewH,
                           BYTE selfPlayer);

#ifdef __cplusplus
}
#endif

#endif /* MAPVIEW_H */
