/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Regression (issue #340): a pillbox in the man's hands when his player quits
 * was lost for the rest of the round.
 *
 * Ordering a pill built takes it off the tank's carry list and hands it to the
 * man, leaving the pillbox's record marked inTank the whole time he walks it
 * over. serverSimRemovePlayer then dropped the tank's remaining cargo through
 * tankDestroy — which never sees the man's pill, it is not on that list any
 * more — and deleted the man outright, without the pill drop his death routine
 * does. The record survived still flagged as carried, by a man who no longer
 * existed: off the map, so nobody could shoot it, capture it or pick it up,
 * and the ownership migration further down only changed whose name was on it.
 *
 * run_lgm_quit_drops_carried_pill drives the real order path — tank picks the
 * pill up, player orders it built, man walks out with it — then removes the
 * player mid-errand and reads the pillbox list back. Pre-fix the pill is still
 * inTank and pillsExistPos finds nothing at its square.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, playerConnected */
#include "game_sim.h"              /* GameSim: mp/bs/pb/mns, tanks[], lgmen[] */
#include "lgm.h"                   /* lgmAddRequest, lgmGetMX/MY, struct lgmObj */
#include "bolo_map.h"              /* mapGetPos and the terrain constants */
#include "pillbox.h"               /* pillsGetPill, pillsExistPos, pillsGetNumPills */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* tankTakePill, tankSetTrees, tankGetMX/MY */
#include "test_harness.h"

#define LQP_SLOT 0

/* A grass square the man could be sent to build on: no base, pill or mine,
 * and not the one the tank is standing on (an order there is refused). */
static bool lqp_find_build_tile(ServerSim *sim, BYTE *ox, BYTE *oy) {
    BYTE tx = tankGetMX(&sim->sim.tanks[LQP_SLOT]);
    BYTE ty = tankGetMY(&sim->sim.tanks[LQP_SLOT]);
    int x, y;
    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
            if ((BYTE)x == tx && (BYTE)y == ty) {
                continue;
            }
            if (mapGetPos(&sim->sim.mp, (BYTE)x, (BYTE)y) == GRASS &&
                !basesExistPos(&sim->sim.bs, (BYTE)x, (BYTE)y) &&
                !pillsExistPos(&sim->sim.pb, (BYTE)x, (BYTE)y) &&
                !minesExistPos(&sim->sim.mns, &sim->sim.mp, (BYTE)x, (BYTE)y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

int run_lgm_quit_drops_carried_pill(void) {
    ServerSim *sim = ut_make_running_sim("Quitter");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = &sim->sim;
    UT_ASSERT_MSG(gs->tanks[LQP_SLOT] != NULL && gs->lgmen[LQP_SLOT] != NULL,
                  "slot %d joined without a tank or a man", LQP_SLOT);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "the map carries no pillboxes to hand the man");

    /* Pick a pill that is on the map, and put it in the tank the way a
     * capture does. */
    BYTE pillNum = 1;
    pillbox p;
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, pillNum);
    UT_ASSERT_MSG(p.inTank == FALSE,
                  "pill %u starts carried — this test needs one on the map",
                  (unsigned)pillNum);
    tankTakePill(gs, &gs->tanks[LQP_SLOT], pillNum);

    /* Order it built somewhere the man has to walk to. The order costs wood,
     * so stock the tank first; the tank picks the pill off its own carry list
     * as it sends him. */
    BYTE bx = 0, by = 0;
    UT_ASSERT_MSG(lqp_find_build_tile(sim, &bx, &by),
                  "no plain grass tile found to send the man to");
    tankSetTrees(gs, &gs->tanks[LQP_SLOT], (BYTE)TANK_FULL_TREES);
    lgmAddRequest(gs, &gs->lgmen[LQP_SLOT], &gs->tanks[LQP_SLOT], bx, by,
                  LGM_PILL_REQUEST);

    /* Not vacuous: he must really be out there holding it, and the pillbox
     * must really be off the map while he does. */
    UT_ASSERT_MSG(gs->lgmen[LQP_SLOT]->numPills == pillNum,
                  "the man is not carrying pill %u (numPills=%u) — the build "
                  "order never went out",
                  (unsigned)pillNum,
                  (unsigned)gs->lgmen[LQP_SLOT]->numPills);
    UT_ASSERT_MSG(gs->lgmen[LQP_SLOT]->inTank == FALSE,
                  "the man never left the tank");
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, pillNum);
    UT_ASSERT_MSG(p.inTank == TRUE,
                  "pill %u is not marked carried while the man holds it",
                  (unsigned)pillNum);

    BYTE manX = lgmGetMX(&gs->lgmen[LQP_SLOT]);
    BYTE manY = lgmGetMY(&gs->lgmen[LQP_SLOT]);

    /* He quits mid-errand. */
    serverSimRemovePlayer(sim, LQP_SLOT);
    UT_ASSERT_MSG(gs->lgmen[LQP_SLOT] == NULL,
                  "the man survived the removal — nothing was torn down");

    /* THE ASSERTIONS. The pillbox is back on the map, on or just past the
     * square he was standing on, and no longer flagged as carried. Pre-fix
     * inTank is still TRUE and pillsExistPos finds nothing anywhere. */
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, pillNum);
    UT_ASSERT_MSG(p.inTank == FALSE,
                  "pill %u is still marked carried after its carrier left — "
                  "it is nobody's to pick up for the rest of the round",
                  (unsigned)pillNum);
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, p.x, p.y) == TRUE,
                  "pill %u is not on the map at its own square (%u,%u)",
                  (unsigned)pillNum, (unsigned)p.x, (unsigned)p.y);
    {
        int dx = (int)p.x - (int)manX;
        int dy = (int)p.y - (int)manY;
        UT_ASSERT_MSG(dx >= 0 && dx <= 12 && dy >= -1 && dy <= 12,
                      "pill %u landed at (%u,%u), nowhere near the square the "
                      "man was on (%u,%u)",
                      (unsigned)pillNum, (unsigned)p.x, (unsigned)p.y,
                      (unsigned)manX, (unsigned)manY);
    }

    /* And it goes through the same ownership handover as the rest of the
     * leaver's pills: no connected ally here, so it turns neutral. That only
     * holds because the drop happens before the migration walks the list. */
    UT_ASSERT_MSG(pillsGetPillOwner(&gs->pb, pillNum) == NEUTRAL,
                  "dropped pill %u kept owner %u — the migration ran before "
                  "the drop and missed it",
                  (unsigned)pillNum,
                  (unsigned)pillsGetPillOwner(&gs->pb, pillNum));

    serverSimDestroy(sim);
    return 0;
}
