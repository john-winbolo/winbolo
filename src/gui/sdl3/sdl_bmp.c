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

#include "sdl_bmp.h"

#define COLOR_KEY_R 0
#define COLOR_KEY_G 255
#define COLOR_KEY_B 0

SDL_Texture *sdlLoadBmpStreamAsTexture(SDL_Renderer *renderer,
                                       SDL_IOStream *src, bool closeio,
                                       bool useColorKey) {
    SDL_Surface *surface = SDL_LoadBMP_IO(src, closeio);
    if (!surface) {
        return NULL;
    }

    if (!useColorKey) {
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_DestroySurface(surface);
        return texture;
    }

    /* Convert to ARGB8888 so the color key works alongside alpha
     * blending — anti-aliased edges from text or scaled blits stay
     * smooth instead of fringing. */
    SDL_Surface *converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(surface);
    if (!converted) {
        return NULL;
    }

    const SDL_PixelFormatDetails *fmt = SDL_GetPixelFormatDetails(converted->format);
    Uint32 colorKey = SDL_MapRGB(fmt, NULL, COLOR_KEY_R, COLOR_KEY_G, COLOR_KEY_B);
    SDL_SetSurfaceColorKey(converted, true, colorKey);

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, converted);
    SDL_DestroySurface(converted);
    if (!texture) {
        return NULL;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

SDL_Texture *sdlLoadBmpAsTexture(SDL_Renderer *renderer,
                                 const char *path,
                                 bool useColorKey) {
    SDL_IOStream *src = SDL_IOFromFile(path, "rb");
    if (!src) {
        return NULL;
    }
    return sdlLoadBmpStreamAsTexture(renderer, src, true, useColorKey);
}
