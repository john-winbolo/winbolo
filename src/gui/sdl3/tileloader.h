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
 * Name:          tileloader.h
 * Purpose:
 *   Builds the 496x176 RGBA32 sprite sheet from individual
 *   SVG/PNG files in data/svg/, falling back to data/skin.bmp.
 *********************************************************/

#ifndef TILELOADER_H
#define TILELOADER_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SkinSource;

/*********************************************************
 * NAME:          tileLoaderBuildSheet
 * PURPOSE:
 *   Assembles a 496x176 RGBA32 surface matching the legacy
 *   skin.bmp layout.  For each sprite in the tilemap:
 *     1. Try data/svg/<name>.svg  (nanosvg rasterize)
 *     2. Try data/svg/<name>.png  (stb_image load)
 *     3. Fall back to blitting from data/skin.bmp
 *   The active skin, when there is one, is consulted ahead
 *   of all three.
 *   Returns NULL on failure.  Caller owns the surface.
 *********************************************************/
SDL_Surface *tileLoaderBuildSheet(int tileSize);

/*********************************************************
 * NAME:          tileLoaderBuildSheetFor
 * PURPOSE:
 *   tileLoaderBuildSheet against a caller-chosen skin.
 *   Each sprite resolves in this order:
 *     1. skin <name>.svg
 *     2. skin <name>.png
 *     3. crop from the skin's tiles.bmp or skin.bmp
 *     4. data/svg/<name>.svg
 *     5. data/svg/<name>.png
 *     6. crop from data/skin.bmp
 *   A NULL skin skips 1-3 and builds the stock sheet.
 *   Returns NULL on failure.  Caller owns the surface.
 *********************************************************/
SDL_Surface *tileLoaderBuildSheetFor(struct SkinSource *skin, int tileSize);

/*********************************************************
 * NAME:          tileLoaderCleanup
 * PURPOSE:
 *   Frees any cached state held by the tile loader.
 *********************************************************/
void tileLoaderCleanup(void);

/* Highest density the coverage scan looks for, and the ceiling on how many
   gTileMap[] entries the per-sprite results can hold. */
#define SKIN_DENSITY_MAX          8
#define SKIN_DENSITY_MAX_SPRITES  512

typedef enum SkinDensityCoverage {
    SKIN_DENSITY_COVER_NONE = 0,
    SKIN_DENSITY_COVER_SOME,
    SKIN_DENSITY_COVER_ALL
} SkinDensityCoverage;

/* Which densities a skin can serve. Density N means N times each sprite's
   own gTileMap[] size, not a fixed 16px. */
typedef struct SkinDensityInfo {
    int           spriteCount;                          /* gTileMap entries scanned */
    unsigned char coverage[SKIN_DENSITY_MAX + 1];       /* SkinDensityCoverage; [0] unused */
    unsigned char spriteMax[SKIN_DENSITY_MAX_SPRITES];  /* best density per sprite, >= 1 */
    int           highestAll;   /* highest density with ALL coverage; always >= 1 */
    int           highestAny;   /* highest density any sprite has; always >= 1 */
} SkinDensityInfo;

/* Tile Detail modes. Mirrors GfxTileDetail so tileloader needs no dependency
   on the settings module; gfx_settings.c static-asserts that the two agree. */
#define TILE_DETAIL_CLASSIC      0
#define TILE_DETAIL_MATCH_ZOOM   1
#define TILE_DETAIL_HIGH         2

/* The multiple a whole-sheet BMP is drawn at, read from its header alone:
   1 for 496x176, 2 for 992x352, and so on. 0 when the bytes are not a BMP
   or its size is not a whole multiple of the sheet layout on both axes. */
int tileLoaderSheetDensityFromBmp(const void *buf, size_t len);

/*********************************************************
 * NAME:          tileLoaderScanDensity
 * PURPOSE:
 *   Records which densities a skin can serve, per density
 *   and per sprite.  Name-index lookups plus one read of
 *   the skin's whole sheet, whose header says what
 *   multiple it is drawn at; no image is decoded, so this
 *   is cheap enough to run whenever the active skin
 *   changes.  A whole sheet at N covers every sprite up
 *   to N.
 *   HUD art is left out of the per-density coverage so
 *   a skin that redraws only the world still counts as
 *   covering a density in full.  A NULL skin reports
 *   density 1 and nothing above it.
 *********************************************************/
void tileLoaderScanDensity(struct SkinSource *skin, SkinDensityInfo *out);

/*********************************************************
 * NAME:          tileLoaderGetDensityInfo
 * PURPOSE:
 *   tileLoaderScanDensity behind a one-entry cache keyed
 *   on the source pointer, so a sheet build does not
 *   rescan per sprite.  Never returns NULL.
 *********************************************************/
const SkinDensityInfo *tileLoaderGetDensityInfo(struct SkinSource *skin);

/*********************************************************
 * NAME:          tileLoaderPickDensity
 * PURPOSE:
 *   The density to load one sprite at under a Tile Detail
 *   mode.  Classic is always 1; Match to Zoom is the
 *   highest fully covered density that fits the sheet
 *   scale; High Detail is the best that sprite alone has.
 *   Returns 1 for anything malformed.
 *********************************************************/
int  tileLoaderPickDensity(const SkinDensityInfo *info, int spriteIndex,
                           int mode, int scale);

#ifdef __cplusplus
}
#endif

#endif /* TILELOADER_H */
