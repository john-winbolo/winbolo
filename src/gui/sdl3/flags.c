/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Flags
 *Filename:      flags.c
 *Purpose:
 *  Country flag rendering from individual SVG files.
 *  Flags are loaded on demand from data/flags/<code>.svg
 *  using nanosvg and cached as SDL textures.
 *********************************************************/

#include "flags.h"
#include <ctype.h>
#include <string.h>
#include <SDL3/SDL_log.h>

#include "../../common/wb_log.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

/* Rasterize flags at this height; width determined by aspect ratio */
#define RASTER_HEIGHT 44
/* Caches: 26x26 grid indexed by (row * 26 + col) where row = c0-'a', col = c1-'a'.
 * NULL means not yet loaded; FAILED_*SENTINEL means load was attempted and
 * failed. The surface cache is the source: renderer-free rasterizations that
 * per-renderer consumers (the tank-label caches) build their own textures
 * from. The texture caches on top are per renderer — a texture may only be
 * drawn through the renderer that created it, so the players pop-out window
 * builds its own copies from the shared surfaces rather than sharing the game
 * window's. Two slots: the game window and the one pop-out that draws flags. */
#define CACHE_SIZE (26 * 26)
#define SLOT_COUNT 2

static SDL_Surface *flagSurfCache[CACHE_SIZE];
static SDL_Texture *flagCache[SLOT_COUNT][CACHE_SIZE];
static SDL_Renderer *s_renderer[SLOT_COUNT];
static NSVGrasterizer *s_rasterizer = NULL;

/* Sentinel values to distinguish "not loaded" from "failed to load" */
#define FAILED_SENTINEL      ((SDL_Texture *)(uintptr_t)1)
#define FAILED_SURF_SENTINEL ((SDL_Surface *)(uintptr_t)1)

/* Slot holding renderer, or -1 if it was never registered. */
static int slotFor(SDL_Renderer *renderer) {
    if (!renderer) return -1;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (s_renderer[i] == renderer) return i;
    }
    return -1;
}

static SDL_Surface *loadFlagSurface(const char c0, const char c1) {
    char path[256];
    SDL_snprintf(path, sizeof(path), "data/flags/%c%c.svg", c0, c1);

    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) {
        return NULL;
    }
    if (image->width < 1.0f || image->height < 1.0f) {
        nsvgDelete(image);
        return NULL;
    }

    /* Scale to fit RASTER_HEIGHT, preserving aspect ratio */
    float scale = (float)RASTER_HEIGHT / image->height;
    int w = (int)(image->width * scale + 0.5f);
    int h = RASTER_HEIGHT;

    /* Rasterize straight into a surface that owns its pixels, so the cached
     * surface outlives this call. */
    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        nsvgDelete(image);
        return NULL;
    }
    memset(surface->pixels, 0, (size_t)surface->pitch * (size_t)h);
    nsvgRasterize(s_rasterizer, image, 0, 0, scale,
                  (unsigned char *)surface->pixels, w, h, surface->pitch);
    nsvgDelete(image);

    WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[FLAGS] Loaded %s (%dx%d)", path, w, h);
    return surface;
}

bool flagsCreate(SDL_Renderer *renderer) {
    if (!s_rasterizer) {
        s_rasterizer = nsvgCreateRasterizer();
        memset(flagSurfCache, 0, sizeof(flagSurfCache));
    }
    if (!renderer) {
        return s_rasterizer != NULL;
    }
    /* Already has a slot — keep the cache it built rather than starting over,
     * and never move it, so a second renderer cannot take slot 0 away from
     * the game window. */
    if (slotFor(renderer) >= 0) {
        return s_rasterizer != NULL;
    }
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!s_renderer[i]) {
            s_renderer[i] = renderer;
            memset(flagCache[i], 0, sizeof(flagCache[i]));
            WB_LOG_INFO(WB_LOG_CAT_ASSET,
                        "[FLAGS] Initialized slot %d (SVG-based, lazy loading)", i);
            return s_rasterizer != NULL;
        }
    }
    WB_LOG_INFO(WB_LOG_CAT_ASSET, "[FLAGS] No free cache slot for renderer %p",
                (void *)renderer);
    return false;
}

void flagsDestroy(void) {
    for (int s = 0; s < SLOT_COUNT; s++) {
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (flagCache[s][i] && flagCache[s][i] != FAILED_SENTINEL) {
                SDL_DestroyTexture(flagCache[s][i]);
            }
            flagCache[s][i] = NULL;
        }
        s_renderer[s] = NULL;
    }
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (flagSurfCache[i] && flagSurfCache[i] != FAILED_SURF_SENTINEL) {
            SDL_DestroySurface(flagSurfCache[i]);
        }
        flagSurfCache[i] = NULL;
    }
    if (s_rasterizer) {
        nsvgDeleteRasterizer(s_rasterizer);
        s_rasterizer = NULL;
    }
}

SDL_Surface *flagsGetSurface(const char countryCode[2]) {
    if (!s_rasterizer || !countryCode) {
        return NULL;
    }

    char c0 = (char)tolower((unsigned char)countryCode[0]);
    char c1 = (char)tolower((unsigned char)countryCode[1]);

    if (c0 < 'a' || c0 > 'z' || c1 < 'a' || c1 > 'z') {
        return NULL;
    }

    int idx = (c0 - 'a') * 26 + (c1 - 'a');
    SDL_Surface *cached = flagSurfCache[idx];

    if (cached == FAILED_SURF_SENTINEL) {
        return NULL;
    }
    if (cached) {
        return cached;
    }

    /* First request for this code — try to load */
    SDL_Surface *surface = loadFlagSurface(c0, c1);
    flagSurfCache[idx] = surface ? surface : FAILED_SURF_SENTINEL;
    return surface;
}

static SDL_Texture *getTextureInSlot(int slot, const char countryCode[2]) {
    if (slot < 0 || !s_renderer[slot] || !s_rasterizer || !countryCode) {
        return NULL;
    }

    char c0 = (char)tolower((unsigned char)countryCode[0]);
    char c1 = (char)tolower((unsigned char)countryCode[1]);

    if (c0 < 'a' || c0 > 'z' || c1 < 'a' || c1 > 'z') {
        return NULL;
    }

    int idx = (c0 - 'a') * 26 + (c1 - 'a');
    SDL_Texture *cached = flagCache[slot][idx];

    if (cached == FAILED_SENTINEL) {
        return NULL;
    }
    if (cached) {
        return cached;
    }

    /* First request for this code in this slot — build a texture from the
     * shared surface, rasterizing it now if no other slot has yet. */
    SDL_Surface *surface = flagsGetSurface(countryCode);
    if (!surface) {
        flagCache[slot][idx] = FAILED_SENTINEL;
        return NULL;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(s_renderer[slot], surface);
    flagCache[slot][idx] = tex ? tex : FAILED_SENTINEL;
    return tex;
}

SDL_Texture *flagsGetTexture(const char countryCode[2]) {
    return getTextureInSlot(0, countryCode);
}

SDL_Texture *flagsGetTextureFor(SDL_Renderer *renderer,
                                const char countryCode[2]) {
    return getTextureInSlot(slotFor(renderer), countryCode);
}
