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
 * Name:          mapview_overlay.h
 * Purpose:
 *   The entity layer both map views draw over their
 *   terrain: build cursor, shells, tanks, tank names,
 *   LGMs, gunsight and the pill and base numbers, in
 *   that order. The classic view and the map overview
 *   each fill a MapViewOverlay and make one call, so
 *   the two place everything by the same arithmetic.
 *
 *   Apart from mapview.h because it draws text through
 *   SDL_ttf and tank_label.c; the targets that link the
 *   sprite drawers without either (the gym, BrainTest)
 *   never include this.
 *********************************************************/

#ifndef MAPVIEW_OVERLAY_H
#define MAPVIEW_OVERLAY_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdbool.h>
#include "global.h"
#include "tilenum.h"      /* the pill and base tile ranges */
#include "client_sim.h"   /* OverviewItemLabel */
#include "mapview.h"
#include "tank_label.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Numbers a pill or base can carry: the sim indexes both from 0, so the
   highest is one less than the larger of the two counts. A number outside
   this range is drawn without the cache — the classic view builds its list
   from the screen buffer's tiles, so a pill tile on a square the sim has no
   pill for still produces the not-found number the view has always drawn. */
#define ITEM_LABEL_MAX_NUM ((MAX_PILLS) > (MAX_BASES) ? (MAX_PILLS) : (MAX_BASES))

/* The rendered numbers, one texture per number per face. Rasterising a
   glyph is cheap; making and destroying a GPU texture for it every frame is
   not, and the overview can put a number on every pill and base at once.
   Kept by the host rather than this file because textures belong to the
   renderer that made them and the overview may be on the pop-out's, the
   same reason TankLabelCache is passed in.

   Zero-initialise to start empty. Rebuilt from nothing when the renderer or
   either face changes under it — which is what a zoom change reopening the
   fonts looks like from here. The draw scales the glyphs, so one texture
   serves every zoom the face itself did not change for. */
typedef struct ItemLabelCache {
  SDL_Renderer *renderer;
  TTF_Font     *pillFont;
  TTF_Font     *baseFont;
  float         pillFontSize;   /* size the pill textures were rendered at */
  float         baseFontSize;
  SDL_Texture  *pillTex[ITEM_LABEL_MAX_NUM];
  SDL_Texture  *baseTex[ITEM_LABEL_MAX_NUM];
} ItemLabelCache;

/* Destroys every texture and forgets what they were built against. Call from
   the host's teardown, ahead of the renderer; the draw flushes on its own
   when the renderer or a face changes. */
void itemLabelCacheFlush(ItemLabelCache *c);

/* Squares and game pixels here are in the sprite lists' frame: the classic
   view's 17x17 buffer squares, the overview's absolute map squares. */
typedef struct MapViewOverlay {
  /* Overview below 1x: directional markers in place of tank sprites. */
  bool             simpleTanks;
  /* Build cursor, drawn under the sprites. Faint is the locked target with
     build mode off; solid is build mode on or the pointer over the map. */
  bool             cursorShown;
  bool             cursorFaint;
  BYTE             cursorMapX, cursorMapY;

  /* Gunsight, drawn over them. Its top-left goes where a tile sprite's
     would at the sight's square and pixel, which puts the centre pixel of
     a crosshair one pixel wider than a tile on the aim point. */
  bool             gunsightShown;
  BYTE             gsMapX, gsMapY, gsPixelX, gsPixelY;
  SDL_Texture     *crosshairTex;
  int              crosshairPx;      /* sprite side in game pixels */

  /* Tank name labels, one square to the right of the sprite. The face is
     opened at the main window's zoom and the glyphs drawn at that size
     times labelDisplayScale. A NULL cache or font draws no names. */
  TankLabelCache  *labelCache;
  TTF_Font        *labelFont;
  float            labelDisplayScale;

  /* Pill and base numbers; drawn only when ctx->scale >= itemLabelMinScale.
     A base's number sits in the top-left of its square in baseFont, a
     pill's centred in pillFont, each on a black backing. The glyphs and the
     backing scale by labelDisplayScale as the names do. */
  const OverviewItemLabel *itemLabels;
  int                      itemLabelCount;
  TTF_Font                *pillFont;
  TTF_Font                *baseFont;
  float                    itemLabelMinScale;
  /* Where the rendered numbers are kept between frames. NULL draws them the
     slow way, rebuilding and destroying a texture per number per frame. */
  ItemLabelCache          *itemLabelCache;

  /* Labels are clipped to this box in renderer coordinates: a name's left
     edge is held at clipLeft, and nothing is drawn outside the box. */
  float            clipLeft, clipTop, clipRight, clipBottom;
} MapViewOverlay;

/* Whether the tile drawn on a square is a pillbox or a base: the test both
   views make before putting a number on the square. */
static inline bool mapViewTileIsPill(BYTE tile) {
  return tile == PILL_EVIL_15 ||
         (tile >= PILL_EVIL_14 && tile <= PILL_EVIL_0) ||
         (tile >= PILL_GOOD_15 && tile <= PILL_GOOD_0);
}

static inline bool mapViewTileIsBase(BYTE tile) {
  return tile == BASE_GOOD || tile == BASE_NEUTRAL || tile == BASE_EVIL;
}

/* Draws the whole layer: the cursor, then mapViewDrawShells and
   mapViewDrawTanks, the tank names, mapViewDrawLGMs, then the gunsight and
   the item numbers. The origin, tile and edge arguments are the sprite
   drawers' — the lists' square 0,0 lands at originX - tileW - edgeX — and
   the pass places everything from that base at ctx->scale. */
void mapViewDrawOverlay(MapViewCtx *ctx, const MapViewOverlay *ov,
                        screenTanks *tks, screenLgm *lgms, screenBullets *sb,
                        float originX, float originY,
                        float tileW, float tileH,
                        float edgeX, float edgeY);

#ifdef __cplusplus
}
#endif

#endif /* MAPVIEW_OVERLAY_H */
