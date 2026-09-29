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
#include <stdlib.h>
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

/* A tank_slide_step above 63 makes step * 512 too big for 16 bits. When
 * bumpX/bumpY were int16_t, a step of 200 (102400) wrapped to -28672, so a
 * shell fired east pushed the tank west. */
int run_tank_knockback_large_step(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    int32_t dx, dy;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    gs->rules.tank_slide_step = 200;
    if (tk_arm_and_hit(gs, 64) != 0) {
        return 1;
    }
    UT_ASSERT_MSG(gs->tanks[0]->bumpX == 200 * 512,
                  "bumpX is %d after a step-200 hit east, wanted %d",
                  (int)gs->tanks[0]->bumpX, 200 * 512);

    tk_settle(gs, &dx, &dy);
    serverSimDestroy(sim);

    /* Step 200 at decay shift 2 slides roughly 200 * 4 WU. */
    UT_ASSERT_MSG(dx > 600,
                  "a step-200 hit fired east pushed x by %d WU, wanted well over 600",
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

/* The Mac Bolo shell push (tank_slide_mac on). Raw Mac logs show a shove
 * separate from drive speed, growing as armour falls, with most movement
 * in 0.2-0.4 seconds. Assert those observations through the real hit and
 * movement paths, allowing a pixel for rounding. Coordinates are world
 * units (16 per original Bolo pixel). */

#define KB_ORIGIN (40 * 256 + 128)

static void kb_place(GameSim *gs, BYTE armour) {
    int x, y;
    tank *value = &gs->tanks[0];
    for (y = 37; y <= 43; y++) {
        for (x = 37; x <= 43; x++) {
            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, ROAD, FALSE, FALSE);
        }
    }
    tankSetWorld(gs, value, KB_ORIGIN, KB_ORIGIN, TANK_EAST, FALSE);
    tankSetDestroyed(value, FALSE);
    tankSetArmour(value, armour);
    tankSetDeathWait(value, 0);
    tankSetOnBoat(value, FALSE);
    tankSetSpeed(value, 0);
    tankClearResidualSpeed(value);
    (*value)->autoSlowdown = FALSE;
    (*value)->bumpX = 0;
    (*value)->bumpY = 0;
    (*value)->slideX = 0;
    (*value)->slideY = 0;
    (*value)->slideRetention = 0;
    gs->inStartFind = FALSE;
}

/* Turn the Mac Bolo push on at the step #380 played it with. The armour
 * bonus (32) and decay shift (2) are already the rule defaults. */
static void kb_mac(GameSim *gs) {
    gs->rules.tank_slide_mac = 1;
    gs->rules.tank_slide_step = 28;
}

static tankHit kb_hit(GameSim *gs, TURNTYPE angle, bool rewound) {
    WORLD x, y;
    tankGetWorld(&gs->tanks[0], &x, &y);
    if (rewound) {
        /* Both collision coordinates are historical, distinct from the
         * real tank. Knockback must still move its real position. */
        return tankIsTankHitAtPosition(gs, &gs->tanks[0],
                                      x - 256, y, x - 256, y, angle, 1);
    }
    return tankIsTankHit(gs, &gs->tanks[0], x, y, angle, 1);
}

static void kb_advance(GameSim *gs, int frames) {
    int i;
    for (i = 0; i < frames; i++) {
        tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
    }
}

int run_tank_knockback_armour_paths(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    /* Deliberately use the midpoint of Mac's opposite-direction distances:
     * 7 px at full armour to 14 px on the last surviving shell hit.
     * WinBolo's armour is five units per displayed Mac armour bar. */
    const BYTE armour[] = {40, 35, 30, 25, 20, 15, 10, 5};
    const int pixels[] = {7, 8, 9, 10, 11, 12, 13, 14};
    const TURNTYPE angles[] = {TANK_NORTH, TANK_EAST, TANK_SOUTH, TANK_WEST};
    int a, d, route;
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    kb_mac(gs);
    for (a = 0; a < 8; a++) {
        for (d = 0; d < 4; d++) {
            int direct_dx = 0, direct_dy = 0;
            for (route = 0; route < 2; route++) {
                int dx, dy, along, across;
                int expected = pixels[a];
                kb_place(gs, armour[a]);
                UT_ASSERT(kb_hit(gs, angles[d], route != 0) == TH_HIT);
                UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN && gs->tanks[0]->y == KB_ORIGIN);
                kb_advance(gs, 80);
                dx = (int)gs->tanks[0]->x - KB_ORIGIN;
                dy = (int)gs->tanks[0]->y - KB_ORIGIN;
                along = d == 0 ? -dy : d == 1 ? dx : d == 2 ? dy : -dx;
                across = (d == 0 || d == 2) ? dx : dy;
                UT_ASSERT_MSG(along == expected * 16,
                              "armour %u dir %d route %d moved %d WU, expected about %d",
                              armour[a], d, route, along, expected * 16);
                UT_ASSERT(across == 0);
                UT_ASSERT(tankGetActualSpeed(&gs->tanks[0]) == 0);
                UT_ASSERT(gs->tanks[0]->angle == TANK_EAST);
                UT_ASSERT(gs->tanks[0]->slideX == 0 && gs->tanks[0]->slideY == 0);
                if (route == 0) { direct_dx = dx; direct_dy = dy; }
                else { UT_ASSERT(dx == direct_dx && dy == direct_dy); }
            }
        }
    }
    /* Diagonal shoves must use both axes, independently of hull facing. */
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, 32, FALSE) == TH_HIT);
    kb_advance(gs, 80);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN + 48);
    UT_ASSERT(gs->tanks[0]->y < KB_ORIGIN - 48);
    UT_ASSERT(gs->tanks[0]->angle == TANK_EAST);

    /* Opposite headings must mirror every step and the decaying vector,
     * including oblique angles and the legal decay-shift boundaries. */
    {
        const int shifts[] = {0, 2, 31};
        int dx[80], dy[80];
        double vx[80], vy[80];
        int s, heading, frame;
        for (s = 0; s < 3; s++) {
            int frames = shifts[s] == 31 ? 8 : 80;
            gs->rules.tank_bump_decay_shift = shifts[s];
            for (heading = 0; heading < 128; heading++) {
                kb_place(gs, 5);
                UT_ASSERT(kb_hit(gs, (TURNTYPE)heading, FALSE) == TH_HIT);
                for (frame = 0; frame < frames; frame++) {
                    kb_advance(gs, 1);
                    dx[frame] = (int)gs->tanks[0]->x - KB_ORIGIN;
                    dy[frame] = (int)gs->tanks[0]->y - KB_ORIGIN;
                    vx[frame] = gs->tanks[0]->slideX;
                    vy[frame] = gs->tanks[0]->slideY;
                }
                kb_place(gs, 5);
                UT_ASSERT(kb_hit(gs, (TURNTYPE)(heading + 128), TRUE) == TH_HIT);
                for (frame = 0; frame < frames; frame++) {
                    kb_advance(gs, 1);
                    UT_ASSERT_MSG((int)gs->tanks[0]->x - KB_ORIGIN == -dx[frame] &&
                                  (int)gs->tanks[0]->y - KB_ORIGIN == -dy[frame] &&
                                  gs->tanks[0]->slideX == -vx[frame] &&
                                  gs->tanks[0]->slideY == -vy[frame],
                                  "heading %d shift %d frame %d is not mirrored",
                                  heading, shifts[s], frame);
                }
                if (shifts[s] != 31) {
                    UT_ASSERT(gs->tanks[0]->slideX == 0 && gs->tanks[0]->slideY == 0);
                }
            }
        }
    }
    serverSimDestroy(sim);
    return 0;
}

int run_tank_knockback_timing_drive(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    const int ticks[] = {2, 7, 14, 20};
    const int north_pixels[] = {3, 8, 11, 12};
    int i, elapsed = 0, control_x, control_y;
    double initial, first;
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    kb_mac(gs);
    kb_place(gs, 15);
    UT_ASSERT(kb_hit(gs, TANK_NORTH, FALSE) == TH_HIT);
    /* emulator_solo tick 31239, directly observed positions. */
    for (i = 0; i < 4; i++) {
        int distance;
        kb_advance(gs, ticks[i] - elapsed);
        elapsed = ticks[i];
        distance = KB_ORIGIN - (int)gs->tanks[0]->y;
        UT_ASSERT_MSG(abs(distance - north_pixels[i] * 16) <= 16,
                      "after %d ticks moved %d WU, expected about %d",
                      elapsed, distance, north_pixels[i] * 16);
    }

    /* Every 20 ms tick moves the tank; two ticks retain exactly 75% of
     * the remaining shove. Check physical displacement as well as state. */
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    initial = gs->tanks[0]->slideX;
    kb_advance(gs, 1);
    first = gs->tanks[0]->slideX;
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 15);
    UT_ASSERT(fabs(first / initial - 0.8660254037844386) < 1e-12);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(fabs(gs->tanks[0]->slideX / initial - 0.75) < 1e-12);
    kb_advance(gs, 8);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 85);
    UT_ASSERT(fabs(gs->tanks[0]->slideX / initial - 0.2373046875) < 1e-12);

    kb_place(gs, 40);
    tankSetSpeed(&gs->tanks[0], 8);
    kb_advance(gs, 20);
    control_x = gs->tanks[0]->x;
    control_y = gs->tanks[0]->y;
    kb_place(gs, 40);
    tankSetSpeed(&gs->tanks[0], 8);
    UT_ASSERT(kb_hit(gs, TANK_NORTH, FALSE) == TH_HIT);
    kb_advance(gs, 20);
    UT_ASSERT(gs->tanks[0]->x == control_x);
    UT_ASSERT(gs->tanks[0]->y < control_y - 80);
    UT_ASSERT(tankGetActualSpeed(&gs->tanks[0]) == 8);
    UT_ASSERT(gs->tanks[0]->angle == TANK_EAST);
    serverSimDestroy(sim);
    return 0;
}

int run_tank_knockback_replaces_mines(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    int y;
    double bounce;
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    kb_mac(gs);
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_NORTH, FALSE) == TH_HIT);
    kb_advance(gs, 3);
    y = gs->tanks[0]->y;
    UT_ASSERT(kb_hit(gs, TANK_EAST, TRUE) == TH_HIT);
    kb_advance(gs, 10);
    UT_ASSERT(gs->tanks[0]->y == y);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN);

    /* Mines neither start a shove nor overwrite one already in progress. */
    kb_place(gs, 40);
    tankMineDamage(gs, &gs->tanks[0], 40, 40, 1);
    UT_ASSERT(tankGetArmour(&gs->tanks[0]) < 40);
    kb_advance(gs, 20);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN && gs->tanks[0]->y == KB_ORIGIN);
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    kb_advance(gs, 1);
    bounce = gs->tanks[0]->slideX;
    tankMineDamage(gs, &gs->tanks[0], 40, 40, 1);
    UT_ASSERT(gs->tanks[0]->slideX == bounce);
    UT_ASSERT(gs->tanks[0]->slideY == 0);
    serverSimDestroy(sim);
    return 0;
}

int run_tank_knockback_rules_respawn(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    double half_armour, full_armour;
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    kb_mac(gs);
    kb_place(gs, 20);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    half_armour = gs->tanks[0]->slideX;
    gs->rules.tank_full_armour = 80;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->slideX == half_armour);

    /* Disabling the armour bonus makes full and weak tanks move alike. */
    gs->rules.tank_full_armour = 40;
    gs->rules.tank_slide_armour_bonus = 0;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    full_armour = gs->tanks[0]->slideX;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->slideX == full_armour);

    /* Zero decay shift consumes the whole shove in one game tick. */
    gs->rules.tank_bump_decay_shift = 0;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(gs->tanks[0]->slideX == 0);
    gs->rules.tank_bump_decay_shift = 2;

    /* Changing rules during a shove applies to the next hit, without
     * turning a large stored distance into an instantaneous jump. */
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    gs->rules.tank_bump_decay_shift = 0;
    kb_advance(gs, 2);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(fabs(gs->tanks[0]->slideX - 84.0) < 1e-12);
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(gs->tanks[0]->slideX == 0);
    gs->rules.tank_bump_decay_shift = 2;

    gs->rules.tank_slide_armour_bonus = 32;
    gs->rules.tank_slide_step = 0;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_NORTH, FALSE) == TH_HIT);
    kb_advance(gs, 20);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN && gs->tanks[0]->y == KB_ORIGIN);

    /* Legal large slide rules must not wrap the displacement backwards. */
    gs->rules.tank_slide_step = 255;
    gs->rules.tank_slide_armour_bonus = 255;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->slideX > 4 * 255);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN);
    /* A huge remaining distance must still produce a bounded first step. */
    gs->rules.tank_bump_decay_shift = 31;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->slideX > 2147483647.0);
    gs->rules.tank_bump_decay_shift = 0;
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN && gs->tanks[0]->x <= KB_ORIGIN + 510);
    gs->rules.tank_bump_decay_shift = 2;
    tankDeath(gs, &gs->tanks[0]);
    UT_ASSERT(gs->tanks[0]->slideX == 0 && gs->tanks[0]->slideY == 0);
    UT_ASSERT(gs->tanks[0]->slideRetention == 0);

    /* A custom zero-capacity, zero-damage game has no armour denominator. */
    gs->rules.tank_full_armour = 0;
    gs->rules.shell_damage = 0;
    gs->rules.tank_slide_step = 28;
    kb_place(gs, 0);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN);
    serverSimDestroy(sim);
    return 0;
}

/* With tank_slide_mac off the armour bonus is not read: a weak tank gets
 * the same WinBolo push as a full one, whatever the bonus. */
int run_tank_knockback_mac_off_ignores_bonus(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    gs->rules.tank_slide_armour_bonus = 255;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT_MSG(gs->tanks[0]->bumpX == TANK_SLIDE * 512 &&
                  gs->tanks[0]->bumpY == 0,
                  "bump (%d, %d), wanted (%d, 0)",
                  (int)gs->tanks[0]->bumpX, (int)gs->tanks[0]->bumpY,
                  TANK_SLIDE * 512);
    UT_ASSERT(gs->tanks[0]->slideX == 0 && gs->tanks[0]->slideY == 0);
    serverSimDestroy(sim);
    return 0;
}
