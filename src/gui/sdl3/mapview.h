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

/* Tile-number -> atlas-coordinate lookup tables (populated by mapViewInit). */
extern int mapViewPosX[256];
extern int mapViewPosY[256];

/* Initialize lookup tables. Called once at startup. */
void mapViewInit(void);

/* Smooth-mode shell render: walks sim->shs and draws each live
 * shell at its full 1/256-tile (sub-wu) position with the same
 * tip-anchor / ingamerotate logic as the classic path.  Called
 * after mapViewDrawShells when Animation Style is Smooth so the
 * tip-anchor lands on the shell's authoritative coordinate.  Pass
 * the same originX/Y/edgeX/Y/tileW/tileH the sprite renders use. */
/* Forward decl: GameSim is the typedef'd struct in game_sim.h. */
struct GameSim;
/* Populate the per-frame shell sub-pixel cache used by
 * mapViewDrawShells when Animation = Smooth.  Call before
 * mapViewDrawShells with cs->xOffset/yOffset.  The classic shell
 * render then adds the fractional game-pixel from sim on top of its
 * normal position so shells slide at 1/256-tile precision. */
void mapViewSetShellsFromSim(struct GameSim *sim, int xOffset, int yOffset);

/* Same idea for LGMs — populate before mapViewDrawLGMs. */
void mapViewSetLgmsFromSim(struct GameSim *sim, int xOffset, int yOffset);

/* xOffset/yOffset are the engine's camera tile origin (cs->xOffset/Y).
 * Sim positions are world coords; the classic edgeX/Y is buffer-relative
 * (camera offset is implicit in the pre-built screen buffers), so we
 * have to subtract the camera tile origin to land at the same screen
 * position as the classic path. */
void mapViewDrawShellsFromSim(MapViewCtx *ctx, struct GameSim *sim,
                              int xOffset, int yOffset,
                              int originX, int originY,
                              int tileW, int tileH,
                              int edgeX, int edgeY);

/* Smooth-mode LGM render: like the shell version above, walks
 * sim->lgmen[] and draws each live LGM at full 1/256-tile precision
 * with the (1.5, 2.0) game-pixel body anchor.  Called after
 * mapViewDrawLGMs when Animation Style is Smooth. */
void mapViewDrawLGMsFromSim(MapViewCtx *ctx, struct GameSim *sim,
                            int xOffset, int yOffset,
                            int originX, int originY,
                            int tileW, int tileH,
                            int edgeX, int edgeY);


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

/* Provide per-player tank angles direct from sim before
 * mapViewDrawTanks runs.  When set, the ingamerotate path uses these
 * full TURNTYPE (0..255) angles instead of the 16-step dir derived
 * from the screenTanks frame.  Pass NULL to clear. */
void mapViewSetTankAnglesFromSim(struct GameSim *sim);

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
