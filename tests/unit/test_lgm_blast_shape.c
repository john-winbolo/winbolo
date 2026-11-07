/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The open-ground area round an explosion that kills a builder
 * (lgm_blast_shape, lgmBlastOpenGroundCatches, lgmBlastCatches).
 *
 *   0 "Half-square circle" — the WinBolo rule and the default: a circle of
 *     radius 128 world units (half a square), edge included.
 *   1 "Mac Bolo box" — |dx| < 128 and |dy| < 128, as Mac Bolo's explode()
 *     tests it, centre to centre.
 *   2 "Mac Bolo equivalent area circle" — a circle of radius 144, edge
 *     included, about the area of the box.
 *
 * Each case checks one shape twice: the shape test on its own, at 127, 128,
 * 129, 143, 144 and 145 world units straight across on both axes and both
 * signs, and at 100 and 120 on each diagonal; then the same distances
 * through lgmDeathCheckAtPosition on a running sim, with the builder out on
 * grass and the explosion on grass. A last case checks that an explosion
 * on a building still kills only a builder on its own square, whatever the
 * shape.
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
                            &gs->tanks[0]);
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
    UT_ASSERT_MSG(sim->sim.rules.lgm_blast_shape == 0,
                  "a new round has lgm_blast_shape %d, not 0",
                  (int) sim->sim.rules.lgm_blast_shape);
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
    UT_ASSERT(LGM_BLAST_SHAPE_CLASSIC == LGM_BLAST_SHAPE_HALF_SQUARE);
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
