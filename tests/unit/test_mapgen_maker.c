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
 * let a tank reach every base and pillbox from the first start.
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

/* Every base and pillbox can be reached from the first start over
 * tiles that are not walls (water counts, as a tank can take a boat).
 * Returns the number of objects that cannot be reached. */
static int unreachableCount(const GenOut *g, const MapGenConfig *cfg) {
    static uint8_t seen[256][256];
    static int queue[256 * 256];
    int x1, y1, x2, y2, head = 0, tail = 0, bad = 0;
    int sx, sy;
    mapGenEffectiveRegion(cfg, &x1, &y1, &x2, &y2);
    if (g->ss->numStarts > 0) { sx = g->ss->item[0].x; sy = g->ss->item[0].y; }
    else if (g->bs->numBases > 0) { sx = g->bs->item[0].x; sy = g->bs->item[0].y; }
    else if (g->pb->numPills > 0) { sx = g->pb->item[0].x; sy = g->pb->item[0].y; }
    else return 0;
    memset(seen, 0, sizeof(seen));
    seen[sx][sy] = 1;
    queue[tail++] = sx * 256 + sy;
    while (head < tail) {
        int k = queue[head++], x = k / 256, y = k % 256;
        static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; d++) {
            int nx = x + dx[d], ny = y + dy[d];
            if (!inBox(nx, ny, x1, y1, x2, y2) || seen[nx][ny]) continue;
            if (tankBlocks(g->mp->mapItem[nx][ny])) continue;
            seen[nx][ny] = 1;
            queue[tail++] = nx * 256 + ny;
        }
    }
    for (int i = 0; i < g->bs->numBases; i++) {
        if (!seen[g->bs->item[i].x][g->bs->item[i].y]) bad++;
    }
    for (int i = 0; i < g->pb->numPills; i++) {
        if (!seen[g->pb->item[i].x][g->pb->item[i].y]) bad++;
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
int run_mapgen_legacy_seed_unchanged(void) {
    for (int type = 0; type < MAPGEN_LEGACY_TYPE_COUNT; type++) {
        MapGenConfig a = mapGenDefaultConfig(type), b;
        char seed[64], again[64];
        UT_ASSERT_MSG(a.size == MAPGEN_SIZE_FULL, "type %d does not default to full size", type);
        a.seed = 0xC0FFEE11u;
        mapGenConfigToSeed(&a, seed, sizeof(seed));
        UT_ASSERT_MSG(strchr(seed, 'Z') == NULL, "full-size seed '%s' has a size part", seed);
        UT_ASSERT_MSG(seed[0] != 'K', "type %d wrote a Map Maker seed", type);
        b = mapGenDefaultConfig(MAPGEN_MAKER);
        UT_ASSERT(mapGenSeedToConfig(seed, &b));
        UT_ASSERT(b.size == MAPGEN_SIZE_FULL);
        mapGenConfigToSeed(&b, again, sizeof(again));
        UT_ASSERT_MSG(strcmp(seed, again) == 0, "'%s' came back as '%s'", seed, again);
    }

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
