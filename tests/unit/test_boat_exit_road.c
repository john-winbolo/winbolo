/*
 * Leaving a boat needs the boat-exit speed on every kind of land, road
 * included (test_boat_exit_road.c).
 *
 * The original WinBolo tankMoveOnBoat let a boat onto land only at
 * BOAT_EXIT_SPEED or more, whatever the land was, and WinBolo 1.17 and Mac
 * Bolo play the same way. The port had made road a hard surface that a boat
 * landed on at any speed, so a slow boat ran ashore onto road but was held
 * off grass. Road is now held like grass: below the tank's exit speed
 * (tankBoatExitSpeed) the bank clamp keeps the tank's centre
 * tank_boat_exit_inset inside the last river square, and at that speed it
 * lands.
 *
 * Every case drives tank 0 through the whole tankUpdate on a server sim, so
 * the bank clamp and the exit branch in Step 9 both run, along a lane laid
 * east-west on row LANE_Y:
 *
 *   1. slow onto road is held at the bank, still in its boat;
 *   2. fast onto road lands and leaves the boat on the last river square;
 *   3. grass is unchanged: slow is held, fast lands;
 *   4. picking up a parked boat from road works at a slow speed, and a slow
 *      tank that has just picked one up is then held off the road beyond;
 *   5. a base square beside the water reads as ROAD (map load forces ROAD
 *      under every base) and so is held and landed on like road, as the
 *      original code did for any land.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "game_sim.h"
#include "tank.h"
#include "bolo_map.h"
#include "bases.h"
#include "test_harness.h"

#define LANE_Y      100
#define BANK_X      104  /* the last river square before the land */
#define SLOW_SPEED  8    /* half the classic boat-exit speed of 16 */
#define DRIVE_TICKS 600  /* far longer than any run needs to reach the bank */

/* Lay river on columns firstRiver..BANK_X and `land` on BANK_X+1..lastLand,
 * five rows high around LANE_Y so nothing to the north or south is land. */
static void layLane(ServerSim *sim, BYTE firstRiver, BYTE land, BYTE lastLand) {
    int x, y;
    for (y = LANE_Y - 2; y <= LANE_Y + 2; y++) {
        for (x = firstRiver; x <= lastLand; x++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE) x, (BYTE) y,
                      (BYTE) (x <= BANK_X ? RIVER : land), FALSE, FALSE);
        }
    }
}

/* Put tank 0 in the middle of square (mx, LANE_Y), at rest, facing east,
 * afloat or not, with no boat trail and no auto slowdown (so a released key
 * keeps the speed it had). */
static void placeTank(ServerSim *sim, BYTE mx, bool inBoat) {
    tankSetWorld(&sim->sim, &sim->sim.tanks[0],
                 (WORLD) ((mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (WORLD) ((LANE_Y << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (TURNTYPE) BRADIANS_EAST, FALSE);
    tankSetSpeed(&sim->sim.tanks[0], 0);
    tankSetOnBoat(&sim->sim.tanks[0], inBoat);
    tankClearBoatTrail(&sim->sim.tanks[0]);
    sim->sim.tanks[0]->residualSpeed = 0;
    sim->sim.tanks[0]->autoSlowdown = FALSE;
}

/* Drive tank 0 east for DRIVE_TICKS. A cap of 0 holds accelerate the whole
 * way; otherwise accelerate until the speed reaches the cap and coast. A
 * tank that set off in a boat brakes once it is ashore, so a full-speed run
 * stops on the land it reached instead of rolling on across the map. */
static void drive(ServerSim *sim, SPEEDTYPE cap) {
    bool startedInBoat = tankIsOnBoat(&sim->sim.tanks[0]);
    int t;
    for (t = 0; t < DRIVE_TICKS; t++) {
        tankButton tb = TACCEL;
        if (startedInBoat && tankIsOnBoat(&sim->sim.tanks[0]) == FALSE) {
            tb = TDECEL;
        } else if (cap > 0 && tankGetActualSpeed(&sim->sim.tanks[0]) >= cap) {
            tb = TNONE;
        }
        tankUpdate(&sim->sim, &sim->sim.tanks[0], tb, FALSE, FALSE);
    }
}

/* The tank is still afloat on the last river square, its centre no nearer
 * the bank than the clamp allows. */
static int assertHeld(ServerSim *sim, const char *label) {
    WORLD x, y;
    WORLD limit = (WORLD) ((((WORLD) BANK_X + 1) << TANK_SHIFT_MAPSIZE) - 1 -
                           (WORLD) sim->sim.rules.tank_boat_exit_inset);
    tankGetWorld(&sim->sim.tanks[0], &x, &y);
    UT_ASSERT_MSG(tankIsOnBoat(&sim->sim.tanks[0]) == TRUE,
                  "%s: a slow boat left its boat on square %d", label,
                  (int) (x >> TANK_SHIFT_MAPSIZE));
    UT_ASSERT_MSG((x >> TANK_SHIFT_MAPSIZE) == BANK_X,
                  "%s: held on square %d, expected the bank square %d", label,
                  (int) (x >> TANK_SHIFT_MAPSIZE), BANK_X);
    UT_ASSERT_MSG(x <= limit,
                  "%s: centre at %d, past the bank hold at %d", label,
                  (int) x, (int) limit);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, BANK_X, LANE_Y) == RIVER,
                  "%s: the bank square is %d, not river", label,
                  (int) mapGetPos(&sim->sim.mp, BANK_X, LANE_Y));
    return 0;
}

/* The tank is ashore past the bank and its boat waits on the bank square. */
static int assertLanded(ServerSim *sim, const char *label) {
    WORLD x, y;
    tankGetWorld(&sim->sim.tanks[0], &x, &y);
    UT_ASSERT_MSG(tankIsOnBoat(&sim->sim.tanks[0]) == FALSE,
                  "%s: a full-speed boat never landed (centre %d, square %d)",
                  label, (int) x, (int) (x >> TANK_SHIFT_MAPSIZE));
    UT_ASSERT_MSG((x >> TANK_SHIFT_MAPSIZE) > BANK_X,
                  "%s: landed on square %d, not past the bank", label,
                  (int) (x >> TANK_SHIFT_MAPSIZE));
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, BANK_X, LANE_Y) == BOAT,
                  "%s: the boat was not left on the bank square (terrain %d)",
                  label, (int) mapGetPos(&sim->sim.mp, BANK_X, LANE_Y));
    return 0;
}

/* Drive a boat east at `cap` (0 = full speed) at a bank of `land`, then
 * check the result with `check`. The lane is long enough for a full-speed
 * run to reach the boat's top speed, which is also its exit speed, before
 * the bank. */
static int bankRun(BYTE land, SPEEDTYPE cap,
                   int (*check)(ServerSim *, const char *), const char *label) {
    ServerSim *sim = ut_make_running_sim("Tester");
    int rc;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);
    UT_ASSERT_MSG(tankBoatExitSpeed(&sim->sim, sim->sim.tanks[0]) == MAP_SPEED_TBOAT,
                  "an unmodified tank leaves a boat at %d", MAP_SPEED_TBOAT);
    layLane(sim, 84, land, 112);
    placeTank(sim, 88, TRUE);
    drive(sim, cap);
    rc = check(sim, label);
    serverSimDestroy(sim);
    return rc;
}

/* A half-speed boat is held off `land`. */
static int slowHeld(BYTE land, const char *label) {
    return bankRun(land, (SPEEDTYPE) SLOW_SPEED, assertHeld, label);
}

/* A full-speed boat lands on `land`. */
static int fastLands(BYTE land, const char *label) {
    return bankRun(land, 0, assertLanded, label);
}

int run_boat_exit_slow_onto_road_held(void) {
    return slowHeld(ROAD, "road, half speed");
}

int run_boat_exit_fast_onto_road_lands(void) {
    return fastLands(ROAD, "road, full speed");
}

int run_boat_exit_grass_unchanged(void) {
    if (slowHeld(GRASS, "grass, half speed") != 0) {
        return 1;
    }
    return fastLands(GRASS, "grass, full speed");
}

/* Driving onto a parked boat takes it at any speed: the tank on road at
 * half speed rolls onto the BOAT square, which turns back into river under
 * it, and sails on. With road again past the boat, that same slow tank is
 * then held off the road like any other slow boat. */
int run_boat_exit_parked_boat_pickup(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    WORLD x, y;
    int px, py;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    /* Road up to the boat, the boat on BANK_X, river past it. */
    for (py = LANE_Y - 2; py <= LANE_Y + 2; py++) {
        for (px = 84; px <= 120; px++) {
            BYTE t = (px < BANK_X) ? ROAD : RIVER;
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE) px, (BYTE) py, t, FALSE, FALSE);
        }
    }
    mapSetPos(&sim->sim, &sim->sim.mp, BANK_X, LANE_Y, BOAT, FALSE, FALSE);
    placeTank(sim, 96, FALSE);
    drive(sim, (SPEEDTYPE) SLOW_SPEED);
    tankGetWorld(&sim->sim.tanks[0], &x, &y);
    UT_ASSERT_MSG(tankIsOnBoat(&sim->sim.tanks[0]) == TRUE,
                  "a slow tank did not pick up the parked boat");
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, BANK_X, LANE_Y) == RIVER,
                  "the picked-up boat's square is %d, not river",
                  (int) mapGetPos(&sim->sim.mp, BANK_X, LANE_Y));
    UT_ASSERT_MSG((x >> TANK_SHIFT_MAPSIZE) > BANK_X,
                  "the tank stopped on square %d instead of sailing on",
                  (int) (x >> TANK_SHIFT_MAPSIZE));

    /* Road, the boat on BANK_X - 1, then road again from BANK_X + 1: the
     * square the boat stood on becomes the river square the slow tank is
     * held on. Laid as a lane whose only river is BANK_X - 1 .. BANK_X. */
    layLane(sim, (BYTE) (BANK_X - 1), ROAD, 112);
    for (py = LANE_Y - 2; py <= LANE_Y + 2; py++) {
        for (px = 84; px < BANK_X - 1; px++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE) px, (BYTE) py, ROAD, FALSE, FALSE);
        }
    }
    mapSetPos(&sim->sim, &sim->sim.mp, (BYTE) (BANK_X - 1), LANE_Y, BOAT, FALSE, FALSE);
    placeTank(sim, 96, FALSE);
    drive(sim, (SPEEDTYPE) SLOW_SPEED);
    if (assertHeld(sim, "road after a picked-up boat, half speed") != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    serverSimDestroy(sim);
    return 0;
}

/* A base square beside the water. Map load writes ROAD under every base, so
 * the exit code sees a base as road: below the exit speed a boat is held off
 * it, at the exit speed it lands on it. The original tankMoveOnBoat asked
 * the same of any land (mapIsLand, a speed above 0 there, and speed >=
 * BOAT_EXIT_SPEED). The base is made neutral first, which any tank may drive
 * onto. */
int run_boat_exit_base_beside_water(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    base bse;
    BYTE bx, by;
    int px, py, pass;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    memset(&bse, 0, sizeof(bse));
    basesGetBase(&sim->sim.bs, &bse, 1);
    bx = bse.x;
    by = bse.y;
    /* Neutral, so any tank may drive onto it, whoever the sim gave it to. */
    bse.owner = NEUTRAL;
    basesSetBase(&sim->sim.bs, &bse, 1);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, bx, by) == ROAD,
                  "the terrain under base 1 at (%d,%d) of %d is %d, expected ROAD",
                  (int) bx, (int) by, (int) basesGetNumBases(&sim->sim.bs),
                  (int) mapGetPos(&sim->sim.mp, bx, by));
    /* The lane must stay clear of the mined map border, where a boat sinks. */
    UT_ASSERT_MSG(bx - 14 > MAP_MINE_EDGE_LEFT && by - 1 > MAP_MINE_EDGE_TOP &&
                  by + 1 < MAP_MINE_EDGE_BOTTOM,
                  "base 1 at (%d,%d) sits too near the map border", (int) bx, (int) by);
    /* The river strip below must hold no base or pill. A base makes
     * mapIsLand answer TRUE on a river square, and a pill is a solid wall
     * in the boat's path; either one would change the bank silently. */
    for (py = by - 1; py <= by + 1; py++) {
        for (px = bx - 14; px < bx; px++) {
            UT_ASSERT_MSG(basesExistPos(&sim->sim.bs, (BYTE) px, (BYTE) py) == FALSE,
                          "a base sits at (%d,%d) on the river strip west of base 1",
                          px, py);
            UT_ASSERT_MSG(pillsExistPos(&sim->sim.pb, (BYTE) px, (BYTE) py) == FALSE,
                          "a pill sits at (%d,%d) on the river strip west of base 1",
                          px, py);
        }
    }

    for (pass = 0; pass < 2; pass++) {
        WORLD x, y;
        WORLD limit = (WORLD) ((((WORLD) bx) << TANK_SHIFT_MAPSIZE) - 1 -
                               (WORLD) sim->sim.rules.tank_boat_exit_inset);
        /* River on the fourteen squares west of the base, three rows high. */
        for (py = by - 1; py <= by + 1; py++) {
            for (px = bx - 14; px < bx; px++) {
                mapSetPos(&sim->sim, &sim->sim.mp, (BYTE) px, (BYTE) py, RIVER, FALSE, FALSE);
            }
        }
        tankSetWorld(&sim->sim, &sim->sim.tanks[0],
                     (WORLD) (((bx - 12) << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                     (WORLD) ((by << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                     (TURNTYPE) BRADIANS_EAST, FALSE);
        tankSetSpeed(&sim->sim.tanks[0], 0);
        tankSetOnBoat(&sim->sim.tanks[0], TRUE);
        tankClearBoatTrail(&sim->sim.tanks[0]);
        sim->sim.tanks[0]->residualSpeed = 0;
        sim->sim.tanks[0]->autoSlowdown = FALSE;

        drive(sim, pass == 0 ? (SPEEDTYPE) SLOW_SPEED : 0);
        tankGetWorld(&sim->sim.tanks[0], &x, &y);
        if (pass == 0) {
            UT_ASSERT_MSG(tankIsOnBoat(&sim->sim.tanks[0]) == TRUE,
                          "a slow boat landed on the base");
            UT_ASSERT_MSG((x >> TANK_SHIFT_MAPSIZE) == bx - 1 && x <= limit,
                          "a slow boat beside the base is at %d (square %d), "
                          "expected held at or short of %d",
                          (int) x, (int) (x >> TANK_SHIFT_MAPSIZE), (int) limit);
        } else {
            UT_ASSERT_MSG(tankIsOnBoat(&sim->sim.tanks[0]) == FALSE,
                          "a full-speed boat never landed on the base "
                          "(centre %d, square %d)",
                          (int) x, (int) (x >> TANK_SHIFT_MAPSIZE));
            UT_ASSERT_MSG((x >> TANK_SHIFT_MAPSIZE) >= bx,
                          "a full-speed boat landed on square %d, short of the base",
                          (int) (x >> TANK_SHIFT_MAPSIZE));
        }
    }

    serverSimDestroy(sim);
    return 0;
}
