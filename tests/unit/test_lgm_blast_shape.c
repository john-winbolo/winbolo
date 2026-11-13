/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The open-ground area round an explosion that kills a builder
 * (lgm_blast_shape, lgmBlastOpenGroundCatches, lgmBlastCatches).
 *
 *   0 "Half-square circle" — the old WinBolo rule: a circle of radius 128
 *     world units (half a square), edge included.
 *   1 "Mac Bolo box" — |dx| < 128 and |dy| < 128, as Mac Bolo's
 *     drawexplosion routine tests it (explode() in the Go port), centre to
 *     centre.
 *   2 "Mac Bolo equivalent area circle" — the default: a circle of radius
 *     144, edge included, about the area of the box.
 *
 * Each case checks one shape twice: the shape test on its own, at 127, 128,
 * 129, 143, 144 and 145 world units straight across on both axes and both
 * signs, and at 100 and 120 on each diagonal; then the same distances
 * through lgmDeathCheckAtPosition on a running sim, with the builder out on
 * grass and the explosion on grass. A last case checks that an explosion
 * on a building still kills only a builder on its own square, whatever the
 * shape. Another checks that shape 0 is the old test exactly, pill_range
 * limit included. The last checks that every shape stops at pill_range
 * when pill_range is under its reach, in the shape test and on a running
 * sim. The classic case checks that the classic table plays the circle of
 * 144 on a running sim: 140 straight across dies, 150 lives.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "game_sim.h"              /* GameSim: mp/bs/pb, tanks[], lgmen[] */
#include "bolo_map.h"              /* mapSetPos */
#include "pillbox.h"               /* pillsExistPos */
#include "bases.h"                 /* basesExistPos */
#include "lgm.h"                   /* lgmDeathCheckAtPosition, LGM_BLAST_* */
#include "sim_rules.h"             /* SimRules */
#include "util.h"                  /* utilIsItemInRange */
#include "test_harness.h"

/* One distance under test and whether the shape kills there. */
typedef struct {
    long dx, dy;
    bool dies;
} LbsPoint;

/* The points every shape is checked at, with the answer for each shape.
 * Straight across: 127, 128, 129, 143, 144, 145. Diagonal: 100 (141.4 off)
 * and 120 (169.7 off). */
typedef struct {
    long dx, dy;
    bool dies[3];   /* indexed by shape */
} LbsRow;

static const LbsRow kLbsRows[] = {
    /*  dx    dy    half   box    area */
    {  127,    0, { true,  true,  true  } },
    {  128,    0, { true,  false, true  } },
    {  129,    0, { false, false, true  } },
    {  143,    0, { false, false, true  } },
    {  144,    0, { false, false, true  } },
    {  145,    0, { false, false, false } },
    {  100,  100, { false, true,  true  } },
    {  120,  120, { false, true,  false } },
};
#define LBS_ROW_COUNT ((int) (sizeof(kLbsRows) / sizeof(kLbsRows[0])))

static const char *const kLbsShapeName[3] = {
    "Half-square circle", "Mac Bolo box", "Mac Bolo equivalent area circle",
};

/* The square the builder stands in the middle of, and the half-width of
 * the grass patch painted round it (two squares each way covers 145 and the
 * 120 diagonal). */
#define LBS_PATCH 2

/* The shape test on its own, at every row, turned to all four quarters (a
 * straight row is also tried on the other axis). */
static int lbsCheckPure(int shape) {
    int i, q;

    for (i = 0; i < LBS_ROW_COUNT; i++) {
        const LbsRow *row = &kLbsRows[i];
        for (q = 0; q < 4; q++) {
            long sx = (q & 1) ? -1 : 1;
            long sy = (q & 2) ? -1 : 1;
            long dx = row->dx * sx;
            long dy = row->dy * sy;
            bool got = lgmBlastOpenGroundCatches(shape, 0xFFFF, dx, dy);

            UT_ASSERT_MSG(got == row->dies[shape],
                          "shape %d (%s) at (%ld,%ld) %s, wanted %s", shape,
                          kLbsShapeName[shape], dx, dy,
                          got ? "kills" : "spares",
                          row->dies[shape] ? "kills" : "spares");
            if (row->dy == 0) {
                got = lgmBlastOpenGroundCatches(shape, 0xFFFF, dy, dx);
                UT_ASSERT_MSG(got == row->dies[shape],
                              "shape %d (%s) at (%ld,%ld) %s, wanted %s",
                              shape, kLbsShapeName[shape], dy, dx,
                              got ? "kills" : "spares",
                              row->dies[shape] ? "kills" : "spares");
            }
        }
    }
    return 0;
}

/* A running sim with a grass patch round a square that has no pillbox and
 * no base near it. The square is written to *cx, *cy. */
static ServerSim *lbsMakeSim(BYTE *cx, BYTE *cy) {
    ServerSim *sim = ut_make_running_sim("BlastShape");
    GameSim   *gs;
    int        x, y, dx, dy;

    if (sim == NULL) {
        return NULL;
    }
    gs = &sim->sim;
    if (gs->lgmen[0] == NULL || gs->tanks[0] == NULL) {
        fprintf(stderr, "  the round built no builder for slot 0\n");
        serverSimDestroy(sim);
        return NULL;
    }
    for (y = 30; y < 220; y++) {
        for (x = 30; x < 220; x++) {
            bool clear = true;
            for (dy = -LBS_PATCH; dy <= LBS_PATCH && clear; dy++) {
                for (dx = -LBS_PATCH; dx <= LBS_PATCH && clear; dx++) {
                    BYTE px = (BYTE) (x + dx);
                    BYTE py = (BYTE) (y + dy);
                    if (pillsExistPos(&gs->pb, px, py) == TRUE ||
                        basesExistPos(&gs->bs, px, py) == TRUE) {
                        clear = false;
                    }
                }
            }
            if (clear) {
                for (dy = -LBS_PATCH; dy <= LBS_PATCH; dy++) {
                    for (dx = -LBS_PATCH; dx <= LBS_PATCH; dx++) {
                        mapSetPos(gs, &gs->mp, (BYTE) (x + dx),
                                  (BYTE) (y + dy), GRASS, FALSE, FALSE);
                    }
                }
                *cx = (BYTE) x;
                *cy = (BYTE) y;
                return sim;
            }
        }
    }
    fprintf(stderr, "  no square on the map is clear of pillboxes and "
            "bases\n");
    serverSimDestroy(sim);
    return NULL;
}

/* Stand the builder of slot 0 out on (lx, ly), set off an explosion at
 * (wx, wy), and say whether it killed him. */
static bool lbsBlastKills(ServerSim *sim, WORLD lx, WORLD ly, WORLD wx,
                          WORLD wy) {
    GameSim *gs = &sim->sim;

    gs->lgmen[0]->inTank = FALSE;
    gs->lgmen[0]->isDead = FALSE;
    gs->lgmen[0]->x      = lx;
    gs->lgmen[0]->y      = ly;
    lgmDeathCheckAtPosition(gs, &gs->lgmen[0], lx, ly, wx, wy, NEUTRAL,
                            NEUTRAL, DMG_SRC_UNKNOWN, DMG_NO_PILL,
                            &gs->tanks[0], FALSE);
    return gs->lgmen[0]->isDead != FALSE;
}

/* Every row through the engine's own builder death test. */
static int lbsCheckSim(int shape) {
    ServerSim *sim;
    BYTE       cx = 0, cy = 0;
    WORLD      lx, ly;
    int        i, q;

    sim = lbsMakeSim(&cx, &cy);
    UT_ASSERT_MSG(sim != NULL, "could not build a running sim");
    UT_ASSERT_MSG(sim->sim.rules.lgm_blast_shape == LGM_BLAST_SHAPE_CLASSIC,
                  "a new round has lgm_blast_shape %d, not the classic %d",
                  (int) sim->sim.rules.lgm_blast_shape,
                  LGM_BLAST_SHAPE_CLASSIC);
    sim->sim.rules.lgm_blast_shape = shape;
    lx = (WORLD) (((WORLD) cx << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    ly = (WORLD) (((WORLD) cy << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    for (i = 0; i < LBS_ROW_COUNT; i++) {
        const LbsRow *row = &kLbsRows[i];
        for (q = 0; q < 4; q++) {
            long  dx = row->dx * ((q & 1) ? -1 : 1);
            long  dy = row->dy * ((q & 2) ? -1 : 1);
            bool  got = lbsBlastKills(sim, lx, ly, (WORLD) ((long) lx + dx),
                                      (WORLD) ((long) ly + dy));

            if (got != row->dies[shape]) {
                serverSimDestroy(sim);
                UT_ASSERT_MSG(false,
                              "shape %d (%s): an explosion (%ld,%ld) from "
                              "the builder %s him, wanted %s", shape,
                              kLbsShapeName[shape], dx, dy,
                              got ? "killed" : "spared",
                              row->dies[shape] ? "killed" : "spared");
            }
        }
    }
    serverSimDestroy(sim);
    return 0;
}

int run_lgm_blast_shape_half_square(void) {
    UT_ASSERT(lbsCheckPure(LGM_BLAST_SHAPE_HALF_SQUARE) == 0);
    UT_ASSERT(lbsCheckSim(LGM_BLAST_SHAPE_HALF_SQUARE) == 0);
    return 0;
}

int run_lgm_blast_shape_mac_box(void) {
    UT_ASSERT(lbsCheckPure(LGM_BLAST_SHAPE_MAC_BOX) == 0);
    UT_ASSERT(lbsCheckSim(LGM_BLAST_SHAPE_MAC_BOX) == 0);
    return 0;
}

int run_lgm_blast_shape_mac_area(void) {
    UT_ASSERT(lbsCheckPure(LGM_BLAST_SHAPE_MAC_AREA) == 0);
    UT_ASSERT(lbsCheckSim(LGM_BLAST_SHAPE_MAC_AREA) == 0);
    return 0;
}

/* An explosion on a building kills only a builder on that square, whatever
 * the shape: a builder 100 off on the next square lives, and one 100 off
 * on the same square dies. */
int run_lgm_blast_shape_solid_square(void) {
    ServerSim *sim;
    BYTE       cx = 0, cy = 0;
    WORLD      bx, by;
    int        shape;

    sim = lbsMakeSim(&cx, &cy);
    UT_ASSERT_MSG(sim != NULL, "could not build a running sim");
    mapSetPos(&sim->sim, &sim->sim.mp, cx, cy, BUILDING, FALSE, FALSE);
    /* The explosion near the building square's left edge. */
    bx = (WORLD) (((WORLD) cx << M_W_SHIFT_SIZE) + 20);
    by = (WORLD) (((WORLD) cy << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    for (shape = 0; shape < 3; shape++) {
        bool across, inside;

        sim->sim.rules.lgm_blast_shape = shape;
        /* 100 west: on the grass square next door. */
        across = lbsBlastKills(sim, (WORLD) (bx - 100), by, bx, by);
        /* 100 east: on the building square itself. */
        inside = lbsBlastKills(sim, (WORLD) (bx + 100), by, bx, by);
        if (across || !inside) {
            serverSimDestroy(sim);
            UT_ASSERT_MSG(false,
                          "shape %d (%s) on a building: next square %s, "
                          "same square %s; wanted spared and killed", shape,
                          kLbsShapeName[shape],
                          across ? "killed" : "spared",
                          inside ? "killed" : "spared");
        }
    }
    serverSimDestroy(sim);
    return 0;
}

/* Shape 0 is the test lgmBlastCatches made before the rule, exactly: the
 * distance utilIsItemInRange gives with the pill_range rule as its limit,
 * then at most MAP_SQUARE_MIDDLE. Checked at every offset up to 200 each way
 * for pill_range values under, at and over 128. A value outside the three
 * shapes plays the same. run_lgm_blast_shape_pill_range checks the other
 * two shapes against pill_range. */
int run_lgm_blast_shape_classic_matches_old(void) {
    static const int32_t kRanges[] = { 0, 1, 64, 100, 127, 128, 129, 144,
                                       2048, 65535 };
    static const int32_t kShapes[] = { LGM_BLAST_SHAPE_HALF_SQUARE, 3, -1 };
    const WORLD lx = 0x8000;
    const WORLD ly = 0x8000;
    int r, k;
    long dx, dy;

    for (r = 0; r < (int) (sizeof(kRanges) / sizeof(kRanges[0])); r++) {
        for (dy = -200; dy <= 200; dy++) {
            for (dx = -200; dx <= 200; dx++) {
                double distance;
                bool   old;

                utilIsItemInRange(lx, ly, (WORLD) ((long) lx + dx),
                                  (WORLD) ((long) ly + dy),
                                  (WORLD) kRanges[r], &distance);
                old = distance <= MAP_SQUARE_MIDDLE;
                for (k = 0; k < (int) (sizeof(kShapes) / sizeof(kShapes[0]));
                     k++) {
                    bool now = lgmBlastOpenGroundCatches(kShapes[k],
                                                         kRanges[r], dx, dy);
                    UT_ASSERT_MSG(now == old,
                                  "shape %d, pill_range %d, at (%ld,%ld): "
                                  "%s, the old test %s", (int) kShapes[k],
                                  (int) kRanges[r], dx, dy,
                                  now ? "kills" : "spares",
                                  old ? "kills" : "spares");
                }
            }
        }
    }
    /* pill_range 0 leaves the half-square circle only its centre. */
    UT_ASSERT(lgmBlastOpenGroundCatches(LGM_BLAST_SHAPE_HALF_SQUARE, 0,
                                        0, 0) == true);
    UT_ASSERT(lgmBlastOpenGroundCatches(LGM_BLAST_SHAPE_HALF_SQUARE, 0,
                                        1, 0) == false);
    return 0;
}

/* Every shape stops at pill_range when pill_range is under its reach: a
 * builder further than pill_range from the explosion is safe, and inside
 * pill_range the shape is unchanged. Checked in the shape test at every
 * offset up to 200 each way for pill_range 0, 1, 64, 100 and 127, then on a
 * running sim with pill_range 100: 100 straight across dies, 101 lives,
 * (70,70) (99.0 off) dies and (71,71) (100.4 off) lives, for each shape. */
int run_lgm_blast_shape_pill_range(void) {
    static const int32_t kRanges[] = { 0, 1, 64, 100, 127 };
    static const struct {
        long dx, dy;
        bool dies;
    } kSimPoints[] = {
        { 100, 0, true }, { 101, 0, false }, { 70, 70, true },
        { 71, 71, false },
    };
    int shape, r, i;
    long dx, dy;

    for (shape = 0; shape < 3; shape++) {
        for (r = 0; r < (int) (sizeof(kRanges) / sizeof(kRanges[0])); r++) {
            long pr = (long) kRanges[r];
            for (dy = -200; dy <= 200; dy++) {
                for (dx = -200; dx <= 200; dx++) {
                    bool area = lgmBlastOpenGroundCatches(shape, 0xFFFF,
                                                          dx, dy);
                    bool want = area && (dx * dx + dy * dy <= pr * pr);
                    bool got = lgmBlastOpenGroundCatches(shape, (int32_t) pr,
                                                         dx, dy);
                    UT_ASSERT_MSG(got == want,
                                  "shape %d (%s), pill_range %ld, at "
                                  "(%ld,%ld): %s, wanted %s", shape,
                                  kLbsShapeName[shape], pr, dx, dy,
                                  got ? "kills" : "spares",
                                  want ? "kills" : "spares");
                }
            }
            /* The edge: pill_range straight across dies, one more lives. */
            UT_ASSERT(lgmBlastOpenGroundCatches(shape, (int32_t) pr, pr,
                                                0) == true);
            UT_ASSERT(lgmBlastOpenGroundCatches(shape, (int32_t) pr, 0,
                                                -(pr + 1)) == false);
        }
        /* pill_range under 0 catches no builder. */
        UT_ASSERT(lgmBlastOpenGroundCatches(shape, -1, 0, 0) == false);
    }

    for (shape = 0; shape < 3; shape++) {
        ServerSim *sim;
        BYTE       cx = 0, cy = 0;
        WORLD      lx, ly;

        sim = lbsMakeSim(&cx, &cy);
        UT_ASSERT_MSG(sim != NULL, "could not build a running sim");
        sim->sim.rules.lgm_blast_shape = shape;
        sim->sim.rules.pill_range = 100;
        lx = (WORLD) (((WORLD) cx << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
        ly = (WORLD) (((WORLD) cy << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
        for (i = 0; i < (int) (sizeof(kSimPoints) / sizeof(kSimPoints[0]));
             i++) {
            bool got = lbsBlastKills(sim, lx, ly,
                                     (WORLD) ((long) lx + kSimPoints[i].dx),
                                     (WORLD) ((long) ly + kSimPoints[i].dy));
            if (got != kSimPoints[i].dies) {
                serverSimDestroy(sim);
                UT_ASSERT_MSG(false,
                              "shape %d (%s), pill_range 100: an explosion "
                              "(%ld,%ld) from the builder %s him, wanted %s",
                              shape, kLbsShapeName[shape], kSimPoints[i].dx,
                              kSimPoints[i].dy, got ? "killed" : "spared",
                              kSimPoints[i].dies ? "killed" : "spared");
            }
        }
        serverSimDestroy(sim);
    }
    return 0;
}

/* The classic table plays the Mac Bolo equivalent area circle, radius 144.
 * On a running sim with the round's own rules (nothing set by the test), a
 * builder on grass 140 world units straight across from an explosion on
 * grass dies and one 150 across lives, on both axes and both signs. Under
 * the old WinBolo circle of 128 the builder at 140 would live. */
int run_lgm_blast_shape_classic_144(void) {
    static const struct {
        long off;
        bool dies;
    } kPoints[] = { { 140, true }, { 150, false } };
    ServerSim *sim;
    BYTE       cx = 0, cy = 0;
    WORLD      lx, ly;
    int        i, q;

    UT_ASSERT(LGM_BLAST_SHAPE_CLASSIC == LGM_BLAST_SHAPE_MAC_AREA);
    UT_ASSERT(LGM_BLAST_MAC_AREA_RADIUS == 144);
    sim = lbsMakeSim(&cx, &cy);
    UT_ASSERT_MSG(sim != NULL, "could not build a running sim");
    if (sim->sim.rules.lgm_blast_shape != LGM_BLAST_SHAPE_MAC_AREA) {
        int got = (int) sim->sim.rules.lgm_blast_shape;
        serverSimDestroy(sim);
        UT_ASSERT_MSG(false, "a new round has lgm_blast_shape %d, not %d",
                      got, LGM_BLAST_SHAPE_MAC_AREA);
    }
    lx = (WORLD) (((WORLD) cx << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    ly = (WORLD) (((WORLD) cy << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    for (i = 0; i < (int) (sizeof(kPoints) / sizeof(kPoints[0])); i++) {
        for (q = 0; q < 4; q++) {
            long  s  = (q & 1) ? -1 : 1;
            long  dx = (q & 2) ? 0 : kPoints[i].off * s;
            long  dy = (q & 2) ? kPoints[i].off * s : 0;
            bool  got = lbsBlastKills(sim, lx, ly, (WORLD) ((long) lx + dx),
                                      (WORLD) ((long) ly + dy));

            if (got != kPoints[i].dies) {
                serverSimDestroy(sim);
                UT_ASSERT_MSG(false,
                              "classic table: an explosion (%ld,%ld) from "
                              "the builder %s him, wanted %s", dx, dy,
                              got ? "killed" : "spared",
                              kPoints[i].dies ? "killed" : "spared");
            }
        }
    }
    serverSimDestroy(sim);
    return 0;
}
