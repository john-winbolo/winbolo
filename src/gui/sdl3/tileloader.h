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
 * NAME:          tileLoaderSetSkin / tileLoaderGetSkin
 * PURPOSE:
 *   Select a skin directory under data/skins/.  When set
 *   (non-empty), tileLoaderBuildSheet looks first in
 *   data/skins/<name>/<spritename>.svg|png and falls back
 *   to data/svg/<spritename>.svg|png if missing.  Pass an
 *   empty string or NULL to disable the skin override.
 *********************************************************/
void        tileLoaderSetSkin(const char *name);
const char *tileLoaderGetSkin(void);

/* True when the active skin is "ingamerotate"-style: only the
 * north-facing (_00) sprite is on disk for tanks/boats/shells, and
 * the renderer is expected to rotate it for the other 15 (or 256)
 * directions at draw time.  Detected by name suffix _ingamerotate. */
bool tileLoaderSkinRotates(void);

/* Skin metadata loaded from data/skins/<active>/skin.ini.  All
 * fields are NUL-terminated strings; max_pixel_density is the
 * skin author's declared cap on density (default 1 — "classic
 * Bolo only").  Strings are owned by tileloader; copy if you need
 * to keep them across a tileLoaderSetSkin call. */
typedef struct {
    char  name[64];
    char  author[64];
    char  email[128];
    char  website[256];
    char  release_date[32];
    int   max_pixel_density;   /* 1, 2, 3, 4, … */
    bool  has_ini;             /* false = defaults; skin.ini missing */
} TileLoaderSkinInfo;

/* Returns the active skin's metadata.  Always non-NULL — defaults
 * (max_pixel_density=1, all strings empty, has_ini=false) when the
 * skin has no skin.ini or no skin is active.  The pointer is
 * valid until the next tileLoaderSetSkin call. */
const TileLoaderSkinInfo *tileLoaderGetSkinInfo(void);

/* Density coverage of the active skin.  Filled in once at skin
 * load.  density (1..max_pixel_density) maps to:
 *   0 = no tiles at all at this density
 *   1 = some tiles at this density (partial coverage)
 *   2 = all tiles at this density (full coverage)
 * Density 1 is always 2 (every base sprite has at least the 1×
 * default).  SVG covers densities up to skinInfo.max_pixel_density. */
int tileLoaderGetDensityCoverage(int density);

/* Highest "all tiles" density at or below cap.  Used by the
 * Match-to-zoom path to pick the atlas resolution.  Always returns
 * at least 1. */
int tileLoaderGetAllTilesDensityAtMost(int cap);

/* Per-sprite max density actually available — what the High Detail
 * path uses.  spriteName is one of gTileMap[]'s names ("tank_self_00",
 * "shell_03", etc.).  Returns 1 when no skin prefix or SVG provides
 * higher detail. */
int tileLoaderGetSpriteMaxDensity(const char *spriteName);

/* True when the active skin actually ships a hand-crafted sprite
 * for <baseName>_<NN> (e.g. "tank_selfboat", 5).  Checks .svg, .png,
 * and prefixed N-<baseName>_<NN>.png variants under
 * data/skins/<active>/.  Used by mapview for ingamerotate skins:
 * if the user provided the per-direction file, render code should
 * use the atlas slot directly instead of rotating _00.  Result is
 * cached per (baseName, skin) so per-frame use is cheap. */
bool tileLoaderSkinHasSprite(const char *baseName, int dir);

/* Enumerate sprite names missing from the active skin at the given
 * density.  Fills outBuf with newline-separated names (NUL-terminated)
 * and returns the count of missing sprites.  When the active skin is
 * empty (default sprites), returns 0.  outBuf may be NULL to query
 * just the count. */
int tileLoaderGetMissingSprites(int density, char *outBuf, int outBufSize);

/* Same as tileLoaderGetMissingSprites but for an arbitrary skin name
 * (does not change the active skin).  Useful for previewing a
 * dropdown selection before the user clicks Apply. */
int tileLoaderQueryMissingSprites(const char *skinName, int density,
                                   char *outBuf, int outBufSize);

/* Read skin metadata (skin.ini fields and max_pixel_density) for an
 * arbitrary skin name into *out.  Pure read; does not change the
 * active skin.  Useful for previewing dropdown selections. */
void tileLoaderQuerySkinInfo(const char *skinName, TileLoaderSkinInfo *out);

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
