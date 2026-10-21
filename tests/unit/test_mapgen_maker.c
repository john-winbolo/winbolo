/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Random map generator: the Size control and the Map Maker presets.
 *
 * Size is shared by every generator type. It must stay inside its
 * range, put the land in a centred square, and leave the rest of the
 * region as deep sea. A full-size config must write the same seed
 * string as it did before Size existed, so that old seeds still make
 * the same maps.
 *
 * Each Map Maker preset must give the same map for the same seed, keep
 * every object count inside the game's limits, keep every object and
 * every land tile inside the region, use only real terrain codes, and
 * let a tank reach every base and pillbox from a start.
 *
 * Fixed seeds must make fixed maps. The older types are checked against
 * map hashes taken from the code before Map Maker was added, and Map
 * Maker against hashes taken when its maps were made with integer
 * angles and noise, so that a change to either shows here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapgen.h"
#include "types.h"

#include "test_harness.h"

#define REGION_X1 (MAP_MINE_EDGE_LEFT + 1)
#define REGION_Y1 (MAP_MINE_EDGE_TOP + 1)
#define REGION_X2 (MAP_MINE_EDGE_RIGHT - 1)
#define REGION_Y2 (MAP_MINE_EDGE_BOTTOM - 1)

typedef struct {
    struct mapObj    *mp;
    struct basesObj  *bs;
    struct pillsObj  *pb;
    struct startsObj *ss;
} GenOut;

static void setRegion(MapGenConfig *cfg) {
    cfg->x1 = REGION_X1;
    cfg->y1 = REGION_Y1;
    cfg->x2 = REGION_X2;
    cfg->y2 = REGION_Y2;
}

static bool genAlloc(GenOut *g) {
    g->mp = (struct mapObj *)calloc(1, sizeof(struct mapObj));
    g->bs = (struct basesObj *)calloc(1, sizeof(struct basesObj));
    g->pb = (struct pillsObj *)calloc(1, sizeof(struct pillsObj));
    g->ss = (struct startsObj *)calloc(1, sizeof(struct startsObj));
    return g->mp && g->bs && g->pb && g->ss;
}

static void genFree(GenOut *g) {
    free(g->mp);
    free(g->bs);
    free(g->pb);
    free(g->ss);
    memset(g, 0, sizeof(*g));
}

/* Same start state as the server's random map path. */
static void genRun(GenOut *g, const MapGenConfig *cfg) {
    memset(g->mp->mapItem, DEEP_SEA, sizeof(g->mp->mapItem));
    g->bs->numBases = 0;
    g->pb->numPills = 0;
    g->ss->numStarts = 0;
    mapGenRun(g->mp, g->bs, g->pb, g->ss, cfg);
}

static bool genSame(const GenOut *a, const GenOut *b) {
    if (memcmp(a->mp->mapItem, b->mp->mapItem, sizeof(a->mp->mapItem)) != 0) return false;
    if (a->bs->numBases != b->bs->numBases) return false;
    if (a->pb->numPills != b->pb->numPills) return false;
    if (a->ss->numStarts != b->ss->numStarts) return false;
    for (int i = 0; i < a->bs->numBases; i++) {
        if (a->bs->item[i].x != b->bs->item[i].x || a->bs->item[i].y != b->bs->item[i].y) return false;
    }
    for (int i = 0; i < a->pb->numPills; i++) {
        if (a->pb->item[i].x != b->pb->item[i].x || a->pb->item[i].y != b->pb->item[i].y) return false;
    }
    for (int i = 0; i < a->ss->numStarts; i++) {
        if (a->ss->item[i].x != b->ss->item[i].x || a->ss->item[i].y != b->ss->item[i].y) return false;
    }
    return true;
}

static bool inBox(int x, int y, int x1, int y1, int x2, int y2) {
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

/* Every object and every tile that is not deep sea must be inside the
 * effective region. Returns a short reason, or NULL when the map is
 * inside. */
static const char *checkInside(const GenOut *g, const MapGenConfig *cfg) {
    int x1, y1, x2, y2;
    mapGenEffectiveRegion(cfg, &x1, &y1, &x2, &y2);
    for (int x = 0; x < 256; x++) {
        for (int y = 0; y < 256; y++) {
            if (g->mp->mapItem[x][y] != DEEP_SEA && !inBox(x, y, x1, y1, x2, y2)) {
                return "a tile that is not deep sea is outside the region";
            }
        }
    }
    for (int i = 0; i < g->bs->numBases; i++) {
        if (!inBox(g->bs->item[i].x, g->bs->item[i].y, x1, y1, x2, y2)) return "a base is outside the region";
    }
    for (int i = 0; i < g->pb->numPills; i++) {
        if (!inBox(g->pb->item[i].x, g->pb->item[i].y, x1, y1, x2, y2)) return "a pillbox is outside the region";
    }
    for (int i = 0; i < g->ss->numStarts; i++) {
        if (!inBox(g->ss->item[i].x, g->ss->item[i].y, x1, y1, x2, y2)) return "a start is outside the region";
    }
    return NULL;
}

/* Terrain codes from types.h: 0..9 plain, 10..15 mined swamp to mined
 * grass (terrain + MINE_SUBTRACT), and deep sea. */
static bool terrainValid(int t) {
    if (t == DEEP_SEA) return true;
    if (t >= BUILDING && t <= BOAT) return true;
    return t >= SWAMP + MINE_SUBTRACT && t <= GRASS + MINE_SUBTRACT;
}

static bool tankBlocks(int t) {
    return t == BUILDING || t == HALFBUILDING;
}

/* Every base and pillbox can be reached by a tank from a start, by the
 * game's rules for boats (see next_boat_state in brain_pathfinder.c):
 * a tank starts in a boat; a tank on a boat tile has a boat; a tank in
 * a boat stays in it on river and deep sea and leaves it on land. Deep
 * sea without a boat drowns the tank, and walls block. The search is
 * over (tile, boat) states. A boat that is left behind on the water
 * needs no model: the tank can only take it again from the tile where
 * it left it, and that tile was already reached with a boat.
 * Returns the number of objects that cannot be reached. */
static int unreachableCount(const GenOut *g, const MapGenConfig *cfg) {
    static uint8_t seen[2][256][256];
    static int queue[2 * 256 * 256];
    int x1, y1, x2, y2, head = 0, tail = 0, bad = 0;
    mapGenEffectiveRegion(cfg, &x1, &y1, &x2, &y2);
    memset(seen, 0, sizeof(seen));
    if (g->ss->numStarts > 0) {
        for (int i = 0; i < g->ss->numStarts; i++) {
            int sx = g->ss->item[i].x, sy = g->ss->item[i].y;
            if (seen[1][sx][sy]) continue;
            seen[1][sx][sy] = 1;
            queue[tail++] = (1 << 16) | (sx << 8) | sy;
        }
    } else if (g->bs->numBases > 0) {
        seen[0][g->bs->item[0].x][g->bs->item[0].y] = 1;
        queue[tail++] = (g->bs->item[0].x << 8) | g->bs->item[0].y;
    } else if (g->pb->numPills > 0) {
        seen[0][g->pb->item[0].x][g->pb->item[0].y] = 1;
        queue[tail++] = (g->pb->item[0].x << 8) | g->pb->item[0].y;
    } else {
        return 0;
    }
    while (head < tail) {
        int k = queue[head++], boat = k >> 16, x = (k >> 8) & 0xFF, y = k & 0xFF;
        static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; d++) {
            int nx = x + dx[d], ny = y + dy[d], t, nb;
            if (!inBox(nx, ny, x1, y1, x2, y2)) continue;
            t = g->mp->mapItem[nx][ny];
            if (tankBlocks(t)) continue;
            nb = (t == BOAT) || (boat && (t == RIVER || t == DEEP_SEA));
            if (t == DEEP_SEA && !nb) continue;
            if (seen[nb][nx][ny]) continue;
            seen[nb][nx][ny] = 1;
            queue[tail++] = (nb << 16) | (nx << 8) | ny;
        }
    }
    for (int i = 0; i < g->bs->numBases; i++) {
        int x = g->bs->item[i].x, y = g->bs->item[i].y;
        if (!seen[0][x][y] && !seen[1][x][y]) bad++;
    }
    for (int i = 0; i < g->pb->numPills; i++) {
        int x = g->pb->item[i].x, y = g->pb->item[i].y;
        if (!seen[0][x][y] && !seen[1][x][y]) bad++;
    }
    return bad;
}

/* FNV-1a over the map, then the count and place of each base, pillbox
 * and start (and each start's direction). */
static uint32_t fnvBytes(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t genHash(const GenOut *g) {
    uint32_t h = 2166136261u;
    uint8_t v[3];
    h = fnvBytes(h, g->mp->mapItem, sizeof(g->mp->mapItem));
    v[0] = (uint8_t)g->bs->numBases;
    h = fnvBytes(h, v, 1);
    for (int i = 0; i < g->bs->numBases; i++) {
        v[0] = g->bs->item[i].x; v[1] = g->bs->item[i].y;
        h = fnvBytes(h, v, 2);
    }
    v[0] = (uint8_t)g->pb->numPills;
    h = fnvBytes(h, v, 1);
    for (int i = 0; i < g->pb->numPills; i++) {
        v[0] = g->pb->item[i].x; v[1] = g->pb->item[i].y;
        h = fnvBytes(h, v, 2);
    }
    v[0] = (uint8_t)g->ss->numStarts;
    h = fnvBytes(h, v, 1);
    for (int i = 0; i < g->ss->numStarts; i++) {
        v[0] = g->ss->item[i].x; v[1] = g->ss->item[i].y; v[2] = g->ss->item[i].dir;
        h = fnvBytes(h, v, 3);
    }
    return h;
}

typedef struct {
    const char *seed;
    uint32_t hash;
} SeedHash;

/* Decode each seed into the default config of its type, make the map
 * the way the server does, and compare the hash. Returns the number of
 * seeds that do not match, and prints each one. */
static int checkSeedHashes(GenOut *g, const SeedHash *list, size_t count, int type) {
    int bad = 0;
    for (size_t i = 0; i < count; i++) {
        MapGenConfig cfg = mapGenDefaultConfig(type);
        uint32_t h;
        if (!mapGenSeedToConfig(list[i].seed, &cfg)) {
            printf("  seed '%s' does not decode\n", list[i].seed);
            bad++;
            continue;
        }
        genRun(g, &cfg);
        h = genHash(g);
        if (h != list[i].hash) {
            printf("  { \"%s\", 0x%08Xu }, (expected 0x%08Xu)\n", list[i].seed, h, list[i].hash);
            bad++;
        }
    }
    return bad;
}

/*---------------------------------------------------------*/

int run_mapgen_size_bounds(void) {
    MapGenConfig cfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
    int x1, y1, x2, y2;
    setRegion(&cfg);

    /* Full uses the whole region. */
    cfg.size = MAPGEN_SIZE_FULL;
    mapGenEffectiveRegion(&cfg, &x1, &y1, &x2, &y2);
    UT_ASSERT(x1 == REGION_X1 && y1 == REGION_Y1 && x2 == REGION_X2 && y2 == REGION_Y2);

    /* Every size in range gives a centred square of that side. */
    for (int s = MAPGEN_SIZE_MIN; s <= MAPGEN_SIZE_MAX; s++) {
        cfg.size = s;
        mapGenEffectiveRegion(&cfg, &x1, &y1, &x2, &y2);
        UT_ASSERT_MSG(x2 - x1 + 1 == s && y2 - y1 + 1 == s,
                      "size %d gave %dx%d", s, x2 - x1 + 1, y2 - y1 + 1);
        UT_ASSERT_MSG(x1 >= REGION_X1 && y1 >= REGION_Y1 && x2 <= REGION_X2 && y2 <= REGION_Y2,
                      "size %d leaves the region", s);
        UT_ASSERT_MSG(abs((x1 - REGION_X1) - (REGION_X2 - x2)) <= 1,
                      "size %d is not centred", s);
    }

    /* Out-of-range sizes are clamped, never used as they are. */
    cfg.size = 3;
    mapGenClampConfig(&cfg);
    UT_ASSERT(cfg.size == MAPGEN_SIZE_MIN);
    cfg.size = 9999;
    mapGenClampConfig(&cfg);
    UT_ASSERT(cfg.size == MAPGEN_SIZE_MAX);
    cfg.size = -5;
    mapGenEffectiveRegion(&cfg, &x1, &y1, &x2, &y2);
    UT_ASSERT(x2 - x1 + 1 == MAPGEN_SIZE_MIN);

    /* Size travels in the seed string for every generator type. */
    for (int type = 0; type < MAPGEN_TYPE_COUNT; type++) {
        static const int sizes[] = { MAPGEN_SIZE_FULL, MAPGEN_SIZE_MIN, 100, MAPGEN_SIZE_MAX };
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            MapGenConfig a = mapGenDefaultConfig(type), b;
            char seed[64];
            a.seed = 0x1234ABCDu;
            a.size = sizes[i];
            mapGenConfigToSeed(&a, seed, sizeof(seed));
            UT_ASSERT_MSG(strlen(seed) <= 30, "seed '%s' is too long for a map name", seed);
            b = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
            b.size = 77;
            UT_ASSERT_MSG(mapGenSeedToConfig(seed, &b), "seed '%s' does not decode", seed);
            UT_ASSERT_MSG(b.size == sizes[i], "seed '%s' gave size %d, wanted %d", seed, b.size, sizes[i]);
            UT_ASSERT_MSG(b.genType == type, "seed '%s' gave type %d", seed, b.genType);
            if (type == MAPGEN_NATURAL) {
                /* A sized Natural seed starts with Q instead. */
                UT_ASSERT_MSG(seed[0] == (sizes[i] != MAPGEN_SIZE_FULL ? 'Q' : 'N'),
                              "seed '%s' has the wrong first letter", seed);
                UT_ASSERT(strchr(seed, 'Z') == NULL);
                /* Every size survives, not only the ones above. */
                for (int sz = MAPGEN_SIZE_MIN; sz <= MAPGEN_SIZE_MAX; sz++) {
                    MapGenConfig c = a, d = mapGenDefaultConfig(MAPGEN_MAZE);
                    char s2[64], s3[64];
                    c.size = sz;
                    mapGenConfigToSeed(&c, s2, sizeof(s2));
                    UT_ASSERT_MSG(strlen(s2) == 30, "seed '%s' is not 30 characters", s2);
                    UT_ASSERT(mapGenSeedToConfig(s2, &d));
                    UT_ASSERT_MSG(d.size == sz && d.genType == MAPGEN_NATURAL && d.seed == c.seed,
                                  "seed '%s' gave size %d", s2, d.size);
                    UT_ASSERT(memcmp(&d.params.natural, &c.params.natural, sizeof(c.params.natural)) == 0);
                    d.size = MAPGEN_SIZE_FULL;
                    c.size = MAPGEN_SIZE_FULL;
                    mapGenConfigToSeed(&c, s2, sizeof(s2));
                    mapGenConfigToSeed(&d, s3, sizeof(s3));
                    UT_ASSERT_MSG(strcmp(s2, s3) == 0, "'%s' and '%s' differ", s2, s3);
                }
            } else {
                UT_ASSERT_MSG((strchr(seed, 'Z') != NULL) == (sizes[i] != MAPGEN_SIZE_FULL),
                              "seed '%s': the Z suffix must be there only for a size that is not full", seed);
            }
        }
    }

    /* Bad size suffixes are refused. */
    {
        MapGenConfig a = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
        char seed[64], bad[80];
        a.seed = 42;
        mapGenConfigToSeed(&a, seed, sizeof(seed));
        snprintf(bad, sizeof(bad), "%sZ", seed);
        UT_ASSERT(!mapGenSeedToConfig(bad, &a));
        snprintf(bad, sizeof(bad), "%sZ1", seed);
        UT_ASSERT(!mapGenSeedToConfig(bad, &a));
        snprintf(bad, sizeof(bad), "%sZ123", seed);
        UT_ASSERT(!mapGenSeedToConfig(bad, &a));
        snprintf(bad, sizeof(bad), "%sZQQ", seed);
        UT_ASSERT(!mapGenSeedToConfig(bad, &a));
        /* Q seeds: the last digit holds 3 bits, and non-hex is refused. */
        UT_ASSERT(!mapGenSeedToConfig("Q00000000000000000000000000038", &a));
        UT_ASSERT(!mapGenSeedToConfig("Q0000000000000000000000000003G", &a));
        UT_ASSERT(!mapGenSeedToConfig("Q000000000000000000000000000X3", &a));
    }
    return 0;
}

/* A config at full size writes a seed with no size part, and decoding
 * it gives back the same config, for the four older generator types. */
/* Seeds and map hashes made with the code from before Map Maker and
 * Size were added. Each type is run at three seeds; Tournament also
 * at each symmetry (with three water barriers) and Natural at each
 * style. The hashes were made with MSVC on Windows x64. Clang 18
 * (clang-cl, -O2, on Windows x64, with and without AVX2 and fused
 * multiply-add) gave the same hashes.
 *
 * The older types are not built for the same map on every compiler.
 * They use float arithmetic and atan2f (start directions and the
 * Tournament start spread in mapgen.c), and mapgen.c is not built with
 * -ffp-contract=off. If these hashes differ on another platform, look
 * there first. That was so before Map Maker was added, and this test
 * does not change it. */
static const SeedHash kLegacyTournament[] = {
    { "T02B8420000000020", 0x59F8CAA7u },
    { "T02B842181FFDC220", 0x6E91B73Au },
    { "T02B84200EB79A2A0", 0x9EC60985u },
    { "T12B8420000000024", 0xE4222169u },
    { "T12B842181FFDC224", 0x710C8C2Eu },
    { "T12B84200EB79A2A4", 0xBCAA9248u },
    { "T22B8420000000028", 0x6E3B190Fu },
    { "T22B842181FFDC228", 0x3619C711u },
    { "T22B84200EB79A2A8", 0x847FC7C0u },
    { "T32B8420000000020", 0x074ECD2Du },
    { "T32B842181FFDC220", 0xC10D1716u },
    { "T32B84200EB79A2A0", 0x6BA7012Bu },
    { "T42B8420000000024", 0x65E93011u },
    { "T42B842181FFDC224", 0x5DF7ED38u },
    { "T42B84200EB79A2A4", 0x157B0173u },
};
static const SeedHash kLegacyNatural[] = {
    { "N83C645050A502310840000000040", 0xFC5B23F6u },
    { "N83C645050A50231084303FFB8440", 0xA87B35FDu },
    { "N83C645050A5023108401D6F34540", 0x86FA8479u },
    { "N914C878518503510840000000040", 0x84AE257Fu },
    { "N914C87851850351084303FFB8440", 0x70E86B1Eu },
    { "N914C8785185035108401D6F34540", 0xF8ED1911u },
    { "NA3C645050A509210840000000040", 0x0C30FB43u },
    { "NA3C645050A50921084303FFB8440", 0xECA2A851u },
    { "NA3C645050A5092108401D6F34540", 0x75AEF297u },
    { "NB3C5028A0A659210840000000040", 0xEB9A0F3Bu },
    { "NB3C5028A0A65921084303FFB8440", 0x5047251Fu },
    { "NB3C5028A0A6592108401D6F34540", 0xE8C7C1F4u },
    { "NC5078505143C3410840000000040", 0x7B319AC9u },
    { "NC5078505143C341084303FFB8440", 0xFF3C3F5Du },
    { "NC5078505143C34108401D6F34540", 0xDCFAA5C8u },
};
static const SeedHash kLegacyMaze[] = {
    { "M050840020000000080", 0xCD872BC6u },
    { "M05084002607FF70880", 0x070E158Du },
    { "M0508400203ADE68A80", 0xD77ECC11u },
};
static const SeedHash kLegacyFractal[] = {
    { "F46AD6619040000000080", 0x1CD43C51u },
    { "F46AD661904607FF70880", 0xE6D17DDCu },
    { "F46AD66190403ADE68A80", 0xD9828E65u },
};

int run_mapgen_legacy_seed_unchanged(void) {
    /* The default config of each older type, at a fixed seed, writes
     * the same seed string as before. */
    static const char *const kDefaultSeeds[MAPGEN_LEGACY_TYPE_COUNT] = {
        "T02B842181FFDC220",
        "N83C645050A50231084303FFB8440",
        "M05084002607FF70880",
        "F46AD661904607FF70880",
    };
    GenOut g;
    int bad = 0;
    for (int type = 0; type < MAPGEN_LEGACY_TYPE_COUNT; type++) {
        MapGenConfig a = mapGenDefaultConfig(type), b;
        char seed[64], again[64];
        UT_ASSERT_MSG(a.size == MAPGEN_SIZE_FULL, "type %d does not default to full size", type);
        a.seed = 0xC0FFEE11u;
        mapGenConfigToSeed(&a, seed, sizeof(seed));
        UT_ASSERT_MSG(strcmp(seed, kDefaultSeeds[type]) == 0, "type %d wrote '%s', not '%s'",
                      type, seed, kDefaultSeeds[type]);
        b = mapGenDefaultConfig(MAPGEN_MAKER);
        UT_ASSERT(mapGenSeedToConfig(seed, &b));
        UT_ASSERT(b.size == MAPGEN_SIZE_FULL);
        mapGenConfigToSeed(&b, again, sizeof(again));
        UT_ASSERT_MSG(strcmp(seed, again) == 0, "'%s' came back as '%s'", seed, again);
    }

    /* The same seeds make the same maps as before. */
    UT_ASSERT(genAlloc(&g));
    bad += checkSeedHashes(&g, kLegacyTournament, sizeof(kLegacyTournament) / sizeof(kLegacyTournament[0]), MAPGEN_TOURNAMENT);
    bad += checkSeedHashes(&g, kLegacyNatural, sizeof(kLegacyNatural) / sizeof(kLegacyNatural[0]), MAPGEN_NATURAL);
    bad += checkSeedHashes(&g, kLegacyMaze, sizeof(kLegacyMaze) / sizeof(kLegacyMaze[0]), MAPGEN_MAZE);
    bad += checkSeedHashes(&g, kLegacyFractal, sizeof(kLegacyFractal) / sizeof(kLegacyFractal[0]), MAPGEN_FRACTAL);
    genFree(&g);
    UT_ASSERT_MSG(bad == 0, "%d older-type seeds made a different map", bad);

    /* A plain number seed only changes the seed. */
    {
        MapGenConfig a = mapGenDefaultConfig(MAPGEN_MAKER);
        a.size = 90;
        UT_ASSERT(mapGenSeedToConfig("12345", &a));
        UT_ASSERT(a.seed == 12345u);
        UT_ASSERT(a.size == 90);
        UT_ASSERT(a.genType == MAPGEN_MAKER);
    }
    return 0;
}

/* Every preset, with its controls moved, comes back from its seed. */
int run_mapgen_maker_seed_roundtrip(void) {
    int count = mapGenPresetCount();
    UT_ASSERT(count >= 2);
    for (int p = 0; p < count; p++) {
        const char *key = mapGenPresetKey(p);
        UT_ASSERT_MSG(key != NULL && mapGenPresetFromKey(key) == p, "preset %d key does not look up", p);
        UT_ASSERT(mapGenPresetName(p) != NULL);
        for (int v = 0; v < 3; v++) {
            MapGenConfig a = mapGenDefaultConfig(MAPGEN_TOURNAMENT), b;
            char seed[64];
            mapGenApplyPreset(p, &a);
            UT_ASSERT(a.genType == MAPGEN_MAKER);
            UT_ASSERT(a.params.maker.preset == p);
            a.seed = 0x9E3779B9u * (uint32_t)(p + 1) + (uint32_t)v;
            if (v == 1) {
                a.params.maker.outline = MAPGEN_OUTLINE_COUNT - 1;
                a.params.maker.symmetry = MAPGEN_MSYM_COUNT - 1;
                a.params.maker.waterPct = 100;
                a.params.maker.forestPct = 0;
                a.params.maker.roadPct = 57;
                a.params.maker.buildingPct = 100;
                a.params.maker.islands = 8;
                a.params.maker.mineDensityPct = 5;
                a.params.maker.detail = 10;
                a.bases = 16; a.pills = 0; a.starts = 16;
                a.size = MAPGEN_SIZE_MIN;
            } else if (v == 2) {
                a.params.maker.outline = 0;
                a.params.maker.symmetry = 0;
                a.params.maker.waterPct = 0;
                a.params.maker.forestPct = 100;
                a.params.maker.roadPct = 0;
                a.params.maker.buildingPct = 0;
                a.params.maker.islands = 1;
                a.params.maker.mineDensityPct = 0;
                a.params.maker.detail = 0;
                a.bases = 0; a.pills = 16; a.starts = 1;
                a.size = MAPGEN_SIZE_FULL;
            }
            mapGenConfigToSeed(&a, seed, sizeof(seed));
            UT_ASSERT_MSG(seed[0] == 'K', "preset %d wrote '%s'", p, seed);
            UT_ASSERT_MSG(strlen(seed) <= 30, "seed '%s' is too long for a map name", seed);
            b = mapGenDefaultConfig(MAPGEN_NATURAL);
            UT_ASSERT_MSG(mapGenSeedToConfig(seed, &b), "seed '%s' does not decode", seed);
            UT_ASSERT(b.genType == MAPGEN_MAKER);
            UT_ASSERT(b.seed == a.seed);
            UT_ASSERT(b.size == a.size);
            UT_ASSERT(b.bases == a.bases && b.pills == a.pills && b.starts == a.starts);
            UT_ASSERT_MSG(memcmp(&a.params.maker, &b.params.maker, sizeof(a.params.maker)) == 0,
                          "preset %d variant %d: '%s' lost a control", p, v, seed);
        }
    }
    UT_ASSERT(mapGenPresetFromKey("BERSERKIR") == 0);
    UT_ASSERT(mapGenPresetFromKey("no-such-maker") == -1);
    UT_ASSERT(mapGenPresetFromKey("") == -1);
    return 0;
}

/* Same seed, same map, for every preset and at two sizes. */
int run_mapgen_maker_deterministic(void) {
    GenOut a, b;
    int differ = 0, pairs = 0;
    UT_ASSERT(genAlloc(&a) && genAlloc(&b));
    for (int p = 0; p < mapGenPresetCount(); p++) {
        static const int sizes[] = { MAPGEN_SIZE_FULL, 80 };
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            MapGenConfig cfg = mapGenDefaultConfig(MAPGEN_MAKER);
            mapGenApplyPreset(p, &cfg);
            setRegion(&cfg);
            cfg.size = sizes[s];
            cfg.seed = 1000u + (uint32_t)p;
            genRun(&a, &cfg);
            genRun(&b, &cfg);
            UT_ASSERT_MSG(genSame(&a, &b), "preset %d size %d: two runs differ", p, sizes[s]);
            cfg.seed = 2000u + (uint32_t)p;
            genRun(&b, &cfg);
            pairs++;
            if (!genSame(&a, &b)) differ++;
        }
    }
    genFree(&a);
    genFree(&b);
    UT_ASSERT_MSG(differ == pairs, "only %d of %d seed pairs gave different maps", differ, pairs);
    return 0;
}

/* Counts, bounds, terrain codes and reachability for every preset. */
int run_mapgen_maker_valid(void) {
    GenOut g;
    UT_ASSERT(genAlloc(&g));
    for (int p = 0; p < mapGenPresetCount(); p++) {
        static const int sizes[] = { MAPGEN_SIZE_FULL, MAPGEN_SIZE_MIN, 64, 150 };
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (uint32_t seed = 1; seed <= 4; seed++) {
                MapGenConfig cfg = mapGenDefaultConfig(MAPGEN_MAKER);
                const char *why;
                int bad;
                mapGenApplyPreset(p, &cfg);
                setRegion(&cfg);
                cfg.size = sizes[s];
                cfg.seed = seed * 7919u;
                if (seed == 4) {
                    /* The ends of every control at once. */
                    cfg.params.maker.waterPct = 100;
                    cfg.params.maker.buildingPct = 100;
                    cfg.params.maker.islands = 8;
                    cfg.params.maker.mineDensityPct = 5;
                    cfg.params.maker.detail = 10;
                    cfg.bases = 16; cfg.pills = 16; cfg.starts = 16;
                }
                genRun(&g, &cfg);
                UT_ASSERT_MSG(g.bs->numBases <= MAX_BASES && g.bs->numBases <= cfg.bases,
                              "preset %d: %d bases", p, g.bs->numBases);
                UT_ASSERT_MSG(g.pb->numPills <= MAX_PILLS && g.pb->numPills <= cfg.pills,
                              "preset %d: %d pillboxes", p, g.pb->numPills);
                UT_ASSERT_MSG(g.ss->numStarts <= MAX_STARTS && g.ss->numStarts <= cfg.starts,
                              "preset %d: %d starts", p, g.ss->numStarts);
                UT_ASSERT_MSG(g.ss->numStarts > 0 || cfg.starts == 0,
                              "preset %d size %d seed %u: no starts", p, sizes[s], cfg.seed);
                why = checkInside(&g, &cfg);
                UT_ASSERT_MSG(why == NULL, "preset %d size %d seed %u: %s", p, sizes[s], cfg.seed, why);
                for (int x = 0; x < 256; x++) {
                    for (int y = 0; y < 256; y++) {
                        UT_ASSERT_MSG(terrainValid(g.mp->mapItem[x][y]),
                                      "preset %d: terrain %d at %d,%d", p, g.mp->mapItem[x][y], x, y);
                    }
                }
                bad = unreachableCount(&g, &cfg);
                UT_ASSERT_MSG(bad == 0, "preset %d size %d seed %u: %d objects cannot be reached",
                              p, sizes[s], cfg.seed, bad);
                /* Bases and pillboxes stand on land, not in a wall. */
                for (int i = 0; i < g.bs->numBases; i++) {
                    int t = g.mp->mapItem[g.bs->item[i].x][g.bs->item[i].y];
                    UT_ASSERT_MSG(!tankBlocks(t) && t != DEEP_SEA, "preset %d: base on terrain %d", p, t);
                }
                for (int i = 0; i < g.pb->numPills; i++) {
                    int t = g.mp->mapItem[g.pb->item[i].x][g.pb->item[i].y];
                    UT_ASSERT_MSG(!tankBlocks(t) && t != DEEP_SEA, "preset %d: pillbox on terrain %d", p, t);
                }
            }
        }
    }
    genFree(&g);
    return 0;
}

/* Size works for the four older generator types too: the land and the
 * objects stay inside the smaller square. */
int run_mapgen_size_all_types(void) {
    GenOut g;
    UT_ASSERT(genAlloc(&g));
    for (int type = 0; type < MAPGEN_LEGACY_TYPE_COUNT; type++) {
        static const int sizes[] = { MAPGEN_SIZE_MIN, 60, 120 };
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            MapGenConfig cfg = mapGenDefaultConfig(type);
            const char *why;
            setRegion(&cfg);
            cfg.size = sizes[s];
            cfg.seed = 31337u;
            genRun(&g, &cfg);
            why = checkInside(&g, &cfg);
            UT_ASSERT_MSG(why == NULL, "type %d size %d: %s", type, sizes[s], why);
            UT_ASSERT_MSG(g.bs->numBases <= MAX_BASES && g.pb->numPills <= MAX_PILLS &&
                          g.ss->numStarts <= MAX_STARTS, "type %d size %d: too many objects", type, sizes[s]);
        }
    }
    genFree(&g);
    return 0;
}

/* Fraction (in percent) of land tiles that match the tile at the
 * mirror place, left to right, inside the box around the land. Mines
 * are not counted, as they are not part of the shape. */
static int mirrorMatchPct(const GenOut *g) {
    int x1 = 256, y1 = 256, x2 = -1, y2 = -1, same = 0, all = 0;
    for (int x = 0; x < 256; x++) {
        for (int y = 0; y < 256; y++) {
            if (g->mp->mapItem[x][y] == DEEP_SEA) continue;
            if (x < x1) x1 = x;
            if (x > x2) x2 = x;
            if (y < y1) y1 = y;
            if (y > y2) y2 = y;
        }
    }
    if (x2 < x1) return 100;
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            int a = g->mp->mapItem[x][y], b = g->mp->mapItem[x1 + x2 - x][y];
            if (a == DEEP_SEA && b == DEEP_SEA) continue;
            if (a != DEEP_SEA && a >= SWAMP + MINE_SUBTRACT && a <= GRASS + MINE_SUBTRACT) a -= MINE_SUBTRACT;
            if (b != DEEP_SEA && b >= SWAMP + MINE_SUBTRACT && b <= GRASS + MINE_SUBTRACT) b -= MINE_SUBTRACT;
            all++;
            if (a == b) same++;
        }
    }
    return all > 0 ? (same * 100) / all : 100;
}

/* The room kinds of the town presets (maze, checker, star, pond, house
 * cluster, hedge), the Slugfest seeds with no forest rim and the island
 * maze yards: many seeds, each one the same map twice, valid, and every
 * object can be reached. The Sluggo town maps keep the left to right
 * match. */
int run_mapgen_maker_room_kinds(void) {
    static const char *keys[] = { "sluggo-tournament", "slugdeath", "sluggo-symmetric",
                                  "slugfest", "bl-islands", "megawatt-islands",
                                  "sluggo-adventurous" };
    GenOut a, b;
    UT_ASSERT(genAlloc(&a) && genAlloc(&b));
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        int p = mapGenPresetFromKey(keys[k]);
        UT_ASSERT_MSG(p >= 0, "no preset with key %s", keys[k]);
        for (uint32_t seed = 1; seed <= 24; seed++) {
            MapGenConfig cfg = mapGenDefaultConfig(MAPGEN_MAKER);
            int bad;
            const char *why;
            mapGenApplyPreset(p, &cfg);
            setRegion(&cfg);
            cfg.seed = seed * 104729u;
            if (seed % 3 == 0) cfg.params.maker.detail = 10;
            genRun(&a, &cfg);
            genRun(&b, &cfg);
            UT_ASSERT_MSG(genSame(&a, &b), "%s seed %u: two runs differ", keys[k], cfg.seed);
            why = checkInside(&a, &cfg);
            UT_ASSERT_MSG(why == NULL, "%s seed %u: %s", keys[k], cfg.seed, why);
            for (int x = 0; x < 256; x++) {
                for (int y = 0; y < 256; y++) {
                    UT_ASSERT_MSG(terrainValid(a.mp->mapItem[x][y]),
                                  "%s: terrain %d at %d,%d", keys[k], a.mp->mapItem[x][y], x, y);
                }
            }
            bad = unreachableCount(&a, &cfg);
            UT_ASSERT_MSG(bad == 0, "%s seed %u: %d objects cannot be reached", keys[k], cfg.seed, bad);
            UT_ASSERT_MSG(a.ss->numStarts > 0, "%s seed %u: no starts", keys[k], cfg.seed);
            if (k < 2) {
                int m = mirrorMatchPct(&a);
                UT_ASSERT_MSG(m >= 95, "%s seed %u: only %d%% of tiles match left to right",
                              keys[k], cfg.seed, m);
            }
        }
    }
    genFree(&a);
    genFree(&b);
    return 0;
}

/* Map Maker seeds and map hashes. A change here means that a seed made
 * by an older build makes a different map, so change them only on
 * purpose. Each preset is run at its own size and counts at two
 * seeds, and at size 90. The hashes were made with MSVC 19.50 (Visual
 * Studio 18) on Windows x64. Clang 18 (clang-cl, -O2, on Windows x64,
 * with and without AVX2 and fused multiply-add) gave the same hashes.
 * They have not yet been checked on ARM64, macOS or Linux. */
static const SeedHash kMakerHashes[] = {
    { "K076010AA1422A1040000000080Z46", 0xC3956ADBu },
    { "K076010AA1422A104607FF70880Z46", 0xE6515047u },
    { "K076010AA1422A104607FF70880Z5A", 0x6AD08072u },
    { "K08678CA33221A1080000000080Z73", 0x0963FF6Bu },
    { "K08678CA33221A108607FF70880Z73", 0x5F8E6AAFu },
    { "K08678CA33221A108607FF70880Z5A", 0xF88BD54Fu },
    { "K17678F283221A1080000000080Z5E", 0x167FA25Bu },
    { "K17678F283221A108607FF70880Z5E", 0xBC1FEBDFu },
    { "K17678F283221A108607FF70880Z5A", 0x3DCE9AAFu },
    { "K1F664F280A219B080000000080Z40", 0x21FE9B51u },
    { "K1F664F280A219B08607FF70880Z40", 0xFF03C726u },
    { "K1F664F280A219B08607FF70880Z5A", 0x767318CEu },
    { "K2608D69E3221A1080000000080Z60", 0x47EAAEEAu },
    { "K2608D69E3221A108607FF70880Z60", 0x89420E88u },
    { "K2608D69E3221A108607FF70880Z5A", 0xE2583932u },
    { "K2F678F233221A1080000000080Z5C", 0x8EF2AB3Bu },
    { "K2F678F233221A108607FF70880Z5C", 0xE051FB47u },
    { "K2F678F233221A108607FF70880Z5A", 0x1D476623u },
    { "K37678F37142221080000000080Z56", 0x57E7AD5Fu },
    { "K37678F3714222108607FF70880Z56", 0x0B9B1B9Bu },
    { "K37678F3714222108607FF70880Z5A", 0x03603C4Fu },
    { "K3F8291B2142158E60000000080Z46", 0x434D44C9u },
    { "K3F8291B2142158E6607FF70880Z46", 0x6DF4FF7Fu },
    { "K3F8291B2142158E6607FF70880Z5A", 0x2F0576D0u },
    { "K46065694506161080000000080Z60", 0x76FACA6Bu },
    { "K4606569450616108607FF70880Z60", 0x901FC44Au },
    { "K4606569450616108607FF70880Z5A", 0x6DF1A5D4u },
    { "K4F83CF3C4620D9060000000080Z40", 0x3DF7FB7Bu },
    { "K4F83CF3C4620D906607FF70880Z40", 0xF818D643u },
    { "K4F83CF3C4620D906607FF70880Z5A", 0xFC2748FFu },
    { "K52850F323C2161080000000080Z5C", 0xCA31A013u },
    { "K52850F323C216108607FF70880Z5C", 0x52F1F4F3u },
    { "K52850F323C216108607FF70880Z5A", 0xAF4D3F07u },
    { "K5E051E14464161080000000080Z64", 0x28283D09u },
    { "K5E051E1446416108607FF70880Z64", 0x4195DBE2u },
    { "K5E051E1446416108607FF70880Z5A", 0x8D70FB4Au },
    { "K60628CBC322161080000000080Z50", 0x31217A4Fu },
    { "K60628CBC32216108607FF70880Z50", 0x5032E507u },
    { "K60628CBC32216108607FF70880Z5A", 0xD0A35683u },
    { "K6F6011B20A2199020000000080Z3E", 0x5627D579u },
    { "K6F6011B20A219902607FF70880Z3E", 0xBD466539u },
    { "K6F6011B20A219902607FF70880Z5A", 0x45D310C9u },
    { "K728014320A2019040000000080Z3C", 0x34C7504Fu },
    { "K728014320A201904607FF70880Z3C", 0xA0057F3Fu },
    { "K728014320A201904607FF70880Z5A", 0xF0533C83u },
};

int run_mapgen_maker_hashes(void) {
    GenOut g;
    int bad, count = 0;
    UT_ASSERT(genAlloc(&g));
    /* Every preset is in the table. A missing seed is printed with its
     * hash, in the form of a table row. */
    for (int p = 0; p < mapGenPresetCount(); p++) {
        static const uint32_t seeds[] = { 1u, 0xC0FFEE11u };
        for (size_t k = 0; k < 3; k++) {
            MapGenConfig a = mapGenDefaultConfig(MAPGEN_MAKER);
            char seed[64];
            bool found = false;
            mapGenApplyPreset(p, &a);
            a.seed = seeds[k < 2 ? k : 1];
            if (k == 2) a.size = 90;
            mapGenConfigToSeed(&a, seed, sizeof(seed));
            for (size_t i = 0; i < sizeof(kMakerHashes) / sizeof(kMakerHashes[0]); i++) {
                if (strcmp(kMakerHashes[i].seed, seed) == 0) found = true;
            }
            if (!found) {
                MapGenConfig b = mapGenDefaultConfig(MAPGEN_MAKER);
                UT_ASSERT(mapGenSeedToConfig(seed, &b));
                genRun(&g, &b);
                printf("    { \"%s\", 0x%08Xu }, (not in the table)\n", seed, genHash(&g));
                count++;
            }
        }
    }
    UT_ASSERT_MSG(count == 0, "%d Map Maker seeds are not in the table", count);
    bad = checkSeedHashes(&g, kMakerHashes, sizeof(kMakerHashes) / sizeof(kMakerHashes[0]), MAPGEN_MAKER);
    genFree(&g);
    UT_ASSERT_MSG(bad == 0, "%d Map Maker seeds made a different map", bad);
    return 0;
}
