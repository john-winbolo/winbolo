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
 * Name:          sprite_atlas.h
 * Purpose:
 *   A second copy of the moving sprites — tanks, boats,
 *   shells, LGMs, explosions — laid out with a texel of
 *   padding around each, and the lookup that turns a
 *   classic sheet address into a rect on it.
 *********************************************************/

#ifndef SPRITE_ATLAS_H
#define SPRITE_ATLAS_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why this exists.
 *
 * SDL_RenderTexture's source rect picks texture coordinates.  It does not
 * clamp the filter kernel, so under Linear or Pixel Art filtering the
 * sample nearest a rect edge mixes in the texel just outside it.  The
 * classic 496x176 layout packs its slots edge to edge — tank_selfboat_07..15
 * sit directly under pill_evil_02..10 — so that texel is another sprite,
 * and a tank on a boat draws a line of the pillbox's bottom row above
 * itself.  tileLoaderBleedEdges cannot help: it only rewrites transparent
 * texels inside a slot, and the pillbox row is opaque.
 *
 * The classic layout cannot grow gutters without moving every address in
 * tiles.h, which skins are authored against and the map editor and log
 * viewer read.  So the sprites that need it get a second, padded texture
 * and the terrain keeps drawing from the sheet.  Only the sprites are worth
 * the copy: terrain lands on the tile grid at whole pixels, where the
 * sampler has nothing to blend, while a tank under smooth motion sits at a
 * fractional position and blends on every edge.
 *
 * One texel of padding is enough for any of it.  A bilinear kernel reaches
 * half a texel past its sample point and SDL keeps sample points inside the
 * source rect, rotated draws included.
 */
#define SPRITE_ATLAS_GUTTER 1

typedef struct {
    int srcX, srcY;   /* the slot's address on the classic sheet, at 1x */
    int w, h;         /* and its size there, also at 1x */
    int x, y;         /* where it landed on the atlas, in atlas texels */
} SpriteAtlasSlot;

typedef struct {
    SpriteAtlasSlot *slots;   /* sorted by srcY then srcX, for the search */
    int              count;
    int              scale;   /* the sheet scale it was copied out of */
    SDL_Surface     *surface; /* released once it is a texture; see
                                 tileLoaderSpriteAtlasDropSurface */
} SpriteAtlas;

/*********************************************************
 * NAME:          spriteAtlasFind
 * PURPOSE:
 *   The rect on the atlas holding the sprite that lives at
 *   (x, y) size (w, h) on the classic sheet — 1x
 *   coordinates, as tiles.h gives them.  False when the
 *   atlas does not hold that sprite, which is every
 *   terrain slot, and when there is no atlas at all, so a
 *   caller can pass whatever it has and take the classic
 *   sheet as the answer.
 *   Inline, and header-only, so a target that draws
 *   sprites need not link the builder.
 *********************************************************/
static inline bool spriteAtlasFind(const SpriteAtlas *a, int x, int y,
                                   int w, int h, SDL_FRect *out) {
    int lo, hi;

    if (a == NULL || a->count <= 0) return false;

    lo = 0;
    hi = a->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const SpriteAtlasSlot *s = &a->slots[mid];
        if (s->srcY != y) {
            if (s->srcY < y) lo = mid + 1; else hi = mid - 1;
            continue;
        }
        if (s->srcX != x) {
            if (s->srcX < x) lo = mid + 1; else hi = mid - 1;
            continue;
        }
        /* A caller asking for a different size than the slot was built at is
           asking for something this atlas does not hold. */
        if (s->w != w || s->h != h) return false;
        out->x = (float)s->x;
        out->y = (float)s->y;
        out->w = (float)(s->w * a->scale);
        out->h = (float)(s->h * a->scale);
        return true;
    }
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* SPRITE_ATLAS_H */
