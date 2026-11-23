/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
#include "map_colours.h"  /* the pill and base tile ranges, and the marker palette */
#include "client_sim.h"   /* OverviewItemLabel */
#include "mapview.h"
#include "tank_label.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Squares and game pixels here are in the sprite lists' frame: the classic
   view's 17x17 buffer squares, the overview's absolute map squares. */
typedef struct MapViewOverlay {
  /* Draw each tank as a triangle pointing the way it faces, in the shared
     marker palette (map_colours.h), instead of its sprite. The overview
     asks for this at the zooms where a tank sprite is a smudge. */
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
     Each is drawn as the Log Viewer draws its pillbox numbers: the small
     square-cell digits of its builder marker (logviewer/lv_builder_draw.h),
     white with a black edge, at the bottom right corner of the square. */
  const OverviewItemLabel *itemLabels;
  int                      itemLabelCount;
  float                    itemLabelMinScale;

  /* Labels are clipped to this box in renderer coordinates: a name's left
     edge is held at clipLeft, and nothing is drawn outside the box. */
  float            clipLeft, clipTop, clipRight, clipBottom;
} MapViewOverlay;

/* Whether the tile drawn on a square is a pillbox or a base: the test both
   views make before putting a number on the square. The ranges themselves
   live in map_colours.h, which the marker passes classify squares with, so
   the numbers and the markers cannot disagree about what is on a square. */
static inline bool mapViewTileIsPill(BYTE tile) {
  MapColourItem kind = mapColourItemKind(tile);
  return kind == MAP_COLOUR_ITEM_PILL_GOOD || kind == MAP_COLOUR_ITEM_PILL_EVIL;
}

static inline bool mapViewTileIsBase(BYTE tile) {
  return mapColourItemKind(tile) >= MAP_COLOUR_ITEM_BASE_GOOD;
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
