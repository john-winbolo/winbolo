/*
 * Open-game "ideal" start selection (test_starts_open_ideal.c).
 *
 * startsGetStart in gameOpen scans starts from a random offset and returns
 * the first "ideal" one. A start is ideal when no tanks are nearby AND no
 * ENEMY or NEUTRAL pillboxes are within range — friendly pillboxes are
 * allowed. This test pins both halves of that rule:
 *
 *   - a start with only a friendly pill nearby IS ideal, so it is sometimes
 *     chosen over an equally-ideal clean start;
 *   - a start with a neutral pill nearby is NOT ideal, so it is never chosen
 *     while an ideal start exists.
 *
 * Three well-separated starts on forced deep-sea squares, two synthetic
 * pillboxes (one friendly, one neutral), and the map's own pills/bases
 * cleared so only this setup drives the result. The PRNG is re-seeded each
 * iteration so the scan offset is controlled and every offset is exercised.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"     /* mapSetPos */
#include "pillbox.h"
#include "starts.h"       /* startsGetStart */
#include "gametype.h"     /* gameOpen */
#include "bolo_rand.h"    /* bolo_srand */
#include "server_sim.h"   /* ut_make_running_sim, serverSimGetGameSim */
#include "test_harness.h"

#define S_CLEAN    0
#define S_FRIENDLY 1
#define S_NEUTRAL  2

/* start 0 clean, start 1 has a friendly pill nearby, start 2 a neutral pill. */
static void build_open_scene(GameSim *gs, BYTE playerNum) {
    static const BYTE sx[3] = {40, 200, 40};
    static const BYTE sy[3] = {40, 40, 200};
    int i;
    for (i = 0; i < 3; i++) {
        mapSetPos(gs, &gs->mp, sx[i], sy[i], DEEP_SEA, FALSE, TRUE);
        gs->ss->item[i].x = sx[i];
        gs->ss->item[i].y = sy[i];
        gs->ss->item[i].dir = 0;
    }
    startsSetNumStarts(&gs->ss, 3);

    gs->bs->numBases = 0;   /* open path ignores bases; keep the scene clean */

    /* pill 0: friendly, 5 squares from the friendly start (within range) */
    gs->pb->item[0].x = 200;
    gs->pb->item[0].y = 45;
    gs->pb->item[0].owner = playerNum;
    gs->pb->item[0].armour = 15;
    gs->pb->item[0].inTank = FALSE;
    /* pill 1: neutral, 5 squares from the neutral start (within range) */
    gs->pb->item[1].x = 40;
    gs->pb->item[1].y = 205;
    gs->pb->item[1].owner = NEUTRAL;
    gs->pb->item[1].armour = 15;
    gs->pb->item[1].inTank = FALSE;
    pillsSetNumPills(&gs->pb, 2);

    gs->game = gameOpen;
}

/* Which of the three start squares is the returned (x,y) nearest to? The
 * starts are far apart, so a simple Manhattan nearest is unambiguous. */
static int nearest_start(GameSim *gs, BYTE x, BYTE y) {
    int best = -1;
    int bestD = 1 << 30;
    int i;
    for (i = 0; i < 3; i++) {
        int dx = (int)x - gs->ss->item[i].x;
        int dy = (int)y - gs->ss->item[i].y;
        int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

int run_starts_open_ideal_friendly_pill_eligible(void) {
    ServerSim *sim = ut_make_running_sim("OpenIdeal");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(gs->pb != NULL && gs->ss != NULL);

    BYTE playerNum = 0;
    int friendlyChosen = 0;
    int neutralChosen = 0;
    int cleanChosen = 0;
    int s;

    build_open_scene(gs, playerNum);

    for (s = 0; s < 64; s++) {
        BYTE x = 0;
        BYTE y = 0;
        TURNTYPE dir = 0;
        gs->pendingStartIdx[playerNum] = MAX_STARTS;  /* force the open path */
        bolo_srand((uint64_t)(s + 1));
        startsGetStart(gs, &gs->ss, &x, &y, &dir, playerNum);
        switch (nearest_start(gs, x, y)) {
            case S_FRIENDLY: friendlyChosen++; break;
            case S_NEUTRAL:  neutralChosen++;  break;
            default:         cleanChosen++;    break;
        }
    }

    /* friendly-pill start is ideal -> chosen on at least some scan offsets */
    UT_ASSERT_MSG(friendlyChosen > 0,
                  "friendly-pill start never chosen (not treated as ideal)");
    /* neutral-pill start is NOT ideal -> never chosen while an ideal exists */
    UT_ASSERT_MSG(neutralChosen == 0,
                  "neutral-pill start chosen %d times (should never be ideal)",
                  neutralChosen);
    /* sanity: the clean start is ideal too and does get picked */
    UT_ASSERT_MSG(cleanChosen > 0, "clean start never chosen");
    serverSimDestroy(sim);
    return 0;
}

/* A pillbox a removal has taken off the map does not disqualify a start: the
 * neutral pill's start becomes ideal once that pill is removed. */
int run_starts_open_ideal_removed_pill_ignored(void) {
    ServerSim *sim = ut_make_running_sim("OpenIdeal");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    BYTE playerNum = 0;
    int neutralChosen = 0;
    int s;

    build_open_scene(gs, playerNum);
    UT_ASSERT_MSG(pillsRemoveItem(&gs->pb, 2), "setup: removing pill 2 failed");

    for (s = 0; s < 64; s++) {
        BYTE x = 0;
        BYTE y = 0;
        TURNTYPE dir = 0;
        gs->pendingStartIdx[playerNum] = MAX_STARTS;
        bolo_srand((uint64_t)(s + 1));
        startsGetStart(gs, &gs->ss, &x, &y, &dir, playerNum);
        if (nearest_start(gs, x, y) == S_NEUTRAL) neutralChosen++;
    }
    UT_ASSERT_MSG(neutralChosen > 0,
                  "a start beside a removed pillbox was never chosen: the "
                  "placement sweep still counts the pillbox");
    serverSimDestroy(sim);
    return 0;
}

/* A start a removal has taken off the map is never chosen, ideal or not, and
 * nor is it the fallback when nothing else qualifies. */
int run_starts_open_removed_start_never_chosen(void) {
    ServerSim *sim = ut_make_running_sim("OpenIdeal");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    BYTE playerNum = 0;
    int cleanChosen = 0;
    int s;

    build_open_scene(gs, playerNum);
    UT_ASSERT_MSG(startsRemoveItem(&gs->ss, 1), "setup: removing start 1 failed");

    for (s = 0; s < 64; s++) {
        BYTE x = 0;
        BYTE y = 0;
        TURNTYPE dir = 0;
        gs->pendingStartIdx[playerNum] = MAX_STARTS;
        bolo_srand((uint64_t)(s + 1));
        startsGetStart(gs, &gs->ss, &x, &y, &dir, playerNum);
        if (nearest_start(gs, x, y) == S_CLEAN) cleanChosen++;
    }
    UT_ASSERT_MSG(cleanChosen == 0,
                  "a removed start was chosen %d times", cleanChosen);

    /* With every other start under water that is not deep sea, the fallback
       is still not the removed one. */
    mapSetPos(gs, &gs->mp, gs->ss->item[1].x, gs->ss->item[1].y, GRASS, FALSE, TRUE);
    mapSetPos(gs, &gs->mp, gs->ss->item[2].x, gs->ss->item[2].y, GRASS, FALSE, TRUE);
    {
        BYTE x = 0;
        BYTE y = 0;
        TURNTYPE dir = 0;
        gs->pendingStartIdx[playerNum] = MAX_STARTS;
        bolo_srand(7);
        startsGetStart(gs, &gs->ss, &x, &y, &dir, playerNum);
        UT_ASSERT_MSG(nearest_start(gs, x, y) != S_CLEAN,
                      "the fallback placed the tank at a removed start");
    }
    serverSimDestroy(sim);
    return 0;
}
