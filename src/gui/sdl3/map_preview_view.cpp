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
 * Name:          map_preview_view.cpp
 * Purpose:       Implementation of the reusable map preview
 *                widget — see map_preview_view.h. Lifted
 *                wholesale from the original
 *                map_preview_popup.cpp (whose file-static
 *                state is now folded into a per-instance
 *                struct so two views can coexist — e.g.
 *                the lobby inline preview and the modal
 *                popup).
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "global.h"
#include "client_mappreview.h"
#include "screencalc.h"
#include "tilenum.h"
#include "../tiles.h"
#include "map_preview_view.h"
#include "macos_pinch.h"
#include "sprite_positions.h"
#include "minimap_render.h"

/* From tileloader.h */
extern SDL_Surface *tileLoaderBuildSheet(int tileSize);
}

/* Zoom steps. The four sub-0.5 entries put the widget into "minimap
 * mode" where each tile is painted as a single coloured rect instead
 * of a full sprite — at 0.0625× the whole 256-tile map fits in 256
 * pixels (1 pixel per tile). 0.5×+ uses the existing tile-sprite
 * renderer. */
static const float kZoomSteps[] = {
    0.0625f, 0.125f, 0.25f, 0.375f,
    0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
    9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f
};
#define ZOOM_STEP_COUNT 25
#define ZOOM_STEP_1X    9     /* index of 1.0f */
#define ZOOM_MINIMAP_MAX 0.33f /* < this: minimap-colour mode */

/* Boat sprite atlas coords for start position overlays. */
static const int kBoatAtlasX[16] = {
    TANK_SELFBOAT_0_X,  TANK_SELFBOAT_1_X,  TANK_SELFBOAT_2_X,  TANK_SELFBOAT_3_X,
    TANK_SELFBOAT_4_X,  TANK_SELFBOAT_5_X,  TANK_SELFBOAT_6_X,  TANK_SELFBOAT_7_X,
    TANK_SELFBOAT_8_X,  TANK_SELFBOAT_9_X,  TANK_SELFBOAT_10_X, TANK_SELFBOAT_11_X,
    TANK_SELFBOAT_12_X, TANK_SELFBOAT_13_X, TANK_SELFBOAT_14_X, TANK_SELFBOAT_15_X
};
static const int kBoatAtlasY[16] = {
    TANK_SELFBOAT_0_Y,  TANK_SELFBOAT_1_Y,  TANK_SELFBOAT_2_Y,  TANK_SELFBOAT_3_Y,
    TANK_SELFBOAT_4_Y,  TANK_SELFBOAT_5_Y,  TANK_SELFBOAT_6_Y,  TANK_SELFBOAT_7_Y,
    TANK_SELFBOAT_8_Y,  TANK_SELFBOAT_9_Y,  TANK_SELFBOAT_10_Y, TANK_SELFBOAT_11_Y,
    TANK_SELFBOAT_12_Y, TANK_SELFBOAT_13_Y, TANK_SELFBOAT_14_Y, TANK_SELFBOAT_15_Y
};

struct MapPreviewView {
    /* Source — either compressed buffer or a file path. The latter
     * wins on parse if both are set (callers pick one). */
    BYTE  *compressedData;
    int    compressedLen;
    char  *filePath;

    /* Parsed map data — built lazily inside RenderOffscreen so the
     * caller can Load* outside a render frame cheaply. */
    MapPreview *preview;
    bool        dataLoaded;

    /* Tile atlas — shared per-widget; rebuilt at the requested tile
     * size on first render. */
    SDL_Texture *tilesTex;

    /* Offscreen render target. Sized at RenderOffscreen time to match
     * the requested viewport (oversampled when zoom < 1 so the
     * destination Image can downscale for crisp output). */
    SDL_Texture *offscreen;
    int          offscreenW;
    int          offscreenH;

    /* Intermediate scratch target used in sub-1x sprite mode for one
     * manual 2:1 bilinear downsample pass — chained with the final
     * ImGui Image-time downsample, this gives ~16-sample averaging
     * per output pixel vs the 4-sample bilinear of a single pass. */
    SDL_Texture *scratch;
    int          scratchW;
    int          scratchH;
    /* Which texture should ImGui display (offscreen for non-sub-1x,
     * scratch for sub-1x sprite mode). */
    SDL_Texture *displayTex;

    /* Viewport size cache so we can detect resizes between calls. */
    int          lastViewW;
    int          lastViewH;

    /* View state. */
    int   zoomIndex;
    float zoomLevel;
    WORLD centerX;
    WORLD centerY;

    /* Auto-fit happens the first frame after data parses. */
    bool autoFitDone;

    /* macOS pinch accumulator. */
    float pinchAccum;
};

/* ── Adjacency-aware tile calculation (lifted verbatim) ──────────── */

static BYTE viewNeighbour(MapPreviewView *v, BYTE nx, BYTE ny) {
    if (clientMapPreviewIsBase(v->preview, nx, ny)) return ROAD;
    BYTE t = clientMapPreviewGetTerrain(v->preview, nx, ny);
    if (t >= MINE_START && t <= MINE_END) return (BYTE)(t - MINE_SUBTRACT);
    return t;
}

static BYTE viewCalcTile(MapPreviewView *v, BYTE xValue, BYTE yValue) {
    if (clientMapPreviewIsPill(v->preview, xValue, yValue)) {
        static const BYTE pillTileForArmour[16] = {
            PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
            PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
            PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
            PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
        };
        BYTE armour = clientMapPreviewGetPillArmourAt(v->preview, xValue, yValue);
        if (armour <= 15) return pillTileForArmour[armour];
        return PILL_EVIL_15;
    }
    if (clientMapPreviewIsBase(v->preview, xValue, yValue)) return BASE_NEUTRAL;
    if (clientMapPreviewIsStart(v->preview, xValue, yValue)) return DEEP_SEA_SOLID;

    BYTE currentPos = clientMapPreviewGetTerrain(v->preview, xValue, yValue);
    if (currentPos >= MINE_START && currentPos <= MINE_END)
        currentPos = (BYTE)(currentPos - MINE_SUBTRACT);

    BYTE aboveLeft  = viewNeighbour(v, (BYTE)(xValue-1), (BYTE)(yValue-1));
    BYTE above      = viewNeighbour(v, xValue,            (BYTE)(yValue-1));
    BYTE aboveRight = viewNeighbour(v, (BYTE)(xValue+1), (BYTE)(yValue-1));
    BYTE leftPos    = viewNeighbour(v, (BYTE)(xValue-1), yValue);
    BYTE rightPos   = viewNeighbour(v, (BYTE)(xValue+1), yValue);
    BYTE belowLeft  = viewNeighbour(v, (BYTE)(xValue-1), (BYTE)(yValue+1));
    BYTE below      = viewNeighbour(v, xValue,            (BYTE)(yValue+1));
    BYTE belowRight = viewNeighbour(v, (BYTE)(xValue+1), (BYTE)(yValue+1));

    switch (currentPos) {
    case ROAD:     return screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BUILDING: return screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case FOREST:   return screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case RIVER:    return screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case DEEP_SEA: return screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BOAT:     return screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case CRATER:   return screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    default:       return currentPos;
    }
}

/* ── Rendering ───────────────────────────────────────────────────── */

static void viewRenderStarts(MapPreviewView *v, SDL_Renderer *renderer,
                             int screenW, int screenH, float tileScale) {
    int tileSize = TILE_SIZE_X;
    float scaledTileF = (float)tileSize * tileScale;
    if (scaledTileF < 1.0f) scaledTileF = 1.0f;

    int centerPX = ((int)v->centerX * tileSize) >> 8;
    int centerPY = ((int)v->centerY * tileSize) >> 8;
    float halfX = (float)screenW / (2.0f * tileScale);
    float halfY = (float)screenH / (2.0f * tileScale);
    float camPXf = (float)centerPX - halfX;
    float camPYf = (float)centerPY - halfY;

    SDL_SetTextureAlphaMod(v->tilesTex, 200);

    BYTE numStarts = clientMapPreviewGetStartCount(v->preview);
    for (BYTE i = 1; i <= numStarts; i++) {
        BYTE sx, sy, sdir;
        if (!clientMapPreviewGetStart(v->preview, i, &sx, &sy, &sdir)) continue;
        float dx = ((float)((int)sx * tileSize) - camPXf) * tileScale;
        float dy = ((float)((int)sy * tileSize) - camPYf) * tileScale;
        if (dx + scaledTileF < 0 || dx > screenW ||
            dy + scaledTileF < 0 || dy > screenH) continue;
        int dir = sdir;
        SDL_FRect src = {
            (float)kBoatAtlasX[dir], (float)kBoatAtlasY[dir],
            (float)tileSize, (float)tileSize
        };
        SDL_FRect dest = { dx, dy, scaledTileF, scaledTileF };
        SDL_RenderTexture(renderer, v->tilesTex, &src, &dest);
    }

    SDL_SetTextureAlphaMod(v->tilesTex, 255);
}

/* Minimap-colour rendering — one coloured rect per map tile, sized to
 * whatever fits the current sub-0.5 zoom. Replaces tile-sprite drawing
 * for the far-zoomed-out view. Includes pill/base/start dots and the
 * border-zone darkening that minimapRenderPixels does. */
static void viewRenderMinimapToOffscreen(MapPreviewView *v,
                                         SDL_Renderer *renderer,
                                         int screenW, int screenH) {
    /* tilePx ≥ 1; at 0.0625× it's 1 (16 game px × 0.0625 = 1). */
    float tilePxF = (float)TILE_SIZE_X * v->zoomLevel;
    int tilePx = (int)tilePxF;
    if (tilePx < 1) tilePx = 1;

    int tileSizeWU = 256; /* WORLD units per tile (= 1 << TANK_SHIFT_MAPSIZE) */
    /* Camera centre in pixels at current zoom. */
    float centerPxF = ((float)v->centerX / (float)tileSizeWU) * tilePxF;
    float centerPyF = ((float)v->centerY / (float)tileSizeWU) * tilePxF;
    float camPxF = centerPxF - (float)screenW * 0.5f;
    float camPyF = centerPyF - (float)screenH * 0.5f;

    /* First / last visible tile (inclusive). Pad by one so partial
     * tiles at the edges still draw. */
    int firstMX = (int)floorf(camPxF / tilePxF) - 1;
    int firstMY = (int)floorf(camPyF / tilePxF) - 1;
    int lastMX  = (int)floorf((camPxF + (float)screenW) / tilePxF) + 1;
    int lastMY  = (int)floorf((camPyF + (float)screenH) / tilePxF) + 1;
    if (firstMX < 0) firstMX = 0;
    if (firstMY < 0) firstMY = 0;
    if (lastMX > 255) lastMX = 255;
    if (lastMY > 255) lastMY = 255;

    for (int my = firstMY; my <= lastMY; my++) {
        for (int mx = firstMX; mx <= lastMX; mx++) {
            BYTE raw = clientMapPreviewGetTerrain(v->preview,
                                                   (BYTE)mx, (BYTE)my);
            BYTE terrain = raw;
            bool isMined = false;
            if (raw >= MINE_START && raw <= MINE_END) {
                isMined = true;
                terrain = (BYTE)(raw - MINE_SUBTRACT);
            }
            uint8_t cr, cg, cb;
            minimapTerrainColor(terrain, &cr, &cg, &cb);
            if (isMined) {
                cr = (uint8_t)((float)cr * 0.8f);
                cg = (uint8_t)((float)cg * 0.8f);
                cb = (uint8_t)((float)cb * 0.8f);
            }
            /* Darken the mine-border ring the game auto-mines so the
             * border reads even at minimap zoom. */
            if (mx <= MAP_MINE_EDGE_LEFT || mx >= MAP_MINE_EDGE_RIGHT ||
                my <= MAP_MINE_EDGE_TOP  || my >= MAP_MINE_EDGE_BOTTOM) {
                cr = (uint8_t)((float)cr * 0.5f);
                cg = (uint8_t)((float)cg * 0.5f);
                cb = (uint8_t)((float)cb * 0.5f);
            }

            float dx = (float)mx * tilePxF - camPxF;
            float dy = (float)my * tilePxF - camPyF;
            SDL_FRect dest = { dx, dy, tilePxF + 0.5f, tilePxF + 0.5f };
            SDL_SetRenderDrawColor(renderer, cr, cg, cb, 255);
            SDL_RenderFillRect(renderer, &dest);
        }
    }

    /* Object dots — pill (red), base (white), start (yellow). Size
     * scales with tilePx so they stay legible. */
    int dotSize = tilePx;
    if (dotSize < 2) dotSize = 2;
    if (dotSize > 4 && tilePx <= 4) dotSize = 4;

    BYTE numPills = clientMapPreviewGetPillCount(v->preview);
    for (BYTE i = 1; i <= numPills; i++) {
        BYTE px, py;
        if (!clientMapPreviewGetPill(v->preview, i, &px, &py, NULL, NULL)) continue;
        float dx = (float)px * tilePxF - camPxF;
        float dy = (float)py * tilePxF - camPyF;
        SDL_FRect dot = { dx, dy, (float)dotSize, (float)dotSize };
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        SDL_RenderFillRect(renderer, &dot);
    }
    BYTE numBases = clientMapPreviewGetBaseCount(v->preview);
    for (BYTE i = 1; i <= numBases; i++) {
        BYTE bx, by;
        if (!clientMapPreviewGetBase(v->preview, i, &bx, &by, NULL)) continue;
        float dx = (float)bx * tilePxF - camPxF;
        float dy = (float)by * tilePxF - camPyF;
        SDL_FRect dot = { dx, dy, (float)dotSize, (float)dotSize };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &dot);
    }
    BYTE numStarts = clientMapPreviewGetStartCount(v->preview);
    for (BYTE i = 1; i <= numStarts; i++) {
        BYTE sx, sy;
        if (!clientMapPreviewGetStart(v->preview, i, &sx, &sy, NULL)) continue;
        float dx = (float)sx * tilePxF - camPxF;
        float dy = (float)sy * tilePxF - camPyF;
        SDL_FRect dot = { dx, dy, (float)dotSize, (float)dotSize };
        SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
        SDL_RenderFillRect(renderer, &dot);
    }
}

static void viewRenderTilesToOffscreen(MapPreviewView *v,
                                       SDL_Renderer *renderer,
                                       int screenW, int screenH,
                                       float tileScale) {
    /* `tileScale` is "offscreen pixels per game pixel" — i.e. how big
     * each game tile appears in this offscreen. Derived by the caller
     * from the *actual* post-clamp offscreen size so the apparent
     * zoom on display matches v->zoomLevel even when ofsW is capped
     * at 4096. Float-valued because clamps produce non-integer values
     * (e.g. ofsW=4096 at zoom 0.9 / viewW=1500 → tileScale ≈ 2.46). */
    int tileSize = TILE_SIZE_X;
    float scaledTileF = (float)tileSize * tileScale;
    if (scaledTileF < 1.0f) scaledTileF = 1.0f;

    int centerPX = ((int)v->centerX * tileSize) >> 8;
    int centerPY = ((int)v->centerY * tileSize) >> 8;
    /* Camera half-span in tile-pixel units (16 per tile, scale
     * independent). The offscreen carries scaledTileF pixels per
     * tile so half-span = screenW / (2 * scaledTileF) tiles, and
     * each tile = tileSize tile-pixels. */
    float halfX = (float)screenW / (2.0f * tileScale);
    float halfY = (float)screenH / (2.0f * tileScale);
    float camPXf = (float)centerPX - halfX;
    float camPYf = (float)centerPY - halfY;

    int camMX = (int)floorf(camPXf / (float)tileSize);
    int camMY = (int)floorf(camPYf / (float)tileSize);
    float edgeX = (camPXf - (float)(camMX * tileSize)) * tileScale;
    float edgeY = (camPYf - (float)(camMY * tileSize)) * tileScale;

    int tilesW = (int)((float)screenW / scaledTileF) + 3;
    int tilesH = (int)((float)screenH / scaledTileF) + 3;
    if (tilesW > 256) tilesW = 256;
    if (tilesH > 256) tilesH = 256;

    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;
            BYTE tileNum;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) {
                tileNum = DEEP_SEA_SOLID;
            } else {
                tileNum = viewCalcTile(v, (BYTE)mapX, (BYTE)mapY);
            }
            SDL_FRect src = {
                (float)(mapViewPosX[tileNum]),
                (float)(mapViewPosY[tileNum]),
                (float)tileSize, (float)tileSize
            };
            SDL_FRect dest = {
                (float)x * scaledTileF - edgeX,
                (float)y * scaledTileF - edgeY,
                scaledTileF, scaledTileF
            };
            SDL_RenderTexture(renderer, v->tilesTex, &src, &dest);
        }
    }

    /* Mine overlay — translucent mine sprite on top of mined tiles
     * AND the border zone the game treats as auto-mined. */
    SDL_SetTextureAlphaMod(v->tilesTex, 180);
    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) continue;
            bool mined = false;
            BYTE raw = clientMapPreviewGetTerrain(v->preview, (BYTE)mapX, (BYTE)mapY);
            if (raw >= MINE_START && raw <= MINE_END) {
                mined = true;
            } else if (mapX <= MAP_MINE_EDGE_LEFT || mapX >= MAP_MINE_EDGE_RIGHT ||
                       mapY <= MAP_MINE_EDGE_TOP  || mapY >= MAP_MINE_EDGE_BOTTOM) {
                mined = true;
            }
            if (mined) {
                SDL_FRect mineSrc = {
                    (float)MINE_X, (float)MINE_Y,
                    (float)tileSize, (float)tileSize
                };
                SDL_FRect dest = {
                    (float)x * scaledTileF - edgeX,
                    (float)y * scaledTileF - edgeY,
                    scaledTileF, scaledTileF
                };
                SDL_RenderTexture(renderer, v->tilesTex, &mineSrc, &dest);
            }
        }
    }
    SDL_SetTextureAlphaMod(v->tilesTex, 255);

    viewRenderStarts(v, renderer, screenW, screenH, tileScale);
}

/* ── Map data lifecycle ──────────────────────────────────────────── */

static void viewFreeMapData(MapPreviewView *v) {
    if (v->offscreen) { SDL_DestroyTexture(v->offscreen); v->offscreen = NULL; }
    v->offscreenW = 0;
    v->offscreenH = 0;
    if (v->scratch) { SDL_DestroyTexture(v->scratch); v->scratch = NULL; }
    v->scratchW = 0;
    v->scratchH = 0;
    v->displayTex = NULL;
    if (v->dataLoaded && v->preview) {
        clientMapPreviewDestroy(v->preview);
        v->preview = NULL;
        v->dataLoaded = false;
    }
    v->autoFitDone = false;
}

static void viewAutoFitZoom(MapPreviewView *v, int viewW, int viewH) {
    if (!v->dataLoaded || !v->preview) return;
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) {
            BYTE t = clientMapPreviewGetTerrain(v->preview, (BYTE)x, (BYTE)y);
            if (t != DEEP_SEA) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    {
        BYTE n = clientMapPreviewGetPillCount(v->preview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE px, py;
            if (!clientMapPreviewGetPill(v->preview, i, &px, &py, NULL, NULL)) continue;
            if (px < minX) minX = px; if (px > maxX) maxX = px;
            if (py < minY) minY = py; if (py > maxY) maxY = py;
        }
    }
    {
        BYTE n = clientMapPreviewGetBaseCount(v->preview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE bx, by;
            if (!clientMapPreviewGetBase(v->preview, i, &bx, &by, NULL)) continue;
            if (bx < minX) minX = bx; if (bx > maxX) maxX = bx;
            if (by < minY) minY = by; if (by > maxY) maxY = by;
        }
    }
    {
        BYTE n = clientMapPreviewGetStartCount(v->preview);
        for (BYTE i = 1; i <= n; i++) {
            BYTE sx, sy;
            if (!clientMapPreviewGetStart(v->preview, i, &sx, &sy, NULL)) continue;
            if (sx < minX) minX = sx; if (sx > maxX) maxX = sx;
            if (sy < minY) minY = sy; if (sy > maxY) maxY = sy;
        }
    }

    if (maxX < minX || maxY < minY) {
        v->zoomIndex = ZOOM_STEP_1X;
        v->zoomLevel = 1.0f;
        return;
    }

    minX -= 2; minY -= 2; maxX += 2; maxY += 2;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > 255) maxX = 255;
    if (maxY > 255) maxY = 255;

    v->centerX = ((minX + maxX) / 2) << 8;
    v->centerY = ((minY + maxY) / 2) << 8;

    int contentW = (maxX - minX + 1) * TILE_SIZE_X;
    int contentH = (maxY - minY + 1) * TILE_SIZE_Y;

    float zoomW = (float)viewW / (float)contentW;
    float zoomH = (float)viewH / (float)contentH;
    float idealZoom = (zoomW < zoomH) ? zoomW : zoomH;
    /* Lower bound is the first zoom step (0.0625 = 1 px per tile). */
    if (idealZoom < kZoomSteps[0]) idealZoom = kZoomSteps[0];

    v->zoomIndex = 0;
    for (int i = 0; i < ZOOM_STEP_COUNT; i++) {
        if (kZoomSteps[i] <= idealZoom) v->zoomIndex = i;
        else                            break;
    }
    v->zoomLevel = kZoomSteps[v->zoomIndex];
}

static void viewEnsureParsed(MapPreviewView *v, SDL_Renderer *renderer) {
    if (v->dataLoaded) return;
    if (!v->compressedData && !v->filePath) return;

    if (!v->tilesTex) {
        SDL_Surface *sheet = tileLoaderBuildSheet(16);
        if (sheet) {
            v->tilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
            SDL_SetTextureScaleMode(v->tilesTex, SDL_SCALEMODE_NEAREST);
            SDL_DestroySurface(sheet);
        }
    }

    if (v->compressedData) {
        v->preview = clientMapPreviewLoadFromBuffer(v->compressedData,
                                                    v->compressedLen);
    } else if (v->filePath) {
        v->preview = clientMapPreviewLoadFromFile(v->filePath);
        if (!v->preview) {
            /* iOS/macOS bundle fallback — copy to a writable temp
             * file the parser can open. */
            size_t fileSize = 0;
            void *fileData = SDL_LoadFile(v->filePath, &fileSize);
            if (fileData && fileSize > 0) {
                char *tmpDir = SDL_GetPrefPath("WinBolo", "WinBolo");
                char tmpPath[512];
                SDL_snprintf(tmpPath, sizeof(tmpPath),
                             "%s_view_temp.map", tmpDir ? tmpDir : "");
                SDL_free(tmpDir);
                FILE *fp = fopen(tmpPath, "wb");
                if (fp) {
                    fwrite(fileData, 1, fileSize, fp);
                    fclose(fp);
                    v->preview = clientMapPreviewLoadFromFile(tmpPath);
                    remove(tmpPath);
                }
            }
            SDL_free(fileData);
        }
    }
    v->dataLoaded = (v->preview != NULL);
}

/* ── Public API ──────────────────────────────────────────────────── */

extern "C" MapPreviewView *mapPreviewViewCreate(void) {
    MapPreviewView *v = (MapPreviewView *)SDL_calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->zoomIndex = ZOOM_STEP_1X;
    v->zoomLevel = 1.0f;
    v->centerX   = 128 << 8;
    v->centerY   = 128 << 8;
    return v;
}

extern "C" void mapPreviewViewDestroy(MapPreviewView *v) {
    if (!v) return;
    viewFreeMapData(v);
    if (v->compressedData) { SDL_free(v->compressedData); v->compressedData = NULL; }
    if (v->filePath)       { SDL_free(v->filePath);       v->filePath       = NULL; }
    if (v->tilesTex)       { SDL_DestroyTexture(v->tilesTex); v->tilesTex   = NULL; }
    if (v->scratch)        { SDL_DestroyTexture(v->scratch); v->scratch     = NULL; }
    SDL_free(v);
}

extern "C" bool mapPreviewViewLoadCompressed(MapPreviewView *v,
                                              const BYTE *data, int len) {
    if (!v || !data || len <= 0) return false;
    if (v->compressedData) { SDL_free(v->compressedData); v->compressedData = NULL; v->compressedLen = 0; }
    if (v->filePath)       { SDL_free(v->filePath);       v->filePath = NULL; }
    v->compressedData = (BYTE *)SDL_malloc(len);
    if (!v->compressedData) return false;
    SDL_memcpy(v->compressedData, data, len);
    v->compressedLen = len;
    viewFreeMapData(v);
    return true;
}

extern "C" bool mapPreviewViewLoadFile(MapPreviewView *v, const char *path) {
    if (!v || !path || path[0] == '\0') return false;
    if (v->compressedData) { SDL_free(v->compressedData); v->compressedData = NULL; v->compressedLen = 0; }
    if (v->filePath)       { SDL_free(v->filePath);       v->filePath = NULL; }
    v->filePath = SDL_strdup(path);
    if (!v->filePath) return false;
    viewFreeMapData(v);
    return true;
}

extern "C" void mapPreviewViewSetInitialBounds(MapPreviewView *v,
                                                int minX, int minY,
                                                int maxX, int maxY) {
    if (!v) return;
    v->zoomIndex   = ZOOM_STEP_1X + 1;
    v->zoomLevel   = 2.0f;
    v->centerX     = ((minX + maxX) / 2) << 8;
    v->centerY     = ((minY + maxY) / 2) << 8;
    v->autoFitDone = false;
}

extern "C" void mapPreviewViewRenderOffscreen(MapPreviewView *v,
                                               SDL_Renderer *renderer,
                                               int viewW, int viewH) {
    if (!v || !renderer || viewW < 1 || viewH < 1) return;

    viewEnsureParsed(v, renderer);
    if (!v->dataLoaded || !v->tilesTex) return;

    /* Auto-fit zoom on first frame after data is loaded. */
    if (!v->autoFitDone) {
        viewAutoFitZoom(v, viewW, viewH);
        v->autoFitDone = true;
    }

    /* Detect viewport resize. */
    if (viewW != v->lastViewW || viewH != v->lastViewH) {
        v->lastViewW = viewW;
        v->lastViewH = viewH;
        if (v->offscreen) {
            SDL_DestroyTexture(v->offscreen);
            v->offscreen  = NULL;
            v->offscreenW = 0;
            v->offscreenH = 0;
        }
    }

    /* Sub-1x zoom in sprite mode: 4x oversample.  Combined with a
     * manual 2:1 bilinear downsample pass below, the bilinear chain
     * ends up averaging ~16 source pixels per displayed pixel at
     * zoom 0.5 (4 from each of the two bilinear passes) instead of
     * 4 from a single pass.  The tile renderer draws at zf=4 to
     * match — see viewRenderTilesToOffscreen.  Capped at 4096² for
     * VRAM safety; below the cap quality is maximal.
     *
     * IMPORTANT: clamp PROPORTIONALLY — scaling each axis down by
     * its own ratio if it exceeded 4096 would drift the offscreen's
     * aspect ratio away from the display's, and ImGui::Image's
     * stretch-to-fit would show as "zoom in X but not Y". Scale
     * both axes by the same factor so the texture aspect matches
     * the display rect at any zoom level. */
    int ofsW = viewW, ofsH = viewH;
    if (v->zoomLevel < 1.0f && v->zoomLevel >= ZOOM_MINIMAP_MAX) {
        float scale = 4.0f / v->zoomLevel;
        float fW = (float)viewW * scale;
        float fH = (float)viewH * scale;
        float largest = (fW > fH) ? fW : fH;
        if (largest > 4096.0f) {
            float cap = 4096.0f / largest;
            fW *= cap;
            fH *= cap;
        }
        ofsW = (int)fW;
        ofsH = (int)fH;
        if (ofsW < 1) ofsW = 1;
        if (ofsH < 1) ofsH = 1;
    }

    if (!v->offscreen || v->offscreenW != ofsW || v->offscreenH != ofsH) {
        if (v->offscreen) SDL_DestroyTexture(v->offscreen);
        v->offscreen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET, ofsW, ofsH);
        v->offscreenW = ofsW;
        v->offscreenH = ofsH;
        if (v->offscreen) {
            SDL_SetTextureScaleMode(v->offscreen, SDL_SCALEMODE_LINEAR);
        }
    }

    if (v->offscreen) {
        /* Tile atlas filter mode — LINEAR when sub-1x sprite mode so
         * the 16->64-px tile upscale anti-aliases tile edges; NEAREST
         * otherwise so pixel art stays crisp at >=1x. */
        bool subOne = (v->zoomLevel >= ZOOM_MINIMAP_MAX &&
                       v->zoomLevel < 1.0f);
        if (v->tilesTex) {
            SDL_SetTextureScaleMode(v->tilesTex,
                subOne ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        }

        SDL_SetRenderTarget(renderer, v->offscreen);
        SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
        SDL_RenderClear(renderer);
        /* Below 0.33× game scale, switch to minimap-colour mode —
         * tile sprites at < ~5 px per tile look like noise, so swap
         * in the per-tile colour rep used by the dedicated 256×256
         * minimap. Above that, fall through to the sprite renderer. */
        if (v->zoomLevel < ZOOM_MINIMAP_MAX) {
            viewRenderMinimapToOffscreen(v, renderer, ofsW, ofsH);
        } else {
            /* tileScale = "tile pixels in offscreen per game pixel".
             * Derived from the *actual* (post-clamp) ofsW so the
             * displayed zoom matches v->zoomLevel even when the
             * offscreen had to be capped at 4096. For zoom 1.0 this
             * works out to 1.0 (offscreen at viewW, no oversample);
             * for sub-1x sprite mode (4x oversample target) it's
             * close to 4 when uncapped, and lower when clamped. */
            float tileScale =
                (float)ofsW * v->zoomLevel / (float)viewW;
            if (tileScale < 0.5f) tileScale = 0.5f;
            viewRenderTilesToOffscreen(v, renderer, ofsW, ofsH, tileScale);
        }
        SDL_SetRenderTarget(renderer, NULL);

        /* Sub-1x: do one manual 2:1 bilinear downsample pass into
         * `scratch`. ImGui then bilinearly scales scratch down to
         * the display size — net effect ~16 source pixels averaged
         * per output pixel for sub-1x zoom levels. For other modes
         * we display offscreen directly. */
        if (subOne) {
            int s2W = ofsW / 2;
            int s2H = ofsH / 2;
            if (s2W < 1) s2W = 1;
            if (s2H < 1) s2H = 1;
            if (!v->scratch || v->scratchW != s2W || v->scratchH != s2H) {
                if (v->scratch) SDL_DestroyTexture(v->scratch);
                v->scratch = SDL_CreateTexture(renderer,
                    SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                    s2W, s2H);
                v->scratchW = s2W;
                v->scratchH = s2H;
                if (v->scratch) {
                    SDL_SetTextureScaleMode(v->scratch, SDL_SCALEMODE_LINEAR);
                }
            }
            if (v->scratch) {
                SDL_SetRenderTarget(renderer, v->scratch);
                SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, v->offscreen, NULL, NULL);
                SDL_SetRenderTarget(renderer, NULL);
                v->displayTex = v->scratch;
            } else {
                v->displayTex = v->offscreen;
            }
        } else {
            v->displayTex = v->offscreen;
        }
    }
}

extern "C" SDL_Texture *mapPreviewViewGetTexture(MapPreviewView *v) {
    if (!v) return NULL;
    /* displayTex is the active "after all downsample passes" texture
     * — scratch in sub-1x sprite mode, offscreen everywhere else. */
    return v->displayTex ? v->displayTex : v->offscreen;
}

extern "C" void mapPreviewViewGetTextureSize(const MapPreviewView *v,
                                              int *outW, int *outH) {
    if (outW) *outW = v ? v->offscreenW : 0;
    if (outH) *outH = v ? v->offscreenH : 0;
}

extern "C" bool mapPreviewViewIsReady(const MapPreviewView *v) {
    return v && v->dataLoaded;
}

extern "C" float mapPreviewViewGetZoom(const MapPreviewView *v) {
    return v ? v->zoomLevel : 1.0f;
}

extern "C" float mapPreviewViewZoomIn(MapPreviewView *v) {
    if (!v) return 1.0f;
    if (v->zoomIndex < ZOOM_STEP_COUNT - 1) v->zoomIndex++;
    v->zoomLevel = kZoomSteps[v->zoomIndex];
    return v->zoomLevel;
}

extern "C" float mapPreviewViewZoomOut(MapPreviewView *v) {
    if (!v) return 1.0f;
    if (v->zoomIndex > 0) v->zoomIndex--;
    v->zoomLevel = kZoomSteps[v->zoomIndex];
    return v->zoomLevel;
}

extern "C" void mapPreviewViewHandleInput(MapPreviewView *v, bool hovered,
                                           const MapPreviewInputOpts *opts) {
    if (!v) return;
    MapPreviewInputOpts defaultOpts = { true, true, true, false };
    if (!opts) opts = &defaultOpts;
    ImGuiIO &io = ImGui::GetIO();

    if (hovered) {
        if (opts->dragPan && ImGui::IsMouseDragging(0)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            float zf = v->zoomLevel;
            float tileSize = 16.0f;
            float dx = io.MouseDelta.x / (zf * tileSize) * 256.0f;
            float dy = io.MouseDelta.y / (zf * tileSize) * 256.0f;
            int newCX = (int)v->centerX - (int)dx;
            int newCY = (int)v->centerY - (int)dy;
            if (newCX < 0)     newCX = 0;
            if (newCX > 65280) newCX = 65280;
            if (newCY < 0)     newCY = 0;
            if (newCY > 65280) newCY = 65280;
            v->centerX = (WORLD)newCX;
            v->centerY = (WORLD)newCY;
        } else if (opts->dragPan) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (opts->wheelZoom && io.MouseWheel != 0) {
            if (io.MouseWheel > 0 && v->zoomIndex < ZOOM_STEP_COUNT - 1)
                v->zoomIndex++;
            else if (io.MouseWheel < 0 && v->zoomIndex > 0)
                v->zoomIndex--;
            v->zoomLevel = kZoomSteps[v->zoomIndex];
        }
    }

    if (opts->pinchZoom) {
        float pinch = macOSPinchZoomConsume();
        if (pinch != 0.0f) {
            v->pinchAccum += pinch;
            while (v->pinchAccum > 0.15f) {
                if (v->zoomIndex < ZOOM_STEP_COUNT - 1) v->zoomIndex++;
                v->pinchAccum -= 0.15f;
            }
            while (v->pinchAccum < -0.15f) {
                if (v->zoomIndex > 0) v->zoomIndex--;
                v->pinchAccum += 0.15f;
            }
            v->zoomLevel = kZoomSteps[v->zoomIndex];
        }
    }

    if (opts->arrowPan) {
        float panSpeed = 512.0f / v->zoomLevel;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
            int newCX = (int)v->centerX - (int)panSpeed;
            v->centerX = (WORLD)(newCX < 0 ? 0 : newCX);
        }
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
            int newCX = (int)v->centerX + (int)panSpeed;
            v->centerX = (WORLD)(newCX > 65280 ? 65280 : newCX);
        }
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
            int newCY = (int)v->centerY - (int)panSpeed;
            v->centerY = (WORLD)(newCY < 0 ? 0 : newCY);
        }
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
            int newCY = (int)v->centerY + (int)panSpeed;
            v->centerY = (WORLD)(newCY > 65280 ? 65280 : newCY);
        }
    }
}
