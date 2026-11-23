/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * shell_start_add in the pillbox aim solver. A shell starts shell_start_add
 * steps out from where it is fired (shellApplyStartOffset), so the lead
 * pillsTargetTankMove works out has to count the shell's flight from there.
 * The solver used a fixed 5, the rule's default, so a pillbox under any
 * other value led a moving tank as if its shell started somewhere else.
 *
 * A shell that starts further out reaches the tank's path sooner, so the
 * tank has moved less by then and the pillbox needs less lead. The case
 * puts a tank due east of the pill, driving south along a road of its own,
 * and checks the lead shrinks as shell_start_add grows. East and south keep
 * every angle clear of north, where utilCalcAngle answers 256 rather than 0.
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

#define PAS_ROW        40  /* the row the pill and the tank start on */
#define PAS_PILL_X     40  /* the pill's column */
#define PAS_COL        46  /* the column the road runs down, 6 squares east */
#define PAS_ROAD_FIRST 38  /* the road's north end */
#define PAS_ROAD_LAST  60  /* the road's south end */

static WORLD pasCentre(BYTE square) {
    return (WORLD)((square << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A straight road down PAS_COL, with no pill or base on it to stop the
 * predicted tank. */
static int pasLayRoad(ServerSim *sim) {
    BYTE y;

    for (y = PAS_ROAD_FIRST; y <= PAS_ROAD_LAST; y++) {
        UT_ASSERT_MSG(!pillsExistPos(&sim->sim.pb, PAS_COL, y) &&
                      !basesExistPos(&sim->sim.bs, PAS_COL, y),
                      "square %d,%d carries a pill or a base", PAS_COL, (int)y);
        mapSetPos(&sim->sim, &sim->sim.mp, PAS_COL, y, ROAD, FALSE, FALSE);
    }
    return 0;
}

static TURNTYPE pasAim(ServerSim *sim, int32_t startAdd) {
    sim->sim.rules.shell_start_add = startAdd;
    return pillsTargetTankMove(&sim->sim, &sim->sim.mp, &sim->sim.pb,
                               &sim->sim.bs,
                               pasCentre(PAS_PILL_X), pasCentre(PAS_ROW),
                               pasCentre(PAS_COL), pasCentre(PAS_ROW),
                               (TURNTYPE)BRADIANS_SOUTH, 16, FALSE,
                               MAP_SPEED_TBOAT);
}

int run_pill_aim_lead_follows_shell_start_add(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    TURNTYPE direct;
    TURNTYPE aimDefault;
    TURNTYPE aimFar;
    UT_ASSERT(sim != NULL);
    UT_ASSERT(pasLayRoad(sim) == 0);

    direct = utilCalcAngle(pasCentre(PAS_PILL_X), pasCentre(PAS_ROW),
                           pasCentre(PAS_COL), pasCentre(PAS_ROW));
    aimDefault = pasAim(sim, 5);
    aimFar = pasAim(sim, 20);

    /* The tank is due east and driving south, so every lead is south of
       east and short of due south. */
    UT_ASSERT_MSG(fabs((double)direct - BRADIANS_EAST) < 0.5,
                  "the tank's bearing is %.2f, not due east", (double)direct);
    UT_ASSERT_MSG(aimDefault > direct && aimDefault < BRADIANS_SOUTH,
                  "shell_start_add 5 aimed at %.2f, not south of east",
                  (double)aimDefault);
    UT_ASSERT_MSG(aimFar > direct && aimFar < aimDefault,
                  "shell_start_add 20 aimed at %.2f, not between the tank's "
                  "bearing %.2f and the default's lead %.2f",
                  (double)aimFar, (double)direct, (double)aimDefault);

    serverSimDestroy(sim);
    return 0;
}
