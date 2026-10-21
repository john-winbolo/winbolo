/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapgen_maker.h
 * Purpose:
 *   The Map Maker generator: maps in the style of the
 *   classic Bolo map makers, built from a preset and a
 *   set of style values. Kept free of the game structs
 *   so it can be built and tested on its own; mapgen.c
 *   copies its output into the map, pill, base and start
 *   lists.
 *********************************************************/

#ifndef MAPGEN_MAKER_H
#define MAPGEN_MAKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAKER_MAX_OBJECTS 16

/* Terrain codes used by Map Maker. They match types.h; mapgen.c checks
 * this at compile time. */
#define MAKER_TERRAIN_BUILDING     0
#define MAKER_TERRAIN_RIVER        1
#define MAKER_TERRAIN_SWAMP        2
#define MAKER_TERRAIN_CRATER       3
#define MAKER_TERRAIN_ROAD         4
#define MAKER_TERRAIN_FOREST       5
#define MAKER_TERRAIN_RUBBLE       6
#define MAKER_TERRAIN_GRASS        7
#define MAKER_TERRAIN_HALFBUILDING 8
#define MAKER_TERRAIN_BOAT         9
#define MAKER_TERRAIN_DEEP_SEA     0xFF
#define MAKER_TERRAIN_MINE_ADD     8

typedef struct MakerParams {
    /* Region to generate into, already reduced by Size. */
    int x1, y1, x2, y2;
    uint32_t seed;
    int preset;
    int outline;          /* MAPGEN_OUTLINE_* */
    int symmetry;         /* MAPGEN_MSYM_* */
    int waterPct;         /* 0-100 */
    int forestPct;        /* 0-100 */
    int roadPct;          /* 0-100 */
    int buildingPct;      /* 0-100 */
    int islands;          /* 1-8 */
    int mineDensityPct;   /* 0-5 */
    int detail;           /* 0-10 */
    int bases, pills, starts;  /* 0-16 each */
} MakerParams;

typedef struct MakerObjects {
    int numPills;
    uint8_t pillX[MAKER_MAX_OBJECTS], pillY[MAKER_MAX_OBJECTS];
    int numBases;
    uint8_t baseX[MAKER_MAX_OBJECTS], baseY[MAKER_MAX_OBJECTS];
    int numStarts;
    uint8_t startX[MAKER_MAX_OBJECTS], startY[MAKER_MAX_OBJECTS];
} MakerObjects;

int makerPresetCount(void);
const char *makerPresetKey(int preset);
const char *makerPresetName(int preset);
/* The outlines a preset's maps use: bit i set means outline i
 * (MAPGEN_OUTLINE_*, never Varied). An outline of Varied picks one of
 * these by the seed. Returns 0 for a preset out of range. */
int makerPresetOutlineSet(int preset);

/* Fill the style values, size and object counts of a preset. Fields
 * not named by the preset (region, seed) are left alone. sizeOut gets
 * the preset's playable size in tiles. */
void makerPresetDefaults(int preset, MakerParams *p, int *sizeOut);

/* Generate into grid[x][y] (the 256x256 terrain array) within the
 * region of p, and list the pills, bases and starts placed. Tiles of
 * the region are all written; tiles outside it are not touched. */
void makerGenerate(uint8_t grid[256][256], MakerObjects *out,
                   const MakerParams *p);

/* Stored map fragments (mapgen_fragments.c). */
typedef struct MakerFragment {
    const char *maker;    /* who drew the original */
    const char *source;   /* the map it was taken from */
    int set;              /* MAKER_FRAG_SET_* */
    int w, h;
    const char *const *rows;
} MakerFragment;

#define MAKER_FRAG_SET_BERSERKIR_POD    0
#define MAKER_FRAG_SET_SLUGGO_HOUSE     1
#define MAKER_FRAG_SET_SLUGGO_CENTRE    2
#define MAKER_FRAG_SET_STRONGHOLD       3
#define MAKER_FRAG_SET_SLUGFEST_PANEL   4
#define MAKER_FRAG_SET_COUNT            5

int makerFragmentCount(void);
const MakerFragment *makerFragment(int index);

#ifdef __cplusplus
}
#endif

#endif /* MAPGEN_MAKER_H */
