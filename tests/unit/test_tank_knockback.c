/* Raw Mac logs show a shove separate from drive speed, growing as armour
 * falls, with most movement in 0.2-0.4 seconds. Assert those observations
 * through the real hit and movement paths, allowing a pixel for rounding.
 * Coordinates are world units (16 per original Bolo pixel). */
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
    (*value)->bumpWait = 0;
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
    /* Observed cardinal displacement at full, four and two pre-hit HP.
     * WinBolo's armour is five units per displayed Mac armour bar. */
    const BYTE armour[] = {40, 15, 5};
    const int negative_pixels[] = {8, 13, 15};
    const int positive_pixels[] = {6, 11, 13};
    const TURNTYPE angles[] = {TANK_NORTH, TANK_EAST, TANK_SOUTH, TANK_WEST};
    int a, d, route;
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    for (a = 0; a < 3; a++) {
        for (d = 0; d < 4; d++) {
            int direct_dx = 0, direct_dy = 0;
            for (route = 0; route < 2; route++) {
                int dx, dy, along, across;
                int expected = (d == 0 || d == 3) ? negative_pixels[a] : positive_pixels[a];
                kb_place(gs, armour[a]);
                UT_ASSERT(kb_hit(gs, angles[d], route != 0) == TH_HIT);
                UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN && gs->tanks[0]->y == KB_ORIGIN);
                kb_advance(gs, 80);
                dx = (int)gs->tanks[0]->x - KB_ORIGIN;
                dy = (int)gs->tanks[0]->y - KB_ORIGIN;
                along = d == 0 ? -dy : d == 1 ? dx : d == 2 ? dy : -dx;
                across = (d == 0 || d == 2) ? dx : dy;
                UT_ASSERT_MSG(abs(along - expected * 16) <= 16,
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
    serverSimDestroy(sim);
    return 0;
}

int run_tank_knockback_timing_drive(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    const int ticks[] = {2, 7, 14, 20};
    const int north_pixels[] = {3, 8, 11, 12};
    int i, elapsed = 0, control_x, control_y;
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
    int32_t bounce;
    BYTE wait;
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
    wait = gs->tanks[0]->bumpWait;
    tankMineDamage(gs, &gs->tanks[0], 40, 40, 1);
    UT_ASSERT(gs->tanks[0]->bumpX == bounce);
    UT_ASSERT(gs->tanks[0]->bumpY == 0 && gs->tanks[0]->bumpWait == wait);
    serverSimDestroy(sim);
    return 0;
}

int run_tank_knockback_rules_respawn(void) {
    ServerSim *sim = ut_make_running_sim("Knockback");
    GameSim *gs;
    int32_t half_armour, full_armour;
    int fast_x, slow_x, settled_x;
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

    /* The interval changes skid duration, not its eventual distance. */
    gs->rules.tank_bump_interval = 1;
    kb_advance(gs, 6);
    fast_x = gs->tanks[0]->x;
    kb_advance(gs, 74);
    settled_x = gs->tanks[0]->x;
    gs->rules.tank_bump_interval = 2;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    kb_advance(gs, 6);
    slow_x = gs->tanks[0]->x;
    UT_ASSERT(fast_x > slow_x);
    kb_advance(gs, 74);
    UT_ASSERT(gs->tanks[0]->x == settled_x);

    gs->rules.tank_slide_armour_bonus = 32;
    gs->rules.tank_slide_step = 0;
    kb_place(gs, 40);
    UT_ASSERT(kb_hit(gs, TANK_NORTH, FALSE) == TH_HIT);
    kb_advance(gs, 20);
    UT_ASSERT(gs->tanks[0]->x == KB_ORIGIN && gs->tanks[0]->y == KB_ORIGIN);

    /* Legal large slide rules must not wrap the signed velocity backwards. */
    gs->rules.tank_slide_step = 255;
    gs->rules.tank_slide_armour_bonus = 255;
    kb_place(gs, 5);
    UT_ASSERT(kb_hit(gs, TANK_EAST, FALSE) == TH_HIT);
    UT_ASSERT(gs->tanks[0]->bumpX > 32767);
    kb_advance(gs, 1);
    UT_ASSERT(gs->tanks[0]->x > KB_ORIGIN);
    tankDeath(gs, &gs->tanks[0]);
    UT_ASSERT(gs->tanks[0]->bumpX == 0 && gs->tanks[0]->bumpY == 0);
    UT_ASSERT(gs->tanks[0]->bumpWait == 0);

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
