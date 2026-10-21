/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapgen_maker.c
 * Purpose:
 *   The Map Maker generator. Each preset describes the
 *   style of one classic Bolo map maker (or one of that
 *   maker's collections) from winbolo.net, and the engine
 *   builds a new map in that style from a seed.
 *
 *   There are three families of style:
 *     FORMAL  an arena made of rings from the coast inward
 *             (river rim, forest rim, building rim, track,
 *             floor), with lines across the floor, pods on a
 *             grid of cells and a centre feature.
 *     TOWN    the rings, then a street grid that splits the
 *             floor into blocks of houses, hedges and ponds.
 *     ISLAND  organic land with a jagged river rim, forest at
 *             the coast, lakes, rivers, a thin road network,
 *             houses and walled base areas.
 *
 *   Every map is painted freely and then made symmetric by
 *   copying each tile from the first tile of its symmetry
 *   group, so any shape or feature comes out symmetric.
 *   Objects are placed in symmetry groups.
 *
 *   The code has no game structs in it so that it can be
 *   built and tested on its own. mapgen.c copies the result
 *   into the map, pill, base and start lists.
 *********************************************************/

#include "mapgen_maker.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* The same seed must make the same map on every compiler and CPU, as
 * the server and each client make the map on their own. So this file
 * uses no sin, cos, atan2 or other library function whose last bit
 * can differ between C libraries: angles come from an integer sine
 * table and the noise is integer value noise. The double arithmetic
 * that is left is + - * / and sqrt, floor, ceil and fabs, which IEEE
 * 754 defines to the last bit, as long as the compiler does not fuse
 * a multiply and an add into one step and does not keep extra
 * precision. The pragma below and -ffp-contract=off in
 * src/bolo/CMakeLists.txt stop the fused step on GCC and Clang. MSVC
 * from Visual Studio 2022 (MSVC 19.30) on does not fuse under its
 * default /fp:precise; older MSVC could on ARM64 and with /arch:AVX2.
 * The check below stops a build that keeps extra precision. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
#if defined(FLT_EVAL_METHOD) && (FLT_EVAL_METHOD == 1 || FLT_EVAL_METHOD == 2)
#error "mapgen_maker.c needs double arithmetic without extra precision (SSE2 or later on x86)"
#endif

/* Terrain codes. mapgen.c checks at compile time that these match
 * types.h. */
#define T_BUILDING MAKER_TERRAIN_BUILDING
#define T_RIVER    MAKER_TERRAIN_RIVER
#define T_SWAMP    MAKER_TERRAIN_SWAMP
#define T_CRATER   MAKER_TERRAIN_CRATER
#define T_ROAD     MAKER_TERRAIN_ROAD
#define T_FOREST   MAKER_TERRAIN_FOREST
#define T_RUBBLE   MAKER_TERRAIN_RUBBLE
#define T_GRASS    MAKER_TERRAIN_GRASS
#define T_HALF     MAKER_TERRAIN_HALFBUILDING
#define T_BOAT     MAKER_TERRAIN_BOAT
#define T_DEEP     MAKER_TERRAIN_DEEP_SEA
#define T_MINE_ADD MAKER_TERRAIN_MINE_ADD

/* These match the MAPGEN_OUTLINE_* and MAPGEN_MSYM_* values in mapgen.h. */
#define OL_BOX     0
#define OL_OCTAGON 1
#define OL_ROUND   2
#define OL_DIAMOND 3
#define OL_CROSS   4
#define OL_CLOVER  5
#define OL_ISLAND  6
#define OL_VARIED  7   /* the seed picks one of the preset's outlines */
#define OL_COUNT   8

#define SY_NONE    0
#define SY_MIRROR  1
#define SY_ROT2    2
#define SY_MIRROR4 3
#define SY_ROT4    4
#define SY_COUNT   5

#define FAM_FORMAL 0
#define FAM_TOWN   1
#define FAM_ISLAND 2

/* Lines across the floor */
#define SK_AXES  1
#define SK_DIAG  2
#define SK_RING  4
#define SK_PANEL 8

/* Pod kinds */
#define POD_NONE    0
#define POD_RING    1   /* building core, forest ring, grass ring */
#define POD_HEDGE   2   /* forest block */
#define POD_POND    3   /* river with a swamp edge */
#define POD_DIAMOND 4   /* forest diamond on grass */
#define POD_FRAG    5   /* a stored fragment */
#define POD_BLOCK   6   /* building block in a grass square */
#define POD_LAWN    7   /* grass square with a forest edge */
#define POD_CHECKER 8   /* grass and forest checkered square */

/* Centre features */
#define CEN_NONE     0
#define CEN_POD      1
#define CEN_POND     2   /* deep pond that holds the starts */
#define CEN_ROUND    3   /* roundabout */
#define CEN_FOUNTAIN 4   /* small river ring with a road ring */
#define CEN_DEEP     5   /* deep pond, no starts */

/* Object placement modes */
#define PM_NEAR_BASE 0
#define PM_PAIRS     1
#define PM_JUNCTION  2
#define PM_ANY       3
#define PM_TRACK     4

#define BM_TRACK 0
#define BM_CELLS 1
#define BM_ANY   2
#define BM_EDGE  3   /* floor tiles next to the inner rim */

#define SM_SEA     0
#define SM_CORNERS 1
#define SM_SIDES   2
#define SM_POND    3
#define SM_AXES    4   /* clusters off the ends of the axes */

/* Layer of each tile, used to find places for objects. */
#define L_SEA    0
#define L_RRIM   1
#define L_RIM    2
#define L_TRACK  3
#define L_FLOOR  4
#define L_SKEL   5
#define L_POD    6
#define L_CENTRE 7
#define L_STREET 8
#define L_BLOCK  9
#define L_WALL   10

typedef struct Preset {
    const char *key;
    const char *name;
    int family;
    int size;        /* side of the playable square */
    int aspect;      /* land width / height x 100 */
    int outline, symmetry;
    int water, forest, road, building, islands, mines, detail;
    int bases, pills, starts;
    int cut;         /* octagon corner cut: |u|+|v| limit x 100 */
    int riverRim, forestRim, buildRim, buildOuter;
    int track, trackW, floor;
    int skel, skelTerr, skelW, panel;
    int pod1, pod2, pod3, cell;
    int centre, gateTerr, gateW;
    int jag;         /* rim noise in tenths of a tile */
    int pillMode, baseMode, startMode;
    int endForest;   /* TOWN: forest at the two short ends */
    int blockMin, blockMax;
    int fragSet;     /* MAKER_FRAG_SET_* or -1 */
    /* Variety from one seed to the next */
    int skelAlt;     /* bit i allows kSkelOpts[i]; 0 keeps skel */
    int aspectVar;   /* percent the aspect may move either way */
    int cutVar;      /* hundredths the octagon cut may move either way */
    int podCore;     /* 0 building, 1 building or halfbuilding, 2 halfbuilding */
    int gateStyle;   /* 0 strip, 1 wedge, 2 strip on the diagonals */
    int outlineSet;  /* outlines the seed picks from when outline is varied */
    int lobes;       /* clover lobes are forest */
    int rimDrop;     /* percent of seeds with no forest rim (grass to the coast) */
    int mazeYard;    /* ISLAND: percent of seeds with a walled maze yard */
} Preset;

static const int kSkelOpts[6] = {
    SK_AXES, SK_DIAG, SK_AXES | SK_DIAG, SK_AXES | SK_RING, SK_DIAG | SK_RING, SK_RING
};

#define NO_TERR (-1)

#define OS(o) (1 << (o))

static const Preset kPresets[] = {
    /* Berserkir: a road arena inside a forest rim and a grass track,
     * ring pods with building or halfbuilding cores, grass lines,
     * halfbuilding wedges where the axes meet the rim, pills in pairs
     * and the starts in pairs at the corners. */
    { .key = "berserkir", .name = "Berserkir", .family = FAM_FORMAL,
      .size = 70, .aspect = 120, .outline = OL_VARIED, .symmetry = SY_MIRROR4,
      .water = 0, .forest = 33, .road = 42, .building = 10, .islands = 1, .mines = 0, .detail = 10,
      .bases = 16, .pills = 16, .starts = 8,
      .cut = 170, .forestRim = 4, .track = T_GRASS, .trackW = 1, .floor = T_ROAD,
      .skel = SK_AXES, .skelTerr = T_GRASS, .skelW = 1,
      .pod1 = POD_RING, .pod2 = POD_RING, .pod3 = POD_RING, .cell = 14,
      .centre = CEN_POD, .gateTerr = T_HALF, .gateW = 1,
      .pillMode = PM_PAIRS, .baseMode = BM_CELLS, .startMode = SM_CORNERS,
      .fragSet = MAKER_FRAG_SET_BERSERKIR_POD,
      .skelAlt = 0x1F, .aspectVar = 20, .cutVar = 25, .podCore = 1, .gateStyle = 1,
      .outlineSet = OS(OL_OCTAGON) | OS(OL_ROUND) | OS(OL_BOX) | OS(OL_CROSS) },
    /* Sluggo Tournament: a wide box with a river rim, forest at the
     * ends and a road ring; inside, a street grid of grass blocks with
     * houses, hedges and ponds. Starts along the long sides. */
    { .key = "sluggo-tournament", .name = "Sluggo Tournament", .family = FAM_TOWN,
      .size = 115, .aspect = 190, .outline = OL_BOX, .symmetry = SY_MIRROR4,
      .water = 30, .forest = 25, .road = 35, .building = 25, .islands = 1, .mines = 0, .detail = 6,
      .bases = 16, .pills = 16, .starts = 16,
      .riverRim = 1, .track = T_ROAD, .trackW = 2, .floor = T_GRASS,
      .skelTerr = T_ROAD, .skelW = 2, .cell = 16,
      .centre = CEN_NONE, .gateTerr = T_ROAD, .gateW = 0,
      .pillMode = PM_JUNCTION, .baseMode = BM_ANY, .startMode = SM_SIDES,
      .endForest = 3, .blockMin = 14, .blockMax = 30, .fragSet = MAKER_FRAG_SET_SLUGGO_HOUSE,
      .outlineSet = OS(OL_BOX) },
    /* Sluggo Slugdeath: mirrored four ways, a forest rim, axis and
     * diagonal roads, house blocks and a river fountain at the centre. */
    { .key = "slugdeath", .name = "Sluggo Slugdeath", .family = FAM_TOWN,
      .size = 94, .aspect = 165, .outline = OL_VARIED, .symmetry = SY_MIRROR4,
      .water = 30, .forest = 30, .road = 40, .building = 25, .islands = 1, .mines = 0, .detail = 6,
      .bases = 16, .pills = 16, .starts = 16,
      .riverRim = 1, .forestRim = 2, .track = T_ROAD, .trackW = 2, .floor = T_GRASS,
      .skel = SK_AXES | SK_DIAG, .skelTerr = T_ROAD, .skelW = 2, .cell = 16,
      .centre = CEN_FOUNTAIN, .gateTerr = T_ROAD,
      .pillMode = PM_JUNCTION, .baseMode = BM_ANY, .startMode = SM_SEA,
      .blockMin = 12, .blockMax = 22, .fragSet = MAKER_FRAG_SET_SLUGGO_CENTRE,
      .outlineSet = OS(OL_BOX) | OS(OL_ROUND) | OS(OL_DIAMOND) | OS(OL_OCTAGON) },
    /* Sluggo Slugfest: small and compact, a river rim and forest band,
     * grass panels split by road lines, forest diamonds and discs. */
    { .key = "slugfest", .name = "Sluggo Slugfest", .family = FAM_FORMAL,
      .size = 64, .aspect = 100, .outline = OL_VARIED, .symmetry = SY_MIRROR4,
      .water = 25, .forest = 30, .road = 40, .building = 5, .islands = 1, .mines = 0, .detail = 6,
      .bases = 13, .pills = 16, .starts = 16,
      .cut = 145, .riverRim = 1, .forestRim = 2, .track = T_ROAD, .trackW = 1, .floor = T_GRASS,
      .skel = SK_AXES | SK_DIAG | SK_PANEL, .skelTerr = T_ROAD, .skelW = 1, .panel = 10,
      .pod1 = POD_DIAMOND, .pod2 = POD_POND, .pod3 = POD_LAWN, .cell = 14,
      .centre = CEN_ROUND, .gateTerr = T_ROAD, .gateW = 1,
      .pillMode = PM_JUNCTION, .baseMode = BM_ANY, .startMode = SM_SEA,
      .fragSet = MAKER_FRAG_SET_SLUGFEST_PANEL,
      .skelAlt = 0x3F, .cutVar = 20, .podCore = 1, .rimDrop = 45,
      .outlineSet = OS(OL_ROUND) | OS(OL_BOX) | OS(OL_OCTAGON) | OS(OL_DIAMOND) | OS(OL_CLOVER) },
    /* Sluggo Adventurous: one organic island, no symmetry, a jagged
     * river rim, forest at the coast, lakes, rivers, a thin road
     * network and small houses. */
    { .key = "sluggo-adventurous", .name = "Sluggo Adventurous", .family = FAM_ISLAND,
      .size = 96, .aspect = 105, .outline = OL_ISLAND, .symmetry = SY_NONE,
      .water = 35, .forest = 45, .road = 30, .building = 25, .islands = 1, .mines = 0, .detail = 6,
      .bases = 16, .pills = 16, .starts = 16,
      .riverRim = 2, .forestRim = 2, .track = NO_TERR, .floor = T_GRASS,
      .skelTerr = T_ROAD, .skelW = 1, .cell = 16, .jag = 12,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_ANY, .startMode = SM_SEA,
      .fragSet = MAKER_FRAG_SET_SLUGGO_HOUSE, .aspectVar = 25,
      .outlineSet = OS(OL_ISLAND) },
    /* Sluggo Symmetric: a cross or clover of town blocks. */
    { .key = "sluggo-symmetric", .name = "Sluggo Symmetric", .family = FAM_TOWN,
      .size = 92, .aspect = 100, .outline = OL_VARIED, .symmetry = SY_MIRROR4,
      .water = 30, .forest = 30, .road = 35, .building = 25, .islands = 1, .mines = 0, .detail = 6,
      .bases = 16, .pills = 16, .starts = 16,
      .riverRim = 1, .forestRim = 1, .track = T_ROAD, .trackW = 1, .floor = T_GRASS,
      .skel = SK_AXES, .skelTerr = T_ROAD, .skelW = 2, .cell = 16,
      .centre = CEN_POD, .gateTerr = T_ROAD,
      .pillMode = PM_JUNCTION, .baseMode = BM_ANY, .startMode = SM_SEA,
      .blockMin = 10, .blockMax = 18, .fragSet = MAKER_FRAG_SET_SLUGGO_HOUSE, .aspectVar = 30,
      .outlineSet = OS(OL_CROSS) | OS(OL_CLOVER) | OS(OL_ROUND) | OS(OL_DIAMOND) },
    /* Sluggo Stoogefest: a road arena with grass panels and big ponds. */
    { .key = "stoogefest", .name = "Sluggo Stoogefest", .family = FAM_FORMAL,
      .size = 86, .aspect = 100, .outline = OL_VARIED, .symmetry = SY_MIRROR4,
      .water = 30, .forest = 30, .road = 55, .building = 10, .islands = 1, .mines = 0, .detail = 8,
      .bases = 16, .pills = 16, .starts = 16,
      .cut = 150, .riverRim = 1, .forestRim = 3, .track = T_GRASS, .trackW = 1, .floor = T_ROAD,
      .skel = SK_AXES, .skelTerr = T_GRASS, .skelW = 2,
      .pod1 = POD_LAWN, .pod2 = POD_POND, .pod3 = POD_RING, .cell = 18,
      .centre = CEN_POD, .gateTerr = T_ROAD, .gateW = 2,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_CELLS, .startMode = SM_SEA,
      .skelAlt = 0x1F, .cutVar = 20, .podCore = 1,
      .fragSet = -1,
      .outlineSet = OS(OL_BOX) | OS(OL_OCTAGON) | OS(OL_CLOVER) },
    /* Black Lightning: a clover or round arena with a road floor,
     * forest lobes, thin lines across the floor and the starts in
     * clusters, turned four ways. */
    { .key = "black-lightning", .name = "Black Lightning", .family = FAM_FORMAL,
      .size = 70, .aspect = 100, .outline = OL_VARIED, .symmetry = SY_ROT4,
      .water = 10, .forest = 35, .road = 50, .building = 10, .islands = 1, .mines = 0, .detail = 5,
      .bases = 12, .pills = 14, .starts = 12,
      .cut = 145, .forestRim = 2, .track = T_GRASS, .trackW = 1, .floor = T_ROAD,
      .skel = SK_DIAG, .skelTerr = T_GRASS, .skelW = 1,
      .pod1 = POD_HEDGE, .pod2 = POD_BLOCK, .pod3 = POD_DIAMOND, .cell = 18,
      .centre = CEN_POD, .gateTerr = T_ROAD, .gateW = 1, .gateStyle = 2,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_ANY, .startMode = SM_AXES,
      .skelAlt = 0x07, .cutVar = 15, .podCore = 1, .lobes = 1,
      .fragSet = -1,
      .outlineSet = OS(OL_CLOVER) | OS(OL_ROUND) | OS(OL_DIAMOND) | OS(OL_OCTAGON) },
    /* Black Lightning Islands: island maps with walled base areas. */
    { .key = "bl-islands", .name = "Black Lightning Islands", .family = FAM_ISLAND,
      .size = 96, .aspect = 110, .outline = OL_ISLAND, .symmetry = SY_NONE,
      .water = 25, .forest = 45, .road = 20, .building = 40, .islands = 3, .mines = 0, .detail = 5,
      .bases = 16, .pills = 16, .starts = 16,
      .riverRim = 1, .forestRim = 2, .track = NO_TERR, .floor = T_GRASS,
      .skelTerr = T_ROAD, .skelW = 1, .cell = 16, .jag = 12,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_ANY, .startMode = SM_SEA,
      .fragSet = MAKER_FRAG_SET_STRONGHOLD, .aspectVar = 25, .mazeYard = 60,
      .outlineSet = OS(OL_ISLAND) },
    /* Deathhand: an octagon road arena with a building rim, forest
     * lines and a deep pond at the centre. */
    { .key = "deathhand", .name = "Deathhand", .family = FAM_FORMAL,
      .size = 64, .aspect = 100, .outline = OL_VARIED, .symmetry = SY_ROT4,
      .water = 15, .forest = 30, .road = 60, .building = 35, .islands = 1, .mines = 0, .detail = 3,
      .bases = 12, .pills = 16, .starts = 12,
      .cut = 145, .forestRim = 3, .buildRim = 2, .buildOuter = 1, .track = NO_TERR, .floor = T_ROAD,
      .skel = SK_AXES, .skelTerr = T_FOREST, .skelW = 2,
      .pod1 = POD_HEDGE, .pod2 = POD_BLOCK, .pod3 = POD_HEDGE, .cell = 16,
      .centre = CEN_DEEP, .gateTerr = T_BUILDING, .gateW = 2,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_ANY, .startMode = SM_SEA,
      .skelAlt = 0x07, .cutVar = 20,
      .fragSet = -1,
      .outlineSet = OS(OL_OCTAGON) | OS(OL_BOX) | OS(OL_ROUND) },
    /* Megawatt TC: round, a thick outer building rim, a thin forest
     * ring, and a deep pond at the centre that holds a ring of starts. */
    { .key = "megawatt-tc", .name = "Megawatt TC", .family = FAM_FORMAL,
      .size = 92, .aspect = 100, .outline = OL_ROUND, .symmetry = SY_ROT4,
      .water = 20, .forest = 30, .road = 50, .building = 30, .islands = 1, .mines = 0, .detail = 5,
      .bases = 16, .pills = 16, .starts = 16,
      .forestRim = 1, .buildRim = 4, .buildOuter = 1, .track = NO_TERR, .floor = T_ROAD,
      .skel = SK_AXES, .skelTerr = T_FOREST, .skelW = 4,
      .pod1 = POD_HEDGE, .pod2 = POD_CHECKER, .pod3 = POD_LAWN, .cell = 18,
      .centre = CEN_POND, .gateTerr = T_HALF, .gateW = 0,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_ANY, .startMode = SM_POND,
      .skelAlt = 0x07,
      .fragSet = -1,
      .outlineSet = OS(OL_ROUND) },
    /* Megawatt Islands: organic islands, dense forest, walled base areas. */
    { .key = "megawatt-islands", .name = "Megawatt Islands", .family = FAM_ISLAND,
      .size = 100, .aspect = 110, .outline = OL_ISLAND, .symmetry = SY_NONE,
      .water = 20, .forest = 60, .road = 20, .building = 35, .islands = 2, .mines = 0, .detail = 5,
      .bases = 16, .pills = 16, .starts = 16,
      .riverRim = 1, .forestRim = 3, .track = NO_TERR, .floor = T_GRASS,
      .skelTerr = T_ROAD, .skelW = 1, .cell = 16, .jag = 15,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_ANY, .startMode = SM_SEA,
      .fragSet = MAKER_FRAG_SET_STRONGHOLD, .aspectVar = 30,
      .outlineSet = OS(OL_ISLAND) },
    /* Nature Mosh Pit: a square road arena with a thin forest rim,
     * checkered grass and forest panels and a walled centre. */
    { .key = "mosh-pit", .name = "Nature Mosh Pit", .family = FAM_FORMAL,
      .size = 80, .aspect = 100, .outline = OL_BOX, .symmetry = SY_MIRROR4,
      .water = 10, .forest = 25, .road = 60, .building = 25, .islands = 1, .mines = 0, .detail = 5,
      .bases = 16, .pills = 16, .starts = 16,
      .forestRim = 1, .track = NO_TERR, .floor = T_ROAD,
      .skel = SK_AXES, .skelTerr = T_FOREST, .skelW = 3,
      .pod1 = POD_CHECKER, .pod2 = POD_LAWN, .pod3 = POD_CHECKER, .cell = 26,
      .centre = CEN_POD, .gateTerr = T_ROAD, .gateW = 0,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_EDGE, .startMode = SM_SEA,
      .skelAlt = 0x0B, .podCore = 1,
      .fragSet = -1,
      .outlineSet = OS(OL_BOX) },
    /* Palpatine: a rounded box with a forest rim, a grass track, a road
     * floor and grass diamonds. */
    { .key = "palpatine", .name = "Palpatine", .family = FAM_FORMAL,
      .size = 62, .aspect = 100, .outline = OL_VARIED, .symmetry = SY_MIRROR4,
      .water = 0, .forest = 35, .road = 50, .building = 5, .islands = 1, .mines = 0, .detail = 6,
      .bases = 12, .pills = 16, .starts = 4,
      .cut = 165, .forestRim = 3, .track = T_GRASS, .trackW = 1, .floor = T_ROAD,
      .skel = SK_AXES | SK_DIAG, .skelTerr = T_GRASS, .skelW = 1,
      .pod1 = POD_DIAMOND, .pod2 = POD_LAWN, .pod3 = POD_DIAMOND, .cell = 16,
      .centre = CEN_POD, .gateTerr = T_ROAD, .gateW = 1,
      .pillMode = PM_PAIRS, .baseMode = BM_EDGE, .startMode = SM_CORNERS,
      .skelAlt = 0x1F, .cutVar = 15, .podCore = 1,
      .fragSet = -1,
      .outlineSet = OS(OL_OCTAGON) | OS(OL_ROUND) | OS(OL_BOX) },
    /* Chew Toy: a round arena with a thick forest rim cut by bays on
     * the diagonals, a grass track, thin lines, bases on the track. */
    { .key = "chew-toy", .name = "Chew Toy", .family = FAM_FORMAL,
      .size = 60, .aspect = 100, .outline = OL_ROUND, .symmetry = SY_ROT4,
      .water = 0, .forest = 40, .road = 50, .building = 5, .islands = 1, .mines = 0, .detail = 0,
      .bases = 12, .pills = 16, .starts = 8,
      .forestRim = 5, .track = T_GRASS, .trackW = 1, .floor = T_ROAD,
      .skel = SK_AXES, .skelTerr = T_FOREST, .skelW = 1,
      .pod1 = POD_LAWN, .pod2 = POD_LAWN, .pod3 = POD_LAWN, .cell = 20,
      .centre = CEN_NONE, .gateTerr = T_DEEP, .gateW = 3, .gateStyle = 2,
      .pillMode = PM_NEAR_BASE, .baseMode = BM_TRACK, .startMode = SM_CORNERS,
      .skelAlt = 0x07,
      .fragSet = -1,
      .outlineSet = OS(OL_ROUND) },
};

#define PRESET_COUNT ((int)(sizeof(kPresets) / sizeof(kPresets[0])))

int makerPresetCount(void) { return PRESET_COUNT; }

const char *makerPresetKey(int preset) {
    if (preset < 0 || preset >= PRESET_COUNT) return "";
    return kPresets[preset].key;
}

const char *makerPresetName(int preset) {
    if (preset < 0 || preset >= PRESET_COUNT) return "";
    return kPresets[preset].name;
}

int makerPresetOutlineSet(int preset) {
    if (preset < 0 || preset >= PRESET_COUNT) return 0;
    return kPresets[preset].outlineSet & ((1 << OL_VARIED) - 1);
}

void makerPresetDefaults(int preset, MakerParams *p, int *sizeOut) {
    const Preset *pr;
    if (preset < 0 || preset >= PRESET_COUNT) preset = 0;
    pr = &kPresets[preset];
    p->preset = preset;
    p->outline = pr->outline;
    p->symmetry = pr->symmetry;
    p->waterPct = pr->water;
    p->forestPct = pr->forest;
    p->roadPct = pr->road;
    p->buildingPct = pr->building;
    p->islands = pr->islands;
    p->mineDensityPct = pr->mines;
    p->detail = pr->detail;
    p->bases = pr->bases;
    p->pills = pr->pills;
    p->starts = pr->starts;
    if (sizeOut) *sizeOut = pr->size;
}

/*=========================================================
 * Work state
 *=========================================================*/

#define OBJ_PILL  0
#define OBJ_BASE  1
#define OBJ_START 2

typedef struct Ctx {
    int W, H;
    uint8_t *t;        /* terrain, index y * W + x */
    uint8_t *layer;    /* L_* */
    uint8_t *mine;
    uint8_t *obj;      /* 0 = none, else OBJ_* + 1 */
    int *din;          /* chamfer distance inside land (x3) */
    int *dout;         /* chamfer distance from any non-deep tile (x3) */
    int *canon;        /* first tile of the symmetry group */
    uint32_t rng;
    uint32_t seed;
    int sym;
    const Preset *pr;
    MakerParams p;
    double cx, cy;     /* centre of the region */
    double ax, ay;     /* half extents of the land */
    int skel;          /* lines across the floor for this map */
    double cut;        /* octagon corner cut for this map */
    int coreT;         /* terrain of pod cores for this map */
    int rimTotal;      /* tiles of rim inside the coast */
    int noForestRim;   /* this seed has no forest rim */
    int n[3];
    int ox[3][MAKER_MAX_OBJECTS], oy[3][MAKER_MAX_OBJECTS];
    /* Hints from fragments and features for each object kind */
    int nh[3];
    int hx[3][64], hy[3][64];
    /* Pod centres */
    int npod;
    int podx[96], pody[96], podr[96];
    /* TOWN: how many rooms of each kind so far, to keep them varied */
    int roomUsed[16];
} Ctx;

#define IDX(c, x, y) ((y) * (c)->W + (x))

static uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int rnd(Ctx *c, int n) {
    if (n <= 1) return 0;
    return (int)(xs32(&c->rng) % (uint32_t)n);
}

static int rrange(Ctx *c, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + rnd(c, hi - lo + 1);
}

static double rfloat(Ctx *c) {
    return (double)(xs32(&c->rng) & 0xFFFFFF) / 16777216.0;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int inb(const Ctx *c, int x, int y) {
    return x >= 0 && y >= 0 && x < c->W && y < c->H;
}

/*---------------------------------------------------------
 * Noise
 *---------------------------------------------------------*/

static uint32_t hash2(int x, int y, uint32_t s) {
    uint32_t h = s ^ 0x9E3779B9u;
    h ^= (uint32_t)x * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h = h * 5u + 0xE6546B64u;
    h ^= (uint32_t)y * 0xC2B2AE35u;
    h = (h << 13) | (h >> 19);
    h = h * 5u + 0xE6546B64u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

/* Value noise in integers. Positions are 16.16 fixed point, lattice
 * values and results are 0..65535. */
static int64_t lattice(int x, int y, uint32_t s) {
    return (int64_t)(hash2(x, y, s) & 0xFFFF);
}

/* Smoothstep of t (0..65536), in 0..65536. */
static int64_t smooth16(int64_t t) {
    return (t * t * (3 * 65536 - 2 * t)) >> 32;
}

/* a + (b - a) * t with t in 0..65536; no negative value is shifted. */
static int64_t lerp16(int64_t a, int64_t b, int64_t t) {
    return (a * (65536 - t) + b * t) >> 16;
}

/* The whole part of a 16.16 value, rounded down. */
static int floor16(int64_t v) {
    return v >= 0 ? (int)(v / 65536) : -(int)((-v + 65535) / 65536);
}

static int64_t vnoise(int64_t x, int64_t y, uint32_t s) {
    int ix = floor16(x), iy = floor16(y);
    int64_t fx = smooth16(x - (int64_t)ix * 65536);
    int64_t fy = smooth16(y - (int64_t)iy * 65536);
    int64_t top = lerp16(lattice(ix, iy, s), lattice(ix + 1, iy, s), fx);
    int64_t bot = lerp16(lattice(ix, iy + 1, s), lattice(ix + 1, iy + 1, s), fx);
    return lerp16(top, bot, fy);
}

/* Fractal noise in 0..1. x and y are turned into 16.16 fixed point
 * once (a scale by a power of two and floor, both exact), and the rest
 * is integer arithmetic. */
static double fbm(double x, double y, uint32_t s, int oct) {
    int64_t fx = (int64_t)floor(x * 65536.0), fy = (int64_t)floor(y * 65536.0);
    int64_t sum = 0, norm = 0, amp = 32768;
    int i;
    for (i = 0; i < oct; i++) {
        sum += amp * vnoise(fx, fy, s + (uint32_t)i * 101u);
        norm += amp;
        amp /= 2;
        fx *= 2;
        fy *= 2;
    }
    return (double)sum / ((double)norm * 65535.0);
}

/* Integer sine: angles in 1024ths of a turn, results scaled by 65536.
 * kSinQ holds the first quarter turn. */
static const int32_t kSinQ[257] = {
    0, 402, 804, 1206, 1608, 2010, 2412, 2814, 3216, 3617, 4019, 4420,
    4821, 5222, 5623, 6023, 6424, 6824, 7224, 7623, 8022, 8421, 8820, 9218,
    9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391, 12785, 13180, 13573, 13966,
    14359, 14751, 15143, 15534, 15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
    19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699, 22078, 22457, 22834, 23210,
    23586, 23961, 24335, 24708, 25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
    28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538, 30893, 31248, 31600, 31952,
    32303, 32652, 33000, 33347, 33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
    36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716, 39040, 39362, 39683, 40002,
    40320, 40636, 40951, 41264, 41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056, 46341, 46624, 46906, 47186,
    47464, 47741, 48015, 48288, 48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
    50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398, 52639, 52878, 53114, 53349,
    53581, 53812, 54040, 54267, 54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
    56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607, 57798, 57986, 58172, 58356,
    58538, 58718, 58896, 59071, 59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
    60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568, 61705, 61839, 61971, 62101,
    62228, 62353, 62476, 62596, 62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
    63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197, 64277, 64354, 64429, 64501,
    64571, 64639, 64704, 64766, 64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
    65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436, 65457, 65476, 65492, 65505,
    65516, 65525, 65531, 65535, 65536
};

static int32_t isin(int a) {
    /* a can be negative. The C standard defines the conversion to
     * unsigned as modulo 2^N, so the mask gives 0..1023 on every
     * compiler. */
    a = (int)((unsigned)a & 1023u);
    if (a <= 256) return kSinQ[a];
    if (a <= 512) return kSinQ[512 - a];
    if (a <= 768) return -kSinQ[a - 512];
    return -kSinQ[1024 - a];
}

static int32_t icos(int a) {
    return isin(a + 256);
}

/*---------------------------------------------------------
 * Symmetry
 *---------------------------------------------------------*/

static int images(const Ctx *c, int x, int y, int *xs, int *ys) {
    int W = c->W, H = c->H, n = 0, i, j;
    int cx[4], cy[4];
    cx[n] = x; cy[n] = y; n++;
    switch (c->sym) {
    case SY_MIRROR:
        cx[n] = W - 1 - x; cy[n] = y; n++;
        break;
    case SY_ROT2:
        cx[n] = W - 1 - x; cy[n] = H - 1 - y; n++;
        break;
    case SY_MIRROR4:
        cx[n] = W - 1 - x; cy[n] = y; n++;
        cx[n] = x; cy[n] = H - 1 - y; n++;
        cx[n] = W - 1 - x; cy[n] = H - 1 - y; n++;
        break;
    case SY_ROT4:
        cx[n] = W - 1 - y; cy[n] = x; n++;
        cx[n] = W - 1 - x; cy[n] = H - 1 - y; n++;
        cx[n] = y; cy[n] = W - 1 - x; n++;
        break;
    default:
        break;
    }
    j = 0;
    for (i = 0; i < n; i++) {
        int k, dup = 0;
        for (k = 0; k < j; k++) {
            if (xs[k] == cx[i] && ys[k] == cy[i]) dup = 1;
        }
        if (!dup) {
            xs[j] = cx[i];
            ys[j] = cy[i];
            j++;
        }
    }
    return j;
}

static void buildCanon(Ctx *c) {
    int x, y;
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int xs[4], ys[4], n, i, best = IDX(c, x, y);
            n = images(c, x, y, xs, ys);
            for (i = 0; i < n; i++) {
                int k = IDX(c, xs[i], ys[i]);
                if (k < best) best = k;
            }
            c->canon[IDX(c, x, y)] = best;
        }
    }
}

static int inFund(const Ctx *c, int x, int y) {
    return c->canon[IDX(c, x, y)] == IDX(c, x, y);
}

/* Copy every tile from the first tile of its group. */
static void symmetrize(Ctx *c) {
    int i, n = c->W * c->H;
    for (i = 0; i < n; i++) {
        int k = c->canon[i];
        if (k != i) {
            c->t[i] = c->t[k];
            c->layer[i] = c->layer[k];
        }
    }
}

/* Write a tile and all its images. */
static void sput(Ctx *c, int x, int y, int v, int layer) {
    int xs[4], ys[4], n, i;
    if (!inb(c, x, y)) return;
    n = images(c, x, y, xs, ys);
    for (i = 0; i < n; i++) {
        int k = IDX(c, xs[i], ys[i]);
        c->t[k] = (uint8_t)v;
        if (layer >= 0) c->layer[k] = (uint8_t)layer;
    }
}

static void put(Ctx *c, int x, int y, int v, int layer) {
    int k;
    if (!inb(c, x, y)) return;
    k = IDX(c, x, y);
    c->t[k] = (uint8_t)v;
    if (layer >= 0) c->layer[k] = (uint8_t)layer;
}

/*---------------------------------------------------------
 * Distances
 *---------------------------------------------------------*/

#define DBIG 0x3FFFFFFF

/* d = chamfer 3-4 distance from each mask tile to the nearest tile not
 * in the mask (the edge of the work area counts as not in the mask). */
static void chamfer(const Ctx *c, const uint8_t *mask, int *d, int edgeIn) {
    int W = c->W, H = c->H, x, y;
    int e = edgeIn ? DBIG : 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) d[y * W + x] = mask[y * W + x] ? DBIG : 0;
    }
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            int k = y * W + x, v = d[k];
            if (!v) continue;
            v = (v < DBIG) ? v : DBIG;
            {
                int l = (x > 0) ? d[k - 1] : e;
                int u = (y > 0) ? d[k - W] : e;
                int ul = (x > 0 && y > 0) ? d[k - W - 1] : e;
                int ur = (x < W - 1 && y > 0) ? d[k - W + 1] : e;
                if (l < DBIG && l + 3 < v) v = l + 3;
                if (u < DBIG && u + 3 < v) v = u + 3;
                if (ul < DBIG && ul + 4 < v) v = ul + 4;
                if (ur < DBIG && ur + 4 < v) v = ur + 4;
            }
            d[k] = v;
        }
    }
    for (y = H - 1; y >= 0; y--) {
        for (x = W - 1; x >= 0; x--) {
            int k = y * W + x, v = d[k];
            if (!v) continue;
            {
                int r = (x < W - 1) ? d[k + 1] : e;
                int dn = (y < H - 1) ? d[k + W] : e;
                int dr = (x < W - 1 && y < H - 1) ? d[k + W + 1] : e;
                int dl = (x > 0 && y < H - 1) ? d[k + W - 1] : e;
                if (r < DBIG && r + 3 < v) v = r + 3;
                if (dn < DBIG && dn + 3 < v) v = dn + 3;
                if (dr < DBIG && dr + 4 < v) v = dr + 4;
                if (dl < DBIG && dl + 4 < v) v = dl + 4;
            }
            d[k] = v;
        }
    }
}

static int isLandT(int t) {
    return t != T_DEEP && t != T_RIVER && t != T_BOAT;
}

/* din: depth inside solid land. dout: distance of deep tiles from any
 * tile that is not deep sea. */
static void computeDistances(Ctx *c) {
    int n = c->W * c->H, i;
    uint8_t *m = (uint8_t *)malloc((size_t)n);
    if (!m) return;
    for (i = 0; i < n; i++) m[i] = (uint8_t)isLandT(c->t[i]);
    chamfer(c, m, c->din, 0);
    for (i = 0; i < n; i++) m[i] = (uint8_t)(c->t[i] == T_DEEP);
    chamfer(c, m, c->dout, 1);
    free(m);
}

/* Remove land pieces smaller than minSize tiles (4-connected). */
static void removeSmallLand(Ctx *c, int minSize) {
    int n = c->W * c->H, i;
    int *lab = (int *)calloc((size_t)n, sizeof(int));
    int *stack = (int *)malloc((size_t)n * sizeof(int));
    int *members = (int *)malloc((size_t)n * sizeof(int));
    int cur = 0;
    if (!lab || !stack || !members) {
        free(lab); free(stack); free(members);
        return;
    }
    for (i = 0; i < n; i++) {
        int sp = 0, cnt = 0, j;
        if (c->t[i] == T_DEEP || lab[i]) continue;
        cur++;
        lab[i] = cur;
        stack[sp++] = i;
        while (sp) {
            int k = stack[--sp], x = k % c->W, y = k / c->W, dd;
            static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
            members[cnt++] = k;
            for (dd = 0; dd < 4; dd++) {
                int nx = x + dx[dd], ny = y + dy[dd], nk;
                if (!inb(c, nx, ny)) continue;
                nk = IDX(c, nx, ny);
                if (c->t[nk] == T_DEEP || lab[nk]) continue;
                lab[nk] = cur;
                stack[sp++] = nk;
            }
        }
        if (cnt < minSize) {
            for (j = 0; j < cnt; j++) c->t[members[j]] = T_DEEP;
        }
    }
    free(lab);
    free(stack);
    free(members);
}

/*---------------------------------------------------------
 * Slider scaling
 *---------------------------------------------------------*/

/* A thickness or count that equals base at the preset's default
 * slider value def and scales with the slider. When the preset has
 * none of it (def 0), the slider adds perUnit at 100. */
static double scd(double base, int slider, int def, double perUnit) {
    if (def > 0) {
        /* The preset has none of it: add some only above the default. */
        if (base <= 0.0) return slider > def ? (double)(slider - def) * perUnit / 100.0 : 0.0;
        return base * (double)slider / (double)def;
    }
    return (double)slider * perUnit / 100.0;
}

static int sci(double base, int slider, int def, double perUnit) {
    return (int)floor(scd(base, slider, def, perUnit) + 0.5);
}

/* Fraction of the preset default: 1.0 at the default. */
static double ratio(int slider, int def) {
    if (def <= 0) return (double)slider / 25.0;
    return (double)slider / (double)def;
}

/*---------------------------------------------------------
 * Shapes
 *---------------------------------------------------------*/

static int outlineInside(const Ctx *c, double u, double v, double nz) {
    double au = fabs(u), av = fabs(v);
    switch (c->p.outline) {
    case OL_BOX:
        return au <= 1.0 && av <= 1.0;
    case OL_OCTAGON: {
        return au <= 1.0 && av <= 1.0 && au + av <= c->cut;
    }
    case OL_ROUND:
        return u * u + v * v <= 1.0;
    case OL_DIAMOND:
        return au + av <= 1.0;
    case OL_CROSS:
        return (au <= 0.42 && av <= 1.0) || (av <= 0.42 && au <= 1.0);
    case OL_CLOVER: {
        double d1, d2;
        if (au <= 0.5 && av <= 0.5) return 1;
        d1 = (au - 0.55) * (au - 0.55) + v * v;
        d2 = u * u + (av - 0.55) * (av - 0.55);
        return d1 <= 0.45 * 0.45 || d2 <= 0.45 * 0.45;
    }
    case OL_ISLAND:
    default:
        return sqrt(u * u + v * v) + (nz - 0.5) * 0.7 <= 0.9;
    }
}

/* Fill a filled shape centred at (x0,y0): kind 0 square, 1 diamond, 2 disc. */
static int inShape(int kind, int dx, int dy, double r) {
    switch (kind) {
    case 0: return abs(dx) <= r && abs(dy) <= r;
    case 1: return abs(dx) + abs(dy) <= r;
    default: return dx * dx + dy * dy <= r * r + r * 0.6;
    }
}

static void fillShape(Ctx *c, int x0, int y0, int kind, double r, int v,
                      int layer, int onlyLayer) {
    int R = (int)ceil(r), dx, dy;
    for (dy = -R; dy <= R; dy++) {
        for (dx = -R; dx <= R; dx++) {
            int x = x0 + dx, y = y0 + dy;
            if (!inb(c, x, y) || !inShape(kind, dx, dy, r)) continue;
            if (onlyLayer >= 0 && c->layer[IDX(c, x, y)] != onlyLayer &&
                c->layer[IDX(c, x, y)] != L_SKEL) continue;
            put(c, x, y, v, layer);
        }
    }
}

static void fillRect(Ctx *c, int x1, int y1, int x2, int y2, int v, int layer) {
    int x, y;
    for (y = y1; y <= y2; y++) {
        for (x = x1; x <= x2; x++) put(c, x, y, v, layer);
    }
}

/* A line 1 tile wide that steps in 8 directions. */
static void line8(Ctx *c, int x0, int y0, int x1, int y1, int v, int layer,
                  int width, int landOnly) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        int ox, oy;
        for (oy = 0; oy < width; oy++) {
            for (ox = 0; ox < width; ox++) {
                int x = x0 + ox, y = y0 + oy;
                if (!inb(c, x, y)) continue;
                if (landOnly && c->t[IDX(c, x, y)] == T_DEEP) continue;
                put(c, x, y, v, layer);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

/*---------------------------------------------------------
 * Fragments
 *---------------------------------------------------------*/

static int fragTerrain(char ch) {
    switch (ch) {
    case 'B': return T_BUILDING;
    case 'h': return T_HALF;
    case '=': return T_ROAD;
    case 'f': return T_FOREST;
    case 'g': return T_GRASS;
    case 'r': return T_RUBBLE;
    case 'c': return T_CRATER;
    case 's': return T_SWAMP;
    case 'w': return T_RIVER;
    case '~': return T_DEEP;
    case 'b': return T_BOAT;
    case 'P': case 'A': return T_GRASS;
    case 'S': return T_DEEP;
    default: return -1;
    }
}

/* Pick a fragment of a set that fits in maxW x maxH (either way round). */
static const MakerFragment *pickFragment(Ctx *c, int set, int maxW, int maxH) {
    int i, cnt = 0, pick;
    for (i = 0; i < makerFragmentCount(); i++) {
        const MakerFragment *f = makerFragment(i);
        if (f->set != set) continue;
        if ((f->w <= maxW && f->h <= maxH) || (f->h <= maxW && f->w <= maxH)) cnt++;
    }
    if (!cnt) return NULL;
    pick = rnd(c, cnt);
    for (i = 0; i < makerFragmentCount(); i++) {
        const MakerFragment *f = makerFragment(i);
        if (f->set != set) continue;
        if ((f->w <= maxW && f->h <= maxH) || (f->h <= maxW && f->w <= maxH)) {
            if (pick-- == 0) return f;
        }
    }
    return NULL;
}

static void addHint(Ctx *c, int kind, int x, int y) {
    if (c->nh[kind] < 64) {
        c->hx[kind][c->nh[kind]] = x;
        c->hy[kind][c->nh[kind]] = y;
        c->nh[kind]++;
    }
}

/* Stamp a fragment centred at (x0,y0) with a random turn and flip so
 * that it fits in maxW x maxH. Returns 1 if stamped. */
static int stampFragment(Ctx *c, const MakerFragment *f, int x0, int y0,
                         int maxW, int maxH, int layer) {
    int rot, flip, fw, fh, i, j;
    if (!f) return 0;
    rot = rnd(c, 4);
    flip = rnd(c, 2);
    fw = (rot & 1) ? f->h : f->w;
    fh = (rot & 1) ? f->w : f->h;
    if (fw > maxW || fh > maxH) {
        rot ^= 1;
        fw = (rot & 1) ? f->h : f->w;
        fh = (rot & 1) ? f->w : f->h;
        if (fw > maxW || fh > maxH) return 0;
    }
    for (j = 0; j < fh; j++) {
        for (i = 0; i < fw; i++) {
            int sx, sy, x, y, tv;
            char ch;
            /* Turn (i,j) in the stamp back into the fragment's own place. */
            switch (rot) {
            case 0: sx = i; sy = j; break;
            case 1: sx = j; sy = f->h - 1 - i; break;
            case 2: sx = f->w - 1 - i; sy = f->h - 1 - j; break;
            default: sx = f->w - 1 - j; sy = i; break;
            }
            if (flip) sx = f->w - 1 - sx;
            ch = f->rows[sy][sx];
            tv = fragTerrain(ch);
            if (tv < 0) continue;
            x = x0 - fw / 2 + i;
            y = y0 - fh / 2 + j;
            if (!inb(c, x, y)) continue;
            put(c, x, y, tv, layer);
            if (ch == 'P') addHint(c, OBJ_PILL, x, y);
            else if (ch == 'A') addHint(c, OBJ_BASE, x, y);
            else if (ch == 'S') addHint(c, OBJ_START, x, y);
        }
    }
    return 1;
}

/*---------------------------------------------------------
 * Land outline and rims (FORMAL and TOWN)
 *---------------------------------------------------------*/

static double uOf(const Ctx *c, int x) { return ((double)x - c->cx) / c->ax; }
static double vOf(const Ctx *c, int y) { return ((double)y - c->cy) / c->ay; }

static void paintOutline(Ctx *c) {
    int x, y;
    uint32_t s = c->seed * 2654435761u + 17u;
    double ns = (c->ax + c->ay) * 0.22;
    if (ns < 4.0) ns = 4.0;
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int k = c->canon[IDX(c, x, y)];
            int kx = k % c->W, ky = k / c->W;
            double nz = fbm(kx / ns, ky / ns, s, 3);
            int land = outlineInside(c, uOf(c, kx), vOf(c, ky), nz);
            put(c, x, y, land ? T_GRASS : T_DEEP, land ? L_FLOOR : L_SEA);
        }
    }
}

/* Cut deep channels so that the land falls into the asked number of
 * islands: 2 a channel down the middle, 3-4 a cross, 5 or more a
 * cross and both diagonals. */
static void cutChannels(Ctx *c) {
    int k = c->p.islands, x, y;
    double cw;
    if (k <= 1) return;
    if (c->sym == SY_ROT4 && k == 2) k = 4;
    cw = (c->ax < c->ay ? c->ax : c->ay) * 0.07;
    if (cw < 1.0) cw = 1.0;
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            double dx = x - c->cx, dy = y - c->cy;
            int cut = fabs(dx) <= cw;
            if (k >= 3 && fabs(dy) <= cw) cut = 1;
            if (k >= 5) {
                double ddx = dx / c->ax * c->ay;
                if (fabs(ddx - dy) <= cw * 1.41 || fabs(ddx + dy) <= cw * 1.41) cut = 1;
            }
            if (cut) put(c, x, y, T_DEEP, L_SEA);
        }
    }
    removeSmallLand(c, 12);
}

/* Paint the rims from the coast inward and the floor. Returns the
 * total rim depth in tiles. */
static int paintRims(Ctx *c, int floorT, int trackT, int trackW) {
    const Preset *pr = c->pr;
    int rr = sci(pr->riverRim, c->p.waterPct, pr->water, 2.0);
    int fr = sci(pr->forestRim, c->p.forestPct, pr->forest, 6.0);
    int br = sci(pr->buildRim, c->p.buildingPct, pr->building, 3.0);
    int x, y, total;
    uint32_t s = c->seed * 40503u + 99u;
    double jag = pr->jag / 10.0;
    double rl[4];
    int rt[4], nr = 0, i;
    if (rr > 4) rr = 4;
    if (fr > 12) fr = 12;
    if (c->noForestRim) fr = 0;
    if (br > 4) br = 4;
    /* Keep a floor in the middle on small maps. */
    {
        double minHalf = (c->ax < c->ay ? c->ax : c->ay);
        double room = minHalf - 4.0;
        while (fr + br + trackW > room && fr + br + trackW > 0) {
            if (fr > 1) fr--;
            else if (br > 0) br--;
            else if (trackW > 0) trackW--;
            else if (fr > 0) fr--;
            else break;
        }
    }
    computeDistances(c);
    if (pr->buildOuter) {
        rt[nr] = T_BUILDING; rl[nr++] = br;
        rt[nr] = T_FOREST;   rl[nr++] = fr;
    } else {
        rt[nr] = T_FOREST;   rl[nr++] = fr;
        rt[nr] = T_BUILDING; rl[nr++] = br;
    }
    rt[nr] = trackT; rl[nr++] = (trackT == NO_TERR) ? 0 : trackW;
    total = fr + br + ((trackT == NO_TERR) ? 0 : trackW);
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int k = IDX(c, x, y);
            double j = 0.0;
            if (jag > 0.0) {
                int ck = c->canon[k];
                j = (fbm((ck % c->W) / 3.0, (ck / c->W) / 3.0, s, 2) - 0.5) * 2.0 * jag;
            }
            if (c->t[k] == T_DEEP) {
                if (rr > 0 && c->dout[k] <= rr * 3 + 1 + (int)(j * 3)) put(c, x, y, T_RIVER, L_RRIM);
                continue;
            }
            {
                double depth = c->din[k] / 3.0 + j;
                double acc = 0.0;
                int done = 0;
                for (i = 0; i < nr; i++) {
                    if (rl[i] <= 0) continue;
                    acc += rl[i];
                    if (depth <= acc + 0.34) {
                        put(c, x, y, rt[i], (i == nr - 1 && rt[i] == trackT) ? L_TRACK : L_RIM);
                        done = 1;
                        break;
                    }
                }
                if (!done) put(c, x, y, floorT, L_FLOOR);
            }
        }
    }
    c->rimTotal = total;
    return total;
}

/* Cut ways in through the rims at the ends of the axes, or on the
 * diagonals when diag is set. A wedge also reaches into the floor as a
 * triangle. Deep water ways in become bays of the sea. */
static void paintGates(Ctx *c, int terr, int w, int wedge, int diag) {
    int x, y;
    double half = (w - 1) / 2.0;
    double fx = c->cx - floor(c->cx), fy = c->cy - floor(c->cy);
    double hx = half, hy = half;
    if (fx > 0.25 && hx < 0.5) hx = 0.5;
    if (fy > 0.25 && hy < 0.5) hy = 0.5;
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int k = IDX(c, x, y);
            if (diag && (c->layer[k] == L_RIM || c->layer[k] == L_TRACK || c->layer[k] == L_RRIM)) {
                double r = c->ay / c->ax, dx = x - c->cx, dy = y - c->cy;
                double a = fabs(dx * r - dy) / 1.414, b = fabs(dx * r + dy) / 1.414;
                if (a <= half + 0.5 || b <= half + 0.5) {
                    if (terr == T_DEEP) put(c, x, y, T_DEEP, L_SEA);
                    else if (c->layer[k] != L_RRIM) put(c, x, y, terr, L_RIM);
                }
            } else if (!diag && (c->layer[k] == L_RIM || c->layer[k] == L_TRACK)) {
                if (fabs(x - c->cx) <= hx + 0.01 || fabs(y - c->cy) <= hy + 0.01) {
                    put(c, x, y, terr, L_RIM);
                }
            } else if (!diag && wedge > 0 && (c->layer[k] == L_FLOOR || c->layer[k] == L_SKEL)) {
                double e = c->din[k] / 3.0 - c->rimTotal;
                double ox = fabs(x - c->cx) - hx, oy = fabs(y - c->cy) - hy;
                if (ox < 0) ox = 0;
                if (oy < 0) oy = 0;
                if (e <= wedge && (ox <= wedge - e || oy <= wedge - e)) {
                    /* only where the axis meets the rim */
                    double ex = fabs(x - c->cx), ey = fabs(y - c->cy);
                    if ((ox <= wedge - e && ey > c->ay * 0.5) || (oy <= wedge - e && ex > c->ax * 0.5)) {
                        put(c, x, y, terr, L_RIM);
                    }
                }
            }
        }
    }
}

/*---------------------------------------------------------
 * FORMAL family
 *---------------------------------------------------------*/

static int floorTerrain(const Ctx *c) {
    const Preset *pr = c->pr;
    int f = pr->floor;
    if (f == T_ROAD && c->p.roadPct < pr->road / 2) f = T_GRASS;
    else if (f == T_GRASS && c->p.roadPct > pr->road + (100 - pr->road) / 2) f = T_ROAD;
    return f;
}

/* Lines across the floor: axes, diagonals, a ring and a panel grid. */
static void paintSkeleton(Ctx *c, int skel, int terr, int w) {
    int x, y;
    double half = (w - 1) / 2.0;
    double fx = c->cx - floor(c->cx), fy = c->cy - floor(c->cy);
    double hx = half, hy = half;
    double maxDepth = 0.0;
    int panel = c->pr->panel;
    if (fx > 0.25 && hx < 0.5) hx = 0.5;
    if (fy > 0.25 && hy < 0.5) hy = 0.5;
    if (panel > 0) {
        panel = (int)floor(panel * sqrt(ratio(c->p.roadPct, c->pr->road) > 0.2 ?
                                         1.0 / ratio(c->p.roadPct, c->pr->road) : 5.0) + 0.5);
        if (panel < 5) panel = 5;
    }
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            double d = c->din[IDX(c, x, y)] / 3.0;
            if (d > maxDepth) maxDepth = d;
        }
    }
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int k = IDX(c, x, y), on = 0;
            double dx = x - c->cx, dy = y - c->cy;
            if (c->layer[k] != L_FLOOR) continue;
            if ((skel & SK_AXES) && (fabs(dx) <= hx + 0.01 || fabs(dy) <= hy + 0.01)) on = 1;
            if (skel & SK_DIAG) {
                double r = c->ay / c->ax;
                double a = fabs(dx * r - dy) / 1.414, b = fabs(dx * r + dy) / 1.414;
                if (a <= half + 0.5 || b <= half + 0.5) on = 1;
            }
            if (skel & SK_RING) {
                double ringD = c->rimTotal + (maxDepth - c->rimTotal) * 0.45;
                if (fabs(c->din[k] / 3.0 - ringD) <= half + 0.5) on = 1;
            }
            if ((skel & SK_PANEL) && panel > 0) {
                double ax2 = fabs(dx) + (fx > 0.25 ? 0.5 : 0.0);
                double ay2 = fabs(dy) + (fy > 0.25 ? 0.5 : 0.0);
                int gx = ((int)floor(ax2 + 0.5)) % panel;
                int gy = ((int)floor(ay2 + 0.5)) % panel;
                if (gx < w || gy < w) on = 1;
            }
            if (on) put(c, x, y, terr, L_SKEL);
        }
    }
}

static int podKindFor(Ctx *c) {
    const Preset *pr = c->pr;
    int kinds[3], i;
    double wts[3], sum = 0.0, r;
    kinds[0] = pr->pod1; kinds[1] = pr->pod2; kinds[2] = pr->pod3;
    for (i = 0; i < 3; i++) {
        double w = 1.0;
        switch (kinds[i]) {
        case POD_POND: w = ratio(c->p.waterPct, pr->water); break;
        case POD_HEDGE: case POD_DIAMOND: case POD_LAWN: case POD_CHECKER:
            w = ratio(c->p.forestPct, pr->forest); break;
        case POD_BLOCK: w = ratio(c->p.buildingPct, pr->building); break;
        case POD_RING: w = 1.0; break;
        default: w = 0.0; break;
        }
        if (kinds[i] == POD_NONE) w = 0.0;
        wts[i] = w;
        sum += w;
    }
    if (sum <= 0.0) return POD_NONE;
    r = rfloat(c) * sum;
    for (i = 0; i < 3; i++) {
        if (r < wts[i]) return kinds[i];
        r -= wts[i];
    }
    return kinds[2];
}

static void paintPod(Ctx *c, int kind, int x0, int y0, double R) {
    const Preset *pr = c->pr;
    double fk = sqrt(ratio(c->p.forestPct, pr->forest));
    double bk = sqrt(ratio(c->p.buildingPct, pr->building));
    double wk = sqrt(ratio(c->p.waterPct, pr->water));
    int shape = rnd(c, 3);
    switch (kind) {
    case POD_RING: {
        double rb = R * 0.30 * bk, rf = R * 0.30 * bk + R * 0.38 * fk;
        if (rf > R - 1.0) rf = R - 1.0;
        if (shape == 2) shape = 0;
        fillShape(c, x0, y0, shape, R, T_GRASS, L_POD, L_FLOOR);
        if (rf >= 1.0) fillShape(c, x0, y0, shape, rf, T_FOREST, L_POD, L_POD);
        if (rb >= 0.5) fillShape(c, x0, y0, shape, rb, c->coreT, L_POD, L_POD);
        break;
    }
    case POD_HEDGE: {
        int hw = (int)(R * 0.8 * fk + 0.5), hh = (int)(R * 0.45 * fk + 0.5);
        if (rnd(c, 2)) { int tt = hw; hw = hh; hh = tt; }
        if (hw < 1 || hh < 1) break;
        fillRect(c, x0 - hw, y0 - hh, x0 + hw, y0 + hh, T_FOREST, L_POD);
        break;
    }
    case POD_POND: {
        double rp = R * 0.6 * wk;
        if (rp < 1.0) break;
        fillShape(c, x0, y0, 2, rp + 1.0, T_SWAMP, L_POD, L_FLOOR);
        fillShape(c, x0, y0, 2, rp, T_RIVER, L_POD, L_POD);
        break;
    }
    case POD_DIAMOND: {
        double rd = R * 0.85;
        if (c->noForestRim) {
            /* With no forest rim the forest stands in round clumps. */
            if (R * fk >= 1.5) fillShape(c, x0, y0, 2, R * 0.9 * (fk > 1.0 ? 1.0 : fk), T_FOREST, L_POD, L_FLOOR);
            break;
        }
        fillShape(c, x0, y0, 1, rd, T_GRASS, L_POD, L_FLOOR);
        if (rd * fk * 0.7 >= 1.0) fillShape(c, x0, y0, 1, rd * 0.7 * fk, T_FOREST, L_POD, L_POD);
        break;
    }
    case POD_BLOCK: {
        int hw = (int)(R * 0.75 + 0.5), bw = (int)(R * 0.45 * bk + 0.5);
        fillRect(c, x0 - hw, y0 - hw, x0 + hw, y0 + hw, T_GRASS, L_POD);
        if (bw >= 1) {
            fillRect(c, x0 - bw, y0 - bw, x0 + bw, y0 + bw, T_BUILDING, L_POD);
            /* a way in on one side */
            if (rnd(c, 2)) fillRect(c, x0, y0 - bw, x0, y0 + bw, T_HALF, L_POD);
        }
        break;
    }
    case POD_LAWN: {
        int hw = (int)(R * 0.8 + 0.5), e = (int)(R * 0.25 * fk + 0.5);
        fillShape(c, x0, y0, shape == 2 ? 2 : 0, hw, T_FOREST, L_POD, L_FLOOR);
        if (hw - e >= 1) fillShape(c, x0, y0, shape == 2 ? 2 : 0, hw - (e < 1 ? 1 : e), T_GRASS, L_POD, L_POD);
        break;
    }
    case POD_CHECKER: {
        int hw = (int)(R * 0.85 + 0.5), cs = R >= 6.0 ? 3 : 2, dx, dy;
        double keep = fk > 1.0 ? 1.0 : fk;
        for (dy = -hw; dy <= hw; dy++) {
            for (dx = -hw; dx <= hw; dx++) {
                int gx = (dx + hw) / cs, gy = (dy + hw) / cs;
                int forest = ((gx + gy) & 1) && (hash2(gx, gy, c->seed + 71u) & 0xFF) < keep * 255.0;
                put(c, x0 + dx, y0 + dy, forest ? T_FOREST : T_GRASS, L_POD);
            }
        }
        break;
    }
    case POD_FRAG: {
        const MakerFragment *f = pickFragment(c, pr->fragSet, (int)(R * 2) + 1, (int)(R * 2) + 1);
        if (!stampFragment(c, f, x0, y0, (int)(R * 2) + 1, (int)(R * 2) + 1, L_POD)) {
            paintPod(c, pr->pod1 == POD_FRAG ? POD_HEDGE : pr->pod1, x0, y0, R);
        }
        break;
    }
    default:
        break;
    }
}

static void addPod(Ctx *c, int x, int y, int r) {
    if (c->npod < 96) {
        c->podx[c->npod] = x;
        c->pody[c->npod] = y;
        c->podr[c->npod] = r;
        c->npod++;
    }
}

/* Pods on a grid of cells over the floor. Larger maps get more cells. */
static void paintPods(Ctx *c, int centreFeature) {
    const Preset *pr = c->pr;
    double ix = c->ax - c->rimTotal, iy = c->ay - c->rimTotal;
    double cellN = pr->cell;
    int nx, ny, i, j;
    double chance = c->p.detail * 0.11;
    if (ix < 3 || iy < 3) return;
    nx = clampi((int)floor(2.0 * ix / cellN + 0.5), 1, 8);
    ny = clampi((int)floor(2.0 * iy / cellN + 0.5), 1, 8);
    if (c->sym == SY_ROT4) { if (nx < ny) nx = ny; ny = nx; }
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            double px = c->cx + (i - (nx - 1) / 2.0) * (2.0 * ix / nx);
            double py = c->cy + (j - (ny - 1) / 2.0) * (2.0 * iy / ny);
            int x0 = (int)floor(px + 0.5), y0 = (int)floor(py + 0.5);
            double cw = 2.0 * ix / nx, ch = 2.0 * iy / ny;
            double R = (cw < ch ? cw : ch) * 0.36;
            int isCentre = fabs(px - c->cx) < 1.0 && fabs(py - c->cy) < 1.0;
            int kind;
            if (!inb(c, x0, y0) || !inFund(c, x0, y0)) continue;
            if (R > 9.0) R = 9.0;
            {
                double room = c->din[IDX(c, x0, y0)] / 3.0 - c->rimTotal - 1.0;
                if (R > room) R = room;
            }
            if (R < 2.0) continue;
            if (isCentre && centreFeature != CEN_NONE) continue;
            if (rfloat(c) >= chance) continue;
            kind = podKindFor(c);
            if (pr->fragSet >= 0 && c->p.detail >= 7 && rnd(c, 4) == 0) kind = POD_FRAG;
            if (kind == POD_NONE) continue;
            paintPod(c, kind, x0, y0, R);
            addPod(c, x0, y0, (int)R);
        }
    }
}

static void paintCentre(Ctx *c, int feature) {
    int x0 = (int)floor(c->cx), y0 = (int)floor(c->cy);
    double inner = (c->ax < c->ay ? c->ax : c->ay) - c->rimTotal;
    double wk = sqrt(ratio(c->p.waterPct, c->pr->water));
    double R;
    int dx, dy;
    if (inner < 4) return;
    switch (feature) {
    case CEN_POD: {
        int kind = podKindFor(c);
        R = inner * 0.24;
        if (R > 10.0) R = 10.0;
        if (R < 2.0) return;
        if (kind == POD_NONE) return;
        paintPod(c, kind, x0, y0, R);
        addPod(c, x0, y0, (int)R);
        break;
    }
    case CEN_POND:
    case CEN_DEEP: {
        double rp = inner * (feature == CEN_POND ? 0.30 : 0.18) * wk;
        if (feature == CEN_POND && rp < 3.0) rp = 3.0;
        if (rp < 1.0) return;
        R = rp;
        for (dy = -(int)R - 3; dy <= (int)R + 3; dy++) {
            for (dx = -(int)R - 3; dx <= (int)R + 3; dx++) {
                double ddx = x0 + dx - c->cx, ddy = y0 + dy - c->cy;
                double d = sqrt(ddx * ddx + ddy * ddy);
                int x = x0 + dx, y = y0 + dy;
                if (!inb(c, x, y)) continue;
                if (d <= R) put(c, x, y, T_DEEP, L_CENTRE);
                else if (d <= R + 1.0) put(c, x, y, T_RIVER, L_CENTRE);
                else if (d <= R + 2.0 && c->pr->buildOuter) put(c, x, y, T_GRASS, L_CENTRE);
            }
        }
        break;
    }
    case CEN_ROUND: {
        R = inner * 0.16;
        if (R < 2.0) R = 2.0;
        for (dy = -(int)R - 2; dy <= (int)R + 2; dy++) {
            for (dx = -(int)R - 2; dx <= (int)R + 2; dx++) {
                double ddx = x0 + dx - c->cx, ddy = y0 + dy - c->cy;
                double d = sqrt(ddx * ddx + ddy * ddy);
                int x = x0 + dx, y = y0 + dy;
                if (!inb(c, x, y)) continue;
                if (d <= R - 1.0) put(c, x, y, T_GRASS, L_CENTRE);
                else if (d <= R + 1.0) put(c, x, y, T_ROAD, L_CENTRE);
            }
        }
        break;
    }
    case CEN_FOUNTAIN: {
        R = inner * 0.10 * wk + 1.0;
        for (dy = -(int)R - 4; dy <= (int)R + 4; dy++) {
            for (dx = -(int)R - 4; dx <= (int)R + 4; dx++) {
                double ddx = x0 + dx - c->cx, ddy = y0 + dy - c->cy;
                double d = sqrt(ddx * ddx + ddy * ddy);
                int x = x0 + dx, y = y0 + dy;
                if (!inb(c, x, y)) continue;
                if (d <= R - 1.5) put(c, x, y, T_GRASS, L_CENTRE);
                else if (d <= R) put(c, x, y, T_RIVER, L_CENTRE);
                else if (d <= R + 2.0) put(c, x, y, T_ROAD, L_CENTRE);
            }
        }
        break;
    }
    default:
        break;
    }
}

/* Extra forest, ponds and walls when a slider is above the preset. */
static void paintExtras(Ctx *c, int onLayer) {
    const Preset *pr = c->pr;
    double extraF = (c->p.forestPct - pr->forest) / 100.0;
    double extraW = (c->p.waterPct - pr->water) / 100.0;
    double extraB = (c->p.buildingPct - pr->building) / 100.0;
    uint32_t s = c->seed * 7919u + 3u;
    int x, y;
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int k = IDX(c, x, y);
            double n1, n2, n3;
            if (c->layer[k] != onLayer) continue;
            n1 = fbm(x / 6.0, y / 6.0, s, 3);
            n2 = fbm(x / 9.0, y / 9.0, s + 55u, 3);
            n3 = (double)(hash2(x, y, s + 9u) & 0xFFFF) / 65535.0;
            if (extraW > 0.0 && n2 > 1.0 - extraW * 0.45) put(c, x, y, T_RIVER, -1);
            else if (extraF > 0.0 && n1 > 1.0 - extraF * 0.7) put(c, x, y, T_FOREST, -1);
            else if (extraB > 0.0 && n3 < extraB * 0.06) put(c, x, y, T_BUILDING, -1);
        }
    }
}

static void paintFormal(Ctx *c) {
    const Preset *pr = c->pr;
    int floorT = floorTerrain(c);
    int trackT = pr->track;
    int skelT = pr->skelTerr;
    int skelW = pr->skelW;
    if (trackT == T_GRASS && c->p.roadPct > 85) trackT = T_ROAD;
    if (skelT == T_ROAD && c->p.roadPct < pr->road / 3) skelT = T_GRASS;
    if (skelT == floorT) skelT = (floorT == T_ROAD) ? T_GRASS : T_ROAD;
    if (c->p.roadPct > pr->road + 25 && skelW < 3) skelW++;
    paintOutline(c);
    cutChannels(c);
    paintRims(c, floorT, trackT, pr->trackW);
    paintSkeleton(c, c->skel, skelT, skelW);
    if (pr->lobes && c->p.outline == OL_CLOVER && c->p.forestPct * 2 >= pr->forest) {
        /* The lobes of the clover are forest; the square in the middle
         * and the lines across the floor stay open. */
        int x, y;
        for (y = 0; y < c->H; y++) {
            for (x = 0; x < c->W; x++) {
                int k = IDX(c, x, y);
                double au = fabs(uOf(c, x)), av = fabs(vOf(c, y));
                if (c->layer[k] != L_FLOOR) continue;
                if (au <= 0.5 && av <= 0.5) continue;
                if (au <= 0.5 + 1.5 / c->ax && av <= 0.5 + 1.5 / c->ay) continue;
                put(c, x, y, T_FOREST, L_RIM);
            }
        }
    }
    paintPods(c, pr->centre);
    paintCentre(c, pr->centre);
    paintExtras(c, L_FLOOR);
    if (pr->gateW > 0) {
        int wedge = 0;
        if (pr->gateStyle == 1) {
            double inner = (c->ax < c->ay ? c->ax : c->ay) - c->rimTotal;
            wedge = (int)(inner * 0.12 * sqrt(ratio(c->p.buildingPct, pr->building)) + 0.5);
        }
        paintGates(c, pr->gateTerr, pr->gateW, wedge, pr->gateStyle == 2);
    }
}

/*---------------------------------------------------------
 * TOWN family
 *---------------------------------------------------------*/

typedef struct Rect { int x1, y1, x2, y2; } Rect;

/* Room kinds of the TOWN family. Sluggo's rooms hold mazes, ponds,
 * checker fields, forest stars, free forest clumps and rows of small
 * houses as well as single houses and hedges. */
#define RK_HOUSES  0   /* a few houses */
#define RK_HEDGE   1   /* a forest block or a forest ring */
#define RK_POND    2   /* a pond with a swamp edge and trees round it */
#define RK_PARK    3   /* single trees */
#define RK_FRAG    4   /* a stored fragment */
#define RK_MAZE    5   /* a forest maze with paths one tile wide */
#define RK_CHECKER 6   /* a forest and grass checker field */
#define RK_STAR    7   /* a forest star with eight arms */
#define RK_CLUMPS  8   /* free forest clumps */
#define RK_CLUSTER 9   /* rows of one small house shape */
#define RK_COUNT   10

static void roomHouses(Ctx *c, Rect r, int n) {
    int w = r.x2 - r.x1 + 1, h = r.y2 - r.y1 + 1, i;
    for (i = 0; i < n; i++) {
        int hw = rrange(c, 1, w >= 6 ? 3 : 1), hh = rrange(c, 1, h >= 6 ? 2 : 1);
        int hx, hy, form = rnd(c, 4);
        if (w - 2 * hw - 2 < 1 || h - 2 * hh - 2 < 1) { hw = 1; hh = 1; }
        if (w < 4 || h < 4) { hw = 0; hh = 0; }
        hx = rrange(c, r.x1 + 1 + hw, r.x2 - 1 - hw);
        hy = rrange(c, r.y1 + 1 + hh, r.y2 - 1 - hh);
        if (form == 0 || hw == 0) {
            fillRect(c, hx - hw, hy - hh, hx + hw, hy + hh, T_BUILDING, L_BLOCK);
        } else if (form == 1) {   /* H */
            fillRect(c, hx - hw, hy - hh, hx - hw, hy + hh, T_BUILDING, L_BLOCK);
            fillRect(c, hx + hw, hy - hh, hx + hw, hy + hh, T_BUILDING, L_BLOCK);
            fillRect(c, hx - hw, hy, hx + hw, hy, T_BUILDING, L_BLOCK);
        } else if (form == 2) {   /* plus */
            fillRect(c, hx - hw, hy, hx + hw, hy, T_BUILDING, L_BLOCK);
            fillRect(c, hx, hy - hh, hx, hy + hh, T_BUILDING, L_BLOCK);
        } else {                  /* ring with a gap */
            fillRect(c, hx - hw, hy - hh, hx + hw, hy + hh, T_BUILDING, L_BLOCK);
            if (hw >= 1 && hh >= 1) {
                fillRect(c, hx - hw + 1, hy - hh + 1, hx + hw - 1, hy + hh - 1, T_GRASS, L_BLOCK);
                put(c, hx, hy + hh, T_HALF, L_BLOCK);
            }
        }
    }
}

/* A maze of forest walls with grass paths one tile wide, made by a
 * depth-first walk over cells two tiles apart, with a few extra gaps
 * for loops and a way in on each side. Returns 0 if it does not fit. */
static int roomMaze(Ctx *c, Rect r, int wallT, int layer) {
    static const int ddx[4] = { 1, -1, 0, 0 }, ddy[4] = { 0, 0, 1, -1 };
    int x1 = r.x1, y1 = r.y1, x2 = r.x2, y2 = r.y2;
    int nx, ny, n, i, sp = 0, cur;
    int *stack;
    uint8_t *vis;
    if ((x2 - x1) & 1) x2--;
    if ((y2 - y1) & 1) y2--;
    nx = (x2 - x1) / 2;
    ny = (y2 - y1) / 2;
    if (nx < 2 || ny < 2) return 0;
    n = nx * ny;
    stack = (int *)malloc((size_t)n * sizeof(int));
    vis = (uint8_t *)calloc((size_t)n, 1);
    if (!stack || !vis) {
        free(stack);
        free(vis);
        return 0;
    }
    fillRect(c, x1, y1, x2, y2, wallT, layer);
    cur = rnd(c, n);
    vis[cur] = 1;
    stack[sp++] = cur;
    put(c, x1 + 1 + 2 * (cur % nx), y1 + 1 + 2 * (cur / nx), T_GRASS, layer);
    while (sp > 0) {
        int cx, cy, opts[4], no = 0, d;
        cur = stack[sp - 1];
        cx = cur % nx;
        cy = cur / nx;
        for (d = 0; d < 4; d++) {
            int qx = cx + ddx[d], qy = cy + ddy[d];
            if (qx < 0 || qy < 0 || qx >= nx || qy >= ny) continue;
            if (!vis[qy * nx + qx]) opts[no++] = d;
        }
        if (!no) {
            sp--;
            continue;
        }
        d = opts[rnd(c, no)];
        {
            int qx = cx + ddx[d], qy = cy + ddy[d];
            put(c, x1 + 1 + 2 * cx + ddx[d], y1 + 1 + 2 * cy + ddy[d], T_GRASS, layer);
            put(c, x1 + 1 + 2 * qx, y1 + 1 + 2 * qy, T_GRASS, layer);
            vis[qy * nx + qx] = 1;
            stack[sp++] = qy * nx + qx;
        }
    }
    for (i = 0; i < n / 7; i++) {
        int cx = rnd(c, nx), cy = rnd(c, ny);
        if (rnd(c, 2) && cx < nx - 1) put(c, x1 + 2 + 2 * cx, y1 + 1 + 2 * cy, T_GRASS, layer);
        else if (cy < ny - 1) put(c, x1 + 1 + 2 * cx, y1 + 2 + 2 * cy, T_GRASS, layer);
    }
    put(c, x1, y1 + 1 + 2 * rnd(c, ny), T_GRASS, layer);
    put(c, x2, y1 + 1 + 2 * rnd(c, ny), T_GRASS, layer);
    put(c, x1 + 1 + 2 * rnd(c, nx), y1, T_GRASS, layer);
    put(c, x1 + 1 + 2 * rnd(c, nx), y2, T_GRASS, layer);
    free(stack);
    free(vis);
    return 1;
}

/* Checker field of single forest tiles, in the whole room or in a
 * diamond inside it. */
static void roomChecker(Ctx *c, Rect r, int diamond) {
    double mx = (r.x1 + r.x2) / 2.0, my = (r.y1 + r.y2) / 2.0;
    double rx = (r.x2 - r.x1) / 2.0 + 0.5, ry = (r.y2 - r.y1) / 2.0 + 0.5;
    int x, y;
    for (y = r.y1; y <= r.y2; y++) {
        for (x = r.x1; x <= r.x2; x++) {
            if (diamond && fabs(x - mx) / rx + fabs(y - my) / ry > 1.0) continue;
            if (((x - r.x1) + (y - r.y1)) & 1) continue;
            put(c, x, y, T_FOREST, L_BLOCK);
        }
    }
}

/* A forest star: a core and eight arms, axis arms three tiles wide. */
static void roomStar(Ctx *c, Rect r) {
    double mx = (r.x1 + r.x2) / 2.0, my = (r.y1 + r.y2) / 2.0;
    double R = ((r.x2 - r.x1) < (r.y2 - r.y1) ? (r.x2 - r.x1) : (r.y2 - r.y1)) / 2.0;
    /* Thin arms on a small star so that the grass between them shows. */
    double armA = R >= 6.0 ? 1.01 : 0.51, armD = R >= 7.0 ? 1.01 : 0.51;
    int x, y;
    for (y = r.y1; y <= r.y2; y++) {
        for (x = r.x1; x <= r.x2; x++) {
            double ax = fabs(x - mx), ay = fabs(y - my);
            double d = ax > ay ? ax : ay;
            int on = 0;
            if (d <= R * 0.22 + 0.5) on = 1;
            else if (d <= R + 0.01) {
                if (ax <= armA || ay <= armA || fabs(ax - ay) <= armD) on = 1;
            }
            if (on) put(c, x, y, T_FOREST, L_BLOCK);
        }
    }
}

/* Free forest clumps from noise, with a few single trees. cover is
 * about the part of the room that becomes forest. */
static void roomClumps(Ctx *c, Rect r, double cover) {
    uint32_t s = hash2(r.x1, r.y1, c->seed + 977u);
    double thr = 0.5 + (0.4 - cover) * 0.45;
    int x, y;
    for (y = r.y1; y <= r.y2; y++) {
        for (x = r.x1; x <= r.x2; x++) {
            double nz = fbm(x / 3.0, y / 3.0, s, 2);
            if (nz > thr || (hash2(x, y, s + 5u) & 63u) < 2u) put(c, x, y, T_FOREST, L_BLOCK);
        }
    }
}

/* A pond of uneven outline: river, a swamp edge, and forest clumps
 * round it. */
static void roomPond(Ctx *c, Rect r) {
    uint32_t s = hash2(r.x2, r.y2, c->seed + 4441u);
    double mx = (r.x1 + r.x2) / 2.0, my = (r.y1 + r.y2) / 2.0;
    double rx = (r.x2 - r.x1) / 2.0, ry = (r.y2 - r.y1) / 2.0;
    int x, y;
    if (rx < 2.0 || ry < 2.0) return;
    for (y = r.y1; y <= r.y2; y++) {
        for (x = r.x1; x <= r.x2; x++) {
            double u = (x - mx) / rx, v = (y - my) / ry;
            double d = sqrt(u * u + v * v) + (fbm(x / 2.5, y / 2.5, s, 2) - 0.5) * 0.9;
            if (d <= 0.68) put(c, x, y, T_RIVER, L_BLOCK);
            else if (d <= 0.78) put(c, x, y, T_SWAMP, L_BLOCK);
            else if (d <= 1.15 && fbm(x / 2.2, y / 2.2, s + 7u, 2) > 0.54) put(c, x, y, T_FOREST, L_BLOCK);
        }
    }
}

/* Rows of one small house shape, or of two shapes in turn. */
static void roomCluster(Ctx *c, Rect r) {
    static const struct { int w, h; const char *rows[3]; } kMotif[] = {
        { 3, 3, { ".B.", "BBB", ".B." } },
        { 3, 3, { "B.B", "BBB", "B.B" } },
        { 2, 2, { "BB", "BB", "" } },
        { 1, 3, { "B", "B", "B" } },
        { 3, 3, { "BBB", "B.B", "BhB" } },
        { 3, 2, { "BBB", ".B.", "" } },
        { 2, 3, { "BB", "BB", "BB" } },
    };
    int nm = (int)(sizeof(kMotif) / sizeof(kMotif[0]));
    int m1 = rnd(c, nm), m2 = rnd(c, 3) ? m1 : rnd(c, nm);
    int mw = kMotif[m1].w > kMotif[m2].w ? kMotif[m1].w : kMotif[m2].w;
    int mh = kMotif[m1].h > kMotif[m2].h ? kMotif[m1].h : kMotif[m2].h;
    int gx = rrange(c, 1, 2), gy = rrange(c, 1, 2);
    int w = r.x2 - r.x1 + 1, h = r.y2 - r.y1 + 1;
    int nx = (w + gx) / (mw + gx), ny = (h + gy) / (mh + gy);
    int ox, oy, i, j, a, b;
    if (nx < 1 || ny < 1) return;
    ox = r.x1 + (w - (nx * (mw + gx) - gx)) / 2;
    oy = r.y1 + (h - (ny * (mh + gy) - gy)) / 2;
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            int m = ((i + j) & 1) ? m2 : m1;
            int px = ox + i * (mw + gx) + (mw - kMotif[m].w) / 2;
            int py = oy + j * (mh + gy) + (mh - kMotif[m].h) / 2;
            for (b = 0; b < kMotif[m].h; b++) {
                for (a = 0; a < kMotif[m].w; a++) {
                    char ch = kMotif[m].rows[b][a];
                    if (ch == 'B') put(c, px + a, py + b, T_BUILDING, L_BLOCK);
                    else if (ch == 'h') put(c, px + a, py + b, T_HALF, L_BLOCK);
                }
            }
        }
    }
}

static Rect inset(Rect r, int m) {
    r.x1 += m; r.y1 += m; r.x2 -= m; r.y2 -= m;
    return r;
}

static void townBlock(Ctx *c, Rect r) {
    const Preset *pr = c->pr;
    int w = r.x2 - r.x1 + 1, h = r.y2 - r.y1 + 1;
    double wh = ratio(c->p.buildingPct, pr->building);
    double wf = ratio(c->p.forestPct, pr->forest);
    double ww = ratio(c->p.waterPct, pr->water);
    double wd = c->p.detail / 10.0;
    double wdp = 0.3 + wd;
    int square = w * 10 <= h * 16 && h * 10 <= w * 16;
    int kind, frame;
    Rect in;
    if (w < 2 || h < 2) {
        fillRect(c, r.x1, r.y1, r.x2, r.y2, T_GRASS, L_BLOCK);
        return;
    }
    fillRect(c, r.x1, r.y1, r.x2, r.y2, T_GRASS, L_BLOCK);
    {
        double wt[RK_COUNT], sum = 0.0, pick;
        wt[RK_HOUSES] = 0.8 * wh;
        wt[RK_HEDGE] = 0.35 * wf;
        wt[RK_POND] = (w >= 7 && h >= 7) ? 0.8 * ww : 0.0;
        wt[RK_PARK] = 0.15;
        wt[RK_FRAG] = (pr->fragSet >= 0) ? 0.6 * wd : 0.0;
        wt[RK_MAZE] = (w >= 9 && h >= 9) ? 0.8 * wf * wdp : 0.0;
        wt[RK_CHECKER] = (w >= 6 && h >= 6) ? 0.5 * wf * wdp : 0.0;
        wt[RK_STAR] = (w >= 9 && h >= 9 && square) ? 0.6 * wf * wdp : 0.0;
        wt[RK_CLUMPS] = 0.8 * wf;
        wt[RK_CLUSTER] = (w >= 6 && h >= 5) ? 0.9 * wh : 0.0;
        for (kind = 0; kind < RK_COUNT; kind++) {
            wt[kind] /= 1.0 + c->roomUsed[kind];
            sum += wt[kind];
        }
        if (sum <= 0.0) return;
        pick = rfloat(c) * sum;
        for (kind = 0; kind < RK_COUNT - 1; kind++) {
            if (pick < wt[kind]) break;
            pick -= wt[kind];
        }
        c->roomUsed[kind]++;
    }
    /* Sluggo often rings a room with a hedge one tile wide. */
    frame = w >= 8 && h >= 8 && (kind == RK_STAR || rnd(c, 100) < 35 * wf);
    if (frame) {
        fillRect(c, r.x1, r.y1, r.x2, r.y2, T_FOREST, L_BLOCK);
        fillRect(c, r.x1 + 1, r.y1 + 1, r.x2 - 1, r.y2 - 1, T_GRASS, L_BLOCK);
        /* a gap in the hedge on each side */
        put(c, (r.x1 + r.x2) / 2, r.y1, T_GRASS, L_BLOCK);
        put(c, (r.x1 + r.x2) / 2, r.y2, T_GRASS, L_BLOCK);
        put(c, r.x1, (r.y1 + r.y2) / 2, T_GRASS, L_BLOCK);
        put(c, r.x2, (r.y1 + r.y2) / 2, T_GRASS, L_BLOCK);
    }
    in = inset(r, frame ? 1 : 0);
    switch (kind) {
    case RK_HOUSES: {
        int n = 1 + (w * h) / 40;
        if (n > 4) n = 4;
        roomHouses(c, in, n);
        break;
    }
    case RK_HEDGE: {
        int m = (w >= 6 && h >= 6) ? 1 : 0;
        if (rnd(c, 2) && w >= 7 && h >= 7) {
            fillRect(c, r.x1 + m, r.y1 + m, r.x2 - m, r.y2 - m, T_FOREST, L_BLOCK);
            fillRect(c, r.x1 + m + 2, r.y1 + m + 2, r.x2 - m - 2, r.y2 - m - 2, T_GRASS, L_BLOCK);
        } else {
            fillRect(c, r.x1 + m, r.y1 + m, r.x2 - m, r.y2 - m, T_FOREST, L_BLOCK);
        }
        break;
    }
    case RK_POND:
        roomPond(c, inset(in, 1));
        if (w * h >= 160 && rnd(c, 2)) roomHouses(c, in, 1);
        break;
    case RK_PARK: {
        int i, n = (w * h) / 12;
        for (i = 0; i < n; i++) {
            int px = rrange(c, in.x1, in.x2);
            int py = rrange(c, in.y1, in.y2);
            put(c, px, py, T_FOREST, L_BLOCK);
        }
        break;
    }
    case RK_MAZE:
        if (!roomMaze(c, inset(in, 1), T_FOREST, L_BLOCK)) roomClumps(c, inset(in, 1), 0.35);
        break;
    case RK_CHECKER:
        roomChecker(c, inset(in, 1), rnd(c, 3) == 0);
        break;
    case RK_STAR:
        roomStar(c, inset(in, 1));
        break;
    case RK_CLUMPS:
        roomClumps(c, inset(in, 1), 0.3 + 0.1 * (wf > 1.0 ? 1.0 : wf));
        if (rnd(c, 2)) roomHouses(c, in, 1 + (w * h) / 120);
        break;
    case RK_CLUSTER:
        roomCluster(c, inset(in, 1));
        break;
    default: {  /* fragment */
        const MakerFragment *f = pickFragment(c, pr->fragSet, w, h);
        if (!stampFragment(c, f, (r.x1 + r.x2 + 1) / 2, (r.y1 + r.y2 + 1) / 2, w, h, L_BLOCK)) {
            fillRect(c, r.x1 + 1, r.y1 + 1, r.x2 - 1, r.y2 - 1, T_FOREST, L_BLOCK);
        }
        break;
    }
    }
}

static void townSplit(Ctx *c, Rect r, int sw, int depth) {
    const Preset *pr = c->pr;
    int w = r.x2 - r.x1 + 1, h = r.y2 - r.y1 + 1;
    int maxB = rrange(c, pr->blockMin, pr->blockMax);
    if (depth > 12 || (w <= maxB && h <= maxB) || (w < 2 * 4 + sw && h < 2 * 4 + sw)) {
        townBlock(c, r);
        return;
    }
    if (w >= h && w >= 2 * 4 + sw) {
        int cut = rrange(c, r.x1 + 4, r.x2 - 4 - sw + 1);
        Rect a = r, b = r;
        a.x2 = cut - 1;
        b.x1 = cut + sw;
        townSplit(c, a, sw, depth + 1);
        townSplit(c, b, sw, depth + 1);
    } else {
        int cut = rrange(c, r.y1 + 4, r.y2 - 4 - sw + 1);
        Rect a = r, b = r;
        a.y2 = cut - 1;
        b.y1 = cut + sw;
        townSplit(c, a, sw, depth + 1);
        townSplit(c, b, sw, depth + 1);
    }
}

static void paintTown(Ctx *c) {
    const Preset *pr = c->pr;
    int sw = clampi(1 + c->p.roadPct / 30, 1, 4);
    int x, y, bx1 = c->W, by1 = c->H, bx2 = -1, by2 = -1;
    int ef = sci(pr->endForest, c->p.forestPct, pr->forest, 0.0);
    int ringW = c->p.roadPct < 10 ? 0 : pr->trackW;
    uint8_t *saveT = NULL, *saveL = NULL;
    paintOutline(c);
    cutChannels(c);
    paintRims(c, T_ROAD, ringW > 0 ? pr->track : NO_TERR, ringW);
    /* Forest at the short ends */
    if (ef > 0) {
        for (y = 0; y < c->H; y++) {
            for (x = 0; x < c->W; x++) {
                int k = IDX(c, x, y);
                double ex = c->ax - fabs(x - c->cx);
                if (c->layer[k] == L_FLOOR || c->layer[k] == L_TRACK) {
                    if (ex <= c->rimTotal + ef + 0.5) {
                        put(c, x, y, T_FOREST, L_RIM);
                    }
                }
            }
        }
    }
    /* Bounding box of the floor in the first symmetry part */
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            if (c->layer[IDX(c, x, y)] != L_FLOOR) continue;
            if (x < bx1) bx1 = x;
            if (y < by1) by1 = y;
            if (x > bx2) bx2 = x;
            if (y > by2) by2 = y;
        }
    }
    if (bx2 < 0) return;
    saveT = (uint8_t *)malloc((size_t)c->W * c->H);
    saveL = (uint8_t *)malloc((size_t)c->W * c->H);
    if (!saveT || !saveL) { free(saveT); free(saveL); return; }
    memcpy(saveT, c->t, (size_t)c->W * c->H);
    memcpy(saveL, c->layer, (size_t)c->W * c->H);
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            if (c->layer[IDX(c, x, y)] == L_FLOOR) c->layer[IDX(c, x, y)] = L_STREET;
        }
    }
    {
        Rect r;
        int hx, hy;
        r.x1 = bx1; r.y1 = by1; r.x2 = bx2; r.y2 = by2;
        /* Work on the first part only; the rest is copied by symmetry.
         * The half next to a centre line keeps a street on the line
         * when the preset has axes. */
        hx = (int)floor(c->cx);
        hy = (int)floor(c->cy);
        if (c->sym == SY_MIRROR || c->sym == SY_MIRROR4 || c->sym == SY_ROT4) {
            r.x2 = (pr->skel & SK_AXES) ? hx - sw / 2 - 1 : hx;
        }
        if (c->sym == SY_MIRROR4 || c->sym == SY_ROT4 || c->sym == SY_ROT2) {
            r.y2 = (pr->skel & SK_AXES) ? hy - sw / 2 - 1 : hy;
        }
        if (r.x2 >= r.x1 && r.y2 >= r.y1) townSplit(c, r, sw, 0);
    }
    /* Blocks only where the floor was: put back the sea and the rims,
     * and keep a street next to the rim. */
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int k = IDX(c, x, y);
            if (saveL[k] != L_FLOOR) {
                c->t[k] = saveT[k];
                c->layer[k] = saveL[k];
            } else if (c->din[k] / 3.0 <= c->rimTotal + 0.34 && c->layer[k] == L_BLOCK) {
                put(c, x, y, T_ROAD, L_STREET);
            }
        }
    }
    free(saveT);
    free(saveL);
    if (pr->skel & SK_DIAG) {
        int x0 = (int)floor(c->cx), y0 = (int)floor(c->cy);
        int ex = (int)floor(c->cx - (c->ax - c->rimTotal)), ey = (int)floor(c->cy - (c->ay - c->rimTotal));
        line8(c, x0, y0, ex, ey, T_ROAD, L_STREET, sw, 1);
    }
    paintCentre(c, pr->centre == CEN_POD ? CEN_ROUND : pr->centre);
}

/*---------------------------------------------------------
 * ISLAND family
 *---------------------------------------------------------*/

static void islandLand(Ctx *c) {
    int k = c->p.islands, x, y, i, pass;
    double bx[8], by[8], br[8];
    /* The phase in 1024ths of a turn, from one draw as before. */
    int ph = (int)((xs32(&c->rng) & 0xFFFFFF) >> 14);
    uint32_t s = c->seed * 2246822519u + 5u;
    double ns = (c->ax + c->ay) * 0.18;
    int n = c->W * c->H;
    uint8_t *tmp;
    if (ns < 3.0) ns = 3.0;
    if (k <= 1) {
        bx[0] = 0; by[0] = 0; br[0] = 0.92;
        k = 1;
    } else {
        /* sin(pi / k) scaled by 65536, for k = 2..8 */
        static const int32_t kSinPiOverK[9] = { 0, 0, 65536, 56756, 46341, 38521, 32768, 28435, 25080 };
        double dist = 0.52, r = 0.62 * ((double)kSinPiOverK[k > 8 ? 8 : k] / 65536.0) * 0.92;
        if (r > 0.46) r = 0.46;
        for (i = 0; i < k; i++) {
            /* A jitter of up to 40/1024 of a turn (about 0.25 radians)
             * either way. */
            int jit = (int)(((uint64_t)(xs32(&c->rng) & 0xFFFFFF) * 81u) >> 24) - 40;
            int a = ph + (1024 * i) / k + jit;
            double rr;
            bx[i] = (double)icos(a) / 65536.0 * dist;
            by[i] = (double)isin(a) / 65536.0 * dist;
            rr = rfloat(c);
            br[i] = r * (0.85 + rr * 0.3);
        }
    }
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int kk = c->canon[IDX(c, x, y)];
            int kx = kk % c->W, ky = kk / c->W;
            double u = uOf(c, kx), v = vOf(c, ky), best = 9.0;
            double nz = fbm(kx / ns, ky / ns, s, 4);
            for (i = 0; i < k; i++) {
                double du = u - bx[i], dv = v - by[i];
                double d = sqrt(du * du + dv * dv) / br[i];
                if (d < best) best = d;
            }
            best += (nz - 0.5) * 0.9;
            /* keep inside the box */
            if (fabs(u) > 1.0 || fabs(v) > 1.0) best = 9.0;
            put(c, x, y, best <= 1.0 ? T_GRASS : T_DEEP, best <= 1.0 ? L_FLOOR : L_SEA);
        }
    }
    /* Smooth the coast */
    tmp = (uint8_t *)malloc((size_t)n);
    if (tmp) {
        for (pass = 0; pass < 2; pass++) {
            for (y = 0; y < c->H; y++) {
                for (x = 0; x < c->W; x++) {
                    int cnt = 0, dx, dy;
                    for (dy = -1; dy <= 1; dy++) {
                        for (dx = -1; dx <= 1; dx++) {
                            int nx = x + dx, ny = y + dy;
                            if (inb(c, nx, ny) && c->t[IDX(c, nx, ny)] != T_DEEP) cnt++;
                        }
                    }
                    tmp[IDX(c, x, y)] = (uint8_t)(cnt >= 5);
                }
            }
            for (i = 0; i < n; i++) {
                c->t[i] = tmp[i] ? T_GRASS : T_DEEP;
                c->layer[i] = tmp[i] ? L_FLOOR : L_SEA;
            }
        }
        free(tmp);
    }
    removeSmallLand(c, 30);
    symmetrize(c);
}

static int landAt(const Ctx *c, int x, int y) {
    return inb(c, x, y) && isLandT(c->t[IDX(c, x, y)]);
}

/* A meandering river from the coast inward. */
static void islandRiver(Ctx *c) {
    int tries, x = 0, y = 0, i, len;
    int dir;
    for (tries = 0; tries < 200; tries++) {
        x = rnd(c, c->W);
        y = rnd(c, c->H);
        if (landAt(c, x, y) && c->din[IDX(c, x, y)] <= 4) break;
    }
    if (tries == 200) return;
    /* Head toward the centre: the nearest of the 8 directions, picked
     * by comparing the two sides against tan(22.5 degrees). */
    {
        double vx = c->cx - x, vy = c->cy - y;
        double ax = fabs(vx), ay = fabs(vy);
        if (ay <= ax * 0.41421356) dir = vx >= 0.0 ? 0 : 4;
        else if (ax <= ay * 0.41421356) dir = vy > 0.0 ? 2 : 6;
        else if (vx > 0.0) dir = vy > 0.0 ? 1 : 7;
        else dir = vy > 0.0 ? 3 : 5;
    }
    len = (int)((c->ax + c->ay) * (0.4 + rfloat(c) * 0.5));
    for (i = 0; i < len; i++) {
        static const int dx8[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int dy8[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        int r = rnd(c, 10);
        if (r == 0) dir = (dir + 1) % 8;
        else if (r == 1) dir = (dir + 7) % 8;
        x += dx8[dir];
        y += dy8[dir];
        if (!inb(c, x, y) || c->t[IDX(c, x, y)] == T_DEEP) break;
        put(c, x, y, T_RIVER, L_FLOOR);
        if (dx8[dir] && dy8[dir] && inb(c, x, y - dy8[dir]) && landAt(c, x, y - dy8[dir])) {
            put(c, x, y - dy8[dir], T_RIVER, L_FLOOR);
        }
    }
}

static void islandLake(Ctx *c, double R) {
    int tries, x0 = 0, y0 = 0, dx, dy, ring;
    uint32_t s = c->seed + (uint32_t)rnd(c, 100000);
    for (tries = 0; tries < 100; tries++) {
        x0 = rnd(c, c->W);
        y0 = rnd(c, c->H);
        if (landAt(c, x0, y0) && c->din[IDX(c, x0, y0)] / 3.0 > R + 3.0) break;
    }
    if (tries == 100) return;
    ring = rnd(c, 3);   /* 0 swamp, 1 halfbuilding, 2 none */
    for (dy = -(int)R - 3; dy <= (int)R + 3; dy++) {
        for (dx = -(int)R - 3; dx <= (int)R + 3; dx++) {
            int x = x0 + dx, y = y0 + dy;
            double d, nz;
            if (!landAt(c, x, y)) continue;
            nz = fbm(x / 3.0, y / 3.0, s, 2);
            d = sqrt((double)(dx * dx + dy * dy)) + (nz - 0.5) * 2.0;
            if (d <= R) put(c, x, y, T_RIVER, L_POD);
            else if (d <= R + 1.0 && ring == 0) put(c, x, y, T_SWAMP, L_POD);
            else if (d <= R + 1.0 && ring == 1 && rnd(c, 3)) put(c, x, y, T_HALF, L_POD);
        }
    }
}

/* Walled base area: a building ring with a gap, a base inside, a pill
 * in a corner. */
static int islandStronghold(Ctx *c) {
    int tries, x0 = 0, y0 = 0, half, gx, gy, side;
    const MakerFragment *f = NULL;
    half = rrange(c, 3, 4);
    for (tries = 0; tries < 150; tries++) {
        int ok = 1, dx, dy;
        x0 = rnd(c, c->W);
        y0 = rnd(c, c->H);
        if (!landAt(c, x0, y0) || c->din[IDX(c, x0, y0)] / 3.0 < half + 3.0) continue;
        for (dy = -half - 1; dy <= half + 1 && ok; dy++) {
            for (dx = -half - 1; dx <= half + 1 && ok; dx++) {
                int k;
                if (!inb(c, x0 + dx, y0 + dy)) { ok = 0; break; }
                k = IDX(c, x0 + dx, y0 + dy);
                if (c->layer[k] == L_WALL || c->layer[k] == L_POD || !isLandT(c->t[k])) ok = 0;
            }
        }
        if (ok) break;
    }
    if (tries == 150) return 0;
    if (c->pr->fragSet == MAKER_FRAG_SET_STRONGHOLD && c->p.detail >= 4 && rnd(c, 2)) {
        f = pickFragment(c, MAKER_FRAG_SET_STRONGHOLD, 2 * half + 3, 2 * half + 3);
    }
    if (f && stampFragment(c, f, x0, y0, 2 * half + 3, 2 * half + 3, L_WALL)) return 1;
    fillRect(c, x0 - half, y0 - half, x0 + half, y0 + half, T_BUILDING, L_WALL);
    fillRect(c, x0 - half + 1, y0 - half + 1, x0 + half - 1, y0 + half - 1, T_GRASS, L_WALL);
    side = rnd(c, 4);
    gx = x0; gy = y0;
    if (side == 0) gx = x0 + half;
    else if (side == 1) gx = x0 - half;
    else if (side == 2) gy = y0 + half;
    else gy = y0 - half;
    put(c, gx, gy, T_ROAD, L_WALL);
    if (side < 2) { put(c, gx, gy + 1, T_ROAD, L_WALL); }
    else { put(c, gx + 1, gy, T_ROAD, L_WALL); }
    addHint(c, OBJ_BASE, x0, y0);
    {
        int px = x0 + (rnd(c, 2) ? half - 1 : -half + 1);
        int py = y0 + (rnd(c, 2) ? half - 1 : -half + 1);
        addHint(c, OBJ_PILL, px, py);
    }
    return 1;
}

/* The tile at an odd offset from lo that is nearest v (maze cells
 * sit at odd offsets from the maze's corner). */
static int r1odd(int lo, int v) {
    return ((v - lo) & 1) ? v : v + 1;
}

/* A maze yard: a square maze with building walls and a base in the
 * middle, as in Black Lightning's Bitter Ends. Returns 1 if made. */
static int islandMazeYard(Ctx *c) {
    int tries, x0 = 0, y0 = 0, half = rrange(c, 5, 7);
    for (tries = 0; tries < 150; tries++) {
        int ok = 1, dx, dy;
        x0 = rnd(c, c->W);
        y0 = rnd(c, c->H);
        if (!landAt(c, x0, y0) || c->din[IDX(c, x0, y0)] / 3.0 < half + 3.0) continue;
        for (dy = -half - 1; dy <= half + 1 && ok; dy++) {
            for (dx = -half - 1; dx <= half + 1 && ok; dx++) {
                int k;
                if (!inb(c, x0 + dx, y0 + dy)) { ok = 0; break; }
                k = IDX(c, x0 + dx, y0 + dy);
                if (c->layer[k] == L_WALL || c->layer[k] == L_POD || !isLandT(c->t[k])) ok = 0;
            }
        }
        if (ok) break;
    }
    if (tries == 150) return 0;
    {
        Rect r;
        r.x1 = x0 - half; r.y1 = y0 - half; r.x2 = x0 + half; r.y2 = y0 + half;
        if (!roomMaze(c, r, rnd(c, 3) ? T_BUILDING : T_FOREST, L_WALL)) return 0;
    }
    /* The cell nearest the middle holds the base. */
    x0 = r1odd(x0 - half, x0);
    y0 = r1odd(y0 - half, y0);
    addHint(c, OBJ_BASE, x0, y0);
    return 1;
}

static void islandRoads(Ctx *c) {
    int nodes = 0, i, j, tries;
    int nxs[48], nys[48];
    double area = 0.0;
    int x, y;
    double spacing;
    int want;
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) if (landAt(c, x, y)) area += 1.0;
    }
    if (c->p.roadPct <= 0) return;
    spacing = 13.0 * sqrt(30.0 / (c->p.roadPct + 5.0));
    want = (int)(area / (spacing * spacing) + 0.5);
    if (want > 48) want = 48;
    if (want < 2) want = 2;
    for (tries = 0; tries < want * 30 && nodes < want; tries++) {
        int ok = 1;
        x = rnd(c, c->W);
        y = rnd(c, c->H);
        if (!landAt(c, x, y) || c->din[IDX(c, x, y)] < 9) continue;
        for (i = 0; i < nodes; i++) {
            double dx = x - nxs[i], dy = y - nys[i];
            if (dx * dx + dy * dy < spacing * spacing * 0.6) { ok = 0; break; }
        }
        if (!ok) continue;
        nxs[nodes] = x;
        nys[nodes] = y;
        nodes++;
    }
    if (nodes < 2) return;
    /* Prim's tree, then a few more edges for loops. */
    {
        int in[48], done = 1;
        memset(in, 0, sizeof(in));
        in[0] = 1;
        while (done < nodes) {
            int ba = -1, bb = -1;
            double bd = 1e18;
            for (i = 0; i < nodes; i++) {
                if (!in[i]) continue;
                for (j = 0; j < nodes; j++) {
                    double dx, dy, d;
                    if (in[j]) continue;
                    dx = nxs[i] - nxs[j];
                    dy = nys[i] - nys[j];
                    d = dx * dx + dy * dy;
                    if (d < bd) { bd = d; ba = i; bb = j; }
                }
            }
            if (bb < 0) break;
            in[bb] = 1;
            done++;
            {
                int mx, my;
                if (rnd(c, 2)) {
                    int ddx = nxs[bb] - nxs[ba], ddy = nys[bb] - nys[ba];
                    int m = abs(ddx) < abs(ddy) ? abs(ddx) : abs(ddy);
                    mx = nxs[ba] + (ddx > 0 ? m : -m);
                    my = nys[ba] + (ddy > 0 ? m : -m);
                } else {
                    mx = nxs[bb];
                    my = nys[ba];
                }
                line8(c, nxs[ba], nys[ba], mx, my, T_ROAD, L_STREET, 1, 1);
                line8(c, mx, my, nxs[bb], nys[bb], T_ROAD, L_STREET, 1, 1);
            }
        }
        {
            int extra = nodes * c->p.roadPct / 100;
            for (i = 0; i < extra; i++) {
                int a = rnd(c, nodes), b = rnd(c, nodes);
                double dx = nxs[a] - nxs[b], dy = nys[a] - nys[b];
                if (a == b || dx * dx + dy * dy > spacing * spacing * 3.0) continue;
                line8(c, nxs[a], nys[a], nxs[b], nys[a], T_ROAD, L_STREET, 1, 1);
                line8(c, nxs[b], nys[a], nxs[b], nys[b], T_ROAD, L_STREET, 1, 1);
            }
        }
    }
}

static void islandHouses(Ctx *c, int count) {
    int i, tries;
    for (i = 0; i < count; i++) {
        for (tries = 0; tries < 60; tries++) {
            int x = rnd(c, c->W), y = rnd(c, c->H), w, h, ok = 1, dx, dy, nearRoad = 0;
            if (!landAt(c, x, y)) continue;
            w = rrange(c, 1, 3);
            h = rrange(c, 1, 2);
            if (rnd(c, 2)) { int t = w; w = h; h = t; }
            for (dy = -1; dy <= h && ok; dy++) {
                for (dx = -1; dx <= w && ok; dx++) {
                    int k;
                    if (!inb(c, x + dx, y + dy)) { ok = 0; break; }
                    k = IDX(c, x + dx, y + dy);
                    if (c->t[k] == T_ROAD) {
                        if (dx >= 0 && dx < w && dy >= 0 && dy < h) ok = 0;
                        else nearRoad = 1;
                    } else if (c->t[k] != T_GRASS && c->t[k] != T_FOREST) ok = 0;
                    if (c->layer[k] == L_WALL) ok = 0;
                }
            }
            if (!ok || !nearRoad) continue;
            fillRect(c, x, y, x + w - 1, y + h - 1, T_BUILDING, L_POD);
            break;
        }
    }
}

static void paintIsland(Ctx *c) {
    const Preset *pr = c->pr;
    int x, y, i, n;
    double area = 0.0;
    uint32_t s = c->seed * 1103515245u + 12345u;
    double fk = ratio(c->p.forestPct, pr->forest);
    islandLand(c);
    paintRims(c, T_GRASS, NO_TERR, 0);
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) if (landAt(c, x, y)) area += 1.0;
    }
    /* Forest clumps, a mottled band of mixed forest and grass round
     * each clump, and single trees. The fractal noise mostly stays
     * between 0.3 and 0.7, so the cut-off is set inside that range. */
    {
        double f = c->p.forestPct / 100.0;
        double thr = 0.5 + (0.5 - f) * 0.35;
        double band = 0.03 + 0.07 * (f > 1.0 ? 1.0 : f);
        uint32_t trees = (uint32_t)(c->p.forestPct * 0.6 * fk > 60 ? 60 : c->p.forestPct * 0.6);
        for (y = 0; y < c->H && c->p.forestPct > 0; y++) {
            for (x = 0; x < c->W; x++) {
                int k = IDX(c, x, y);
                double nz;
                uint32_t hh;
                if (c->layer[k] != L_FLOOR) continue;
                nz = fbm(x / 7.0, y / 7.0, s, 3);
                hh = hash2(x, y, s + 1u);
                if (nz > thr) put(c, x, y, T_FOREST, -1);
                else if (nz > thr - band && (hh & 1u)) put(c, x, y, T_FOREST, -1);
                else if (((hh >> 1) & 1023u) < trees) put(c, x, y, T_FOREST, -1);
                else if (((hh >> 11) & 1023u) < 6u) put(c, x, y, ((hh >> 21) & 1u) ? T_SWAMP : T_CRATER, -1);
            }
        }
    }
    /* Swamp and craters, a few patches */
    n = (int)(area / 900.0 + 0.5);
    for (i = 0; i < n; i++) {
        int tries;
        for (tries = 0; tries < 30; tries++) {
            int x0 = rnd(c, c->W), y0 = rnd(c, c->H), dx, dy;
            int tv = rnd(c, 3) ? T_SWAMP : T_CRATER;
            if (!landAt(c, x0, y0) || c->layer[IDX(c, x0, y0)] != L_FLOOR) continue;
            for (dy = -2; dy <= 2; dy++) {
                for (dx = -2; dx <= 2; dx++) {
                    if (landAt(c, x0 + dx, y0 + dy) && rnd(c, 3) &&
                        c->layer[IDX(c, x0 + dx, y0 + dy)] == L_FLOOR) {
                        put(c, x0 + dx, y0 + dy, tv, -1);
                    }
                }
            }
            break;
        }
    }
    /* Lakes and rivers */
    n = (int)(area * c->p.waterPct / 100.0 / 600.0 + 0.5);
    for (i = 0; i < n; i++) islandLake(c, 1.5 + rfloat(c) * 3.0);
    n = (int)(area * c->p.waterPct / 100.0 / 2000.0 + 0.5);
    for (i = 0; i < n; i++) islandRiver(c);
    /* Roads, then walled base areas and houses */
    islandRoads(c);
    n = (int)(area * c->p.buildingPct / 100.0 / 1500.0 + 0.5);
    if (n > 8) n = 8;
    for (i = 0; i < n; i++) islandStronghold(c);
    if (pr->mazeYard > 0 && c->p.detail >= 4 && rnd(c, 100) < pr->mazeYard) islandMazeYard(c);
    n = (int)(area * c->p.buildingPct / 100.0 / 120.0 + 0.5);
    islandHouses(c, n);
    if (c->p.detail >= 6 && pr->fragSet == MAKER_FRAG_SET_SLUGGO_HOUSE) {
        n = (int)(area / 2500.0 * (c->p.detail - 5) / 3.0 + 0.5);
        for (i = 0; i < n; i++) {
            int tries;
            for (tries = 0; tries < 40; tries++) {
                int x0 = rnd(c, c->W), y0 = rnd(c, c->H);
                const MakerFragment *f;
                if (!landAt(c, x0, y0) || c->din[IDX(c, x0, y0)] < 18) continue;
                if (c->layer[IDX(c, x0, y0)] != L_FLOOR) continue;
                f = pickFragment(c, pr->fragSet, 9, 9);
                stampFragment(c, f, x0, y0, 9, 9, L_POD);
                break;
            }
        }
    }
}

/*---------------------------------------------------------
 * Objects
 *---------------------------------------------------------*/

static int tileFree(const Ctx *c, int x, int y) {
    return c->obj[IDX(c, x, y)] == 0;
}

static int objTerrainOk(const Ctx *c, int kind, int x, int y) {
    int t = c->t[IDX(c, x, y)];
    if (kind == OBJ_START) return t == T_DEEP;
    if (x < 1 || y < 1 || x >= c->W - 1 || y >= c->H - 1) return 0;
    switch (t) {
    case T_ROAD: case T_GRASS: case T_SWAMP: case T_CRATER: case T_RUBBLE:
        return 1;
    case T_FOREST:
        return kind == OBJ_PILL;
    default:
        return 0;
    }
}

static double nearestSame(const Ctx *c, int kind, int x, int y) {
    int i;
    double best = 1e9;
    for (i = 0; i < c->n[kind]; i++) {
        double dx = x - c->ox[kind][i], dy = y - c->oy[kind][i];
        double d = sqrt(dx * dx + dy * dy);
        if (d < best) best = d;
    }
    return best;
}

static void commitObj(Ctx *c, int kind, int x, int y) {
    c->ox[kind][c->n[kind]] = x;
    c->oy[kind][c->n[kind]] = y;
    c->n[kind]++;
    c->obj[IDX(c, x, y)] = (uint8_t)(kind + 1);
}

/* Try to place the group of images of (x,y) (and of a neighbour for
 * pairs). Returns the number placed. */
static int tryGroup(Ctx *c, int kind, int x, int y, int want, double minDist,
                    int pair, int useSym) {
    int gx[8], gy[8], g = 0, i, j;
    int xs[4], ys[4], n;
    if (useSym) n = images(c, x, y, xs, ys);
    else { xs[0] = x; ys[0] = y; n = 1; }
    for (i = 0; i < n; i++) { gx[g] = xs[i]; gy[g] = ys[i]; g++; }
    if (pair) {
        static const int pdx[4] = { 1, 0, -1, 0 }, pdy[4] = { 0, 1, 0, -1 };
        int d, found = 0;
        for (d = 0; d < 4 && !found; d++) {
            int qx = x + pdx[d], qy = y + pdy[d], m, dupe = 0;
            int qxs[4], qys[4];
            if (!inb(c, qx, qy) || !objTerrainOk(c, kind, qx, qy) || !tileFree(c, qx, qy)) continue;
            if (useSym) m = images(c, qx, qy, qxs, qys);
            else { qxs[0] = qx; qys[0] = qy; m = 1; }
            for (i = 0; i < m; i++) {
                for (j = 0; j < g; j++) if (gx[j] == qxs[i] && gy[j] == qys[i]) dupe = 1;
            }
            if (dupe) continue;
            for (i = 0; i < m; i++) { gx[g] = qxs[i]; gy[g] = qys[i]; g++; }
            found = 1;
        }
        if (!found) return 0;
    }
    if (g > want) return 0;
    for (i = 0; i < g; i++) {
        if (!inb(c, gx[i], gy[i]) || !objTerrainOk(c, kind, gx[i], gy[i]) || !tileFree(c, gx[i], gy[i])) return 0;
        if (nearestSame(c, kind, gx[i], gy[i]) < minDist) return 0;
        for (j = 0; j < i; j++) {
            double dx = gx[i] - gx[j], dy = gy[i] - gy[j];
            if (sqrt(dx * dx + dy * dy) < (pair ? 1.0 : (minDist < 2.0 ? minDist : 2.0))) return 0;
        }
    }
    for (i = 0; i < g; i++) commitObj(c, kind, gx[i], gy[i]);
    return g;
}

static void shuffle(Ctx *c, int *a, int n) {
    int i;
    for (i = n - 1; i > 0; i--) {
        int j = rnd(c, i + 1), t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

/* Place `want` objects of a kind from the candidate tiles. */
static void placeKind(Ctx *c, int kind, int *cand, int ncand, int want,
                      double minDist, int pair) {
    double md = minDist;
    int i, goal = c->n[kind] + want;
    if (goal > MAKER_MAX_OBJECTS) goal = MAKER_MAX_OBJECTS;
    /* Hints first: places a fragment or a walled base area asked for. */
    for (i = 0; i < c->nh[kind] && c->n[kind] < goal; i++) {
        int hx = c->hx[kind][i], hy = c->hy[kind][i];
        if (!inb(c, hx, hy)) continue;
        tryGroup(c, kind, hx, hy, goal - c->n[kind], 1.0, 0, 0);
    }
    shuffle(c, cand, ncand);
    while (c->n[kind] < goal && md >= 1.0) {
        for (i = 0; i < ncand && c->n[kind] < goal; i++) {
            int x = cand[i] % c->W, y = cand[i] / c->W;
            if (!inFund(c, x, y)) continue;
            tryGroup(c, kind, x, y, goal - c->n[kind], md, pair, 1);
        }
        md *= 0.75;
    }
    /* What is left: one at a time, closest to the centre first. */
    if (c->n[kind] < goal) {
        int j;
        for (i = 1; i < ncand; i++) {
            int v = cand[i];
            double dv = (v % c->W - c->cx) * (v % c->W - c->cx) + (v / c->W - c->cy) * (v / c->W - c->cy);
            for (j = i - 1; j >= 0; j--) {
                int u = cand[j];
                double du = (u % c->W - c->cx) * (u % c->W - c->cx) + (u / c->W - c->cy) * (u / c->W - c->cy);
                if (du <= dv) break;
                cand[j + 1] = u;
            }
            cand[j + 1] = v;
        }
        for (i = 0; i < ncand && c->n[kind] < goal; i++) {
            tryGroup(c, kind, cand[i] % c->W, cand[i] / c->W, 1, 1.0, 0, 0);
        }
    }
}

static int collect(Ctx *c, int kind, int *cand, int filter) {
    int x, y, n = 0;
    for (y = 0; y < c->H; y++) {
        for (x = 0; x < c->W; x++) {
            int k = IDX(c, x, y), L = c->layer[k], ok = 0;
            if (!objTerrainOk(c, kind, x, y) || !tileFree(c, x, y)) continue;
            switch (filter) {
            case 0: ok = 1; break;                                      /* anywhere */
            case 1: ok = (L == L_TRACK); break;                         /* track */
            case 2: ok = (L == L_FLOOR || L == L_SKEL || L == L_POD || L == L_BLOCK ||
                          L == L_STREET || L == L_CENTRE || L == L_WALL); break;   /* inside */
            case 3: {                                                    /* edge of floor */
                double d = c->din[k] / 3.0;
                ok = (L == L_FLOOR || L == L_SKEL) && d <= c->rimTotal + 2.4;
                break;
            }
            case 4: {                                                    /* pods */
                int i;
                for (i = 0; i < c->npod && !ok; i++) {
                    int dx = abs(x - c->podx[i]), dy = abs(y - c->pody[i]);
                    int r = c->podr[i];
                    if (dx <= r + 1 && dy <= r + 1 && (dx >= r - 1 || dy >= r - 1)) ok = 1;
                }
                break;
            }
            case 5: {                                                    /* near a base */
                int i;
                for (i = 0; i < c->n[OBJ_BASE] && !ok; i++) {
                    int dx = abs(x - c->ox[OBJ_BASE][i]), dy = abs(y - c->oy[OBJ_BASE][i]);
                    int d = dx > dy ? dx : dy;
                    if (d >= 1 && d <= 3) ok = 1;
                }
                break;
            }
            case 6: {                                                    /* junctions */
                int hz = 0, vt = 0;
                if (L != L_SKEL && L != L_STREET) break;
                if (x > 0 && x < c->W - 1 && c->t[k - 1] == c->t[k] && c->t[k + 1] == c->t[k]) hz = 1;
                if (y > 0 && y < c->H - 1 && c->t[k - c->W] == c->t[k] && c->t[k + c->W] == c->t[k]) vt = 1;
                ok = hz && vt;
                break;
            }
            case 7: {                                                    /* sea ring */
                int d = c->dout[k];
                ok = (L == L_SEA) && d >= 6 && d <= 10;
                break;
            }
            case 8: ok = (L == L_CENTRE) && c->dout[k] >= 3; break;   /* centre pond */
            default: break;
            }
            if (ok) cand[n++] = k;
        }
    }
    return n;
}

static void placeObjects(Ctx *c) {
    const Preset *pr = c->pr;
    int n = c->W * c->H, nc, i;
    int *cand = (int *)malloc((size_t)n * sizeof(int));
    double landArea = 0.0, md;
    if (!cand) return;
    for (i = 0; i < n; i++) if (isLandT(c->t[i])) landArea += 1.0;

    /* Starts first so the pond and sea rings are free. */
    if (c->p.starts > 0) {
        int mode = pr->startMode;
        if (mode == SM_POND) {
            nc = collect(c, OBJ_START, cand, 8);
            if (nc >= c->p.starts) {
                placeKind(c, OBJ_START, cand, nc, c->p.starts, sqrt((double)nc) * 0.9, 0);
            }
        }
        nc = collect(c, OBJ_START, cand, 7);
        if (mode == SM_CORNERS || mode == SM_SIDES || mode == SM_AXES) {
            int m = 0;
            for (i = 0; i < nc; i++) {
                int x = cand[i] % c->W, y = cand[i] / c->W;
                double u = fabs(uOf(c, x)), v = fabs(vOf(c, y)), ok = 0;
                if (mode == SM_CORNERS) ok = u > 0.45 && v > 0.45;
                else if (mode == SM_SIDES) ok = v >= u;
                else ok = (u < 0.3 || v < 0.3);
                if (ok) cand[m++] = cand[i];
            }
            if (m >= c->p.starts - c->n[OBJ_START]) {
                placeKind(c, OBJ_START, cand, m, c->p.starts - c->n[OBJ_START],
                          mode == SM_SIDES ? (double)m / 1.6 / (c->p.starts + 1) : 3.0, 0);
            }
            nc = collect(c, OBJ_START, cand, 7);
        }
        md = (double)nc / 1.8 / (c->p.starts > 0 ? c->p.starts : 1) * 0.85;
        if (md < 2.0) md = 2.0;
        placeKind(c, OBJ_START, cand, nc, c->p.starts - c->n[OBJ_START], md, 0);
        if (c->n[OBJ_START] < c->p.starts) {
            nc = collect(c, OBJ_START, cand, 0);
            placeKind(c, OBJ_START, cand, nc, c->p.starts - c->n[OBJ_START], 2.0, 0);
        }
    }

    /* Bases */
    if (c->p.bases > 0) {
        int filter = 2;
        md = sqrt(landArea / (c->p.bases + 1)) * 0.75;
        switch (pr->baseMode) {
        case BM_TRACK: filter = 1; break;
        case BM_CELLS: filter = 4; break;
        case BM_EDGE: filter = 3; break;
        default: filter = 2; break;
        }
        nc = collect(c, OBJ_BASE, cand, filter);
        if (filter == 1) {
            md = (double)nc / (c->p.bases > 0 ? c->p.bases : 1) * 0.8;
        }
        placeKind(c, OBJ_BASE, cand, nc, c->p.bases, md, 0);
        if (c->n[OBJ_BASE] < c->p.bases) {
            nc = collect(c, OBJ_BASE, cand, 2);
            placeKind(c, OBJ_BASE, cand, nc, c->p.bases - c->n[OBJ_BASE], 2.0, 0);
        }
        if (c->n[OBJ_BASE] < c->p.bases) {
            nc = collect(c, OBJ_BASE, cand, 0);
            placeKind(c, OBJ_BASE, cand, nc, c->p.bases - c->n[OBJ_BASE], 1.0, 0);
        }
    }

    /* Pills */
    if (c->p.pills > 0) {
        int pair = pr->pillMode == PM_PAIRS;
        int filter = 2;
        md = sqrt(landArea / (c->p.pills + 1)) * (pair ? 0.9 : 0.6);
        switch (pr->pillMode) {
        case PM_NEAR_BASE: filter = c->n[OBJ_BASE] ? 5 : 2; md *= 0.6; break;
        case PM_PAIRS: filter = c->n[OBJ_BASE] ? 5 : 2; break;
        case PM_JUNCTION: filter = 6; break;
        case PM_TRACK: filter = 1; break;
        default: filter = 2; break;
        }
        nc = collect(c, OBJ_PILL, cand, filter);
        if (pair && c->p.pills - c->n[OBJ_PILL] >= 2) {
            placeKind(c, OBJ_PILL, cand, nc, (c->p.pills - c->n[OBJ_PILL]) & ~1, md, 1);
        }
        placeKind(c, OBJ_PILL, cand, nc, c->p.pills - c->n[OBJ_PILL], md, 0);
        if (c->n[OBJ_PILL] < c->p.pills) {
            nc = collect(c, OBJ_PILL, cand, 2);
            placeKind(c, OBJ_PILL, cand, nc, c->p.pills - c->n[OBJ_PILL], md * 0.6, 0);
        }
        if (c->n[OBJ_PILL] < c->p.pills) {
            nc = collect(c, OBJ_PILL, cand, 0);
            placeKind(c, OBJ_PILL, cand, nc, c->p.pills - c->n[OBJ_PILL], 1.0, 0);
        }
    }
    free(cand);
}

/*---------------------------------------------------------
 * Reachability
 *---------------------------------------------------------*/

static int blocks(int t) {
    return t == T_BUILDING || t == T_HALF;
}

/* Make every object reachable from the first start (or the first
 * object when there are no starts). Walls on the way are cut to road,
 * with the same cut made in every symmetry image. */
static void fixReach(Ctx *c) {
    int n = c->W * c->H, i, kind, guard;
    uint8_t *seen = (uint8_t *)malloc((size_t)n);
    int *q = (int *)malloc((size_t)n * sizeof(int));
    int *dist = (int *)malloc((size_t)n * sizeof(int));
    int *from = (int *)malloc((size_t)n * sizeof(int));
    int sx, sy;
    if (!seen || !q || !dist || !from) goto out;
    if (c->n[OBJ_START]) { sx = c->ox[OBJ_START][0]; sy = c->oy[OBJ_START][0]; }
    else if (c->n[OBJ_BASE]) { sx = c->ox[OBJ_BASE][0]; sy = c->oy[OBJ_BASE][0]; }
    else if (c->n[OBJ_PILL]) { sx = c->ox[OBJ_PILL][0]; sy = c->oy[OBJ_PILL][0]; }
    else goto out;
    for (guard = 0; guard < 3 * MAKER_MAX_OBJECTS + 2; guard++) {
        int head = 0, tail = 0, tx = -1, ty = -1;
        memset(seen, 0, (size_t)n);
        q[tail++] = IDX(c, sx, sy);
        seen[IDX(c, sx, sy)] = 1;
        while (head < tail) {
            int k = q[head++], x = k % c->W, y = k / c->W, d;
            static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
            for (d = 0; d < 4; d++) {
                int nx = x + dx[d], ny = y + dy[d], nk;
                if (!inb(c, nx, ny)) continue;
                nk = IDX(c, nx, ny);
                if (seen[nk] || blocks(c->t[nk])) continue;
                seen[nk] = 1;
                q[tail++] = nk;
            }
        }
        for (kind = 0; kind < 3 && tx < 0; kind++) {
            for (i = 0; i < c->n[kind]; i++) {
                if (!seen[IDX(c, c->ox[kind][i], c->oy[kind][i])]) {
                    tx = c->ox[kind][i];
                    ty = c->oy[kind][i];
                    break;
                }
            }
        }
        if (tx < 0) break;
        /* 0-1 search from the object to any reached tile; walls cost 1. */
        {
            int *dq = q, front, back, k, found = -1;
            for (k = 0; k < n; k++) { dist[k] = DBIG; from[k] = -1; }
            front = n / 2;
            back = n / 2;
            dist[IDX(c, tx, ty)] = 0;
            dq[back++] = IDX(c, tx, ty);
            while (front < back && found < 0) {
                int x, y, d;
                static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
                k = dq[front++];
                if (seen[k]) { found = k; break; }
                x = k % c->W;
                y = k / c->W;
                for (d = 0; d < 4; d++) {
                    int nx = x + dx[d], ny = y + dy[d], nk, w;
                    if (!inb(c, nx, ny)) continue;
                    nk = IDX(c, nx, ny);
                    w = blocks(c->t[nk]) ? 1 : 0;
                    if (dist[k] + w < dist[nk]) {
                        dist[nk] = dist[k] + w;
                        from[nk] = k;
                        if (w == 0 && front > 0) dq[--front] = nk;
                        else if (back < n) dq[back++] = nk;
                    }
                }
            }
            if (found < 0) break;
            for (k = found; k >= 0; k = from[k]) {
                if (blocks(c->t[k])) sput(c, k % c->W, k / c->W, T_ROAD, -1);
            }
        }
    }
out:
    free(seen);
    free(q);
    free(dist);
    free(from);
}

/*---------------------------------------------------------
 * Mines
 *---------------------------------------------------------*/

static void placeMines(Ctx *c) {
    int i, n = c->W * c->H;
    uint32_t s = c->seed * 3266489917u + 7u;
    int pct = clampi(c->p.mineDensityPct, 0, 5);
    if (pct <= 0) return;
    for (i = 0; i < n; i++) {
        int t = c->t[i];
        if (t < T_SWAMP || t > T_GRASS) continue;
        if (c->obj[i]) continue;
        {
            int k = c->canon[i];
            if ((hash2(k % c->W, k / c->W, s) % 1000u) < (uint32_t)(pct * 10)) c->mine[i] = 1;
        }
    }
}

/*---------------------------------------------------------
 * Entry point
 *---------------------------------------------------------*/

static void sanitize(MakerParams *p) {
    p->preset = clampi(p->preset, 0, PRESET_COUNT - 1);
    p->outline = clampi(p->outline, 0, OL_COUNT - 1);
    p->symmetry = clampi(p->symmetry, 0, SY_COUNT - 1);
    p->waterPct = clampi(p->waterPct, 0, 100);
    p->forestPct = clampi(p->forestPct, 0, 100);
    p->roadPct = clampi(p->roadPct, 0, 100);
    p->buildingPct = clampi(p->buildingPct, 0, 100);
    p->islands = clampi(p->islands, 1, 8);
    p->mineDensityPct = clampi(p->mineDensityPct, 0, 5);
    p->detail = clampi(p->detail, 0, 10);
    p->bases = clampi(p->bases, 0, MAKER_MAX_OBJECTS);
    p->pills = clampi(p->pills, 0, MAKER_MAX_OBJECTS);
    p->starts = clampi(p->starts, 0, MAKER_MAX_OBJECTS);
}

void makerGenerate(uint8_t grid[256][256], MakerObjects *out, const MakerParams *pin) {
    Ctx c;
    int n, i, x, y, aspect;
    double availX, availY, margin;
    memset(&c, 0, sizeof(c));
    memset(out, 0, sizeof(*out));
    c.p = *pin;
    sanitize(&c.p);
    if (c.p.x1 < 0) c.p.x1 = 0;
    if (c.p.y1 < 0) c.p.y1 = 0;
    if (c.p.x2 > 255) c.p.x2 = 255;
    if (c.p.y2 > 255) c.p.y2 = 255;
    c.W = c.p.x2 - c.p.x1 + 1;
    c.H = c.p.y2 - c.p.y1 + 1;
    if (c.W < 4 || c.H < 4) return;
    c.pr = &kPresets[c.p.preset];
    c.seed = pin->seed ? pin->seed : 1u;
    c.rng = c.seed ^ 0xA5A5F00Du;
    if (!c.rng) c.rng = 1;
    for (i = 0; i < 4; i++) xs32(&c.rng);
    c.sym = c.p.symmetry;
    if (c.sym == SY_ROT4 && c.W != c.H) c.sym = SY_MIRROR4;
    n = c.W * c.H;
    c.t = (uint8_t *)malloc((size_t)n);
    c.layer = (uint8_t *)calloc((size_t)n, 1);
    c.mine = (uint8_t *)calloc((size_t)n, 1);
    c.obj = (uint8_t *)calloc((size_t)n, 1);
    c.din = (int *)malloc((size_t)n * sizeof(int));
    c.dout = (int *)malloc((size_t)n * sizeof(int));
    c.canon = (int *)malloc((size_t)n * sizeof(int));
    if (!c.t || !c.layer || !c.mine || !c.obj || !c.din || !c.dout || !c.canon) goto done;
    memset(c.t, T_DEEP, (size_t)n);
    buildCanon(&c);

    /* Land half extents: leave room in the sea for the starts. */
    c.cx = (c.W - 1) / 2.0;
    c.cy = (c.H - 1) / 2.0;
    margin = (c.W < 40 || c.H < 40) ? 3.0 : 4.0;
    availX = c.cx - margin;
    availY = c.cy - margin;
    if (availX < 2.0) availX = 2.0;
    if (availY < 2.0) availY = 2.0;
    aspect = c.pr->aspect;
    if (c.pr->aspectVar > 0) {
        aspect = (int)(aspect * (1.0 + (rfloat(&c) - 0.5) * 2.0 * c.pr->aspectVar / 100.0));
    }
    if (c.sym == SY_ROT4) aspect = 100;
    c.cut = (c.pr->cut > 0 ? c.pr->cut : 145) / 100.0;
    if (c.pr->cutVar > 0) c.cut += (rfloat(&c) - 0.5) * 2.0 * c.pr->cutVar / 100.0;
    c.skel = c.pr->skel;
    if (c.pr->skelAlt) {
        int opts[6], no = 0;
        for (i = 0; i < 6; i++) if (c.pr->skelAlt & (1 << i)) opts[no++] = kSkelOpts[i];
        c.skel = (c.pr->skel & ~(SK_AXES | SK_DIAG | SK_RING)) | opts[rnd(&c, no)];
    }
    if (c.pr->rimDrop > 0) c.noForestRim = rnd(&c, 100) < c.pr->rimDrop;
    c.coreT = T_BUILDING;
    if (c.pr->podCore == 2 || (c.pr->podCore == 1 && rnd(&c, 2))) c.coreT = T_HALF;
    if (c.p.outline == OL_VARIED) {
        int opts[OL_COUNT], no = 0;
        for (i = 0; i < OL_VARIED; i++) if (c.pr->outlineSet & (1 << i)) opts[no++] = i;
        if (no == 0) opts[no++] = (c.pr->outline == OL_VARIED) ? OL_OCTAGON : c.pr->outline;
        c.p.outline = opts[rnd(&c, no)];
    }
    c.ax = availX;
    if (c.ax > availY * aspect / 100.0) c.ax = availY * aspect / 100.0;
    c.ay = c.ax * 100.0 / aspect;

    switch (c.pr->family) {
    case FAM_TOWN: paintTown(&c); break;
    case FAM_ISLAND: paintIsland(&c); break;
    default: paintFormal(&c); break;
    }
    symmetrize(&c);
    computeDistances(&c);
    placeObjects(&c);
    fixReach(&c);
    placeMines(&c);

    for (y = 0; y < c.H; y++) {
        for (x = 0; x < c.W; x++) {
            int k = IDX(&c, x, y), t = c.t[k];
            if (c.mine[k] && t >= T_SWAMP && t <= T_GRASS) t += T_MINE_ADD;
            grid[c.p.x1 + x][c.p.y1 + y] = (uint8_t)t;
        }
    }
    out->numStarts = c.n[OBJ_START];
    for (i = 0; i < c.n[OBJ_START]; i++) {
        out->startX[i] = (uint8_t)(c.p.x1 + c.ox[OBJ_START][i]);
        out->startY[i] = (uint8_t)(c.p.y1 + c.oy[OBJ_START][i]);
    }
    out->numBases = c.n[OBJ_BASE];
    for (i = 0; i < c.n[OBJ_BASE]; i++) {
        out->baseX[i] = (uint8_t)(c.p.x1 + c.ox[OBJ_BASE][i]);
        out->baseY[i] = (uint8_t)(c.p.y1 + c.oy[OBJ_BASE][i]);
    }
    out->numPills = c.n[OBJ_PILL];
    for (i = 0; i < c.n[OBJ_PILL]; i++) {
        out->pillX[i] = (uint8_t)(c.p.x1 + c.ox[OBJ_PILL][i]);
        out->pillY[i] = (uint8_t)(c.p.y1 + c.oy[OBJ_PILL][i]);
    }
done:
    free(c.t);
    free(c.layer);
    free(c.mine);
    free(c.obj);
    free(c.din);
    free(c.dout);
    free(c.canon);
}
