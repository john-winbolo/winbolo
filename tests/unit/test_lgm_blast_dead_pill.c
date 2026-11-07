/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A blast on a pillbox's square against a builder (lgmBlastCatches, lgm.c).
 *
 * A blast on a solid square (a wall, a live pillbox or a base) kills a
 * builder only if he is on that same square, at any distance. A blast on
 * open ground kills him within half a square (MAP_SQUARE_MIDDLE, 128 world
 * units). Shells fly over a dead pill, so a dead pill's square is open
 * ground for this test.
 *
 * lgm_blast_dead_pill_edge       - a dead pill. The blast is 6 world units
 *                                  inside the east edge of its square and
 *                                  the builder is 4 world units across that
 *                                  edge, 10 away: he dies. Before the fix
 *                                  the square counted as solid and he lived.
 * lgm_blast_dead_pill_far_corner - a dead pill. The same blast, the builder
 *                                  in the far (north-west) corner of the
 *                                  pill's own square, more than 128 away: he
 *                                  lives. Before the fix he died. A blast on
 *                                  the square's middle (the place the shot
 *                                  that kills a pill explodes) also leaves
 *                                  him alive in that corner and kills him 16
 *                                  from the middle.
 * lgm_blast_live_pill_unchanged  - a live pill, the same places: the builder
 *                                  across the edge lives and the builder in
 *                                  the far corner dies, as before.
 *
 * Calls lgmDeathCheckAtPosition directly on ut_make_running_sim (slot 0)
 * with the builder out of his tank, so no tick runs. Pill 0 is moved onto a
 * free square whose east neighbour is free too, and both are made grass.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "game_sim.h"              /* GameSim: mp/bs/pb, tanks[], lgmen[] */
#include "lgm.h"                   /* lgmDeathCheckAtPosition, struct lgmObj */
#include "bolo_map.h"              /* mapSetPos and the terrain constants */
#include "pillbox.h"               /* pillsExistPos, pillsIsPillHit */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "test_harness.h"

#define LBD_SLOT 0
#define LBD_PILL 0

typedef struct {
    ServerSim *sim;
    GameSim   *gs;
    BYTE  px, py;      /* the pill's square */
    WORLD left, top;   /* its north-west corner in world units */
} LbdFix;

static bool lbd_square_free(GameSim *gs, int x, int y) {
    return !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) &&
           !pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) &&
           !minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y);
}

/* A free square with a free square east of it. */
static bool lbd_find(GameSim *gs, BYTE *ox, BYTE *oy) {
    int x, y;
    for (y = 40; y <= 200; y++) {
        for (x = 40; x <= 200; x++) {
            if (lbd_square_free(gs, x, y) && lbd_square_free(gs, x + 1, y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* A running game with pill 0 on a grass square at the given armour. */
static int lbd_open(LbdFix *f, BYTE armour) {
    pillboxes pb;

    memset(f, 0, sizeof(*f));
    f->sim = ut_make_running_sim("Builder");
    UT_ASSERT_MSG(f->sim != NULL, "ut_make_running_sim returned NULL");
    f->gs = &f->sim->sim;
    UT_ASSERT_MSG(f->gs->tanks[LBD_SLOT] != NULL && f->gs->lgmen[LBD_SLOT] != NULL,
                  "slot %d joined without a tank or a man", LBD_SLOT);
    pb = f->gs->pb;
    UT_ASSERT_MSG(pb->numPills > LBD_PILL, "the map has no pill %d", LBD_PILL);
    UT_ASSERT_MSG(lbd_find(f->gs, &f->px, &f->py),
                  "no pair of squares free of bases, pillboxes and mines");

    mapSetPos(f->gs, &f->gs->mp, f->px, f->py, GRASS, FALSE, FALSE);
    mapSetPos(f->gs, &f->gs->mp, (BYTE)(f->px + 1), f->py, GRASS, FALSE, FALSE);
    pb->item[LBD_PILL].x = f->px;
    pb->item[LBD_PILL].y = f->py;
    pb->item[LBD_PILL].inTank = FALSE;
    pb->item[LBD_PILL].armour = armour;
    pb->active[LBD_PILL] = 1;
    UT_ASSERT_MSG(pillsExistPos(&f->gs->pb, f->px, f->py) == TRUE,
                  "pill %d is not on square (%u,%u)", LBD_PILL,
                  (unsigned)f->px, (unsigned)f->py);
    UT_ASSERT_MSG(pillsIsPillHit(&f->gs->pb, f->px, f->py) == (armour > 0 ? TRUE : FALSE),
                  "pill %d at armour %u: pillsIsPillHit gave the wrong answer",
                  LBD_PILL, (unsigned)armour);

    f->left = (WORLD)((WORLD)f->px << TANK_SHIFT_MAPSIZE);
    f->top  = (WORLD)((WORLD)f->py << TANK_SHIFT_MAPSIZE);
    return 0;
}

static void lbd_close(LbdFix *f) {
    if (f->sim != NULL) {
        serverSimDestroy(f->sim);
        f->sim = NULL;
    }
}

/* Put the builder out of his tank at (lx, ly), set off a shell blast at
 * (wx, wy) and report whether it killed him. */
static bool lbd_blast_kills(LbdFix *f, WORLD lx, WORLD ly, WORLD wx, WORLD wy) {
    GameSim *gs = f->gs;
    lgm *man = &gs->lgmen[LBD_SLOT];

    (*man)->isDead = FALSE;
    (*man)->inTank = FALSE;
    (*man)->state = LGM_STATE_IDLE;
    (*man)->x = lx;
    (*man)->y = ly;
    lgmDeathCheckAtPosition(gs, man, lx, ly, wx, wy, NEUTRAL, NEUTRAL,
                            DMG_SRC_SHELL, DMG_NO_PILL, &gs->tanks[LBD_SLOT]);
    return (*man)->isDead == TRUE;
}

/* The blast 6 inside the east edge of the pill's square, on its middle row. */
#define LBD_BLAST_X(f) ((WORLD)((f)->left + 250))
#define LBD_MID_Y(f)   ((WORLD)((f)->top + MAP_SQUARE_MIDDLE))
/* The builder 4 across the east edge: 10 from the blast. */
#define LBD_ACROSS_X(f) ((WORLD)((f)->left + 260))
/* The builder in the north-west corner of the pill's square: about 278 from
 * the blast near the east edge, about 178 from the middle. */
#define LBD_CORNER_X(f) ((WORLD)((f)->left + 2))
#define LBD_CORNER_Y(f) ((WORLD)((f)->top + 2))

int run_lgm_blast_dead_pill_edge(void) {
    LbdFix f;
    bool died;

    if (lbd_open(&f, 0) != 0) {
        lbd_close(&f);
        return 1;
    }
    died = lbd_blast_kills(&f, LBD_ACROSS_X(&f), LBD_MID_Y(&f),
                           LBD_BLAST_X(&f), LBD_MID_Y(&f));
    lbd_close(&f);
    UT_ASSERT_MSG(died,
                  "a blast 6 inside the edge of a dead pill's square left a "
                  "builder 10 away, across the edge, alive");
    return 0;
}

int run_lgm_blast_dead_pill_far_corner(void) {
    LbdFix f;
    bool cornerEdge, cornerMid, nearMid;
    WORLD midX;

    if (lbd_open(&f, 0) != 0) {
        lbd_close(&f);
        return 1;
    }
    midX = (WORLD)(f.left + MAP_SQUARE_MIDDLE);
    cornerEdge = lbd_blast_kills(&f, LBD_CORNER_X(&f), LBD_CORNER_Y(&f),
                                 LBD_BLAST_X(&f), LBD_MID_Y(&f));
    cornerMid = lbd_blast_kills(&f, LBD_CORNER_X(&f), LBD_CORNER_Y(&f),
                                midX, LBD_MID_Y(&f));
    nearMid = lbd_blast_kills(&f, (WORLD)(midX + 16), LBD_MID_Y(&f),
                              midX, LBD_MID_Y(&f));
    lbd_close(&f);
    UT_ASSERT_MSG(!cornerEdge,
                  "a blast near the east edge of a dead pill's square killed "
                  "a builder in its far corner, more than 128 away");
    UT_ASSERT_MSG(!cornerMid,
                  "a blast on the middle of a dead pill's square killed a "
                  "builder in its corner, more than 128 away");
    UT_ASSERT_MSG(nearMid,
                  "a blast on the middle of a dead pill's square left a "
                  "builder 16 away alive");
    return 0;
}

int run_lgm_blast_live_pill_unchanged(void) {
    LbdFix f;
    bool across, corner;

    if (lbd_open(&f, 15) != 0) {
        lbd_close(&f);
        return 1;
    }
    across = lbd_blast_kills(&f, LBD_ACROSS_X(&f), LBD_MID_Y(&f),
                             LBD_BLAST_X(&f), LBD_MID_Y(&f));
    corner = lbd_blast_kills(&f, LBD_CORNER_X(&f), LBD_CORNER_Y(&f),
                             LBD_BLAST_X(&f), LBD_MID_Y(&f));
    lbd_close(&f);
    UT_ASSERT_MSG(!across,
                  "a blast on a live pill's square killed a builder on the "
                  "next square");
    UT_ASSERT_MSG(corner,
                  "a blast on a live pill's square left a builder on that "
                  "square alive");
    return 0;
}
