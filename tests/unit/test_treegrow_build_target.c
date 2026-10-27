/*
 * Tree growth and the man's build orders (test_treegrow_build_target.c).
 *
 * A tree could come up on the square the man had been sent to build on
 * while he was walking out to it. He then built anyway, putting the road,
 * building or pillbox down as if the tree weren't there. The grow is now
 * skipped while a man is on his way to build on its square, so the tree
 * never appears.
 *
 * The first test pins the skip for each kind of build order. The second
 * pins that the skip goes no further: a man going there to farm, or going
 * somewhere else, leaves the tree to grow as before.
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
#include "lgm.h"                  /* lgmCreate, LGM_*_REQUEST, LGM_STATE_* */
#include "treegrow.h"             /* treeGrowCheckGrowTree */
#include "server_sim.h"           /* serverSimGetGameSim */
#include "test_harness.h"

/* Find an in-bounds GRASS tile with no pill or base on it. */
static bool find_grass(GameSim *gs, BYTE *outX, BYTE *outY) {
    int x, y;
    for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == GRASS &&
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

/* Find an lgmen[] slot nobody is using. */
static int find_free_man_slot(GameSim *gs) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (gs->lgmen[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/* Send a man out on `action` to (destX, destY), aim the grow at (x, y)
 * with the timer about to run out, fire it, and report whether (x, y)
 * turned to forest. The man is taken out of the sim again afterwards. */
static bool grows_with_man_out(GameSim *gs, BYTE x, BYTE y, BYTE action,
                               BYTE state, BYTE destX, BYTE destY) {
    int slot = find_free_man_slot(gs);
    lgm man;
    bool grew;

    if (slot < 0) {
        return false;
    }
    man = lgmCreate((BYTE)slot);
    man->state = state;
    man->action = action;
    man->inTank = FALSE;
    man->destX = (WORLD)((destX << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    man->destY = (WORLD)((destY << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    gs->lgmen[slot] = man;

    mapSetPos(gs, &gs->mp, x, y, GRASS, FALSE, FALSE);
    gs->treeGrowX = x;
    gs->treeGrowY = y;
    gs->treeGrowScore = 500;
    gs->treeGrowTime = 1;   /* decremented to 0 -> grow fires this call */

    treeGrowCheckGrowTree(gs);
    grew = (mapGetPos(&gs->mp, x, y) == FOREST);

    lgmDestroy(&gs->lgmen[slot]);
    gs->lgmen[slot] = NULL;
    return grew;
}

/* ---- 1. No tree under a road, building or pillbox the man is going to put down. ---- */
int run_treegrow_skips_square_man_is_building_on(void) {
    static const struct {
        BYTE action;
        const char *name;
    } orders[] = {
        { LGM_ROAD_REQUEST,     "road"     },
        { LGM_BUILDING_REQUEST, "building" },
        { LGM_PILL_REQUEST,     "pillbox"  },
    };
    size_t i;

    ServerSim *sim = ut_make_running_sim("Woodsman");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    BYTE gx = 0, gy = 0;
    UT_ASSERT_MSG(find_grass(gs, &gx, &gy) == true,
                  "expected an in-bounds grass tile on the test map");
    UT_ASSERT_MSG(find_free_man_slot(gs) >= 0, "expected a free lgmen[] slot");

    for (i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        UT_ASSERT_MSG(grows_with_man_out(gs, gx, gy, orders[i].action,
                                         LGM_STATE_GOING, gx, gy) == false,
                      "a tree grew on (%u,%u) while the man was going there "
                      "to build a %s", (unsigned)gx, (unsigned)gy,
                      orders[i].name);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ---- 2. Anything else leaves the tree to grow. ---- */
int run_treegrow_still_grows_when_man_not_building_there(void) {
    ServerSim *sim = ut_make_running_sim("Woodsman");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    BYTE gx = 0, gy = 0;
    UT_ASSERT_MSG(find_grass(gs, &gx, &gy) == true,
                  "expected an in-bounds grass tile on the test map");
    UT_ASSERT_MSG(find_free_man_slot(gs) >= 0, "expected a free lgmen[] slot");

    UT_ASSERT_MSG(grows_with_man_out(gs, gx, gy, LGM_BUILDING_REQUEST,
                                     LGM_STATE_GOING,
                                     (BYTE)(gx + 1), gy) == true,
                  "a man building on the next square stopped the tree");
    UT_ASSERT_MSG(grows_with_man_out(gs, gx, gy, LGM_TREE_REQUEST,
                                     LGM_STATE_GOING, gx, gy) == true,
                  "a man going to farm the square stopped the tree");
    UT_ASSERT_MSG(grows_with_man_out(gs, gx, gy, LGM_BUILDING_REQUEST,
                                     LGM_STATE_RETURN, gx, gy) == true,
                  "a man walking back from the square stopped the tree");

    serverSimDestroy(sim);
    return 0;
}
