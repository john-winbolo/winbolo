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

#ifndef SDL_BMP_H
#define SDL_BMP_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load an opaque BMP from disk and create an SDL texture. No key colour and
 * no blend mode change - for images that cover what they are drawn over, such
 * as the background or the splash screen.
 *
 * Returns NULL on any failure (file missing, texture creation failed). Caller
 * owns the returned texture and must SDL_DestroyTexture it.
 */
SDL_Texture *sdlLoadBmpAsTexture(SDL_Renderer *renderer, const char *path);

/* The same from an already-open SDL_IOStream, for bytes that did not come
 * from a plain file.
 *
 * If closeio is true the stream is closed whether the load succeeds or fails.
 * A stream wrapping caller-owned memory (SDL_IOFromMem) closes the stream
 * only; the caller still frees the bytes.
 */
SDL_Texture *sdlLoadBmpStreamAsTexture(SDL_Renderer *renderer,
                                       SDL_IOStream *src, bool closeio);

/* Read a BMP sprite sheet into the RGBA32 surface the sheet is drawn from:
 * the key colour turned into transparency, and the colour underneath that
 * transparency replaced with the sprite's own by tileLoaderBleedEdges.
 *
 * `cellW` x `cellH` is the grid the sheet is packed on - 16x16 for the log
 * viewer's tanks, boats and items. The bleed runs per cell, because the cells
 * touch: one pass over the whole surface would spread each sprite into the
 * one packed beside it. A cell size of 0 or less bleeds the surface as a
 * single image.
 *
 * Doing the key here rather than through SDL_SetSurfaceColorKey is what makes
 * the bleed possible at all. SDL's own conversion
 * (SDL_ConvertColorkeyToAlpha) clears the alpha bits and leaves the RGB, so a
 * texture built that way keeps the key colour under its transparency and
 * fringes green wherever a sampler blends across a sprite's edge - the same
 * defect the tile atlas had.
 *
 * Returns NULL on any failure. Caller owns the surface and must
 * SDL_DestroySurface it. Split out from the texture call below so the result
 * can be inspected without a renderer.
 */
SDL_Surface *sdlLoadBmpSheetSurface(const char *path, int cellW, int cellH);

/* sdlLoadBmpSheetSurface uploaded, with alpha blending on and point sampling
 * set explicitly - SDL3 defaults a new texture to linear, which is what would
 * make the fringe visible on any blit that is not 1:1.
 *
 * Returns NULL on any failure. Caller owns the returned texture and must
 * SDL_DestroyTexture it.
 */
SDL_Texture *sdlLoadBmpSheetAsTexture(SDL_Renderer *renderer, const char *path,
                                      int cellW, int cellH);

#ifdef __cplusplus
}
#endif

#endif /* SDL_BMP_H */
