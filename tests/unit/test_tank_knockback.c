/*
 * Shell knockback heading symmetry (test_tank_knockback.c).
 *
 * A tank a shell hits and survives gets pushed along the shell's own
 * direction of travel: tankIsTankHit sets bumpX/bumpY from the shell's
 * angle, and tankMoveUnified applies and decays them a little each tick
 * until the push dies out. bumpX/bumpY are signed, and C's >> on a
 * negative value rounds toward -infinity rather than toward zero, so the
 * old code kept an extra world unit of travel per tick on a push with a
 * west or north component that the same push east or south discarded —
 * the settled slide ended up as far as 150 WU pushed north against 119 WU
 * pushed east for the same shell speed. The fix shifts by magnitude and
 * restores the sign, so the settled distance no longer depends on which
 * way the shell was travelling.
 *
 * Driven directly on a ServerSim's GameSim, the same way
 * test_base_death_prediction.c does: tankIsTankHit and tankMoveUnified are
 * shared code, so this exercises the real push without standing up a
 * networked client.
 */

#include <math.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "bolo_map.h"   /* mapSetPos — clear the ground round the test tank */
#include "tank.h"
#include "test_harness.h"

/* Game ticks to run after the hit: the push is fully decayed (bumpX/bumpY
 * back to 0) well inside this many at the default tank_slide_step/
 * tank_bump_decay_shift rule values. */
#define TK_SETTLE_TICKS 60

/* A map square's world centre, away from the edges and clear of anything
 * the map generator put there. */
#define TK_TEST_MAP_X 128
#define TK_TEST_MAP_Y 128

static WORLD tk_square_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* Put the slot-0 tank stationary at the test square, clear of anything that
 * could nudge it besides the bump itself, and land one survivable hit from
 * the given angle. */
static void tk_arm_and_hit(GameSim *gs, TURNTYPE angle) {
    int dx, dy;
    WORLD wx, wy;

    tankSetDeathWait(&gs->tanks[0], 0);
    tankSetDestroyed(&gs->tanks[0], FALSE);
    tankSetArmour(&gs->tanks[0], TANK_FULL_ARMOUR);
    tankSetOnBoat(&gs->tanks[0], FALSE);
    tankSetSpeed(&gs->tanks[0], 0);
    gs->inStartFind = FALSE;

    /* Clear a ring round the square to grass so the building nudge cannot
     * contribute anything — only the bump under test is left to move the
     * tank. The push settles at well under one map square, so a 3x3 ring
     * is enough room for it to run in. */
    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            int nx = TK_TEST_MAP_X + dx, ny = TK_TEST_MAP_Y + dy;
            mapSetPos(gs, &gs->mp, (BYTE)nx, (BYTE)ny, GRASS, FALSE, FALSE);
        }
    }

    tankSetWorld(gs, &gs->tanks[0], tk_square_world(TK_TEST_MAP_X),
                tk_square_world(TK_TEST_MAP_Y), 0, false);

    tankGetWorld(&gs->tanks[0], &wx, &wy);
    /* Owner 1: any player other than the tank's own (0), so the hit counts
     * as damage taken rather than a self-hit the client path would ignore. */
    UT_ASSERT_MSG(tankIsTankHit(gs, &gs->tanks[0], wx, wy, angle, 1) == TH_HIT,
                  "the hit at brad %d did not land as a plain TH_HIT",
                  (int)angle);
}

/* Run the tank forward with no input until the bump has fully decayed, and
 * report how far it ended up from the square centre it started at. */
static void tk_settle(GameSim *gs, int32_t *outDX, int32_t *outDY) {
    WORLD startX, startY, endX, endY;
    int i;

    startX = tk_square_world(TK_TEST_MAP_X);
    startY = tk_square_world(TK_TEST_MAP_Y);

    for (i = 0; i < TK_SETTLE_TICKS; i++) {
        tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
    }

    tankGetWorld(&gs->tanks[0], &endX, &endY);
    *outDX = (int32_t)endX - (int32_t)startX;
    *outDY = (int32_t)endY - (int32_t)startY;
}

int run_tank_knockback_heading_symmetric(void) {
    ServerSim *simNorth = ut_make_running_sim("P0");
    ServerSim *simEast;
    GameSim *gsNorth, *gsEast;
    int32_t dxN, dyN, dxE, dyE;
    double magNorth, magEast, diff;

    UT_ASSERT_MSG(simNorth != NULL, "ut_make_running_sim returned NULL");
    gsNorth = serverSimGetGameSim(simNorth);
    UT_ASSERT_MSG(gsNorth != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gsNorth->tanks[0] != NULL, "slot-0 tank not valid");

    /* North (brad 0): the heading the old flooring bug pushed furthest. */
    tk_arm_and_hit(gsNorth, 0);
    tk_settle(gsNorth, &dxN, &dyN);
    serverSimDestroy(simNorth);

    simEast = ut_make_running_sim("P0");
    UT_ASSERT_MSG(simEast != NULL, "ut_make_running_sim returned NULL");
    gsEast = serverSimGetGameSim(simEast);
    UT_ASSERT_MSG(gsEast != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gsEast->tanks[0] != NULL, "slot-0 tank not valid");

    /* East (brad 64): the heading the old flooring bug pushed least. */
    tk_arm_and_hit(gsEast, 64);
    tk_settle(gsEast, &dxE, &dyE);
    serverSimDestroy(simEast);

    magNorth = sqrt((double)dxN * dxN + (double)dyN * dyN);
    magEast  = sqrt((double)dxE * dxE + (double)dyE * dyE);
    diff = fabs(magNorth - magEast);

    /* Both pushes moved the tank at all. */
    UT_ASSERT_MSG(magNorth > 32.0, "north push settled at only %.1f WU", magNorth);
    UT_ASSERT_MSG(magEast > 32.0, "east push settled at only %.1f WU", magEast);

    /* The old asymmetric shift settled these ~31 WU apart (150 vs 119).
     * The symmetric shift should leave them within ordinary integer
     * rounding of each other. */
    UT_ASSERT_MSG(diff <= 4.0,
                  "north push settled at %.1f WU, east at %.1f WU — %.1f WU "
                  "apart, still heading-dependent",
                  magNorth, magEast, diff);

    return 0;
}

int run_tank_knockback_follows_shell_angle(void) {
    ServerSim *simNorth = ut_make_running_sim("P0");
    ServerSim *simEast;
    GameSim *gsNorth, *gsEast;
    int32_t dxN, dyN, dxE, dyE;

    UT_ASSERT_MSG(simNorth != NULL, "ut_make_running_sim returned NULL");
    gsNorth = serverSimGetGameSim(simNorth);
    UT_ASSERT_MSG(gsNorth != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gsNorth->tanks[0] != NULL, "slot-0 tank not valid");

    /* North (brad 0): WORLD y grows downward, so a hit travelling north
     * must push y negative and leave x at (near) zero. */
    tk_arm_and_hit(gsNorth, 0);
    tk_settle(gsNorth, &dxN, &dyN);
    serverSimDestroy(simNorth);

    UT_ASSERT_MSG(dyN < -32, "a hit fired north pushed y by %d, wanted a firm negative", (int)dyN);
    UT_ASSERT_MSG(dxN > -2 && dxN < 2, "a hit fired due north drifted x by %d WU", (int)dxN);

    simEast = ut_make_running_sim("P0");
    UT_ASSERT_MSG(simEast != NULL, "ut_make_running_sim returned NULL");
    gsEast = serverSimGetGameSim(simEast);
    UT_ASSERT_MSG(gsEast != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gsEast->tanks[0] != NULL, "slot-0 tank not valid");

    /* East (brad 64): pushes x positive, leaves y at (near) zero. */
    tk_arm_and_hit(gsEast, 64);
    tk_settle(gsEast, &dxE, &dyE);
    serverSimDestroy(simEast);

    UT_ASSERT_MSG(dxE > 32, "a hit fired east pushed x by %d, wanted a firm positive", (int)dxE);
    UT_ASSERT_MSG(dyE > -2 && dyE < 2, "a hit fired due east drifted y by %d WU", (int)dyE);

    return 0;
}

int run_tank_knockback_speed_untouched(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    int32_t dx, dy;
    BYTE speedBefore, speedAfter;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    tk_arm_and_hit(gs, 64);
    speedBefore = tankGetSpeed(&gs->tanks[0]);
    UT_ASSERT_MSG(speedBefore == 0, "tank was not stationary before settling");

    tk_settle(gs, &dx, &dy);

    speedAfter = tankGetSpeed(&gs->tanks[0]);
    UT_ASSERT_MSG(speedAfter == 0,
                  "a shell hit changed the tank's own speed byte (%d -> %d) — "
                  "the push must be carried separately from driving speed",
                  (int)speedBefore, (int)speedAfter);
    UT_ASSERT_MSG(dx != 0 || dy != 0,
                  "the tank did not move at all — the hit did not push it");

    serverSimDestroy(sim);
    return 0;
}
