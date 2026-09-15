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
 *   filtering and the fog of war look - for the code that
 *   draws with them.  Every value defaults to 0, which is
 *   what the game did before these settings existed.
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

/* What both views wash ground the player cannot see into with.  Read by the
   classic view's tile pass and by the full screen map's fog pass; fog_look.h
   says what each value looks like.  Grey is the default, which is what the
   game drew before the setting existed. */
FogStyle          gfxGetFogStyle(void);
void              gfxSetFogStyle(FogStyle v);

#ifdef __cplusplus
}
#endif

#endif /* GFX_SETTINGS_H */
