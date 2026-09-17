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
 * Name:          minimap_render.c
 * Purpose:       Shared 256x256 minimap renderer used by
 *                the map editor, lobby, and map chooser.
 *********************************************************/

#include "minimap_render.h"
#include "client_mappreview.h"
#include "global.h"  /* MAP_MINE_EDGE_*, MINE_START/END/SUBTRACT, terrain constants */
#include "types.h"   /* struct mapObj/pillsObj/basesObj/startsObj layouts */
#include "map_colours.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void minimapTerrainColor(BYTE terrain, uint8_t *r, uint8_t *g, uint8_t *b) {
    /* Strip mine overlay */
    if (terrain >= MINE_START && terrain <= MINE_END) {
        terrain -= MINE_SUBTRACT;
    }
    switch (terrain) {
        case BUILDING:      *r = 128; *g = 128; *b = 128; break;
        case RIVER:         *r =   0; *g =  80; *b = 200; break;
        case SWAMP:         *r =   0; *g = 100; *b =   0; break;
        case CRATER:        *r = 139; *g =  90; *b =  43; break;
        case ROAD:          *r =  80; *g =  80; *b =  80; break;
        case FOREST:        *r =   0; *g = 140; *b =   0; break;
        case RUBBLE:        *r = 180; *g = 160; *b = 130; break;
        case GRASS:         *r = 100; *g = 200; *b =  50; break;
        case HALFBUILDING:  *r = 160; *g = 160; *b = 160; break;
        case BOAT:          *r =   0; *g =  80; *b = 200; break;
        case DEEP_SEA:
        default:            *r =   0; *g =   0; *b =  80; break;
    }
}

void minimapRenderPixels(const MapPreview *view,
                         uint8_t *pixels,
                         MinimapBounds *bounds,
                         uint32_t flags) {
    int minX = MINIMAP_SIZE, minY = MINIMAP_SIZE, maxX = 0, maxY = 0;
    int x, y;
    const struct mapObj    *mp = clientMapPreviewMap(view);
    const struct pillsObj  *pb = clientMapPreviewPills(view);
    const struct basesObj  *bs = clientMapPreviewBases(view);
    const struct startsObj *ss = clientMapPreviewStarts(view);

    for (y = 0; y < MINIMAP_SIZE; y++) {
        for (x = 0; x < MINIMAP_SIZE; x++) {
            int idx = (y * MINIMAP_SIZE + x) * 4;
            BYTE raw = mp->mapItem[x][y];
            BYTE terrain = raw;
            bool isMined = false;

            if (raw >= MINE_START && raw <= MINE_END) {
                isMined = true;
                terrain = (BYTE)(raw - MINE_SUBTRACT);
            }

            /* mapColourTerrain takes raw terrain, so the mine strip above is
               work it would have done anyway; minimapTerrainColor strips too.
               Neither can refuse a terrain a map file holds, so the fallback
               only catches a byte no terrain uses. */
            if (flags & MINIMAP_EDIT_PALETTE) {
                minimapTerrainColor(terrain, &pixels[idx], &pixels[idx+1],
                                    &pixels[idx+2]);
            } else {
                SDL_Color c = { 0, 0, 0, 255 };
                mapColourTerrain(terrain, &c);
                pixels[idx]   = c.r;
                pixels[idx+1] = c.g;
                pixels[idx+2] = c.b;
            }
            pixels[idx+3] = 255;

            /* Darken mined tiles */
            if ((flags & MINIMAP_DARKEN_MINES) && isMined) {
                pixels[idx]   = (uint8_t)(pixels[idx]   * 0.8f);
                pixels[idx+1] = (uint8_t)(pixels[idx+1] * 0.8f);
                pixels[idx+2] = (uint8_t)(pixels[idx+2] * 0.8f);
            }

            /* Darken border zone */
            if ((flags & MINIMAP_DARKEN_BORDER) &&
                (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                 y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM)) {
                pixels[idx]   = (uint8_t)(pixels[idx]   * 0.5f);
                pixels[idx+1] = (uint8_t)(pixels[idx+1] * 0.5f);
                pixels[idx+2] = (uint8_t)(pixels[idx+2] * 0.5f);
            }

            /* Track non-sea terrain for bounds */
            if (terrain != DEEP_SEA) {
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }
    }

    /* Default bounds if no non-sea terrain found */
    if (minX > maxX) {
        minX = 0; minY = 0;
        maxX = MINIMAP_SIZE - 1; maxY = MINIMAP_SIZE - 1;
    }

    /* Overlay standard object markers */
    if (pb || bs || ss) {
        static const uint8_t pillCol[3]  = {255, 0, 0};     /* red */
        static const uint8_t baseCol[3]  = {255, 255, 255}; /* white */
        static const uint8_t startCol[3] = {255, 255, 0};   /* yellow */
        minimapDrawObjects(pixels, view, pillCol, baseCol, startCol);
    }

    if (bounds) {
        bounds->minX = minX;
        bounds->minY = minY;
        bounds->maxX = maxX;
        bounds->maxY = maxY;
    }
}

/* Optional per-start ownership colouring for the lobby preview. 0-based,
 * parallel to startsObj order: 0=unclaimed, 1=self, 2=ally, 3=enemy, with
 * MINIMAP_OWNER_OFFSIDE possibly set on top (the dot is then dimmed). Set
 * only for the duration of a minimapFromCompressedOwned call (single-thread
 * main-thread use); other callers leave it NULL and get the default colour. */
static const uint8_t *s_startOwnerOverride      = NULL;
static int            s_startOwnerOverrideCount = 0;

/* Map an ownership code (off-side bit already stripped) to a start-dot
 * colour. Returns false for unclaimed (0) so the caller keeps the default
 * (yellow). Self and allies are both green; the caller additionally draws a
 * gray border ring under the self dot so it reads apart from allies.
 * Enemies are red. */
static bool minimapOwnerColor(uint8_t owner, uint8_t out[3]) {
    switch (owner) {
        case 1: out[0] = 0;   out[1] = 210; out[2] = 0;   return true; /* self  green (+gray border) */
        case 2: out[0] = 0;   out[1] = 210; out[2] = 0;   return true; /* ally  green */
        case 3: out[0] = 230; out[1] = 50;  out[2] = 50;  return true; /* enemy red */
        default: return false;                                          /* free  yellow */
    }
}

/* Write one start-dot pixel. A dimmed pixel is blended half into what is
 * already there — the terrain — so an off-side start keeps its ownership
 * hue at half strength rather than vanishing or taking a new colour. */
static void minimapPutStartPixel(uint8_t *pixels, int nx, int ny,
                                 const uint8_t col[3], bool dim) {
    int idx;
    if (nx < 0 || nx >= MINIMAP_SIZE || ny < 0 || ny >= MINIMAP_SIZE) return;
    idx = (ny * MINIMAP_SIZE + nx) * 4;
    if (dim) {
        pixels[idx]   = (uint8_t)((pixels[idx]   + col[0]) / 2);
        pixels[idx+1] = (uint8_t)((pixels[idx+1] + col[1]) / 2);
        pixels[idx+2] = (uint8_t)((pixels[idx+2] + col[2]) / 2);
    } else {
        pixels[idx]   = col[0];
        pixels[idx+1] = col[1];
        pixels[idx+2] = col[2];
    }
    pixels[idx+3] = 255;
}

void minimapDrawObjects(uint8_t *pixels,
                        const MapPreview *view,
                        const uint8_t pillColor[3],
                        const uint8_t baseColor[3],
                        const uint8_t startColor[3]) {
    int i, dx, dy;
    const struct pillsObj  *pb = clientMapPreviewPills(view);
    const struct basesObj  *bs = clientMapPreviewBases(view);
    const struct startsObj *ss = clientMapPreviewStarts(view);

    if (pillColor && pb) {
        for (i = 0; i < pb->numPills; i++) {
            int px = pb->item[i].x;
            int py = pb->item[i].y;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int nx = px + dx, ny = py + dy;
                    if (nx >= 0 && nx < MINIMAP_SIZE && ny >= 0 && ny < MINIMAP_SIZE) {
                        int idx = (ny * MINIMAP_SIZE + nx) * 4;
                        pixels[idx]   = pillColor[0];
                        pixels[idx+1] = pillColor[1];
                        pixels[idx+2] = pillColor[2];
                        pixels[idx+3] = 255;
                    }
                }
            }
        }
    }

    if (baseColor && bs) {
        for (i = 0; i < bs->numBases; i++) {
            int bx = bs->item[i].x;
            int by = bs->item[i].y;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int nx = bx + dx, ny = by + dy;
                    if (nx >= 0 && nx < MINIMAP_SIZE && ny >= 0 && ny < MINIMAP_SIZE) {
                        int idx = (ny * MINIMAP_SIZE + nx) * 4;
                        pixels[idx]   = baseColor[0];
                        pixels[idx+1] = baseColor[1];
                        pixels[idx+2] = baseColor[2];
                        pixels[idx+3] = 255;
                    }
                }
            }
        }
    }

    if (startColor && ss) {
        for (i = 0; i < ss->numStarts; i++) {
            int sx = ss->item[i].x;
            int sy = ss->item[i].y;
            /* Colour by ownership when an override is in effect, else the
             * caller's default (yellow). The off-side bit rides on top of
             * the code and dims the dot rather than changing its colour. */
            uint8_t ownerByte = (s_startOwnerOverride && i < s_startOwnerOverrideCount)
                                    ? s_startOwnerOverride[i] : 0;
            uint8_t owner = ownerByte & MINIMAP_OWNER_CODE_MASK;
            bool    dim   = (ownerByte & MINIMAP_OWNER_OFFSIDE) != 0;
            const uint8_t *col = startColor;
            uint8_t ownerCol[3];
            if (minimapOwnerColor(owner, ownerCol)) {
                col = ownerCol;
            }
            /* Your own start: a gray border ring (5x5) under the green so it
             * stands out from allies (same green, no border). */
            if (owner == 1) {
                static const uint8_t ringCol[3] = { 105, 105, 105 };
                for (dy = -2; dy <= 2; dy++) {
                    for (dx = -2; dx <= 2; dx++) {
                        minimapPutStartPixel(pixels, sx + dx, sy + dy, ringCol, dim);
                    }
                }
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    minimapPutStartPixel(pixels, sx + dx, sy + dy, col, dim);
                }
            }
        }
    }
}

SDL_Texture *minimapCreateTexture(SDL_Renderer *renderer,
                                  const MapPreview *view,
                                  MinimapBounds *bounds,
                                  uint32_t flags) {
    uint8_t *pixels;
    SDL_Surface *surface;
    SDL_Texture *tex = NULL;

    pixels = (uint8_t *)malloc(MINIMAP_SIZE * MINIMAP_SIZE * 4);
    if (!pixels) return NULL;

    minimapRenderPixels(view, pixels, bounds, flags);

    surface = SDL_CreateSurfaceFrom(
        MINIMAP_SIZE, MINIMAP_SIZE, SDL_PIXELFORMAT_RGBA32,
        pixels, MINIMAP_SIZE * 4);
    if (surface) {
        tex = SDL_CreateTextureFromSurface(renderer, surface);
        if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        SDL_DestroySurface(surface);
    }

    free(pixels);
    return tex;
}

SDL_Texture *minimapFromCompressed(SDL_Renderer *renderer,
                                   const BYTE *compressedData, int dataLen,
                                   MinimapBounds *bounds,
                                   int *outPills, int *outBases, int *outStarts) {
    SDL_Texture *tex;

    MapPreview *mp = clientMapPreviewLoadFromBuffer(compressedData, dataLen);
    if (!mp) return NULL;

    if (outPills)  *outPills  = clientMapPreviewGetPillCount(mp);
    if (outBases)  *outBases  = clientMapPreviewGetBaseCount(mp);
    if (outStarts) *outStarts = clientMapPreviewGetStartCount(mp);

    tex = minimapCreateTexture(renderer, mp, bounds, 0);

    clientMapPreviewDestroy(mp);
    return tex;
}

SDL_Texture *minimapFromCompressedOwned(SDL_Renderer *renderer,
                                        const BYTE *compressedData, int dataLen,
                                        MinimapBounds *bounds,
                                        int *outPills, int *outBases, int *outStarts,
                                        const uint8_t *startOwners, int ownerCount) {
    SDL_Texture *tex;
    /* Install the per-start ownership colours for the duration of the build,
     * then clear so other callers (map editor, chooser) keep the default. */
    s_startOwnerOverride      = startOwners;
    s_startOwnerOverrideCount = startOwners ? ownerCount : 0;
    tex = minimapFromCompressed(renderer, compressedData, dataLen, bounds,
                                outPills, outBases, outStarts);
    s_startOwnerOverride      = NULL;
    s_startOwnerOverrideCount = 0;
    return tex;
}

SDL_Texture *minimapFromFile(SDL_Renderer *renderer, const char *mapPath,
                             MinimapBounds *bounds,
                             int *outPills, int *outBases, int *outStarts) {
    SDL_Texture *tex;

    /* Try direct fopen-based load first (desktop) */
    MapPreview *mp = clientMapPreviewLoadFromFile(mapPath);
    if (!mp) {
        /* Try SDL_LoadFile (Android APK) -> temp file -> mapRead */
        size_t fileSize = 0;
        void *fileData = SDL_LoadFile(mapPath, &fileSize);
        if (fileData && fileSize > 0) {
            char *tmpDir = SDL_GetPrefPath("WinBolo", "WinBolo");
            char tmpPath[512];
            SDL_snprintf(tmpPath, sizeof(tmpPath), "%s_preview_temp.map", tmpDir ? tmpDir : "");
            SDL_free(tmpDir);
            FILE *fp = fopen(tmpPath, "wb");
            if (fp) {
                fwrite(fileData, 1, fileSize, fp);
                fclose(fp);
                mp = clientMapPreviewLoadFromFile(tmpPath);
                remove(tmpPath);
            }
            SDL_free(fileData);
        }
    }

    if (!mp) return NULL;

    if (outPills)  *outPills  = clientMapPreviewGetPillCount(mp);
    if (outBases)  *outBases  = clientMapPreviewGetBaseCount(mp);
    if (outStarts) *outStarts = clientMapPreviewGetStartCount(mp);

    tex = minimapCreateTexture(renderer, mp, bounds, 0);

    clientMapPreviewDestroy(mp);
    return tex;
}
