/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_export.c
 * Purpose:
 *   Export the current map as a PNG image — either
 *   full-resolution tile render or compact minimap preview.
 *********************************************************/

#include "mapeditor_export.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "screencalc.h"
#include "tilenum.h"
#include "../gui/tiles.h"
#include "../gui/sdl3/minimap_render.h"

#include <stb_image_write.h>
#include <stdlib.h>
#include <string.h>

#include "../gui/sdl3/sprite_positions.h"

/* From starts.h */
extern BYTE startsConvertDir(BYTE dir);
extern SDL_Surface *tileLoaderBuildSheet(int tileSize);

/* Boat sprite atlas coordinates (same table as in mapeditor.c) */
static const int exportBoatAtlasX[16] = {
    TANK_SELFBOAT_0_X,  TANK_SELFBOAT_1_X,  TANK_SELFBOAT_2_X,  TANK_SELFBOAT_3_X,
    TANK_SELFBOAT_4_X,  TANK_SELFBOAT_5_X,  TANK_SELFBOAT_6_X,  TANK_SELFBOAT_7_X,
    TANK_SELFBOAT_8_X,  TANK_SELFBOAT_9_X,  TANK_SELFBOAT_10_X, TANK_SELFBOAT_11_X,
    TANK_SELFBOAT_12_X, TANK_SELFBOAT_13_X, TANK_SELFBOAT_14_X, TANK_SELFBOAT_15_X
};
static const int exportBoatAtlasY[16] = {
    TANK_SELFBOAT_0_Y,  TANK_SELFBOAT_1_Y,  TANK_SELFBOAT_2_Y,  TANK_SELFBOAT_3_Y,
    TANK_SELFBOAT_4_Y,  TANK_SELFBOAT_5_Y,  TANK_SELFBOAT_6_Y,  TANK_SELFBOAT_7_Y,
    TANK_SELFBOAT_8_Y,  TANK_SELFBOAT_9_Y,  TANK_SELFBOAT_10_Y, TANK_SELFBOAT_11_Y,
    TANK_SELFBOAT_12_Y, TANK_SELFBOAT_13_Y, TANK_SELFBOAT_14_Y, TANK_SELFBOAT_15_Y
};

/* Read a neighbour tile for adjacency calculation (same as meNeighbour in mapeditor.c) */
static BYTE exportNeighbour(map mp, bases bs, BYTE nx, BYTE ny) {
    if (basesExistPos(&bs, nx, ny) == TRUE) return ROAD;
    BYTE t = mp->mapItem[nx][ny];
    if (t >= MINE_START && t <= MINE_END) return (BYTE)(t - MINE_SUBTRACT);
    return t;
}

/* Adjacency-aware tile calculation (same as meCalcTile in mapeditor.c) */
static BYTE exportCalcTile(map mp, bases bs, pillboxes pb, starts ss,
                           BYTE xValue, BYTE yValue) {
    if (pillsExistPos(&pb, xValue, yValue) == TRUE) {
        static const BYTE pillTileForArmour[16] = {
            PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
            PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
            PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
            PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
        };
        BYTE armour = pillsGetArmourPos(&pb, xValue, yValue);
        return (armour <= 15) ? pillTileForArmour[armour] : PILL_EVIL_15;
    }
    if (basesExistPos(&bs, xValue, yValue) == TRUE) return BASE_NEUTRAL;
    if (startsExistPos(&ss, xValue, yValue) == TRUE) return DEEP_SEA_SOLID;

    BYTE currentPos = mp->mapItem[xValue][yValue];
    if (currentPos >= MINE_START && currentPos <= MINE_END)
        currentPos = (BYTE)(currentPos - MINE_SUBTRACT);

    BYTE aboveLeft  = exportNeighbour(mp, bs, (BYTE)(xValue-1), (BYTE)(yValue-1));
    BYTE above      = exportNeighbour(mp, bs, xValue,            (BYTE)(yValue-1));
    BYTE aboveRight = exportNeighbour(mp, bs, (BYTE)(xValue+1), (BYTE)(yValue-1));
    BYTE leftPos    = exportNeighbour(mp, bs, (BYTE)(xValue-1), yValue);
    BYTE rightPos   = exportNeighbour(mp, bs, (BYTE)(xValue+1), yValue);
    BYTE belowLeft  = exportNeighbour(mp, bs, (BYTE)(xValue-1), (BYTE)(yValue+1));
    BYTE below      = exportNeighbour(mp, bs, xValue,            (BYTE)(yValue+1));
    BYTE belowRight = exportNeighbour(mp, bs, (BYTE)(xValue+1), (BYTE)(yValue+1));

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

/* -----------------------------------------------------------------------
 * Full-resolution export (4096x4096, 16px per tile)
 * ----------------------------------------------------------------------- */
static bool exportFull(const char *filePath, ExportConfig *cfg,
                       SDL_Renderer *renderer, SDL_Texture *tilesTex, int tileSize,
                       map mp, bases bs, pillboxes pb, starts ss) {
    const int mapSize = 256;
    const int imgSize = mapSize * tileSize; /* 4096 */

    /* Try to create a full 4096x4096 render target */
    SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_TARGET,
                                            imgSize, imgSize);
    if (!target) {
        /* GPU memory may be insufficient — fall back to strip-based */
        /* Strip approach: 16 strips of 4096x256 each */
        const int stripH = tileSize * 16; /* 256 pixels = 16 tile rows */
        const int numStrips = mapSize / 16;
        uint8_t *fullPixels = (uint8_t *)malloc((size_t)imgSize * imgSize * 4);
        if (!fullPixels) return false;

        target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_TARGET,
                                   imgSize, stripH);
        if (!target) { free(fullPixels); return false; }

        for (int strip = 0; strip < numStrips; strip++) {
            int startRow = strip * 16;
            SDL_SetRenderTarget(renderer, target);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

            for (int x = 0; x < mapSize; x++) {
                for (int y = 0; y < 16; y++) {
                    int mapY = startRow + y;
                    BYTE tileNum = exportCalcTile(mp, bs, pb, ss, (BYTE)x, (BYTE)mapY);
                    SDL_FRect src = { (float)mapViewPosX[tileNum], (float)mapViewPosY[tileNum],
                                      (float)tileSize, (float)tileSize };
                    SDL_FRect dest = { (float)(x * tileSize), (float)(y * tileSize),
                                       (float)tileSize, (float)tileSize };
                    SDL_RenderTexture(renderer, tilesTex, &src, &dest);

                    /* Mine overlay */
                    if (cfg->showMines) {
                        BYTE raw = mp->mapItem[x][mapY];
                        bool hasMine = (raw >= MINE_START && raw <= MINE_END);
                        if (!hasMine &&
                            (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                             mapY <= MAP_MINE_EDGE_TOP || mapY >= MAP_MINE_EDGE_BOTTOM)) {
                            hasMine = true;
                        }
                        if (hasMine) {
                            SDL_FRect mineSrc = { (float)MINE_X, (float)MINE_Y,
                                                  (float)tileSize, (float)tileSize };
                            SDL_RenderTexture(renderer, tilesTex, &mineSrc, &dest);
                        }
                    }
                }
            }

            /* Object overlays for this strip */
            if (cfg->showObjects) {
                for (int i = 0; i < bs->numBases; i++) {
                    int by = bs->item[i].y;
                    if (by >= startRow && by < startRow + 16) {
                        BYTE tileNum = BASE_NEUTRAL;
                        SDL_FRect src = { (float)mapViewPosX[tileNum], (float)mapViewPosY[tileNum],
                                          (float)tileSize, (float)tileSize };
                        SDL_FRect dest = { (float)(bs->item[i].x * tileSize),
                                           (float)((by - startRow) * tileSize),
                                           (float)tileSize, (float)tileSize };
                        SDL_RenderTexture(renderer, tilesTex, &src, &dest);
                    }
                }
                for (int i = 0; i < pb->numPills; i++) {
                    int py = pb->item[i].y;
                    if (py >= startRow && py < startRow + 16) {
                        static const BYTE pillTileForArmour[16] = {
                            PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
                            PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
                            PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
                            PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
                        };
                        BYTE a = pb->item[i].armour;
                        BYTE tileNum = (a <= 15) ? pillTileForArmour[a] : PILL_EVIL_15;
                        SDL_FRect src = { (float)mapViewPosX[tileNum], (float)mapViewPosY[tileNum],
                                          (float)tileSize, (float)tileSize };
                        SDL_FRect dest = { (float)(pb->item[i].x * tileSize),
                                           (float)((py - startRow) * tileSize),
                                           (float)tileSize, (float)tileSize };
                        SDL_RenderTexture(renderer, tilesTex, &src, &dest);
                    }
                }
                for (int i = 0; i < ss->numStarts; i++) {
                    int sy = ss->item[i].y;
                    if (sy >= startRow && sy < startRow + 16) {
                        int dir = startsConvertDir((ss->item[i].dir < 16) ? ss->item[i].dir : 0);
                        SDL_FRect src = { (float)exportBoatAtlasX[dir], (float)exportBoatAtlasY[dir],
                                          (float)tileSize, (float)tileSize };
                        SDL_FRect dest = { (float)(ss->item[i].x * tileSize),
                                           (float)((sy - startRow) * tileSize),
                                           (float)tileSize, (float)tileSize };
                        SDL_RenderTexture(renderer, tilesTex, &src, &dest);
                    }
                }
            }

            /* Grid lines for this strip */
            if (cfg->showGrid) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
                for (int x = 0; x <= mapSize; x++) {
                    float px = (float)(x * tileSize);
                    SDL_RenderLine(renderer, px, 0, px, (float)stripH);
                }
                for (int y = 0; y <= 16; y++) {
                    float py = (float)(y * tileSize);
                    SDL_RenderLine(renderer, 0, py, (float)imgSize, py);
                }
            }

            /* Read pixels for this strip */
            SDL_Surface *surf = SDL_RenderReadPixels(renderer, NULL);
            if (surf) {
                int rowBytes = imgSize * 4;
                int copyBytes = rowBytes < surf->pitch ? rowBytes : surf->pitch;
                for (int row = 0; row < stripH; row++) {
                    memcpy(fullPixels + ((size_t)(startRow * tileSize + row) * rowBytes),
                           (uint8_t *)surf->pixels + (size_t)row * surf->pitch,
                           (size_t)copyBytes);
                }
                SDL_DestroySurface(surf);
            }
        }

        SDL_SetRenderTarget(renderer, NULL);
        SDL_DestroyTexture(target);

        bool ok = stbi_write_png(filePath, imgSize, imgSize, 4, fullPixels, imgSize * 4) != 0;
        free(fullPixels);
        return ok;
    }

    /* Full render target succeeded — render everything in one pass */
    SDL_SetRenderTarget(renderer, target);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */

    /* Terrain tiles */
    for (int x = 0; x < mapSize; x++) {
        for (int y = 0; y < mapSize; y++) {
            BYTE tileNum = exportCalcTile(mp, bs, pb, ss, (BYTE)x, (BYTE)y);
            SDL_FRect src = { (float)mapViewPosX[tileNum], (float)mapViewPosY[tileNum],
                              (float)tileSize, (float)tileSize };
            SDL_FRect dest = { (float)(x * tileSize), (float)(y * tileSize),
                               (float)tileSize, (float)tileSize };
            SDL_RenderTexture(renderer, tilesTex, &src, &dest);

            /* Mine overlay */
            if (cfg->showMines) {
                BYTE raw = mp->mapItem[x][y];
                bool hasMine = (raw >= MINE_START && raw <= MINE_END);
                if (!hasMine &&
                    (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                     y <= MAP_MINE_EDGE_TOP || y >= MAP_MINE_EDGE_BOTTOM)) {
                    hasMine = true;
                }
                if (hasMine) {
                    SDL_FRect mineSrc = { (float)MINE_X, (float)MINE_Y,
                                          (float)tileSize, (float)tileSize };
                    SDL_RenderTexture(renderer, tilesTex, &mineSrc, &dest);
                }
            }
        }
    }

    /* Object overlays */
    if (cfg->showObjects) {
        for (int i = 0; i < bs->numBases; i++) {
            BYTE tileNum = BASE_NEUTRAL;
            SDL_FRect src = { (float)mapViewPosX[tileNum], (float)mapViewPosY[tileNum],
                              (float)tileSize, (float)tileSize };
            SDL_FRect dest = { (float)(bs->item[i].x * tileSize),
                               (float)(bs->item[i].y * tileSize),
                               (float)tileSize, (float)tileSize };
            SDL_RenderTexture(renderer, tilesTex, &src, &dest);
        }
        for (int i = 0; i < pb->numPills; i++) {
            static const BYTE pillTileForArmour2[16] = {
                PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
                PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
                PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
                PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
            };
            BYTE a2 = pb->item[i].armour;
            BYTE tileNum = (a2 <= 15) ? pillTileForArmour2[a2] : PILL_EVIL_15;
            SDL_FRect src = { (float)mapViewPosX[tileNum], (float)mapViewPosY[tileNum],
                              (float)tileSize, (float)tileSize };
            SDL_FRect dest = { (float)(pb->item[i].x * tileSize),
                               (float)(pb->item[i].y * tileSize),
                               (float)tileSize, (float)tileSize };
            SDL_RenderTexture(renderer, tilesTex, &src, &dest);
        }
        for (int i = 0; i < ss->numStarts; i++) {
            int dir = startsConvertDir((ss->item[i].dir < 16) ? ss->item[i].dir : 0);
            SDL_FRect src = { (float)exportBoatAtlasX[dir], (float)exportBoatAtlasY[dir],
                              (float)tileSize, (float)tileSize };
            SDL_FRect dest = { (float)(ss->item[i].x * tileSize),
                               (float)(ss->item[i].y * tileSize),
                               (float)tileSize, (float)tileSize };
            SDL_RenderTexture(renderer, tilesTex, &src, &dest);
        }
    }

    /* Grid lines */
    if (cfg->showGrid) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
        for (int x = 0; x <= mapSize; x++) {
            float px = (float)(x * tileSize);
            SDL_RenderLine(renderer, px, 0, px, (float)imgSize);
        }
        for (int y = 0; y <= mapSize; y++) {
            float py = (float)(y * tileSize);
            SDL_RenderLine(renderer, 0, py, (float)imgSize, py);
        }
    }

    /* Read pixels back */
    SDL_Surface *surf = SDL_RenderReadPixels(renderer, NULL);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_DestroyTexture(target);

    if (!surf) return false;

    /* Copy surface data to a contiguous buffer for stbi */
    uint8_t *pixels = (uint8_t *)malloc((size_t)imgSize * imgSize * 4);
    if (!pixels) { SDL_DestroySurface(surf); return false; }

    {
        int rowBytes = imgSize * 4;
        int copyBytes = rowBytes < surf->pitch ? rowBytes : surf->pitch;
        for (int row = 0; row < imgSize; row++) {
            memcpy(pixels + (size_t)row * rowBytes,
                   (uint8_t *)surf->pixels + (size_t)row * surf->pitch,
                   (size_t)copyBytes);
        }
    }
    SDL_DestroySurface(surf);

    bool ok = stbi_write_png(filePath, imgSize, imgSize, 4, pixels, imgSize * 4) != 0;
    free(pixels);
    return ok;
}

/* -----------------------------------------------------------------------
 * Preview export (256x256 or 512x512, minimap-style)
 * ----------------------------------------------------------------------- */
static void exportPreviewSetPixel(uint8_t *pixels, int width, int px, int py,
                                  uint8_t r, uint8_t g, uint8_t b) {
    int idx = (py * width + px) * 4;
    pixels[idx]   = r;
    pixels[idx+1] = g;
    pixels[idx+2] = b;
    pixels[idx+3] = 255;
}

static bool exportPreview(const char *filePath, ExportConfig *cfg,
                          map mp, bases bs, pillboxes pb, starts ss) {
    int scale = (cfg->previewSize == ME_PREVIEW_512) ? 2 : 1;
    int width = 256 * scale;
    int height = 256 * scale;

    uint8_t *pixels = (uint8_t *)calloc((size_t)width * height * 4, 1);
    if (!pixels) return false;

    /* Render terrain */
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) {
            BYTE raw = mp->mapItem[x][y];
            uint8_t r, g, b;
            minimapTerrainColor(raw, &r, &g, &b);

            /* Darken mined tiles */
            if (cfg->showMines && raw >= MINE_START && raw <= MINE_END) {
                r = (uint8_t)(r * 7 / 10);
                g = (uint8_t)(g * 7 / 10);
                b = (uint8_t)(b * 7 / 10);
            }

            if (scale == 1) {
                exportPreviewSetPixel(pixels, width, x, y, r, g, b);
            } else {
                exportPreviewSetPixel(pixels, width, x*2,   y*2,   r, g, b);
                exportPreviewSetPixel(pixels, width, x*2+1, y*2,   r, g, b);
                exportPreviewSetPixel(pixels, width, x*2,   y*2+1, r, g, b);
                exportPreviewSetPixel(pixels, width, x*2+1, y*2+1, r, g, b);
            }
        }
    }

    /* Object markers */
    if (cfg->showObjects) {
        for (int i = 0; i < bs->numBases; i++) {
            int bx = bs->item[i].x * scale;
            int by = bs->item[i].y * scale;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    if (bx+dx < width && by+dy < height)
                        exportPreviewSetPixel(pixels, width, bx+dx, by+dy, 60, 60, 255);
        }
        for (int i = 0; i < pb->numPills; i++) {
            int px = pb->item[i].x * scale;
            int py = pb->item[i].y * scale;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    if (px+dx < width && py+dy < height)
                        exportPreviewSetPixel(pixels, width, px+dx, py+dy, 255, 60, 60);
        }
        for (int i = 0; i < ss->numStarts; i++) {
            int sx = ss->item[i].x * scale;
            int sy = ss->item[i].y * scale;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    if (sx+dx < width && sy+dy < height)
                        exportPreviewSetPixel(pixels, width, sx+dx, sy+dy, 60, 255, 60);
        }
    }

    bool ok = stbi_write_png(filePath, width, height, 4, pixels, width * 4) != 0;
    free(pixels);
    return ok;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
bool mapExportPNG(const char *filePath, ExportConfig *cfg,
                  SDL_Renderer *renderer,
                  map mp, bases bs, pillboxes pb, starts ss) {
    if (!filePath || !cfg || !mp || !bs || !pb || !ss) return false;

    if (cfg->mode != ME_EXPORT_FULL) {
        return exportPreview(filePath, cfg, mp, bs, pb, ss);
    }

    /* A 1x atlas of our own: the editor's is rasterized for the current zoom
     * and display density, and the atlas cell coordinates below are 1x. */
    SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X);
    if (!sheet) return false;
    SDL_Texture *tilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
    SDL_DestroySurface(sheet);
    if (!tilesTex) return false;
    SDL_SetTextureScaleMode(tilesTex, SDL_SCALEMODE_NEAREST);

    bool ok = exportFull(filePath, cfg, renderer, tilesTex, TILE_SIZE_X,
                         mp, bs, pb, ss);
    SDL_DestroyTexture(tilesTex);
    return ok;
}
