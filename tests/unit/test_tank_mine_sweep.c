/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A tank reveals hidden mines ahead of it (test_tank_mine_sweep.c).
 *
 * With hidden mines on, tankNearMines marks the tank's own square and the
 * three squares ahead of it as seen. Its callers in tankUpdate pass the
 * tank's angle snapped to one of 16 directions in bradians (0, 16, ... 240),
 * and the sweep used to take the direction from the low four bits, which
 * are always zero for those angles. Every tank swept the three squares
 * north of it, whichever way it faced.
 *
 *   run_tank_mine_sweep_follows_heading
 *       hidden mines on all eight squares round the tank, swept once per
 *       direction, passing the angle the way tankUpdate does. The squares
 *       ahead are revealed, and none level with the tank or behind it
 *       are. Ahead and behind
 *       are worked out from the heading here, not read from the sweep's own
 *       tables, so a table and this test cannot share a mistake.
 */

#include <math.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "mines.h"
#include "tank.h"
#include "everard_map.h"
#include "test_harness.h"

/* A square well inside the map's mined border, so mapIsMine reads the
 * terrain rather than answering TRUE for the edge. */
#define TMS_X 100
#define TMS_Y 100

static ServerSim *tms_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "Sweeper", false);
    serverSimStartGame(sim);
    return sim;
}

/* The bearing of neighbour (dx, dy) in degrees clockwise from north, where
 * north is -y as on the map. */
static double tms_bearing(int dx, int dy) {
    double b = atan2((double)dx, (double)-dy) * 180.0 / 3.14159265358979323846;
    return (b < 0.0) ? b + 360.0 : b;
}

static double tms_angle_between(double a, double b) {
    double d = fabs(a - b);
    return (d > 180.0) ? 360.0 - d : d;
}

int run_tank_mine_sweep_follows_heading(void) {
    ServerSim *sim = tms_sim();
    GameSim *gs;
    int dir;

    UT_ASSERT_MSG(sim != NULL, "tms_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");

    /* Hidden mines: a mine counts as seen only once its pos is set. */
    gs->mns->minesHiddenMines = TRUE;

    for (dir = 0; dir < 16; dir++) {
        BYTE angle = (BYTE)(dir * 16); /* what utilGet16Dir hands the sweep */
        double heading = dir * 22.5;
        int dx;
        int dy;

        /* A mine on every square round the tank and under it, none seen. */
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                BYTE x = (BYTE)(TMS_X + dx);
                BYTE y = (BYTE)(TMS_Y + dy);
                mapSetPos(gs, &gs->mp, x, y, (BYTE)(GRASS + MINE_SUBTRACT), FALSE, FALSE);
                minesRemoveItem(&gs->mns, x, y);
            }
        }

        tankNearMines(gs, TMS_X, TMS_Y, angle);

        UT_ASSERT_MSG(minesExistPos(&gs->mns, &gs->mp, TMS_X, TMS_Y) == TRUE,
                      "angle %u: the mine under the tank was not revealed",
                      (unsigned)angle);

        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                double off;
                bool seen;

                if (dx == 0 && dy == 0) {
                    continue;
                }
                off = tms_angle_between(tms_bearing(dx, dy), heading);
                seen = minesExistPos(&gs->mns, &gs->mp,
                                     (BYTE)(TMS_X + dx), (BYTE)(TMS_Y + dy));
                /* Straight ahead, or the two squares either side of a
                 * heading that falls between them, must be swept. */
                if (off < 23.0) {
                    UT_ASSERT_MSG(seen == TRUE,
                                  "angle %u (%.1f deg): the mine ahead at "
                                  "(%+d, %+d) was not revealed",
                                  (unsigned)angle, heading, dx, dy);
                }
                /* The sweep's squares are never more than 67.5 degrees off
                 * the heading, so nothing level with the tank or behind it
                 * may be. */
                if (off > 89.0) {
                    UT_ASSERT_MSG(seen == FALSE,
                                  "angle %u (%.1f deg): the mine behind at "
                                  "(%+d, %+d) was revealed",
                                  (unsigned)angle, heading, dx, dy);
                }
            }
        }
    }

    serverSimDestroy(sim);
    return 0;
}
