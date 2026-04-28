/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *   a raw GameSim* (bg_game welcome screen).
 *********************************************************/

#ifndef MAPVIEW_H
#define MAPVIEW_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "../../bolo/global.h"
#include "../../bolo/screen.h"
#include "../../bolo/screenbullet.h"
#include "../../bolo/screentank.h"
#include "../../bolo/screenlgm.h"
#include "../../bolo/game_sim.h"
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

/* Draw pre-built tile buffer. */
void mapViewDrawTiles(MapViewCtx *ctx, screen *value, screenMines *mineView,
                      int originX, int originY, int tileW, int tileH,
                      int edgeX, int edgeY);

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

/* Adjacency-aware tile calculation from a GameSim (no module-static state).
   selfPlayer = NEUTRAL (0xFF) for "no self" (bg_game case). */
BYTE mapViewCalcSquare(GameSim *sim, BYTE mapX, BYTE mapY, bool *outMine, BYTE selfPlayer);

/* Max tile buffer dimensions for mapViewRenderCentered */
#define MAPVIEW_MAX_TILES_W 256
#define MAPVIEW_MAX_TILES_H 256

/* All-in-one: compute camera from world center, build tile+sprite buffers
   from GameSim, render everything. selfPlayer=NEUTRAL for bg_game. */
void mapViewRenderCentered(MapViewCtx *ctx, GameSim *sim,
                           WORLD centerWX, WORLD centerWY,
                           int originX, int originY,
                           int viewW, int viewH,
                           BYTE selfPlayer);

#ifdef __cplusplus
}
#endif

#endif /* MAPVIEW_H */
