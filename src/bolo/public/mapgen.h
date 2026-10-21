/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapgen.h
 * Purpose:
 *   Procedural map generation framework. Defines the
 *   config struct and dispatch API used by the sim, the
 *   GUI map-chooser, and the map editor.
 *********************************************************/

#ifndef MAPGEN_H
#define MAPGEN_H

#include <stdint.h>
#include <stdbool.h>
#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generator types */
#define MAPGEN_TOURNAMENT 0
#define MAPGEN_NATURAL    1
#define MAPGEN_MAZE       2
#define MAPGEN_FRACTAL    3
#define MAPGEN_MAKER      4
#define MAPGEN_TYPE_COUNT 5
/* The generator types a plain "-randommap" picks from. The Map Maker
 * type is left out so that a server started with no type keeps
 * making the same kinds of map as before it existed. */
#define MAPGEN_LEGACY_TYPE_COUNT 4

/* Size of the playable area, as the side of a square in tiles,
 * centred in the region. MAPGEN_SIZE_FULL (0) uses the whole region,
 * which is what every map did before the Size control existed. */
#define MAPGEN_SIZE_FULL 0
#define MAPGEN_SIZE_MIN  24
#define MAPGEN_SIZE_MAX  215

/* Tournament symmetry modes */
#define MAPGEN_SYM_4CORNER   0
#define MAPGEN_SYM_MIRROR_H  1
#define MAPGEN_SYM_MIRROR_V  2
#define MAPGEN_SYM_ROTATE180 3
#define MAPGEN_SYM_ROTATE90  4
#define MAPGEN_SYM_COUNT     5

/* Tournament terrain roughness */
#define MAPGEN_ROUGH_LOW    0
#define MAPGEN_ROUGH_MEDIUM 1
#define MAPGEN_ROUGH_HIGH   2
#define MAPGEN_ROUGH_COUNT  3

/* Natural map styles */
#define MAPGEN_STYLE_OCEAN       0
#define MAPGEN_STYLE_CONTINENT   1
#define MAPGEN_STYLE_ISLANDS     2
#define MAPGEN_STYLE_ARCHIPELAGO 3
#define MAPGEN_STYLE_INLAND      4
#define MAPGEN_STYLE_COUNT       5

/* Lock bit indices for Randomize — one bit per lockable parameter.
 * Shared bits (0-1), then unique bits for every parameter across all types.
 * Bases/pills/starts share bits across types so locking "Bases" in
 * tournament keeps it locked if randomize switches to natural or maze. */
#define MAPGEN_LOCK_GENTYPE      (1u << 0)
#define MAPGEN_LOCK_SEED         (1u << 1)
/* Shared across all generator types */
#define MAPGEN_LOCK_BASES        (1u << 2)
#define MAPGEN_LOCK_PILLS        (1u << 3)
#define MAPGEN_LOCK_STARTS       (1u << 4)
/* Tournament-specific (bits 5-8, plus bit 34) */
#define MAPGEN_LOCK_T_SYMMETRY     (1u  << 5)
#define MAPGEN_LOCK_T_LANDMASS     (1u  << 6)
#define MAPGEN_LOCK_T_ROUGHNESS    (1u  << 7)
#define MAPGEN_LOCK_T_ROADS        (1u  << 8)
#define MAPGEN_LOCK_T_WATERBARRIER (1ull << 34)
/* Natural-specific (bits 9-22) */
#define MAPGEN_LOCK_N_STYLE      (1u << 9)
#define MAPGEN_LOCK_N_GRASS      (1u << 10)
#define MAPGEN_LOCK_N_FOREST     (1u << 11)
#define MAPGEN_LOCK_N_BUILDING   (1u << 12)
#define MAPGEN_LOCK_N_SWAMP      (1u << 13)
#define MAPGEN_LOCK_N_RIVER      (1u << 14)
#define MAPGEN_LOCK_N_MINES      (1u << 15)
#define MAPGEN_LOCK_N_RIVERCOUNT (1u << 16)
#define MAPGEN_LOCK_N_CITYCOUNT  (1u << 17)
#define MAPGEN_LOCK_N_MAZECOUNT  (1u << 18)
#define MAPGEN_LOCK_N_BOAT       (1u << 19)
/* Maze-specific (bits 20-26) */
#define MAPGEN_LOCK_M_ALGO       (1u << 20)
#define MAPGEN_LOCK_M_WALLTHICK  (1u << 21)
#define MAPGEN_LOCK_M_CORRIDOR   (1u << 22)
#define MAPGEN_LOCK_M_ENTRIES    (1u << 23)
#define MAPGEN_LOCK_M_CITYROOMS  (1u << 24)
#define MAPGEN_LOCK_M_WALLTERR   (1u << 25)
#define MAPGEN_LOCK_M_CORRTERR   (1u << 26)
/* Fractal-specific (bits 27-31, plus bit 32) */
#define MAPGEN_LOCK_F_LAND        (1u << 27)
#define MAPGEN_LOCK_F_ROUGHNESS   (1u << 28)
#define MAPGEN_LOCK_F_DETAIL      (1u << 29)
#define MAPGEN_LOCK_F_COAST       (1u << 30)
#define MAPGEN_LOCK_F_LAYERS      (1u << 31)
#define MAPGEN_LOCK_F_RIVERS      (1ull << 32)
#define MAPGEN_LOCK_F_MINES       (1ull << 33)
/* Shared by every generator type */
#define MAPGEN_LOCK_SIZE          (1ull << 35)
/* Map Maker-specific (bits 36-45) */
#define MAPGEN_LOCK_K_PRESET      (1ull << 36)
#define MAPGEN_LOCK_K_OUTLINE     (1ull << 37)
#define MAPGEN_LOCK_K_SYMMETRY    (1ull << 38)
#define MAPGEN_LOCK_K_WATER       (1ull << 39)
#define MAPGEN_LOCK_K_FOREST      (1ull << 40)
#define MAPGEN_LOCK_K_ROAD        (1ull << 41)
#define MAPGEN_LOCK_K_BUILDING    (1ull << 42)
#define MAPGEN_LOCK_K_ISLANDS     (1ull << 43)
#define MAPGEN_LOCK_K_MINES       (1ull << 44)
#define MAPGEN_LOCK_K_DETAIL      (1ull << 45)
/* Map Maker style levers (bits 46-53), one per MAPGEN_KSTYLE_* lever:
 * the lock of lever i is MAPGEN_LOCK_KS_FIRST << i. */
#define MAPGEN_LOCK_KS_FIRST      (1ull << 46)
#define MAPGEN_LOCK_KS_ROADS      (1ull << 46)
#define MAPGEN_LOCK_KS_SHAPE      (1ull << 47)
#define MAPGEN_LOCK_KS_CUT        (1ull << 48)
#define MAPGEN_LOCK_KS_CORE       (1ull << 49)
#define MAPGEN_LOCK_KS_RIM        (1ull << 50)
#define MAPGEN_LOCK_KS_YARD       (1ull << 51)
#define MAPGEN_LOCK_KS_WALLS      (1ull << 52)
#define MAPGEN_LOCK_KS_MIDDLE     (1ull << 53)

/* Map Maker outlines: the shape of the land as a whole. A preset can
 * stretch the box and round outlines into a wide rectangle or an oval. */
#define MAPGEN_OUTLINE_BOX      0
#define MAPGEN_OUTLINE_OCTAGON  1
#define MAPGEN_OUTLINE_ROUND    2
#define MAPGEN_OUTLINE_DIAMOND  3
#define MAPGEN_OUTLINE_CROSS    4
#define MAPGEN_OUTLINE_CLOVER   5
#define MAPGEN_OUTLINE_ISLAND   6
/* The seed picks one of the outlines the preset uses. */
#define MAPGEN_OUTLINE_VARIED   7
#define MAPGEN_OUTLINE_COUNT    8

/* Map Maker symmetry. */
#define MAPGEN_MSYM_NONE     0
#define MAPGEN_MSYM_MIRROR   1   /* left half mirrored onto the right */
#define MAPGEN_MSYM_ROTATE2  2   /* half turn */
#define MAPGEN_MSYM_MIRROR4  3   /* mirrored both ways */
#define MAPGEN_MSYM_ROTATE4  4   /* quarter turns */
#define MAPGEN_MSYM_COUNT    5

/* Map Maker style levers: the choices the seed makes for each map of
 * a preset. A lever's value 0 means "from seed": the seed picks, the
 * same as before the levers existed. Other values (see
 * mapGenStyleRange):
 *   ROADS: 1 + MAPGEN_ROADS_* (the lines across the floor).
 *   SHAPE: land width against height, x 100.
 *   CUT:   octagon corner cut x 100; larger values cut smaller corners.
 *   CORE:  pod centres: 1 building, 2 halfbuilding.
 *   RIM:   1 forest rim, 2 no forest rim.
 *   YARD:  walled maze yard: 1 none, 2 building walls, 3 forest walls.
 *   WALLS: building walls on the axes through the forest rim: 1 none,
 *          2 walls.
 *   MIDDLE: buildings near the middle: 1 open, 2 short pieces at 45
 *          degrees on the diagonals, 3 a clump at the centre, 4 both. */
#define MAPGEN_KSTYLE_ROADS  0
#define MAPGEN_KSTYLE_SHAPE  1
#define MAPGEN_KSTYLE_CUT    2
#define MAPGEN_KSTYLE_CORE   3
#define MAPGEN_KSTYLE_RIM    4
#define MAPGEN_KSTYLE_YARD   5
#define MAPGEN_KSTYLE_WALLS  6
#define MAPGEN_KSTYLE_MIDDLE 7
#define MAPGEN_KSTYLE_COUNT  8

#define MAPGEN_ROADS_AXES       0
#define MAPGEN_ROADS_DIAG       1
#define MAPGEN_ROADS_AXES_DIAG  2
#define MAPGEN_ROADS_AXES_RING  3
#define MAPGEN_ROADS_DIAG_RING  4
#define MAPGEN_ROADS_RING       5
#define MAPGEN_ROADS_COUNT      6

typedef struct MapGenConfig {
    /* Region to generate into (playable area or selection, pre-normalized) */
    int x1, y1, x2, y2;

    /* RNG seed */
    uint32_t seed;

    /* Generator type: MAPGEN_TOURNAMENT / MAPGEN_NATURAL / MAPGEN_MAZE / MAPGEN_FRACTAL */
    int genType;

    /* Bitmask of locked parameters (MAPGEN_LOCK_*) — locked fields are
     * not changed by Randomize. */
    uint64_t locks;

    /* Object counts — shared across all generator types.
     * Stored at top level so locking/randomizing doesn't need per-type switches. */
    int bases;              /* 0-16, default 16 */
    int pills;              /* 0-16, default 16 */
    int starts;             /* 0-16, default 16 */

    /* Side of the playable square in tiles (MAPGEN_SIZE_MIN to
     * MAPGEN_SIZE_MAX), or MAPGEN_SIZE_FULL for the whole region.
     * Shared by every generator type; travels in the seed string as an
     * optional "Z" suffix so seeds written before it read as full. */
    int size;

    /* Generator-specific parameters */
    union {
        struct {
            int symmetryMode;     /* MAPGEN_SYM_* */
            int landMassPct;      /* 1-25, default 5 */
            int roughness;        /* MAPGEN_ROUGH_* */
            bool includeRoads;
            /* 0-5: thickness in tiles of the RIVER (shallow-water)
             * rim painted around every landmass at the end of the
             * Tournament generator. 0 = leave DEEP_SEA up to the
             * coast (legacy behaviour). N = N successive 8-neighbour
             * dilations of "non-deep-sea" — each pass converts one
             * additional layer of DEEP_SEA tiles touching the
             * current land+shallow rim to RIVER. */
            int waterBarrier;
        } tournament;

        struct {
            int mapStyle;         /* MAPGEN_STYLE_* */
            int grassPct;         /* 0-100 */
            int forestPct;        /* 0-100 */
            int buildingPct;      /* 0-100 */
            int swampPct;         /* 0-100 */
            int riverPct;         /* 0-100 */
            int boatPct;          /* 0-100 — % of eligible river tiles to convert to BOAT */
            int mineDensityPct;   /* 0-5 */
            int riverCount;       /* 0-5 */
            int cityCount;        /* 0-10 */
            int mazeCount;        /* 0-5 */
        } natural;

        struct {
            int algo;             /* MAZE_ALGO_* */
            int wallThick;        /* 1-2 */
            int corridorWidth;    /* 1-2 */
            int entries;          /* 1-8 */
            int cityRooms;        /* 0-5 */
            int wallTerrain;      /* terrain for walls (default BUILDING) */
            int corridorTerrain;  /* terrain for corridors (default ROAD) */
        } maze;

        struct {
            int landPct;          /* 5-80, default 35 — % of region that is land */
            int roughness;        /* 1-10, default 5 — diamond-square jitter decay */
            int detail;           /* 1-5, default 3 — erosion/smoothing passes */
            int coastJaggedness;  /* 0-10, default 5 — fractal coastline perturbation */
            int terrainLayers;    /* 2-6, default 4 — elevation band count */
            bool rivers;          /* default true — carve drainage rivers */
            bool lakes;           /* default true — fill closed basins */
            int mineDensityPct;   /* 0-5, default 0 */
        } fractal;

        /* Map Maker: a style taken from a classic map maker, set by a
         * preset, with every style value open to change. */
        struct {
            int preset;           /* MAPGEN_PRESET_* */
            int outline;          /* MAPGEN_OUTLINE_* */
            int symmetry;         /* MAPGEN_MSYM_* */
            int waterPct;         /* 0-100: inner water, moats, river rims */
            int forestPct;        /* 0-100 */
            int roadPct;          /* 0-100: roads, and road arena floors */
            int buildingPct;      /* 0-100: walls and walled base areas */
            int islands;          /* 1-8: separate land masses */
            int mineDensityPct;   /* 0-5 */
            int detail;           /* 0-10: pods, rings and stored fragments */
            /* Style levers, indexed by MAPGEN_KSTYLE_*; 0 = from seed. */
            int style[MAPGEN_KSTYLE_COUNT];
        } maker;
    } params;
} MapGenConfig;

/* xorshift32 PRNG — deterministic, platform-independent. */
uint32_t mapGenXorshift32(uint32_t *state);

/* Return a config with sensible defaults for the given generator type. */
MapGenConfig mapGenDefaultConfig(int genType);

/* Return default natural params for the given map style. */
void mapGenNaturalStyleDefaults(int mapStyle, MapGenConfig *cfg);

/* Map Maker presets. Each one sets the style values, the size and the
 * object counts to match the maps of one classic map maker (or one of
 * that maker's collections). */
int mapGenPresetCount(void);
/* Short key used by "-randommap <key>" (for example "berserkir"). */
const char *mapGenPresetKey(int preset);
/* Plain ASCII name used in generated map names (for example "Berserkir"). */
const char *mapGenPresetName(int preset);
/* Returns the preset for a key (case is ignored), or -1. */
int mapGenPresetFromKey(const char *key);
/* The outlines a preset's maps use: bit i set means MAPGEN_OUTLINE_i
 * (never Varied). Returns 0 for a preset out of range. */
int mapGenPresetOutlineSet(int preset);
/* 1 if the preset's maps are several islands (the island family),
 * else 0. For the other presets Islands above 1 splits the land.
 * Returns 0 for a preset out of range. */
int mapGenPresetIsIsland(int preset);
/* Map Maker style levers. mapGenStyleRange returns 1 if the seed
 * picks the lever on the preset's maps, and then sets lo and hi to its
 * lowest and highest value (never 0); it returns 0 if the preset does
 * not vary the lever. mapGenStyleAllowed returns 1 if value is 0 (from
 * seed) or a value the preset allows (a road layout between lo and hi
 * can still be not allowed). */
int mapGenStyleRange(int preset, int lever, int *lo, int *hi);
int mapGenStyleAllowed(int preset, int lever, int value);
/* Set cfg to the preset: Map Maker type, the preset's style values,
 * size and object counts, and every style lever to 0 (from seed). The
 * seed, locks and region are kept. */
void mapGenApplyPreset(int preset, MapGenConfig *cfg);

/* Clamp every field of cfg into its range. Used on decoded seeds and
 * before generation so that no field can index out of range. */
void mapGenClampConfig(MapGenConfig *cfg);

/* The region a config generates into once Size is applied: a square of
 * side cfg->size centred in cfg's region, or cfg's region when Size is
 * MAPGEN_SIZE_FULL or not smaller than the region. */
void mapGenEffectiveRegion(const MapGenConfig *cfg,
                           int *x1, int *y1, int *x2, int *y2);

/* Composite seed encoding: packs all config fields into a prefixed hex string.
 * Tournament seeds start with 'T', natural seeds start with 'N'.
 * A Map Maker seed starts with 'K' when every style lever is "from
 * seed", as before the levers existed. With a lever set it starts with
 * 'S' and holds the size too, in base 32 (0-9 and A-V), 27 characters.
 * A Size that is not full adds "Z" and two hex digits, except on a
 * Natural seed, which then starts with 'Q' and gains one digit, so that
 * every seed fits in 30 characters. out must be at least 32 bytes. */
void mapGenConfigToSeed(const MapGenConfig *cfg, char *out, size_t outLen);

/* Composite seed decoding: parses a seed string back into a config struct.
 * If seedStr is a plain number (no T/N prefix), only cfg->seed is updated.
 * Returns true on success. */
bool mapGenSeedToConfig(const char *seedStr, MapGenConfig *cfg);

/* Build a human-readable map name from the generator type, sub-style
 * (Natural only), and the seed (as 8-char hex). Used to label
 * generated maps consistently across the server's sim->mapName and
 * the chooser's UI title. Example outputs:
 *   "Tournament_DEADBEEF"
 *   "Natural_Ocean_12345678"
 *   "Maze_A1B2C3D4"
 *   "Fractal_FF00FF00"
 * Fits in MAP_STR_SIZE (36) for all generator types. `out` should
 * be at least 32 bytes. */
void mapGenBuildDisplayName(const MapGenConfig *cfg,
                            char *out, size_t outLen);

/* Top-level dispatch: clears the region and calls the appropriate generator stub. */
void mapGenRun(struct mapObj *mp, struct basesObj *bs,
               struct pillsObj *pb, struct startsObj *ss,
               const MapGenConfig *cfg);

/* Orient all starts toward the nearest land mass (center-of-mass of
 * non-deep-sea tiles within a search radius). Call after mapGenRun
 * to fix up start directions. */
void mapGenPointStartsToLand(struct mapObj *mp, struct startsObj *ss);

/* Generate a random map from cfg, return it as a heap-allocated MapPreview
 * that owns its internal substructs (release with clientMapPreviewDestroy).
 * If outBuf is non-NULL, also writes the network-compressed serialization
 * into outBuf[0..outBufCap-1] and stores the byte count in *outCompressedLen;
 * a value <= 0 there indicates the buffer was too small or compression
 * failed. Returns NULL (without writing outBuf) on allocation or generation
 * failure. */
struct MapPreview;
struct MapPreview *mapGenRunAsPreview(const MapGenConfig *cfg,
                                      BYTE *outBuf,
                                      int outBufCap,
                                      int *outCompressedLen);

#ifdef __cplusplus
}
#endif

#endif /* MAPGEN_H */
