/*
 * Tree-growth water regressions (test_treegrow_no_sea.c).
 *
 * Trees were appearing on open deep sea at a "random" interior
 * coordinate after a map change. Two defects combine to produce it:
 *
 * 1. serverSimResetGameWorld rebuilds every other terrain subsystem
 *    (grass, swamp, rubble, flood, mines...) and reloads the map, but
 *    leaves treeGrowX/treeGrowY/treeGrowScore/treeGrowTime untouched.
 *    The converged grow target from the previous map carries over; on
 *    the new map that coordinate is open sea.
 *
 * 2. treeGrowCheckGrowTree's grow gate only rejects RIVER/BUILDING/
 *    HALFBUILDING — not DEEP_SEA (or BOAT) — so when the stale target
 *    fires, forest is planted on water.
 *
 * The first test pins the grow gate directly; the second pins the
 * reset.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "game_sim.h"
#include "bolo_map.h"             /* mapGetPos / mapSetPos */
#include "pillbox.h"              /* pillsExistPos */
#include "bases.h"                /* basesExistPos */
#include "treegrow.h"             /* treeGrowCheckGrowTree, TREEGROW_INITIAL_* */
#include "server_sim.h"           /* serverSimGetGameSim, serverSimResetGameWorld */
#include "test_harness.h"

/* Find an in-bounds open-water tile (DEEP_SEA, no pill, no base) on the
 * loaded map. Returns TRUE and writes the coordinate on success. The
 * scan stays strictly inside the mine-edge window so mapGetPos reports
 * the stored terrain rather than the out-of-range DEEP_SEA default. */
static bool find_open_sea(GameSim *gs, BYTE *outX, BYTE *outY) {
    int x, y;
    for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == DEEP_SEA &&
                pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) == FALSE &&
                basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) == FALSE) {
                *outX = (BYTE)x;
                *outY = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* ---- 1. The grow gate must never convert a deep-sea tile to forest. ---- */
int run_treegrow_never_plants_on_deep_sea(void) {
    ServerSim *sim = ut_make_running_sim("Elvis3");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    BYTE sx = 0, sy = 0;
    UT_ASSERT_MSG(find_open_sea(gs, &sx, &sy) == true,
                  "expected an in-bounds open-sea tile on Everard Island");

    /* Aim the grow at the sea tile with the timer about to expire — the
     * exact state a stale cross-map target leaves behind. */
    gs->treeGrowX = sx;
    gs->treeGrowY = sy;
    gs->treeGrowScore = 500;   /* a "converged" positive score */
    gs->treeGrowTime = 1;      /* decremented to 0 -> grow fires this call */

    treeGrowCheckGrowTree(gs);

    UT_ASSERT_MSG(mapGetPos(&gs->mp, sx, sy) == DEEP_SEA,
                  "tree must not grow on deep sea: tile (%u,%u) became %u",
                  (unsigned)sx, (unsigned)sy,
                  (unsigned)mapGetPos(&gs->mp, sx, sy));

    serverSimDestroy(sim);
    return 0;
}

/* ---- 2. A world reset must clear the carried-over grow target. ---- */
int run_treegrow_reset_clears_stale_target(void) {
    ServerSim *sim = ut_make_running_sim("Elvis3");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    /* Simulate a hunt that converged on a land tile late in the prior
     * round, with the grow imminent. */
    gs->treeGrowX = 80;
    gs->treeGrowY = 100;
    gs->treeGrowScore = 500;
    gs->treeGrowTime = 5;

    serverSimResetGameWorld(sim);
    gs = serverSimGetGameSim(sim);

    /* After the reset the grow state must be back to its just-created
     * values, so a coordinate that was land on the old map can't fire
     * on the new map. */
    UT_ASSERT_MSG(gs->treeGrowScore == TREEGROW_INITIAL_SCORE,
                  "treeGrowScore must reset to %d, got %d",
                  TREEGROW_INITIAL_SCORE, gs->treeGrowScore);
    UT_ASSERT_MSG(gs->treeGrowTime == TREEGROW_INITIAL_TIME,
                  "treeGrowTime must reset to %d, got %d",
                  TREEGROW_INITIAL_TIME, gs->treeGrowTime);

    serverSimDestroy(sim);
    return 0;
}
