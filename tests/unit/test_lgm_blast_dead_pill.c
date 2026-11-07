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
 * lgm_blast_wall_rubble_shot     - real shells into a wall until it is
 *                                  rubble: building_life + 1 hits. Hit 1
 *                                  makes it a half building, the hits after
 *                                  it wear it down, the last one makes it
 *                                  rubble. Every hit is a hit on the wall's
 *                                  square, so the square rule holds on all
 *                                  of them, the last one included, although
 *                                  the square is rubble when the builder is
 *                                  tested: a builder in either far corner
 *                                  of the square dies on each hit, and a
 *                                  builder on the first world unit of the
 *                                  square to the right or below (128 from
 *                                  the blast) lives on each hit. For
 *                                  building_life 4 (the default), 1 and 7,
 *                                  each once with no rewind and once with a
 *                                  rewind. As in Mac Bolo, where a shell's
 *                                  hit on a square explodes at that square.
 * lgm_blast_forest_shot          - one land shell into a forest (it becomes
 *                                  grass), the same four builder places and
 *                                  the same answers: a hit on the square.
 * lgm_blast_boat_shot            - the same into a boat (it becomes river).
 * lgm_blast_shore_shot           - the same with a boat's shell into grass,
 *                                  the shore it stops on.
 * lgm_blast_bridge_shot          - the same with a boat's shell into a road
 *                                  with river past it: a bridge, shot away
 *                                  to river.
 * lgm_blast_road_open_ground     - a boat's shell into a road with grass
 *                                  past it. The road stays and the hit is
 *                                  not a hit on the square: the blast on its
 *                                  middle is open ground, so the builders in
 *                                  the far corners live and the builders 128
 *                                  away on the next squares die. In Mac Bolo
 *                                  the shell falls there with no hit.
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
 * with the builder out at (lx, ly). boat: the shell is a boat's (onBoat),
 * which grass, swamp, rubble and road stop too. With comp above 0 the shell
 * carries that rewind and the builder's history says he was at (lx, ly) on
 * every frame.
 * One shellsUpdate: the shell moves 32 and hits what is on the square.
 * Reports whether the builder died. */
static bool lbd_fire_east_kills(LbdFix *f, WORLD lx, WORLD ly, uint8_t comp,
                                bool boat) {
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
                  DMG_NO_PILL, boat ? TRUE : FALSE);
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

/* The builder places the wall test and the terrain tests use: the two far
 * corners of the hit square, and the first world unit of the square to the
 * right and of the square below, each 128 from the blast on the middle of
 * the hit square. */
#define LBD_SPOTS 4
static const char *lbdSpotNames[LBD_SPOTS] = {
    "in the north-west corner of the hit square",
    "in the south-east corner of the hit square",
    "on the square to the right, 128 from the blast",
    "on the square below, 128 from the blast"
};
static const bool lbdSpotOnSquare[LBD_SPOTS] = { true, true, false, false };

/* The builder's place for spot s. */
static void lbd_spot(const LbdFix *f, int s, WORLD *lx, WORLD *ly) {
    switch (s) {
        case 0:  *lx = (WORLD)(f->left + 2);   *ly = (WORLD)(f->top + 2);   break;
        case 1:  *lx = (WORLD)(f->left + 253); *ly = (WORLD)(f->top + 253); break;
        case 2:  *lx = (WORLD)(f->left + 256); *ly = LBD_MID_Y(f);         break;
        default: *lx = (WORLD)(f->left + MAP_SQUARE_MIDDLE);
                 *ly = (WORLD)(f->top + 256);                               break;
    }
}

/* A running game with pill 0 off its square (carried) and the square below
 * free grass, so the terrain put on the square is all the shell can hit. */
static int lbd_open_bare(LbdFix *f) {
    bool belowFree;

    if (lbd_open(f, 0) != 0) {
        lbd_close(f);
        return 1;
    }
    f->gs->pb->item[LBD_PILL].inTank = TRUE;
    belowFree = lbd_square_free(f->gs, f->px, f->py + 1);
    if (!belowFree) {
        lbd_close(f);
    }
    UT_ASSERT_MSG(belowFree, "the square below the hit square is not free");
    mapSetPos(f->gs, &f->gs->mp, f->px, (BYTE)(f->py + 1), GRASS, FALSE, FALSE);
    return 0;
}

/* The most hits the wall test fires: building_life 7, plus one. */
#define LBD_WALL_MAX_HITS 8

int run_lgm_blast_wall_rubble_shot(void) {
    static const uint8_t comps[2] = { 0, 3 };
    /* 0 keeps the fixture's building_life, the default (4). */
    static const int lives[3] = { 0, 1, 7 };
    LbdFix f;
    int    l, i, s, h;

    for (l = 0; l < 3; l++) {
        for (i = 0; i < 2; i++) {
            for (s = 0; s < LBD_SPOTS; s++) {
                WORLD lx, ly;
                BYTE  after[LBD_WALL_MAX_HITS];
                bool  died[LBD_WALL_MAX_HITS];
                int   life, hits;

                if (lbd_open_bare(&f) != 0) {
                    return 1;
                }
                if (lives[l] != 0) {
                    f.gs->rules.building_life = lives[l];
                }
                life = (int)f.gs->rules.building_life;
                /* A new wall takes building_life + 1 hits to rubble
                 * (buildingAddItem). */
                hits = life + 1;
                if ((lives[l] == 0 && life != 4) || hits > LBD_WALL_MAX_HITS) {
                    lbd_close(&f);
                }
                UT_ASSERT_MSG(lives[l] != 0 || life == 4,
                              "the default building_life is %d, not 4", life);
                UT_ASSERT_MSG(hits <= LBD_WALL_MAX_HITS,
                              "building_life %d needs more than %d hits",
                              life, LBD_WALL_MAX_HITS);
                mapSetPos(f.gs, &f.gs->mp, f.px, f.py, BUILDING, FALSE, FALSE);

                lbd_spot(&f, s, &lx, &ly);
                for (h = 0; h < hits; h++) {
                    died[h] = lbd_fire_east_kills(&f, lx, ly, comps[i], false);
                    after[h] = mapGetPos(&f.gs->mp, f.px, f.py);
                }
                lbd_close(&f);

                for (h = 0; h < hits; h++) {
                    BYTE want = (h == hits - 1) ? RUBBLE : HALFBUILDING;
                    UT_ASSERT_MSG(after[h] == want,
                                  "building_life %d, rewind %u: after hit %d "
                                  "the wall's square is terrain %u, not %u",
                                  life, (unsigned)comps[i], h + 1,
                                  (unsigned)after[h], (unsigned)want);
                    UT_ASSERT_MSG(died[h] == lbdSpotOnSquare[s],
                                  "building_life %d, rewind %u: hit %d on a "
                                  "wall %s a builder %s",
                                  life, (unsigned)comps[i], h + 1,
                                  died[h] ? "killed" : "left alive",
                                  lbdSpotNames[s]);
                }
            }
        }
    }
    return 0;
}

/* One shell east into the square made `terrain`, with `east` on the square
 * to its right, for each builder place, once with no rewind and once with a
 * rewind. squareHit: the hit is a hit on the square, so only the builders on
 * it die; otherwise the blast on its middle is open ground and only the
 * builders 128 away die. The square is `want` after the hit. */
static int lbd_terrain_shot(const char *what, BYTE terrain, BYTE east,
                            bool boat, bool squareHit, BYTE want) {
    static const uint8_t comps[2] = { 0, 3 };
    LbdFix f;
    int    i, s;

    for (i = 0; i < 2; i++) {
        for (s = 0; s < LBD_SPOTS; s++) {
            WORLD lx, ly;
            BYTE  after;
            bool  died;

            if (lbd_open_bare(&f) != 0) {
                return 1;
            }
            mapSetPos(f.gs, &f.gs->mp, (BYTE)(f.px + 1), f.py, east, FALSE, FALSE);
            mapSetPos(f.gs, &f.gs->mp, f.px, f.py, terrain, FALSE, FALSE);
            lbd_spot(&f, s, &lx, &ly);
            died = lbd_fire_east_kills(&f, lx, ly, comps[i], boat);
            after = mapGetPos(&f.gs->mp, f.px, f.py);
            lbd_close(&f);

            UT_ASSERT_MSG(after == want,
                          "rewind %u: after the hit on %s the square is "
                          "terrain %u, not %u", (unsigned)comps[i], what,
                          (unsigned)after, (unsigned)want);
            UT_ASSERT_MSG(died == (lbdSpotOnSquare[s] == squareHit),
                          "rewind %u: a hit on %s %s a builder %s",
                          (unsigned)comps[i], what,
                          died ? "killed" : "left alive", lbdSpotNames[s]);
        }
    }
    return 0;
}

int run_lgm_blast_forest_shot(void) {
    return lbd_terrain_shot("a forest", FOREST, GRASS, false, true, GRASS);
}

int run_lgm_blast_boat_shot(void) {
    return lbd_terrain_shot("a boat", BOAT, GRASS, false, true, RIVER);
}

/* The first boat's shell on grass no shell has touched leaves it grass
 * (grassAddItem): a hit that changes nothing yet, and still a hit on the
 * square. */
int run_lgm_blast_shore_shot(void) {
    return lbd_terrain_shot("the shore", GRASS, GRASS, true, true, GRASS);
}

int run_lgm_blast_bridge_shot(void) {
    return lbd_terrain_shot("a bridge", ROAD, RIVER, true, true, RIVER);
}

int run_lgm_blast_road_open_ground(void) {
    return lbd_terrain_shot("a road", ROAD, GRASS, true, false, ROAD);
}
