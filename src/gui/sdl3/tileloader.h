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
 *   of all three, at the density the player's Tile Detail
 *   setting asks for.
 *   Returns NULL on failure.  Caller owns the surface.
 *********************************************************/
SDL_Surface *tileLoaderBuildSheet(int tileSize);

/*********************************************************
 * NAME:          tileLoaderBuildSheetFor
 * PURPOSE:
 *   tileLoaderBuildSheet against a caller-chosen skin,
 *   under a TILE_DETAIL_* mode.
 *   tileLoaderPickDensity says what density N each sprite
 *   is wanted at.  Above 1, these come first:
 *     1. skin <name>@<N>x.png
 *     2. skin <name>@<M>x.png, smallest M above N
 *     3. skin <name>.svg, unless MaxPixelDensity caps
 *        the skin below N
 *     4. skin <name>@<M>x.png, largest M below N, M >= 2
 *   Then, at N == 1 and whenever none of those loaded:
 *     5. skin <name>.svg
 *     6. skin <name>.png
 *     7. crop from the skin's tiles.bmp or skin.bmp
 *     8. data/svg/<name>.svg
 *     9. data/svg/<name>.png
 *    10. crop from data/skin.bmp
 *   Every one of them is decoded straight to the sprite's
 *   slot in the sheet, point-sampled when the file is not
 *   already that size.
 *   A NULL skin skips 1-7 and builds the stock sheet.
 *   Returns NULL on failure.  Caller owns the surface.
 *********************************************************/
SDL_Surface *tileLoaderBuildSheetFor(struct SkinSource *skin, int tileSize,
                                     int mode);

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

/* True when a sheet pixel is the transparent green a whole-sheet BMP uses.
   Not an exact match: an anti-aliased sheet carries near-greens beside the
   pure one, and leaving those opaque puts a green fringe on every sprite.
   The band is narrow on purpose - wide enough for encoding noise, not wide
   enough to reach green art. */
bool tileLoaderIsSheetKeyColor(Uint8 r, Uint8 g, Uint8 b);

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

/* Draw-time tank rotation. Off: the sheet build already turns a rotating
   skin's north sprite into the group's other fifteen slots, which is what
   makes such a skin render correctly everywhere. Turning the north slot
   again at draw time buys the tank's full 0..255 angle instead of sixteen
   steps, and on a 16x16 sprite that is close to invisible — one bradian
   moves the outermost pixel about 0.2 px, which nearest sampling rounds
   away.

   Before switching this on, move the lookup out of the draw loop.
   tileLoaderRotatedSource scans the rotation table per tank per frame,
   keyed on sheet coordinates; the sheet build already knows the answer and
   should hand the renderer something it can index directly. */
#define WB_SKIN_DRAWTIME_ROTATION 0

/*********************************************************
 * NAME:          tileLoaderRotatedSource
 * PURPOSE:
 *   When the sheet slot at (srcX, srcY) — 1x sheet
 *   coordinates, as gTileMap[] and tiles.h give them —
 *   was filled by rotating its group's north sprite,
 *   writes that sprite's slot position to *baseX / *baseY
 *   and returns true.  A group's north slot reports
 *   itself, so a caller holding an angle finer than the
 *   sixteen frames can rotate that facing too.
 *   False when the slot holds art of its own, which a
 *   hand-drawn per-direction file always does, and for
 *   every slot of a skin that does not ask for rotation.
 *   Describes the sheet the last tileLoaderBuildSheetFor
 *   built.
 *   This, and the table behind it, are compiled only when
 *   WB_SKIN_DRAWTIME_ROTATION is on.  The sheet build's
 *   own rotation runs either way.
 *********************************************************/
#if WB_SKIN_DRAWTIME_ROTATION
bool tileLoaderRotatedSource(int srcX, int srcY, int *baseX, int *baseY);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TILELOADER_H */
