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
 * Name:          tileloader.c
 * Purpose:
 *   Builds the 496x176 RGBA32 sprite sheet from individual
 *   SVG/PNG files, falling back to skin.bmp cutouts.
 *********************************************************/

#include "tileloader.h"
#include "tilemap.h"
#include "skin_source.h"
#include "../tiles.h"

#include "platform_types.h"   /* BOLO_STATIC_ASSERT */

#include "../../common/wb_log.h"

#include "nanosvg.h"
#include "nanosvgrast.h"

#include "stb_image.h"

#include <stdio.h>
#include <string.h>

/* SkinDensityInfo carries one byte per tilemap entry in a fixed array. */
BOLO_STATIC_ASSERT(TILE_MAP_COUNT <= SKIN_DENSITY_MAX_SPRITES,
                   tile_map_fits_skin_density_info);

/* HUD art: the menu indents, the status pane items, the gunsight, the
 * mouse square, the status tank icon, the transparent tank and the static
 * screen.  These are left out of the per-density coverage count, so a skin
 * that redraws every terrain tile but no HUD art still counts as covering a
 * density in full.  They keep a per-sprite max of their own either way. */
static bool isNonWorldSprite(const char *name) {
    static const char *hudPrefixes[] = {
        "indent",
        "status_",
        "gunsight",
        "mouse_square",
        "tank_icon",
        "tank_transparent",
        "static"
    };

    for (int i = 0; i < (int)(sizeof(hudPrefixes) / sizeof(hudPrefixes[0])); i++) {
        if (strncmp(name, hudPrefixes[i], strlen(hudPrefixes[i])) == 0) return true;
    }
    return false;
}

void tileLoaderScanDensity(struct SkinSource *skin, SkinDensityInfo *out) {
    if (!out) return;

    SDL_memset(out, 0, sizeof(*out));
    out->spriteCount = (int)TILE_MAP_COUNT;

    /* Density 1 needs no files: the per-sprite chain bottoms out at the
       built-in assets, so every skin serves it in full. */
    out->coverage[1] = SKIN_DENSITY_COVER_ALL;
    out->highestAll  = 1;
    out->highestAny  = 1;
    for (int i = 0; i < out->spriteCount; i++) {
        out->spriteMax[i] = 1;
    }
    if (!skin) return;

    SkinInfo skinInfo;
    skinSourceReadIni(skin, &skinInfo);
    /* MaxPixelDensity=0 means unlimited, so an SVG counts at every density.
       A non-zero value is the author saying their vector art is not meant to
       be rasterized finer than that.  @Nx PNGs are never capped: shipping
       the file is the statement. */
    int svgCap = skinInfo.maxPixelDensity;

    int worldTotal = 0;
    int worldHits[SKIN_DENSITY_MAX + 1];
    SDL_memset(worldHits, 0, sizeof(worldHits));

    /* Name-index lookups only.  skinSourceExists is O(1) for a directory and
       for an archive alike, so the whole scan costs no file I/O. */
    char nameBuf[SKIN_PATH_MAX];
    for (int i = 0; i < out->spriteCount; i++) {
        const char *name = gTileMap[i].name;
        bool isWorld = !isNonWorldSprite(name);
        if (isWorld) worldTotal++;

        SDL_snprintf(nameBuf, sizeof(nameBuf), "%s.svg", name);
        bool hasSvg = skinSourceExists(skin, nameBuf);

        for (int n = 2; n <= SKIN_DENSITY_MAX; n++) {
            bool has = hasSvg && (svgCap == 0 || n <= svgCap);
            if (!has) {
                SDL_snprintf(nameBuf, sizeof(nameBuf), "%s@%dx.png", name, n);
                has = skinSourceExists(skin, nameBuf);
            }
            if (!has) continue;

            if (out->spriteMax[i] < n) out->spriteMax[i] = (unsigned char)n;
            if (out->highestAny < n)   out->highestAny   = n;
            if (isWorld) worldHits[n]++;
        }
    }

    for (int n = 2; n <= SKIN_DENSITY_MAX; n++) {
        if (worldTotal > 0 && worldHits[n] == worldTotal) {
            out->coverage[n] = SKIN_DENSITY_COVER_ALL;
            out->highestAll  = n;
        } else if (worldHits[n] > 0) {
            out->coverage[n] = SKIN_DENSITY_COVER_SOME;
        }
    }
}

/* One-entry scan cache.  skinSetActive closes and reopens the source, so a
   different skin always presents a different pointer.  The valid flag is
   what makes a NULL skin a real key rather than "not scanned yet". */
static struct SkinSource *s_densitySkin  = NULL;
static SkinDensityInfo    s_densityInfo;
static bool               s_densityValid = false;

const SkinDensityInfo *tileLoaderGetDensityInfo(struct SkinSource *skin) {
    if (!s_densityValid || s_densitySkin != skin) {
        tileLoaderScanDensity(skin, &s_densityInfo);
        s_densitySkin  = skin;
        s_densityValid = true;
    }
    return &s_densityInfo;
}

int tileLoaderPickDensity(const SkinDensityInfo *info, int spriteIndex,
                          int mode, int scale) {
    int density;

    if (!info || scale < 1 ||
        spriteIndex < 0 || spriteIndex >= SKIN_DENSITY_MAX_SPRITES) {
        return 1;
    }

    if (mode == TILE_DETAIL_MATCH_ZOOM) {
        /* One density for the whole sheet, so the sprite does not matter. */
        density = info->highestAll < scale ? info->highestAll : scale;
    } else if (mode == TILE_DETAIL_HIGH) {
        density = info->spriteMax[spriteIndex];
    } else {
        return 1;   /* Classic, and anything unrecognised */
    }
    return density < 1 ? 1 : density;
}

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

/* Rasterize SVG bytes at the given size.  nanosvg parses in place, so `data`
 * must be writable and NUL-terminated at [len] — both SDL_LoadFile and
 * skinSourceRead hand back a buffer like that.
 * Returns true on success and writes RGBA pixels into `out`. */
static bool decodeSVG(char *data, size_t len, int w, int h,
                      unsigned char *out, NSVGrasterizer *rast) {
    (void)len;
    NSVGimage *image = nsvgParse(data, "px", 96.0f);
    if (!image) return false;
    if (image->width < 1.0f || image->height < 1.0f) {
        nsvgDelete(image);
        return false;
    }
    float scaleX = (float)w / image->width;
    float scaleY = (float)h / image->height;
    float scale = scaleX < scaleY ? scaleX : scaleY;
    memset(out, 0, (size_t)(w * h * 4));
    nsvgRasterize(rast, image, 0, 0, scale, out, w, h, w * 4);
    nsvgDelete(image);
    return true;
}

/* Decode PNG bytes via stb_image, scaling to the target size when they do
 * not already match.
 * Returns true on success and writes RGBA pixels into `out`. */
static bool decodePNG(const void *data, size_t len, int w, int h,
                      unsigned char *out) {
    int imgW = 0, imgH = 0, channels = 0;
    unsigned char *img = NULL;

    if (data && len > 0) {
        img = stbi_load_from_memory((const unsigned char *)data, (int)len,
                                    &imgW, &imgH, &channels, 4);
    }
    if (!img) return false;

    /* If sizes match exactly, just copy. */
    if (imgW == w && imgH == h) {
        memcpy(out, img, (size_t)(w * h * 4));
        stbi_image_free(img);
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
            const unsigned char *sp = img + (srcY * imgW + srcX) * 4;
            unsigned char *dp = out + (y * w + x) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
    stbi_image_free(img);
    return true;
}

/* Try loading an SVG file and rasterizing it at the given size.
 * Uses SDL_LoadFile so that Android APK assets are accessible.
 * Returns true on success and writes RGBA pixels into `out`. */
static bool tryLoadSVG(const char *path, int w, int h,
                       unsigned char *out, NSVGrasterizer *rast) {
    size_t fileSize = 0;
    char *fileData = (char *)SDL_LoadFile(path, &fileSize);
    if (!fileData) return false;
    bool ok = decodeSVG(fileData, fileSize, w, h, out, rast);
    SDL_free(fileData);
    return ok;
}

/* Try loading a PNG file via stb_image.
 * Uses SDL_LoadFile so that Android APK assets are accessible.
 * Returns true on success and writes RGBA pixels into `out`. */
static bool tryLoadPNG(const char *path, int w, int h,
                       unsigned char *out) {
    size_t fileSize = 0;
    void *fileData = SDL_LoadFile(path, &fileSize);
    if (!fileData) return false;
    bool ok = decodePNG(fileData, fileSize, w, h, out);
    SDL_free(fileData);
    return ok;
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

/* Cut one sprite out of a whole-sheet BMP (either the skin's own or
 * data/skin.bmp) into the output sheet, nearest-neighbor scaled when the
 * tile size is above 16.  False when there is no source sheet. */
static bool blitSheetSprite(SDL_Surface *sheet, SDL_Surface *src,
                            const TileMapEntry *e, int scale) {
    if (!src) return false;

    int w = e->width  * scale;
    int h = e->height * scale;
    int dstX = e->sheetX * scale;
    int dstY = e->sheetY * scale;

    if (scale == 1) {
        blitFromBMP(sheet, src, dstX, dstY,
                    e->sheetX, e->sheetY, e->width, e->height);
    } else {
        /* Blit the sprite at 1x, then nearest-neighbor scale it up. */
        SDL_Surface *tmpSurf = SDL_CreateSurface(e->width, e->height,
                                                 SDL_PIXELFORMAT_RGBA32);
        if (tmpSurf) {
            SDL_Rect srcR = { e->sheetX, e->sheetY, e->width, e->height };
            SDL_Rect dstR = { 0, 0, e->width, e->height };
            SDL_BlitSurface(src, &srcR, tmpSurf, &dstR);
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
    return true;
}

/* Apply the green color key to a freshly loaded sheet BMP and convert it to
 * RGBA32 so the key becomes real transparency.  Consumes `raw`; NULL in
 * gives NULL out. */
static SDL_Surface *loadKeyedSheetFromSurface(SDL_Surface *raw) {
    if (!raw) return NULL;
    Uint32 key = SDL_MapRGB(SDL_GetPixelFormatDetails(raw->format),
                            NULL, 0, 255, 0);
    SDL_SetSurfaceColorKey(raw, true, key);
    SDL_Surface *out = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    return out;
}

/* Load the skin's own whole sheet — tiles.bmp, else skin.bmp — color-keyed
 * the same way as data/skin.bmp.  NULL when the skin holds neither. */
static SDL_Surface *loadSkinSheet(struct SkinSource *skin) {
    static const char *names[] = { "tiles.bmp", "skin.bmp" };

    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        void *buf = NULL;
        size_t len = 0;
        if (!skinSourceRead(skin, names[i], &buf, &len)) continue;
        SDL_Surface *keyed = NULL;
        SDL_IOStream *io = len > 0 ? SDL_IOFromMem(buf, len) : NULL;
        if (io) {
            /* true closes the stream for us; the buffer stays ours to free. */
            keyed = loadKeyedSheetFromSurface(SDL_LoadBMP_IO(io, true));
        }
        SDL_free(buf);
        if (keyed) return keyed;
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: skin %s is not a usable BMP",
                names[i]);
    }
    return NULL;
}

SDL_Surface *tileLoaderBuildSheetFor(struct SkinSource *skin, int tileSize) {
    /* Scale factor: tileSize / BASE_TILE (16).  When tileSize==16, scale==1
       and the sheet is the classic 496x176.  When tileSize==32, scale==2
       and SVGs are rasterized at 2x for crisper rendering. */
    int scale = tileSize / TILE_SIZE_X;
    if (scale < 1) scale = 1;

    int sheetW = TILE_FILE_X * scale;
    int sheetH = TILE_FILE_Y * scale;

    /* Use SDL_GetBasePath() so asset files are found regardless of CWD.
       On macOS the CWD often differs from the executable directory. */
    const char *basePath = SDL_GetBasePath();
    if (!basePath) basePath = "";

    /* Create the output RGBA32 surface at scaled size. */
    SDL_Surface *sheet = SDL_CreateSurface(sheetW, sheetH,
                                           SDL_PIXELFORMAT_RGBA32);
    if (!sheet) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: SDL_CreateSurface(%dx%d) failed: %s",
                sheetW, sheetH, SDL_GetError());
        return NULL;
    }

    /* Clear to fully transparent. */
    SDL_memset(sheet->pixels, 0, (size_t)(sheet->pitch * sheet->h));

    /* Load the BMP fallback surface and apply green color key.
       If scale > 1 we scale the BMP up so it lands at the right position. */
    char bmpPathBuf[512];
    SDL_snprintf(bmpPathBuf, sizeof(bmpPathBuf), "%sdata/skin.bmp", basePath);
    SDL_Surface *bmp = loadKeyedSheetFromSurface(SDL_LoadBMP(bmpPathBuf));
    if (!bmp) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: could not load %s fallback", bmpPathBuf);
    }

    /* The skin's whole sheet is read once and cropped per sprite, so a
       sheet-only skin costs one decode per build. */
    SDL_Surface *skinSheet = skin ? loadSkinSheet(skin) : NULL;

    /* Name the skin in the summary below so a skin resolving to the wrong
       assets can be told apart from the built-in set in the log. */
    const char *skinLabel = "none";
    SkinInfo    skinInfo;
    if (skin) {
        if (skin == skinGetActiveSource() && skinGetActive()[0] != '\0') {
            skinLabel = skinGetActive();
        } else {
            skinSourceReadIni(skin, &skinInfo);
            skinLabel = skinInfo.name[0] ? skinInfo.name : "(unnamed)";
        }
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();

    /* Temp buffer: largest sprite is indent tiles (54x54) scaled up. */
    int maxSpriteSize = 54 * scale;
    unsigned char *tmpBuf = (unsigned char *)SDL_malloc(
        (size_t)(maxSpriteSize * maxSpriteSize * 4));

    char pathBuf[512];
    int svgCount = 0, pngCount = 0, bmpCount = 0;
    int skinSvgCount = 0, skinPngCount = 0, skinSheetCount = 0;

    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const TileMapEntry *e = &gTileMap[i];
        int w = e->width  * scale;
        int h = e->height * scale;
        int dstX = e->sheetX * scale;
        int dstY = e->sheetY * scale;
        bool loaded = false;

        /* The skin gets first refusal on every sprite: its own SVG, then its
           own PNG, then a cutout of its whole sheet. */
        if (skin) {
            void *buf = NULL;
            size_t len = 0;

            SDL_snprintf(pathBuf, sizeof(pathBuf), "%s.svg", e->name);
            if (skinSourceRead(skin, pathBuf, &buf, &len)) {
                if (decodeSVG((char *)buf, len, w, h, tmpBuf, rast)) {
                    blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                    loaded = true;
                    skinSvgCount++;
                }
                SDL_free(buf);
            }

            if (!loaded) {
                SDL_snprintf(pathBuf, sizeof(pathBuf), "%s.png", e->name);
                if (skinSourceRead(skin, pathBuf, &buf, &len)) {
                    if (decodePNG(buf, len, w, h, tmpBuf)) {
                        blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                        loaded = true;
                        skinPngCount++;
                    }
                    SDL_free(buf);
                }
            }

            if (!loaded && blitSheetSprite(sheet, skinSheet, e, scale)) {
                loaded = true;
                skinSheetCount++;
            }
        }

        /* Try SVG first — rasterized at scaled size. */
        SDL_snprintf(pathBuf, sizeof(pathBuf), "%sdata/svg/%s.svg", basePath, e->name);
        if (!loaded && tryLoadSVG(pathBuf, w, h, tmpBuf, rast)) {
            blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
            loaded = true;
            svgCount++;
        }

        /* Try PNG next — scaled to target size. */
        if (!loaded) {
            SDL_snprintf(pathBuf, sizeof(pathBuf), "%sdata/svg/%s.png", basePath, e->name);
            if (tryLoadPNG(pathBuf, w, h, tmpBuf)) {
                blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                loaded = true;
                pngCount++;
            }
        }

        /* Fall back to BMP — blit at 1x then scale up if needed. */
        if (!loaded && blitSheetSprite(sheet, bmp, e, scale)) {
            bmpCount++;
        }
    }

    WB_LOG_INFO(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: scale=%d, sheet=%dx%d, loaded %d SVG, %d PNG, %d BMP fallback sprites; skin=%s: %d SVG, %d PNG, %d sheet sprites",
            scale, sheetW, sheetH, svgCount, pngCount, bmpCount,
            skinLabel, skinSvgCount, skinPngCount, skinSheetCount);

    SDL_free(tmpBuf);
    if (rast) nsvgDeleteRasterizer(rast);
    if (bmp) SDL_DestroySurface(bmp);
    if (skinSheet) SDL_DestroySurface(skinSheet);

    return sheet;
}

SDL_Surface *tileLoaderBuildSheet(int tileSize) {
    return tileLoaderBuildSheetFor(skinGetActiveSource(), tileSize);
}

void tileLoaderCleanup(void) {
    /* Drop the density scan: once the source is freed its address can be
     * handed to a later skin, and the cache is keyed on that pointer.
     * Reserved for future caching (e.g. keeping parsed SVGs for re-rasterization). */
    s_densitySkin  = NULL;
    s_densityValid = false;
}
