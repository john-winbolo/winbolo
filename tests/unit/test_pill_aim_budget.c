/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pill_aim_iterations — the step budget pillsTargetTankMove gets to lead a
 * moving tank.
 *
 *   1. A budget past 255 that runs out still ends. The loop counter was a
 *      BYTE, so it wrapped at 256 and never reached a budget above that: a
 *      tank outrunning the pill's shells towards a wall the shell could not
 *      reach spun the server forever.
 *   2. A budget of 1, which runs no steps at all, aims at where the tank is
 *      standing now, as the rule's description says, rather than due north.
 *
 * Both cases call pillsTargetTankMove directly with a pill and a tank on a
 * straight road of their own, so nothing else on the map steers the lead.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "game_sim.h"
#include "tank.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "util.h"
#include "test_harness.h"

#define PAB_ROW        40  /* the map row the road runs along */
#define PAB_ROAD_FIRST 36  /* the road's west end */
#define PAB_PILL_X     38  /* the pill's square */
#define PAB_TANK_X     42  /* the tank's square, east of the pill */
#define PAB_WALL_X     60  /* the wall ending the road, ~22 squares out */

static WORLD pabCentre(BYTE square) {
    return (WORLD)((square << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A straight road along PAB_ROW from PAB_ROAD_FIRST to just short of a wall
 * at PAB_WALL_X, with no pill or base on it to stop the predicted tank. */
static int pabLayRoad(ServerSim *sim) {
    BYTE x;

    for (x = PAB_ROAD_FIRST; x <= PAB_WALL_X; x++) {
        UT_ASSERT_MSG(!pillsExistPos(&sim->sim.pb, x, PAB_ROW) &&
                      !basesExistPos(&sim->sim.bs, x, PAB_ROW),
                      "square %d,%d carries a pill or a base", (int)x, PAB_ROW);
        mapSetPos(&sim->sim, &sim->sim.mp, x, PAB_ROW,
                  x == PAB_WALL_X ? BUILDING : ROAD, FALSE, FALSE);
    }
    return 0;
}

static TURNTYPE pabAim(ServerSim *sim) {
    return pillsTargetTankMove(&sim->sim, &sim->sim.mp, &sim->sim.pb,
                               &sim->sim.bs,
                               pabCentre(PAB_PILL_X), pabCentre(PAB_ROW),
                               pabCentre(PAB_TANK_X), pabCentre(PAB_ROW),
                               (TURNTYPE)BRADIANS_EAST, 16, FALSE,
                               MAP_SPEED_TBOAT);
}

int run_pill_aim_budget_past_a_byte_ends(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    TURNTYPE aim;
    UT_ASSERT(sim != NULL);
    UT_ASSERT(pabLayRoad(sim) == 0);

    /* A tank driving east at road speed 16 outruns shells at 8, so no step
       catches it before it stops at the wall. By then it is ~5500 units
       out, past the (300 + 5) * 8 = 2440 the shell reaches in the budget,
       so the budget runs out with no hit found. */
    sim->sim.rules.shell_speed = 8;
    sim->sim.rules.pill_aim_iterations = 300;

    aim = pabAim(sim);

    /* The tank never leaves the row, so the last estimate is due east. */
    UT_ASSERT_MSG(fabs((double)aim - BRADIANS_EAST) < 1.0,
                  "a budget that ran out aimed at %.2f, expected about %d",
                  (double)aim, BRADIANS_EAST);

    serverSimDestroy(sim);
    return 0;
}

int run_pill_aim_budget_one_aims_at_tank(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    TURNTYPE aim;
    TURNTYPE direct;
    UT_ASSERT(sim != NULL);
    UT_ASSERT(pabLayRoad(sim) == 0);

    sim->sim.rules.pill_aim_iterations = 1;

    aim = pabAim(sim);
    direct = utilCalcAngle(pabCentre(PAB_PILL_X), pabCentre(PAB_ROW),
                           pabCentre(PAB_TANK_X), pabCentre(PAB_ROW));

    UT_ASSERT_MSG(aim == direct,
                  "a budget of 1 aimed at %.2f, not at the tank's %.2f",
                  (double)aim, (double)direct);

    serverSimDestroy(sim);
    return 0;
}
