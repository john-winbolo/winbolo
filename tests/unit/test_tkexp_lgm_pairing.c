/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * tkExplosionUpdate's lgm sweep: index pairing and coverage.
 *
 * The world-update stage of simRunHalfStep hands its subsystems two arrays
 * COMPACTED over the connected players — lgmPtrs[i] and tanksArray[i] both
 * describe the i'th connected player, whatever raw slot that is. The sweeps
 * pair them positionally: lgmDeathCheck(lgms[n-1], ..., &tanks[n-1]), where
 * the tank argument tells a killed man which tank to walk back to.
 *
 * Two defects, both only observable once the occupied slots are
 * non-contiguous or the player count is above one — i.e. after anyone leaves
 * mid-game:
 *
 *  1. server_sim.c passed &sim->sim.tanks[0] — the RAW slot array — as the
 *     tank argument while lgmPtrs was compacted. With players in slots 0 and
 *     2, compacted index 1 paired slot 2's man with raw slot 1's tank: a
 *     different player's tank, or (as here) an empty slot, which sends the
 *     man back to where he died instead of to his tank.
 *
 *  2. The small-explosion sweep in tkExplosionUpdate looped `count < numLgm`
 *     where its four siblings in tkExplosionBigExplosion and the one in
 *     minesExpCheckFill all use `count <= numLgm` over 1-based indices. The
 *     highest-indexed player's man was therefore never checked against a
 *     small tank explosion at all.
 *
 * The test puts two players in slots 0 and 2, drops the LAST one's man out of
 * his tank, and detonates a small tank explosion on top of him. Defect 2 makes
 * him survive it; defect 1 makes him walk back to the wrong place.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"  /* sim->sim, playerConnected — T2 */
#include "server_sim_lifecycle.h"
#include "tankexp.h"              /* tkExplosionAddItem/Update, TK_* */
#include "lgm.h"
#include "everard_map.h"
#include "test_harness.h"

/* Two squares east — far enough that his tank is not where he is standing, so
 * "walked back to his tank" and "stayed where he died" are distinguishable. */
#define MAN_OFFSET_SQUARES 2

int run_tkexp_lgm_pairing(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    /* Deliberate gap at slot 1: this is what a mid-game departure leaves. */
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    serverSimStartGame(sim);

    GameSim *gs = &sim->sim;
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[2] != NULL,
                  "both players need tanks");
    UT_ASSERT_MSG(gs->tanks[1] == NULL,
                  "slot 1 must be empty — the gap is the whole point");
    UT_ASSERT(gs->lgmen[2] != NULL);

    /* The compacted snapshot simRunHalfStep builds and passes down. */
    tank  tanksArray[MAX_TANKS];
    lgm  *lgmPtrs[MAX_TANKS];
    BYTE  numTanks = 0;
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && gs->tanks[i] != NULL) {
            tanksArray[numTanks] = gs->tanks[i];
            lgmPtrs[numTanks] = &gs->lgmen[i];
            numTanks++;
        }
    }
    UT_ASSERT_MSG(numTanks == 2, "expected 2 connected players, got %d",
                  (int)numTanks);
    UT_ASSERT_MSG(lgmPtrs[1] == &gs->lgmen[2],
                  "slot 2 must land at the LAST compacted index for this test "
                  "to exercise the off-by-one");
    UT_ASSERT_MSG(tanksArray[1] == gs->tanks[2],
                  "the compacted tank array's last entry is slot 2's tank — "
                  "this is what the caller must pair with lgmPtrs[1], and what "
                  "the raw &tanks[0] got wrong");

    /* Slot 2's man, out of his tank and two squares away from it. */
    WORLD tankX = 0, tankY = 0;
    UT_ASSERT(serverSimGetTankState(sim, 2, &tankX, &tankY));

    lgm man = gs->lgmen[2];
    man->inTank = FALSE;
    man->isDead = FALSE;
    man->x = (WORLD)(tankX + (MAN_OFFSET_SQUARES << TANK_SHIFT_MAPSIZE));
    man->y = tankY;
    WORLD manX = man->x, manY = man->y;
    UT_ASSERT_MSG(manX != tankX,
                  "the man must not be standing on his own tank, or the two "
                  "outcomes are indistinguishable");

    /* A small tank explosion right on top of him, already at end of life so
     * the very next update takes the detonate-and-remove branch. */
    tkExplosionAddItem(gs, manX, manY, 0, TK_EXPLODE_DEATH,
                       TK_SMALL_EXPLOSION, /*creator*/ 2);
    UT_ASSERT_MSG(gs->tankExplosions != NULL, "explosion was not added");

    /* Driven through serverSimTick, NOT by calling tkExplosionUpdate directly:
     * defect 1 lives in the CALLER's choice of tank array, so a test that
     * builds its own arrays and calls the sweep directly cannot see it. The
     * sweep runs once per tick (it is inside simRunHalfStep's !isKeysTick
     * half), throttled to every TK_UPDATE_TIME-th call — prime the throttle so
     * this tick is the one that fires. */
    gs->tkExpUpdateTime = TK_UPDATE_TIME - 1;
    serverSimTick(sim);

    /* Defect 2: pre-fix the loop stopped before the last compacted index, so
     * this man was never even considered. */
    UT_ASSERT_MSG(man->isDead == TRUE,
                  "the last player's man survived a small tank explosion "
                  "standing on top of him — the sweep never reached his index");

    /* Defect 1: he must be sent back to HIS tank. Pre-fix, compacted index 1
     * paired him with raw slot 1 (empty), so lgmDeathCheckAtPosition took its
     * no-live-tank fallback and left him heading for where he died. */
    UT_ASSERT_MSG(man->destX == tankX && man->destY == tankY,
                  "killed man heads for (%u,%u); his tank is at (%u,%u) and he "
                  "died at (%u,%u) — wrong tank paired with his lgm",
                  (unsigned)man->destX, (unsigned)man->destY,
                  (unsigned)tankX, (unsigned)tankY,
                  (unsigned)manX, (unsigned)manY);

    /* The other player's man is untouched — the sweep killed exactly the man
     * who was standing in the blast. */
    UT_ASSERT_MSG(gs->lgmen[0] != NULL && gs->lgmen[0]->isDead == FALSE,
                  "slot 0's man died too, but the blast was nowhere near him");

    serverSimDestroy(sim);
    return 0;
}
