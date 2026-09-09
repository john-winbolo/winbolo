/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Ping Icons
 *Filename:      ping_icons.c
 *Purpose:       See ping_icons.h.
 *********************************************************/

#include "ping_icons.h"

#include <string.h>

#include "nanosvg.h"
#include "nanosvgrast.h"

#include "../ping_kinds.h"

/* Rasterised well above the largest size anything draws them at — the world
 * marker is two map squares wide, which is 128px at 4x zoom — so scaling down
 * stays clean and there is only ever one texture per kind. */
#define PING_ICON_TEX_PX 128

/* Keep the strokes off the edge of the bitmap: the art is fitted to its own
 * drawn bounds, and a stroke centred on the outermost path would otherwise be
 * clipped in half. */
#define PING_ICON_MARGIN 0.08f

static SDL_Texture  *s_icons[PING_KIND_COUNT];
/* Which renderer the textures belong to, so a caller that has no obvious
   one-time setup hook (the log viewer draws straight from its frame loop) can
   call pingIconsInit every frame and pay for it once. Cleared by the
   shutdown, so a new renderer at the same address still reloads. */
static SDL_Renderer *s_owner = NULL;

/* Rasterise one SVG, fitted to the union of its drawn shapes rather than to
 * its declared page, and force the colour channels white so the alpha is the
 * only thing the file contributes. Mirrors imguiLoadSvgIconWhiteFit, which
 * does the same for the dialogs; this copy exists because the log viewer
 * needs it and has no ImGui context to reach that header through. */
static SDL_Texture *loadIcon(SDL_Renderer *rend, const char *path) {
    NSVGimage      *image;
    NSVGrasterizer *rast;
    SDL_Surface    *surface;
    SDL_Texture    *tex;
    unsigned char  *pixels;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    float cw, ch, avail, scale, offX, offY;
    bool  any = false;
    int   i;
    const int size = PING_ICON_TEX_PX;

    image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return NULL;

    {
        NSVGshape *sh;
        for (sh = image->shapes; sh; sh = sh->next) {
            if (!(sh->flags & NSVG_FLAGS_VISIBLE)) continue;
            if (sh->bounds[0] < minx) minx = sh->bounds[0];
            if (sh->bounds[1] < miny) miny = sh->bounds[1];
            if (sh->bounds[2] > maxx) maxx = sh->bounds[2];
            if (sh->bounds[3] > maxy) maxy = sh->bounds[3];
            any = true;
        }
    }
    cw = maxx - minx;
    ch = maxy - miny;
    if (!any || cw < 1e-3f || ch < 1e-3f) { nsvgDelete(image); return NULL; }

    avail = (float)size * (1.0f - 2.0f * PING_ICON_MARGIN);
    scale = avail / (cw > ch ? cw : ch);
    offX  = ((float)size - cw * scale) * 0.5f - minx * scale;
    offY  = ((float)size - ch * scale) * 0.5f - miny * scale;

    pixels = (unsigned char *)SDL_malloc((size_t)(size * size * 4));
    if (!pixels) { nsvgDelete(image); return NULL; }
    memset(pixels, 0, (size_t)(size * size * 4));

    rast = nsvgCreateRasterizer();
    if (!rast) { SDL_free(pixels); nsvgDelete(image); return NULL; }
    nsvgRasterize(rast, image, offX, offY, scale, pixels, size, size, size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    for (i = 0; i < size * size; i++) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 255;
        pixels[i * 4 + 2] = 255;
    }

    surface = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32,
                                    pixels, size * 4);
    if (!surface) { SDL_free(pixels); return NULL; }
    tex = SDL_CreateTextureFromSurface(rend, surface);
    SDL_DestroySurface(surface);
    SDL_free(pixels);
    if (tex) {
        /* Every drawer tints and fades these, so the texture has to carry the
           modulation and blend the alpha it was authored with. */
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    return tex;
}

void pingIconsShutdown(void) {
    int i;
    s_owner = NULL;
    for (i = 0; i < PING_KIND_COUNT; i++) {
        if (s_icons[i]) SDL_DestroyTexture(s_icons[i]);
        s_icons[i] = NULL;
    }
}

void pingIconsInit(SDL_Renderer *renderer) {
    int i;
    if (renderer != NULL && renderer == s_owner) return;   /* already loaded */
    pingIconsShutdown();
    if (!renderer) return;
    s_owner = renderer;
    for (i = 0; i < PING_KIND_COUNT; i++) {
        char path[1024];
        SDL_snprintf(path, sizeof(path), "data/ui/ping/%s",
                     kPingKindStyles[i].iconFile);
        s_icons[i] = loadIcon(renderer, path);
        if (!s_icons[i]) {
            /* An installed build and a macOS bundle resolve data/ through the
               base path rather than the working directory — the same two-step
               every other loose asset read does. */
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(path, sizeof(path), "%sdata/ui/ping/%s", base,
                             kPingKindStyles[i].iconFile);
                s_icons[i] = loadIcon(renderer, path);
            }
        }
    }
}

SDL_Texture *pingIconTexture(unsigned char kind) {
    if (kind >= PING_KIND_COUNT) return NULL;
    return s_icons[kind];
}
