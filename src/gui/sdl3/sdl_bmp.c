/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "sdl_bmp.h"

/* For the key colour's tolerance band and the edge bleed, so a sprite sheet
 * loaded here and one cut out of the tile atlas agree on both. */
#include "tileloader.h"

SDL_Texture *sdlLoadBmpStreamAsTexture(SDL_Renderer *renderer,
                                       SDL_IOStream *src, bool closeio) {
    SDL_Surface *surface = SDL_LoadBMP_IO(src, closeio);
    if (!surface) {
        return NULL;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

SDL_Texture *sdlLoadBmpAsTexture(SDL_Renderer *renderer, const char *path) {
    SDL_IOStream *src = SDL_IOFromFile(path, "rb");
    if (!src) {
        return NULL;
    }
    return sdlLoadBmpStreamAsTexture(renderer, src, true);
}

SDL_Surface *sdlLoadBmpSheetSurface(const char *path, int cellW, int cellH) {
    SDL_Surface *raw = SDL_LoadBMP(path);
    if (!raw) {
        return NULL;
    }

    /* RGBA32 rather than ARGB8888: it is what tileLoaderBleedEdges works on,
       and what lets the key pass below index the channels directly. */
    SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (!rgba) {
        return NULL;
    }

    /* Alpha only, the same as the tile sheet's own key pass. The colour left
       behind is the bleed's problem, immediately below. */
    for (int y = 0; y < rgba->h; y++) {
        unsigned char *row = (unsigned char *)rgba->pixels +
                             (size_t)y * (size_t)rgba->pitch;
        for (int x = 0; x < rgba->w; x++) {
            unsigned char *p = row + (size_t)x * 4;
            if (tileLoaderIsSheetKeyColor(p[0], p[1], p[2])) p[3] = 0;
        }
    }

    if (cellW > 0 && cellH > 0) {
        /* Per cell, because the cells touch. A partial cell at the right or
           bottom edge is clipped by tileLoaderBleedEdges itself. */
        for (int y = 0; y < rgba->h; y += cellH) {
            for (int x = 0; x < rgba->w; x += cellW) {
                SDL_Rect cell = { x, y, cellW, cellH };
                tileLoaderBleedEdges(rgba, &cell);
            }
        }
    } else {
        tileLoaderBleedEdges(rgba, NULL);
    }

    return rgba;
}

SDL_Texture *sdlLoadBmpSheetAsTexture(SDL_Renderer *renderer, const char *path,
                                      int cellW, int cellH) {
    SDL_Surface *sheet = sdlLoadBmpSheetSurface(path, cellW, cellH);
    if (!sheet) {
        return NULL;
    }

    /* No colour key is set on the surface: the transparency is already in its
       alpha, and asking SDL to key it again here would only re-run the
       conversion the bleed was built to replace. */
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, sheet);
    SDL_DestroySurface(sheet);
    if (!texture) {
        return NULL;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    /* Explicit: a new SDL3 texture takes the renderer's default, which is
       linear. */
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    return texture;
}
