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
 *                                  the square's middle that did not hit the
 *                                  pill (a mine's or a tank's explosion
 *                                  there) also leaves him alive in that
 *                                  corner and kills him 16 from the middle.
 *                                  The shot that kills the pill is a hit and
 *                                  is tested in lgm_blast_pill_kill_shot.
 * lgm_blast_live_pill_unchanged  - a live pill, the same places: the builder
 *                                  across the edge lives and the builder in
 *                                  the far corner dies, as before.
 * lgm_blast_pill_kill_shot       - the shot that kills a pill. A real shell
 *                                  goes through shellsUpdate into pill 0 at
 *                                  armour 1 and takes it to 0. That is a hit
 *                                  on the pill's square, so the square rule
 *                                  holds although the pill is dead when the
 *                                  builder is tested: a builder in either
 *                                  far corner of the square dies, and a
 *                                  builder on the first world unit of the
 *                                  east neighbour (128 from the blast, inside
 *                                  the open-ground circle) lives. Once with
 *                                  no rewind and once with a rewind, the two
 *                                  ways shellsBlastTestBuilder tests him.
 * lgm_blast_wall_rubble_shot     - five real shells into a wall (a building
 *                                  of BUILDING_LIFE). Hit 1 makes it a half
 *                                  building, hits 2 to 4 wear it down, hit 5
 *                                  makes it rubble. Every hit is a hit on
 *                                  the wall's square, so the square rule
 *                                  holds on all five, hit 5 included,
 *                                  although the square is rubble when the
 *                                  builder is tested: a builder in either
 *                                  far corner of the square dies on each
 *                                  hit, and a builder on the first world
 *                                  unit of the square to the right or below
 *                                  (128 from the blast) lives on each hit.
 *                                  Once with no rewind and once with a
 *                                  rewind. As in Mac Bolo, where a shell's
 *                                  hit on a square explodes at that square.
 *
 * The first three call lgmDeathCheckAtPosition directly, with no pill hit,
 * on ut_make_running_sim (slot 0) with the builder out of his tank, so no
 * tick runs. Pill 0 is moved onto a free square whose east neighbour is free
 * too, and both are made grass.
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
#include "position_history.h"      /* posHistoryInit, posHistoryRecord */
#include "shells.h"                /* shellsAddItem, shellsUpdate */
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
                            DMG_SRC_SHELL, DMG_NO_PILL, &gs->tanks[LBD_SLOT],
                            FALSE);
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

/* Fire a real shell east into the square, from 20 inside its west edge,
 * with the builder out at (lx, ly). With comp above 0 the shell carries that
 * rewind and the builder's history says he was at (lx, ly) on every frame.
 * One shellsUpdate: the shell moves 32 and hits what is on the square.
 * Reports whether the builder died. */
static bool lbd_fire_east_kills(LbdFix *f, WORLD lx, WORLD ly, uint8_t comp) {
    GameSim  *gs = f->gs;
    lgm      *man = &gs->lgmen[LBD_SLOT];
    tank      tanks[MAX_TANKS];
    lgm      *lgms[MAX_TANKS];
    BYTE      n = 0;
    BYTE      c;
    shells    q;
    int       k;

    /* The tank well away from the shell's path. */
    gs->tanks[LBD_SLOT]->x = (WORLD)(f->left + MAP_SQUARE_MIDDLE);
    gs->tanks[LBD_SLOT]->y = (WORLD)(f->top - 10 * 256);
    (*man)->isDead = FALSE;
    (*man)->inTank = FALSE;
    (*man)->state = LGM_STATE_IDLE;
    (*man)->x = lx;
    (*man)->y = ly;
    posHistoryInit(&f->sim->lgmPosHistory[LBD_SLOT]);
    for (k = 0; k < POSITION_HISTORY_SIZE; k++) {
        posHistoryRecord(&f->sim->lgmPosHistory[LBD_SLOT], lx, ly, true);
    }

    shellsAddItem(gs, &gs->shs, (WORLD)(f->left + 20), LBD_MID_Y(f),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)7, NEUTRAL, NEUTRAL,
                  DMG_NO_PILL, FALSE);
    q = gs->shs;
    q->x = (WORLD)(f->left + 20);
    q->y = LBD_MID_Y(f);
    q->xAcc = 0;
    q->yAcc = 0;
    q->compensationTicks = comp;

    for (c = 0; c < MAX_TANKS; c++) {
        if (f->sim->playerConnected[c] && gs->tanks[c] != NULL) {
            tanks[n] = gs->tanks[c];
            lgms[n]  = &gs->lgmen[c];
            n++;
        }
    }
    shellsUpdate(gs, tanks, n, lgms, &gs->ss);
    return (*man)->isDead == TRUE;
}

/* The same shell into pill 0, which is at armour 1. *pillDied says whether
 * the shot took the pill's armour to 0. */
static bool lbd_kill_shot_kills(LbdFix *f, WORLD lx, WORLD ly, uint8_t comp,
                                bool *pillDied) {
    bool died;

    f->gs->pb->item[LBD_PILL].armour = 1;
    died = lbd_fire_east_kills(f, lx, ly, comp);
    *pillDied = (f->gs->pb->item[LBD_PILL].armour == 0);
    return died;
}

int run_lgm_blast_pill_kill_shot(void) {
    static const uint8_t comps[2] = { 0, 3 };
    LbdFix f;
    int    i;

    for (i = 0; i < 2; i++) {
        bool nw, se, across;
        bool pillNw, pillSe, pillAcross;

        if (lbd_open(&f, 1) != 0) {
            lbd_close(&f);
            return 1;
        }
        UT_ASSERT_MSG(f.sim->sim.lgmPosHistoryPtr != NULL,
                      "the running sim keeps no builder history");
        nw = lbd_kill_shot_kills(&f, LBD_CORNER_X(&f), LBD_CORNER_Y(&f),
                                 comps[i], &pillNw);
        se = lbd_kill_shot_kills(&f, (WORLD)(f.left + 253),
                                 (WORLD)(f.top + 253), comps[i], &pillSe);
        across = lbd_kill_shot_kills(&f, (WORLD)(f.left + 256), LBD_MID_Y(&f),
                                     comps[i], &pillAcross);
        lbd_close(&f);
        UT_ASSERT_MSG(pillNw && pillSe && pillAcross,
                      "rewind %u: the shell did not take the pill from "
                      "armour 1 to 0", (unsigned)comps[i]);
        UT_ASSERT_MSG(nw,
                      "rewind %u: the shot that killed a pill left a builder "
                      "in the north-west corner of its square alive",
                      (unsigned)comps[i]);
        UT_ASSERT_MSG(se,
                      "rewind %u: the shot that killed a pill left a builder "
                      "in the south-east corner of its square alive",
                      (unsigned)comps[i]);
        UT_ASSERT_MSG(!across,
                      "rewind %u: the shot that killed a pill killed a "
                      "builder on the next square, 128 from the blast",
                      (unsigned)comps[i]);
    }
    return 0;
}

/* The wall test's builder places: the two far corners of the wall's square,
 * and the first world unit of the square to the right and of the square
 * below, each 128 from the blast on the middle of the wall's square. */
#define LBD_WALL_SPOTS 4
#define LBD_WALL_HITS  5

int run_lgm_blast_wall_rubble_shot(void) {
    static const uint8_t comps[2] = { 0, 3 };
    static const char *names[LBD_WALL_SPOTS] = {
        "in the north-west corner of the wall's square",
        "in the south-east corner of the wall's square",
        "on the square to the right, 128 from the blast",
        "on the square below, 128 from the blast"
    };
    static const bool onSquare[LBD_WALL_SPOTS] = { true, true, false, false };
    LbdFix f;
    int    i, s, h;

    for (i = 0; i < 2; i++) {
        for (s = 0; s < LBD_WALL_SPOTS; s++) {
            WORLD lx, ly;
            BYTE  after[LBD_WALL_HITS];
            bool  died[LBD_WALL_HITS];
            bool  belowFree;

            if (lbd_open(&f, 0) != 0) {
                lbd_close(&f);
                return 1;
            }
            /* Pill 0 off the square (carried), the square below free grass
             * and the square a wall, so the wall is all the shell can hit. */
            f.gs->pb->item[LBD_PILL].inTank = TRUE;
            belowFree = lbd_square_free(f.gs, f.px, f.py + 1);
            if (!belowFree) {
                lbd_close(&f);
            }
            UT_ASSERT_MSG(belowFree, "the square below the wall is not free");
            mapSetPos(f.gs, &f.gs->mp, f.px, (BYTE)(f.py + 1), GRASS, FALSE, FALSE);
            mapSetPos(f.gs, &f.gs->mp, f.px, f.py, BUILDING, FALSE, FALSE);

            switch (s) {
                case 0:  lx = (WORLD)(f.left + 2);   ly = (WORLD)(f.top + 2);   break;
                case 1:  lx = (WORLD)(f.left + 253); ly = (WORLD)(f.top + 253); break;
                case 2:  lx = (WORLD)(f.left + 256); ly = LBD_MID_Y(&f);        break;
                default: lx = (WORLD)(f.left + MAP_SQUARE_MIDDLE);
                         ly = (WORLD)(f.top + 256);                              break;
            }
            for (h = 0; h < LBD_WALL_HITS; h++) {
                died[h] = lbd_fire_east_kills(&f, lx, ly, comps[i]);
                after[h] = mapGetPos(&f.gs->mp, f.px, f.py);
            }
            lbd_close(&f);

            for (h = 0; h < LBD_WALL_HITS; h++) {
                BYTE want = (h == LBD_WALL_HITS - 1) ? RUBBLE : HALFBUILDING;
                UT_ASSERT_MSG(after[h] == want,
                              "rewind %u: after hit %d the wall's square is "
                              "terrain %u, not %u", (unsigned)comps[i], h + 1,
                              (unsigned)after[h], (unsigned)want);
                UT_ASSERT_MSG(died[h] == onSquare[s],
                              "rewind %u: hit %d on a wall %s a builder %s",
                              (unsigned)comps[i], h + 1,
                              died[h] ? "killed" : "left alive", names[s]);
            }
        }
    }
    return 0;
}
