/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
 * from. The texture cache on top serves the main window's ImGui panels. */
#define CACHE_SIZE (26 * 26)
static SDL_Surface *flagSurfCache[CACHE_SIZE];
static SDL_Texture *flagCache[CACHE_SIZE];
static SDL_Renderer *s_renderer = NULL;
static NSVGrasterizer *s_rasterizer = NULL;

/* Sentinel values to distinguish "not loaded" from "failed to load" */
#define FAILED_SENTINEL      ((SDL_Texture *)(uintptr_t)1)
#define FAILED_SURF_SENTINEL ((SDL_Surface *)(uintptr_t)1)

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
    flagsDestroy();
    s_renderer = renderer;
    s_rasterizer = nsvgCreateRasterizer();
    memset(flagCache, 0, sizeof(flagCache));
    memset(flagSurfCache, 0, sizeof(flagSurfCache));
    WB_LOG_INFO(WB_LOG_CAT_ASSET, "[FLAGS] Initialized (SVG-based, lazy loading)");
    return s_rasterizer != NULL;
}

void flagsDestroy(void) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (flagCache[i] && flagCache[i] != FAILED_SENTINEL) {
            SDL_DestroyTexture(flagCache[i]);
        }
        flagCache[i] = NULL;
        if (flagSurfCache[i] && flagSurfCache[i] != FAILED_SURF_SENTINEL) {
            SDL_DestroySurface(flagSurfCache[i]);
        }
        flagSurfCache[i] = NULL;
    }
    if (s_rasterizer) {
        nsvgDeleteRasterizer(s_rasterizer);
        s_rasterizer = NULL;
    }
    s_renderer = NULL;
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

SDL_Texture *flagsGetTexture(const char countryCode[2]) {
    if (!s_renderer) {
        return NULL;
    }

    SDL_Surface *surface = flagsGetSurface(countryCode);
    if (!surface) {
        return NULL;
    }

    char c0 = (char)tolower((unsigned char)countryCode[0]);
    char c1 = (char)tolower((unsigned char)countryCode[1]);
    int idx = (c0 - 'a') * 26 + (c1 - 'a');
    SDL_Texture *cached = flagCache[idx];

    if (cached == FAILED_SENTINEL) {
        return NULL;
    }
    if (cached) {
        return cached;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(s_renderer, surface);
    flagCache[idx] = tex ? tex : FAILED_SENTINEL;
    return tex;
}
