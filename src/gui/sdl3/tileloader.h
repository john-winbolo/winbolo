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
