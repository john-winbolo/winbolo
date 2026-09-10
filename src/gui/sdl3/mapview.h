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
#include "sprite_atlas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SDL_Renderer *renderer;
    SDL_Texture  *tilesTex;
    int           zoomFactor;  /* whole-number zoom, read by mapViewRenderCentered */
    int           sheetScale;  /* atlas scale: source coords *= sheetScale */
    float         scale;       /* screen pixels per game pixel, read by the
                                  sprite drawers. A positional initialiser
                                  has to give it too: left at 0 it puts
                                  every sprite on the origin. */
    /* The padded copy of the moving sprites, and the index into it. Both
       NULL draws every sprite from tilesTex, which is what a caller that
       never built one gets and what the drawers did before there was one.
       Fill them in and the tank, shell and LGM draws move across; the
       terrain does not. sprite_atlas.h says why they are worth carrying. */
    SDL_Texture       *spritesTex;
    const SpriteAtlas *sprites;
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

/* Where a sprite is drawn from: the padded atlas when the context carries
 * one and it holds this sprite, the sheet otherwise. (x, y, w, h) are the
 * 1x sheet coordinates the drawers switch to, as tiles.h gives them.
 *
 * No inset on the atlas rect. The inset above is for a NEAREST sample that
 * rounds outward at a fractional scale, and on the atlas that rounding
 * lands in the gutter, which is a copy of the texel it was reaching for. A
 * filtered sample finds the same copy, which is the whole point of the
 * gutter — while shifting the rect by a twentieth of a texel is what puts
 * the sample inside the blend band in the first place. */
static inline SDL_Texture *mapViewSpriteSrc(const MapViewCtx *ctx,
                                            int x, int y, int w, int h,
                                            SDL_FRect *out) {
    if (ctx->spritesTex != NULL &&
        spriteAtlasFind(ctx->sprites, x, y, w, h, out)) {
        return ctx->spritesTex;
    }
    *out = mapViewAtlasSrc(x, y, w, h, ctx->sheetScale);
    return ctx->tilesTex;
}

/* Draw pre-built tile buffer. */
void mapViewDrawTiles(MapViewCtx *ctx, screen *value, screenMines *mineView,
                      int originX, int originY, int tileW, int tileH,
                      int edgeX, int edgeY);

/* The layer both views draw over their terrain — build cursor, these three
   passes, tank names, gunsight and the pill and base numbers, in one order —
   is mapViewDrawOverlay in mapview_overlay.h. It stays out of this file
   because it draws text: the targets that link these drawers without a font
   library or the label drawer (the gym, BrainTest) must go on linking. */

/* Draw pre-built sprite lists. The lists' square 0,0 lands at
   originX - tileW - edgeX, and a sprite sits ctx->scale screen pixels
   further on per game pixel. Float because the overview's camera is
   continuous and its origin is fractional; the classic view passes whole
   pixels and gets whole pixels back. tileW/tileH is also the size a tank
   sprite is drawn at. */
void mapViewDrawShells(MapViewCtx *ctx, screenBullets *sBullets,
                       float originX, float originY, float tileW, float tileH,
                       float edgeX, float edgeY);

void mapViewDrawTanks(MapViewCtx *ctx, screenTanks *tks,
                      float originX, float originY, float tileW, float tileH,
                      float edgeX, float edgeY);

void mapViewDrawLGMs(MapViewCtx *ctx, screenLgm *lgms,
                     float originX, float originY, float tileW, float tileH,
                     float edgeX, float edgeY);

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
