/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lgm_reach_flood -- the builder reach flood (brainPathfinderLgmReachFlood)
 * against the builder walk (brainPathfinderLgmTravelTicksMap), on real maps.
 *
 * The pill take's lgm_reach (pilltake_v2_old.lua world_inp) is
 *   |mx - omx| + |my - omy| <= 1, or else
 *   lgm_travel_ticks_map(omx, omy, mx, my, 0, 0, 2000, 150) ~= -1.
 * The walk is not a search: each tick the man heads straight at the target
 * and slides along what stops him, and he gives up when he stays 150 ticks
 * on one square or walks 2000 ticks. So one flood from the start cannot give
 * the same answer for every target. What a flood can give is the squares the
 * walk can never reach. This test checks that, for every start and every
 * target it tries:
 *
 *   - the flood is safe: a target the flood leaves out always has a walk
 *     answer of "no". So "flood, then walk only the targets the flood
 *     reaches" gives the walk's answer for every target (0 differences).
 *   - and it counts the targets the flood reaches but the walk does not
 *     (the man stuck on a wall, or out of ticks): there a flood alone would
 *     say "yes" and change the pill take's answer. The test needs some of
 *     those, so the stuck rule is hit on these maps.
 *
 * Maps: some of the shipped maps, plus the pill take arena map
 * (tests/unit/data/pt2_grass_double_take_demo.map). The brain map is made
 * from each map as the bot sees it: the terrain, pills as pillbox squares
 * and bases as refuel squares; then some bases are marked as walls (enemy
 * bases) and some pills as dead pills (road speed), as perception.lua does.
 * Starts: random squares the man can stand on, some squares he cannot
 * (water, a building), and some walks from a blessed start square. Targets:
 * every square within 16 of the start, plus random squares anywhere.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "brain.h"                 /* BDEEPSEA, BPILLBOX_T, BREFBASE_T */
#include "brain_pathfinder.h"
#include "test_harness.h"

#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

#define LRF_MAX_TICKS   2000
#define LRF_STUCK_TICKS 150
#define LRF_BOX         16
#define LRF_STARTS      40
#define LRF_FAR         200

typedef struct {
    long walks, yes, flood_no, flood_yes_walk_no, bad;
} LrfCount;

static uint32_t lrf_seed;
static uint32_t lrf_rand(void) {
    lrf_seed = lrf_seed * 1664525u + 1013904223u;
    return lrf_seed >> 8;
}

/* the brain's map (screenBrainMap terrain codes) from a .map file */
static int lrf_load(const char *path, BYTE *world, BrainPathfinder *pf) {
    map mp = NULL;
    pillboxes pb = NULL;
    bases bs = NULL;
    starts ss = NULL;
    int x, y, i, n;
    mapCreate(&mp); pillsCreate(&pb); basesCreate(&bs); startsCreate(&ss);
    if (!mapRead((char *)path, &mp, &pb, &bs, &ss)) {
        mapDestroy(&mp); pillsDestroy(&pb); basesDestroy(&bs); startsDestroy(&ss);
        return 0;
    }
    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
            BYTE t = mapGetPos(&mp, (BYTE)x, (BYTE)y);
            if (t == DEEP_SEA) t = BDEEPSEA;
            else if (t >= MINE_START && t <= MINE_END) t = (BYTE)(t - MINE_SUBTRACT);
            world[y * 256 + x] = t;
        }
    }
    n = pillsGetNumPills(&pb);
    for (i = 1; i <= n; i++) {
        pillbox p;
        memset(&p, 0, sizeof(p));
        pillsGetPill(&pb, &p, (BYTE)i);
        world[p.y * 256 + p.x] = BPILLBOX_T;
    }
    n = basesGetNumBases(&bs);
    for (i = 1; i <= n; i++) {
        base b;
        memset(&b, 0, sizeof(b));
        basesGetBase(&bs, &b, (BYTE)i);
        world[b.y * 256 + b.x] = BREFBASE_T;
    }
    brainPathfinderSetMap(pf, world);
    /* perception's stamps: every other base an enemy base (a wall), every
     * third pill a dead pill (walked at road speed) */
    brainPathfinderClearLgmBlock(pf);
    n = basesGetNumBases(&bs);
    for (i = 1; i <= n; i += 2) {
        base b;
        memset(&b, 0, sizeof(b));
        basesGetBase(&bs, &b, (BYTE)i);
        brainPathfinderSetLgmBlock(pf, b.x, b.y);
    }
    n = pillsGetNumPills(&pb);
    for (i = 1; i <= n; i += 3) {
        pillbox p;
        memset(&p, 0, sizeof(p));
        pillsGetPill(&pb, &p, (BYTE)i);
        brainPathfinderSetLgmRoad(pf, p.x, p.y);
    }
    mapDestroy(&mp); pillsDestroy(&pb); basesDestroy(&bs); startsDestroy(&ss);
    return 1;
}

/* one target: the walk's answer (as the pill take asks it) against
 * "flood, then walk" */
static void lrf_target(BrainPathfinder *pf, const uint8_t *mark, int sx, int sy,
                       int tx, int ty, BYTE bx, BYTE by, LrfCount *c,
                       const char *name) {
    int ref, fast, walk;
    if (abs(tx - sx) + abs(ty - sy) <= 1) return;   /* lgm_reach: true, no walk */
    walk = brainPathfinderLgmTravelTicksMap(pf, (BYTE)sx, (BYTE)sy, (BYTE)tx, (BYTE)ty,
                                            bx, by, LRF_MAX_TICKS, LRF_STUCK_TICKS);
    ref = walk != -1;
    fast = mark[ty * 256 + tx] ? ref : 0;
    c->walks++;
    if (ref) c->yes++;
    if (!mark[ty * 256 + tx]) c->flood_no++;
    else if (!ref) c->flood_yes_walk_no++;
    if (ref != fast) {
        if (c->bad < 5) {
            fprintf(stderr, "FAIL %s: start (%d,%d) bless (%d,%d) target (%d,%d): "
                    "walk %d ticks but the flood leaves it out\n",
                    name, sx, sy, bx, by, tx, ty, walk);
        }
        c->bad++;
    }
}

static void lrf_start(BrainPathfinder *pf, uint8_t *mark, int sx, int sy,
                      BYTE bx, BYTE by, LrfCount *c, const char *name) {
    int dx, dy, k;
    if (brainPathfinderLgmReachFlood(pf, (BYTE)sx, (BYTE)sy, bx, by, mark) < 0) {
        fprintf(stderr, "FAIL %s: no flood from (%d,%d)\n", name, sx, sy);
        c->bad++;
        return;
    }
    for (dy = -LRF_BOX; dy <= LRF_BOX; dy++) {
        for (dx = -LRF_BOX; dx <= LRF_BOX; dx++) {
            int tx = sx + dx, ty = sy + dy;
            if (tx < 0 || tx > 255 || ty < 0 || ty > 255) continue;
            lrf_target(pf, mark, sx, sy, tx, ty, bx, by, c, name);
        }
    }
    for (k = 0; k < LRF_FAR; k++) {
        lrf_target(pf, mark, sx, sy, (int)(lrf_rand() & 255), (int)(lrf_rand() & 255),
                   bx, by, c, name);
    }
}

int run_lgm_reach_flood(void) {
    static const char *maps[] = {
        WB_REPO_ROOT_DIR "/tests/unit/data/pt2_grass_double_take_demo.map",
        WB_REPO_ROOT_DIR "/data/maps/Everard Island.map",
        WB_REPO_ROOT_DIR "/data/maps/Chew Toy 3.map",
        WB_REPO_ROOT_DIR "/data/maps/Baringi.map",
        WB_REPO_ROOT_DIR "/data/maps/Crankcase.map",
        WB_REPO_ROOT_DIR "/data/maps/Bad Lands.map",
    };
    BYTE *world = (BYTE *)malloc(256 * 256);
    uint8_t *mark = (uint8_t *)malloc(256 * 256);
    LrfCount all;
    int m;
    UT_ASSERT(world != NULL && mark != NULL);
    memset(&all, 0, sizeof(all));
    for (m = 0; m < (int)(sizeof(maps) / sizeof(maps[0])); m++) {
        BrainPathfinder *pf = brainPathfinderCreate();
        const char *name = strrchr(maps[m], '/') + 1;
        LrfCount c;
        int s, tries, land = 0, dead = 0, blessed = 0;
        UT_ASSERT(pf != NULL);
        memset(&c, 0, sizeof(c));
        lrf_seed = 42u + (uint32_t)m;
        if (!lrf_load(maps[m], world, pf)) {
            brainPathfinderDestroy(pf);
            free(world); free(mark);
            UT_FAIL("cannot read %s", maps[m]);
        }
        /* starts the man can stand on; then squares he cannot (the walk
         * stops on its first tick); then walks that leave a blessed square */
        for (s = 0, tries = 0; s < LRF_STARTS && tries < 200000; tries++) {
            int sx = (int)(lrf_rand() & 255), sy = (int)(lrf_rand() & 255);
            if (brainPathfinderLgmManSpeed(pf, (BYTE)sx, (BYTE)sy) <= 0) continue;
            lrf_start(pf, mark, sx, sy, 0, 0, &c, name);
            s++; land++;
        }
        for (s = 0, tries = 0; s < 3 && tries < 200000; tries++) {
            int sx = (int)(lrf_rand() & 255), sy = (int)(lrf_rand() & 255);
            if (brainPathfinderLgmManSpeed(pf, (BYTE)sx, (BYTE)sy) > 0) continue;
            lrf_start(pf, mark, sx, sy, 0, 0, &c, name);
            s++; dead++;
        }
        for (s = 0, tries = 0; s < 4 && tries < 200000; tries++) {
            int sx = (int)(lrf_rand() & 255), sy = (int)(lrf_rand() & 255);
            if (brainPathfinderLgmManSpeed(pf, (BYTE)sx, (BYTE)sy) <= 0) continue;
            lrf_start(pf, mark, sx, sy, (BYTE)sx, (BYTE)sy, &c, name);
            s++; blessed++;
        }
        printf("lgm_reach_flood %s: starts %d (+%d off land, +%d blessed), walks %ld, "
               "reached %ld, flood says no %ld, flood yes but walk no %ld, differences %ld\n",
               name, land, dead, blessed, c.walks, c.yes, c.flood_no,
               c.flood_yes_walk_no, c.bad);
        all.walks += c.walks; all.yes += c.yes; all.flood_no += c.flood_no;
        all.flood_yes_walk_no += c.flood_yes_walk_no; all.bad += c.bad;
        brainPathfinderDestroy(pf);
    }
    free(world);
    free(mark);
    printf("lgm_reach_flood all: walks %ld, reached %ld, flood says no %ld, "
           "flood yes but walk no %ld, differences %ld\n",
           all.walks, all.yes, all.flood_no, all.flood_yes_walk_no, all.bad);
    UT_ASSERT_MSG(all.bad == 0, "%ld targets the flood leaves out are reached by the walk", all.bad);
    UT_ASSERT_MSG(all.flood_yes_walk_no > 0, "no walk was stuck inside the flood: the test does not hit the stuck rule");
    UT_ASSERT_MSG(all.flood_no > 0, "the flood never said no: the test does not try a target the man cannot reach");
    return 0;
}
