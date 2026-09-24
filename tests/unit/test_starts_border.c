/*
 * Starts in the mined border (test_starts_border.c).
 *
 * A map file can put a start outside the playable area: Harvard Yard's first
 * start is at file (1,254), far out in the corner. mapIsMine counts every
 * square past MAP_MINE_EDGE_* as mined, so no tank can be placed there. The
 * map loaders take such a start off the map (its live flag goes to FALSE),
 * unless no start is inside the border, when the list is left as it was.
 *
 *   - a border start comes off and the others stay on;
 *   - a list with every start in the border keeps them all;
 *   - a compressed map, which carries no live flags, loads with the same
 *     start off, so a client reading it agrees with the server;
 *   - startsGetStart handed the removed start by a scenario or the lobby
 *     batch does not use it, and startsGetRandStart never draws it;
 *   - Harvard Yard itself loads with start 1 off and the other ten on
 *     (skipped when data/maps is not reachable from the cwd).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"     /* mapPosInBounds, mapSetPos, map (de)compress, mapRead */
#include "pillbox.h"
#include "bases.h"
#include "starts.h"       /* startsRemoveBorderStarts, startsGetStart */
#include "bolo_rand.h"    /* bolo_srand */
#include "server_sim.h"   /* ut_make_running_sim, serverSimGetGameSim */
#include "test_harness.h"

/* Slot 0 in the border (Harvard Yard's square), slots 1 and 2 well inside. */
static const BYTE kBx[3] = {1, 60, 190};
static const BYTE kBy[3] = {254, 60, 190};

static void set_three_starts(starts *ss) {
    int i;
    startsSetNumStarts(ss, 3);
    for (i = 0; i < 3; i++) {
        start item;
        item.x = kBx[i];
        item.y = kBy[i];
        item.dir = 0;
        startsSetStart(ss, &item, (BYTE)(i + 1));
        startsSetActive(ss, (BYTE)(i + 1), TRUE);
    }
}

int run_starts_border_start_dropped(void) {
    starts ss;
    startsCreate(&ss);
    set_three_starts(&ss);

    UT_ASSERT(!mapPosInBounds(kBx[0], kBy[0]));
    UT_ASSERT_MSG(startsRemoveBorderStarts(&ss) == 1,
                  "expected exactly one border start taken off");
    UT_ASSERT_MSG(startsIsActive(&ss, 1) == FALSE, "border start 1 still on the map");
    UT_ASSERT_MSG(startsIsActive(&ss, 2) == TRUE, "start 2 was taken off");
    UT_ASSERT_MSG(startsIsActive(&ss, 3) == TRUE, "start 3 was taken off");
    UT_ASSERT(startsGetNumStarts(&ss) == 3);   /* slot numbers are kept */
    UT_ASSERT(startsGetNumActive(&ss) == 2);
    UT_ASSERT(startsExistPos(&ss, kBx[0], kBy[0]) == FALSE);
    /* A second pass finds nothing more to do. */
    UT_ASSERT(startsRemoveBorderStarts(&ss) == 0);
    startsDestroy(&ss);
    return 0;
}

int run_starts_border_all_border_kept(void) {
    static const BYTE xs[3] = {1, 250, 128};
    static const BYTE ys[3] = {254, 5, 240};
    starts ss;
    int i;
    startsCreate(&ss);
    startsSetNumStarts(&ss, 3);
    for (i = 0; i < 3; i++) {
        start item;
        item.x = xs[i];
        item.y = ys[i];
        item.dir = 0;
        startsSetStart(&ss, &item, (BYTE)(i + 1));
        startsSetActive(&ss, (BYTE)(i + 1), TRUE);
        UT_ASSERT(!mapPosInBounds(xs[i], ys[i]));
    }
    UT_ASSERT_MSG(startsRemoveBorderStarts(&ss) == 0,
                  "an all-border list must be left as it is");
    UT_ASSERT(startsGetNumActive(&ss) == 3);
    startsDestroy(&ss);
    return 0;
}

/* The wire blob carries coordinates only; the loader must reach the same
 * list from them that mapReadStream reached from the file. */
int run_starts_border_compressed_load_agrees(void) {
    map mp, mp2;
    pillboxes pb, pb2;
    bases bs, bs2;
    starts ss, ss2;
    BYTE *buf;
    int len;

    mapCreate(&mp);  pillsCreate(&pb);  basesCreate(&bs);  startsCreate(&ss);
    mapCreate(&mp2); pillsCreate(&pb2); basesCreate(&bs2); startsCreate(&ss2);
    set_three_starts(&ss);
    UT_ASSERT(startsRemoveBorderStarts(&ss) == 1);

    buf = (BYTE *)malloc(MAP_COMPRESSED_MAX_SIZE);
    UT_ASSERT(buf != NULL);
    len = mapSaveCompressedMap(&mp, &pb, &bs, &ss, buf, MAP_COMPRESSED_MAX_SIZE);
    UT_ASSERT_MSG(len > 0, "mapSaveCompressedMap failed");
    UT_ASSERT_MSG(mapLoadCompressedMap(&mp2, &pb2, &bs2, &ss2, buf, len),
                  "mapLoadCompressedMap failed");
    UT_ASSERT(startsGetNumStarts(&ss2) == 3);
    UT_ASSERT_MSG(startsIsActive(&ss2, 1) == FALSE,
                  "client copy put the border start back on the map");
    UT_ASSERT(startsIsActive(&ss2, 2) == TRUE);
    UT_ASSERT(startsIsActive(&ss2, 3) == TRUE);

    free(buf);
    mapDestroy(&mp);  pillsDestroy(&pb);  basesDestroy(&bs);  startsDestroy(&ss);
    mapDestroy(&mp2); pillsDestroy(&pb2); basesDestroy(&bs2); startsDestroy(&ss2);
    return 0;
}

/* The removed start's index handed to startsGetStart by the paths that go
 * straight to a named start is refused: the tank lands inside the playable
 * area, never at or scattered in from the border square. */
int run_starts_border_named_inactive_safe(void) {
    ServerSim *sim = ut_make_running_sim("BorderStart");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->ss != NULL);
    BYTE playerNum = 0;
    int i;
    int s;

    for (i = 1; i < 3; i++) {
        mapSetPos(gs, &gs->mp, kBx[i], kBy[i], DEEP_SEA, FALSE, TRUE);
    }
    set_three_starts(&gs->ss);
    UT_ASSERT(startsRemoveBorderStarts(&gs->ss) == 1);

    for (s = 0; s < 32; s++) {
        BYTE x = 0;
        BYTE y = 0;
        TURNTYPE dir = 0;
        bolo_srand((uint64_t)(s + 1));
        /* Alternate the two named paths: a scenario op's start and the
           lobby batch's reservation. */
        if ((s & 1) == 0) {
            gs->scenarioStartIdx[playerNum] = 0;
        } else {
            gs->pendingStartIdx[playerNum] = 0;
        }
        startsGetStart(gs, &gs->ss, &x, &y, &dir, playerNum);
        UT_ASSERT_MSG(mapPosInBounds(x, y),
                      "seed %d: tank placed at (%u,%u) in the border", s,
                      (unsigned)x, (unsigned)y);
        UT_ASSERT(gs->scenarioStartIdx[playerNum] == MAX_STARTS);
        gs->pendingStartIdx[playerNum] = MAX_STARTS;
    }

    for (s = 0; s < 64; s++) {
        BYTE x = 0;
        BYTE y = 0;
        TURNTYPE dir = 0;
        bolo_srand((uint64_t)(s + 1));
        startsGetRandStart(gs, &gs->ss, &x, &y, &dir);
        UT_ASSERT_MSG(!(x == kBx[0] && y == kBy[0]),
                      "seed %d: random start drew the border start", s);
    }
    serverSimDestroy(sim);
    return 0;
}

int run_starts_border_harvard_yard(void) {
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;
    char path[] = "data/maps/Harvard Yard.map";
    BYTE i;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    if (!mapRead(path, &mp, &pb, &bs, &ss)) {
        fprintf(stderr, "  (skipped '%s' — not reachable from cwd)\n", path);
    } else {
        start first;
        startsGetStartStruct(&ss, &first, 1);
        fprintf(stderr, "  Harvard Yard start 1 in game at (%u,%u)\n",
                (unsigned)first.x, (unsigned)first.y);
        UT_ASSERT(startsGetNumStarts(&ss) == 11);
        UT_ASSERT(!mapPosInBounds(first.x, first.y));
        UT_ASSERT_MSG(startsIsActive(&ss, 1) == FALSE,
                      "Harvard Yard start 1 is still on the map");
        for (i = 2; i <= 11; i++) {
            UT_ASSERT_MSG(startsIsActive(&ss, i) == TRUE,
                          "Harvard Yard start %u was taken off", (unsigned)i);
        }
    }
    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return 0;
}
