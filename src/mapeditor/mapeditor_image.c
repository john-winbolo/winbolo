/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_image.c
 * Purpose:
 *   Image import for the map editor. Loads a raster image
 *   and converts pixels to terrain tiles via median-cut
 *   color quantization and nearest-terrain mapping.
 *********************************************************/

#include "mapeditor_image.h"
#include "mapeditor_imgui.h"   /* ME_TRANSPARENT */
#include "../gui/sdl3/minimap_render.h"
#include "../../third_party/stb/stb_image.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* -----------------------------------------------------------
 * Reference terrain colors for auto-mapping
 * ----------------------------------------------------------- */
typedef struct {
    uint8_t r, g, b;
    BYTE    terrain;
} TerrainColorRef;

static const TerrainColorRef s_terrainColors[] = {
    {   0,   0, 128, DEEP_SEA },
    {   0,   0, 255, DEEP_SEA },
    {   0, 128, 255, RIVER },
    {   0, 180,   0, GRASS },
    {   0, 100,   0, FOREST },
    { 128, 128, 128, ROAD },
    { 180, 120,  60, BUILDING },
    { 200, 160, 100, HALFBUILDING },
    { 160, 160,  80, SWAMP },
    { 100,  80,  60, RUBBLE },
    { 140, 120, 100, CRATER },
    { 255, 255, 255, ROAD },
    {   0,   0,   0, BUILDING },
};
#define NUM_TERRAIN_COLORS (sizeof(s_terrainColors) / sizeof(s_terrainColors[0]))

/* -----------------------------------------------------------
 * Helper: Euclidean distance squared in RGB space
 * ----------------------------------------------------------- */
static int colorDistSq(int r1, int g1, int b1, int r2, int g2, int b2) {
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;
    return dr * dr + dg * dg + db * db;
}

/* -----------------------------------------------------------
 * Median-cut color quantization
 * ----------------------------------------------------------- */

typedef struct {
    uint8_t r, g, b;
} PixelRGB;

typedef struct {
    PixelRGB *pixels;
    int count;
    /* Centroid */
    uint8_t cr, cg, cb;
} ColorBox;

static void boxComputeCentroid(ColorBox *box) {
    if (box->count == 0) {
        box->cr = box->cg = box->cb = 0;
        return;
    }
    long sumR = 0, sumG = 0, sumB = 0;
    for (int i = 0; i < box->count; i++) {
        sumR += box->pixels[i].r;
        sumG += box->pixels[i].g;
        sumB += box->pixels[i].b;
    }
    box->cr = (uint8_t)(sumR / box->count);
    box->cg = (uint8_t)(sumG / box->count);
    box->cb = (uint8_t)(sumB / box->count);
}

static int cmpR(const void *a, const void *b) {
    return ((const PixelRGB *)a)->r - ((const PixelRGB *)b)->r;
}
static int cmpG(const void *a, const void *b) {
    return ((const PixelRGB *)a)->g - ((const PixelRGB *)b)->g;
}
static int cmpB(const void *a, const void *b) {
    return ((const PixelRGB *)a)->b - ((const PixelRGB *)b)->b;
}

/* Find the channel with the largest range in a box */
static int boxLargestAxis(const ColorBox *box) {
    uint8_t minR = 255, maxR = 0;
    uint8_t minG = 255, maxG = 0;
    uint8_t minB = 255, maxB = 0;
    for (int i = 0; i < box->count; i++) {
        if (box->pixels[i].r < minR) minR = box->pixels[i].r;
        if (box->pixels[i].r > maxR) maxR = box->pixels[i].r;
        if (box->pixels[i].g < minG) minG = box->pixels[i].g;
        if (box->pixels[i].g > maxG) maxG = box->pixels[i].g;
        if (box->pixels[i].b < minB) minB = box->pixels[i].b;
        if (box->pixels[i].b > maxB) maxB = box->pixels[i].b;
    }
    int rangeR = maxR - minR;
    int rangeG = maxG - minG;
    int rangeB = maxB - minB;
    if (rangeR >= rangeG && rangeR >= rangeB) return 0;
    if (rangeG >= rangeR && rangeG >= rangeB) return 1;
    return 2;
}

/* Median-cut: split numPixels into numColors boxes, return centroids */
static int medianCut(PixelRGB *pixels, int numPixels, int numColors,
                     uint8_t outR[], uint8_t outG[], uint8_t outB[]) {
    if (numPixels == 0 || numColors <= 0) return 0;

    /* Allocate boxes */
    ColorBox boxes[ME_IMAGE_MAX_COLORS];
    int numBoxes = 1;
    boxes[0].pixels = pixels;
    boxes[0].count = numPixels;

    while (numBoxes < numColors) {
        /* Find the largest box (by pixel count) */
        int bestIdx = -1;
        int bestCount = 1; /* need at least 2 to split */
        for (int i = 0; i < numBoxes; i++) {
            if (boxes[i].count > bestCount) {
                bestCount = boxes[i].count;
                bestIdx = i;
            }
        }
        if (bestIdx < 0) break; /* can't split any further */

        /* Sort along the largest axis */
        int axis = boxLargestAxis(&boxes[bestIdx]);
        int (*cmpFn)(const void *, const void *) = (axis == 0) ? cmpR : (axis == 1) ? cmpG : cmpB;
        qsort(boxes[bestIdx].pixels, (size_t)boxes[bestIdx].count,
              sizeof(PixelRGB), cmpFn);

        /* Split at median */
        int mid = boxes[bestIdx].count / 2;
        ColorBox newBox;
        newBox.pixels = boxes[bestIdx].pixels + mid;
        newBox.count = boxes[bestIdx].count - mid;
        boxes[bestIdx].count = mid;
        boxes[numBoxes] = newBox;
        numBoxes++;
    }

    /* Compute centroids */
    for (int i = 0; i < numBoxes; i++) {
        boxComputeCentroid(&boxes[i]);
        outR[i] = boxes[i].cr;
        outG[i] = boxes[i].cg;
        outB[i] = boxes[i].cb;
    }
    return numBoxes;
}

/* -----------------------------------------------------------
 * Compute output dimensions from scale mode and image size
 * ----------------------------------------------------------- */
static void computeOutputDims(ImageImportConfig *cfg) {
    if (!cfg->pixels) return;

    switch (cfg->scaleMode) {
        case ME_SCALE_FIT_SELECTION: {
            int fitW = cfg->selW > 0 ? cfg->selW : 64;
            int fitH = cfg->selH > 0 ? cfg->selH : 64;
            /* Scale to fit within selection bounds, preserving aspect ratio */
            float scaleX = (float)fitW / (float)cfg->imgW;
            float scaleY = (float)fitH / (float)cfg->imgH;
            float scale = scaleX < scaleY ? scaleX : scaleY;
            cfg->outW = (int)(cfg->imgW * scale);
            cfg->outH = (int)(cfg->imgH * scale);
            if (cfg->outW < 1) cfg->outW = 1;
            if (cfg->outH < 1) cfg->outH = 1;
            if (cfg->outW > 256) cfg->outW = 256;
            if (cfg->outH > 256) cfg->outH = 256;
            break;
        }
        case ME_SCALE_FIT_PLAYABLE: {
            float scaleX = 215.0f / (float)cfg->imgW;
            float scaleY = 215.0f / (float)cfg->imgH;
            float scale = scaleX < scaleY ? scaleX : scaleY;
            cfg->outW = (int)(cfg->imgW * scale);
            cfg->outH = (int)(cfg->imgH * scale);
            if (cfg->outW < 1) cfg->outW = 1;
            if (cfg->outH < 1) cfg->outH = 1;
            if (cfg->outW > 256) cfg->outW = 256;
            if (cfg->outH > 256) cfg->outH = 256;
            break;
        }
        case ME_SCALE_1TO1:
        default:
            cfg->outW = cfg->imgW < 256 ? cfg->imgW : 256;
            cfg->outH = cfg->imgH < 256 ? cfg->imgH : 256;
            break;
    }
}

/* -----------------------------------------------------------
 * Downsample image to outW x outH using area averaging
 * Returns malloc'd RGB buffer (caller frees).
 * ----------------------------------------------------------- */
static uint8_t *downsampleImage(const uint8_t *srcRGBA, int srcW, int srcH,
                                int dstW, int dstH, bool nearest) {
    uint8_t *dst = (uint8_t *)malloc((size_t)(dstW * dstH * 3));
    if (!dst) return NULL;

    if (nearest) {
        for (int dy = 0; dy < dstH; dy++) {
            int sy = dy * srcH / dstH;
            if (sy >= srcH) sy = srcH - 1;
            for (int dx = 0; dx < dstW; dx++) {
                int sx = dx * srcW / dstW;
                if (sx >= srcW) sx = srcW - 1;
                const uint8_t *sp = srcRGBA + (sy * srcW + sx) * 4;
                uint8_t *dp = dst + (dy * dstW + dx) * 3;
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
            }
        }
    } else {
        /* Area-average downsampling */
        for (int dy = 0; dy < dstH; dy++) {
            int sy0 = dy * srcH / dstH;
            int sy1 = (dy + 1) * srcH / dstH;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > srcH) sy1 = srcH;
            for (int dx = 0; dx < dstW; dx++) {
                int sx0 = dx * srcW / dstW;
                int sx1 = (dx + 1) * srcW / dstW;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > srcW) sx1 = srcW;
                int sumR = 0, sumG = 0, sumB = 0, count = 0;
                for (int sy = sy0; sy < sy1; sy++) {
                    for (int sx = sx0; sx < sx1; sx++) {
                        const uint8_t *sp = srcRGBA + (sy * srcW + sx) * 4;
                        sumR += sp[0];
                        sumG += sp[1];
                        sumB += sp[2];
                        count++;
                    }
                }
                uint8_t *dp = dst + (dy * dstW + dx) * 3;
                dp[0] = (uint8_t)(sumR / count);
                dp[1] = (uint8_t)(sumG / count);
                dp[2] = (uint8_t)(sumB / count);
            }
        }
    }
    return dst;
}

/* -----------------------------------------------------------
 * Public API
 * ----------------------------------------------------------- */

void imageImportLoad(ImageImportConfig *cfg, const char *path,
                     SDL_Renderer *renderer) {
    /* Free previous */
    if (cfg->pixels) {
        stbi_image_free(cfg->pixels);
        cfg->pixels = NULL;
    }
    if (cfg->previewTex) {
        SDL_DestroyTexture(cfg->previewTex);
        cfg->previewTex = NULL;
    }
    if (cfg->resultPreviewTex) {
        SDL_DestroyTexture(cfg->resultPreviewTex);
        cfg->resultPreviewTex = NULL;
    }
    cfg->resultReady = false;

    cfg->errorMsg[0] = '\0';

    /* Load image */
    int w, h, channels;
    uint8_t *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) {
        snprintf(cfg->errorMsg, sizeof(cfg->errorMsg),
                 "Failed to load image: %s", stbi_failure_reason());
        strncpy(cfg->filePath, path, sizeof(cfg->filePath) - 1);
        cfg->filePath[sizeof(cfg->filePath) - 1] = '\0';
        return;
    }

    cfg->pixels = data;
    cfg->imgW = w;
    cfg->imgH = h;
    strncpy(cfg->filePath, path, sizeof(cfg->filePath) - 1);
    cfg->filePath[sizeof(cfg->filePath) - 1] = '\0';

    /* Create preview texture (scaled to fit ~512x512) */
    int prevW = w, prevH = h;
    if (prevW > 512 || prevH > 512) {
        float scale = 512.0f / (prevW > prevH ? (float)prevW : (float)prevH);
        prevW = (int)(prevW * scale);
        prevH = (int)(prevH * scale);
        if (prevW < 1) prevW = 1;
        if (prevH < 1) prevH = 1;
    }

    /* Downsample for preview */
    uint8_t *prevPixels = downsampleImage(data, w, h, prevW, prevH, false);
    if (prevPixels) {
        /* Convert RGB to RGBA for SDL texture */
        uint8_t *rgba = (uint8_t *)malloc((size_t)(prevW * prevH * 4));
        if (rgba) {
            for (int i = 0; i < prevW * prevH; i++) {
                rgba[i * 4 + 0] = prevPixels[i * 3 + 0];
                rgba[i * 4 + 1] = prevPixels[i * 3 + 1];
                rgba[i * 4 + 2] = prevPixels[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
            SDL_Surface *surf = SDL_CreateSurfaceFrom(prevW, prevH,
                SDL_PIXELFORMAT_RGBA32, rgba, prevW * 4);
            if (surf) {
                cfg->previewTex = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_DestroySurface(surf);
            }
            free(rgba);
        }
        free(prevPixels);
    }
}

void imageImportAutoMap(ImageImportConfig *cfg) {
    if (!cfg->pixels) return;

    /* Recompute output dimensions */
    computeOutputDims(cfg);

    /* Downsample to output dimensions */
    bool nearest = (cfg->scaleMode == ME_SCALE_1TO1);
    uint8_t *downsampled = downsampleImage(cfg->pixels, cfg->imgW, cfg->imgH,
                                           cfg->outW, cfg->outH, nearest);
    if (!downsampled) return;

    /* Build pixel array for median cut */
    int numPixels = cfg->outW * cfg->outH;
    PixelRGB *pixArr = (PixelRGB *)malloc((size_t)numPixels * sizeof(PixelRGB));
    if (!pixArr) {
        free(downsampled);
        return;
    }
    for (int i = 0; i < numPixels; i++) {
        pixArr[i].r = downsampled[i * 3 + 0];
        pixArr[i].g = downsampled[i * 3 + 1];
        pixArr[i].b = downsampled[i * 3 + 2];
    }
    free(downsampled);

    /* Median-cut quantization */
    uint8_t centR[ME_IMAGE_MAX_COLORS], centG[ME_IMAGE_MAX_COLORS], centB[ME_IMAGE_MAX_COLORS];
    int nc = medianCut(pixArr, numPixels, cfg->numColors, centR, centG, centB);
    free(pixArr);

    /* Auto-map each centroid to nearest terrain color */
    cfg->numMappings = nc;
    for (int i = 0; i < nc; i++) {
        cfg->mappings[i].r = centR[i];
        cfg->mappings[i].g = centG[i];
        cfg->mappings[i].b = centB[i];
        cfg->mappings[i].enabled = true;

        /* Find nearest terrain */
        int bestDist = INT32_MAX;
        BYTE bestTerrain = GRASS;
        for (int t = 0; t < (int)NUM_TERRAIN_COLORS; t++) {
            int d = colorDistSq(centR[i], centG[i], centB[i],
                                s_terrainColors[t].r, s_terrainColors[t].g,
                                s_terrainColors[t].b);
            if (d < bestDist) {
                bestDist = d;
                bestTerrain = s_terrainColors[t].terrain;
            }
        }
        cfg->mappings[i].terrain = bestTerrain;
    }
}

void imageImportConvert(ImageImportConfig *cfg, SDL_Renderer *renderer) {
    if (!cfg->pixels || cfg->numMappings == 0) return;

    computeOutputDims(cfg);

    /* Downsample to output dimensions */
    bool nearest = (cfg->scaleMode == ME_SCALE_1TO1);
    uint8_t *downsampled = downsampleImage(cfg->pixels, cfg->imgW, cfg->imgH,
                                           cfg->outW, cfg->outH, nearest);
    if (!downsampled) return;

    /* Map each pixel to nearest enabled color mapping */
    cfg->resultW = cfg->outW;
    cfg->resultH = cfg->outH;

    for (int x = 0; x < cfg->outW; x++) {
        for (int y = 0; y < cfg->outH; y++) {
            /* Check alpha from source image at corresponding position */
            int srcX = x * cfg->imgW / cfg->outW;
            int srcY = y * cfg->imgH / cfg->outH;
            if (srcX >= cfg->imgW) srcX = cfg->imgW - 1;
            if (srcY >= cfg->imgH) srcY = cfg->imgH - 1;
            uint8_t alpha = cfg->pixels[(srcY * cfg->imgW + srcX) * 4 + 3];
            if (alpha < 128) {
                cfg->resultTerrain[x][y] = ME_TRANSPARENT;
                continue;
            }

            int idx = (y * cfg->outW + x) * 3;
            int pr = downsampled[idx + 0];
            int pg = downsampled[idx + 1];
            int pb = downsampled[idx + 2];

            int bestDist = INT32_MAX;
            BYTE bestTerrain = ME_TRANSPARENT;

            for (int m = 0; m < cfg->numMappings; m++) {
                if (!cfg->mappings[m].enabled) continue;
                int d = colorDistSq(pr, pg, pb,
                                    cfg->mappings[m].r, cfg->mappings[m].g,
                                    cfg->mappings[m].b);
                if (d < bestDist) {
                    bestDist = d;
                    bestTerrain = cfg->mappings[m].terrain;
                }
            }

            cfg->resultTerrain[x][y] = bestTerrain;
        }
    }
    free(downsampled);

    cfg->resultReady = true;

    /* Rebuild result preview texture using minimap terrain colors */
    if (cfg->resultPreviewTex) {
        SDL_DestroyTexture(cfg->resultPreviewTex);
        cfg->resultPreviewTex = NULL;
    }

    /* Scale to fit ~512x512 */
    int prevW = cfg->resultW, prevH = cfg->resultH;
    if (prevW > 512 || prevH > 512) {
        float scale = 512.0f / (prevW > prevH ? (float)prevW : (float)prevH);
        prevW = (int)(prevW * scale);
        prevH = (int)(prevH * scale);
        if (prevW < 1) prevW = 1;
        if (prevH < 1) prevH = 1;
    }

    uint8_t *rgba = (uint8_t *)malloc((size_t)(prevW * prevH * 4));
    if (rgba) {
        for (int py = 0; py < prevH; py++) {
            int ty = py * cfg->resultH / prevH;
            if (ty >= cfg->resultH) ty = cfg->resultH - 1;
            for (int px = 0; px < prevW; px++) {
                int tx = px * cfg->resultW / prevW;
                if (tx >= cfg->resultW) tx = cfg->resultW - 1;
                BYTE t = cfg->resultTerrain[tx][ty];
                uint8_t r, g, b;
                if (t == ME_TRANSPARENT) {
                    r = g = b = 40; /* dark gray for transparent */
                } else {
                    minimapTerrainColor(t, &r, &g, &b);
                }
                int i = (py * prevW + px) * 4;
                rgba[i + 0] = r;
                rgba[i + 1] = g;
                rgba[i + 2] = b;
                rgba[i + 3] = 255;
            }
        }
        SDL_Surface *surf = SDL_CreateSurfaceFrom(prevW, prevH,
            SDL_PIXELFORMAT_RGBA32, rgba, prevW * 4);
        if (surf) {
            cfg->resultPreviewTex = SDL_CreateTextureFromSurface(renderer, surf);
            SDL_DestroySurface(surf);
        }
        free(rgba);
    }
}

void imageImportFree(ImageImportConfig *cfg) {
    if (cfg->pixels) {
        stbi_image_free(cfg->pixels);
    }
    if (cfg->previewTex) {
        SDL_DestroyTexture(cfg->previewTex);
    }
    if (cfg->resultPreviewTex) {
        SDL_DestroyTexture(cfg->resultPreviewTex);
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->numColors = 10; /* Reset default */
}
