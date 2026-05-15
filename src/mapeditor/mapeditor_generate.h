/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_generate.h
 * Purpose:
 *   Procedural map generation framework for the map
 *   editor. Defines the config struct and dispatch API.
 *********************************************************/

#ifndef MAPEDITOR_GENERATE_H
#define MAPEDITOR_GENERATE_H

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
#define MAPGEN_TYPE_COUNT 4

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
    } params;
} MapGenConfig;

/* xorshift32 PRNG — deterministic, platform-independent. */
uint32_t mapGenXorshift32(uint32_t *state);

/* Return a config with sensible defaults for the given generator type. */
MapGenConfig mapGenDefaultConfig(int genType);

/* Return default natural params for the given map style. */
void mapGenNaturalStyleDefaults(int mapStyle, MapGenConfig *cfg);

/* Composite seed encoding: packs all config fields into a prefixed hex string.
 * Tournament seeds start with 'T', natural seeds start with 'N'.
 * out must be at least 32 bytes. */
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
void mapEditorGenerate(struct mapObj *mp, struct basesObj *bs,
                       struct pillsObj *pb, struct startsObj *ss,
                       const MapGenConfig *cfg);

/* Orient all starts toward the nearest land mass (center-of-mass of
 * non-deep-sea tiles within a search radius). Call after mapEditorGenerate
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
struct MapPreview *mapEditorGenerateAsPreview(const MapGenConfig *cfg,
                                              BYTE *outBuf,
                                              int outBufCap,
                                              int *outCompressedLen);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_GENERATE_H */
