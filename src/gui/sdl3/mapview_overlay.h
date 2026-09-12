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

/* The overview's marker shapes, drawn below 1x in place of sprites: tank
   triangles in this file, pill discs and base squares in overview_view.cpp.
   One allegiance palette and one stroke for all of them, so a tank parked on
   a base is outlined the same way the base is. The stroke is three layers:
   an outline MAPVIEW_MARKER_STROKE_PX / 2 outside the shape at
   MAPVIEW_MARKER_OUTLINE_ALPHA, the fill darkened by MAPVIEW_MARKER_DARKEN
   over the inner half of the stroke, then the plain fill. The colours are the
   replay viewer's. */
#define MAPVIEW_MARKER_STROKE_PX     1.5f
#define MAPVIEW_MARKER_OUTLINE_ALPHA 0.65f
#define MAPVIEW_MARKER_DARKEN        0.35f

/* Functions rather than header constants: a C file that includes this for
   the tile tests alone would otherwise carry three unused statics. */
static inline SDL_FColor mapViewMarkerGood(void) {
  SDL_FColor c = { 88 / 255.0f, 216 / 255.0f, 88 / 255.0f, 1.0f };
  return c;
}

static inline SDL_FColor mapViewMarkerEvil(void) {
  SDL_FColor c = { 255 / 255.0f, 93 / 255.0f, 93 / 255.0f, 1.0f };
  return c;
}

static inline SDL_FColor mapViewMarkerNeutral(void) {
  SDL_FColor c = { 240 / 255.0f, 180 / 255.0f, 41 / 255.0f, 1.0f };
  return c;
}

/* The three layers' colours, outermost first, for a marker filled `fill`. */
static inline void mapViewMarkerShades(SDL_FColor fill, SDL_FColor out[3]) {
  out[0].r = 0.0f;
  out[0].g = 0.0f;
  out[0].b = 0.0f;
  out[0].a = MAPVIEW_MARKER_OUTLINE_ALPHA;
  out[1].r = fill.r * MAPVIEW_MARKER_DARKEN;
  out[1].g = fill.g * MAPVIEW_MARKER_DARKEN;
  out[1].b = fill.b * MAPVIEW_MARKER_DARKEN;
  out[1].a = fill.a;
  out[2] = fill;
}

/* How far outside the shape layer `layer` (0..2) of the stroke reaches. */
static inline float mapViewMarkerLayerGrow(int layer) {
  return MAPVIEW_MARKER_STROKE_PX * 0.5f * (float)(1 - layer);
}

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
