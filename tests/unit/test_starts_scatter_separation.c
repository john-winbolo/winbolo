/*
 * Spawn scatter separation tests (test_starts_scatter_separation.c).
 *
 * startsScatterFind spirals out from a start square for a deep-sea square
 * with no mine. It is static, so these tests drive it through the public
 * startsGetStart pending-index path: gs->pendingStartIdx[slot] names a
 * 0-based start, and startsGetStart consumes it (resetting the field to
 * MAX_STARTS) before scattering around that start. Two rules are pinned:
 *
 *   (12) a square held by another tank, or beside one, is passed over, so
 *        two slots sent to the same start land at least
 *        START_SPAWN_SEPARATION squares apart;
 *   (13) when no square within the spiral's reach keeps that separation,
 *        the second pass drops the rule and still returns a deep-sea
 *        square rather than failing or running on.
 *
 * The obstacle tank is made with tankCreate on a slot whose pendingStartIdx
 * points at the start under test, so it lands on the start square itself.
 * tankCreate needs the active sim set; ut_make_running_sim's add-player
 * path already did that for this sim before creating slot 0's tank. The
 * scene sits in a deep-sea corner of Everard Island, away from that tank,
 * and each test checks so before it starts.
 */

#include <stdbool.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"     /* mapSetPos, mapGetPos */
#include "starts.h"       /* startsGetStart */
#include "tank.h"         /* tankCreate, tankGetWorld */
#include "server_sim.h"   /* ut_make_running_sim, serverSimGetGameSim */
#include "test_harness.h"

/* Mirrors START_SPAWN_SEPARATION in starts.c, which is private to it. */
#define K_SEPARATION 2

/* The start under test, in a deep-sea corner of the map. */
#define K_SX 24
#define K_SY 24

/* Chebyshev distance, the way startsMapDistance measures. */
static int cheb(int x1, int y1, int x2, int y2) {
    int dx = x1 - x2;
    int dy = y1 - y2;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (dx > dy) ? dx : dy;
}

/* Map square a live tank sits on. */
static void tank_square(GameSim *gs, BYTE slot, int *mx, int *my) {
    WORLD wx;
    WORLD wy;
    tankGetWorld(&gs->tanks[slot], &wx, &wy);
    *mx = (int)(wx >> M_W_SHIFT_SIZE);
    *my = (int)(wy >> M_W_SHIFT_SIZE);
}

/* Set every square within r of the start to terrain, mines cleared. */
static void fill_block(GameSim *gs, int r, BYTE terrain) {
    int x;
    int y;
    for (y = K_SY - r; y <= K_SY + r; y++) {
        for (x = K_SX - r; x <= K_SX + r; x++) {
            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, terrain, FALSE, TRUE);
        }
    }
}

/* One start, index 0, at (K_SX,K_SY). */
static void build_start(GameSim *gs) {
    gs->ss->item[0].x = K_SX;
    gs->ss->item[0].y = K_SY;
    gs->ss->item[0].dir = 0;
    startsSetNumStarts(&gs->ss, 1);
}

/* True when no live tank sits within margin squares of the start. */
static bool scene_is_clear(GameSim *gs, int margin) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        int tx;
        int ty;
        if (gs->tanks[i] == NULL) continue;
        tank_square(gs, (BYTE)i, &tx, &ty);
        if (cheb(tx, ty, K_SX, K_SY) <= margin) return false;
    }
    return true;
}

/* (12) Two slots sent to the same start. Slot 1's tank takes the start
 *      square; slot 2 then lands at least K_SEPARATION squares from it,
 *      on deep sea, and still beside the start. */
int run_starts_scatter_avoids_existing_tanks(void) {
    ServerSim *sim = ut_make_running_sim("Scatter");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    int ax;
    int ay;
    BYTE x = 0;
    BYTE y = 0;
    TURNTYPE dir = 0;

    fill_block(gs, 4, DEEP_SEA);     /* open sea around the start */
    build_start(gs);
    UT_ASSERT_MSG(scene_is_clear(gs, 8), "another tank sits inside the test scene");

    /* First rider: a live tank on slot 1, created at start 0. */
    gs->pendingStartIdx[1] = 0;
    tankCreate(gs, &gs->tanks[1]);
    UT_ASSERT(gs->tanks[1] != NULL);
    tank_square(gs, 1, &ax, &ay);
    UT_ASSERT_MSG(ax == K_SX && ay == K_SY,
                  "first rider should take the start square, got (%d,%d)", ax, ay);

    /* Second rider: slot 2 asks for the same start. */
    gs->pendingStartIdx[2] = 0;
    startsGetStart(gs, &gs->ss, &x, &y, &dir, 2);

    UT_ASSERT_MSG(cheb(x, y, ax, ay) >= K_SEPARATION,
                  "second rider at (%d,%d) is within %d of the tank at (%d,%d)",
                  (int)x, (int)y, K_SEPARATION, ax, ay);
    UT_ASSERT_MSG(mapGetPos(&gs->mp, x, y) == DEEP_SEA,
                  "second rider at (%d,%d) is not on deep sea", (int)x, (int)y);
    UT_ASSERT_MSG(cheb(x, y, K_SX, K_SY) <= K_SEPARATION,
                  "second rider at (%d,%d) wandered off the start", (int)x, (int)y);
    serverSimDestroy(sim);
    return 0;
}

/* (13) The start ringed by land with a tank already on it. START_SCATTER_MAX
 *      steps of the spiral reach out to Chebyshev radius 16, so land out
 *      to 17 with a 3x3 deep-sea pocket at the start leaves every square in
 *      reach either land or within 1 of the obstacle. The separated pass
 *      finds nothing; the second pass still returns a deep-sea square. */
int run_starts_scatter_falls_back_when_crowded(void) {
    ServerSim *sim = ut_make_running_sim("Crowded");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    int ax;
    int ay;
    BYTE x = 0;
    BYTE y = 0;
    TURNTYPE dir = 0;

    fill_block(gs, 17, GRASS);
    fill_block(gs, 1, DEEP_SEA);
    build_start(gs);
    UT_ASSERT_MSG(scene_is_clear(gs, 20), "another tank sits inside the test scene");

    /* The obstacle: slot 1's tank on the start square. */
    gs->pendingStartIdx[1] = 0;
    tankCreate(gs, &gs->tanks[1]);
    UT_ASSERT(gs->tanks[1] != NULL);
    tank_square(gs, 1, &ax, &ay);
    UT_ASSERT_MSG(ax == K_SX && ay == K_SY,
                  "obstacle should take the start square, got (%d,%d)", ax, ay);

    /* Slot 2 asks for the same start. The call has to return. */
    gs->pendingStartIdx[2] = 0;
    startsGetStart(gs, &gs->ss, &x, &y, &dir, 2);

    UT_ASSERT_MSG(mapGetPos(&gs->mp, x, y) == DEEP_SEA,
                  "fallback at (%d,%d) is not on deep sea", (int)x, (int)y);
    UT_ASSERT_MSG(cheb(x, y, K_SX, K_SY) <= 1,
                  "fallback at (%d,%d) is outside the deep-sea pocket", (int)x, (int)y);
    /* Every pocket square is within 1 of the obstacle, so only the pass
     * without the separation rule can have produced this. */
    UT_ASSERT_MSG(cheb(x, y, ax, ay) < K_SEPARATION,
                  "fallback at (%d,%d) is %d or more from the obstacle at (%d,%d)",
                  (int)x, (int)y, K_SEPARATION, ax, ay);
    serverSimDestroy(sim);
    return 0;
}
