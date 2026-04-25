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
 * Name:          tileloader.h
 * Purpose:
 *   Builds the 496x176 RGBA32 sprite sheet from individual
 *   SVG/PNG files in data/svg/, falling back to data/skin.bmp.
 *********************************************************/

#ifndef TILELOADER_H
#define TILELOADER_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
 * NAME:          tileLoaderBuildSheet
 * PURPOSE:
 *   Assembles a 496x176 RGBA32 surface matching the legacy
 *   skin.bmp layout.  For each sprite in the tilemap:
 *     1. Try data/svg/<name>.svg  (nanosvg rasterize)
 *     2. Try data/svg/<name>.png  (stb_image load)
 *     3. Fall back to blitting from data/skin.bmp
 *   Returns NULL on failure.  Caller owns the surface.
 *********************************************************/
SDL_Surface *tileLoaderBuildSheet(int tileSize);

/*********************************************************
 * NAME:          tileLoaderSetTheme / tileLoaderGetTheme
 * PURPOSE:
 *   Select a theme directory under data/theme/.  When set
 *   (non-empty), tileLoaderBuildSheet looks first in
 *   data/theme/<name>/<spritename>.svg|png and falls back
 *   to data/svg/<spritename>.svg|png if missing.  Pass an
 *   empty string or NULL to disable the theme override.
 *********************************************************/
void        tileLoaderSetTheme(const char *name);
const char *tileLoaderGetTheme(void);

/* True when the active theme is "ingamerotate"-style: only the
 * north-facing (_00) sprite is on disk for tanks/boats/shells, and
 * the renderer is expected to rotate it for the other 15 (or 256)
 * directions at draw time.  Detected by name suffix _ingamerotate. */
bool tileLoaderThemeRotates(void);

/* Rotation-group identifiers for the ingamerotate texture cache.
 * Tank groups 0..5 must match the engine's screenTanks `frame >> 4`
 * encoding so mapview.c can use that directly as the lookup key. */
typedef enum {
    TLR_GROUP_TANK_SELF     = 0,  /* TANK_SELF_0     == 0  */
    TLR_GROUP_TANK_SELFBOAT = 1,  /* TANK_SELFBOAT_0 == 16 */
    TLR_GROUP_TANK_GOOD     = 2,  /* TANK_GOOD_0     == 32 */
    TLR_GROUP_TANK_GOODBOAT = 3,  /* TANK_GOODBOAT_0 == 48 */
    TLR_GROUP_TANK_EVIL     = 4,  /* TANK_EVIL_0     == 64 */
    TLR_GROUP_TANK_EVILBOAT = 5,  /* TANK_EVILBOAT_0 == 80 */
    TLR_GROUP_SHELL         = 6,
    TLR_GROUP_COUNT         = 7
} TileLoaderRotGroup;

/* Per-group geometry used by both the cache builder and the render
 * path so they agree on cache size + pivot placement.  cacheSize is
 * in unscaled game pixels (atlas-1× units); pivotX/Y is the pixel
 * inside the cache that the render path should align with the
 * sprite's world position (sprite centre for tanks, tip pixel for
 * shells). */
typedef struct {
    int   cacheSize;
    float pivotX;
    float pivotY;
} TileLoaderRotInfo;

bool tileLoaderGetRotInfo(TileLoaderRotGroup group, TileLoaderRotInfo *out);

/* Build per-direction rotated textures for the active ingamerotate
 * theme.  Re-builds from scratch each call (drops the prior cache
 * first).  Renderer is required because we create SDL_Textures here.
 * No-op when tileLoaderThemeRotates() is false. */
void tileLoaderBuildRotatedCache(SDL_Renderer *renderer);

/* Drop all cached rotated textures.  Safe to call any time. */
void tileLoaderClearRotatedCache(void);

/* Look up the cached rotated texture for (group, dir).  Returns
 * NULL if no cache exists for the group, or for direction 0
 * (callers should keep using the atlas slot for the N-facing
 * default).  dir is 0..15. */
SDL_Texture *tileLoaderGetRotatedTexture(TileLoaderRotGroup group, int dir);

/*********************************************************
 * NAME:          tileLoaderCleanup
 * PURPOSE:
 *   Frees any cached state held by the tile loader.
 *********************************************************/
void tileLoaderCleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* TILELOADER_H */
