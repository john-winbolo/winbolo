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
 * lgm_blast_shell_runs_out_dead_pill - a real shell flies over a dead pill
 *                                  and runs out 24 inside the east edge of
 *                                  its square (the end-of-life path, no
 *                                  hit). Open ground: a builder 28 away
 *                                  across the edge dies and a builder in
 *                                  the far (north-west) corner of the
 *                                  pill's square lives.
 * lgm_blast_shell_hits_tank_dead_pill - a real shell hits a tank that
 *                                  stands on a dead pill's square. The
 *                                  blast is at the shell's own place and is
 *                                  not a hit on the square: a builder 78
 *                                  away across the east edge dies and a
 *                                  builder in the far (north-west) corner
 *                                  lives.
 *
 * The first three call lgmDeathCheckAtPosition directly, with no pill hit,
 * on ut_make_running_sim (slot 0) with the builder out of his tank, so no
 * tick runs. Pill 0 is moved onto a free square whose east and south
 * neighbours are free too, and all three are made grass.
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

/* A free square with a free square east of it and a free square south of
 * it, so a builder can stand on either neighbour. The shell tests fire east
 * into the square. */
static bool lbd_find(GameSim *gs, BYTE *ox, BYTE *oy) {
    int x, y;
    for (y = 40; y <= 200; y++) {
        for (x = 40; x <= 200; x++) {
            if (lbd_square_free(gs, x, y) && lbd_square_free(gs, x + 1, y) &&
                lbd_square_free(gs, x, y + 1)) {
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
                  "no square with its east and south neighbours free of "
                  "bases, pillboxes and mines");

    mapSetPos(f->gs, &f->gs->mp, f->px, f->py, GRASS, FALSE, FALSE);
    mapSetPos(f->gs, &f->gs->mp, (BYTE)(f->px + 1), f->py, GRASS, FALSE, FALSE);
    mapSetPos(f->gs, &f->gs->mp, f->px, (BYTE)(f->py + 1), GRASS, FALSE, FALSE);
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

/* Fire a real shell east along the square's middle row from startX, with
 * the builder out at (lx, ly) and the tank at (tankX, tankY). boat: the
 * shell is a boat's (onBoat), which grass, swamp, rubble and road stop too.
 * With comp above 0 the shell carries that rewind, and the builder's and
 * the tank's histories say they were where they are on every frame.
 * length: the shell's life in ticks; updates: how many shellsUpdate calls
 * run. Each call moves the shell 32 and tests what it hits; a shell whose
 * life is over explodes where it is.
 * Reports whether the builder died. */
static bool lbd_fire_east_run(LbdFix *f, WORLD startX, WORLD lx, WORLD ly,
                              uint8_t comp, bool boat, WORLD tankX,
                              WORLD tankY, BYTE length, int updates) {
    GameSim  *gs = f->gs;
    lgm      *man = &gs->lgmen[LBD_SLOT];
    tank      tanks[MAX_TANKS];
    lgm      *lgms[MAX_TANKS];
    BYTE      n = 0;
    BYTE      c;
    shells    q;
    int       k;

    gs->tanks[LBD_SLOT]->x = tankX;
    gs->tanks[LBD_SLOT]->y = tankY;
    posHistoryInit(&f->sim->posHistory[LBD_SLOT]);
    for (k = 0; k < POSITION_HISTORY_SIZE; k++) {
        posHistoryRecord(&f->sim->posHistory[LBD_SLOT], tankX, tankY, true);
    }
    (*man)->isDead = FALSE;
    (*man)->inTank = FALSE;
    (*man)->state = LGM_STATE_IDLE;
    (*man)->x = lx;
    (*man)->y = ly;
    posHistoryInit(&f->sim->lgmPosHistory[LBD_SLOT]);
    for (k = 0; k < POSITION_HISTORY_SIZE; k++) {
        posHistoryRecord(&f->sim->lgmPosHistory[LBD_SLOT], lx, ly, true);
    }

    shellsAddItem(gs, &gs->shs, startX, LBD_MID_Y(f),
                  (TURNTYPE)BRADIANS_EAST, (TURNTYPE)7, NEUTRAL, NEUTRAL,
                  DMG_NO_PILL, boat ? TRUE : FALSE);
    q = gs->shs;
    q->x = startX;
    q->y = LBD_MID_Y(f);
    q->xAcc = 0;
    q->yAcc = 0;
    q->compensationTicks = comp;
    q->length = length;

    for (c = 0; c < MAX_TANKS; c++) {
        if (f->sim->playerConnected[c] && gs->tanks[c] != NULL) {
            tanks[n] = gs->tanks[c];
            lgms[n]  = &gs->lgmen[c];
            n++;
        }
    }
    for (k = 0; k < updates; k++) {
        shellsUpdate(gs, tanks, n, lgms, &gs->ss);
    }
    return (*man)->isDead == TRUE;
}

/* A shell from 20 inside the square's west edge, the tank well away from
 * its path. One shellsUpdate: the shell moves 32 and hits what is on the
 * square. */
static bool lbd_fire_east_kills(LbdFix *f, WORLD lx, WORLD ly, uint8_t comp,
                                bool boat) {
    return lbd_fire_east_run(f, (WORLD)(f->left + 20), lx, ly, comp, boat,
                             (WORLD)(f->left + MAP_SQUARE_MIDDLE),
                             (WORLD)(f->top - 10 * 256), (BYTE)7, 1);
}

/* The same shell into pill 0, which is at armour 1. *pillDied says whether
 * the shot took the pill's armour to 0. */
static bool lbd_kill_shot_kills(LbdFix *f, WORLD lx, WORLD ly, uint8_t comp,
                                bool *pillDied) {
    bool died;

    f->gs->pb->item[LBD_PILL].armour = 1;
    died = lbd_fire_east_kills(f, lx, ly, comp, false);
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

/* A real shell over a dead pill, for the builder 28 away across the east
 * edge (*across) and the builder in the far north-west corner (*corner).
 * tankHit: the tank stands on the square at 200 and the shell from 150 hits
 * it on its first move, at 182; otherwise the tank is well away and the
 * shell from 200 moves once to 232 and runs out there on the next update.
 * Either way the blast is not on the square's middle, and the pill must
 * still be dead and on its square after it. */
static int lbd_dead_pill_shell(bool tankHit, uint8_t comp, bool *across,
                               bool *corner) {
    LbdFix f;
    int    s;

    for (s = 0; s < 2; s++) {
        WORLD lx, ly;
        bool  died, pillDead;

        if (lbd_open(&f, 0) != 0) {
            lbd_close(&f);
            return 1;
        }
        if (s == 0) {
            lx = LBD_ACROSS_X(&f);
            ly = LBD_MID_Y(&f);
        } else {
            lx = LBD_CORNER_X(&f);
            ly = LBD_CORNER_Y(&f);
        }
        if (tankHit) {
            died = lbd_fire_east_run(&f, (WORLD)(f.left + 150), lx, ly, comp,
                                     false, (WORLD)(f.left + 200),
                                     LBD_MID_Y(&f), (BYTE)7, 1);
        } else {
            /* Life 1: the first update moves it and takes the life to 0,
             * the second sets it off where it is. */
            died = lbd_fire_east_run(&f, (WORLD)(f.left + 200), lx, ly, comp,
                                     false, (WORLD)(f.left + MAP_SQUARE_MIDDLE),
                                     (WORLD)(f.top - 10 * 256),
                                     (BYTE)(SHELL_DEATH + 1), 2);
        }
        pillDead = (f.gs->pb->item[LBD_PILL].armour == 0 &&
                    pillsExistPos(&f.gs->pb, f.px, f.py) == TRUE);
        lbd_close(&f);
        if (s == 0) {
            *across = died;
        } else {
            *corner = died;
        }
        UT_ASSERT_MSG(pillDead, "the dead pill left its square or came back");
    }
    return 0;
}

static int lbd_dead_pill_shell_test(bool tankHit, const char *what) {
    static const uint8_t comps[2] = { 0, 3 };
    int i;

    for (i = 0; i < 2; i++) {
        bool across = false, corner = true;

        if (lbd_dead_pill_shell(tankHit, comps[i], &across, &corner) != 0) {
            return 1;
        }
        UT_ASSERT_MSG(across,
                      "rewind %u: a shell that %s on a dead pill's square left "
                      "a builder across the east edge, inside 128, alive",
                      (unsigned)comps[i], what);
        UT_ASSERT_MSG(!corner,
                      "rewind %u: a shell that %s on a dead pill's square "
                      "killed a builder in its far corner, more than 128 away",
                      (unsigned)comps[i], what);
    }
    return 0;
}

int run_lgm_blast_shell_runs_out_dead_pill(void) {
    return lbd_dead_pill_shell_test(false, "ran out");
}

int run_lgm_blast_shell_hits_tank_dead_pill(void) {
    return lbd_dead_pill_shell_test(true, "hit a tank");
}
