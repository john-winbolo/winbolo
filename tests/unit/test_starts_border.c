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
 *
 * Pillboxes and bases get the same rule (pillsRemoveBorderPills and
 * basesRemoveBorderBases, per kind):
 *
 *   - a border pillbox or base comes off and the others stay on; a pillbox
 *     in a tank is left alone, since its square is only where it was
 *     picked up;
 *   - a list with every item of a kind in the border keeps them all;
 *   - a compressed map loads with the same items off, in the game loader
 *     and in the map preview, whose getters and live counts skip them;
 *   - the game's owner lists, neutral counts, square lookups and the
 *     server readers do not see an item taken off the map.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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
#include "client_mappreview.h"
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

/* ---- Pillboxes and bases ------------------------------------------------ */

/* The same three squares as the starts: slot 1 in the border, 2 and 3
 * inside. The border pill and base belong to player 0, so the owner lists
 * below would show them if they were still counted. */
static void set_three_pills(pillboxes *pb) {
    int i;
    (*pb)->numPills = 0;   /* the setter refuses 0 */
    for (i = 0; i < 3; i++) {
        pillbox item;
        memset(&item, 0, sizeof(item));
        item.x = kBx[i];
        item.y = kBy[i];
        item.owner = (i == 0) ? 0 : NEUTRAL;
        item.armour = 15;
        item.speed = 50;
        item.inTank = FALSE;
        pillsInstallItem(pb, &item, (BYTE)(i + 1));
    }
}

static void set_three_bases(bases *bs) {
    int i;
    (*bs)->numBases = 0;   /* the setter refuses 0 */
    for (i = 0; i < 3; i++) {
        base item;
        memset(&item, 0, sizeof(item));
        item.x = kBx[i];
        item.y = kBy[i];
        item.owner = (i == 0) ? 0 : NEUTRAL;
        item.armour = 90;
        item.shells = 90;
        item.mines = 90;
        basesInstallItem(bs, &item, (BYTE)(i + 1));
    }
}

int run_pills_border_pill_dropped(void) {
    pillboxes pb;
    pillsCreate(&pb);
    set_three_pills(&pb);

    UT_ASSERT(pillsGetNumActive(&pb) == 3);
    UT_ASSERT_MSG(pillsRemoveBorderPills(&pb) == 1,
                  "expected exactly one border pill taken off");
    UT_ASSERT_MSG(pillsIsActive(&pb, 1) == FALSE, "border pill 1 still on the map");
    UT_ASSERT_MSG(pillsIsActive(&pb, 2) == TRUE, "pill 2 was taken off");
    UT_ASSERT_MSG(pillsIsActive(&pb, 3) == TRUE, "pill 3 was taken off");
    UT_ASSERT(pillsGetNumPills(&pb) == 3);   /* slot numbers are kept */
    UT_ASSERT(pillsGetNumActive(&pb) == 2);
    UT_ASSERT(pillsRemoveBorderPills(&pb) == 0);

    /* A pill in a tank is not on a square: even with a border square it
       stays on. */
    pb->numPills = 0;
    {
        pillbox carried;
        memset(&carried, 0, sizeof(carried));
        carried.x = kBx[0];
        carried.y = kBy[0];
        carried.owner = 0;
        carried.armour = 15;
        carried.inTank = TRUE;
        pillsInstallItem(&pb, &carried, 1);
    }
    {
        pillbox inside;
        memset(&inside, 0, sizeof(inside));
        inside.x = kBx[1];
        inside.y = kBy[1];
        inside.owner = NEUTRAL;
        inside.armour = 15;
        pillsInstallItem(&pb, &inside, 2);
    }
    UT_ASSERT_MSG(pillsRemoveBorderPills(&pb) == 0, "a carried pill was taken off");
    UT_ASSERT(pillsIsActive(&pb, 1) == TRUE);
    pillsDestroy(&pb);
    return 0;
}

int run_bases_border_base_dropped(void) {
    bases bs;
    basesCreate(&bs);
    set_three_bases(&bs);

    UT_ASSERT(basesGetNumActive(&bs) == 3);
    UT_ASSERT_MSG(basesRemoveBorderBases(&bs) == 1,
                  "expected exactly one border base taken off");
    UT_ASSERT_MSG(basesIsActive(&bs, 1) == FALSE, "border base 1 still on the map");
    UT_ASSERT_MSG(basesIsActive(&bs, 2) == TRUE, "base 2 was taken off");
    UT_ASSERT_MSG(basesIsActive(&bs, 3) == TRUE, "base 3 was taken off");
    UT_ASSERT(basesGetNumBases(&bs) == 3);
    UT_ASSERT(basesGetNumActive(&bs) == 2);
    UT_ASSERT(basesRemoveBorderBases(&bs) == 0);
    basesDestroy(&bs);
    return 0;
}

/* Every item of a kind in the border: that kind is left as it is, and the
 * rule for one kind does not reach into another. */
int run_items_border_all_border_kept(void) {
    static const BYTE xs[3] = {1, 250, 128};
    static const BYTE ys[3] = {254, 5, 240};
    pillboxes pb;
    bases bs;
    int i;
    pillsCreate(&pb);
    basesCreate(&bs);
    pb->numPills = 0;
    for (i = 0; i < 3; i++) {
        pillbox p;
        memset(&p, 0, sizeof(p));
        p.x = xs[i];
        p.y = ys[i];
        p.owner = NEUTRAL;
        pillsInstallItem(&pb, &p, (BYTE)(i + 1));
        UT_ASSERT(!mapPosInBounds(xs[i], ys[i]));
    }
    set_three_bases(&bs);   /* two of these are inside */
    UT_ASSERT_MSG(pillsRemoveBorderPills(&pb) == 0,
                  "an all-border pill list must be left as it is");
    UT_ASSERT(pillsGetNumActive(&pb) == 3);
    UT_ASSERT(basesRemoveBorderBases(&bs) == 1);

    /* And the other way round: every base in the border. */
    bs->numBases = 0;
    for (i = 0; i < 3; i++) {
        base b;
        memset(&b, 0, sizeof(b));
        b.x = xs[i];
        b.y = ys[i];
        b.owner = NEUTRAL;
        basesInstallItem(&bs, &b, (BYTE)(i + 1));
    }
    UT_ASSERT_MSG(basesRemoveBorderBases(&bs) == 0,
                  "an all-border base list must be left as it is");
    UT_ASSERT(basesGetNumActive(&bs) == 3);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    return 0;
}

/* The blob carries coordinates only. The game loader and the map preview
 * must both reach the list the server reached from the file. */
int run_items_border_compressed_load_agrees(void) {
    map mp, mp2;
    pillboxes pb, pb2;
    bases bs, bs2;
    starts ss, ss2;
    BYTE *buf;
    int len;
    MapPreview *view;
    BYTE x = 0, y = 0;

    mapCreate(&mp);  pillsCreate(&pb);  basesCreate(&bs);  startsCreate(&ss);
    mapCreate(&mp2); pillsCreate(&pb2); basesCreate(&bs2); startsCreate(&ss2);
    set_three_pills(&pb);
    set_three_bases(&bs);
    set_three_starts(&ss);
    UT_ASSERT(pillsRemoveBorderPills(&pb) == 1);
    UT_ASSERT(basesRemoveBorderBases(&bs) == 1);

    buf = (BYTE *)malloc(MAP_COMPRESSED_MAX_SIZE);
    UT_ASSERT(buf != NULL);
    len = mapSaveCompressedMap(&mp, &pb, &bs, &ss, buf, MAP_COMPRESSED_MAX_SIZE);
    UT_ASSERT_MSG(len > 0, "mapSaveCompressedMap failed");
    UT_ASSERT_MSG(mapLoadCompressedMap(&mp2, &pb2, &bs2, &ss2, buf, len),
                  "mapLoadCompressedMap failed");
    UT_ASSERT(pillsGetNumPills(&pb2) == 3);
    UT_ASSERT(basesGetNumBases(&bs2) == 3);
    UT_ASSERT_MSG(pillsIsActive(&pb2, 1) == FALSE,
                  "client copy put the border pill back on the map");
    UT_ASSERT_MSG(basesIsActive(&bs2, 1) == FALSE,
                  "client copy put the border base back on the map");
    UT_ASSERT(pillsIsActive(&pb2, 2) == TRUE && pillsIsActive(&pb2, 3) == TRUE);
    UT_ASSERT(basesIsActive(&bs2, 2) == TRUE && basesIsActive(&bs2, 3) == TRUE);

    /* The map preview reads the same blob. */
    view = clientMapPreviewLoadFromBuffer(buf, len);
    UT_ASSERT_MSG(view != NULL, "clientMapPreviewLoadFromBuffer failed");
    UT_ASSERT(clientMapPreviewGetPillCount(view) == 3);
    UT_ASSERT(clientMapPreviewGetBaseCount(view) == 3);
    UT_ASSERT(clientMapPreviewGetLivePillCount(view) == 2);
    UT_ASSERT(clientMapPreviewGetLiveBaseCount(view) == 2);
    UT_ASSERT(clientMapPreviewGetLiveStartCount(view) == 2);
    UT_ASSERT_MSG(!clientMapPreviewGetPill(view, 1, &x, &y, NULL, NULL),
                  "preview handed out the border pill");
    UT_ASSERT_MSG(!clientMapPreviewGetBase(view, 1, &x, &y, NULL),
                  "preview handed out the border base");
    UT_ASSERT(clientMapPreviewGetPill(view, 2, &x, &y, NULL, NULL) &&
              x == kBx[1] && y == kBy[1]);
    UT_ASSERT(clientMapPreviewGetBase(view, 3, &x, &y, NULL) &&
              x == kBx[2] && y == kBy[2]);
    UT_ASSERT(!clientMapPreviewIsPill(view, kBx[0], kBy[0]));
    UT_ASSERT(!clientMapPreviewIsBase(view, kBx[0], kBy[0]));
    clientMapPreviewDestroy(view);

    free(buf);
    mapDestroy(&mp);  pillsDestroy(&pb);  basesDestroy(&bs);  startsDestroy(&ss);
    mapDestroy(&mp2); pillsDestroy(&pb2); basesDestroy(&bs2); startsDestroy(&ss2);
    return 0;
}

/* In a running game the border pill and base belong to player 0. Once they
 * are off the map nothing that counts, lists or looks up items sees them. */
int run_items_border_game_ignores(void) {
    ServerSim *sim = ut_make_running_sim("BorderItems");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->pb != NULL && gs->bs != NULL);
    ServerSimPillInfo pinfo;
    ServerSimBaseInfo binfo;
    BYTE x = 0, y = 0, owner = 0;

    set_three_pills(&gs->pb);
    set_three_bases(&gs->bs);
    /* Before: player 0 holds the border pill and base. */
    UT_ASSERT(pillsGetNumberOwnedByPlayer(&gs->pb, 0) == 1);
    UT_ASSERT(basesGetNumberOwnedByPlayer(&gs->bs, 0) == 1);

    UT_ASSERT(pillsRemoveBorderPills(&gs->pb) == 1);
    UT_ASSERT(basesRemoveBorderBases(&gs->bs) == 1);

    UT_ASSERT_MSG(pillsGetNumberOwnedByPlayer(&gs->pb, 0) == 0,
                  "player 0 still counted as owning the border pill");
    UT_ASSERT(pillsGetOwnerBitMask(&gs->pb, 0) == 0);
    UT_ASSERT(pillsGetNumNeutral(&gs->pb) == 2);
    UT_ASSERT(pillsGetPillOwner(&gs->pb, 1) == PILL_NOT_FOUND);
    UT_ASSERT(pillsExistPos(&gs->pb, kBx[0], kBy[0]) == FALSE);
    UT_ASSERT(pillsExistPos(&gs->pb, kBx[1], kBy[1]) == TRUE);

    UT_ASSERT_MSG(basesGetNumberOwnedByPlayer(&gs->bs, 0) == 0,
                  "player 0 still counted as owning the border base");
    UT_ASSERT(basesGetOwnerBitMask(&gs->bs, 0) == 0);
    UT_ASSERT(basesGetNumNeutral(&gs->bs) == 2);
    UT_ASSERT(basesExistPos(&gs->bs, kBx[0], kBy[0]) == FALSE);
    UT_ASSERT(basesExistPos(&gs->bs, kBx[2], kBy[2]) == TRUE);

    /* The server readers the headless dump, the gym and the status JSON use
       skip the slot; the *Info readers report it as not active. */
    UT_ASSERT(!serverSimGetPill(sim, 1, &x, &y, &owner, NULL, NULL));
    UT_ASSERT(serverSimGetPill(sim, 2, &x, &y, &owner, NULL, NULL) && owner == NEUTRAL);
    UT_ASSERT(!serverSimGetBase(sim, 1, &x, &y, &owner));
    UT_ASSERT(serverSimGetBase(sim, 2, &x, &y, &owner));
    UT_ASSERT(serverSimGetPillInfo(sim, 1, &pinfo) && !pinfo.active);
    UT_ASSERT(serverSimGetBaseInfo(sim, 1, &binfo) && !binfo.active);

    serverSimDestroy(sim);
    return 0;
}

/* The first Inbuilt Tutorial map in the repo (commit 5b4f8f71, replaced
 * in 4cf7e72d) put pillboxes 11 and 12 on file squares (126,22) and
 * (130,22). After mapCenter they sit at (124,20) and (128,20), in the
 * mined border. A copy is kept as a fixture. */
#ifndef WB_MAP_FIXTURE_DIR
#define WB_MAP_FIXTURE_DIR "tests/fixtures/maps"
#endif
int run_pills_border_old_tutorial(void) {
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;
    char path[512];
    BYTE i;

    snprintf(path, sizeof(path), "%s/old_inbuilt_tutorial.map", WB_MAP_FIXTURE_DIR);
    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    if (!mapRead(path, &mp, &pb, &bs, &ss)) {
        fprintf(stderr, "  (skipped '%s' — not reachable from cwd)\n", path);
    } else {
        UT_ASSERT(pillsGetNumPills(&pb) == 12);
        UT_ASSERT(pillsGetNumActive(&pb) == 10);
        for (i = 1; i <= 12; i++) {
            pillbox p;
            pillsGetPill(&pb, &p, i);
            if (i >= 11) {
                UT_ASSERT(!mapPosInBounds(p.x, p.y));
                UT_ASSERT_MSG(pillsIsActive(&pb, i) == FALSE,
                              "old tutorial pillbox %u is still on the map", (unsigned)i);
            } else {
                UT_ASSERT_MSG(pillsIsActive(&pb, i) == TRUE,
                              "old tutorial pillbox %u was taken off", (unsigned)i);
            }
        }
        UT_ASSERT(basesGetNumActive(&bs) == basesGetNumBases(&bs));
    }
    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return 0;
}
