/* Raw Mac logs show a shove separate from drive speed, growing as armour
 * falls, with most movement in 0.2-0.4 seconds. Assert those observations
 * through the real hit and movement paths, allowing a pixel for rounding.
 * Coordinates are world units (16 per original Bolo pixel). */
#include <math.h>
#include <stdlib.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "tank.h"
#include "bolo_map.h"
#include "test_harness.h"

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
    (*value)->bumpRetention = 0;
    gs->inStartFind = FALSE;
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
                UT_ASSERT(gs->tanks[0]->bumpX == 0 && gs->tanks[0]->bumpY == 0);
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
                    vx[frame] = gs->tanks[0]->bumpX;
                    vy[frame] = gs->tanks[0]->bumpY;
                }
                kb_place(gs, 5);
                UT_ASSERT(kb_hit(gs, (TURNTYPE)(heading + 128), TRUE) == TH_HIT);
                for (frame = 0; frame < frames; frame++) {
                    kb_advance(gs, 1);
                    UT_ASSERT_MSG((int)gs->tanks[0]->x - KB_ORIGIN == -dx[frame] &&
                                  (int)gs->tanks[0]->y - KB_ORIGIN == -dy[frame] &&
                                  gs->tanks[0]->bumpX == -vx[frame] &&
                                  gs->tanks[0]->bumpY == -vy[frame],
                                  "heading %d shift %d frame %d is not mirrored",
                                  heading, shifts[s], frame);
                }
                if (shifts[s] != 31) {
                    UT_ASSERT(gs->tanks[0]->bumpX == 0 && gs->tanks[0]->bumpY == 0);
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
    initial = gs->tanks[0]->bumpX;
    kb_advance(gs, 1);
    first = gs->tanks[0]->bumpX;
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 15);
    UT_ASSERT(fabs(first / initial - 0.8660254037844386) < 1e-12);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(fabs(gs->tanks[0]->bumpX / initial - 0.75) < 1e-12);
    kb_advance(gs, 8);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 85);
    UT_ASSERT(fabs(gs->tanks[0]->bumpX / initial - 0.2373046875) < 1e-12);

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
    bounce = gs->tanks[0]->bumpX;
    tankMineDamage(gs, &gs->tanks[0], 40, 40, 1);
    UT_ASSERT(gs->tanks[0]->bumpX == bounce);
    UT_ASSERT(gs->tanks[0]->bumpY == 0);
    serverSimDestroy(sim);
    return 0;
}

int run_tank_knockback_rules_respawn(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    double half_armour, full_armour;
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    kb_place(gs, 20);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    half_armour = gs->tanks[0]->bumpX;
    gs->rules.tank_full_armour = 80;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->bumpX == half_armour);

    /* Disabling the armour bonus makes full and weak tanks move alike. */
    gs->rules.tank_full_armour = 40;
    gs->rules.tank_slide_armour_bonus = 0;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    full_armour = gs->tanks[0]->bumpX;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->bumpX == full_armour);

    /* Zero decay shift consumes the whole shove in one game tick. */
    gs->rules.tank_bump_decay_shift = 0;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(gs->tanks[0]->bumpX == 0);
    gs->rules.tank_bump_decay_shift = 2;

    /* Changing rules during a shove applies to the next hit, without
     * turning a large stored distance into an instantaneous jump. */
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    gs->rules.tank_bump_decay_shift = 0;
    kb_advance(gs, 2);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(fabs(gs->tanks[0]->bumpX - 84.0) < 1e-12);
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN + 28);
    UT_ASSERT(gs->tanks[0]->bumpX == 0);
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
    UT_ASSERT(gs->tanks[0]->bumpX > 4 * 255);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN);
    /* A huge remaining distance must still produce a bounded first step. */
    gs->rules.tank_bump_decay_shift = 31;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->bumpX > 2147483647.0);
    gs->rules.tank_bump_decay_shift = 0;
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN && gs->tanks[0]->x <= KB_ORIGIN + 510);
    gs->rules.tank_bump_decay_shift = 2;
    tankDeath(gs, &gs->tanks[0]);
    UT_ASSERT(gs->tanks[0]->bumpX == 0 && gs->tanks[0]->bumpY == 0);
    UT_ASSERT(gs->tanks[0]->bumpRetention == 0);

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
