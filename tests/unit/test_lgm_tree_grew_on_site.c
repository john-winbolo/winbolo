/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A tree grows on the square while the man walks out to build there.
 *
 * lgmCheckNewRequest turns a wall, road or pillbox ordered on forest into a
 * harvest, but it runs only when the order is given, and tree growth takes no
 * notice of a man on his way. lgmDoWork re-checked the square on arrival for
 * buildings, water, pillboxes and bases, but not for forest, so he built over
 * the new tree: the wall or road replaced it, and a pillbox went up on it.
 *
 * Mac Bolo 0.99.7 re-checks the whole job when he arrives (checkvalidbuild,
 * called from buildpacket), and forest is not a square a wall, road or
 * pillbox may go on. He turns back with the job undone, bringing his trees
 * and his pillbox with him, and the tree stands.
 *
 * run_lgm_tree_grew_on_site  — the defect: for each of the three, the tree
 *                              survives and the tank gets back what the job
 *                              cost. Fails pre-fix, passes post.
 * run_lgm_tree_none_on_site  — the guard: the same errands with no tree
 *                              grown still build, and still spend. Passes
 *                              pre- and post-fix, and shows the man really
 *                              reaches the square in the case above.
 *
 * The tank is put on a grass square with the target grass square beside it,
 * so the walk is one square over open ground. The tree is grown with
 * mapSetPos once he is out, and the man is driven with lgmUpdate alone, so
 * nothing else on the map moves while he works.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "game_sim.h"              /* GameSim: mp/bs/pb/mns, tanks[], lgmen[] */
#include "lgm.h"                   /* lgmAddRequest, lgmUpdate, LGM_* enums and costs */
#include "bolo_map.h"              /* mapGetPos, mapSetPos and the terrain constants */
#include "pillbox.h"               /* pillsGetPill, pillsExistPos, pillsGetNumPills */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* tankSetWorld, tankTakePill, tankSetTrees */
#include "test_harness.h"

#define LTG_SLOT 0
#define LTG_PILL 1
/* lgmUpdate calls allowed for the walk out, the work and the walk back. */
#define LTG_MAX_UPDATES 2000

/* What the errand left behind, read once the man is back in the tank. */
typedef struct {
    BYTE terrain;     /* the target square */
    bool pillThere;   /* a pillbox stands on the target square */
    BYTE treesSpent;  /* the tank's trees before the order less after */
    bool pillBack;    /* the pillbox is on the tank's carry list again */
} ltg_result;

static bool ltg_open_grass(GameSim *gs, int x, int y) {
    return x >= 0 && x < 256 && y >= 0 && y < 256 &&
           mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == GRASS &&
           !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) &&
           !pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) &&
           !minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y);
}

/* Two open grass squares side by side: the tank's and the target. */
static bool ltg_find_pair(GameSim *gs, BYTE *ox, BYTE *oy) {
    int x, y;
    for (y = 1; y < 255; y++) {
        for (x = 1; x < 254; x++) {
            if (ltg_open_grass(gs, x, y) && ltg_open_grass(gs, x + 1, y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

static WORLD ltg_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* Send the man to build `action` on the square east of the tank, grow a tree
 * there behind him if `growTree`, and run him until he is back in the tank. */
static int ltg_errand(BYTE action, bool growTree, ltg_result *out) {
    ServerSim *sim = ut_make_running_sim("Builder");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = &sim->sim;
    tank *tnk = &gs->tanks[LTG_SLOT];
    lgm *man = &gs->lgmen[LTG_SLOT];
    BYTE tx = 0, ty = 0, bx, by;
    BYTE treesBefore;
    int i;

    UT_ASSERT_MSG(ltg_find_pair(gs, &tx, &ty),
                  "no two open grass squares side by side on the map");
    bx = (BYTE)(tx + 1);
    by = ty;
    tankSetWorld(gs, tnk, ltg_world(tx), ltg_world(ty), 0, false);
    tankSetOnBoat(tnk, FALSE);

    if (action == LGM_PILL_REQUEST) {
        pillbox p;
        UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= LTG_PILL,
                      "the map carries no pillboxes to hand the man");
        memset(&p, 0, sizeof(p));
        pillsGetPill(&gs->pb, &p, LTG_PILL);
        UT_ASSERT_MSG(p.inTank == FALSE,
                      "pill %u starts carried; this test needs one on the map",
                      (unsigned)LTG_PILL);
        tankTakePill(gs, tnk, LTG_PILL);
    }
    tankSetTrees(gs, tnk, (BYTE)TANK_FULL_TREES);
    treesBefore = tankGetTrees(tnk);

    lgmAddRequest(gs, man, tnk, bx, by, action);
    UT_ASSERT_MSG((*man)->inTank == FALSE && (*man)->state == LGM_STATE_GOING,
                  "order %u on grass (%u,%u) did not send the man out",
                  (unsigned)action, (unsigned)bx, (unsigned)by);
    UT_ASSERT_MSG((*man)->action == action,
                  "order %u went out as action %u",
                  (unsigned)action, (unsigned)(*man)->action);

    if (growTree) {
        mapSetPos(gs, &gs->mp, bx, by, FOREST, FALSE, FALSE);
    }

    for (i = 0; i < LTG_MAX_UPDATES; i++) {
        lgmUpdate(gs, man, tnk);
        if ((*man)->state == LGM_STATE_IDLE) {
            break;
        }
    }
    UT_ASSERT_MSG((*man)->state == LGM_STATE_IDLE,
                  "order %u: the man was not back in the tank after %d updates",
                  (unsigned)action, LTG_MAX_UPDATES);

    out->terrain = mapGetPos(&gs->mp, bx, by);
    out->pillThere = pillsExistPos(&gs->pb, bx, by);
    out->treesSpent = (BYTE)(treesBefore - tankGetTrees(tnk));
    out->pillBack = action == LGM_PILL_REQUEST &&
                    tankIsCarryingPill(tnk, LTG_PILL);

    serverSimDestroy(sim);
    return 0;
}

int run_lgm_tree_grew_on_site(void) {
    ltg_result r;
    int rc;

    rc = ltg_errand(LGM_BUILDING_REQUEST, true, &r);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(r.terrain == FOREST,
                  "wall: the tree that grew on the square became terrain %u; "
                  "the man should turn back and leave it standing",
                  (unsigned)r.terrain);
    UT_ASSERT_MSG(r.treesSpent == 0,
                  "wall: the tank is %u trees down after a job not done",
                  (unsigned)r.treesSpent);

    rc = ltg_errand(LGM_ROAD_REQUEST, true, &r);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(r.terrain == FOREST,
                  "road: the tree that grew on the square became terrain %u; "
                  "the man should turn back and leave it standing",
                  (unsigned)r.terrain);
    UT_ASSERT_MSG(r.treesSpent == 0,
                  "road: the tank is %u trees down after a job not done",
                  (unsigned)r.treesSpent);

    rc = ltg_errand(LGM_PILL_REQUEST, true, &r);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(!r.pillThere,
                  "pillbox: it was put down on the tree that grew there");
    UT_ASSERT_MSG(r.terrain == FOREST,
                  "pillbox: the tree that grew on the square became terrain %u",
                  (unsigned)r.terrain);
    UT_ASSERT_MSG(r.pillBack,
                  "pillbox: the man came back without it");
    UT_ASSERT_MSG(r.treesSpent == 0,
                  "pillbox: the tank is %u trees down after a job not done",
                  (unsigned)r.treesSpent);
    return 0;
}

int run_lgm_tree_none_on_site(void) {
    ltg_result r;
    int rc;

    rc = ltg_errand(LGM_BUILDING_REQUEST, false, &r);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(r.terrain == BUILDING,
                  "wall: the square is terrain %u, not a building",
                  (unsigned)r.terrain);
    UT_ASSERT_MSG(r.treesSpent == LGM_COST_BUILDING,
                  "wall: spent %u trees, expected %u",
                  (unsigned)r.treesSpent, (unsigned)LGM_COST_BUILDING);

    rc = ltg_errand(LGM_ROAD_REQUEST, false, &r);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(r.terrain == ROAD,
                  "road: the square is terrain %u, not a road",
                  (unsigned)r.terrain);
    UT_ASSERT_MSG(r.treesSpent == LGM_COST_ROAD,
                  "road: spent %u trees, expected %u",
                  (unsigned)r.treesSpent, (unsigned)LGM_COST_ROAD);

    rc = ltg_errand(LGM_PILL_REQUEST, false, &r);
    if (rc != 0) {
        return rc;
    }
    UT_ASSERT_MSG(r.pillThere,
                  "pillbox: none stands on the square");
    UT_ASSERT_MSG(!r.pillBack,
                  "pillbox: the tank still carries it after it was put down");
    UT_ASSERT_MSG(r.treesSpent == LGM_COST_PILLNEW,
                  "pillbox: spent %u trees, expected %u",
                  (unsigned)r.treesSpent, (unsigned)LGM_COST_PILLNEW);
    return 0;
}
