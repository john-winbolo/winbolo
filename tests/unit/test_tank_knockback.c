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
#define TK_CLEAR_RADIUS 4

#define TK_TEST_MAP_X 128
#define TK_TEST_MAP_Y 128

static WORLD tk_square_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* Put the slot-0 tank stationary at the test square, clear of anything that
 * could nudge it besides the bump itself, and land one survivable hit from
 * the given angle. Returns non-zero, having printed why, if the hit did not
 * land. */
static int tk_arm_and_hit(GameSim *gs, TURNTYPE angle) {
    int dx, dy;
    WORLD wx, wy;

    tankSetDeathWait(&gs->tanks[0], 0);
    tankSetDestroyed(&gs->tanks[0], FALSE);
    tankSetArmour(&gs->tanks[0], TANK_FULL_ARMOUR);
    tankSetOnBoat(&gs->tanks[0], FALSE);
    tankSetSpeed(&gs->tanks[0], 0);
    gs->inStartFind = FALSE;

    /* Clear the squares round the test square to grass so the building
     * nudge cannot contribute anything — only the bump under test is left
     * to move the tank. The default push settles well inside one map
     * square; TK_CLEAR_RADIUS leaves room for the largest tank_slide_step
     * the rules allow. */
    for (dy = -TK_CLEAR_RADIUS; dy <= TK_CLEAR_RADIUS; dy++) {
        for (dx = -TK_CLEAR_RADIUS; dx <= TK_CLEAR_RADIUS; dx++) {
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
    return 0;
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
    if (tk_arm_and_hit(gsNorth, 0) != 0) {
        return 1;
    }
    tk_settle(gsNorth, &dxN, &dyN);
    serverSimDestroy(simNorth);

    simEast = ut_make_running_sim("P0");
    UT_ASSERT_MSG(simEast != NULL, "ut_make_running_sim returned NULL");
    gsEast = serverSimGetGameSim(simEast);
    UT_ASSERT_MSG(gsEast != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gsEast->tanks[0] != NULL, "slot-0 tank not valid");

    /* East (brad 64): the heading the old flooring bug pushed least. */
    if (tk_arm_and_hit(gsEast, 64) != 0) {
        return 1;
    }
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
    if (tk_arm_and_hit(gsNorth, 0) != 0) {
        return 1;
    }
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
    if (tk_arm_and_hit(gsEast, 64) != 0) {
        return 1;
    }
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

    if (tk_arm_and_hit(gs, 64) != 0) {
        return 1;
    }
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

/* The largest tank_slide_step the rules allow pushes the tank the way the
 * shell was going, the whole distance. Steps of 64 and over are refused by
 * the rule's range (test_sim_rules.c); before that cap, and while bumpX/bumpY
 * were int16_t, a step of 200 wrapped to -28672 and pushed the tank west
 * from a shell fired east. */
int run_tank_knockback_large_step(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    int32_t dx, dy;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    gs->rules.tank_slide_step = 63;
    if (tk_arm_and_hit(gs, 64) != 0) {
        return 1;
    }
    UT_ASSERT_MSG(gs->tanks[0]->bumpX == 63 * 512,
                  "bumpX is %d after a step-63 hit east, wanted %d",
                  (int)gs->tanks[0]->bumpX, 63 * 512);

    tk_settle(gs, &dx, &dy);
    serverSimDestroy(sim);

    /* Step 63 at decay shift 2 slides roughly 63 * 4 WU. */
    UT_ASSERT_MSG(dx > 190,
                  "a step-63 hit fired east pushed x by %d WU, wanted well over 190",
                  (int)dx);
    UT_ASSERT_MSG(dy > -2 && dy < 2, "a hit fired due east drifted y by %d WU", (int)dy);

    return 0;
}

/* A push only shrinks toward zero, never changes sign, and stays at zero
 * once it has died out. At decay shift 0 the old decay took a push east
 * or south past zero to -1 for one tick, which moved the tank 1 WU back
 * against the shell. */
int run_tank_knockback_settles_to_zero(void) {
    /* Shifts above 4 slide the tank past the cleared squares, so stop
     * there; the zero-crossing only depends on the low shifts. */
    static const int32_t shifts[] = { 0, 1, TANK_BUMP_DECAY_SHIFT, 4 };
    static const TURNTYPE angles[] = { 0, 64, 128, 192, 32, 160 };
    size_t si, ai;
    int i;

    for (si = 0; si < sizeof(shifts) / sizeof(shifts[0]); si++) {
        for (ai = 0; ai < sizeof(angles) / sizeof(angles[0]); ai++) {
            ServerSim *sim = ut_make_running_sim("P0");
            GameSim *gs;
            int32_t hitX, hitY;

            UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
            gs = serverSimGetGameSim(sim);
            UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
            UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

            gs->rules.tank_bump_decay_shift = shifts[si];
            if (tk_arm_and_hit(gs, angles[ai]) != 0) {
                return 1;
            }
            hitX = gs->tanks[0]->bumpX;
            hitY = gs->tanks[0]->bumpY;

            for (i = 0; i < TK_SETTLE_TICKS * 4; i++) {
                tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
                UT_ASSERT_MSG((int64_t)gs->tanks[0]->bumpX * hitX >= 0 &&
                              (int64_t)gs->tanks[0]->bumpY * hitY >= 0,
                              "shift %d, brad %d: bump (%d, %d) crossed zero "
                              "from the hit's (%d, %d)",
                              (int)shifts[si], (int)angles[ai],
                              (int)gs->tanks[0]->bumpX, (int)gs->tanks[0]->bumpY,
                              (int)hitX, (int)hitY);
                if (gs->tanks[0]->bumpX == 0 && gs->tanks[0]->bumpY == 0) {
                    break;
                }
            }
            UT_ASSERT_MSG(gs->tanks[0]->bumpX == 0 && gs->tanks[0]->bumpY == 0,
                          "shift %d, brad %d: bump still (%d, %d) after %d ticks",
                          (int)shifts[si], (int)angles[ai],
                          (int)gs->tanks[0]->bumpX, (int)gs->tanks[0]->bumpY, i);

            for (i = 0; i < 4; i++) {
                tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
                UT_ASSERT_MSG(gs->tanks[0]->bumpX == 0 && gs->tanks[0]->bumpY == 0,
                              "shift %d, brad %d: a spent bump came back as (%d, %d)",
                              (int)shifts[si], (int)angles[ai],
                              (int)gs->tanks[0]->bumpX, (int)gs->tanks[0]->bumpY);
            }

            serverSimDestroy(sim);
        }
    }

    return 0;
}
