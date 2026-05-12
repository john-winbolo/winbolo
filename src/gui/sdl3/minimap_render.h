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
 * Name:          minimap_render.h
 * Purpose:       Shared 256x256 minimap renderer used by
 *                the map editor, lobby, and map chooser.
 *********************************************************/

#ifndef MINIMAP_RENDER_H
#define MINIMAP_RENDER_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MINIMAP_SIZE 256

typedef struct {
    int minX, minY, maxX, maxY;
} MinimapBounds;

/* Get the canonical terrain colour for a raw terrain byte.
 * Handles mine stripping internally. */
void minimapTerrainColor(BYTE terrain, uint8_t *r, uint8_t *g, uint8_t *b);

/* Render a 256x256 minimap into an RGBA pixel buffer.
 * pixels: caller-provided, at least MINIMAP_SIZE * MINIMAP_SIZE * 4 bytes, row-major RGBA32.
 * bounds: if non-NULL, filled with bounding box of non-sea terrain.
 * flags: bitmask controlling optional effects. */
#define MINIMAP_DARKEN_BORDER  (1 << 0)  /* Dim mine border zone */
#define MINIMAP_DARKEN_MINES   (1 << 1)  /* Slightly darken mined tiles */

void minimapRenderPixels(const struct mapObj *mp,
                         const struct basesObj *bs,
                         const struct pillsObj *pb,
                         const struct startsObj *ss,
                         uint8_t *pixels,
                         MinimapBounds *bounds,
                         uint32_t flags);

/* Render object markers (3x3 dots) into an existing pixel buffer.
 * pillColor/baseColor/startColor: RGB triplets. Pass NULL to skip that object type. */
void minimapDrawObjects(uint8_t *pixels,
                        const struct basesObj *bs,
                        const struct pillsObj *pb,
                        const struct startsObj *ss,
                        const uint8_t pillColor[3],
                        const uint8_t baseColor[3],
                        const uint8_t startColor[3]);

/* Convenience: render to a new SDL_Texture (256x256 RGBA). Caller owns texture. */
SDL_Texture *minimapCreateTexture(SDL_Renderer *renderer,
                                  const struct mapObj *mp,
                                  const struct basesObj *bs,
                                  const struct pillsObj *pb,
                                  const struct startsObj *ss,
                                  MinimapBounds *bounds,
                                  uint32_t flags);

/* Convenience: build from compressed map data (for network lobby).
 * Decompresses internally, renders, returns texture. */
SDL_Texture *minimapFromCompressed(SDL_Renderer *renderer,
                                   const BYTE *compressedData, int dataLen,
                                   MinimapBounds *bounds,
                                   int *outPills, int *outBases, int *outStarts);

/* Convenience: build from a .map file path (for map chooser). */
SDL_Texture *minimapFromFile(SDL_Renderer *renderer, const char *mapPath,
                             MinimapBounds *bounds,
                             int *outPills, int *outBases, int *outStarts);

#ifdef __cplusplus
}
#endif

#endif /* MINIMAP_RENDER_H */
