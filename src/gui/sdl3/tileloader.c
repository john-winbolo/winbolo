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
/* Only tileLoaderBuildSheet reads this, to turn the player's Tile Detail
   setting into the TILE_DETAIL_* value the rest of the file works in. */
#include "gfx_settings.h"
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

/* One little-endian signed 32-bit field of a BMP header, assembled a byte at
 * a time so neither the buffer's alignment nor the host's byte order
 * matters. */
static Sint32 readLE32(const unsigned char *p, size_t off) {
    Uint32 v = (Uint32)p[off]             | ((Uint32)p[off + 1] << 8) |
               ((Uint32)p[off + 2] << 16) | ((Uint32)p[off + 3] << 24);
    return (Sint32)v;
}

int tileLoaderSheetDensityFromBmp(const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    Sint32 w, h;
    long long absH;
    int d;

    /* 26 bytes covers the 14-byte file header plus the width and height of
     * the smallest DIB header that carries them as 32-bit fields. */
    if (p == NULL || len < 26) return 0;
    if (p[0] != 'B' || p[1] != 'M') return 0;

    w = readLE32(p, 18);
    h = readLE32(p, 22);
    /* A negative height is a top-down BMP; the row order says nothing about
     * how big the sheet is. */
    absH = h < 0 ? -(long long)h : (long long)h;

    if (w < TILE_FILE_X || (w % TILE_FILE_X) != 0) return 0;
    d = (int)(w / TILE_FILE_X);
    if (d < 1 || d > SKIN_DENSITY_MAX) return 0;
    if (absH != (long long)TILE_FILE_Y * d) return 0;
    return d;
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

    /* A whole sheet drawn at N times the layout carries every sprite at N,
       so it counts as full coverage up to N before a single @Nx file is
       looked at.  Only the BMP header says how big the sheet is, so only
       the header is read: 26 bytes, whether the sheet is a file or a zip
       entry that would otherwise be inflated whole.  The per-sprite loop
       below stays pure index lookups. */
    static const char *sheetNames[] = { "tiles.bmp", "skin.bmp", "skin32.bmp" };
    int sheetDensity = 0;
    for (int i = 0; i < (int)(sizeof(sheetNames) / sizeof(sheetNames[0])); i++) {
        unsigned char head[26];
        size_t got = 0;
        if (!skinSourceReadHead(skin, sheetNames[i], head, sizeof(head), &got)) {
            continue;
        }
        sheetDensity = tileLoaderSheetDensityFromBmp(head, got);
        /* Bytes that are not a usable sheet do not settle the question: the
           build's own loader moves on to the next name as well, so stopping
           here would leave the scan naming a sheet the build never uses. */
        if (sheetDensity > 0) break;
    }
    if (sheetDensity >= 2) {
        for (int i = 0; i < out->spriteCount; i++) {
            out->spriteMax[i] = (unsigned char)sheetDensity;
        }
        out->highestAny = sheetDensity;
    }

    int worldTotal = 0;
    int worldHits[SKIN_DENSITY_MAX + 1];
    SDL_memset(worldHits, 0, sizeof(worldHits));

    /* Name-index lookups only.  skinSourceExists is O(1) for a directory and
       for an archive alike, so this loop costs no file I/O. */
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
        if (n <= sheetDensity) {
            /* The sheet covers this density for every sprite on its own. */
            out->coverage[n] = SKIN_DENSITY_COVER_ALL;
            out->highestAll  = n;
        } else if (worldTotal > 0 && worldHits[n] == worldTotal) {
            out->coverage[n] = SKIN_DENSITY_COVER_ALL;
            out->highestAll  = n;
        } else if (worldHits[n] > 0) {
            out->coverage[n] = SKIN_DENSITY_COVER_SOME;
        }
    }
}

/* One-entry scan cache, keyed on the source's serial rather than its
   address.  skinSetActive closes one source and opens the next, and the
   allocator is free to give the new one the old one's block; a pointer key
   would then hand the new skin the old skin's scan.  Serials are never
   reused.  The valid flag is what makes a NULL skin (serial 0) a real key
   rather than "not scanned yet". */
static uint64_t        s_densitySerial = 0;
static SkinDensityInfo s_densityInfo;
static bool            s_densityValid  = false;

const SkinDensityInfo *tileLoaderGetDensityInfo(struct SkinSource *skin) {
    uint64_t serial = skinSourceSerial(skin);
    if (!s_densityValid || s_densitySerial != serial) {
        tileLoaderScanDensity(skin, &s_densityInfo);
        s_densitySerial = serial;
        s_densityValid  = true;
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

/* Copy one slot's RGBA pixels back out of the sheet surface.  The sheet is
 * plain RGBA32 memory built by this file, so the slot already holds the
 * final scaled sprite and nothing needs reloading. */
static void readSheetRGBA(const SDL_Surface *sheet, int srcX, int srcY,
                          int w, int h, unsigned char *pixels) {
    if (!pixels) return;
    const unsigned char *src = (const unsigned char *)sheet->pixels;
    int pitch = sheet->pitch;
    SDL_memset(pixels, 0, (size_t)(w * h * 4));
    for (int row = 0; row < h; row++) {
        if (srcY + row < 0 || srcY + row >= sheet->h) continue;
        const unsigned char *srcRow = src + (srcY + row) * pitch + srcX * 4;
        unsigned char *dstRow = pixels + row * w * 4;
        for (int col = 0; col < w; col++) {
            if (srcX + col < 0 || srcX + col >= sheet->w) continue;
            dstRow[col * 4 + 0] = srcRow[col * 4 + 0];
            dstRow[col * 4 + 1] = srcRow[col * 4 + 1];
            dstRow[col * 4 + 2] = srcRow[col * 4 + 2];
            dstRow[col * 4 + 3] = srcRow[col * 4 + 3];
        }
    }
}

/* Rotate a square RGBA sprite about its centre.  Every destination pixel's
 * centre is turned back by the angle to find where it came from, and that
 * point is read from the four texels around it.  A source point outside the
 * sprite leaves the destination pixel fully transparent.
 *
 * The four texels are weighted by alpha as well as by distance: the colour
 * is sum(weight * alpha * rgb) divided by sum(weight * alpha), so a fully
 * transparent texel contributes no colour at all.  Plain bilinear would drag
 * the colour of transparent texels into the sprite's edges, and the sheet's
 * colour-keyed pixels keep their green after the conversion to RGBA — that
 * produced visible green fringing here once already.  Do not simplify this
 * back to a straight four-way average. */
static void rotateRGBA(const unsigned char *src, unsigned char *dst,
                       int size, float degrees) {
    const float rad = degrees * 3.14159265358979323846f / 180.0f;
    const float cs = SDL_cosf(rad);
    const float sn = SDL_sinf(rad);
    const float centre = (float)size * 0.5f;

    SDL_memset(dst, 0, (size_t)(size * size * 4));

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            /* This pixel's centre relative to the sprite centre, turned back
               by the angle.  y grows downward, so a positive angle here is a
               clockwise turn on screen. */
            float dx = (float)x + 0.5f - centre;
            float dy = (float)y + 0.5f - centre;
            float sx = ( cs * dx + sn * dy) + centre - 0.5f;
            float sy = (-sn * dx + cs * dy) + centre - 0.5f;

            /* No texel can reach this far out, so the pixel stays clear. */
            if (sx <= -1.0f || sx >= (float)size ||
                sy <= -1.0f || sy >= (float)size) {
                continue;
            }

            int x0 = (int)SDL_floorf(sx);
            int y0 = (int)SDL_floorf(sy);
            float fx = sx - (float)x0;
            float fy = sy - (float)y0;

            float aSum = 0.0f, rSum = 0.0f, gSum = 0.0f, bSum = 0.0f;
            for (int j = 0; j < 2; j++) {
                int ty = y0 + j;
                float wy = j ? fy : 1.0f - fy;
                for (int i = 0; i < 2; i++) {
                    int tx = x0 + i;
                    float wx = i ? fx : 1.0f - fx;
                    /* Off the edge reads as fully transparent, which adds
                       neither colour nor alpha. */
                    if (tx < 0 || tx >= size || ty < 0 || ty >= size) continue;
                    const unsigned char *sp = src + (ty * size + tx) * 4;
                    float w = wx * wy;
                    float wa = w * (float)sp[3];
                    aSum += wa;
                    rSum += wa * (float)sp[0];
                    gSum += wa * (float)sp[1];
                    bSum += wa * (float)sp[2];
                }
            }
            if (aSum <= 0.0f) continue;

            unsigned char *dp = dst + (y * size + x) * 4;
            dp[0] = (unsigned char)(rSum / aSum + 0.5f);
            dp[1] = (unsigned char)(gSum / aSum + 0.5f);
            dp[2] = (unsigned char)(bSum / aSum + 0.5f);
            /* The four weights sum to one, so aSum is already the blended
               alpha. */
            dp[3] = (unsigned char)(aSum + 0.5f);
        }
    }
}

/* Index of a sprite in gTileMap[] by name, -1 when there is none.  The
 * sixteen frames of one group are neither adjacent nor in order in the
 * table, so every frame is found by name rather than by offset. */
static int tileMapIndexOf(const char *name) {
    for (int i = 0; gTileMap[i].name != NULL; i++) {
        if (strcmp(gTileMap[i].name, name) == 0) return i;
    }
    return -1;
}

/* The sprite groups whose sixteen frames are facings, listed rather than
 * inferred from "has sixteen frames": pillbox_good_00..15 and
 * pillbox_evil_00..15 run to sixteen too, but those are armour levels and
 * turning them would be wrong.
 *
 * Shells are left out on purpose.  Their sprites change size by direction —
 * SHELL_0 is 3x4, SHELL_4 is 4x3, SHELL_2 is 4x4 in src/gui/tiles.h — so a
 * rotated copy would not fit the slot it lands in.  They are turned at draw
 * time instead. */
static const char *const kRotationGroups[] = {
    "tank_self", "tank_good", "tank_evil",
    "tank_selfboat", "tank_goodboat", "tank_evilboat"
};

#define ROTATION_GROUP_FRAMES 16

#if WB_SKIN_DRAWTIME_ROTATION
/* Which sheet slots the fill pass at the end of tileLoaderBuildSheetFor
   turned out of a group's north sprite, and the slot that sprite sits in.
   1x coordinates, the ones gTileMap[] and tiles.h carry, so a draw path
   working in tiles.h constants can match against them as they are.
   A group's north slot is listed against itself: its art is what every other
   slot in the group was turned from, so a caller holding an angle finer than
   the sixteen frames can turn it too.  Six groups of sixteen slots is the
   most there can be. */
static struct {
    int slotX, slotY;
    int baseX, baseY;
} s_rotatedSlots[(sizeof(kRotationGroups) / sizeof(kRotationGroups[0])) *
                 ROTATION_GROUP_FRAMES];
static int s_rotatedSlotCount = 0;

/* Record that the slot `m` holds a turned copy of the sprite in slot `base`.
   The array is sized from the same two constants the fill pass loops over,
   so the bound below cannot be reached; it is there so a later group added
   to kRotationGroups without resizing overruns nothing. */
static void noteRotatedSlot(const TileMapEntry *m, const TileMapEntry *base) {
    int max = (int)(sizeof(s_rotatedSlots) / sizeof(s_rotatedSlots[0]));
    if (s_rotatedSlotCount >= max) return;
    s_rotatedSlots[s_rotatedSlotCount].slotX = m->sheetX;
    s_rotatedSlots[s_rotatedSlotCount].slotY = m->sheetY;
    s_rotatedSlots[s_rotatedSlotCount].baseX = base->sheetX;
    s_rotatedSlots[s_rotatedSlotCount].baseY = base->sheetY;
    s_rotatedSlotCount++;
}

bool tileLoaderRotatedSource(int srcX, int srcY, int *baseX, int *baseY) {
    for (int i = 0; i < s_rotatedSlotCount; i++) {
        if (s_rotatedSlots[i].slotX != srcX ||
            s_rotatedSlots[i].slotY != srcY) {
            continue;
        }
        if (baseX) *baseX = s_rotatedSlots[i].baseX;
        if (baseY) *baseY = s_rotatedSlots[i].baseY;
        return true;
    }
    return false;
}
#endif /* WB_SKIN_DRAWTIME_ROTATION */

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

/* Load <name>@<n>x.png out of the skin into `out` at the slot size.
 * decodePNG point-samples whatever it decodes to that size, and that is what
 * this art wants in both directions: the sprites are color-keyed pixel art,
 * so averaging neighbours to fit a finer file into a coarser slot softens
 * every edge and invents part-transparent pixels the art never had.
 * Returns true on success. */
static bool tryLoadSkinDensityPNG(struct SkinSource *skin, const char *name,
                                  int n, int w, int h, unsigned char *out) {
    char rel[SKIN_PATH_MAX];
    void *buf = NULL;
    size_t len = 0;
    bool ok;

    SDL_snprintf(rel, sizeof(rel), "%s@%dx.png", name, n);
    if (!skinSourceRead(skin, rel, &buf, &len)) return false;
    ok = decodePNG(buf, len, w, h, out);
    SDL_free(buf);
    return ok;
}

/* The nearest @Mx the skin holds for one sprite: searching up from `want`
 * when `up` is set, and down towards 2 when it is not.  Name-index lookups
 * only, which is cheaper than reads that fail.  0 when there is none. */
static int skinNearestDensity(struct SkinSource *skin, const char *name,
                              int want, bool up) {
    char rel[SKIN_PATH_MAX];

    if (up) {
        for (int n = want + 1; n <= SKIN_DENSITY_MAX; n++) {
            SDL_snprintf(rel, sizeof(rel), "%s@%dx.png", name, n);
            if (skinSourceExists(skin, rel)) return n;
        }
    } else {
        int n = want - 1;
        if (n > SKIN_DENSITY_MAX) n = SKIN_DENSITY_MAX;
        for (; n >= 2; n--) {
            SDL_snprintf(rel, sizeof(rel), "%s@%dx.png", name, n);
            if (skinSourceExists(skin, rel)) return n;
        }
    }
    return 0;
}

/* The Tile Detail mode's name for the build summary. */
static const char *tileDetailName(int mode) {
    switch (mode) {
        case TILE_DETAIL_MATCH_ZOOM: return "match zoom";
        case TILE_DETAIL_HIGH:       return "high detail";
        default:                     return "classic";
    }
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
 * data/skin.bmp) into the output sheet.  `srcDensity` is the multiple the
 * source sheet is drawn at and `scale` the multiple the output sheet is
 * built at, so the crop is taken at srcDensity and lands at scale.  False
 * when there is no source sheet. */
static bool blitSheetSprite(SDL_Surface *sheet, SDL_Surface *src,
                            const TileMapEntry *e, int scale, int srcDensity) {
    if (!src) return false;

    int D = srcDensity < 1 ? 1 : srcDensity;
    int w = e->width  * scale;
    int h = e->height * scale;
    int dstX = e->sheetX * scale;
    int dstY = e->sheetY * scale;

    if (D == scale) {
        /* Same multiple both sides: a straight copy of the sprite's rect. */
        blitFromBMP(sheet, src, dstX, dstY,
                    e->sheetX * D, e->sheetY * D, e->width * D, e->height * D);
    } else if (D == 1) {
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
    } else {
        /* A sheet drawn at a different multiple than the slot it lands in.
           Point sampling either way: the sheet is color-keyed pixel art, so
           its transparent pixels still hold the key green, and interpolating
           across a key boundary invents green fringing and part-transparent
           pixels the art never had.  Point sampling is also what 1.x did.
           A sheet finer than the slot loses detail here; the alternative is
           building the output sheet at the skin's density rather than
           scaling into a coarser one. */
        SDL_Rect srcRect = { e->sheetX * D, e->sheetY * D,
                             e->width * D, e->height * D };
        SDL_Rect dstRect = { dstX, dstY, w, h };
        SDL_BlitSurfaceScaled(src, &srcRect, sheet, &dstRect,
                              SDL_SCALEMODE_NEAREST);
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

/* Load the skin's own whole sheet — tiles.bmp, else skin.bmp, else the
 * skin32.bmp that 1.x skins ship — color-keyed the same way as
 * data/skin.bmp.  Writes the multiple the sheet is drawn at to *outDensity.
 * NULL when the skin holds none of them. */
static SDL_Surface *loadSkinSheet(struct SkinSource *skin, int *outDensity) {
    static const char *names[] = { "tiles.bmp", "skin.bmp", "skin32.bmp" };

    if (outDensity) *outDensity = 1;

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
        if (keyed) {
            /* The multiple comes from the pixels, never from the name: the
               32 in skin32.bmp is its tile size, which is 2x here, but
               nothing holds an author to that.  A sheet that is not a whole
               multiple on both axes is used as 1x, which is what happened to
               every sheet before, so it earns a warning and no more. */
            int d = keyed->w / TILE_FILE_X;
            if (d >= 1 && d <= SKIN_DENSITY_MAX &&
                keyed->w == TILE_FILE_X * d && keyed->h == TILE_FILE_Y * d) {
                if (outDensity) *outDensity = d;
            } else {
                WB_LOG_WARN(WB_LOG_CAT_ASSET,
                        "tileLoaderBuildSheet: skin %s is %dx%d, not a whole multiple of %dx%d; using it as 1x",
                        names[i], keyed->w, keyed->h, TILE_FILE_X, TILE_FILE_Y);
            }
            return keyed;
        }
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: skin %s is not a usable BMP",
                names[i]);
    }
    return NULL;
}

SDL_Surface *tileLoaderBuildSheetFor(struct SkinSource *skin, int tileSize,
                                     int mode) {
#if WB_SKIN_DRAWTIME_ROTATION
    /* The fill pass at the end records the slots it turns.  Drop whatever a
       previous build left first: this one may be for a different skin, a
       different Tile Detail mode or a different zoom, and any of the three
       can change which slots end up turned. */
    s_rotatedSlotCount = 0;
#endif

    /* Scale factor: tileSize / BASE_TILE (16).  When tileSize==16, scale==1
       and the sheet is the classic 496x176.  When tileSize==32, scale==2
       and SVGs are rasterized at 2x for crisper rendering. */
    int scale = tileSize / TILE_SIZE_X;
    if (scale < 1) scale = 1;

    /* What the skin can serve, fetched once for the whole build.  The mode
       and the scan are the same for every sprite, so the per-sprite loop
       only asks tileLoaderPickDensity what to do with them. */
    const SkinDensityInfo *density = skin ? tileLoaderGetDensityInfo(skin)
                                          : NULL;

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
    int skinSheetDensity = 1;
    SDL_Surface *skinSheet = skin ? loadSkinSheet(skin, &skinSheetDensity) : NULL;

    /* Name the skin in the summary below so a skin resolving to the wrong
       assets can be told apart from the built-in set in the log.  The same
       read carries MaxPixelDensity, the finest the author means their vector
       art to be rasterized at; 0 is uncapped. */
    const char *skinLabel = "none";
    SkinInfo    skinInfo;
    int         svgCap = 0;
    bool        inGameRotate = false;
    if (skin) {
        skinSourceReadIni(skin, &skinInfo);
        svgCap = skinInfo.maxPixelDensity;
        inGameRotate = skinInfo.inGameRotate != 0;
        if (skin == skinGetActiveSource() && skinGetActive()[0] != '\0') {
            skinLabel = skinGetActive();
        } else {
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
    int skinDensityCount = 0;
    int rotatedCount = 0;

    /* Which sprites came out of the skin rather than out of data/svg/ or
       data/skin.bmp, indexed the same way as gTileMap[].  The static assert
       at the top of the file is what makes that index safe here too. */
    bool skinSupplied[SKIN_DENSITY_MAX_SPRITES];
    SDL_memset(skinSupplied, 0, sizeof(skinSupplied));

    for (int i = 0; gTileMap[i].name != NULL; i++) {
        const TileMapEntry *e = &gTileMap[i];
        int w = e->width  * scale;
        int h = e->height * scale;
        int dstX = e->sheetX * scale;
        int dstY = e->sheetY * scale;
        bool loaded = false;

        /* The density this sprite is wanted at under the current mode.  1 is
           Classic, and anything the mode cannot better; the chain below is
           then exactly the one that has always run. */
        int want = tileLoaderPickDensity(density, i, mode, scale);

        /* Above 1 the finer art comes first: the exact @Nx, the nearest @Mx
           above it, the skin's own SVG, then the nearest @Mx below.  Each of
           these is decoded straight to the slot size, so the sheet needs no
           scaling pass of its own. */
        if (skin && want >= 2) {
            if (tryLoadSkinDensityPNG(skin, e->name, want, w, h, tmpBuf)) {
                blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                loaded = true;
                skinDensityCount++;
            }

            if (!loaded) {
                int above = skinNearestDensity(skin, e->name, want, true);
                if (above > 0 &&
                    tryLoadSkinDensityPNG(skin, e->name, above, w, h, tmpBuf)) {
                    blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                    loaded = true;
                    skinDensityCount++;
                }
            }

            /* Past MaxPixelDensity the author has said their vector art is
               not meant to go, so the coarser @Mx below is the better
               answer. */
            if (!loaded && (svgCap == 0 || want <= svgCap)) {
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
            }

            if (!loaded) {
                int below = skinNearestDensity(skin, e->name, want, false);
                if (below > 0 &&
                    tryLoadSkinDensityPNG(skin, e->name, below, w, h, tmpBuf)) {
                    blitRGBA(sheet, dstX, dstY, w, h, tmpBuf);
                    loaded = true;
                    skinDensityCount++;
                }
            }
        }

        /* The skin gets first refusal on every sprite: its own SVG, then its
           own PNG, then a cutout of its whole sheet. */
        if (skin && !loaded) {
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

            if (!loaded &&
                blitSheetSprite(sheet, skinSheet, e, scale, skinSheetDensity)) {
                loaded = true;
                skinSheetCount++;
            }
        }

        /* Everything above this point is the skin's own art; everything below
           is the built-in chain. */
        skinSupplied[i] = loaded;

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

        /* Fall back to BMP — data/skin.bmp is always drawn at 1x. */
        if (!loaded && blitSheetSprite(sheet, bmp, e, scale, 1)) {
            bmpCount++;
        }
    }

    /* A skin with InGameRotate=1 draws one north-facing frame per tank group
       and leaves the other fifteen to be turned from it.  Filling them into
       the sheet here keeps every consumer — the status bar, the ImGui atlas
       icons, the map editor, the log viewer, the menu background — working
       without knowing anything about rotation. */
    if (inGameRotate && tmpBuf) {
        unsigned char *frame0 = (unsigned char *)SDL_malloc(
            (size_t)(maxSpriteSize * maxSpriteSize * 4));

        for (int g = 0;
             frame0 && g < (int)(sizeof(kRotationGroups) /
                                 sizeof(kRotationGroups[0]));
             g++) {
            char nameBuf[64];

            SDL_snprintf(nameBuf, sizeof(nameBuf), "%s_00", kRotationGroups[g]);
            int baseIdx = tileMapIndexOf(nameBuf);
            /* No _00 from the skin means there is nothing of the author's to
               turn, so the whole group keeps the built-in art. */
            if (baseIdx < 0 || !skinSupplied[baseIdx]) continue;

            const TileMapEntry *base = &gTileMap[baseIdx];
            /* Turning about the centre only lands back in the same slot when
               the sprite is square.  Every group here is 16x16 at density 1,
               but check it rather than trust it. */
            if (base->width != base->height) {
                WB_LOG_WARN(WB_LOG_CAT_ASSET,
                        "tileLoaderBuildSheet: %s_00 is %dx%d, not square; not rotating this group",
                        kRotationGroups[g], base->width, base->height);
                continue;
            }

            int size = base->width * scale;
            readSheetRGBA(sheet, base->sheetX * scale, base->sheetY * scale,
                          size, size, frame0);

#if WB_SKIN_DRAWTIME_ROTATION
            /* The north slot against itself, so a draw path with an angle
               finer than a frame can turn the north facing as well.  Nothing
               about the sheet changes: the slot keeps the author's sprite. */
            noteRotatedSlot(base, base);
#endif

            for (int f = 1; f < ROTATION_GROUP_FRAMES; f++) {
                SDL_snprintf(nameBuf, sizeof(nameBuf), "%s_%02d",
                             kRotationGroups[g], f);
                int idx = tileMapIndexOf(nameBuf);
                if (idx < 0) continue;
                /* A facing the author drew always beats a rotated one. */
                if (skinSupplied[idx]) continue;

                const TileMapEntry *m = &gTileMap[idx];
                if (m->width != base->width || m->height != base->height) {
                    WB_LOG_WARN(WB_LOG_CAT_ASSET,
                            "tileLoaderBuildSheet: %s is %dx%d, not %dx%d like _00; leaving it alone",
                            nameBuf, m->width, m->height,
                            base->width, base->height);
                    continue;
                }

                /* Frame 0 faces north and the frames go clockwise, so this
                   is a clockwise turn of one sixteenth of a circle per
                   frame. */
                rotateRGBA(frame0, tmpBuf, size, (float)f * 22.5f);
                blitRGBA(sheet, m->sheetX * scale, m->sheetY * scale,
                         size, size, tmpBuf);
#if WB_SKIN_DRAWTIME_ROTATION
                noteRotatedSlot(m, base);
#endif
                rotatedCount++;
            }
        }
        SDL_free(frame0);
    }

    WB_LOG_INFO(WB_LOG_CAT_ASSET, "tileLoaderBuildSheet: scale=%d, sheet=%dx%d, tile detail=%s, loaded %d SVG, %d PNG, %d BMP fallback sprites; skin=%s: %d SVG, %d PNG, %d @Nx, %d sheet sprites from a density %d sheet, %d slots filled by rotation",
            scale, sheetW, sheetH, tileDetailName(mode),
            svgCount, pngCount, bmpCount,
            skinLabel, skinSvgCount, skinPngCount, skinDensityCount,
            skinSheetCount, skinSheetDensity, rotatedCount);

    SDL_free(tmpBuf);
    if (rast) nsvgDeleteRasterizer(rast);
    if (bmp) SDL_DestroySurface(bmp);
    if (skinSheet) SDL_DestroySurface(skinSheet);

    return sheet;
}

SDL_Surface *tileLoaderBuildSheet(int tileSize) {
    return tileLoaderBuildSheetFor(skinGetActiveSource(), tileSize,
                                   (int)gfxGetTileDetail());
}

void tileLoaderCleanup(void) {
    /* Drop the density scan.  The serial key already keeps a later skin from
     * hitting it, so this is housekeeping rather than correctness.
     * Reserved for future caching (e.g. keeping parsed SVGs for re-rasterization). */
    s_densitySerial = 0;
    s_densityValid  = false;
#if WB_SKIN_DRAWTIME_ROTATION
    /* And the slots the last build turned: there is no sheet left for them
     * to describe. */
    s_rotatedSlotCount = 0;
#endif
}
