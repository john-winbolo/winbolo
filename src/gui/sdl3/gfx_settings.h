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
 * Name:          gfx_settings.h
 * Purpose:
 *   Holds the graphics settings the player picks - tile
 *   detail, animation smoothness, smooth shells, texture
 *   filtering, the simplified view and the fog of war look -
 *   for the code that draws with them.
 *
 *   The first four default to 0, which is what the game did
 *   before these settings existed. Two are exceptions. The
 *   simplified view defaults to on, because it arrived
 *   already switched on and is what the zoomed-out map is
 *   meant to look like; its sub-option defaults to off,
 *   which leaves it applying everywhere. The fog of war look
 *   defaults to Darker with fog edge (FOG_STYLE_DEFAULT in
 *   fog_look.h), not to the Grey that is style 0.
 *********************************************************/

#ifndef GFX_SETTINGS_H
#define GFX_SETTINGS_H

#include <stdbool.h>

#include "fog_look.h" /* FogStyle - what fog over unseen ground looks like */

#ifdef __cplusplus
extern "C" {
#endif

/* How finely sprites are loaded: at their classic size, at the highest
   density the skin covers in full for the current zoom, or at the best
   density each sprite has of its own. */
typedef enum GfxTileDetail {
    GFX_TILE_DETAIL_CLASSIC    = 0,
    GFX_TILE_DETAIL_MATCH_ZOOM = 1,
    GFX_TILE_DETAIL_HIGH       = 2
} GfxTileDetail;

/* How movement between tiles is drawn. */
typedef enum GfxAnimSmoothness {
    GFX_ANIM_CLASSIC          = 0,
    GFX_ANIM_MATCH_PIXELATION = 1,
    GFX_ANIM_SMOOTH           = 2
} GfxAnimSmoothness;

/* How a texture is sampled when it is not drawn at its own size. */
typedef enum GfxTextureFilter {
    GFX_FILTER_NEAREST  = 0,
    GFX_FILTER_LINEAR   = 1,
    GFX_FILTER_PIXELART = 2
} GfxTextureFilter;

/* All four are read by the drawing code: tile detail picks the size the
   sheet is built at, animation smoothness and smooth shells decide where a
   sprite is put on screen, and the texture filter sets how the sheet is
   sampled.  None of these is dead - do not remove them. */

GfxTileDetail     gfxGetTileDetail(void);
void              gfxSetTileDetail(GfxTileDetail v);
GfxAnimSmoothness gfxGetAnimSmoothness(void);
void              gfxSetAnimSmoothness(GfxAnimSmoothness v);
bool              gfxGetSmoothShells(void);
void              gfxSetSmoothShells(bool v);
GfxTextureFilter  gfxGetTextureFilter(void);
void              gfxSetTextureFilter(GfxTextureFilter v);

/* The simplified view: at the zooms where a 16 px sprite is drawn smaller
   than about 8 px, fill each square with its map colour and draw tanks,
   pills and bases as marker shapes (map_colours.h) instead of sprites.

   Two settings, because the two surfaces that can zoom out are wanted
   separately. Off, every view keeps its sprites at every zoom. On, the
   sub-option decides how far it reaches: cleared, both the Map Overview
   window and the full screen map use it; set, only the Map Overview does,
   and the full screen map keeps its sprites.

   The map choosers ignore both and always draw a zoomed-out map in these
   colours - a whole map in a thumbnail has no sprite to keep. */
bool              gfxGetSimplifiedZoomOut(void);
void              gfxSetSimplifiedZoomOut(bool v);
bool              gfxGetSimplifiedOverviewOnly(void);
void              gfxSetSimplifiedOverviewOnly(bool v);

/* What both views wash ground the player cannot see into with.  Read by the
   classic view's tile pass and by the full screen map's fog pass; fog_look.h
   says what each value looks like, and FOG_STYLE_DEFAULT there is both the
   value this starts at and where gfxSetFogStyle sends anything outside the
   enum. */
FogStyle          gfxGetFogStyle(void);
void              gfxSetFogStyle(FogStyle v);

#ifdef __cplusplus
}
#endif

#endif /* GFX_SETTINGS_H */
