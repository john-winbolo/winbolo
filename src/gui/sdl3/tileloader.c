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
 * Name:          tileloader.c
 * Purpose:
 *   Builds the 496x176 RGBA32 sprite sheet from individual
 *   SVG/PNG files, falling back to skin.bmp cutouts.
 *********************************************************/

#include "tileloader.h"
#include "tilemap.h"
#include "gfx_settings.h"
#include "../tiles.h"

#include "nanosvg.h"
#include "nanosvgrast.h"

#include "stb_image.h"

#include <stdio.h>
#include <string.h>

/* Copy RGBA pixel data into the sheet surface at (dstX, dstY). */
static void blitRGBA(SDL_Surface *sheet, int dstX, int dstY,
                     int w, int h, const unsigned char *pixels) {
    if (!pixels) return;
    unsigned char *dst = (unsigned char *)sheet->pixels;
    int pitch = sheet->pitch;
    for (int row = 0; row < h; row++) {
        if (dstY + row < 0 || dstY + row >= sheet->h) continue;
        const unsigned char *src = pixels + row * w * 4;
        unsigned char *dstRow = dst + (dstY + row) * pitch + dstX * 4;
        for (int col = 0; col < w; col++) {
            if (dstX + col < 0 || dstX + col >= sheet->w) continue;
            dstRow[col * 4 + 0] = src[col * 4 + 0];
            dstRow[col * 4 + 1] = src[col * 4 + 1];
            dstRow[col * 4 + 2] = src[col * 4 + 2];
            dstRow[col * 4 + 3] = src[col * 4 + 3];
        }
    }
}

/* Try loading an SVG file and rasterizing it at the given size.
 * Uses SDL_LoadFile so that Android APK assets are accessible.
 * Returns true on success and writes RGBA pixels into `out`.
 *
 * When `pointSample` is true, rasterize at 4× the target size and
 * pick each output pixel from the centre of its 4×4 source block.
 * That gives crisp pixel-art output (each output pixel = the SVG
 * colour at that pixel's exact centre, no anti-aliased edge blending).
 * Used when AllowSvg is off — preserves the SVG art's intent at
 * pixel resolution without smoothing. */
static bool tryLoadSVGEx(const char *path, int w, int h,
                         unsigned char *out, NSVGrasterizer *rast,
                         bool pointSample) {
    size_t fileSize = 0;
    char *fileData = (char *)SDL_LoadFile(path, &fileSize);
    if (!fileData) return false;
    NSVGimage *image = nsvgParse(fileData, "px", 96.0f);
    SDL_free(fileData);
    if (!image) return false;
    if (image->width < 1.0f || image->height < 1.0f) {
        nsvgDelete(image);
        return false;
    }
    if (!pointSample) {
        float scaleX = (float)w / image->width;
        float scaleY = (float)h / image->height;
        float scale = scaleX < scaleY ? scaleX : scaleY;
        memset(out, 0, (size_t)(w * h * 4));
        nsvgRasterize(rast, image, 0, 0, scale, out, w, h, w * 4);
        nsvgDelete(image);
        return true;
    }
    /* Centre-sample path: rasterize at 4× and pick centre of each
     * 4×4 source block.  hi pixel index = 4*outIdx + 2. */
    const int kSuper = 4;
    int hiW = w * kSuper;
    int hiH = h * kSuper;
    unsigned char *hi = (unsigned char *)SDL_malloc((size_t)(hiW * hiH * 4));
    if (!hi) { nsvgDelete(image); return false; }
    memset(hi, 0, (size_t)(hiW * hiH * 4));
    float scaleX = (float)hiW / image->width;
    float scaleY = (float)hiH / image->height;
    float scale = scaleX < scaleY ? scaleX : scaleY;
    nsvgRasterize(rast, image, 0, 0, scale, hi, hiW, hiH, hiW * 4);
    nsvgDelete(image);
    memset(out, 0, (size_t)(w * h * 4));
    for (int y = 0; y < h; y++) {
        int sy = y * kSuper + kSuper / 2;
        for (int x = 0; x < w; x++) {
            int sx = x * kSuper + kSuper / 2;
            const unsigned char *sp = hi + (sy * hiW + sx) * 4;
            unsigned char *dp = out + (y * w + x) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
    SDL_free(hi);
    return true;
}

/* Backwards-compat wrapper.  Ingamerotate themes always use point-
 * sampling regardless of AllowSvg — the toggle is ignored for those. */
static bool tryLoadSVG(const char *path, int w, int h,
                       unsigned char *out, NSVGrasterizer *rast) {
    bool pointSample = !gfxSettingsGetAllowSvg()
                       || tileLoaderThemeRotates();
    return tryLoadSVGEx(path, w, h, out, rast, pointSample);
}

/* Try loading a PNG file via stb_image.
 * Uses SDL_IOFromFile so that Android APK assets are accessible.
 * Returns true on success and writes RGBA pixels into `out`. */
static bool tryLoadPNG(const char *path, int w, int h,
                       unsigned char *out) {
    int imgW = 0, imgH = 0, channels = 0;
    unsigned char *data = NULL;

    /* Read file via SDL I/O (works with Android assets). */
    size_t fileSize = 0;
    void *fileData = SDL_LoadFile(path, &fileSize);
    if (fileData && fileSize > 0) {
        data = stbi_load_from_memory((const unsigned char *)fileData,
                                     (int)fileSize,
                                     &imgW, &imgH, &channels, 4);
        SDL_free(fileData);
    }
    if (!data) return false;

    /* If sizes match exactly, just copy. */
    if (imgW == w && imgH == h) {
        memcpy(out, data, (size_t)(w * h * 4));
        stbi_image_free(data);
        return true;
    }

    /* Nearest-neighbor scale to target size. */
    memset(out, 0, (size_t)(w * h * 4));
    for (int y = 0; y < h; y++) {
        int srcY = y * imgH / h;
        if (srcY >= imgH) srcY = imgH - 1;
        for (int x = 0; x < w; x++) {
            int srcX = x * imgW / w;
            if (srcX >= imgW) srcX = imgW - 1;
            const unsigned char *sp = data + (srcY * imgW + srcX) * 4;
            unsigned char *dp = out + (y * w + x) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
    stbi_image_free(data);
    return true;
}

/* Blit a rectangle from the BMP fallback surface (with color key applied)
 * into the RGBA sheet. The BMP surface has a green color key so green
 * pixels become transparent in the output. */
static void blitFromBMP(SDL_Surface *sheet, SDL_Surface *bmp,
                        int dstX, int dstY, int srcX, int srcY,
                        int w, int h) {
    SDL_Rect srcRect = { srcX, srcY, w, h };
    SDL_Rect dstRect = { dstX, dstY, w, h };
    SDL_BlitSurface(bmp, &srcRect, sheet, &dstRect);
}

/* Theme override.  When non-empty, tileLoaderBuildSheet looks for
 * sprites in data/theme/<themeName>/<name>.svg|png BEFORE falling
 * back to data/svg/<name>.svg|png and finally the BMP. */
static char s_themeName[64] = "";

void tileLoaderSetTheme(const char *name) {
    if (!name || !name[0]) {
        s_themeName[0] = '\0';
    } else {
        SDL_strlcpy(s_themeName, name, sizeof(s_themeName));
    }
}

const char *tileLoaderGetTheme(void) {
    return s_themeName;
}

bool tileLoaderThemeRotates(void) {
    if (!s_themeName[0]) return false;
    /* Convention: any theme dir ending with "_ingamerotate" only
     * ships the north-facing (_00) variant of rotation groups. */
    size_t n = SDL_strlen(s_themeName);
    const char *suffix = "_ingamerotate";
    size_t sn = SDL_strlen(suffix);
    if (n < sn) return false;
    return SDL_strcmp(s_themeName + (n - sn), suffix) == 0;
}

SDL_Surface *tileLoaderBuildSheet(int tileSize) {
    /* Scale factor: tileSize / BASE_TILE (16).  When tileSize==16, scale==1
       and the sheet is the classic 496x176.  When tileSize==32, scale==2
       and SVGs are rasterized at 2x for crisper rendering. */
    int scale = tileSize / TILE_SIZE_X;
    if (scale < 1) scale = 1;

    int sheetW = TILE_FILE_X * scale;
    int sheetH = TILE_FILE_Y * scale;

    /* Create the output RGBA32 surface at scaled size. */
    SDL_Surface *sheet = SDL_CreateSurface(sheetW, sheetH,
                                           SDL_PIXELFORMAT_RGBA32);
    if (!sheet) {
        SDL_Log("tileLoaderBuildSheet: SDL_CreateSurface(%dx%d) failed: %s",
                sheetW, sheetH, SDL_GetError());
        return NULL;
    }

    /* Clear to fully transparent. */
    SDL_memset(sheet->pixels, 0, (size_t)(sheet->pitch * sheet->h));

    /* Load the BMP fallback surface and apply green color key.
       If scale > 1 we scale the BMP up so it lands at the right position. */
    SDL_Surface *bmpRaw = SDL_LoadBMP("data/skin.bmp");
    SDL_Surface *bmp = NULL;
    if (bmpRaw) {
        Uint32 key = SDL_MapRGB(SDL_GetPixelFormatDetails(bmpRaw->format),
                                NULL, 0, 255, 0);
        SDL_SetSurfaceColorKey(bmpRaw, true, key);
        bmp = SDL_ConvertSurface(bmpRaw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(bmpRaw);
    }
    if (!bmp) {
        SDL_Log("tileLoaderBuildSheet: warning — could not load data/skin.bmp fallback");
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();

    /* Temp buffer: largest sprite is indent tiles (54x54) scaled up. */
    int maxSpriteSize = 54 * scale;
    unsigned char *tmpBuf = (unsigned char *)SDL_malloc(
        (size_t)(maxSpriteSize * maxSpriteSize * 4));

    char pathBuf[512];
    int svgCount = 0, pngCount = 0, bmpCount = 0;

    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const TileMapEntry *e = &gTileMap[i];
        int w = e->width  * scale;
        int h = e->height * scale;
        int dstX = e->sheetX * scale;
        int dstY = e->sheetY * scale;
        bool loaded = false;

        /* Theme override: try data/theme/<theme>/<name>.svg|png first.
         * SVGs are always loaded — themes that only ship SVG (like
         * stock_svg_ingamerotate) need them to display at all.  The
         * AllowSvg toggle only affects the atlas SCALE MODE (linear
         * vs nearest), not which sources are loaded. */
        if (!loaded && s_themeName[0]) {
            SDL_snprintf(pathBuf, sizeof(pathBuf),
                         "data/theme/%s/%s.svg", s_themeName, e->name);
            if (tryLoadSVG(pathBuf, w, h, tmpBuf, rast)) {
                blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                loaded = true;
                svgCount++;
            }
            if (!loaded) {
                SDL_snprintf(pathBuf, sizeof(pathBuf),
                             "data/theme/%s/%s.png", s_themeName, e->name);
                if (tryLoadPNG(pathBuf, w, h, tmpBuf)) {
                    blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                    loaded = true;
                    pngCount++;
                }
            }
            /* Ingamerotate themes only ship _00 of rotation groups —
             * the per-direction _01..15 atlas slots stay empty here
             * and runtime code (mapview.c) detects this via
             * tileLoaderThemeRotates() and uses SDL_RenderTextureRotated
             * to rotate _00 at draw time. */
        }

        /* Try SVG first — rasterized at scaled size. */
        SDL_snprintf(pathBuf, sizeof(pathBuf), "data/svg/%s.svg", e->name);
        if (!loaded && tryLoadSVG(pathBuf, w, h, tmpBuf, rast)) {
            blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
            loaded = true;
            svgCount++;
        }

        /* Try PNG next — scaled to target size. */
        if (!loaded) {
            SDL_snprintf(pathBuf, sizeof(pathBuf), "data/svg/%s.png", e->name);
            if (tryLoadPNG(pathBuf, w, h, tmpBuf)) {
                blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                loaded = true;
                pngCount++;
            }
        }

        /* Fall back to BMP — blit at 1x then scale up if needed. */
        if (!loaded && bmp) {
            if (scale == 1) {
                blitFromBMP(sheet, bmp, dstX, dstY,
                            e->sheetX, e->sheetY, e->width, e->height);
            } else {
                /* Blit BMP into tmpBuf at 1x, then nearest-neighbor scale up. */
                SDL_Surface *tmpSurf = SDL_CreateSurface(e->width, e->height,
                                                         SDL_PIXELFORMAT_RGBA32);
                if (tmpSurf) {
                    SDL_Rect srcR = { e->sheetX, e->sheetY, e->width, e->height };
                    SDL_Rect dstR = { 0, 0, e->width, e->height };
                    SDL_BlitSurface(bmp, &srcR, tmpSurf, &dstR);
                    /* Nearest-neighbor scale into sheet */
                    unsigned char *sp = (unsigned char *)tmpSurf->pixels;
                    unsigned char *dp = (unsigned char *)sheet->pixels;
                    for (int row = 0; row < h; row++) {
                        int srcRow = row * e->height / h;
                        for (int col = 0; col < w; col++) {
                            int srcCol = col * e->width / w;
                            const unsigned char *s = sp + (srcRow * tmpSurf->pitch) + srcCol * 4;
                            unsigned char *d = dp + ((dstY + row) * sheet->pitch) + (dstX + col) * 4;
                            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                        }
                    }
                    SDL_DestroySurface(tmpSurf);
                }
            }
            bmpCount++;
        }
    }

    SDL_Log("tileLoaderBuildSheet: scale=%d, sheet=%dx%d, loaded %d SVG, %d PNG, %d BMP fallback sprites",
            scale, sheetW, sheetH, svgCount, pngCount, bmpCount);

    SDL_free(tmpBuf);
    if (rast) nsvgDeleteRasterizer(rast);
    if (bmp) SDL_DestroySurface(bmp);

    return sheet;
}

void tileLoaderCleanup(void) {
    /* Currently no persistent state to free.
     * Reserved for future caching (e.g. keeping parsed SVGs for re-rasterization). */
}
