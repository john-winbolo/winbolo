/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Where tankDropPills puts a dead tank's pillboxes, and lgmDropCarriedPill the
 * one a dead or departing builder was holding.
 *
 * The search is Mac Bolo's: the tank's own square, then square rings of
 * growing radius, each starting on the square straight north of the tank and
 * going clockwise. The pill at the head of the carry list — the last one
 * picked up — takes the first square that will hold it, the next pill the
 * next one, and so on.
 *
 * run_tank_drop_pills_spiral_order — sixteen pills on open ground fill the
 *                                    tank's square, all of ring one, and the
 *                                    first seven squares of ring two
 * run_tank_drop_pills_blocked      — buildings, half-buildings, boats, pills
 *                                    and bases are passed over in order
 * run_tank_drop_pills_map_edge     — rings running off the minable area,
 *                                    including from a tank outside it and
 *                                    from one on the last square of the map,
 *                                    where a BYTE coordinate would wrap round
 *                                    to the far side
 * run_tank_drop_pills_hostile_map  — a map of buildings with one free square
 *                                    as far from the tank as it can be
 * run_tank_drop_pills_no_room      — a map of boats with no free square at
 *                                    all: the second pass turns the boats
 *                                    under the pills to river
 * run_tank_drop_pills_quit_no_room — the same map, with the player quitting:
 *                                    the pills are on the map afterwards
 * run_lgm_drop_pill_spiral         — the builder's pill goes down on the same
 *                                    spiral round his square
 * run_lgm_drop_pill_no_room        — and on a map of buildings, the second
 *                                    pass turns the building under the pill
 *                                    to rubble
 *
 * Every case lays the map out itself: all grass, the map's bases taken off it,
 * and exactly the pills the case carries.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "game_sim.h"              /* GameSim: mp/pb/bs, tanks[] */
#include "bolo_map.h"              /* the terrain constants */
#include "pillbox.h"               /* pillsInstallItem, pillsGetPill */
#include "bases.h"                 /* basesSetActive, basesInstallItem */
#include "tank.h"                  /* tankDropPills, tankTakePill */
#include "lgm.h"                   /* lgmDropCarriedPill, struct lgmObj */
#include "test_harness.h"

#define TDP_SLOT 0

/* The map's terrain, written straight into the array: the cases below set
 * tens of thousands of squares, and none of them is a change anyone needs
 * telling about. */
static void tdp_set_terrain(ServerSim *sim, int x, int y, BYTE terrain) {
    sim->sim.mp->mapItem[x][y] = terrain;
}

/* All grass, no bases, the tank on (tx, ty) carrying pills 1 to numPills —
 * taken in that order, so pill numPills is at the head of the list and drops
 * first. Every other pill slot is off the map. */
static int tdp_setup(ServerSim *sim, int tx, int ty, BYTE numPills) {
    GameSim *gs = &sim->sim;
    tank *t = &gs->tanks[TDP_SLOT];
    pillbox item;
    BYTE n;
    int x, y;

    UT_ASSERT(*t != NULL);
    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            tdp_set_terrain(sim, x, y, GRASS);
        }
    }
    for (n = 1; n <= basesGetNumBases(&gs->bs); n++) {
        basesSetActive(&gs->bs, n, FALSE);
    }
    for (n = 1; n <= MAX_PILLS; n++) {
        if (n <= numPills) {
            memset(&item, 0, sizeof(item));
            item.owner = NEUTRAL;
            UT_ASSERT(pillsInstallItem(&gs->pb, &item, n));
            tankTakePill(gs, t, n);
        } else if (n <= pillsGetNumPills(&gs->pb)) {
            pillsSetActive(&gs->pb, n, FALSE);
        }
    }
    tankSetLocationData(t, (WORLD) ((tx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                        (WORLD) ((ty << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                        0, 0, FALSE);
    UT_ASSERT(tankGetNumCarriedPills(t) == numPills);
    return 0;
}

/* Pill pillNum is on the map, dead, at (x, y). */
static int tdp_assert_at(ServerSim *sim, BYTE pillNum, int x, int y) {
    pillbox p;

    memset(&p, 0, sizeof(p));
    pillsGetPill(&sim->sim.pb, &p, pillNum);
    UT_ASSERT_MSG(p.inTank == FALSE, "pill %u is still carried",
                  (unsigned) pillNum);
    UT_ASSERT_MSG(p.x == x && p.y == y,
                  "pill %u landed at (%u,%u), wanted (%d,%d)",
                  (unsigned) pillNum, (unsigned) p.x, (unsigned) p.y, x, y);
    UT_ASSERT_MSG(p.armour == 0, "pill %u landed with armour %u",
                  (unsigned) pillNum, (unsigned) p.armour);
    return 0;
}

/* ── Order ───────────────────────────────────────────────────────── */

int run_tank_drop_pills_spiral_order(void) {
    /* The squares in the order they are searched, as offsets from the tank:
       its own square, ring one from due north clockwise, then the start of
       ring two round its north-east corner. */
    static const int order[MAX_PILLS][2] = {
        {  0,  0 },
        {  0, -1 }, {  1, -1 }, {  1,  0 }, {  1,  1 },
        {  0,  1 }, { -1,  1 }, { -1,  0 }, { -1, -1 },
        {  0, -2 }, {  1, -2 }, {  2, -2 }, {  2, -1 },
        {  2,  0 }, {  2,  1 }, {  2,  2 }
    };
    ServerSim *sim = ut_make_running_sim("Tester");
    tank *t;
    int tx = 100, ty = 120;
    int i;

    UT_ASSERT(sim != NULL);
    t = &sim->sim.tanks[TDP_SLOT];
    UT_ASSERT(tdp_setup(sim, tx, ty, MAX_PILLS) == 0);

    tankDropPills(&sim->sim, t);

    UT_ASSERT_MSG(tankGetNumCarriedPills(t) == 0,
                  "%u pills still carried", (unsigned) tankGetNumCarriedPills(t));
    for (i = 0; i < MAX_PILLS; i++) {
        UT_ASSERT(tdp_assert_at(sim, (BYTE) (MAX_PILLS - i),
                                tx + order[i][0], ty + order[i][1]) == 0);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── Squares that will not hold a pill ───────────────────────────── */

int run_tank_drop_pills_blocked(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    tank *t;
    pillbox item;
    base b;
    int tx = 80, ty = 60;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    t = &gs->tanks[TDP_SLOT];
    UT_ASSERT(tdp_setup(sim, tx, ty, 3) == 0);

    /* Everything a pill is refused by, on the tank's square, all of ring
       one, and the first squares of ring two. */
    tdp_set_terrain(sim, tx, ty, BOAT);
    tdp_set_terrain(sim, tx, ty - 1, BUILDING);
    tdp_set_terrain(sim, tx + 1, ty - 1, HALFBUILDING);
    tdp_set_terrain(sim, tx + 1, ty, BUILDING);
    tdp_set_terrain(sim, tx + 1, ty + 1, BUILDING);
    tdp_set_terrain(sim, tx, ty + 1, BUILDING);
    tdp_set_terrain(sim, tx - 1, ty + 1, BUILDING);
    tdp_set_terrain(sim, tx - 1, ty, BUILDING);
    tdp_set_terrain(sim, tx - 1, ty - 1, BUILDING);
    /* A pill already standing on (tx, ty-2)... */
    memset(&item, 0, sizeof(item));
    item.x = (BYTE) tx;
    item.y = (BYTE) (ty - 2);
    item.owner = NEUTRAL;
    UT_ASSERT(pillsInstallItem(&gs->pb, &item, 4));
    /* ...and a base on (tx+1, ty-2). */
    memset(&b, 0, sizeof(b));
    b.x = (BYTE) (tx + 1);
    b.y = (BYTE) (ty - 2);
    b.owner = NEUTRAL;
    UT_ASSERT(basesInstallItem(&gs->bs, &b, 1));

    tankDropPills(gs, t);

    UT_ASSERT(tankGetNumCarriedPills(t) == 0);
    UT_ASSERT(tdp_assert_at(sim, 3, tx + 2, ty - 2) == 0);
    UT_ASSERT(tdp_assert_at(sim, 2, tx + 2, ty - 1) == 0);
    UT_ASSERT(tdp_assert_at(sim, 1, tx + 2, ty) == 0);

    serverSimDestroy(sim);
    return 0;
}

/* ── The edge of the map ─────────────────────────────────────────── */

int run_tank_drop_pills_map_edge(void) {
    ServerSim *sim;
    tank *t;
    int x, y;

    /* On the minable area's top-left square: ring one's north side is off
       it, so the first square it gives is due east. */
    sim = ut_make_running_sim("Tester");
    UT_ASSERT(sim != NULL);
    t = &sim->sim.tanks[TDP_SLOT];
    UT_ASSERT(tdp_setup(sim, MAP_MINE_EDGE_LEFT + 1, MAP_MINE_EDGE_TOP + 1, 3) == 0);
    tankDropPills(&sim->sim, t);
    UT_ASSERT(tankGetNumCarriedPills(t) == 0);
    UT_ASSERT(tdp_assert_at(sim, 3, MAP_MINE_EDGE_LEFT + 1, MAP_MINE_EDGE_TOP + 1) == 0);
    UT_ASSERT(tdp_assert_at(sim, 2, MAP_MINE_EDGE_LEFT + 2, MAP_MINE_EDGE_TOP + 1) == 0);
    UT_ASSERT(tdp_assert_at(sim, 1, MAP_MINE_EDGE_LEFT + 2, MAP_MINE_EDGE_TOP + 2) == 0);
    serverSimDestroy(sim);

    /* Outside the minable area entirely, up in the top-left corner of the
       map. Ring 18 is the first to reach it, and does so first on its east
       side coming south, at the area's top-left square. */
    sim = ut_make_running_sim("Tester");
    UT_ASSERT(sim != NULL);
    t = &sim->sim.tanks[TDP_SLOT];
    UT_ASSERT(tdp_setup(sim, 3, 3, 1) == 0);
    tankDropPills(&sim->sim, t);
    UT_ASSERT(tankGetNumCarriedPills(t) == 0);
    UT_ASSERT(tdp_assert_at(sim, 1, MAP_MINE_EDGE_LEFT + 1, MAP_MINE_EDGE_TOP + 1) == 0);
    serverSimDestroy(sim);

    /* On the last column of the map, with every column of the minable area
       within 40 squares of it built over. Ring 41 is the first to find open
       ground, at x = 214. Its north side runs east from x = 255 to 296,
       which as a BYTE would come back round to the open squares at the left
       of the map; the search must pass them over and go on round to the
       south side, where the first open square coming west is (214, 169). */
    sim = ut_make_running_sim("Tester");
    UT_ASSERT(sim != NULL);
    t = &sim->sim.tanks[TDP_SLOT];
    UT_ASSERT(tdp_setup(sim, 255, 128, 1) == 0);
    for (x = 215; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            tdp_set_terrain(sim, x, y, BUILDING);
        }
    }
    tankDropPills(&sim->sim, t);
    UT_ASSERT(tankGetNumCarriedPills(t) == 0);
    UT_ASSERT(tdp_assert_at(sim, 1, 214, 169) == 0);
    serverSimDestroy(sim);

    return 0;
}

/* ── A map built to have nowhere to put them ─────────────────────── */

int run_tank_drop_pills_hostile_map(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    tank *t;
    int x, y;

    UT_ASSERT(sim != NULL);
    t = &sim->sim.tanks[TDP_SLOT];
    UT_ASSERT(tdp_setup(sim, MAP_MINE_EDGE_LEFT + 1, MAP_MINE_EDGE_BOTTOM - 1, 1) == 0);
    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            tdp_set_terrain(sim, x, y, BUILDING);
        }
    }
    /* The one open square is the far corner of the minable area, on the
       last ring the search walks. */
    tdp_set_terrain(sim, MAP_MINE_EDGE_RIGHT - 1, MAP_MINE_EDGE_TOP + 1, GRASS);

    tankDropPills(&sim->sim, t);

    UT_ASSERT(tankGetNumCarriedPills(t) == 0);
    UT_ASSERT(tdp_assert_at(sim, 1, MAP_MINE_EDGE_RIGHT - 1, MAP_MINE_EDGE_TOP + 1) == 0);

    serverSimDestroy(sim);
    return 0;
}

/* Fills the whole map with one terrain. */
static void tdp_fill(ServerSim *sim, BYTE terrain) {
    int x, y;

    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            tdp_set_terrain(sim, x, y, terrain);
        }
    }
}

/* A map with no square anywhere that will hold a pill. The first pass of the
 * search finds nothing, so the second pass starts again from the tank's
 * square and turns the boat under each pill to river. */
int run_tank_drop_pills_no_room(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    tank *t;

    UT_ASSERT(sim != NULL);
    t = &sim->sim.tanks[TDP_SLOT];
    UT_ASSERT(tdp_setup(sim, 128, 128, 2) == 0);
    tdp_fill(sim, BOAT);

    tankDropPills(&sim->sim, t);

    UT_ASSERT_MSG(tankGetNumCarriedPills(t) == 0,
                  "%u pills still carried, wanted none",
                  (unsigned) tankGetNumCarriedPills(t));
    UT_ASSERT(tdp_assert_at(sim, 2, 128, 128) == 0);
    UT_ASSERT(tdp_assert_at(sim, 1, 128, 127) == 0);
    UT_ASSERT(mapGetPos(&sim->sim.mp, 128, 128) == RIVER);
    UT_ASSERT(mapGetPos(&sim->sim.mp, 128, 127) == RIVER);
    /* The next square of the spiral is not touched. */
    UT_ASSERT(mapGetPos(&sim->sim.mp, 129, 127) == BOAT);

    serverSimDestroy(sim);
    return 0;
}

/* The player quits on the map of boats. tankDestroy frees the carry list
 * after the drop, so a pill left in it would be lost for the round. Both
 * pills must be on the map afterwards. */
int run_tank_drop_pills_quit_no_room(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    pillbox p;
    BYTE n;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(tdp_setup(sim, 128, 128, 2) == 0);
    tdp_fill(sim, BOAT);

    serverSimRemovePlayer(sim, TDP_SLOT);

    for (n = 1; n <= 2; n++) {
        memset(&p, 0, sizeof(p));
        pillsGetPill(&sim->sim.pb, &p, n);
        UT_ASSERT_MSG(p.inTank == FALSE, "pill %u is still marked carried",
                      (unsigned) n);
        UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, p.x, p.y) == RIVER,
                      "pill %u is at (%u,%u) on terrain %u, wanted river",
                      (unsigned) n, (unsigned) p.x, (unsigned) p.y,
                      (unsigned) mapGetPos(&sim->sim.mp, p.x, p.y));
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── The builder's pill ──────────────────────────────────────────── */

/* The slot's man out on (mx, my) holding pill 1, the way a build order leaves
 * him: the pill marked carried and on no tank's list. */
static int tdp_man_holding_pill(ServerSim *sim, int mx, int my) {
    GameSim *gs = &sim->sim;
    lgm *man = &gs->lgmen[TDP_SLOT];
    pillbox item;

    UT_ASSERT(*man != NULL);
    UT_ASSERT(tdp_setup(sim, 30, 30, 0) == 0);
    memset(&item, 0, sizeof(item));
    item.owner = TDP_SLOT;
    item.inTank = TRUE;
    UT_ASSERT(pillsInstallItem(&gs->pb, &item, 1));
    (*man)->x = (WORLD) ((mx << 8) + MAP_SQUARE_MIDDLE);
    (*man)->y = (WORLD) ((my << 8) + MAP_SQUARE_MIDDLE);
    (*man)->inTank = FALSE;
    (*man)->numPills = 1;
    return 0;
}

int run_lgm_drop_pill_spiral(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    lgm *man;
    int mx = 140, my = 90;

    UT_ASSERT(sim != NULL);
    man = &sim->sim.lgmen[TDP_SLOT];
    UT_ASSERT(tdp_man_holding_pill(sim, mx, my) == 0);
    /* His own square and the one due north of it are taken, so the pill
       goes on the next in the spiral: the north-east corner of ring one. */
    tdp_set_terrain(sim, mx, my, HALFBUILDING);
    tdp_set_terrain(sim, mx, my - 1, BUILDING);

    lgmDropCarriedPill(&sim->sim, man);

    UT_ASSERT((*man)->numPills == LGM_NO_PILL);
    UT_ASSERT(tdp_assert_at(sim, 1, mx + 1, my - 1) == 0);

    serverSimDestroy(sim);
    return 0;
}

int run_lgm_drop_pill_no_room(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    lgm *man;

    UT_ASSERT(sim != NULL);
    man = &sim->sim.lgmen[TDP_SLOT];
    UT_ASSERT(tdp_man_holding_pill(sim, 128, 128) == 0);
    tdp_fill(sim, BUILDING);

    lgmDropCarriedPill(&sim->sim, man);

    UT_ASSERT((*man)->numPills == LGM_NO_PILL);
    UT_ASSERT(tdp_assert_at(sim, 1, 128, 128) == 0);
    UT_ASSERT(mapGetPos(&sim->sim.mp, 128, 128) == RUBBLE);
    UT_ASSERT(mapGetPos(&sim->sim.mp, 128, 127) == BUILDING);

    serverSimDestroy(sim);
    return 0;
}
