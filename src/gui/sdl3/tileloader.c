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

#include "../../common/wb_log.h"

#include "nanosvg.h"
#include "nanosvgrast.h"

#include "stb_image.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Nearest-neighbor upscale a srcW x srcH RGBA buffer into a dstW x dstH
 * region of the sheet at (dstX, dstY).  Used by Classic mode at scale>1
 * to preserve the chunky-pixel look of the 1x source instead of letting
 * the SVG rasterizer make crisp lines at the larger size. */
static void blitRGBAScaled(SDL_Surface *sheet, int dstX, int dstY,
                           int dstW, int dstH,
                           int srcW, int srcH,
                           const unsigned char *pixels) {
    if (!pixels || srcW <= 0 || srcH <= 0) return;
    unsigned char *dst = (unsigned char *)sheet->pixels;
    int pitch = sheet->pitch;
    for (int row = 0; row < dstH; row++) {
        int sy = row * srcH / dstH;
        if (dstY + row < 0 || dstY + row >= sheet->h) continue;
        const unsigned char *srcRow = pixels + sy * srcW * 4;
        unsigned char *dstRow = dst + (dstY + row) * pitch + dstX * 4;
        for (int col = 0; col < dstW; col++) {
            int sx = col * srcW / dstW;
            if (dstX + col < 0 || dstX + col >= sheet->w) continue;
            const unsigned char *s = srcRow + sx * 4;
            dstRow[col * 4 + 0] = s[0];
            dstRow[col * 4 + 1] = s[1];
            dstRow[col * 4 + 2] = s[2];
            dstRow[col * 4 + 3] = s[3];
        }
    }
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

/* Backwards-compat wrapper.  Point-sample for the Pixelate modes,
 * standard AA for Max Detail (regardless of skin — Max Detail
 * means Max Detail). */
static bool tryLoadSVG(const char *path, int w, int h,
                       unsigned char *out, NSVGrasterizer *rast) {
    bool pointSample = (gfxSettingsGetTileDetail() != GFX_TILE_DETAIL_HIGH_DETAIL);
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

/* Skin override.  When non-empty, tileLoaderBuildSheet looks for
 * sprites in data/skins/<skinName>/<name>.svg|png BEFORE falling
 * back to data/svg/<name>.svg|png and finally the BMP. */
static char s_skinName[64] = "";

/* Per-direction "is this hand-crafted in the skin" cache, see
 * tileLoaderSkinHasSprite below.  Cleared on skin change. */
typedef struct {
    char     base[24];
    Uint16   mask;       /* bit N set ⇒ has _NN file in current skin */
} SkinMaskEntry;
static SkinMaskEntry s_skinMasks[8];
static int            s_skinMaskCount = 0;

/* Skin metadata, populated from data/skins/<active>/skin.ini on
 * tileLoaderSetSkin.  Defaults to "no INI / classic only" when
 * absent. */
static TileLoaderSkinInfo s_skinInfo = { .max_pixel_density = 1 };

#ifdef _WIN32
#include <windows.h>
#define GetPrivateProfileString GetPrivateProfileStringA
#else
extern DWORD GetPrivateProfileString(const char *section, const char *key,
                                      const char *def, char *dest,
                                      DWORD size, const char *file);
#endif

static void resetSkinInfo(void) {
    s_skinInfo.name[0]         = '\0';
    s_skinInfo.author[0]       = '\0';
    s_skinInfo.email[0]        = '\0';
    s_skinInfo.website[0]      = '\0';
    s_skinInfo.release_date[0] = '\0';
    s_skinInfo.max_pixel_density = 1;
    s_skinInfo.has_ini         = false;
}

/* Fill *out from data/skins/<skinName>/skin.ini.  Pure read — does
 * not touch any global state.  out is reset to defaults first; on
 * a missing INI file out->has_ini stays false. */
static void readSkinInfo(const char *skinName, TileLoaderSkinInfo *out) {
    out->name[0]         = '\0';
    out->author[0]       = '\0';
    out->email[0]        = '\0';
    out->website[0]      = '\0';
    out->release_date[0] = '\0';
    out->max_pixel_density = 1;
    out->has_ini         = false;
    if (!skinName || !skinName[0]) return;
    char iniPath[512];
    SDL_snprintf(iniPath, sizeof(iniPath), "data/skins/%s/skin.ini", skinName);
    SDL_PathInfo pinfo;
    if (!SDL_GetPathInfo(iniPath, &pinfo)) return;
    out->has_ini = true;
    char buf[16];
    GetPrivateProfileString("skin", "name",         "", out->name,         sizeof(out->name),         iniPath);
    GetPrivateProfileString("skin", "author",       "", out->author,       sizeof(out->author),       iniPath);
    GetPrivateProfileString("skin", "email",        "", out->email,        sizeof(out->email),        iniPath);
    GetPrivateProfileString("skin", "website",      "", out->website,      sizeof(out->website),      iniPath);
    GetPrivateProfileString("skin", "release_date", "", out->release_date, sizeof(out->release_date), iniPath);
    GetPrivateProfileString("skin", "max_pixel_density", "1", buf, sizeof(buf), iniPath);
    int v = atoi(buf);
    if (v < 1) v = 1;
    if (v > 16) v = 16;
    out->max_pixel_density = v;
}

void tileLoaderQuerySkinInfo(const char *skinName, TileLoaderSkinInfo *out) {
    if (!out) return;
    readSkinInfo(skinName, out);
}

static void loadSkinInfo(void) {
    readSkinInfo(s_skinName, &s_skinInfo);
    if (!s_skinInfo.has_ini) return;
    SDL_Log("tileLoader: skin '%s' loaded (max_pixel_density=%d)",
            s_skinName, s_skinInfo.max_pixel_density);
}

const TileLoaderSkinInfo *tileLoaderGetSkinInfo(void) {
    return &s_skinInfo;
}

/* Coverage scan: for each density 1..max we flag whether the active
 * skin has all/some/none of the tiles at that density.  Per-sprite
 * max density is also recorded so High Detail can pick per sprite.
 * Density 1 is always FULL because every TileMap sprite has at
 * least a 1× default in data/svg/.  SVG counts as covering up to
 * skinInfo.max_pixel_density. */
#define TLR_MAX_DENSITY 16
static unsigned char s_densityCov[TLR_MAX_DENSITY + 1];   /* 0=none,1=some,2=all */

/* Per-sprite max density.  We don't know gTileMap size at compile
 * time so use a small dynamic array; skin reload tears it down. */
typedef struct { char name[64]; int maxDensity; } SpriteDensity;
static SpriteDensity *s_spriteDensities = NULL;
static int            s_spriteDensitiesCount = 0;

/* True when data/skins/<active>/<density>-<sprite>.png exists, OR
 * (when density <= skinInfo.max_pixel_density) the skin provides
 * the sprite as an SVG that the engine treats as covering this
 * density. */
static bool spriteAtDensityFor(const char *skinName, int skinMaxDensity,
                               const char *spriteName, int density) {
    if (!skinName || !skinName[0]) return false;
    char path[512];
    SDL_PathInfo pi;
    int sizePx = density * TILE_SIZE_X;   /* density 2 => 32 */
    /* Suffix form: <name>_<size>.png (Inkscape batch-export friendly). */
    SDL_snprintf(path, sizeof(path), "data/skins/%s/%s_%d.png",
                 skinName, spriteName, sizePx);
    if (SDL_GetPathInfo(path, &pi)) return true;
    /* Legacy prefix form: <size>-<name>.png. */
    SDL_snprintf(path, sizeof(path), "data/skins/%s/%d-%s.png",
                 skinName, sizePx, spriteName);
    if (SDL_GetPathInfo(path, &pi)) return true;
    if (density == 1) {
        /* Density 1 also accepts the unprefixed PNG / SVG in the
         * skin dir (legacy 1× sprites). */
        SDL_snprintf(path, sizeof(path), "data/skins/%s/%s.png",
                     skinName, spriteName);
        if (SDL_GetPathInfo(path, &pi)) return true;
        SDL_snprintf(path, sizeof(path), "data/skins/%s/%s.svg",
                     skinName, spriteName);
        if (SDL_GetPathInfo(path, &pi)) return true;
    }
    /* SVG counts up to declared max_pixel_density. */
    if (density <= skinMaxDensity) {
        SDL_snprintf(path, sizeof(path), "data/skins/%s/%s.svg",
                     skinName, spriteName);
        if (SDL_GetPathInfo(path, &pi)) return true;
    }
    return false;
}

static bool spriteAtDensity(const char *spriteName, int density) {
    return spriteAtDensityFor(s_skinName, s_skinInfo.max_pixel_density,
                              spriteName, density);
}

static void scanDensityCoverage(void) {
    SDL_memset(s_densityCov, 0, sizeof(s_densityCov));
    if (s_spriteDensities) { SDL_free(s_spriteDensities); s_spriteDensities = NULL; }
    s_spriteDensitiesCount = 0;

    /* Density 1 always full (default skin always supplies 1×). */
    s_densityCov[1] = 2;
    int maxD = s_skinInfo.max_pixel_density;
    if (maxD < 1) maxD = 1;
    if (maxD > TLR_MAX_DENSITY) maxD = TLR_MAX_DENSITY;
    if (!s_skinName[0]) {
        /* No skin override: density 1 is fully covered, nothing else.
         * Still want per-sprite max=1 for the High Detail path. */
        return;
    }

    /* Count sprites in gTileMap. */
    int spriteCount = 0;
    while (gTileMap[spriteCount].name != NULL) spriteCount++;
    s_spriteDensities = (SpriteDensity *)SDL_malloc((size_t)spriteCount * sizeof(SpriteDensity));
    if (!s_spriteDensities) return;
    s_spriteDensitiesCount = spriteCount;

    /* Per-density: hasAll/hasAny across the entire gTileMap. */
    for (int d = 2; d <= maxD; d++) {
        bool any = false;
        bool all = true;
        for (int i = 0; i < spriteCount; i++) {
            if (spriteAtDensity(gTileMap[i].name, d)) {
                any = true;
            } else {
                all = false;
            }
            if (any && !all) break;
        }
        s_densityCov[d] = all ? 2 : (any ? 1 : 0);
    }

    /* Per-sprite max density: highest d where the file exists. */
    for (int i = 0; i < spriteCount; i++) {
        SDL_strlcpy(s_spriteDensities[i].name, gTileMap[i].name,
                    sizeof(s_spriteDensities[i].name));
        s_spriteDensities[i].maxDensity = 1;
        for (int d = maxD; d >= 1; d--) {
            if (spriteAtDensity(gTileMap[i].name, d)) {
                s_spriteDensities[i].maxDensity = d;
                break;
            }
        }
    }

    SDL_Log("tileLoader: density coverage for skin '%s': "
            "1=all, 2=%s, 3=%s, 4=%s",
            s_skinName,
            s_densityCov[2] == 2 ? "all" : (s_densityCov[2] == 1 ? "some" : "none"),
            s_densityCov[3] == 2 ? "all" : (s_densityCov[3] == 1 ? "some" : "none"),
            s_densityCov[4] == 2 ? "all" : (s_densityCov[4] == 1 ? "some" : "none"));
}

int tileLoaderGetDensityCoverage(int density) {
    if (density < 1 || density > TLR_MAX_DENSITY) return 0;
    return (int)s_densityCov[density];
}

int tileLoaderGetAllTilesDensityAtMost(int cap) {
    if (cap < 1) return 1;
    if (cap > TLR_MAX_DENSITY) cap = TLR_MAX_DENSITY;
    for (int d = cap; d >= 1; d--) {
        if (s_densityCov[d] == 2) return d;
    }
    return 1;
}

int tileLoaderGetSpriteMaxDensity(const char *spriteName) {
    if (!spriteName) return 1;
    for (int i = 0; i < s_spriteDensitiesCount; i++) {
        if (SDL_strcmp(s_spriteDensities[i].name, spriteName) == 0) {
            return s_spriteDensities[i].maxDensity;
        }
    }
    return 1;
}

int tileLoaderGetMissingSprites(int density, char *outBuf, int outBufSize) {
    return tileLoaderQueryMissingSprites(s_skinName, density, outBuf, outBufSize);
}

/* HUD/chrome sprite names that don't count toward "full tile coverage" —
 * a skin that ships only world tiles is still considered fully covering
 * the world. */
static bool isNonWorldSprite(const char *name) {
    static const char *kSkipPrefixes[] = {
        "indent_",     /* HUD builder-status slot backgrounds and dots */
        "status_",     /* HUD status icons (base/pill/dead) */
    };
    for (size_t i = 0; i < sizeof(kSkipPrefixes) / sizeof(kSkipPrefixes[0]); i++) {
        size_t plen = SDL_strlen(kSkipPrefixes[i]);
        if (SDL_strncmp(name, kSkipPrefixes[i], plen) == 0) return true;
    }
    static const char *kSkipExact[] = {
        "gunsight", "mouse_square", "tank_icon", "tank_transparent",
        "static",
    };
    for (size_t i = 0; i < sizeof(kSkipExact) / sizeof(kSkipExact[0]); i++) {
        if (SDL_strcmp(name, kSkipExact[i]) == 0) return true;
    }
    return false;
}

int tileLoaderQueryMissingSprites(const char *skinName, int density,
                                   char *outBuf, int outBufSize) {
    if (outBuf && outBufSize > 0) outBuf[0] = '\0';
    if (!skinName || !skinName[0]) return 0;
    if (density < 1) density = 1;
    /* Need the skin's declared max so SVG-as-coverage logic matches. */
    TileLoaderSkinInfo info;
    readSkinInfo(skinName, &info);
    int count = 0;
    int writePos = 0;
    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const char *name = gTileMap[i].name;
        if (isNonWorldSprite(name)) continue;
        if (spriteAtDensityFor(skinName, info.max_pixel_density, name, density)) continue;
        count++;
        if (outBuf && outBufSize > 0) {
            int remaining = outBufSize - writePos;
            int n = SDL_snprintf(outBuf + writePos, remaining,
                                 (writePos == 0) ? "%s" : "\n%s", name);
            if (n > 0 && n < remaining) writePos += n;
        }
    }
    return count;
}

void tileLoaderSetSkin(const char *name) {
    if (!name || !name[0]) {
        s_skinName[0] = '\0';
    } else {
        SDL_strlcpy(s_skinName, name, sizeof(s_skinName));
    }
    /* Skin changed → drop the per-direction-presence cache and
     * reload skin metadata + density coverage. */
    s_skinMaskCount = 0;
    loadSkinInfo();
    scanDensityCoverage();
}

const char *tileLoaderGetSkin(void) {
    return s_skinName;
}

static Uint16 computeSkinMask(const char *base) {
    Uint16 mask = 0;
    if (!s_skinName[0] || !base || !base[0]) return 0;
    static const int kPrefixes[] = { 16, 24, 32, 48, 64, 96, 128 };
    const int kNumPref = (int)(sizeof(kPrefixes) / sizeof(kPrefixes[0]));
    char path[512];
    SDL_PathInfo info;
    for (int dir = 0; dir < 16; dir++) {
        SDL_snprintf(path, sizeof(path), "data/skins/%s/%s_%02d.svg",
                     s_skinName, base, dir);
        if (SDL_GetPathInfo(path, &info)) { mask |= (Uint16)(1 << dir); continue; }
        SDL_snprintf(path, sizeof(path), "data/skins/%s/%s_%02d.png",
                     s_skinName, base, dir);
        if (SDL_GetPathInfo(path, &info)) { mask |= (Uint16)(1 << dir); continue; }
        bool found = false;
        for (int i = 0; i < kNumPref && !found; i++) {
            /* Suffix form first, then legacy prefix. */
            SDL_snprintf(path, sizeof(path), "data/skins/%s/%s_%02d_%d.png",
                         s_skinName, base, dir, kPrefixes[i]);
            if (SDL_GetPathInfo(path, &info)) { found = true; break; }
            SDL_snprintf(path, sizeof(path), "data/skins/%s/%d-%s_%02d.png",
                         s_skinName, kPrefixes[i], base, dir);
            if (SDL_GetPathInfo(path, &info)) { found = true; break; }
        }
        if (found) mask |= (Uint16)(1 << dir);
    }
    return mask;
}

bool tileLoaderSkinHasSprite(const char *baseName, int dir) {
    if (!s_skinName[0]) return false;
    if (!baseName || !baseName[0]) return false;
    if (dir < 0 || dir > 15) return false;
    for (int i = 0; i < s_skinMaskCount; i++) {
        if (SDL_strcmp(s_skinMasks[i].base, baseName) == 0) {
            return ((s_skinMasks[i].mask >> dir) & 1) != 0;
        }
    }
    int slot = s_skinMaskCount;
    if (slot >= (int)(sizeof(s_skinMasks) / sizeof(s_skinMasks[0]))) return false;
    SDL_strlcpy(s_skinMasks[slot].base, baseName,
                sizeof(s_skinMasks[slot].base));
    s_skinMasks[slot].mask = computeSkinMask(baseName);
    s_skinMaskCount++;
    return ((s_skinMasks[slot].mask >> dir) & 1) != 0;
}

bool tileLoaderSkinRotates(void) {
    if (!s_skinName[0]) return false;
    /* Convention: any skin dir ending with "_ingamerotate" only
     * ships the north-facing (_00) variant of rotation groups. */
    size_t n = SDL_strlen(s_skinName);
    const char *suffix = "_ingamerotate";
    size_t sn = SDL_strlen(suffix);
    if (n < sn) return false;
    return SDL_strcmp(s_skinName + (n - sn), suffix) == 0;
}

/* For Pixelate-to-Zoom: pick the best size-tagged PNG in <dir> for
 * the given targetSize.  Accepts both the suffix form
 * <name>_<size>.png (Inkscape batch-export) and the legacy prefix
 * form <size>-<name>.png.  Priority:
 *   1. Exact match (target).
 *   2. Larger sizes in increasing order (downscale — preserves
 *      detail).
 *   3. Smaller sizes in decreasing order (upscale — last resort).
 *   4. Failure → caller falls back to SVG.
 * tryLoadPNG nearest-neighbor scales the source to (w x h). */
static bool tryLoadSizedPNGAt(const char *dir, const char *name, int sz,
                              int w, int h, unsigned char *out) {
    char path[512];
    SDL_snprintf(path, sizeof(path), "%s/%s_%d.png", dir, name, sz);
    if (tryLoadPNG(path, w, h, out)) return true;
    SDL_snprintf(path, sizeof(path), "%s/%d-%s.png", dir, sz, name);
    return tryLoadPNG(path, w, h, out);
}

static bool tryLoadSizedPNG(const char *dir, const char *name,
                            int targetSize, int w, int h,
                            unsigned char *out) {
    static const int kSizes[] = { 24, 32, 48, 64, 96, 128, 160 };
    const int kCount = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
    /* 1. Exact match. */
    for (int i = 0; i < kCount; i++) {
        if (kSizes[i] != targetSize) continue;
        if (tryLoadSizedPNGAt(dir, name, kSizes[i], w, h, out)) return true;
    }
    /* 2. Larger (downscale). */
    for (int i = 0; i < kCount; i++) {
        if (kSizes[i] <= targetSize) continue;
        if (tryLoadSizedPNGAt(dir, name, kSizes[i], w, h, out)) return true;
    }
    /* 3. Smaller (upscale). */
    for (int i = kCount - 1; i >= 0; i--) {
        if (kSizes[i] >= targetSize) continue;
        if (tryLoadSizedPNGAt(dir, name, kSizes[i], w, h, out)) return true;
    }
    return false;
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
        WB_LOG_ERROR(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: SDL_CreateSurface(%dx%d) failed: %s",
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
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: could not load data/skin.bmp fallback");
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();

    /* Temp buffer: largest sprite is indent tiles (54x54) scaled up. */
    int maxSpriteSize = 54 * scale;
    unsigned char *tmpBuf = (unsigned char *)SDL_malloc(
        (size_t)(maxSpriteSize * maxSpriteSize * 4));

    char pathBuf[512];
    int svgCount = 0, pngCount = 0, bmpCount = 0;
    GfxTileDetail detail = gfxSettingsGetTileDetail();

    /* For Match-to-Zoom: pick a single density covering all sprites. */
    int matchToZoomDensity = tileLoaderGetAllTilesDensityAtMost(scale);

    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const TileMapEntry *e = &gTileMap[i];
        int w = e->width  * scale;
        int h = e->height * scale;
        int dstX = e->sheetX * scale;
        int dstY = e->sheetY * scale;
        bool loaded = false;

        /* Determine target density per the Tile Detail mode. */
        int targetDensity = 1;
        switch (detail) {
        case GFX_TILE_DETAIL_CLASSIC:
            targetDensity = 1;
            break;
        case GFX_TILE_DETAIL_MATCH_TO_ZOOM:
            targetDensity = matchToZoomDensity;
            break;
        case GFX_TILE_DETAIL_HIGH_DETAIL:
            targetDensity = tileLoaderGetSpriteMaxDensity(e->name);
            /* Atlas slot is `scale` game-pixels wide — anything above
             * that just gets nearest-downscaled to fit, so cap. */
            if (targetDensity > scale) targetDensity = scale;
            break;
        }

        /* Try N-<name>.png prefix at the chosen density, in the
         * skin dir first then the default.  tryLoadSizedPNG
         * already handles exact / smaller / larger fallback. */
        if (targetDensity > 1) {
            int targetPx = targetDensity * TILE_SIZE_X * (e->width / TILE_SIZE_X);
            (void)targetPx;
            char dir[256];
            if (s_skinName[0]) {
                SDL_snprintf(dir, sizeof(dir), "data/skins/%s", s_skinName);
                if (tryLoadSizedPNG(dir, e->name, w, w, h, tmpBuf)) {
                    blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                    loaded = true;
                    pngCount++;
                }
            }
            if (!loaded) {
                if (tryLoadSizedPNG("data/svg", e->name, w, w, h, tmpBuf)) {
                    blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                    loaded = true;
                    pngCount++;
                }
            }
        }

        /* Classic mode at zoom > 1: rasterize/load at 1x source size
         * and nearest-upscale into the atlas slot, preserving the
         * chunky pixelated look of the legacy 16px tiles. */
        bool classicUpscale = (detail == GFX_TILE_DETAIL_CLASSIC) && scale > 1;
        int srcW = classicUpscale ? e->width  : w;
        int srcH = classicUpscale ? e->height : h;

        /* Skin override: try data/skins/<skin>/<name>.svg|png. */
        if (!loaded && s_skinName[0]) {
            SDL_snprintf(pathBuf, sizeof(pathBuf),
                         "data/skins/%s/%s.svg", s_skinName, e->name);
            if (tryLoadSVG(pathBuf, srcW, srcH, tmpBuf, rast)) {
                blitRGBAScaled(sheet, dstX, dstY, w, h, srcW, srcH, tmpBuf);
                loaded = true;
                svgCount++;
            }
            if (!loaded) {
                SDL_snprintf(pathBuf, sizeof(pathBuf),
                             "data/skins/%s/%s.png", s_skinName, e->name);
                if (tryLoadPNG(pathBuf, srcW, srcH, tmpBuf)) {
                    blitRGBAScaled(sheet, dstX, dstY, w, h, srcW, srcH, tmpBuf);
                    loaded = true;
                    pngCount++;
                }
            }
            /* Ingamerotate skins only ship _00 of rotation groups —
             * runtime code rotates _00 at draw time. */
        }

        /* Try SVG first — rasterized at source size, then upscaled. */
        SDL_snprintf(pathBuf, sizeof(pathBuf), "data/svg/%s.svg", e->name);
        if (!loaded && tryLoadSVG(pathBuf, srcW, srcH, tmpBuf, rast)) {
            blitRGBAScaled(sheet, dstX, dstY, w, h, srcW, srcH, tmpBuf);
            loaded = true;
            svgCount++;
        }

        /* Try PNG next — scaled to source size, then upscaled. */
        if (!loaded) {
            SDL_snprintf(pathBuf, sizeof(pathBuf), "data/svg/%s.png", e->name);
            if (tryLoadPNG(pathBuf, srcW, srcH, tmpBuf)) {
                blitRGBAScaled(sheet, dstX, dstY, w, h, srcW, srcH, tmpBuf);
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

    WB_LOG_INFO(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: scale=%d, sheet=%dx%d, loaded %d SVG, %d PNG, %d BMP fallback sprites",
            scale, sheetW, sheetH, svgCount, pngCount, bmpCount);

    SDL_free(tmpBuf);
    if (rast) nsvgDeleteRasterizer(rast);
    if (bmp) SDL_DestroySurface(bmp);

    return sheet;
}

void tileLoaderCleanup(void) {
    /* No persistent state today.  Reserved for a future per-tile
     * SVG cache when full-vector tiles land. */
}
