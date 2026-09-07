/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapgen.c
 * Purpose:
 *   Procedural map generation framework. Provides default
 *   configs, PRNG, and dispatch to generator stubs.
 *********************************************************/

#include "mapgen.h"
#include "mapgen_maze.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "client_mappreview.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

/* xorshift32 PRNG — deterministic, platform-independent */
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

uint32_t mapGenXorshift32(uint32_t *state) {
    return xorshift32(state);
}

/*=========================================================
 * Composite seed encoding / decoding
 *=========================================================*/

/* Bit writer: packs bits MSB-first into a byte buffer. */
typedef struct {
    uint8_t *buf;
    int bitPos;  /* next bit position to write (0 = MSB of buf[0]) */
} BitWriter;

static void bwInit(BitWriter *bw, uint8_t *buf) {
    bw->buf = buf;
    bw->bitPos = 0;
}

static void bwWrite(BitWriter *bw, uint32_t val, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        int byteIdx = bw->bitPos / 8;
        int bitIdx = 7 - (bw->bitPos % 8);
        if ((val >> i) & 1)
            bw->buf[byteIdx] |= (uint8_t)(1 << bitIdx);
        else
            bw->buf[byteIdx] &= (uint8_t)~(1 << bitIdx);
        bw->bitPos++;
    }
}

/* Bit reader: reads bits MSB-first from a byte buffer. */
typedef struct {
    const uint8_t *buf;
    int bitPos;
    int totalBits;
} BitReader;

static void brInit(BitReader *br, const uint8_t *buf, int totalBits) {
    br->buf = buf;
    br->bitPos = 0;
    br->totalBits = totalBits;
}

static uint32_t brRead(BitReader *br, int bits) {
    uint32_t val = 0;
    for (int i = bits - 1; i >= 0; i--) {
        if (br->bitPos >= br->totalBits) break;
        int byteIdx = br->bitPos / 8;
        int bitIdx = 7 - (br->bitPos % 8);
        if (br->buf[byteIdx] & (1 << bitIdx))
            val |= (1u << i);
        br->bitPos++;
    }
    return val;
}

static void bytesToHex(const uint8_t *bytes, int count, char *out) {
    for (int i = 0; i < count; i++) {
        sprintf(out + i * 2, "%02X", bytes[i]);
    }
    out[count * 2] = '\0';
}

static bool hexToBytes(const char *hex, uint8_t *bytes, int count) {
    for (int i = 0; i < count; i++) {
        unsigned int v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        bytes[i] = (uint8_t)v;
    }
    return true;
}

/*
 * Tournament bit layout (59 bits = 8 bytes, padded):
 *   genType(1) + symmetry(3) + landMass(5) + roughness(2) + roads(1)
 *   + bases(5) + pills(5) + starts(5) + seed(32) = 59 bits
 *   Packed into 8 bytes (64 bits, top 5 unused on decode).
 *
 * Natural bit layout (106 bits = 14 bytes, padded):
 *   genType(1) + style(3) + grass(7) + forest(7) + building(7) + swamp(7)
 *   + river(7) + boat(7) + mineDensity(3) + riverCount(3) + cityCount(4)
 *   + mazeCount(3) + bases(5) + pills(5) + starts(5) + seed(32) = 106 bits
 */

void mapGenBuildDisplayName(const MapGenConfig *cfg, char *out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!cfg) return;

    const char *typeName = "Random";
    char subStyle[24] = "";
    switch (cfg->genType) {
        case MAPGEN_TOURNAMENT: typeName = "Tournament"; break;
        case MAPGEN_NATURAL: {
            typeName = "Natural";
            switch (cfg->params.natural.mapStyle) {
                case MAPGEN_STYLE_OCEAN:       SDL_strlcpy(subStyle, "_Ocean",       sizeof(subStyle)); break;
                case MAPGEN_STYLE_CONTINENT:   SDL_strlcpy(subStyle, "_Continent",   sizeof(subStyle)); break;
                case MAPGEN_STYLE_ISLANDS:     SDL_strlcpy(subStyle, "_Islands",     sizeof(subStyle)); break;
                case MAPGEN_STYLE_ARCHIPELAGO: SDL_strlcpy(subStyle, "_Archipelago", sizeof(subStyle)); break;
                case MAPGEN_STYLE_INLAND:      SDL_strlcpy(subStyle, "_Inland",      sizeof(subStyle)); break;
                default: break;
            }
            break;
        }
        case MAPGEN_MAZE:    typeName = "Maze";    break;
        case MAPGEN_FRACTAL: typeName = "Fractal"; break;
        default: break;
    }

    /* Seed in 8-char hex. The full encoded seed via
     * mapGenConfigToSeed captures every config parameter and would
     * be a better unique key, but at 29+ chars (Natural) it
     * overflows MAP_STR_SIZE once combined with the type prefix.
     * The raw uint32 cfg->seed is enough to disambiguate
     * regenerations and easy to read; Copy Seed in the chooser
     * still surfaces the full encoded form for reproducibility. */
    SDL_snprintf(out, outLen, "%s%s_%08X",
                 typeName, subStyle, cfg->seed);
}

void mapGenConfigToSeed(const MapGenConfig *cfg, char *out, size_t outLen) {
    if (cfg->genType == MAPGEN_TOURNAMENT) {
        /* Layout (62 bits used in an 8-byte buf, 2 bits padding):
         *   genType(1) symmetry(3) landMassPct(5) roughness(2)
         *   includeRoads(1) bases(5) pills(5) starts(5) seed(32)
         *   waterBarrier(3)
         * waterBarrier is appended AFTER seed so legacy seeds
         * (without those 3 bits) read as waterBarrier=0 — the old
         * padding zeros stand in for it. */
        uint8_t buf[8];
        memset(buf, 0, sizeof(buf));
        BitWriter bw;
        bwInit(&bw, buf);
        bwWrite(&bw, (uint32_t)cfg->genType, 1);
        bwWrite(&bw, (uint32_t)cfg->params.tournament.symmetryMode, 3);
        bwWrite(&bw, (uint32_t)cfg->params.tournament.landMassPct, 5);
        bwWrite(&bw, (uint32_t)cfg->params.tournament.roughness, 2);
        bwWrite(&bw, cfg->params.tournament.includeRoads ? 1 : 0, 1);
        bwWrite(&bw, (uint32_t)cfg->bases, 5);
        bwWrite(&bw, (uint32_t)cfg->pills, 5);
        bwWrite(&bw, (uint32_t)cfg->starts, 5);
        bwWrite(&bw, cfg->seed, 32);
        bwWrite(&bw, (uint32_t)cfg->params.tournament.waterBarrier, 3);

        char hex[17];
        bytesToHex(buf, 8, hex);
        snprintf(out, outLen, "T%s", hex);
    } else if (cfg->genType == MAPGEN_NATURAL) {
        uint8_t buf[14];
        memset(buf, 0, sizeof(buf));
        BitWriter bw;
        bwInit(&bw, buf);
        bwWrite(&bw, (uint32_t)cfg->genType, 1);
        bwWrite(&bw, (uint32_t)cfg->params.natural.mapStyle, 3);
        bwWrite(&bw, (uint32_t)cfg->params.natural.grassPct, 7);
        bwWrite(&bw, (uint32_t)cfg->params.natural.forestPct, 7);
        bwWrite(&bw, (uint32_t)cfg->params.natural.buildingPct, 7);
        bwWrite(&bw, (uint32_t)cfg->params.natural.swampPct, 7);
        bwWrite(&bw, (uint32_t)cfg->params.natural.riverPct, 7);
        bwWrite(&bw, (uint32_t)cfg->params.natural.boatPct, 7);
        bwWrite(&bw, (uint32_t)cfg->params.natural.mineDensityPct, 3);
        bwWrite(&bw, (uint32_t)cfg->params.natural.riverCount, 3);
        bwWrite(&bw, (uint32_t)cfg->params.natural.cityCount, 4);
        bwWrite(&bw, (uint32_t)cfg->params.natural.mazeCount, 3);
        bwWrite(&bw, (uint32_t)cfg->bases, 5);
        bwWrite(&bw, (uint32_t)cfg->pills, 5);
        bwWrite(&bw, (uint32_t)cfg->starts, 5);
        bwWrite(&bw, cfg->seed, 32);

        char hex[29];
        bytesToHex(buf, 14, hex);
        snprintf(out, outLen, "N%s", hex);
    } else if (cfg->genType == MAPGEN_MAZE) {
        /* Maze bit layout (65 bits = 9 bytes, padded):
         *   algo(1) + wallThick(1) + corridorWidth(1) + entries(4)
         *   + bases(5) + pills(5) + starts(5) + cityRooms(3)
         *   + wallTerrain(4) + corridorTerrain(4) + seed(32) = 65 bits */
        uint8_t buf[9];
        memset(buf, 0, sizeof(buf));
        BitWriter bw;
        bwInit(&bw, buf);
        bwWrite(&bw, (uint32_t)cfg->params.maze.algo, 1);
        bwWrite(&bw, (uint32_t)(cfg->params.maze.wallThick - 1), 1);
        bwWrite(&bw, (uint32_t)(cfg->params.maze.corridorWidth - 1), 1);
        bwWrite(&bw, (uint32_t)cfg->params.maze.entries, 4);
        bwWrite(&bw, (uint32_t)cfg->bases, 5);
        bwWrite(&bw, (uint32_t)cfg->pills, 5);
        bwWrite(&bw, (uint32_t)cfg->starts, 5);
        bwWrite(&bw, (uint32_t)cfg->params.maze.cityRooms, 3);
        /* Terrain: 0-9 = normal, 10 = DEEP_SEA (0xFF) */
        int wt = cfg->params.maze.wallTerrain;
        int ct = cfg->params.maze.corridorTerrain;
        bwWrite(&bw, (uint32_t)(wt == DEEP_SEA ? 10 : wt), 4);
        bwWrite(&bw, (uint32_t)(ct == DEEP_SEA ? 10 : ct), 4);
        bwWrite(&bw, cfg->seed, 32);

        char hex[19];
        bytesToHex(buf, 9, hex);
        snprintf(out, outLen, "M%s", hex);
    } else if (cfg->genType == MAPGEN_FRACTAL) {
        /* Fractal bit layout (73 bits = 10 bytes, padded):
         *   landPct(7) + roughness(4) + detail(3) + coastJaggedness(4)
         *   + terrainLayers(3) + rivers(1) + lakes(1) + mineDensityPct(3)
         *   + bases(5) + pills(5) + starts(5) + seed(32) = 73 bits */
        uint8_t buf[10];
        memset(buf, 0, sizeof(buf));
        BitWriter bw;
        bwInit(&bw, buf);
        bwWrite(&bw, (uint32_t)cfg->params.fractal.landPct, 7);
        bwWrite(&bw, (uint32_t)cfg->params.fractal.roughness, 4);
        bwWrite(&bw, (uint32_t)cfg->params.fractal.detail, 3);
        bwWrite(&bw, (uint32_t)cfg->params.fractal.coastJaggedness, 4);
        bwWrite(&bw, (uint32_t)cfg->params.fractal.terrainLayers, 3);
        bwWrite(&bw, cfg->params.fractal.rivers ? 1 : 0, 1);
        bwWrite(&bw, cfg->params.fractal.lakes ? 1 : 0, 1);
        bwWrite(&bw, (uint32_t)cfg->params.fractal.mineDensityPct, 3);
        bwWrite(&bw, (uint32_t)cfg->bases, 5);
        bwWrite(&bw, (uint32_t)cfg->pills, 5);
        bwWrite(&bw, (uint32_t)cfg->starts, 5);
        bwWrite(&bw, cfg->seed, 32);

        char hex[21];
        bytesToHex(buf, 10, hex);
        snprintf(out, outLen, "F%s", hex);
    }
}

bool mapGenSeedToConfig(const char *seedStr, MapGenConfig *cfg) {
    if (!seedStr || !seedStr[0]) return false;

    char prefix = (char)toupper((unsigned char)seedStr[0]);

    if (prefix == 'T') {
        const char *hex = seedStr + 1;
        if (strlen(hex) != 16) return false;
        uint8_t buf[8];
        if (!hexToBytes(hex, buf, 8)) return false;

        BitReader br;
        brInit(&br, buf, 64);
        cfg->genType = (int)brRead(&br, 1);
        cfg->params.tournament.symmetryMode = (int)brRead(&br, 3);
        cfg->params.tournament.landMassPct = (int)brRead(&br, 5);
        cfg->params.tournament.roughness = (int)brRead(&br, 2);
        cfg->params.tournament.includeRoads = brRead(&br, 1) != 0;
        cfg->bases = (int)brRead(&br, 5);
        cfg->pills = (int)brRead(&br, 5);
        cfg->starts = (int)brRead(&br, 5);
        cfg->seed = brRead(&br, 32);
        /* waterBarrier is the last 3 bits — legacy seeds (written
         * before this field existed) had zero padding here, which
         * naturally decodes to waterBarrier=0 (no rim). */
        cfg->params.tournament.waterBarrier = (int)brRead(&br, 3);
        return true;
    } else if (prefix == 'N') {
        const char *hex = seedStr + 1;
        size_t hexLen = strlen(hex);
        /* Accept old 24-char (96-bit), 26-char (99-bit), and new 28-char (106-bit) seeds */
        if (hexLen != 24 && hexLen != 26 && hexLen != 28) return false;
        int byteCount = (hexLen == 28) ? 14 : (hexLen == 26) ? 13 : 12;
        uint8_t buf[14];
        memset(buf, 0, sizeof(buf));
        if (!hexToBytes(hex, buf, byteCount)) return false;

        BitReader br;
        brInit(&br, buf, byteCount * 8);
        cfg->genType = (int)brRead(&br, 1);
        cfg->params.natural.mapStyle = (int)brRead(&br, 3);
        cfg->params.natural.grassPct = (int)brRead(&br, 7);
        cfg->params.natural.forestPct = (int)brRead(&br, 7);
        cfg->params.natural.buildingPct = (int)brRead(&br, 7);
        cfg->params.natural.swampPct = (int)brRead(&br, 7);
        cfg->params.natural.riverPct = (int)brRead(&br, 7);
        if (hexLen == 28) {
            cfg->params.natural.boatPct = (int)brRead(&br, 7);
        } else {
            cfg->params.natural.boatPct = 20;
        }
        cfg->params.natural.mineDensityPct = (int)brRead(&br, 3);
        cfg->params.natural.riverCount = (int)brRead(&br, 3);
        cfg->params.natural.cityCount = (int)brRead(&br, 4);
        if (hexLen >= 26) {
            cfg->params.natural.mazeCount = (int)brRead(&br, 3);
        } else {
            cfg->params.natural.mazeCount = 0;
        }
        cfg->bases = (int)brRead(&br, 5);
        cfg->pills = (int)brRead(&br, 5);
        cfg->starts = (int)brRead(&br, 5);
        cfg->seed = brRead(&br, 32);
        return true;
    } else if (prefix == 'M') {
        const char *hex = seedStr + 1;
        size_t hexLen = strlen(hex);
        /* Default terrain for older formats */
        cfg->params.maze.wallTerrain = BUILDING;
        cfg->params.maze.corridorTerrain = ROAD;
        if (hexLen == 18) {
            /* Current format: 9 bytes with terrain types */
            uint8_t buf[9];
            if (!hexToBytes(hex, buf, 9)) return false;

            BitReader br;
            brInit(&br, buf, 72);
            cfg->genType = MAPGEN_MAZE;
            cfg->params.maze.algo = (int)brRead(&br, 1);
            cfg->params.maze.wallThick = (int)brRead(&br, 1) + 1;
            cfg->params.maze.corridorWidth = (int)brRead(&br, 1) + 1;
            cfg->params.maze.entries = (int)brRead(&br, 4);
            cfg->bases = (int)brRead(&br, 5);
            cfg->pills = (int)brRead(&br, 5);
            cfg->starts = (int)brRead(&br, 5);
            cfg->params.maze.cityRooms = (int)brRead(&br, 3);
            int wt = (int)brRead(&br, 4);
            int ct = (int)brRead(&br, 4);
            cfg->params.maze.wallTerrain = (wt == 10) ? DEEP_SEA : wt;
            cfg->params.maze.corridorTerrain = (ct == 10) ? DEEP_SEA : ct;
            cfg->seed = brRead(&br, 32);
        } else if (hexLen == 16) {
            /* Previous format: 8 bytes with bases/pills/starts counts */
            uint8_t buf[8];
            if (!hexToBytes(hex, buf, 8)) return false;

            BitReader br;
            brInit(&br, buf, 64);
            cfg->genType = MAPGEN_MAZE;
            cfg->params.maze.algo = (int)brRead(&br, 1);
            cfg->params.maze.wallThick = (int)brRead(&br, 1) + 1;
            cfg->params.maze.corridorWidth = (int)brRead(&br, 1) + 1;
            cfg->params.maze.entries = (int)brRead(&br, 4);
            cfg->bases = (int)brRead(&br, 5);
            cfg->pills = (int)brRead(&br, 5);
            cfg->starts = (int)brRead(&br, 5);
            cfg->params.maze.cityRooms = (int)brRead(&br, 3);
            cfg->seed = brRead(&br, 32);
        } else if (hexLen == 12) {
            /* Legacy format: 6 bytes with bool placeBases/placePills */
            uint8_t buf[6];
            if (!hexToBytes(hex, buf, 6)) return false;

            BitReader br;
            brInit(&br, buf, 48);
            cfg->genType = MAPGEN_MAZE;
            cfg->params.maze.algo = (int)brRead(&br, 1);
            cfg->params.maze.wallThick = (int)brRead(&br, 1) + 1;
            cfg->params.maze.corridorWidth = (int)brRead(&br, 1) + 1;
            cfg->params.maze.entries = (int)brRead(&br, 4);
            bool oldBases = brRead(&br, 1) != 0;
            bool oldPills = brRead(&br, 1) != 0;
            cfg->bases = oldBases ? 16 : 0;
            cfg->pills = oldPills ? 16 : 0;
            cfg->starts = 16;
            cfg->params.maze.cityRooms = (int)brRead(&br, 3);
            cfg->seed = brRead(&br, 32);
        } else {
            return false;
        }
        return true;
    } else if (prefix == 'F') {
        const char *hex = seedStr + 1;
        size_t hexLen = strlen(hex);
        if (hexLen != 20) return false;
        uint8_t buf[10];
        if (!hexToBytes(hex, buf, 10)) return false;

        BitReader br;
        brInit(&br, buf, 80);
        cfg->genType = MAPGEN_FRACTAL;
        cfg->params.fractal.landPct = (int)brRead(&br, 7);
        cfg->params.fractal.roughness = (int)brRead(&br, 4);
        cfg->params.fractal.detail = (int)brRead(&br, 3);
        cfg->params.fractal.coastJaggedness = (int)brRead(&br, 4);
        cfg->params.fractal.terrainLayers = (int)brRead(&br, 3);
        cfg->params.fractal.rivers = brRead(&br, 1) != 0;
        cfg->params.fractal.lakes = brRead(&br, 1) != 0;
        cfg->params.fractal.mineDensityPct = (int)brRead(&br, 3);
        cfg->bases = (int)brRead(&br, 5);
        cfg->pills = (int)brRead(&br, 5);
        cfg->starts = (int)brRead(&br, 5);
        cfg->seed = brRead(&br, 32);
        return true;
    } else {
        /* Plain number: backwards compatible, only update RNG seed */
        char *end = NULL;
        unsigned long val = strtoul(seedStr, &end, 10);
        if (end == seedStr || *end != '\0') return false;
        cfg->seed = (uint32_t)val;
        return true;
    }
}

/*=========================================================
 * Tournament generator helpers
 *=========================================================*/

/* Integer hash for deterministic noise grid values. Returns [0, 1]. */
static float noiseHash(uint32_t seed, int gx, int gy) {
    uint32_t h = (uint32_t)gx * 374761393u + (uint32_t)gy * 668265263u + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    h = (h ^ (h >> 13)) * 1911520717u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

/* Value noise with smoothstep interpolation at point (fx, fy). */
static float valueNoise(uint32_t seed, float fx, float fy, int cellSize) {
    float cs = (float)cellSize;
    float cx = fx / cs;
    float cy = fy / cs;
    int ix = (int)floorf(cx);
    int iy = (int)floorf(cy);
    float tx = cx - (float)ix;
    float ty = cy - (float)iy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    float v00 = noiseHash(seed, ix, iy);
    float v10 = noiseHash(seed, ix + 1, iy);
    float v01 = noiseHash(seed, ix, iy + 1);
    float v11 = noiseHash(seed, ix + 1, iy + 1);
    float v0 = v00 + (v10 - v00) * tx;
    float v1 = v01 + (v11 - v01) * tx;
    return v0 + (v1 - v0) * ty;
}

/* Fractal Brownian motion: sum multiple octaves of value noise. */
static float fbmNoise(uint32_t seed, float fx, float fy, int baseCell, int octaves) {
    float val = 0.0f, amp = 1.0f, total = 0.0f;
    int cell = baseCell;
    for (int o = 0; o < octaves; o++) {
        if (cell < 1) cell = 1;
        val += amp * valueNoise(seed + (uint32_t)o * 12345u, fx, fy, cell);
        total += amp;
        amp *= 0.5f;
        cell /= 2;
    }
    return val / total;
}

/* Find height threshold so the top landPct% of values are >= threshold.
 * Uses a 1024-bin histogram. */
static float findLandThreshold(const float *heights, int count, int landPct) {
    int bins[1024];
    memset(bins, 0, sizeof(bins));
    for (int i = 0; i < count; i++) {
        int b = (int)(heights[i] * 1023.0f);
        if (b < 0) b = 0;
        if (b > 1023) b = 1023;
        bins[b]++;
    }
    int target = (count * landPct + 50) / 100;
    if (target <= 0) return 2.0f;
    int cumul = 0;
    for (int b = 1023; b >= 0; b--) {
        cumul += bins[b];
        if (cumul >= target) return (float)b / 1023.0f;
    }
    return 0.0f;
}

/* Draw an L-shaped road path on land tiles (horizontal first, then vertical).
 * Only overwrites non-DEEP_SEA tiles within the given bounds. */
static void drawRoadPath(struct mapObj *mp, int fromX, int fromY, int toX, int toY,
                         int bx1, int by1, int bx2, int by2) {
    int x = fromX, y = fromY;
    int dx = (toX > x) ? 1 : (toX < x) ? -1 : 0;
    while (x != toX) {
        if (x >= bx1 && x <= bx2 && y >= by1 && y <= by2 &&
            mp->mapItem[x][y] != DEEP_SEA) {
            mp->mapItem[x][y] = ROAD;
        }
        x += dx;
    }
    int dy = (toY > y) ? 1 : (toY < y) ? -1 : 0;
    while (y != toY) {
        if (x >= bx1 && x <= bx2 && y >= by1 && y <= by2 &&
            mp->mapItem[x][y] != DEEP_SEA) {
            mp->mapItem[x][y] = ROAD;
        }
        y += dy;
    }
}

/* Compute a 0-15 direction from (px,py) pointing toward (cx,cy). */
static BYTE dirTowardCenter(int px, int py, int cx, int cy) {
    float fdx = (float)(cx - px);
    float fdy = (float)(cy - py);
    if (fdx == 0.0f && fdy == 0.0f) return 0;
    float angle = atan2f(fdx, -fdy);
    if (angle < 0.0f) angle += 6.283185307f;
    int dir = (int)(angle / 6.283185307f * 16.0f + 0.5f) % 16;
    return (BYTE)dir;
}

/* Direction transforms for mirroring/rotating objects */
static BYTE tournDirMirrorH(BYTE dir)   { return (BYTE)((16 - dir) % 16); }
static BYTE tournDirMirrorV(BYTE dir)   { return (BYTE)((8 - dir + 16) % 16); }
static BYTE tournDirRotate180(BYTE dir) { return (BYTE)((dir + 8) % 16); }

/* Candidate position for object placement */
typedef struct { int x; int y; } GenPos;

/*=========================================================
 * Coastal fringe: convert DEEP_SEA tiles near land to RIVER
 * (shallow water) for a natural coastline transition.
 *=========================================================*/
static void addCoastalFringe(struct mapObj *mp, int x1, int y1,
                             int x2, int y2, uint32_t *rng) {
    static const int cdx[] = {0, 0, 1, -1};
    static const int cdy[] = {-1, 1, 0, 0};

    /* Pass 1: convert DEEP_SEA tiles cardinally adjacent to land into RIVER.
     * ~75% chance so the fringe isn't perfectly uniform. */
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            if (mp->mapItem[x][y] != DEEP_SEA) continue;
            bool adjLand = false;
            for (int d = 0; d < 4; d++) {
                int nx = x + cdx[d], ny = y + cdy[d];
                if (nx < x1 || nx > x2 || ny < y1 || ny > y2) continue;
                BYTE t = mp->mapItem[nx][ny];
                if (t != DEEP_SEA && t != RIVER) { adjLand = true; break; }
            }
            if (adjLand && (xorshift32(rng) % 100) < 75) {
                mp->mapItem[x][y] = RIVER;
            }
        }
    }

    /* Pass 2: fill short DEEP_SEA gaps (up to 4 tiles) between RIVER/land
     * tiles in cardinal directions to create navigable channels. */
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            if (mp->mapItem[x][y] != RIVER) continue;
            for (int d = 0; d < 4; d++) {
                /* Scan up to 5 tiles in this cardinal direction */
                int gapLen = 0;
                bool foundEnd = false;
                for (int dist = 1; dist <= 5; dist++) {
                    int nx = x + cdx[d] * dist;
                    int ny = y + cdy[d] * dist;
                    if (nx < x1 || nx > x2 || ny < y1 || ny > y2) break;
                    BYTE t = mp->mapItem[nx][ny];
                    if (t == DEEP_SEA) {
                        gapLen++;
                    } else {
                        /* Found land or RIVER on the other side */
                        if (t != DEEP_SEA) foundEnd = true;
                        break;
                    }
                }
                if (foundEnd && gapLen >= 1 && gapLen <= 4) {
                    for (int dist = 1; dist <= gapLen; dist++) {
                        int nx = x + cdx[d] * dist;
                        int ny = y + cdy[d] * dist;
                        mp->mapItem[nx][ny] = RIVER;
                    }
                }
            }
        }
    }
}

/*=========================================================
 * Tournament map generator
 *=========================================================*/
static void mapGenTournament(struct mapObj *mp, struct basesObj *bs,
                             struct pillsObj *pb, struct startsObj *ss,
                             const MapGenConfig *cfg, uint32_t *rng) {
    const int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    const int midX = (x1 + x2) / 2, midY = (y1 + y2) / 2;
    const int landPct = cfg->params.tournament.landMassPct;
    const int rough = cfg->params.tournament.roughness;
    const bool roads = cfg->params.tournament.includeRoads;

    /* Effective symmetry — rotate-90 requires square region, fall back to 4-corner */
    int symMode = cfg->params.tournament.symmetryMode;
    if (symMode == MAPGEN_SYM_ROTATE90 && w != h)
        symMode = MAPGEN_SYM_4CORNER;

    /* Determine sector bounds and number of players (symmetry copies) */
    int sx1 = x1, sy1 = y1, sx2, sy2, numPlayers;
    switch (symMode) {
        case MAPGEN_SYM_MIRROR_H:  sx2 = midX; sy2 = y2;   numPlayers = 2; break;
        case MAPGEN_SYM_MIRROR_V:  sx2 = x2;   sy2 = midY; numPlayers = 2; break;
        case MAPGEN_SYM_ROTATE180: sx2 = x2;   sy2 = midY; numPlayers = 2; break;
        case MAPGEN_SYM_ROTATE90:  sx2 = midX; sy2 = midY; numPlayers = 4; break;
        default: /* MAPGEN_SYM_4CORNER */
            sx2 = midX; sy2 = midY; numPlayers = 4; break;
    }

    const int sw = sx2 - sx1 + 1;
    const int sh = sy2 - sy1 + 1;
    const int sectorArea = sw * sh;

    /* Compute per-player counts from totals and clamp to fit within maximums */
    int basesPerP = cfg->bases / numPlayers;
    int pillsPerP = cfg->pills / numPlayers;
    int startsPerP = cfg->starts / numPlayers;
    if (basesPerP * numPlayers > (int)(MAX_BASES - bs->numBases))
        basesPerP = (int)(MAX_BASES - bs->numBases) / numPlayers;
    if (pillsPerP * numPlayers > (int)(MAX_PILLS - pb->numPills))
        pillsPerP = (int)(MAX_PILLS - pb->numPills) / numPlayers;
    if (startsPerP * numPlayers > (int)(MAX_STARTS - ss->numStarts))
        startsPerP = (int)(MAX_STARTS - ss->numStarts) / numPlayers;

    /* Object placement bounds: inset from center lines to prevent self-overlapping mirrors */
    int objSx2 = sx2, objSy2 = sy2;
    if (symMode == MAPGEN_SYM_4CORNER || symMode == MAPGEN_SYM_MIRROR_H || symMode == MAPGEN_SYM_ROTATE90)
        objSx2 = sx2 - 2;
    if (symMode == MAPGEN_SYM_4CORNER || symMode == MAPGEN_SYM_MIRROR_V ||
        symMode == MAPGEN_SYM_ROTATE180 || symMode == MAPGEN_SYM_ROTATE90)
        objSy2 = sy2 - 2;
    if (objSx2 < sx1) objSx2 = sx1;
    if (objSy2 < sy1) objSy2 = sy1;

    /* Clear the full region to DEEP_SEA */
    for (int x = x1; x <= x2; x++)
        for (int y = y1; y <= y2; y++)
            mp->mapItem[x][y] = DEEP_SEA;

    /* === Step 1: Generate heightmap for the sector === */
    uint32_t noiseSeed1 = xorshift32(rng);
    uint32_t noiseSeed2 = xorshift32(rng);

    float centerX = (float)(x1 + x2) * 0.5f;
    float centerY = (float)(y1 + y2) * 0.5f;
    float halfW = (float)w * 0.5f;
    float halfH = (float)h * 0.5f;

    float *heights = (float *)malloc(sizeof(float) * (size_t)sectorArea);
    if (!heights) return;

    for (int ix = 0; ix < sw; ix++) {
        for (int iy = 0; iy < sh; iy++) {
            float fx = (float)(sx1 + ix);
            float fy = (float)(sy1 + iy);
            float n = fbmNoise(noiseSeed1, fx, fy, 32, 3);
            /* Aggressive radial falloff to concentrate land at center */
            float ndx = (fx - centerX) / halfW;
            float ndy = (fy - centerY) / halfH;
            float dist = sqrtf(ndx * ndx + ndy * ndy);
            float falloff = 1.0f - dist * 1.5f;
            if (falloff < 0.0f) falloff = 0.0f;
            heights[ix * sh + iy] = n * falloff;
        }
    }

    /* === Step 2: Threshold to get target land percentage === */
    float threshold = findLandThreshold(heights, sectorArea, landPct);

    /* === Step 3: Assign terrain types on land tiles === */
    for (int ix = 0; ix < sw; ix++) {
        for (int iy = 0; iy < sh; iy++) {
            if (heights[ix * sh + iy] >= threshold) {
                float n2 = fbmNoise(noiseSeed2, (float)(sx1 + ix), (float)(sy1 + iy), 16, 2);
                BYTE terrain;
                if (rough == MAPGEN_ROUGH_LOW) {
                    terrain = (n2 > 0.45f) ? FOREST : ROAD;
                } else if (rough == MAPGEN_ROUGH_MEDIUM) {
                    if (n2 > 0.55f)      terrain = FOREST;
                    else if (n2 > 0.25f) terrain = ROAD;
                    else if (n2 > 0.05f) terrain = GRASS;
                    else                  terrain = BUILDING;
                } else { /* MAPGEN_ROUGH_HIGH */
                    if (n2 > 0.60f)      terrain = FOREST;
                    else if (n2 > 0.35f) terrain = ROAD;
                    else if (n2 > 0.15f) terrain = GRASS;
                    else if (n2 > 0.05f) terrain = BUILDING;
                    else                  terrain = SWAMP;
                }
                mp->mapItem[sx1 + ix][sy1 + iy] = terrain;
            }
        }
    }

    /* === Step 3b: Ensure land connectivity (single island) === */
    /* Cardinal flood-fill from the land tile nearest sector center to find
     * the main connected mass. Remove disconnected fragments, then regrow
     * if land dropped below 60% of target. */
    {
        static const int ffdx[] = {0, 0, 1, -1};
        static const int ffdy[] = {-1, 1, 0, 0};
        int cmx = (sx1 + sx2) / 2, cmy = (sy1 + sy2) / 2;

        /* Count original land tiles */
        int origLand = 0;
        for (int x = sx1; x <= sx2; x++)
            for (int y = sy1; y <= sy2; y++)
                if (mp->mapItem[x][y] != DEEP_SEA) origLand++;

        /* Find nearest land tile to center as flood-fill seed */
        int seedX = -1, seedY = -1, bestD2 = 999999;
        for (int x = sx1; x <= sx2; x++)
            for (int y = sy1; y <= sy2; y++)
                if (mp->mapItem[x][y] != DEEP_SEA) {
                    int dd = (x - cmx) * (x - cmx) + (y - cmy) * (y - cmy);
                    if (dd < bestD2) { bestD2 = dd; seedX = x; seedY = y; }
                }

        if (seedX >= 0) {
            bool *visited = (bool *)calloc((size_t)sw * (size_t)sh, sizeof(bool));
            if (visited) {
                /* Cardinal BFS from seed */
                GenPos *queue = (GenPos *)malloc(sizeof(GenPos) * (size_t)sectorArea);
                if (queue) {
                    int head = 0, tail = 0;
                    queue[tail++] = (GenPos){seedX, seedY};
                    visited[(seedX - sx1) * sh + (seedY - sy1)] = true;
                    int connectedCount = 0;
                    while (head < tail) {
                        GenPos cur = queue[head++];
                        connectedCount++;
                        for (int d = 0; d < 4; d++) {
                            int nx = cur.x + ffdx[d], ny = cur.y + ffdy[d];
                            if (nx < sx1 || nx > sx2 || ny < sy1 || ny > sy2) continue;
                            int vi = (nx - sx1) * sh + (ny - sy1);
                            if (visited[vi]) continue;
                            if (mp->mapItem[nx][ny] == DEEP_SEA) continue;
                            visited[vi] = true;
                            queue[tail++] = (GenPos){nx, ny};
                        }
                    }
                    free(queue);

                    /* Remove disconnected land tiles */
                    for (int x = sx1; x <= sx2; x++)
                        for (int y = sy1; y <= sy2; y++)
                            if (mp->mapItem[x][y] != DEEP_SEA &&
                                !visited[(x - sx1) * sh + (y - sy1)])
                                mp->mapItem[x][y] = DEEP_SEA;

                    /* Regrow if land dropped below 60% of original */
                    int minLand = origLand * 60 / 100;
                    if (connectedCount < minLand) {
                        /* Iteratively collect DEEP_SEA tiles adjacent to the connected
                         * mass, sorted by descending heightmap value. Each pass grows
                         * the frontier by one ring; repeat until target is met or no
                         * more adjacent candidates exist. */
                        GenPos *growCands = (GenPos *)malloc(sizeof(GenPos) * (size_t)sectorArea);
                        float *growH = (float *)malloc(sizeof(float) * (size_t)sectorArea);
                        if (growCands && growH) {
                            int currentLand = connectedCount;
                            while (currentLand < origLand) {
                                int nGrow = 0;
                                for (int x = sx1; x <= sx2; x++)
                                    for (int y = sy1; y <= sy2; y++) {
                                        if (mp->mapItem[x][y] != DEEP_SEA) continue;
                                        bool adjLand = false;
                                        for (int d = 0; d < 4; d++) {
                                            int nx = x + ffdx[d], ny = y + ffdy[d];
                                            if (nx >= sx1 && nx <= sx2 && ny >= sy1 && ny <= sy2 &&
                                                mp->mapItem[nx][ny] != DEEP_SEA)
                                                { adjLand = true; break; }
                                        }
                                        if (adjLand) {
                                            growCands[nGrow] = (GenPos){x, y};
                                            growH[nGrow] = heights[(x - sx1) * sh + (y - sy1)];
                                            nGrow++;
                                        }
                                    }
                                if (nGrow == 0) break;
                                /* Sort by descending height (simple selection for small n) */
                                for (int i = 0; i < nGrow - 1; i++) {
                                    int best = i;
                                    for (int j = i + 1; j < nGrow; j++)
                                        if (growH[j] > growH[best]) best = j;
                                    if (best != i) {
                                        GenPos tp = growCands[i]; growCands[i] = growCands[best]; growCands[best] = tp;
                                        float tf = growH[i]; growH[i] = growH[best]; growH[best] = tf;
                                    }
                                }
                                /* Grow tiles with terrain assignment */
                                int grew = 0;
                                for (int i = 0; i < nGrow && currentLand < origLand; i++) {
                                    int gx = growCands[i].x, gy = growCands[i].y;
                                    float n2 = fbmNoise(noiseSeed2, (float)gx, (float)gy, 16, 2);
                                    BYTE terrain;
                                    if (rough == MAPGEN_ROUGH_LOW) {
                                        terrain = (n2 > 0.45f) ? FOREST : ROAD;
                                    } else if (rough == MAPGEN_ROUGH_MEDIUM) {
                                        if (n2 > 0.55f)      terrain = FOREST;
                                        else if (n2 > 0.25f) terrain = ROAD;
                                        else if (n2 > 0.05f) terrain = GRASS;
                                        else                  terrain = BUILDING;
                                    } else {
                                        if (n2 > 0.60f)      terrain = FOREST;
                                        else if (n2 > 0.35f) terrain = ROAD;
                                        else if (n2 > 0.15f) terrain = GRASS;
                                        else if (n2 > 0.05f) terrain = BUILDING;
                                        else                  terrain = SWAMP;
                                    }
                                    mp->mapItem[gx][gy] = terrain;
                                    currentLand++;
                                    grew++;
                                }
                                if (grew == 0) break;
                            }
                        }
                        if (growCands) free(growCands);
                        if (growH) free(growH);
                    }
                }
                free(visited);
            }
        }
    }

    /* === Step 3c: Symmetry join guarantee === */
    /* Ensure land tiles exist along the symmetry boundary so mirrored
     * halves connect into one continuous island. */
    {
        /* Determine which boundary edges need land: right edge for H mirrors,
         * bottom edge for V mirrors, both for 4-corner/rotate-90. */
        bool needRight = (symMode == MAPGEN_SYM_4CORNER ||
                          symMode == MAPGEN_SYM_MIRROR_H ||
                          symMode == MAPGEN_SYM_ROTATE90);
        bool needBottom = (symMode == MAPGEN_SYM_4CORNER ||
                           symMode == MAPGEN_SYM_MIRROR_V ||
                           symMode == MAPGEN_SYM_ROTATE180 ||
                           symMode == MAPGEN_SYM_ROTATE90);

        /* For each required boundary, check if land exists on it. If not,
         * find the nearest land tile and bridge to the boundary with GRASS. */
        if (needRight) {
            bool hasLand = false;
            for (int y = sy1; y <= sy2; y++)
                if (mp->mapItem[sx2][y] != DEEP_SEA) { hasLand = true; break; }
            if (!hasLand) {
                /* Find nearest land tile to the right edge */
                int nearX = -1, nearY = -1, nearDist = 999999;
                for (int x = sx1; x <= sx2; x++)
                    for (int y = sy1; y <= sy2; y++)
                        if (mp->mapItem[x][y] != DEEP_SEA) {
                            int d = sx2 - x;
                            if (d < nearDist) { nearDist = d; nearX = x; nearY = y; }
                        }
                if (nearX >= 0) {
                    for (int x = nearX + 1; x <= sx2; x++)
                        if (mp->mapItem[x][nearY] == DEEP_SEA)
                            mp->mapItem[x][nearY] = GRASS;
                }
            }
        }

        if (needBottom) {
            bool hasLand = false;
            for (int x = sx1; x <= sx2; x++)
                if (mp->mapItem[x][sy2] != DEEP_SEA) { hasLand = true; break; }
            if (!hasLand) {
                /* Find nearest land tile to the bottom edge */
                int nearX = -1, nearY = -1, nearDist = 999999;
                for (int x = sx1; x <= sx2; x++)
                    for (int y = sy1; y <= sy2; y++)
                        if (mp->mapItem[x][y] != DEEP_SEA) {
                            int d = sy2 - y;
                            if (d < nearDist) { nearDist = d; nearX = x; nearY = y; }
                        }
                if (nearY >= 0) {
                    for (int y = nearY + 1; y <= sy2; y++)
                        if (mp->mapItem[nearX][y] == DEEP_SEA)
                            mp->mapItem[nearX][y] = GRASS;
                }
            }
        }
    }

    free(heights);

    /* === Step 4: Place objects in sector === */
    int sectorBaseIdx  = bs->numBases;
    int sectorPillIdx  = pb->numPills;
    int sectorStartIdx = ss->numStarts;

    /* 4a. Place bases on any traversable land tile */
    if (basesPerP > 0) {
        GenPos *cands = (GenPos *)malloc(sizeof(GenPos) * (size_t)sectorArea);
        if (cands) {
            int nCand = 0;
            for (int x = sx1; x <= objSx2; x++) {
                for (int y = sy1; y <= objSy2; y++) {
                    BYTE t = mp->mapItem[x][y];
                    if (t != DEEP_SEA)
                        cands[nCand++] = (GenPos){x, y};
                }
            }
            int placed = 0, attempts = 0;
            while (placed < basesPerP && nCand > 0 && attempts < 2000) {
                int idx = (int)(xorshift32(rng) % (uint32_t)nCand);
                int bx = cands[idx].x, by = cands[idx].y;
                bool ok = true;
                for (int i = sectorBaseIdx; i < bs->numBases; i++) {
                    int ddx = (int)bs->item[i].x - bx;
                    int ddy = (int)bs->item[i].y - by;
                    if (ddx * ddx + ddy * ddy < 6 * 6) { ok = false; break; }
                }
                if (ok && bs->numBases < MAX_BASES) {
                    base *b = &bs->item[bs->numBases];
                    b->x = (BYTE)bx; b->y = (BYTE)by;
                    b->owner = 0xFF; b->armour = 90; b->shells = 90; b->mines = 90;
                    b->refuelTime = 0; b->baseTime = 0; b->justStopped = false;
                    bs->numBases++;
                    placed++;
                }
                attempts++;
            }
            free(cands);
        }
    }

    /* 4b. Place pillboxes on any land tile */
    if (pillsPerP > 0) {
        GenPos *cands = (GenPos *)malloc(sizeof(GenPos) * (size_t)sectorArea);
        if (cands) {
            int nCand = 0;
            for (int x = sx1; x <= objSx2; x++) {
                for (int y = sy1; y <= objSy2; y++) {
                    if (mp->mapItem[x][y] != DEEP_SEA)
                        cands[nCand++] = (GenPos){x, y};
                }
            }
            int placed = 0, attempts = 0;
            while (placed < pillsPerP && nCand > 0 && attempts < 2000) {
                int idx = (int)(xorshift32(rng) % (uint32_t)nCand);
                int px = cands[idx].x, py = cands[idx].y;
                bool ok = true;
                for (int i = sectorPillIdx; i < pb->numPills; i++) {
                    int ddx = (int)pb->item[i].x - px;
                    int ddy = (int)pb->item[i].y - py;
                    if (ddx * ddx + ddy * ddy < 4 * 4) { ok = false; break; }
                }
                for (int i = sectorBaseIdx; i < bs->numBases && ok; i++) {
                    if (bs->item[i].x == (BYTE)px && bs->item[i].y == (BYTE)py)
                        ok = false;
                }
                if (ok && pb->numPills < MAX_PILLS) {
                    pillbox *p = &pb->item[pb->numPills];
                    p->x = (BYTE)px; p->y = (BYTE)py;
                    p->owner = 0xFF; p->armour = 15; p->speed = 50;
                    p->inTank = false; p->reload = 0; p->coolDown = 0; p->justSeen = false;
                    pb->numPills++;
                    placed++;
                }
                attempts++;
            }
            free(cands);
        }
    }

    /* 4c. Place starts in DEEP_SEA tiles near the island coast */
    if (startsPerP > 0) {
        GenPos *cands = (GenPos *)malloc(sizeof(GenPos) * (size_t)sectorArea);
        if (cands) {
            int nCand = 0;
            for (int x = sx1; x <= objSx2; x++) {
                for (int y = sy1; y <= objSy2; y++) {
                    if (mp->mapItem[x][y] != DEEP_SEA) continue;
                    bool nearLand = false;
                    for (int ddx = -5; ddx <= 5 && !nearLand; ddx++) {
                        for (int ddy = -5; ddy <= 5 && !nearLand; ddy++) {
                            int nx = x + ddx, ny = y + ddy;
                            if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                                mp->mapItem[nx][ny] != DEEP_SEA)
                                nearLand = true;
                        }
                    }
                    if (nearLand) cands[nCand++] = (GenPos){x, y};
                }
            }
            int placed = 0, attempts = 0;
            while (placed < startsPerP && nCand > 0 && attempts < 2000) {
                int idx = (int)(xorshift32(rng) % (uint32_t)nCand);
                int stx = cands[idx].x, sty = cands[idx].y;
                bool ok = true;
                for (int i = sectorStartIdx; i < ss->numStarts; i++) {
                    int ddx = (int)ss->item[i].x - stx;
                    int ddy = (int)ss->item[i].y - sty;
                    if (ddx * ddx + ddy * ddy < 3 * 3) { ok = false; break; }
                }
                if (ok && ss->numStarts < MAX_STARTS) {
                    start *s = &ss->item[ss->numStarts];
                    s->x = (BYTE)stx; s->y = (BYTE)sty;
                    s->dir = dirTowardCenter(stx, sty,
                                             (int)(centerX + 0.5f), (int)(centerY + 0.5f));
                    ss->numStarts++;
                    placed++;
                }
                attempts++;
            }
            free(cands);
        }
    }

    /* === Step 5: Connect bases with roads === */
    if (roads) {
        int cenX = (int)(centerX + 0.5f);
        int cenY = (int)(centerY + 0.5f);
        for (int i = sectorBaseIdx; i < bs->numBases; i++) {
            drawRoadPath(mp, bs->item[i].x, bs->item[i].y, cenX, cenY,
                         sx1, sy1, sx2, sy2);
            if (i + 1 < bs->numBases) {
                drawRoadPath(mp, bs->item[i].x, bs->item[i].y,
                             bs->item[i + 1].x, bs->item[i + 1].y,
                             sx1, sy1, sx2, sy2);
            }
        }
    }

    /* === Step 5b: Coastal fringe (before symmetry so it gets mirrored) === */
    addCoastalFringe(mp, sx1, sy1, sx2, sy2, rng);

    /* === Step 6: Apply symmetry to fill the full region === */

    /* 6a. Copy terrain */
    switch (symMode) {
    case MAPGEN_SYM_4CORNER:
        /* Copy top-left to top-right (mirror H within top half) */
        for (int x = sx1; x <= sx2; x++) {
            for (int y = sy1; y <= sy2; y++) {
                int mx = x1 + x2 - x;
                if (mx != x) mp->mapItem[mx][y] = mp->mapItem[x][y];
            }
        }
        /* Copy entire top half to bottom half (mirror V) */
        for (int x = x1; x <= x2; x++) {
            for (int y = sy1; y <= sy2; y++) {
                int my = y1 + y2 - y;
                if (my != y) mp->mapItem[x][my] = mp->mapItem[x][y];
            }
        }
        break;

    case MAPGEN_SYM_MIRROR_H:
        for (int x = sx1; x <= sx2; x++) {
            for (int y = y1; y <= y2; y++) {
                int mx = x1 + x2 - x;
                if (mx != x) mp->mapItem[mx][y] = mp->mapItem[x][y];
            }
        }
        break;

    case MAPGEN_SYM_MIRROR_V:
        for (int x = x1; x <= x2; x++) {
            for (int y = sy1; y <= sy2; y++) {
                int my = y1 + y2 - y;
                if (my != y) mp->mapItem[x][my] = mp->mapItem[x][y];
            }
        }
        break;

    case MAPGEN_SYM_ROTATE180:
        for (int x = x1; x <= x2; x++) {
            for (int y = sy1; y <= sy2; y++) {
                int rx = x1 + x2 - x;
                int ry = y1 + y2 - y;
                if (rx != x || ry != y)
                    mp->mapItem[rx][ry] = mp->mapItem[x][y];
            }
        }
        break;

    case MAPGEN_SYM_ROTATE90: {
        int n = w;
        for (int ix = 0; ix < sw; ix++) {
            for (int iy = 0; iy < sh; iy++) {
                BYTE t = mp->mapItem[x1 + ix][y1 + iy];
                int d1x = iy,         d1y = n - 1 - ix;
                int d2x = n - 1 - ix, d2y = n - 1 - iy;
                int d3x = n - 1 - iy, d3y = ix;
                if (d1x != ix || d1y != iy) mp->mapItem[x1 + d1x][y1 + d1y] = t;
                if (d2x != ix || d2y != iy) mp->mapItem[x1 + d2x][y1 + d2y] = t;
                if (d3x != ix || d3y != iy) mp->mapItem[x1 + d3x][y1 + d3y] = t;
            }
        }
        break;
    }
    }

    /* 6b. Duplicate objects to other sectors */
    int sectorBases  = bs->numBases  - sectorBaseIdx;
    int sectorPills  = pb->numPills  - sectorPillIdx;
    int sectorStarts = ss->numStarts - sectorStartIdx;

    switch (symMode) {
    case MAPGEN_SYM_4CORNER: {
        /* H-mirror sector bases → top-right */
        for (int i = 0; i < sectorBases && bs->numBases < MAX_BASES; i++) {
            base src = bs->item[sectorBaseIdx + i];
            int mx = x1 + x2 - (int)src.x;
            if (mx != (int)src.x) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->x = (BYTE)mx;
            }
        }
        /* V-mirror all top-half bases → bottom */
        int topBaseEnd = bs->numBases;
        for (int i = sectorBaseIdx; i < topBaseEnd && bs->numBases < MAX_BASES; i++) {
            base src = bs->item[i];
            int my = y1 + y2 - (int)src.y;
            if (my != (int)src.y) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->y = (BYTE)my;
            }
        }
        /* H-mirror sector pills → top-right */
        for (int i = 0; i < sectorPills && pb->numPills < MAX_PILLS; i++) {
            pillbox src = pb->item[sectorPillIdx + i];
            int mx = x1 + x2 - (int)src.x;
            if (mx != (int)src.x) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->x = (BYTE)mx;
            }
        }
        /* V-mirror all top-half pills → bottom */
        int topPillEnd = pb->numPills;
        for (int i = sectorPillIdx; i < topPillEnd && pb->numPills < MAX_PILLS; i++) {
            pillbox src = pb->item[i];
            int my = y1 + y2 - (int)src.y;
            if (my != (int)src.y) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->y = (BYTE)my;
            }
        }
        /* H-mirror sector starts → top-right */
        for (int i = 0; i < sectorStarts && ss->numStarts < MAX_STARTS; i++) {
            start src = ss->item[sectorStartIdx + i];
            int mx = x1 + x2 - (int)src.x;
            if (mx != (int)src.x) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->x = (BYTE)mx;
                s->dir = tournDirMirrorH(src.dir);
            }
        }
        /* V-mirror all top-half starts → bottom */
        int topStartEnd = ss->numStarts;
        for (int i = sectorStartIdx; i < topStartEnd && ss->numStarts < MAX_STARTS; i++) {
            start src = ss->item[i];
            int my = y1 + y2 - (int)src.y;
            if (my != (int)src.y) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->y = (BYTE)my;
                s->dir = tournDirMirrorV(src.dir);
            }
        }
        break;
    }

    case MAPGEN_SYM_MIRROR_H:
        for (int i = 0; i < sectorBases && bs->numBases < MAX_BASES; i++) {
            base src = bs->item[sectorBaseIdx + i];
            int mx = x1 + x2 - (int)src.x;
            if (mx != (int)src.x) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->x = (BYTE)mx;
            }
        }
        for (int i = 0; i < sectorPills && pb->numPills < MAX_PILLS; i++) {
            pillbox src = pb->item[sectorPillIdx + i];
            int mx = x1 + x2 - (int)src.x;
            if (mx != (int)src.x) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->x = (BYTE)mx;
            }
        }
        for (int i = 0; i < sectorStarts && ss->numStarts < MAX_STARTS; i++) {
            start src = ss->item[sectorStartIdx + i];
            int mx = x1 + x2 - (int)src.x;
            if (mx != (int)src.x) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->x = (BYTE)mx;
                s->dir = tournDirMirrorH(src.dir);
            }
        }
        break;

    case MAPGEN_SYM_MIRROR_V:
        for (int i = 0; i < sectorBases && bs->numBases < MAX_BASES; i++) {
            base src = bs->item[sectorBaseIdx + i];
            int my = y1 + y2 - (int)src.y;
            if (my != (int)src.y) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->y = (BYTE)my;
            }
        }
        for (int i = 0; i < sectorPills && pb->numPills < MAX_PILLS; i++) {
            pillbox src = pb->item[sectorPillIdx + i];
            int my = y1 + y2 - (int)src.y;
            if (my != (int)src.y) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->y = (BYTE)my;
            }
        }
        for (int i = 0; i < sectorStarts && ss->numStarts < MAX_STARTS; i++) {
            start src = ss->item[sectorStartIdx + i];
            int my = y1 + y2 - (int)src.y;
            if (my != (int)src.y) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->y = (BYTE)my;
                s->dir = tournDirMirrorV(src.dir);
            }
        }
        break;

    case MAPGEN_SYM_ROTATE180:
        for (int i = 0; i < sectorBases && bs->numBases < MAX_BASES; i++) {
            base src = bs->item[sectorBaseIdx + i];
            int rx = x1 + x2 - (int)src.x, ry = y1 + y2 - (int)src.y;
            if (rx != (int)src.x || ry != (int)src.y) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->x = (BYTE)rx; b->y = (BYTE)ry;
            }
        }
        for (int i = 0; i < sectorPills && pb->numPills < MAX_PILLS; i++) {
            pillbox src = pb->item[sectorPillIdx + i];
            int rx = x1 + x2 - (int)src.x, ry = y1 + y2 - (int)src.y;
            if (rx != (int)src.x || ry != (int)src.y) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->x = (BYTE)rx; p->y = (BYTE)ry;
            }
        }
        for (int i = 0; i < sectorStarts && ss->numStarts < MAX_STARTS; i++) {
            start src = ss->item[sectorStartIdx + i];
            int rx = x1 + x2 - (int)src.x, ry = y1 + y2 - (int)src.y;
            if (rx != (int)src.x || ry != (int)src.y) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->x = (BYTE)rx; s->y = (BYTE)ry;
                s->dir = tournDirRotate180(src.dir);
            }
        }
        break;

    case MAPGEN_SYM_ROTATE90: {
        int n = w;
        for (int i = 0; i < sectorBases; i++) {
            base src = bs->item[sectorBaseIdx + i];
            int rx = (int)src.x - x1, ry = (int)src.y - y1;
            if (bs->numBases < MAX_BASES) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->x = (BYTE)(x1 + ry); b->y = (BYTE)(y1 + n - 1 - rx);
            }
            if (bs->numBases < MAX_BASES) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->x = (BYTE)(x1 + n - 1 - rx); b->y = (BYTE)(y1 + n - 1 - ry);
            }
            if (bs->numBases < MAX_BASES) {
                base *b = &bs->item[bs->numBases++];
                *b = src; b->x = (BYTE)(x1 + n - 1 - ry); b->y = (BYTE)(y1 + rx);
            }
        }
        for (int i = 0; i < sectorPills; i++) {
            pillbox src = pb->item[sectorPillIdx + i];
            int rx = (int)src.x - x1, ry = (int)src.y - y1;
            if (pb->numPills < MAX_PILLS) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->x = (BYTE)(x1 + ry); p->y = (BYTE)(y1 + n - 1 - rx);
            }
            if (pb->numPills < MAX_PILLS) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->x = (BYTE)(x1 + n - 1 - rx); p->y = (BYTE)(y1 + n - 1 - ry);
            }
            if (pb->numPills < MAX_PILLS) {
                pillbox *p = &pb->item[pb->numPills++];
                *p = src; p->x = (BYTE)(x1 + n - 1 - ry); p->y = (BYTE)(y1 + rx);
            }
        }
        for (int i = 0; i < sectorStarts; i++) {
            start src = ss->item[sectorStartIdx + i];
            int rx = (int)src.x - x1, ry = (int)src.y - y1;
            if (ss->numStarts < MAX_STARTS) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->x = (BYTE)(x1 + ry); s->y = (BYTE)(y1 + n - 1 - rx);
                s->dir = (BYTE)((src.dir + 4) % 16);
            }
            if (ss->numStarts < MAX_STARTS) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->x = (BYTE)(x1 + n - 1 - rx); s->y = (BYTE)(y1 + n - 1 - ry);
                s->dir = tournDirRotate180(src.dir);
            }
            if (ss->numStarts < MAX_STARTS) {
                start *s = &ss->item[ss->numStarts++];
                *s = src; s->x = (BYTE)(x1 + n - 1 - ry); s->y = (BYTE)(y1 + rx);
                s->dir = (BYTE)((src.dir + 12) % 16);
            }
        }
        break;
    }
    }
}

/*=========================================================
 * Natural generator helpers
 *=========================================================*/

static bool natIsWater(struct mapObj *mp, int x, int y,
                       int bx1, int by1, int bx2, int by2) {
    if (x < bx1 || x > bx2 || y < by1 || y > by2) return true;
    BYTE t = mp->mapItem[x][y];
    return t == DEEP_SEA || t == RIVER;
}

static bool natAdjacentToWater(struct mapObj *mp, int x, int y,
                               int bx1, int by1, int bx2, int by2) {
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            if (natIsWater(mp, x + dx, y + dy, bx1, by1, bx2, by2))
                return true;
        }
    return false;
}

/* Check if a city footprint of given size fits on land at (cx,cy). */
static bool natCityFits(struct mapObj *mp, int cx, int cy, int size,
                        int bx1, int by1, int bx2, int by2) {
    int half = size / 2;
    for (int dx = -half; dx < size - half; dx++)
        for (int dy = -half; dy < size - half; dy++) {
            int x = cx + dx, y = cy + dy;
            if (x < bx1 || x > bx2 || y < by1 || y > by2) return false;
            BYTE t = mp->mapItem[x][y];
            if (t == DEEP_SEA || t == RIVER) return false;
        }
    /* Also check the HALFBUILDING perimeter ring fits in bounds */
    int ph = half + 1;
    int ps = size + 2;
    for (int dx = -ph; dx < ps - ph; dx++)
        for (int dy = -ph; dy < ps - ph; dy++) {
            int x = cx + dx, y = cy + dy;
            if (x < bx1 || x > bx2 || y < by1 || y > by2) return false;
        }
    return true;
}

/* Place a city with road grid at (cx,cy).
 * Returns the number of road exits on the perimeter (up to 4 sides).
 * exitX[]/exitY[] are filled with exit coordinates (max 4). */
static int natPlaceCity(struct mapObj *mp, int cx, int cy, int size,
                        int bx1, int by1, int bx2, int by2,
                        int exitX[4], int exitY[4]) {
    int half = size / 2;
    int ox = cx - half;  /* origin x of city footprint */
    int oy = cy - half;  /* origin y of city footprint */

    /* Step 1: Fill entire footprint with BUILDING */
    for (int dx = 0; dx < size; dx++)
        for (int dy = 0; dy < size; dy++) {
            int x = ox + dx, y = oy + dy;
            if (x >= bx1 && x <= bx2 && y >= by1 && y <= by2)
                mp->mapItem[x][y] = BUILDING;
        }

    /* Step 2: Carve road grid through the buildings.
     * For 4x4: roads at row 1, col 1 (0-indexed), forming a cross.
     * For 5x5: roads at row 2, col 2.
     * For 6x6: roads at rows 1,3 and cols 1,3.
     * For 7x7: roads at rows 2,4 and cols 2,4.
     * For 8x8: roads at rows 1,3,5 and cols 1,3,5.
     * General rule: place roads every 2 tiles starting from offset 1 (for even)
     * or 2 (for odd). */
    int roadRows[4], roadCols[4];
    int nRoadRows = 0, nRoadCols = 0;

    if (size <= 5) {
        /* Single cross through the middle */
        roadRows[nRoadRows++] = size / 2;
        roadCols[nRoadCols++] = size / 2;
    } else if (size <= 6) {
        /* Double grid */
        roadRows[nRoadRows++] = 1; roadRows[nRoadRows++] = size - 2;
        roadCols[nRoadCols++] = 1; roadCols[nRoadCols++] = size - 2;
    } else {
        /* Triple grid for 7-8 */
        roadRows[nRoadRows++] = 1;
        roadRows[nRoadRows++] = size / 2;
        roadRows[nRoadRows++] = size - 2;
        roadCols[nRoadCols++] = 1;
        roadCols[nRoadCols++] = size / 2;
        roadCols[nRoadCols++] = size - 2;
    }

    /* Carve horizontal roads (full width of city) */
    for (int r = 0; r < nRoadRows; r++)
        for (int dx = 0; dx < size; dx++) {
            int x = ox + dx, y = oy + roadRows[r];
            if (x >= bx1 && x <= bx2 && y >= by1 && y <= by2)
                mp->mapItem[x][y] = ROAD;
        }

    /* Carve vertical roads (full height of city) */
    for (int c = 0; c < nRoadCols; c++)
        for (int dy = 0; dy < size; dy++) {
            int x = ox + roadCols[c], y = oy + dy;
            if (x >= bx1 && x <= bx2 && y >= by1 && y <= by2)
                mp->mapItem[x][y] = ROAD;
        }

    /* Step 3: HALFBUILDING perimeter (1-tile ring outside the city).
     * Road exits cut through the perimeter (stay as ROAD). */
    for (int dx = -1; dx <= size; dx++)
        for (int dy = -1; dy <= size; dy++) {
            /* Skip interior */
            if (dx >= 0 && dx < size && dy >= 0 && dy < size) continue;
            int x = ox + dx, y = oy + dy;
            if (x < bx1 || x > bx2 || y < by1 || y > by2) continue;
            BYTE t = mp->mapItem[x][y];
            if (t == DEEP_SEA || t == RIVER || t == BUILDING) continue;
            mp->mapItem[x][y] = HALFBUILDING;
        }

    /* Step 4: Determine road exit points (where roads meet the city edge).
     * Pick one exit per side: the middle road row/col for that side. */
    int nExits = 0;

    /* North exit: first road column, row = -1 (just outside top edge) */
    if (nRoadCols > 0) {
        int col = roadCols[nRoadCols / 2];
        int ex = ox + col, ey = oy - 1;
        if (ex >= bx1 && ex <= bx2 && ey >= by1 && ey <= by2) {
            mp->mapItem[ex][ey] = ROAD;
            exitX[nExits] = ex; exitY[nExits] = ey; nExits++;
        }
    }
    /* South exit */
    if (nRoadCols > 0) {
        int col = roadCols[nRoadCols / 2];
        int ex = ox + col, ey = oy + size;
        if (ex >= bx1 && ex <= bx2 && ey >= by1 && ey <= by2) {
            mp->mapItem[ex][ey] = ROAD;
            exitX[nExits] = ex; exitY[nExits] = ey; nExits++;
        }
    }
    /* West exit */
    if (nRoadRows > 0) {
        int row = roadRows[nRoadRows / 2];
        int ex = ox - 1, ey = oy + row;
        if (ex >= bx1 && ex <= bx2 && ey >= by1 && ey <= by2) {
            mp->mapItem[ex][ey] = ROAD;
            exitX[nExits] = ex; exitY[nExits] = ey; nExits++;
        }
    }
    /* East exit */
    if (nRoadRows > 0) {
        int row = roadRows[nRoadRows / 2];
        int ex = ox + size, ey = oy + row;
        if (ex >= bx1 && ex <= bx2 && ey >= by1 && ey <= by2) {
            mp->mapItem[ex][ey] = ROAD;
            exitX[nExits] = ex; exitY[nExits] = ey; nExits++;
        }
    }

    return nExits;
}

/* L-shaped road that skips BUILDING, HALFBUILDING, RIVER, and DEEP_SEA. */
static void natDrawRoad(struct mapObj *mp, int fromX, int fromY, int toX, int toY,
                        int bx1, int by1, int bx2, int by2) {
    int x = fromX, y = fromY;
    int dx = (toX > x) ? 1 : (toX < x) ? -1 : 0;
    while (x != toX) {
        if (x >= bx1 && x <= bx2 && y >= by1 && y <= by2) {
            BYTE t = mp->mapItem[x][y];
            if (t != DEEP_SEA && t != BUILDING && t != HALFBUILDING && t != RIVER)
                mp->mapItem[x][y] = ROAD;
        }
        x += dx;
    }
    int dy = (toY > y) ? 1 : (toY < y) ? -1 : 0;
    while (y != toY) {
        if (x >= bx1 && x <= bx2 && y >= by1 && y <= by2) {
            BYTE t = mp->mapItem[x][y];
            if (t != DEEP_SEA && t != BUILDING && t != HALFBUILDING && t != RIVER)
                mp->mapItem[x][y] = ROAD;
        }
        y += dy;
    }
}

/*=========================================================
 * Natural map generator
 *=========================================================*/
static void mapGenNatural(struct mapObj *mp, struct basesObj *bs,
                          struct pillsObj *pb, struct startsObj *ss,
                          const MapGenConfig *cfg, uint32_t *rng) {
    const int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;
    const int totalTiles = w * h;
    const int style = cfg->params.natural.mapStyle;

    uint32_t nsSeed1 = xorshift32(rng);
    uint32_t nsSeed2 = xorshift32(rng);

    float centerX = (float)(x1 + x2) * 0.5f;
    float centerY = (float)(y1 + y2) * 0.5f;
    float maxDim = (float)(w > h ? w : h);

    /* Clear region to DEEP_SEA */
    for (int x = x1; x <= x2; x++)
        for (int y = y1; y <= y2; y++)
            mp->mapItem[x][y] = DEEP_SEA;

    float *heights = (float *)malloc(sizeof(float) * (size_t)totalTiles);
    if (!heights) return;

    /* ===== 1. Heightmap generation (style-dependent) ===== */

    if (style == MAPGEN_STYLE_INLAND) {
        /* All land — heightmap used only for terrain variation and rivers */
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++)
                heights[ix * h + iy] = fbmNoise(nsSeed1,
                    (float)(x1 + ix), (float)(y1 + iy), 32, 4);

    } else if (style == MAPGEN_STYLE_ISLANDS) {
        /* Multiple radial peaks at random offsets */
        int numIslands = 3 + (int)(xorshift32(rng) % 4);
        if (numIslands > 6) numIslands = 6;
        float iCX[6], iCY[6], iR[6];
        for (int i = 0; i < numIslands; i++) {
            iCX[i] = (float)x1 + (float)w * 0.15f +
                     (float)(xorshift32(rng) % ((uint32_t)(w * 7 / 10) + 1));
            iCY[i] = (float)y1 + (float)h * 0.15f +
                     (float)(xorshift32(rng) % ((uint32_t)(h * 7 / 10) + 1));
            iR[i] = maxDim * (0.06f + 0.08f *
                    (float)(xorshift32(rng) % 100) / 100.0f);
        }
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++) {
                float fx = (float)(x1 + ix), fy = (float)(y1 + iy);
                float n = fbmNoise(nsSeed1, fx, fy, 24, 4);
                float bestF = 0.0f;
                for (int i = 0; i < numIslands; i++) {
                    float ddx = fx - iCX[i], ddy = fy - iCY[i];
                    float d2 = ddx * ddx + ddy * ddy;
                    float r2 = iR[i] * iR[i];
                    if (r2 < 1.0f) r2 = 1.0f;
                    float f = 1.0f - d2 / r2;
                    if (f < 0.0f) f = 0.0f;
                    if (f > bestF) bestF = f;
                }
                heights[ix * h + iy] = n * bestF;
            }

    } else if (style == MAPGEN_STYLE_ARCHIPELAGO) {
        /* High-frequency noise, weak radial falloff → many small islands */
        float fR = maxDim * 0.55f;
        float fR2 = fR * fR;
        if (fR2 < 1.0f) fR2 = 1.0f;
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++) {
                float fx = (float)(x1 + ix), fy = (float)(y1 + iy);
                float n = fbmNoise(nsSeed1, fx, fy, 12, 6);
                float ddx = fx - centerX, ddy = fy - centerY;
                float d2 = ddx * ddx + ddy * ddy;
                float falloff = 1.0f - 0.3f * d2 / fR2;
                if (falloff < 0.0f) falloff = 0.0f;
                heights[ix * h + iy] = n * falloff;
            }

    } else {
        /* Ocean (default) or Continent */
        float fStr = (style == MAPGEN_STYLE_CONTINENT) ? 0.3f : 1.0f;
        float fRad = maxDim * ((style == MAPGEN_STYLE_CONTINENT) ? 0.6f : 0.4f);
        float fR2 = fRad * fRad;
        if (fR2 < 1.0f) fR2 = 1.0f;
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++) {
                float fx = (float)(x1 + ix), fy = (float)(y1 + iy);
                float n = fbmNoise(nsSeed1, fx, fy, 32, 4);
                float ddx = fx - centerX, ddy = fy - centerY;
                float d2 = ddx * ddx + ddy * ddy;
                float falloff = 1.0f - fStr * d2 / fR2;
                if (falloff < 0.0f) falloff = 0.0f;
                heights[ix * h + iy] = n * falloff;
            }
    }

    /* ===== 2. Land/sea threshold ===== */

    int landPct;
    switch (style) {
        case MAPGEN_STYLE_OCEAN:       landPct = 15; break;
        case MAPGEN_STYLE_CONTINENT:   landPct = 70; break;
        case MAPGEN_STYLE_ISLANDS:     landPct = 10; break;
        case MAPGEN_STYLE_ARCHIPELAGO: landPct = 35; break;
        case MAPGEN_STYLE_INLAND:      landPct = 100; break;
        default:                       landPct = 15; break;
    }

    int landCount = 0;
    if (style == MAPGEN_STYLE_INLAND) {
        for (int x = x1; x <= x2; x++)
            for (int y = y1; y <= y2; y++) {
                mp->mapItem[x][y] = GRASS;
                landCount++;
            }
    } else {
        float threshold = findLandThreshold(heights, totalTiles, landPct);
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++)
                if (heights[ix * h + iy] >= threshold) {
                    mp->mapItem[x1 + ix][y1 + iy] = GRASS;
                    landCount++;
                }
    }

    if (landCount == 0) { free(heights); return; }

    /* Style-dependent object spacing (squared distances for comparisons) */
    int baseSpacing2, pillSpacing2, startSpacing2;
    switch (style) {
        case MAPGEN_STYLE_CONTINENT:
            baseSpacing2 = 105 * 105; pillSpacing2 = 80 * 80; startSpacing2 = 60 * 60;
            break;
        case MAPGEN_STYLE_ARCHIPELAGO:
            baseSpacing2 = 60 * 60; pillSpacing2 = 40 * 40; startSpacing2 = 30 * 30;
            break;
        case MAPGEN_STYLE_ISLANDS:
            baseSpacing2 = 40 * 40; pillSpacing2 = 30 * 30; startSpacing2 = 20 * 20;
            break;
        case MAPGEN_STYLE_INLAND:
            baseSpacing2 = 50 * 50; pillSpacing2 = 40 * 40; startSpacing2 = 30 * 30;
            break;
        default: /* Ocean */
            baseSpacing2 = 50 * 50; pillSpacing2 = 35 * 35; startSpacing2 = 25 * 25;
            break;
    }

    /* ===== 3. Forest (noise-based clusters on GRASS) ===== */

    int targetForest = (landCount * cfg->params.natural.forestPct + 50) / 100;
    if (targetForest > 0) {
        int bins[256];
        memset(bins, 0, sizeof(bins));
        float *fNoise = (float *)malloc(sizeof(float) * (size_t)totalTiles);
        if (fNoise) {
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++) {
                    int idx = ix * h + iy;
                    if (mp->mapItem[x1 + ix][y1 + iy] == GRASS) {
                        float v = fbmNoise(nsSeed2, (float)(x1 + ix),
                                           (float)(y1 + iy), 16, 3);
                        fNoise[idx] = v;
                        int b = (int)(v * 255.0f);
                        if (b < 0) b = 0;
                        if (b > 255) b = 255;
                        bins[b]++;
                    } else {
                        fNoise[idx] = 0.0f;
                    }
                }
            /* Find threshold so top forestPct% of land is forest */
            int cumul = 0;
            float fThresh = 1.01f;
            for (int b = 255; b >= 0; b--) {
                cumul += bins[b];
                if (cumul >= targetForest) {
                    fThresh = (float)b / 255.0f;
                    break;
                }
            }
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++)
                    if (mp->mapItem[x1 + ix][y1 + iy] == GRASS &&
                        fNoise[ix * h + iy] >= fThresh)
                        mp->mapItem[x1 + ix][y1 + iy] = FOREST;
            free(fNoise);
        }
    }

    /* ===== 4. Rivers =====
     * Each river connects two coast points through the island interior.
     * Step 1: Build a list of coast-adjacent land tiles.
     * Step 2: Pick two coast tiles far apart, walk between them with meander.
     * riverCount controls main rivers, riverPct target adds more via fill.
     * Rivers never stop early at coast — they walk through interior only. */

    int riverCount = cfg->params.natural.riverCount;
    if (riverCount > 5) riverCount = 5;
    int rPct = cfg->params.natural.riverPct;

    static const int rdx[] = {0, 0, 1, -1};
    static const int rdy[] = {-1, 1, 0, 0};

    /* Build coast tile list: land tiles adjacent to DEEP_SEA or map edge */
    typedef struct { int x, y; } CoastTile;
    int coastCap = w * h;
    CoastTile *coastTiles = (CoastTile *)malloc(sizeof(CoastTile) * (size_t)coastCap);
    int nCoast = 0;
    if (coastTiles) {
        for (int ix = 0; ix < w; ix++) {
            for (int iy = 0; iy < h; iy++) {
                int px = x1 + ix, py = y1 + iy;
                if (mp->mapItem[px][py] == DEEP_SEA) continue;
                if (mp->mapItem[px][py] == BUILDING) continue;
                bool adjSea = false;
                for (int d = 0; d < 4; d++) {
                    int nx = px + rdx[d], ny = py + rdy[d];
                    if (nx < x1 || nx > x2 || ny < y1 || ny > y2)
                        { adjSea = true; break; }  /* map edge = coast */
                    if (mp->mapItem[nx][ny] == DEEP_SEA)
                        { adjSea = true; break; }
                }
                if (adjSea && nCoast < coastCap) {
                    coastTiles[nCoast].x = px;
                    coastTiles[nCoast].y = py;
                    nCoast++;
                }
            }
        }
    }

    /* Helper: carve a meandering river from (startX,startY) toward
     * (goalX,goalY), staying on land tiles. Returns tiles placed.
     * The walker avoids DEEP_SEA — it only walks on land, carving RIVER.
     * It terminates when it reaches a tile adjacent to the goal region
     * (coast/sea near the target) or runs out of steps. */
    #define CARVE_RIVER(startX, startY, goalX, goalY, maxSteps, rngPtr) \
        do { \
            int _cx = (startX), _cy = (startY); \
            for (int _s = 0; _s < (maxSteps); _s++) { \
                if (_cx < x1 || _cx > x2 || _cy < y1 || _cy > y2) break; \
                if (mp->mapItem[_cx][_cy] == DEEP_SEA) break; \
                mp->mapItem[_cx][_cy] = RIVER; \
                /* Check if close enough to goal (within 2 tiles) */ \
                int _gdx = abs(_cx - (goalX)), _gdy = abs(_cy - (goalY)); \
                if (_gdx + _gdy <= 2) break; \
                float _dirX = (float)((goalX) - _cx); \
                float _dirY = (float)((goalY) - _cy); \
                float _dirLen = sqrtf(_dirX * _dirX + _dirY * _dirY); \
                if (_dirLen < 1.0f) break; \
                _dirX /= _dirLen; _dirY /= _dirLen; \
                int _bestD = -1; float _bestSc = -999.0f; \
                for (int _d = 0; _d < 4; _d++) { \
                    int _nx = _cx + rdx[_d], _ny = _cy + rdy[_d]; \
                    if (_nx < x1 || _nx > x2 || _ny < y1 || _ny > y2) continue; \
                    if (mp->mapItem[_nx][_ny] == DEEP_SEA) continue; \
                    if (mp->mapItem[_nx][_ny] == BUILDING) continue; \
                    float _tw = (float)rdx[_d] * _dirX + (float)rdy[_d] * _dirY; \
                    float _ns = (float)(xorshift32(rngPtr) % 100) / 100.0f; \
                    float _sc = _tw * 0.5f + _ns * 0.7f; \
                    if (_sc > _bestSc) { _bestSc = _sc; _bestD = _d; } \
                } \
                if (_bestD < 0) break; \
                _cx += rdx[_bestD]; _cy += rdy[_bestD]; \
            } \
        } while(0)

    /* Section 4a: Main rivers — pick two distant coast points, connect them */
    if (nCoast >= 2) {
        for (int r = 0; r < riverCount; r++) {
            /* Pick first coast point randomly */
            int idx1 = (int)(xorshift32(rng) % (uint32_t)nCoast);
            int ax = coastTiles[idx1].x, ay = coastTiles[idx1].y;

            /* Pick second coast point: sample 20 and take the farthest from first */
            int bx = ax, by = ay;
            int bestDist = 0;
            for (int att = 0; att < 20; att++) {
                int idx2 = (int)(xorshift32(rng) % (uint32_t)nCoast);
                int dx = coastTiles[idx2].x - ax;
                int dy = coastTiles[idx2].y - ay;
                int dist = dx * dx + dy * dy;
                if (dist > bestDist) {
                    bestDist = dist;
                    bx = coastTiles[idx2].x;
                    by = coastTiles[idx2].y;
                }
            }

            /* Walk from A toward B through interior */
            int maxLen = (w + h) * 2;
            CARVE_RIVER(ax, ay, bx, by, maxLen, rng);
        }
    }

    /* Section 4b: Target fill — keep spawning coast-to-coast rivers
     * until riverPct of land tiles are RIVER. */
    if (nCoast >= 2) {
        int targetRiver = (landCount * rPct + 50) / 100;
        int currentRiver = 0;
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++)
                if (mp->mapItem[x1 + ix][y1 + iy] == RIVER)
                    currentRiver++;

        int failStreak = 0;
        for (int round = 0; round < 1000 && currentRiver < targetRiver && failStreak < 50; round++) {
            /* Pick a random coast point as start */
            int idx1 = (int)(xorshift32(rng) % (uint32_t)nCoast);
            int ax = coastTiles[idx1].x, ay = coastTiles[idx1].y;

            /* Pick end: either a distant coast point or an existing river
             * tile in the interior (to create tributaries). */
            int bx = ax, by = ay;
            bool useRiverTarget = (xorshift32(rng) % 3) == 0;  /* 33% chance tributary */

            if (useRiverTarget) {
                /* Find a random RIVER tile to target (creates tributary) */
                bool found = false;
                for (int att = 0; att < 30; att++) {
                    int tx = x1 + (int)(xorshift32(rng) % (uint32_t)w);
                    int ty = y1 + (int)(xorshift32(rng) % (uint32_t)h);
                    if (mp->mapItem[tx][ty] == RIVER) {
                        int dx = tx - ax, dy = ty - ay;
                        if (dx * dx + dy * dy > 16) {  /* at least 4 tiles away */
                            bx = tx; by = ty; found = true; break;
                        }
                    }
                }
                if (!found) useRiverTarget = false;
            }

            if (!useRiverTarget) {
                /* Pick distant coast point */
                int bestDist = 0;
                for (int att = 0; att < 15; att++) {
                    int idx2 = (int)(xorshift32(rng) % (uint32_t)nCoast);
                    int dx = coastTiles[idx2].x - ax;
                    int dy = coastTiles[idx2].y - ay;
                    int dist = dx * dx + dy * dy;
                    if (dist > bestDist) {
                        bestDist = dist;
                        bx = coastTiles[idx2].x;
                        by = coastTiles[idx2].y;
                    }
                }
            }

            /* Count river tiles before carving */
            int before = currentRiver;

            int maxLen = w + h;
            CARVE_RIVER(ax, ay, bx, by, maxLen, rng);

            /* Recount */
            currentRiver = 0;
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++)
                    if (mp->mapItem[x1 + ix][y1 + iy] == RIVER)
                        currentRiver++;

            failStreak = (currentRiver > before) ? 0 : failStreak + 1;
        }
    }

    #undef CARVE_RIVER
    free(coastTiles);

    /* ===== 4d. Boat tiles (water tiles adjacent to land) ===== */
    {
        int boatPct = cfg->params.natural.boatPct;
        if (boatPct > 0) {
            static const int bdx[] = {0, 0, 1, -1};
            static const int bdy[] = {-1, 1, 0, 0};
            for (int ix = 0; ix < w; ix++) {
                for (int iy = 0; iy < h; iy++) {
                    int x = x1 + ix, y = y1 + iy;
                    BYTE cur = mp->mapItem[x][y];
                    if (cur != RIVER && cur != DEEP_SEA) continue;
                    bool adjLand = false;
                    for (int d = 0; d < 4; d++) {
                        int nx = x + bdx[d], ny = y + bdy[d];
                        if (nx < x1 || nx > x2 || ny < y1 || ny > y2) continue;
                        BYTE t = mp->mapItem[nx][ny];
                        if (t != DEEP_SEA && t != RIVER && t != BOAT)
                            { adjLand = true; break; }
                    }
                    if (adjLand && (int)(xorshift32(rng) % 100) < boatPct)
                        mp->mapItem[x][y] = BOAT;
                }
            }
        }
    }

    /* ===== 5. Cities (building clusters with road grids) ===== */

    int cityCount = cfg->params.natural.cityCount;
    if (cityCount > 10) cityCount = 10;
    GenPos cityPos[10];
    int cityExitX[10][4], cityExitY[10][4], cityExitCount[10];
    int citiesPlaced = 0;

    /* City size depends on how many cities we're placing */
    int citySizeMin, citySizeMax;
    if (cityCount <= 3)      { citySizeMin = 6; citySizeMax = 8; }
    else if (cityCount <= 6) { citySizeMin = 5; citySizeMax = 6; }
    else                     { citySizeMin = 4; citySizeMax = 5; }

    for (int c = 0; c < cityCount; c++) {
        int bestX = -1, bestY = -1;
        int citySize = citySizeMin + (int)(xorshift32(rng) % (uint32_t)(citySizeMax - citySizeMin + 1));
        for (int att = 0; att < 500; att++) {
            int tx = x1 + (int)(xorshift32(rng) % (uint32_t)w);
            int ty = y1 + (int)(xorshift32(rng) % (uint32_t)h);
            BYTE t = mp->mapItem[tx][ty];
            if (t == DEEP_SEA || t == RIVER || t == BUILDING) continue;
            /* Check city footprint fits on land */
            if (!natCityFits(mp, tx, ty, citySize, x1, y1, x2, y2)) continue;
            /* Prefer locations away from water (relax after 300 attempts) */
            if (att < 300) {
                bool nearW = false;
                for (int ddx = -3; ddx <= 3 && !nearW; ddx++)
                    for (int ddy = -3; ddy <= 3 && !nearW; ddy++)
                        if (natIsWater(mp, tx + ddx, ty + ddy, x1, y1, x2, y2))
                            nearW = true;
                if (nearW) continue;
            }
            /* Enforce minimum distance between cities */
            bool tooNear = false;
            for (int i = 0; i < citiesPlaced; i++) {
                int ddx = cityPos[i].x - tx, ddy = cityPos[i].y - ty;
                if (ddx * ddx + ddy * ddy < 15 * 15) { tooNear = true; break; }
            }
            if (tooNear) continue;
            bestX = tx; bestY = ty;
            break;
        }
        if (bestX >= 0) {
            int nExits = natPlaceCity(mp, bestX, bestY, citySize, x1, y1, x2, y2,
                                      cityExitX[citiesPlaced], cityExitY[citiesPlaced]);
            cityExitCount[citiesPlaced] = nExits;
            cityPos[citiesPlaced++] = (GenPos){bestX, bestY};

            /* Place a base at the center road intersection if bases remain */
            if (cfg->bases > 0 && bs->numBases < MAX_BASES &&
                (int)bs->numBases < cfg->bases) {
                base *bp = &bs->item[bs->numBases];
                bp->x = (BYTE)bestX; bp->y = (BYTE)bestY;
                bp->owner = 0xFF; bp->armour = 90;
                bp->shells = 90; bp->mines = 90;
                bp->refuelTime = 0; bp->baseTime = 0;
                bp->justStopped = false;
                bs->numBases++;
            }

            /* Place pillboxes at city exits (1 tile outside exit, on the road) */
            for (int e = 0; e < nExits && pb->numPills < MAX_PILLS &&
                     cfg->pills > 0 && (int)pb->numPills < cfg->pills; e++) {
                /* Step 1 more tile outward from the exit */
                int px = cityExitX[citiesPlaced - 1][e];
                int py = cityExitY[citiesPlaced - 1][e];
                int dirX = px - bestX, dirY = py - bestY;
                /* Normalize to unit step */
                if (dirX != 0) dirX = dirX > 0 ? 1 : -1;
                if (dirY != 0) dirY = dirY > 0 ? 1 : -1;
                int pillX = px + dirX, pillY = py + dirY;
                if (pillX < x1 || pillX > x2 || pillY < y1 || pillY > y2) continue;
                BYTE pt = mp->mapItem[pillX][pillY];
                if (pt == DEEP_SEA || pt == RIVER || pt == BUILDING) continue;
                /* Check not overlapping an existing base or pill */
                bool overlap = false;
                for (int i = 0; i < (int)bs->numBases; i++)
                    if (bs->item[i].x == (BYTE)pillX && bs->item[i].y == (BYTE)pillY)
                        { overlap = true; break; }
                for (int i = 0; i < (int)pb->numPills && !overlap; i++)
                    if (pb->item[i].x == (BYTE)pillX && pb->item[i].y == (BYTE)pillY)
                        { overlap = true; break; }
                if (overlap) continue;
                pillbox *p = &pb->item[pb->numPills];
                p->x = (BYTE)pillX; p->y = (BYTE)pillY;
                p->owner = 0xFF; p->armour = 15; p->speed = 50;
                p->inTank = false; p->reload = 0;
                p->coolDown = 0; p->justSeen = false;
                pb->numPills++;
            }
        }
    }

    /* ===== 5b. Additional buildings from buildingPct (noise-based) ===== */

    {
        int targetBuilding = (landCount * cfg->params.natural.buildingPct + 50) / 100;
        /* Count buildings already placed by cities */
        int existingBuilding = 0;
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++)
                if (mp->mapItem[x1 + ix][y1 + iy] == BUILDING ||
                    mp->mapItem[x1 + ix][y1 + iy] == HALFBUILDING)
                    existingBuilding++;
        int remaining = targetBuilding - existingBuilding;
        if (remaining > 0) {
            GenPos *bCands = (GenPos *)malloc(sizeof(GenPos) * (size_t)totalTiles);
            if (bCands) {
                int nCand = 0;
                for (int ix = 0; ix < w; ix++)
                    for (int iy = 0; iy < h; iy++) {
                        BYTE t = mp->mapItem[x1 + ix][y1 + iy];
                        if (t == GRASS || t == FOREST)
                            bCands[nCand++] = (GenPos){x1 + ix, y1 + iy};
                    }
                /* Shuffle and take up to remaining */
                for (int i = nCand - 1; i > 0; i--) {
                    int j = (int)(xorshift32(rng) % (uint32_t)(i + 1));
                    GenPos tmp = bCands[i];
                    bCands[i] = bCands[j];
                    bCands[j] = tmp;
                }
                int toPlace = remaining < nCand ? remaining : nCand;
                for (int i = 0; i < toPlace; i++)
                    mp->mapItem[bCands[i].x][bCands[i].y] = BUILDING;
                free(bCands);
            }
        }
    }

    /* ===== 5c. Mazes (small maze structures on land) ===== */

    int mazeCount = cfg->params.natural.mazeCount;
    if (mazeCount > 5) mazeCount = 5;
    /* Track maze positions and entry points for road connections */
    GenPos mazePos[5];
    int mazeEntryX[5][8], mazeEntryY[5][8], mazeEntryCount[5];
    int mazesPlaced = 0;

    for (int mz = 0; mz < mazeCount; mz++) {
        /* Random maze size: 10-25 tiles on a side, varied by RNG */
        int mazeSize = 10 + (int)(xorshift32(rng) % 16);
        if (mazeSize > w - 2) mazeSize = w - 2;
        if (mazeSize > h - 2) mazeSize = h - 2;
        if (mazeSize < 6) continue; /* Region too small for a maze */
        int halfSz = mazeSize / 2;

        /* Find a land location where the maze footprint fits */
        int bestX = -1, bestY = -1;
        int rangeW = w - mazeSize;
        int rangeH = h - mazeSize;
        if (rangeW < 1) rangeW = 1;
        if (rangeH < 1) rangeH = 1;
        for (int att = 0; att < 500; att++) {
            int tx = x1 + halfSz + (int)(xorshift32(rng) % (uint32_t)rangeW);
            int ty = y1 + halfSz + (int)(xorshift32(rng) % (uint32_t)rangeH);

            /* Check the footprint is all land */
            bool fits = true;
            for (int dx = -halfSz; dx < mazeSize - halfSz && fits; dx++)
                for (int dy = -halfSz; dy < mazeSize - halfSz && fits; dy++) {
                    int fx = tx + dx, fy = ty + dy;
                    if (fx < x1 || fx > x2 || fy < y1 || fy > y2) { fits = false; break; }
                    BYTE t = mp->mapItem[fx][fy];
                    if (t == DEEP_SEA || t == RIVER) fits = false;
                }
            if (!fits) continue;

            /* Enforce minimum distance between mazes and cities */
            bool tooNear = false;
            for (int i = 0; i < mazesPlaced; i++) {
                int ddx = mazePos[i].x - tx, ddy = mazePos[i].y - ty;
                if (ddx * ddx + ddy * ddy < 20 * 20) { tooNear = true; break; }
            }
            for (int i = 0; i < citiesPlaced && !tooNear; i++) {
                int ddx = cityPos[i].x - tx, ddy = cityPos[i].y - ty;
                if (ddx * ddx + ddy * ddy < 12 * 12) { tooNear = true; break; }
            }
            if (tooNear) continue;

            bestX = tx; bestY = ty;
            break;
        }
        if (bestX < 0) continue;

        /* Generate the maze into the map */
        int mzX1 = bestX - halfSz;
        int mzY1 = bestY - halfSz;
        int mzX2 = mzX1 + mazeSize - 1;
        int mzY2 = mzY1 + mazeSize - 1;

        MazeConfig mc = mazeDefaultConfig();
        mc.seed = xorshift32(rng);
        if (mc.seed == 0) mc.seed = 1;
        mc.algo = (int)(xorshift32(rng) % MAZE_ALGO_COUNT);
        mc.entries = 2 + (int)(xorshift32(rng) % 3); /* 2-4 entries */
        mc.cityRooms = (int)(xorshift32(rng) % 2);   /* 0-1 rooms */

        mazeGenerate(mzX1, mzY1, mzX2, mzY2, mp->mapItem, &mc);

        /* Record maze position */
        mazePos[mazesPlaced] = (GenPos){bestX, bestY};

        /* Find the entry points: ROAD tiles on the maze perimeter */
        int nEntries = 0;
        for (int ex = mzX1; ex <= mzX2 && nEntries < 8; ex++) {
            if (ex >= 0 && ex < 256) {
                if (mzY1 >= 0 && mzY1 < 256 && mp->mapItem[ex][mzY1] == ROAD) {
                    mazeEntryX[mazesPlaced][nEntries] = ex;
                    mazeEntryY[mazesPlaced][nEntries] = mzY1;
                    nEntries++;
                }
                if (mzY2 >= 0 && mzY2 < 256 && mp->mapItem[ex][mzY2] == ROAD && nEntries < 8) {
                    mazeEntryX[mazesPlaced][nEntries] = ex;
                    mazeEntryY[mazesPlaced][nEntries] = mzY2;
                    nEntries++;
                }
            }
        }
        for (int ey = mzY1 + 1; ey < mzY2 && nEntries < 8; ey++) {
            if (ey >= 0 && ey < 256) {
                if (mzX1 >= 0 && mzX1 < 256 && mp->mapItem[mzX1][ey] == ROAD) {
                    mazeEntryX[mazesPlaced][nEntries] = mzX1;
                    mazeEntryY[mazesPlaced][nEntries] = ey;
                    nEntries++;
                }
                if (mzX2 >= 0 && mzX2 < 256 && mp->mapItem[mzX2][ey] == ROAD && nEntries < 8) {
                    mazeEntryX[mazesPlaced][nEntries] = mzX2;
                    mazeEntryY[mazesPlaced][nEntries] = ey;
                    nEntries++;
                }
            }
        }
        mazeEntryCount[mazesPlaced] = nEntries;
        mazesPlaced++;
    }

    /* ===== 6. Swamp (seed at water edges, then grow inland) ===== */

    int targetSwamp = (landCount * cfg->params.natural.swampPct + 50) / 100;
    if (targetSwamp > 0) {
        static const int sdx[] = {0, 0, 1, -1};
        static const int sdy[] = {-1, 1, 0, 0};
        GenPos *swCands = (GenPos *)malloc(sizeof(GenPos) * (size_t)totalTiles);
        int placed = 0;

        if (swCands) {
            /* Pass 1: seed swamp at water edges */
            int nCand = 0;
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++) {
                    int x = x1 + ix, y = y1 + iy;
                    BYTE t = mp->mapItem[x][y];
                    if (t != GRASS && t != FOREST) continue;
                    if (natAdjacentToWater(mp, x, y, x1, y1, x2, y2))
                        swCands[nCand++] = (GenPos){x, y};
                }
            for (int i = nCand - 1; i > 0; i--) {
                int j = (int)(xorshift32(rng) % (uint32_t)(i + 1));
                GenPos tmp = swCands[i];
                swCands[i] = swCands[j];
                swCands[j] = tmp;
            }
            int toPlace = targetSwamp < nCand ? targetSwamp : nCand;
            for (int i = 0; i < toPlace; i++)
                mp->mapItem[swCands[i].x][swCands[i].y] = SWAMP;
            placed = toPlace;

            /* Pass 2: grow swamp inland from existing swamp tiles */
            int maxRounds = 30;
            for (int round = 0; round < maxRounds && placed < targetSwamp; round++) {
                int nGrow = 0;
                for (int ix = 0; ix < w; ix++)
                    for (int iy = 0; iy < h; iy++) {
                        int x = x1 + ix, y = y1 + iy;
                        BYTE t = mp->mapItem[x][y];
                        if (t != GRASS && t != FOREST) continue;
                        bool adjSwamp = false;
                        for (int d = 0; d < 4; d++) {
                            int nx = x + sdx[d], ny = y + sdy[d];
                            if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                                mp->mapItem[nx][ny] == SWAMP)
                                { adjSwamp = true; break; }
                        }
                        if (adjSwamp) swCands[nGrow++] = (GenPos){x, y};
                    }
                if (nGrow == 0) break;
                for (int i = nGrow - 1; i > 0; i--) {
                    int j = (int)(xorshift32(rng) % (uint32_t)(i + 1));
                    GenPos tmp = swCands[i];
                    swCands[i] = swCands[j];
                    swCands[j] = tmp;
                }
                int toGrow = (targetSwamp - placed);
                if (toGrow > nGrow) toGrow = nGrow;
                for (int i = 0; i < toGrow; i++) {
                    mp->mapItem[swCands[i].x][swCands[i].y] = SWAMP;
                    placed++;
                }
            }
            free(swCands);
        }
    }

    /* ===== 7. Crater and Rubble (noise-based, very sparse) ===== */

    {
        uint32_t craterSeed = xorshift32(rng);
        int craterTarget = landCount / 200;
        int rubbleTarget = landCount / 300;
        if (craterTarget < 1) craterTarget = 1;
        if (craterTarget > 15) craterTarget = 15;
        if (rubbleTarget < 1) rubbleTarget = 1;
        if (rubbleTarget > 10) rubbleTarget = 10;

        /* Use noise threshold to place craters and rubble on GRASS tiles */
        int bins[256];
        memset(bins, 0, sizeof(bins));
        float *cNoise = (float *)malloc(sizeof(float) * (size_t)totalTiles);
        if (cNoise) {
            int grassCount = 0;
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++) {
                    int idx = ix * h + iy;
                    if (mp->mapItem[x1 + ix][y1 + iy] == GRASS) {
                        float v = fbmNoise(craterSeed, (float)(x1 + ix),
                                           (float)(y1 + iy), 8, 2);
                        cNoise[idx] = v;
                        int b = (int)(v * 255.0f);
                        if (b < 0) b = 0;
                        if (b > 255) b = 255;
                        bins[b]++;
                        grassCount++;
                    } else {
                        cNoise[idx] = -1.0f;
                    }
                }

            /* Find threshold for crater (top craterTarget tiles) */
            int combined = craterTarget + rubbleTarget;
            if (combined > grassCount) combined = grassCount;
            int cumul = 0;
            float cThresh = 1.01f;
            for (int b = 255; b >= 0; b--) {
                cumul += bins[b];
                if (cumul >= combined) {
                    cThresh = (float)b / 255.0f;
                    break;
                }
            }
            /* Also find a higher threshold for rubble (placed at highest noise values) */
            cumul = 0;
            float rThresh = 1.01f;
            for (int b = 255; b >= 0; b--) {
                cumul += bins[b];
                if (cumul >= rubbleTarget) {
                    rThresh = (float)b / 255.0f;
                    break;
                }
            }

            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++) {
                    float v = cNoise[ix * h + iy];
                    if (v < cThresh) continue;
                    if (mp->mapItem[x1 + ix][y1 + iy] != GRASS) continue;
                    if (v >= rThresh)
                        mp->mapItem[x1 + ix][y1 + iy] = RUBBLE;
                    else
                        mp->mapItem[x1 + ix][y1 + iy] = CRATER;
                }
            free(cNoise);
        }
    }

    /* ===== 8. Bases (greedy spread on traversable land) ===== */
    /* baseStart tracks where non-city bases begin (for road connections).
     * baseSpaStart includes city bases (for spacing checks). */
    int baseSpaStart = (int)bs->numBases - citiesPlaced; /* city bases start here */
    if (baseSpaStart < 0) baseSpaStart = 0;
    int baseStart = (int)bs->numBases;
    {
        int numBases = cfg->bases;
        if ((int)bs->numBases + numBases > MAX_BASES)
            numBases = MAX_BASES - (int)bs->numBases;

        GenPos *cands = (GenPos *)malloc(sizeof(GenPos) * (size_t)totalTiles);
        if (cands && numBases > 0) {
            int nCand = 0;
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++) {
                    BYTE t = mp->mapItem[x1 + ix][y1 + iy];
                    if (t == GRASS || t == ROAD || t == SWAMP || t == CRATER ||
                        t == RUBBLE || t == FOREST || t == HALFBUILDING)
                        cands[nCand++] = (GenPos){x1 + ix, y1 + iy};
                }
            /* Greedy: first base random, then maximize min distance.
             * Enforce minimum spacing from baseSpacing2, relaxing if
             * no candidate meets the threshold after many attempts.
             * Check against ALL bases including city-placed ones. */
            int baseMinD2 = baseSpacing2;
            for (int b = 0; b < numBases && nCand > 0; b++) {
                int bestIdx = -1;
                if (b == 0 && baseSpaStart == baseStart) {
                    /* No city bases exist, place first one randomly */
                    bestIdx = (int)(xorshift32(rng) % (uint32_t)nCand);
                } else {
                    int bestMinD2 = -1;
                    int samples = nCand < 200 ? nCand : 200;
                    for (int s = 0; s < samples; s++) {
                        int ci = (int)(xorshift32(rng) % (uint32_t)nCand);
                        int minD2 = 999999;
                        for (int i = baseSpaStart; i < (int)bs->numBases; i++) {
                            int ddx = cands[ci].x - (int)bs->item[i].x;
                            int ddy = cands[ci].y - (int)bs->item[i].y;
                            int d2 = ddx * ddx + ddy * ddy;
                            if (d2 < minD2) minD2 = d2;
                        }
                        if (minD2 > bestMinD2) {
                            bestMinD2 = minD2; bestIdx = ci;
                        }
                    }
                    /* If best candidate is too close, relax spacing by half */
                    if (bestMinD2 < baseMinD2) {
                        baseMinD2 /= 2;
                        if (bestMinD2 < baseMinD2) bestIdx = -1;
                    }
                }
                if (bestIdx >= 0 && bs->numBases < MAX_BASES) {
                    base *bp = &bs->item[bs->numBases];
                    bp->x = (BYTE)cands[bestIdx].x;
                    bp->y = (BYTE)cands[bestIdx].y;
                    bp->owner = 0xFF; bp->armour = 90;
                    bp->shells = 90; bp->mines = 90;
                    bp->refuelTime = 0; bp->baseTime = 0;
                    bp->justStopped = false;
                    bs->numBases++;
                    cands[bestIdx] = cands[--nCand];
                }
            }
        }
        if (cands) free(cands);
    }

    /* ===== 9. Roads (connect city exits, maze entries, and bases) ===== */

    /* Helper: find the nearest city or maze exit to a given point */
    #define NEAREST_EXIT(px, py, outX, outY) do { \
        int _bestD = 999999; outX = px; outY = py; \
        for (int _c = 0; _c < citiesPlaced; _c++) \
            for (int _e = 0; _e < cityExitCount[_c]; _e++) { \
                int _dx = cityExitX[_c][_e] - (px); \
                int _dy = cityExitY[_c][_e] - (py); \
                int _d2 = _dx*_dx + _dy*_dy; \
                if (_d2 < _bestD) { _bestD = _d2; outX = cityExitX[_c][_e]; outY = cityExitY[_c][_e]; } \
            } \
        for (int _m = 0; _m < mazesPlaced; _m++) \
            for (int _e = 0; _e < mazeEntryCount[_m]; _e++) { \
                int _dx = mazeEntryX[_m][_e] - (px); \
                int _dy = mazeEntryY[_m][_e] - (py); \
                int _d2 = _dx*_dx + _dy*_dy; \
                if (_d2 < _bestD) { _bestD = _d2; outX = mazeEntryX[_m][_e]; outY = mazeEntryY[_m][_e]; } \
            } \
    } while(0)

    /* Connect consecutive cities via their nearest exits */
    for (int i = 0; i + 1 < citiesPlaced; i++) {
        /* Find exit of city i nearest to city i+1 */
        int fromX = cityPos[i].x, fromY = cityPos[i].y;
        int bestFromE = 0, bestFromD = 999999;
        for (int e = 0; e < cityExitCount[i]; e++) {
            int dx = cityExitX[i][e] - cityPos[i + 1].x;
            int dy = cityExitY[i][e] - cityPos[i + 1].y;
            int d2 = dx * dx + dy * dy;
            if (d2 < bestFromD) { bestFromD = d2; bestFromE = e; }
        }
        if (cityExitCount[i] > 0) { fromX = cityExitX[i][bestFromE]; fromY = cityExitY[i][bestFromE]; }

        /* Find exit of city i+1 nearest to city i */
        int toX = cityPos[i + 1].x, toY = cityPos[i + 1].y;
        int bestToE = 0, bestToD = 999999;
        for (int e = 0; e < cityExitCount[i + 1]; e++) {
            int dx = cityExitX[i + 1][e] - cityPos[i].x;
            int dy = cityExitY[i + 1][e] - cityPos[i].y;
            int d2 = dx * dx + dy * dy;
            if (d2 < bestToD) { bestToD = d2; bestToE = e; }
        }
        if (cityExitCount[i + 1] > 0) { toX = cityExitX[i + 1][bestToE]; toY = cityExitY[i + 1][bestToE]; }

        natDrawRoad(mp, fromX, fromY, toX, toY, x1, y1, x2, y2);
    }

    /* Connect each maze entry to the nearest city exit or other maze entry */
    for (int mz = 0; mz < mazesPlaced; mz++) {
        for (int e = 0; e < mazeEntryCount[mz]; e++) {
            int ex = mazeEntryX[mz][e];
            int ey = mazeEntryY[mz][e];
            /* Find the nearest exit that isn't from this same maze */
            int nearX = ex, nearY = ey;
            int bestD = 999999;
            for (int c = 0; c < citiesPlaced; c++)
                for (int ce = 0; ce < cityExitCount[c]; ce++) {
                    int ddx = cityExitX[c][ce] - ex;
                    int ddy = cityExitY[c][ce] - ey;
                    int d2 = ddx * ddx + ddy * ddy;
                    if (d2 < bestD) { bestD = d2; nearX = cityExitX[c][ce]; nearY = cityExitY[c][ce]; }
                }
            for (int om = 0; om < mazesPlaced; om++) {
                if (om == mz) continue;
                for (int oe = 0; oe < mazeEntryCount[om]; oe++) {
                    int ddx = mazeEntryX[om][oe] - ex;
                    int ddy = mazeEntryY[om][oe] - ey;
                    int d2 = ddx * ddx + ddy * ddy;
                    if (d2 < bestD) { bestD = d2; nearX = mazeEntryX[om][oe]; nearY = mazeEntryY[om][oe]; }
                }
            }
            if (nearX != ex || nearY != ey) {
                natDrawRoad(mp, ex, ey, nearX, nearY, x1, y1, x2, y2);
            }
        }
    }

    /* Connect non-city bases to the nearest city/maze exit */
    for (int i = baseStart; i < (int)bs->numBases; i++) {
        /* Skip bases that are inside a city (placed by city generation) */
        bool isCity = false;
        for (int c = 0; c < citiesPlaced; c++)
            if (bs->item[i].x == (BYTE)cityPos[c].x && bs->item[i].y == (BYTE)cityPos[c].y)
                { isCity = true; break; }
        if (isCity) continue;

        int nearExitX, nearExitY;
        NEAREST_EXIT((int)bs->item[i].x, (int)bs->item[i].y, nearExitX, nearExitY);
        natDrawRoad(mp, bs->item[i].x, bs->item[i].y, nearExitX, nearExitY, x1, y1, x2, y2);
    }

    #undef NEAREST_EXIT

    /* ===== 10. Pillboxes (prefer near water/choke points) ===== */

    {
        int numPills = cfg->pills;
        if ((int)pb->numPills + numPills > MAX_PILLS)
            numPills = MAX_PILLS - (int)pb->numPills;

        GenPos *cands = (GenPos *)malloc(sizeof(GenPos) * (size_t)totalTiles);
        if (cands && numPills > 0) {
            /* Preferred: land tiles adjacent to water */
            int nPref = 0;
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++) {
                    int x = x1 + ix, y = y1 + iy;
                    BYTE t = mp->mapItem[x][y];
                    if (t == DEEP_SEA || t == RIVER || t == BUILDING) continue;
                    if (natAdjacentToWater(mp, x, y, x1, y1, x2, y2))
                        cands[nPref++] = (GenPos){x, y};
                }
            /* Fallback: all remaining land tiles */
            int nAll = nPref;
            if (nPref < numPills) {
                for (int ix = 0; ix < w; ix++)
                    for (int iy = 0; iy < h; iy++) {
                        int x = x1 + ix, y = y1 + iy;
                        BYTE t = mp->mapItem[x][y];
                        if (t == DEEP_SEA || t == RIVER || t == BUILDING) continue;
                        if (!natAdjacentToWater(mp, x, y, x1, y1, x2, y2))
                            cands[nAll++] = (GenPos){x, y};
                    }
            }

            int pillStart = (int)pb->numPills;
            int placed = 0;
            for (int att = 0; att < 2000 && placed < numPills; att++) {
                int pool = (placed < nPref && nPref > 0) ? nPref : nAll;
                if (pool == 0) break;
                int ci = (int)(xorshift32(rng) % (uint32_t)pool);
                int px = cands[ci].x, py = cands[ci].y;
                /* Enforce spacing between pills and no overlap with bases */
                bool ok = true;
                for (int i = pillStart; i < (int)pb->numPills; i++) {
                    int ddx = (int)pb->item[i].x - px;
                    int ddy = (int)pb->item[i].y - py;
                    if (ddx * ddx + ddy * ddy < pillSpacing2) { ok = false; break; }
                }
                for (int i = 0; i < (int)bs->numBases && ok; i++)
                    if (bs->item[i].x == (BYTE)px && bs->item[i].y == (BYTE)py)
                        ok = false;
                if (ok && pb->numPills < MAX_PILLS) {
                    pillbox *p = &pb->item[pb->numPills];
                    p->x = (BYTE)px; p->y = (BYTE)py;
                    p->owner = 0xFF; p->armour = 15; p->speed = 50;
                    p->inTank = false; p->reload = 0;
                    p->coolDown = 0; p->justSeen = false;
                    pb->numPills++;
                    placed++;
                }
            }
        }
        if (cands) free(cands);
    }

    /* ===== 11. Starts (deep sea near coast) ===== */

    /* Inland maps have no deep sea — carve small pools of DEEP_SEA along
     * rivers so starts can be placed on valid DEEP_SEA tiles. */
    if (style == MAPGEN_STYLE_INLAND && cfg->starts > 0) {
        int needed = cfg->starts;
        if ((int)ss->numStarts + needed > MAX_STARTS)
            needed = MAX_STARTS - (int)ss->numStarts;
        int carved = 0;
        for (int att = 0; att < 2000 && carved < needed; att++) {
            int tx = x1 + (int)(xorshift32(rng) % (uint32_t)w);
            int ty = y1 + (int)(xorshift32(rng) % (uint32_t)h);
            if (mp->mapItem[tx][ty] != RIVER) continue;
            /* Check no other carved DEEP_SEA too close */
            bool tooClose = false;
            for (int ddx = -10; ddx <= 10 && !tooClose; ddx++)
                for (int ddy = -10; ddy <= 10 && !tooClose; ddy++) {
                    int nx = tx + ddx, ny = ty + ddy;
                    if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                        mp->mapItem[nx][ny] == DEEP_SEA)
                        tooClose = true;
                }
            if (tooClose) continue;
            mp->mapItem[tx][ty] = DEEP_SEA;
            carved++;
        }
    }

    {
        int numStarts = cfg->starts;
        if ((int)ss->numStarts + numStarts > MAX_STARTS)
            numStarts = MAX_STARTS - (int)ss->numStarts;

        GenPos *cands = (GenPos *)malloc(sizeof(GenPos) * (size_t)totalTiles);
        if (cands && numStarts > 0) {
            int nCand = 0;
            for (int ix = 0; ix < w; ix++)
                for (int iy = 0; iy < h; iy++) {
                    int x = x1 + ix, y = y1 + iy;
                    if (mp->mapItem[x][y] != DEEP_SEA) continue;
                    /* Must be within 8 tiles of land */
                    bool nearLand = false;
                    for (int ddx = -8; ddx <= 8 && !nearLand; ddx++)
                        for (int ddy = -8; ddy <= 8 && !nearLand; ddy++) {
                            int nnx = x + ddx, nny = y + ddy;
                            if (nnx >= x1 && nnx <= x2 && nny >= y1 && nny <= y2 &&
                                mp->mapItem[nnx][nny] != DEEP_SEA &&
                                mp->mapItem[nnx][nny] != RIVER)
                                nearLand = true;
                        }
                    if (!nearLand) continue;
                    cands[nCand++] = (GenPos){x, y};
                }

            int startStart = (int)ss->numStarts;
            int placed = 0;
            int startMinD2 = startSpacing2;
            for (int att = 0; att < 4000 && placed < numStarts && nCand > 0; att++) {
                int ci = (int)(xorshift32(rng) % (uint32_t)nCand);
                int sx = cands[ci].x, sy = cands[ci].y;
                int minD2 = 999999;
                for (int i = startStart; i < (int)ss->numStarts; i++) {
                    int ddx = (int)ss->item[i].x - sx;
                    int ddy = (int)ss->item[i].y - sy;
                    int d2 = ddx * ddx + ddy * ddy;
                    if (d2 < minD2) minD2 = d2;
                }
                if (minD2 >= startMinD2 && ss->numStarts < MAX_STARTS) {
                    start *s = &ss->item[ss->numStarts];
                    s->x = (BYTE)sx; s->y = (BYTE)sy;
                    s->dir = dirTowardCenter(sx, sy,
                        (int)(centerX + 0.5f), (int)(centerY + 0.5f));
                    ss->numStarts++;
                    placed++;
                } else if (att > 0 && att % 500 == 0 && startMinD2 > 4) {
                    /* Relax spacing after many failed attempts */
                    startMinD2 /= 2;
                }
            }
        }
        if (cands) free(cands);
    }

    /* ===== 12a. Coastal fringe (before mining) ===== */
    addCoastalFringe(mp, x1, y1, x2, y2, rng);

    /* ===== 12b. Mines (biased toward bases/pillboxes) ===== */

    if (cfg->params.natural.mineDensityPct > 0) {
        int mPct = cfg->params.natural.mineDensityPct;
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++) {
                int tx = x1 + ix, ty = y1 + iy;
                BYTE t = mp->mapItem[tx][ty];
                /* Only mine terrain types that have mined variants (SWAMP..GRASS) */
                if (t < SWAMP || t > GRASS) continue;
                /* Increase mine probability near bases and pillboxes */
                int effectivePct = mPct;
                int nearObjR2 = 10 * 10;
                for (int i = 0; i < (int)bs->numBases; i++) {
                    int ddx = tx - (int)bs->item[i].x;
                    int ddy = ty - (int)bs->item[i].y;
                    if (ddx * ddx + ddy * ddy < nearObjR2) {
                        effectivePct = mPct * 3;
                        break;
                    }
                }
                if (effectivePct == mPct) {
                    for (int i = 0; i < (int)pb->numPills; i++) {
                        int ddx = tx - (int)pb->item[i].x;
                        int ddy = ty - (int)pb->item[i].y;
                        if (ddx * ddx + ddy * ddy < nearObjR2) {
                            effectivePct = mPct * 3;
                            break;
                        }
                    }
                }
                if (effectivePct > 100) effectivePct = 100;
                if ((int)(xorshift32(rng) % 100) < effectivePct)
                    mp->mapItem[tx][ty] = (BYTE)(t + MINE_SUBTRACT);
            }
    }

    free(heights);
}

/* Remove all objects within the generation region. */
static void clearObjectsInRegion(struct basesObj *bs, struct pillsObj *pb,
                                 struct startsObj *ss, const MapGenConfig *cfg) {
    /* Remove bases in region (iterate backwards to avoid index issues) */
    for (int i = bs->numBases - 1; i >= 0; i--) {
        base *b = &bs->item[i];
        if (b->x >= cfg->x1 && b->x <= cfg->x2 &&
            b->y >= cfg->y1 && b->y <= cfg->y2) {
            /* Shift remaining items down */
            for (int j = i; j < bs->numBases - 1; j++) {
                bs->item[j] = bs->item[j + 1];
            }
            bs->numBases--;
        }
    }

    /* Remove pillboxes in region */
    for (int i = pb->numPills - 1; i >= 0; i--) {
        pillbox *p = &pb->item[i];
        if (p->x >= cfg->x1 && p->x <= cfg->x2 &&
            p->y >= cfg->y1 && p->y <= cfg->y2) {
            for (int j = i; j < pb->numPills - 1; j++) {
                pb->item[j] = pb->item[j + 1];
            }
            pb->numPills--;
        }
    }

    /* Remove starts in region */
    for (int i = ss->numStarts - 1; i >= 0; i--) {
        start *s = &ss->item[i];
        if (s->x >= cfg->x1 && s->x <= cfg->x2 &&
            s->y >= cfg->y1 && s->y <= cfg->y2) {
            for (int j = i; j < ss->numStarts - 1; j++) {
                ss->item[j] = ss->item[j + 1];
            }
            ss->numStarts--;
        }
    }
}

/* -------------------------------------------------------
 * Maze generator wrapper
 * ------------------------------------------------------- */
static void mapGenMaze(struct mapObj *mp, struct basesObj *bs,
                       struct pillsObj *pb, struct startsObj *ss,
                       const MapGenConfig *cfg, uint32_t *rng) {
    MazeConfig mc = mazeDefaultConfig();
    mc.algo = cfg->params.maze.algo;
    mc.wallThick = cfg->params.maze.wallThick;
    mc.corridorWidth = cfg->params.maze.corridorWidth;
    mc.entries = cfg->params.maze.entries;
    mc.placeBases = true;
    mc.placePills = true;
    mc.cityRooms = cfg->params.maze.cityRooms;
    mc.wallTerrain = (BYTE)cfg->params.maze.wallTerrain;
    mc.corridorTerrain = (BYTE)cfg->params.maze.corridorTerrain;
    mc.seed = cfg->seed;

    /* Inset the maze by 1 tile from the region edges so there's a deep sea
     * border around the maze (important when generating to the full map). */
    int mx1 = cfg->x1 + 1;
    int my1 = cfg->y1 + 1;
    int mx2 = cfg->x2 - 1;
    int my2 = cfg->y2 - 1;
    if (mx2 <= mx1 || my2 <= my1) {
        mx1 = cfg->x1; my1 = cfg->y1;
        mx2 = cfg->x2; my2 = cfg->y2;
    }

    /* Clear the full region to DEEP_SEA first */
    for (int x = cfg->x1; x <= cfg->x2; x++) {
        for (int y = cfg->y1; y <= cfg->y2; y++) {
            mp->mapItem[x][y] = DEEP_SEA;
        }
    }

    /* Generate maze terrain into the inset region */
    mazeGenerate(mx1, my1, mx2, my2, mp->mapItem, &mc);

    /* Place bases at random corridor cells */
    BYTE corrTerrain = (BYTE)cfg->params.maze.corridorTerrain;
    if (cfg->bases > 0) {
        uint32_t placeRng = *rng;
        int maxBases = cfg->bases;
        if (maxBases > MAX_BASES - bs->numBases) maxBases = MAX_BASES - bs->numBases;
        int placed = 0;
        for (int attempt = 0; attempt < 200 && placed < maxBases; attempt++) {
            int bx = mx1 + 1 + (int)(mapGenXorshift32(&placeRng) % (uint32_t)(mx2 - mx1 - 1));
            int by = my1 + 1 + (int)(mapGenXorshift32(&placeRng) % (uint32_t)(my2 - my1 - 1));
            if (bx > 0 && bx < 255 && by > 0 && by < 255 &&
                mp->mapItem[bx][by] == corrTerrain) {
                /* Check no existing base at this spot */
                bool occupied = false;
                for (int i = 0; i < bs->numBases; i++) {
                    if (bs->item[i].x == bx && bs->item[i].y == by) {
                        occupied = true; break;
                    }
                }
                if (occupied) continue;
                base *b = &bs->item[bs->numBases];
                memset(b, 0, sizeof(base));
                b->x = (BYTE)bx;
                b->y = (BYTE)by;
                b->owner = NEUTRAL;
                b->armour = 90;
                b->shells = 90;
                b->mines = 90;
                bs->numBases++;
                placed++;
            }
        }
        *rng = placeRng;
    }

    /* Place pillboxes on corridor tiles */
    if (cfg->pills > 0) {
        uint32_t placeRng = *rng;
        int maxPills = cfg->pills;
        if (maxPills > MAX_PILLS - pb->numPills) maxPills = MAX_PILLS - pb->numPills;
        int placed = 0;
        for (int attempt = 0; attempt < 200 && placed < maxPills; attempt++) {
            int px = mx1 + 1 + (int)(mapGenXorshift32(&placeRng) % (uint32_t)(mx2 - mx1 - 1));
            int py = my1 + 1 + (int)(mapGenXorshift32(&placeRng) % (uint32_t)(my2 - my1 - 1));
            if (px > 0 && px < 255 && py > 0 && py < 255 &&
                mp->mapItem[px][py] == corrTerrain) {
                /* Check no existing pill/base at this spot */
                bool occupied = false;
                for (int i = 0; i < pb->numPills; i++) {
                    if (pb->item[i].x == px && pb->item[i].y == py) {
                        occupied = true; break;
                    }
                }
                for (int i = 0; !occupied && i < bs->numBases; i++) {
                    if (bs->item[i].x == px && bs->item[i].y == py) {
                        occupied = true; break;
                    }
                }
                if (occupied) continue;
                pillbox *p = &pb->item[pb->numPills];
                memset(p, 0, sizeof(pillbox));
                p->x = (BYTE)px;
                p->y = (BYTE)py;
                p->owner = NEUTRAL;
                p->armour = 15;
                p->speed = 50;
                pb->numPills++;
                placed++;
            }
        }
        *rng = placeRng;
    }

    /* Place starts on DEEP_SEA tiles in the border around the maze.
     * Starts must be on DEEP_SEA. Place them evenly around the perimeter. */
    if (cfg->starts > 0) {
        uint32_t placeRng = *rng;
        int maxStarts = cfg->starts;
        if (maxStarts > MAX_STARTS - ss->numStarts) maxStarts = MAX_STARTS - ss->numStarts;
        int placed = 0;
        /* Collect candidate DEEP_SEA tiles in the border region */
        int perimW = cfg->x2 - cfg->x1 + 1;
        int perimH = cfg->y2 - cfg->y1 + 1;
        int perim = 2 * (perimW + perimH) - 4;
        if (perim < 1) perim = 1;
        for (int s = 0; s < maxStarts; s++) {
            /* Try positions evenly spaced around the outer border */
            int basePos = (perim * s) / (maxStarts > 0 ? maxStarts : 1);
            bool found = false;
            for (int offset = 0; offset < perim && !found; offset++) {
                int pos = (basePos + offset) % perim;
                int sx, sy;
                if (pos < perimW) {
                    sx = cfg->x1 + pos; sy = cfg->y1;
                } else if (pos < perimW + perimH - 1) {
                    sx = cfg->x2; sy = cfg->y1 + (pos - perimW + 1);
                } else if (pos < 2 * perimW + perimH - 2) {
                    sx = cfg->x2 - (pos - perimW - perimH + 2); sy = cfg->y2;
                } else {
                    sx = cfg->x1; sy = cfg->y2 - (pos - 2 * perimW - perimH + 3);
                }
                if (sx < 0 || sx > 255 || sy < 0 || sy > 255) continue;
                if (mp->mapItem[sx][sy] != DEEP_SEA) continue;
                /* Check not already occupied */
                bool occ = false;
                for (int i = 0; i < ss->numStarts; i++) {
                    if (ss->item[i].x == sx && ss->item[i].y == sy) {
                        occ = true; break;
                    }
                }
                if (occ) continue;
                start *st = &ss->item[ss->numStarts];
                memset(st, 0, sizeof(start));
                st->x = (BYTE)sx;
                st->y = (BYTE)sy;
                st->dir = 0;
                ss->numStarts++;
                placed++;
                found = true;
            }
            if (!found) break;
        }
        *rng = placeRng;
    }
}

/* -------------------------------------------------------
 * Fractal generator — diamond-square heightmap
 * ------------------------------------------------------- */

#define FRAC_SIZE 257  /* 2^8 + 1 */

static float xorshift32Float(uint32_t *rng) {
    return (float)(xorshift32(rng) & 0xFFFFu) / 65535.0f;
}

static void diamondSquare(float *heights, int size, float roughness,
                          uint32_t *rng) {
    /* Seed corners */
    heights[0 * size + 0]                   = xorshift32Float(rng);
    heights[0 * size + (size - 1)]          = xorshift32Float(rng);
    heights[(size - 1) * size + 0]          = xorshift32Float(rng);
    heights[(size - 1) * size + (size - 1)] = xorshift32Float(rng);

    float amplitude = 1.0f;
    float decay = (float)pow(2.0, -(double)roughness / 5.0);

    for (int step = size - 1; step >= 2; step /= 2) {
        int half = step / 2;

        /* Diamond step */
        for (int y = half; y < size - 1; y += step) {
            for (int x = half; x < size - 1; x += step) {
                float avg = (heights[(y - half) * size + (x - half)] +
                             heights[(y - half) * size + (x + half)] +
                             heights[(y + half) * size + (x - half)] +
                             heights[(y + half) * size + (x + half)]) * 0.25f;
                heights[y * size + x] = avg + amplitude * (xorshift32Float(rng) - 0.5f);
            }
        }

        /* Square step */
        for (int y = 0; y < size; y += half) {
            for (int x = ((y / half) % 2 == 0) ? half : 0; x < size; x += step) {
                float sum = 0.0f;
                int count = 0;
                if (y - half >= 0)      { sum += heights[(y - half) * size + x]; count++; }
                if (y + half < size)    { sum += heights[(y + half) * size + x]; count++; }
                if (x - half >= 0)      { sum += heights[y * size + (x - half)]; count++; }
                if (x + half < size)    { sum += heights[y * size + (x + half)]; count++; }
                heights[y * size + x] = sum / (float)count + amplitude * (xorshift32Float(rng) - 0.5f);
            }
        }

        amplitude *= decay;
    }

    /* Normalize to [0, 1] */
    float minH = heights[0], maxH = heights[0];
    for (int i = 1; i < size * size; i++) {
        if (heights[i] < minH) minH = heights[i];
        if (heights[i] > maxH) maxH = heights[i];
    }
    float range = maxH - minH;
    if (range < 1e-9f) range = 1.0f;
    for (int i = 0; i < size * size; i++) {
        heights[i] = (heights[i] - minH) / range;
    }
}

static void fractalAssignTerrain(struct mapObj *mp, float *heights, int size,
                                 const MapGenConfig *cfg, uint32_t *rng) {
    int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;

    /* Clamp border region to 0 to enforce mine border as sea */
    for (int iy = 0; iy < h && iy < size; iy++) {
        for (int ix = 0; ix < w && ix < size; ix++) {
            int mapX = x1 + ix, mapY = y1 + iy;
            if (mapX <= MAP_MINE_EDGE_LEFT || mapX >= MAP_MINE_EDGE_RIGHT ||
                mapY <= MAP_MINE_EDGE_TOP || mapY >= MAP_MINE_EDGE_BOTTOM) {
                heights[iy * size + ix] = 0.0f;
            }
        }
    }

    /* Extract only valid region heights for accurate threshold computation */
    float *validHeights = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    if (!validHeights) return;
    for (int iy = 0; iy < h; iy++)
        for (int ix = 0; ix < w; ix++)
            validHeights[iy * w + ix] = heights[iy * size + ix];

    float seaLevel = findLandThreshold(validHeights, w * h, cfg->params.fractal.landPct);
    free(validHeights);

    /* Find max land elevation */
    float maxLand = seaLevel;
    for (int iy = 0; iy < h && iy < size; iy++)
        for (int ix = 0; ix < w && ix < size; ix++)
            if (heights[iy * size + ix] > maxLand) maxLand = heights[iy * size + ix];

    float landRange = maxLand - seaLevel;
    if (landRange < 1e-9f) landRange = 1.0f;

    int layers = cfg->params.fractal.terrainLayers;
    uint32_t noiseSeed = xorshift32(rng);

    /* Terrain band table — maps band index to terrain type.
     * With fewer layers, merge bands; with more, subdivide. */
    BYTE bandTerrain[6] = { SWAMP, GRASS, FOREST, ROAD, CRATER, CRATER };
    if (layers <= 2) {
        bandTerrain[0] = GRASS; bandTerrain[1] = FOREST;
    } else if (layers == 3) {
        bandTerrain[0] = SWAMP; bandTerrain[1] = GRASS; bandTerrain[2] = FOREST;
    } else if (layers == 4) {
        bandTerrain[0] = SWAMP; bandTerrain[1] = GRASS;
        bandTerrain[2] = FOREST; bandTerrain[3] = ROAD;
    } else if (layers == 5) {
        bandTerrain[0] = SWAMP; bandTerrain[1] = GRASS;
        bandTerrain[2] = FOREST; bandTerrain[3] = ROAD; bandTerrain[4] = CRATER;
    } else {
        bandTerrain[0] = SWAMP; bandTerrain[1] = GRASS;
        bandTerrain[2] = FOREST; bandTerrain[3] = ROAD;
        bandTerrain[4] = CRATER; bandTerrain[5] = RUBBLE;
    }

    for (int iy = 0; iy < h && iy < size; iy++) {
        for (int ix = 0; ix < w && ix < size; ix++) {
            float hv = heights[iy * size + ix];
            int tx = x1 + ix, ty = y1 + iy;
            if (hv < seaLevel) {
                mp->mapItem[tx][ty] = DEEP_SEA;
            } else {
                float norm = (hv - seaLevel) / landRange;
                /* Perturb band boundaries with secondary noise */
                float perturbation = fbmNoise(noiseSeed, (float)tx, (float)ty, 16, 2);
                norm += (perturbation - 0.5f) * 0.1f;
                if (norm < 0.0f) norm = 0.0f;
                if (norm > 1.0f) norm = 1.0f;
                int band = (int)(norm * (float)layers);
                if (band >= layers) band = layers - 1;
                mp->mapItem[tx][ty] = bandTerrain[band];
            }
        }
    }

    /* Cluster buildings in ROAD band (band 3+): group ROAD tiles into
     * building clusters with HALFBUILDING edges */
    if (layers >= 4) {
        /* Mark ROAD tiles that have 3+ ROAD cardinal neighbors as BUILDING */
        for (int iy = 0; iy < h && iy < size; iy++) {
            for (int ix = 0; ix < w && ix < size; ix++) {
                int tx = x1 + ix, ty = y1 + iy;
                if (mp->mapItem[tx][ty] != ROAD) continue;
                int roadNeighbors = 0;
                static const int dx4[] = {0, 0, 1, -1};
                static const int dy4[] = {-1, 1, 0, 0};
                for (int d = 0; d < 4; d++) {
                    int nx = tx + dx4[d], ny = ty + dy4[d];
                    if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                        mp->mapItem[nx][ny] == ROAD)
                        roadNeighbors++;
                }
                /* 30% of clustered road tiles become buildings */
                if (roadNeighbors >= 3 && (xorshift32(rng) % 100) < 30)
                    mp->mapItem[tx][ty] = BUILDING;
            }
        }
        /* Add HALFBUILDING around BUILDING tiles */
        for (int iy = 0; iy < h && iy < size; iy++) {
            for (int ix = 0; ix < w && ix < size; ix++) {
                int tx = x1 + ix, ty = y1 + iy;
                if (mp->mapItem[tx][ty] != BUILDING) continue;
                static const int dx4[] = {0, 0, 1, -1};
                static const int dy4[] = {-1, 1, 0, 0};
                for (int d = 0; d < 4; d++) {
                    int nx = tx + dx4[d], ny = ty + dy4[d];
                    if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                        mp->mapItem[nx][ny] == ROAD)
                        mp->mapItem[nx][ny] = HALFBUILDING;
                }
            }
        }
    }
}

static void fractalCoastEnhance(struct mapObj *mp, const MapGenConfig *cfg,
                                uint32_t *rng) {
    int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    int jagged = cfg->params.fractal.coastJaggedness;
    static const int dx4[] = {0, 0, 1, -1};
    static const int dy4[] = {-1, 1, 0, 0};

    /* Snapshot buffer prevents cascading: read from snapshot, write to map */
    BYTE *snap = (BYTE *)malloc((size_t)w * (size_t)h);
    if (!snap) return;

    for (int pass = 0; pass < jagged; pass++) {
        /* Take snapshot of current map state */
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++)
                snap[iy * w + ix] = mp->mapItem[x1 + ix][y1 + iy];

        /* Erode: land adjacent to sea -> sea (read from snapshot) */
        for (int ix = 0; ix < w; ix++) {
            for (int iy = 0; iy < h; iy++) {
                if (snap[iy * w + ix] == DEEP_SEA) continue;
                bool adjSea = false;
                for (int d = 0; d < 4; d++) {
                    int nix = ix + dx4[d], niy = iy + dy4[d];
                    if (nix < 0 || nix >= w || niy < 0 || niy >= h) continue;
                    if (snap[niy * w + nix] == DEEP_SEA) { adjSea = true; break; }
                }
                if (adjSea && (xorshift32(rng) % 1000) < (uint32_t)(jagged * 35)) {
                    mp->mapItem[x1 + ix][y1 + iy] = DEEP_SEA;
                }
            }
        }

        /* Re-snapshot after erosion for accretion pass */
        for (int ix = 0; ix < w; ix++)
            for (int iy = 0; iy < h; iy++)
                snap[iy * w + ix] = mp->mapItem[x1 + ix][y1 + iy];

        /* Accrete: sea adjacent to land -> land (read from snapshot) */
        for (int ix = 0; ix < w; ix++) {
            for (int iy = 0; iy < h; iy++) {
                if (snap[iy * w + ix] != DEEP_SEA) continue;
                bool adjLand = false;
                for (int d = 0; d < 4; d++) {
                    int nix = ix + dx4[d], niy = iy + dy4[d];
                    if (nix < 0 || nix >= w || niy < 0 || niy >= h) continue;
                    if (snap[niy * w + nix] != DEEP_SEA && snap[niy * w + nix] != RIVER)
                        { adjLand = true; break; }
                }
                if (adjLand && (xorshift32(rng) % 1000) < (uint32_t)(jagged * 35)) {
                    mp->mapItem[x1 + ix][y1 + iy] = GRASS;
                }
            }
        }
    }
    free(snap);
}

static void fractalEnsureConnected(struct mapObj *mp, const MapGenConfig *cfg) {
    int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    int area = w * h;
    static const int dx4[] = {0, 0, 1, -1};
    static const int dy4[] = {-1, 1, 0, 0};

    /* Label each land tile with its connected component ID via flood fill.
     * Then remove only components smaller than MIN_FRAGMENT tiles. */
    #define MIN_FRAGMENT 10

    int *label = (int *)malloc(sizeof(int) * (size_t)area);
    GenPos *queue = (GenPos *)malloc(sizeof(GenPos) * (size_t)area);
    if (!label || !queue) { free(label); free(queue); return; }
    for (int i = 0; i < area; i++) label[i] = -1;

    int numComponents = 0;
    int *compSize = NULL;
    int compCap = 0;

    for (int sy = y1; sy <= y2; sy++) {
        for (int sx = x1; sx <= x2; sx++) {
            int si = (sy - y1) * w + (sx - x1);
            if (label[si] >= 0) continue;
            if (mp->mapItem[sx][sy] == DEEP_SEA || mp->mapItem[sx][sy] == RIVER) continue;

            /* New component — flood fill */
            int compId = numComponents++;
            if (compId >= compCap) {
                compCap = compCap ? compCap * 2 : 64;
                compSize = (int *)realloc(compSize, sizeof(int) * (size_t)compCap);
                if (!compSize) { free(label); free(queue); return; }
            }
            compSize[compId] = 0;

            int head = 0, tail = 0;
            queue[tail++] = (GenPos){sx, sy};
            label[si] = compId;
            while (head < tail) {
                GenPos cur = queue[head++];
                compSize[compId]++;
                for (int d = 0; d < 4; d++) {
                    int nx = cur.x + dx4[d], ny = cur.y + dy4[d];
                    if (nx < x1 || nx > x2 || ny < y1 || ny > y2) continue;
                    int vi = (ny - y1) * w + (nx - x1);
                    if (label[vi] >= 0) continue;
                    if (mp->mapItem[nx][ny] == DEEP_SEA || mp->mapItem[nx][ny] == RIVER) continue;
                    label[vi] = compId;
                    queue[tail++] = (GenPos){nx, ny};
                }
            }
        }
    }

    /* Remove only small fragments */
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            int vi = (y - y1) * w + (x - x1);
            if (label[vi] >= 0 && compSize[label[vi]] < MIN_FRAGMENT)
                mp->mapItem[x][y] = DEEP_SEA;
        }
    }

    free(label);
    free(queue);
    free(compSize);
    #undef MIN_FRAGMENT
}

static void fractalCarveRivers(struct mapObj *mp, float *heights, int size,
                               const MapGenConfig *cfg, uint32_t *rng) {
    int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    static const int dx4[] = {0, 0, 1, -1};
    static const int dy4[] = {-1, 1, 0, 0};

    /* Flow direction: 0=N, 1=E, 2=S, 3=W, 4=sink */
    uint8_t *flowDir = (uint8_t *)malloc((size_t)w * (size_t)h);
    int *accum = (int *)calloc((size_t)w * (size_t)h, sizeof(int));
    if (!flowDir || !accum) { free(flowDir); free(accum); return; }

    int totalLand = 0;
    for (int iy = 0; iy < h && iy < size; iy++) {
        for (int ix = 0; ix < w && ix < size; ix++) {
            int tx = x1 + ix, ty = y1 + iy;
            int idx = iy * w + ix;
            if (mp->mapItem[tx][ty] == DEEP_SEA || mp->mapItem[tx][ty] == RIVER) {
                flowDir[idx] = 4;
                continue;
            }
            totalLand++;
            float myH = heights[iy * size + ix];
            float bestH = myH;
            int bestDir = 4;
            for (int d = 0; d < 4; d++) {
                int nx = ix + dx4[d], ny = iy + dy4[d];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h || nx >= size || ny >= size) {
                    /* Edge flows to sea */
                    if (bestDir == 4) bestDir = d;
                    continue;
                }
                float nh = heights[ny * size + nx];
                if (nh < bestH) { bestH = nh; bestDir = d; }
            }
            flowDir[idx] = (uint8_t)bestDir;
        }
    }

    /* Flow accumulation */
    for (int iy = 0; iy < h && iy < size; iy++) {
        for (int ix = 0; ix < w && ix < size; ix++) {
            int idx = iy * w + ix;
            if (flowDir[idx] == 4) continue;
            /* Follow chain, incrementing accumulation */
            int cx = ix, cy = iy;
            int maxSteps = w * h;
            while (maxSteps-- > 0) {
                int ci = cy * w + cx;
                int d = flowDir[ci];
                if (d == 4) break;
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) break;
                int ni = ny * w + nx;
                accum[ni]++;
                cx = nx; cy = ny;
            }
        }
    }

    /* River threshold — aim for 2-5 major rivers */
    int threshold = totalLand / 40;
    if (threshold < 10) threshold = 10;

    for (int iy = 0; iy < h && iy < size; iy++) {
        for (int ix = 0; ix < w && ix < size; ix++) {
            int tx = x1 + ix, ty = y1 + iy;
            int idx = iy * w + ix;
            if (accum[idx] < threshold) continue;
            if (mp->mapItem[tx][ty] == DEEP_SEA) continue;
            mp->mapItem[tx][ty] = RIVER;
            /* Wide rivers for high accumulation */
            if (accum[idx] > threshold * 2) {
                for (int d = 0; d < 4; d++) {
                    int nx = tx + dx4[d], ny = ty + dy4[d];
                    if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                        mp->mapItem[nx][ny] != DEEP_SEA && mp->mapItem[nx][ny] != RIVER &&
                        mp->mapItem[nx][ny] != BUILDING && mp->mapItem[nx][ny] != HALFBUILDING) {
                        mp->mapItem[nx][ny] = RIVER;
                    }
                }
            }
        }
    }

    /* Add swamp adjacent to river banks */
    for (int ix = 0; ix < w && ix < size; ix++) {
        for (int iy = 0; iy < h && iy < size; iy++) {
            int tx = x1 + ix, ty = y1 + iy;
            if (mp->mapItem[tx][ty] != RIVER) continue;
            for (int d = 0; d < 4; d++) {
                int nx = tx + dx4[d], ny = ty + dy4[d];
                if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                    mp->mapItem[nx][ny] == GRASS && (xorshift32(rng) % 100) < 40) {
                    mp->mapItem[nx][ny] = SWAMP;
                }
            }
        }
    }

    free(flowDir);
    free(accum);
}

static void fractalFillLakes(struct mapObj *mp, float *heights, int size,
                             const MapGenConfig *cfg) {
    int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    static const int dx4[] = {0, 0, 1, -1};
    static const int dy4[] = {-1, 1, 0, 0};

    bool *processed = (bool *)calloc((size_t)w * (size_t)h, sizeof(bool));
    if (!processed) return;

    for (int iy = 0; iy < h && iy < size; iy++) {
        for (int ix = 0; ix < w && ix < size; ix++) {
            int tx = x1 + ix, ty = y1 + iy;
            int idx = iy * w + ix;
            if (processed[idx]) continue;
            BYTE t = mp->mapItem[tx][ty];
            if (t == DEEP_SEA || t == RIVER) continue;

            /* Check if this is a sink (no lower cardinal neighbor) */
            float myH = heights[iy * size + ix];
            bool isSink = true;
            for (int d = 0; d < 4; d++) {
                int nx = ix + dx4[d], ny = iy + dy4[d];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h || nx >= size || ny >= size) continue;
                if (heights[ny * size + nx] < myH) { isSink = false; break; }
            }
            if (!isSink) continue;

            /* BFS to find basin — tiles that would drain here */
            GenPos *basin = (GenPos *)malloc(sizeof(GenPos) * 64);
            if (!basin) continue;
            int basinCap = 64, basinSize = 0;
            float spillElev = 1e9f;

            GenPos *bfsQ = (GenPos *)malloc(sizeof(GenPos) * (size_t)(w * h));
            bool *inBasin = (bool *)calloc((size_t)w * (size_t)h, sizeof(bool));
            if (!bfsQ || !inBasin) { free(basin); free(bfsQ); free(inBasin); continue; }

            int bHead = 0, bTail = 0;
            bfsQ[bTail++] = (GenPos){ix, iy};
            inBasin[iy * w + ix] = true;
            if (basinSize < basinCap) basin[basinSize++] = (GenPos){tx, ty};

            while (bHead < bTail && basinSize <= 64) {
                GenPos cur = bfsQ[bHead++];
                for (int d = 0; d < 4; d++) {
                    int nx = cur.x + dx4[d], ny = cur.y + dy4[d];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h || nx >= size || ny >= size) continue;
                    if (inBasin[ny * w + nx]) continue;
                    float nh = heights[ny * size + nx];
                    int ntx = x1 + nx, nty = y1 + ny;
                    if (mp->mapItem[ntx][nty] == DEEP_SEA || mp->mapItem[ntx][nty] == RIVER) {
                        /* Already water — basin drains to existing water, no lake needed */
                        basinSize = 999;
                        break;
                    }
                    if (nh <= myH + 0.02f) {
                        inBasin[ny * w + nx] = true;
                        bfsQ[bTail++] = (GenPos){nx, ny};
                        if (basinSize < 64) basin[basinSize++] = (GenPos){ntx, nty};
                        else basinSize++;
                    } else {
                        if (nh < spillElev) spillElev = nh;
                    }
                }
            }

            /* Fill if basin is small enough and doesn't drain to existing water */
            if (basinSize >= 2 && basinSize <= 64) {
                int fillCount = basinSize < 64 ? basinSize : 64;
                for (int i = 0; i < fillCount; i++) {
                    int bx = basin[i].x - x1, by = basin[i].y - y1;
                    float elev = heights[by * size + bx];
                    if (elev <= spillElev) {
                        mp->mapItem[basin[i].x][basin[i].y] = RIVER;
                    }
                    processed[by * w + bx] = true;
                }
            }

            free(basin);
            free(bfsQ);
            free(inBasin);
        }
    }

    free(processed);
}

static void fractalPlaceObjects(struct mapObj *mp, struct basesObj *bs,
                                struct pillsObj *pb, struct startsObj *ss,
                                float *heights, int size,
                                const MapGenConfig *cfg, uint32_t *rng) {
    int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    int numBases = cfg->bases;
    int numPills = cfg->pills;
    int numStarts = cfg->starts;
    static const int dx4[] = {0, 0, 1, -1};
    static const int dy4[] = {-1, 1, 0, 0};

    if (numBases > (int)(MAX_BASES - bs->numBases))
        numBases = MAX_BASES - (int)bs->numBases;
    if (numPills > (int)(MAX_PILLS - pb->numPills))
        numPills = MAX_PILLS - (int)pb->numPills;
    if (numStarts > (int)(MAX_STARTS - ss->numStarts))
        numStarts = MAX_STARTS - (int)ss->numStarts;

    int baseMinD = numBases > 0 ? (int)(200.0f / sqrtf((float)numBases)) : 0;
    int pillMinD = numPills > 0 ? (int)(150.0f / sqrtf((float)numPills)) : 0;

    /* Place bases on GRASS/ROAD at mid-elevations, prefer flat areas.
     * Retry with progressively relaxed spacing if placement fails. */
    for (int placed = 0; placed < numBases; placed++) {
        int bestX = -1, bestY = -1;
        float bestScore = -1.0f;
        int curMinD = baseMinD;

        for (int relaxPass = 0; relaxPass < 4 && bestX < 0; relaxPass++) {
            int attempts = w * h > 2000 ? 2000 : w * h;
            for (int a = 0; a < attempts; a++) {
                int ix = (int)(xorshift32(rng) % (uint32_t)w);
                int iy = (int)(xorshift32(rng) % (uint32_t)h);
                int tx = x1 + ix, ty = y1 + iy;
                BYTE t = mp->mapItem[tx][ty];
                if (t != GRASS && t != ROAD) continue;
                /* Check spacing */
                bool tooClose = false;
                for (int i = 0; i < (int)bs->numBases && !tooClose; i++) {
                    int ddx = tx - (int)bs->item[i].x;
                    int ddy = ty - (int)bs->item[i].y;
                    if (ddx * ddx + ddy * ddy < curMinD * curMinD) tooClose = true;
                }
                if (tooClose) continue;
                /* Score: prefer flat areas (low local variance) */
                float hv = (ix < size && iy < size) ? heights[iy * size + ix] : 0.5f;
                float variance = 0.0f;
                for (int d = 0; d < 4; d++) {
                    int nx = ix + dx4[d], ny = iy + dy4[d];
                    if (nx >= 0 && nx < size && ny >= 0 && ny < size) {
                        float dh = heights[ny * size + nx] - hv;
                        variance += dh * dh;
                    }
                }
                float score = 1.0f - variance * 100.0f; /* flat = high score */
                if (score > bestScore) { bestScore = score; bestX = tx; bestY = ty; }
            }
            curMinD /= 2; /* Halve spacing for next attempt */
        }
        if (bestX >= 0 && bs->numBases < MAX_BASES) {
            base *b = &bs->item[bs->numBases];
            memset(b, 0, sizeof(base));
            b->x = (BYTE)bestX; b->y = (BYTE)bestY;
            b->owner = 0xFF; b->armour = 90; b->shells = 90; b->mines = 90;
            bs->numBases++;
        }
    }

    /* Place pillboxes near choke points and terrain transitions.
     * Retry with progressively relaxed spacing if placement fails. */
    for (int placed = 0; placed < numPills; placed++) {
        int bestX = -1, bestY = -1;
        float bestScore = -1.0f;
        int curMinD = pillMinD;

        for (int relaxPass = 0; relaxPass < 4 && bestX < 0; relaxPass++) {
            int attempts = w * h > 2000 ? 2000 : w * h;
            for (int a = 0; a < attempts; a++) {
                int ix = (int)(xorshift32(rng) % (uint32_t)w);
                int iy = (int)(xorshift32(rng) % (uint32_t)h);
                int tx = x1 + ix, ty = y1 + iy;
                BYTE t = mp->mapItem[tx][ty];
                if (t == DEEP_SEA || t == RIVER || t == BUILDING || t == HALFBUILDING) continue;
                /* Check no overlap with bases/pills */
                bool overlap = false;
                for (int i = 0; i < (int)bs->numBases && !overlap; i++)
                    if (bs->item[i].x == (BYTE)tx && bs->item[i].y == (BYTE)ty) overlap = true;
                for (int i = 0; i < (int)pb->numPills && !overlap; i++) {
                    int ddx = tx - (int)pb->item[i].x;
                    int ddy = ty - (int)pb->item[i].y;
                    if (ddx * ddx + ddy * ddy < curMinD * curMinD) overlap = true;
                }
                if (overlap) continue;
                /* Score: prefer choke points (fewer land neighbors in radius 3) */
                int landCount = 0;
                for (int dy = -3; dy <= 3; dy++)
                    for (int dx = -3; dx <= 3; dx++) {
                        int nx = tx + dx, ny = ty + dy;
                        if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                            mp->mapItem[nx][ny] != DEEP_SEA)
                            landCount++;
                    }
                /* Fewer land tiles = narrower passage = better pillbox spot */
                float score = (float)(49 - landCount);
                /* Bonus for being near river (crossing) */
                for (int d = 0; d < 4; d++) {
                    int nx = tx + dx4[d], ny = ty + dy4[d];
                    if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                        mp->mapItem[nx][ny] == RIVER)
                        score += 10.0f;
                }
                if (score > bestScore) { bestScore = score; bestX = tx; bestY = ty; }
            }
            curMinD /= 2; /* Halve spacing for next attempt */
        }
        if (bestX >= 0 && pb->numPills < MAX_PILLS) {
            pillbox *p = &pb->item[pb->numPills];
            memset(p, 0, sizeof(pillbox));
            p->x = (BYTE)bestX; p->y = (BYTE)bestY;
            p->owner = 0xFF; p->armour = 15; p->speed = 50;
            pb->numPills++;
        }
    }

    /* Place starts on DEEP_SEA near coast.
     * Retry with relaxed spacing and land-proximity if placement fails. */
    int cmx = (x1 + x2) / 2, cmy = (y1 + y2) / 2;
    for (int placed = 0; placed < numStarts; placed++) {
        int bestX = -1, bestY = -1;
        float bestScore = -1.0f;
        int startMinD = 20;
        int nearLandR = 5;

        for (int relaxPass = 0; relaxPass < 4 && bestX < 0; relaxPass++) {
            int attempts = w * h > 3000 ? 3000 : w * h;
            for (int a = 0; a < attempts; a++) {
                int ix = (int)(xorshift32(rng) % (uint32_t)w);
                int iy = (int)(xorshift32(rng) % (uint32_t)h);
                int tx = x1 + ix, ty = y1 + iy;
                if (mp->mapItem[tx][ty] != DEEP_SEA) continue;
                /* Must have land within radius nearLandR */
                bool nearLand = false;
                for (int dy = -nearLandR; dy <= nearLandR && !nearLand; dy++)
                    for (int dx = -nearLandR; dx <= nearLandR && !nearLand; dx++) {
                        int nx = tx + dx, ny = ty + dy;
                        if (nx >= x1 && nx <= x2 && ny >= y1 && ny <= y2 &&
                            mp->mapItem[nx][ny] != DEEP_SEA && mp->mapItem[nx][ny] != RIVER)
                            nearLand = true;
                    }
                if (!nearLand) continue;
                /* Check spacing with existing starts */
                bool tooClose = false;
                for (int i = 0; i < (int)ss->numStarts && !tooClose; i++) {
                    int ddx = tx - (int)ss->item[i].x;
                    int ddy = ty - (int)ss->item[i].y;
                    if (ddx * ddx + ddy * ddy < startMinD * startMinD) tooClose = true;
                }
                if (tooClose) continue;
                /* Spread around map perimeter */
                float angle = atan2f((float)(ty - cmy), (float)(tx - cmx));
                float desiredAngle = 6.283185307f * (float)placed / (float)numStarts;
                float angleDiff = angle - desiredAngle;
                while (angleDiff > 3.14159f) angleDiff -= 6.283185307f;
                while (angleDiff < -3.14159f) angleDiff += 6.283185307f;
                float score = 1.0f - fabsf(angleDiff) / 3.14159f;
                if (score > bestScore) { bestScore = score; bestX = tx; bestY = ty; }
            }
            startMinD /= 2;     /* Halve spacing for next attempt */
            nearLandR += 5;     /* Widen land-proximity search */
        }
        if (bestX >= 0 && ss->numStarts < MAX_STARTS) {
            start *s = &ss->item[ss->numStarts];
            memset(s, 0, sizeof(start));
            s->x = (BYTE)bestX; s->y = (BYTE)bestY;
            s->dir = dirTowardCenter(bestX, bestY, cmx, cmy);
            ss->numStarts++;
        }
    }
}

static void mapGenFractal(struct mapObj *mp, struct basesObj *bs,
                          struct pillsObj *pb, struct startsObj *ss,
                          const MapGenConfig *cfg, uint32_t *rng) {
    int x1 = cfg->x1, y1 = cfg->y1, x2 = cfg->x2, y2 = cfg->y2;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    int mapSize = w > h ? w : h;
    /* Use FRAC_SIZE for the heightmap (covers 256-tile map) */
    int size = FRAC_SIZE;
    if (mapSize + 1 > size) size = mapSize + 1;

    /* Clear region to deep sea */
    for (int x = x1; x <= x2; x++)
        for (int y = y1; y <= y2; y++)
            mp->mapItem[x][y] = DEEP_SEA;

    /* Fork sub-seeds for independent phases */
    uint32_t heightSeed = xorshift32(rng);
    uint32_t terrainSeed = xorshift32(rng);
    uint32_t coastSeed = xorshift32(rng);
    uint32_t riverSeed = xorshift32(rng);
    uint32_t placeSeed = xorshift32(rng);

    /* a. Allocate heightmap */
    float *heights = (float *)malloc((size_t)size * (size_t)size * sizeof(float));
    if (!heights) return;
    memset(heights, 0, (size_t)size * (size_t)size * sizeof(float));

    /* b. Diamond-square */
    diamondSquare(heights, size, (float)cfg->params.fractal.roughness, &heightSeed);

    /* c. Detail smoothing passes */
    int detail = cfg->params.fractal.detail;
    float *tmp = (float *)malloc((size_t)size * (size_t)size * sizeof(float));
    if (tmp) {
        for (int pass = 0; pass < detail; pass++) {
            memcpy(tmp, heights, (size_t)size * (size_t)size * sizeof(float));
            for (int iy = 0; iy < size; iy++) {
                for (int ix = 0; ix < size; ix++) {
                    float sum = tmp[iy * size + ix];
                    int cnt = 1;
                    if (iy > 0)          { sum += tmp[(iy - 1) * size + ix]; cnt++; }
                    if (iy < size - 1)   { sum += tmp[(iy + 1) * size + ix]; cnt++; }
                    if (ix > 0)          { sum += tmp[iy * size + (ix - 1)]; cnt++; }
                    if (ix < size - 1)   { sum += tmp[iy * size + (ix + 1)]; cnt++; }
                    heights[iy * size + ix] = sum / (float)cnt;
                }
            }
        }
        free(tmp);

        /* Re-normalize to [0,1] after smoothing narrowed the range */
        float minH = heights[0], maxH = heights[0];
        for (int i = 1; i < size * size; i++) {
            if (heights[i] < minH) minH = heights[i];
            if (heights[i] > maxH) maxH = heights[i];
        }
        float range = maxH - minH;
        if (range > 1e-9f) {
            for (int i = 0; i < size * size; i++)
                heights[i] = (heights[i] - minH) / range;
        }
    }

    /* d. Assign terrain from heightmap */
    fractalAssignTerrain(mp, heights, size, cfg, &terrainSeed);

    /* e. Coastline enhancement */
    fractalCoastEnhance(mp, cfg, &coastSeed);

    /* f. Ensure connected landmass */
    fractalEnsureConnected(mp, cfg);

    /* g. Coastal fringe (shallow water) */
    addCoastalFringe(mp, x1, y1, x2, y2, rng);

    /* h. Rivers */
    if (cfg->params.fractal.rivers) {
        fractalCarveRivers(mp, heights, size, cfg, &riverSeed);
    }

    /* i. Lakes */
    if (cfg->params.fractal.lakes && cfg->params.fractal.rivers) {
        fractalFillLakes(mp, heights, size, cfg);
    }

    /* j. Place objects */
    fractalPlaceObjects(mp, bs, pb, ss, heights, size, cfg, &placeSeed);

    /* k. Mines */
    if (cfg->params.fractal.mineDensityPct > 0) {
        int mPct = cfg->params.fractal.mineDensityPct;
        for (int x = x1; x <= x2; x++) {
            for (int y = y1; y <= y2; y++) {
                BYTE t = mp->mapItem[x][y];
                if (t < SWAMP || t > GRASS) continue;
                int effectivePct = mPct;
                int nearObjR2 = 8 * 8;
                for (int i = 0; i < (int)bs->numBases; i++) {
                    int ddx = x - (int)bs->item[i].x;
                    int ddy = y - (int)bs->item[i].y;
                    if (ddx * ddx + ddy * ddy < nearObjR2) { effectivePct = mPct * 2; break; }
                }
                if (effectivePct == mPct) {
                    for (int i = 0; i < (int)pb->numPills; i++) {
                        int ddx = x - (int)pb->item[i].x;
                        int ddy = y - (int)pb->item[i].y;
                        if (ddx * ddx + ddy * ddy < nearObjR2) { effectivePct = mPct * 2; break; }
                    }
                }
                if (effectivePct > 100) effectivePct = 100;
                if ((int)(xorshift32(rng) % 100) < effectivePct)
                    mp->mapItem[x][y] = (BYTE)(t + MINE_SUBTRACT);
            }
        }
    }

    /* l. Free heightmap */
    free(heights);
}

void mapGenRun(struct mapObj *mp, struct basesObj *bs,
               struct pillsObj *pb, struct startsObj *ss,
               const MapGenConfig *cfg) {
    uint32_t rng = cfg->seed;
    /* Ensure seed is nonzero for xorshift */
    if (rng == 0) rng = 1;

    /* Clear existing objects in the region */
    clearObjectsInRegion(bs, pb, ss, cfg);

    /* Dispatch to the appropriate generator */
    switch (cfg->genType) {
        case MAPGEN_TOURNAMENT:
            mapGenTournament(mp, bs, pb, ss, cfg, &rng);
            /* Water barrier: paint a RIVER (shallow-water) rim N
             * tiles thick around every landmass, where N comes from
             * the Tournament params. Each iteration is an 8-neighbour
             * dilation of "anything that is not DEEP_SEA" — pass 1
             * converts deep-water tiles touching land to RIVER,
             * pass 2 converts deep-water tiles touching those, and
             * so on. Capped at 5 so it can't drown a small map. */
            {
                int barrier = cfg->params.tournament.waterBarrier;
                if (barrier < 0) barrier = 0;
                if (barrier > 5) barrier = 5;
                for (int pass = 0; pass < barrier; pass++) {
                    /* Two-buffer dilation pass — collect the set of
                     * DEEP_SEA tiles adjacent (8-neighbour) to any
                     * non-DEEP_SEA tile, then convert them after the
                     * sweep so we don't grow the rim mid-iteration. */
                    BYTE toRiver[256][256];
                    memset(toRiver, 0, sizeof(toRiver));
                    for (int x = cfg->x1; x <= cfg->x2; x++) {
                        for (int y = cfg->y1; y <= cfg->y2; y++) {
                            if (mp->mapItem[x][y] != DEEP_SEA) continue;
                            bool touch = false;
                            for (int dy = -1; dy <= 1 && !touch; dy++) {
                                for (int dx = -1; dx <= 1 && !touch; dx++) {
                                    if (dx == 0 && dy == 0) continue;
                                    int nx = x + dx, ny = y + dy;
                                    if (nx < 0 || nx > 255 ||
                                        ny < 0 || ny > 255) continue;
                                    if (mp->mapItem[nx][ny] != DEEP_SEA) {
                                        touch = true;
                                    }
                                }
                            }
                            if (touch) toRiver[x][y] = 1;
                        }
                    }
                    for (int x = cfg->x1; x <= cfg->x2; x++) {
                        for (int y = cfg->y1; y <= cfg->y2; y++) {
                            if (toRiver[x][y]) mp->mapItem[x][y] = RIVER;
                        }
                    }
                }
            }
            break;
        case MAPGEN_NATURAL:
            mapGenNatural(mp, bs, pb, ss, cfg, &rng);
            break;
        case MAPGEN_MAZE:
            mapGenMaze(mp, bs, pb, ss, cfg, &rng);
            break;
        case MAPGEN_FRACTAL:
            mapGenFractal(mp, bs, pb, ss, cfg, &rng);
            break;
        default:
            /* Unknown generator — just clear to deep sea */
            for (int x = cfg->x1; x <= cfg->x2; x++) {
                for (int y = cfg->y1; y <= cfg->y2; y++) {
                    mp->mapItem[x][y] = DEEP_SEA;
                }
            }
            break;
    }

    /* Orient all starts toward nearest land */
    mapGenPointStartsToLand(mp, ss);
}

MapGenConfig mapGenDefaultConfig(int genType) {
    MapGenConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Default region: full playable area */
    cfg.x1 = MAP_MINE_EDGE_LEFT + 1;
    cfg.y1 = MAP_MINE_EDGE_TOP + 1;
    cfg.x2 = MAP_MINE_EDGE_RIGHT - 1;
    cfg.y2 = MAP_MINE_EDGE_BOTTOM - 1;

    cfg.seed = 0; /* Caller should set a random seed */
    cfg.genType = genType;
    cfg.bases = 16;
    cfg.pills = 16;
    cfg.starts = 16;

    switch (genType) {
        case MAPGEN_TOURNAMENT:
            cfg.params.tournament.symmetryMode = MAPGEN_SYM_4CORNER;
            cfg.params.tournament.landMassPct = 5;
            cfg.params.tournament.roughness = MAPGEN_ROUGH_MEDIUM;
            cfg.params.tournament.includeRoads = true;
            cfg.params.tournament.waterBarrier = 0;
            break;
        case MAPGEN_NATURAL:
            cfg.params.natural.mapStyle = MAPGEN_STYLE_OCEAN;
            cfg.params.natural.grassPct = 30;
            cfg.params.natural.forestPct = 25;
            cfg.params.natural.buildingPct = 10;
            cfg.params.natural.swampPct = 5;
            cfg.params.natural.riverPct = 5;
            cfg.params.natural.boatPct = 20;
            cfg.params.natural.mineDensityPct = 0;
            cfg.params.natural.riverCount = 2;
            cfg.params.natural.cityCount = 3;
            cfg.params.natural.mazeCount = 0;
            break;
        case MAPGEN_MAZE:
            cfg.params.maze.algo = MAZE_ALGO_LABYRINTH;
            cfg.params.maze.wallThick = 1;
            cfg.params.maze.corridorWidth = 1;
            cfg.params.maze.entries = 2;
            cfg.params.maze.cityRooms = 0;
            cfg.params.maze.wallTerrain = BUILDING;
            cfg.params.maze.corridorTerrain = ROAD;
            break;
        case MAPGEN_FRACTAL:
            cfg.params.fractal.landPct = 35;
            cfg.params.fractal.roughness = 5;
            cfg.params.fractal.detail = 3;
            cfg.params.fractal.coastJaggedness = 5;
            cfg.params.fractal.terrainLayers = 4;
            cfg.params.fractal.rivers = true;
            cfg.params.fractal.lakes = true;
            cfg.params.fractal.mineDensityPct = 0;
            cfg.bases = 12;
            cfg.pills = 16;
            cfg.starts = 8;
            break;
        default:
            break;
    }

    return cfg;
}

void mapGenNaturalStyleDefaults(int mapStyle, MapGenConfig *cfg) {
    cfg->params.natural.mapStyle = mapStyle;
    switch (mapStyle) {
        case MAPGEN_STYLE_OCEAN:
            cfg->params.natural.grassPct = 30;
            cfg->params.natural.forestPct = 25;
            cfg->params.natural.buildingPct = 10;
            cfg->params.natural.swampPct = 5;
            cfg->params.natural.riverPct = 5;
            cfg->params.natural.boatPct = 20;
            cfg->params.natural.mineDensityPct = 0;
            cfg->params.natural.riverCount = 2;
            cfg->params.natural.cityCount = 3;
            cfg->params.natural.mazeCount = 0;
            break;
        case MAPGEN_STYLE_CONTINENT:
            cfg->params.natural.grassPct = 10;
            cfg->params.natural.forestPct = 50;
            cfg->params.natural.buildingPct = 15;
            cfg->params.natural.swampPct = 5;
            cfg->params.natural.riverPct = 12;
            cfg->params.natural.boatPct = 20;
            cfg->params.natural.mineDensityPct = 0;
            cfg->params.natural.riverCount = 3;
            cfg->params.natural.cityCount = 5;
            cfg->params.natural.mazeCount = 0;
            break;
        case MAPGEN_STYLE_ISLANDS:
            cfg->params.natural.grassPct = 30;
            cfg->params.natural.forestPct = 25;
            cfg->params.natural.buildingPct = 10;
            cfg->params.natural.swampPct = 5;
            cfg->params.natural.riverPct = 5;
            cfg->params.natural.boatPct = 20;
            cfg->params.natural.mineDensityPct = 1;
            cfg->params.natural.riverCount = 1;
            cfg->params.natural.cityCount = 2;
            cfg->params.natural.mazeCount = 0;
            break;
        case MAPGEN_STYLE_ARCHIPELAGO:
            cfg->params.natural.grassPct = 30;
            cfg->params.natural.forestPct = 20;
            cfg->params.natural.buildingPct = 5;
            cfg->params.natural.swampPct = 10;
            cfg->params.natural.riverPct = 5;
            cfg->params.natural.boatPct = 25;
            cfg->params.natural.mineDensityPct = 3;
            cfg->params.natural.riverCount = 1;
            cfg->params.natural.cityCount = 2;
            cfg->params.natural.mazeCount = 0;
            break;
        case MAPGEN_STYLE_INLAND:
            cfg->params.natural.grassPct = 40;
            cfg->params.natural.forestPct = 30;
            cfg->params.natural.buildingPct = 10;
            cfg->params.natural.swampPct = 5;
            cfg->params.natural.riverPct = 10;
            cfg->params.natural.boatPct = 15;
            cfg->params.natural.mineDensityPct = 0;
            cfg->params.natural.riverCount = 3;
            cfg->params.natural.cityCount = 4;
            cfg->params.natural.mazeCount = 0;
            break;
        default:
            break;
    }
}

/*---------------------------------------------------------
 * mapGenPointStartsToLand
 *   For each start, compute the direction toward the nearest
 *   land mass by finding the center-of-mass of all non-deep-sea
 *   tiles within a search radius, then store in Bolo map format.
 *---------------------------------------------------------*/
void mapGenPointStartsToLand(struct mapObj *mp, struct startsObj *ss) {
    #define MGPL_RADIUS 30
    int i;

    for (i = 0; i < ss->numStarts; i++) {
        int sx = ss->item[i].x;
        int sy = ss->item[i].y;
        float totalX = 0, totalY = 0;
        int count = 0;
        int dx, dy;

        for (dx = -MGPL_RADIUS; dx <= MGPL_RADIUS; dx++) {
            int nx = sx + dx;
            if (nx < 0 || nx > 255) continue;
            for (dy = -MGPL_RADIUS; dy <= MGPL_RADIUS; dy++) {
                int ny = sy + dy;
                if (ny < 0 || ny > 255) continue;
                if (dx * dx + dy * dy > MGPL_RADIUS * MGPL_RADIUS) continue;
                if (mp->mapItem[nx][ny] != DEEP_SEA) {
                    totalX += (float)nx;
                    totalY += (float)ny;
                    count++;
                }
            }
        }

        if (count > 0) {
            float cx = totalX / (float)count;
            float cy = totalY / (float)count;
            float fdx = cx - (float)sx;
            float fdy = cy - (float)sy;
            if (fdx != 0.0f || fdy != 0.0f) {
                float angle = atan2f(fdx, -fdy);
                if (angle < 0.0f) angle += 6.283185307f;
                int dir = (int)(angle / 6.283185307f * 16.0f + 0.5f) % 16;
                ss->item[i].dir = startsConvertDir((BYTE)dir);
                continue;
            }
        }
        ss->item[i].dir = 0;
    }
    #undef MGPL_RADIUS
}

/*---------------------------------------------------------
 * mapGenRunAsPreview
 *   Generates a fresh map / pills / bases / starts quartet
 *   from cfg, optionally serializes the result into outBuf
 *   via mapSaveCompressedMap, and returns an owning
 *   MapPreview built by deserializing the same buffer.
 *
 *   The local substructs are torn down before return; the
 *   MapPreview holds its own copies that callers free with
 *   clientMapPreviewDestroy. Returning the deserialized
 *   preview (rather than wrapping the local heap structs)
 *   keeps lifetime simple and reuses the same loader path
 *   that handles .map files.
 *---------------------------------------------------------*/
struct MapPreview *mapGenRunAsPreview(const MapGenConfig *cfg,
                                      BYTE *outBuf,
                                      int outBufCap,
                                      int *outCompressedLen) {
    if (cfg == NULL) {
        if (outCompressedLen != NULL) *outCompressedLen = 0;
        return NULL;
    }

    map        mp;
    pillboxes  pb;
    bases      bs;
    starts     ss;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    memset((*mp).mapItem, DEEP_SEA, sizeof((*mp).mapItem));
    pb->numPills = 0;
    bs->numBases = 0;
    ss->numStarts = 0;

    /* Local non-const copy so the generator can mutate its inputs. */
    MapGenConfig localCfg = *cfg;
    mapGenRun(mp, bs, pb, ss, &localCfg);

    /* Always serialize — both to populate outBuf for the caller (if
     * provided) and to drive the MapPreview construction below. Use a
     * scratch buffer when the caller doesn't provide one. */
    BYTE  scratch[MAP_COMPRESSED_MAX_SIZE];
    BYTE *serBuf  = outBuf;
    int   serCap  = outBufCap;
    if (serBuf == NULL || serCap < (int)sizeof(scratch)) {
        serBuf = scratch;
        serCap = (int)sizeof(scratch);
    }
    int len = mapSaveCompressedMap(&mp, &pb, &bs, &ss, serBuf, serCap);

    if (outCompressedLen != NULL) {
        *outCompressedLen = (outBuf != NULL && serBuf == outBuf) ? len : 0;
    }
    if (outBuf != NULL && serBuf != outBuf && len > 0 && len <= outBufCap) {
        /* Caller provided a buffer smaller than scratch but large enough
         * for this map — copy the serialized bytes across. */
        memcpy(outBuf, serBuf, (size_t)len);
        if (outCompressedLen != NULL) *outCompressedLen = len;
    }

    MapPreview *preview = NULL;
    if (len > 0) {
        preview = clientMapPreviewLoadFromBuffer(serBuf, len);
    }

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);

    return preview;
}
