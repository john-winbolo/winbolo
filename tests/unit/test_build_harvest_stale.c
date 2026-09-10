/*
 * Stale build-order harvest replay.
 *
 * The server keeps a one-shot "harvest" slot so a build/mine action commanded
 * on a tick that got stall-substituted (never executed) is carried onto the
 * next real input rather than lost (serverSimDequeueFresh stores it, the game
 * arm of serverSimApplyOneInput folds it back in on the next buildAction==0
 * tick). The stored order freezes its type AND its target coordinates.
 *
 * Bug: the fold path re-dispatches the frozen order without re-checking the
 * target against the *current* map. By the time it replays, the tile's terrain
 * can no longer suit the order — so a stale tree-harvest lands on a tile that
 * is no longer forest and the player is nagged with "there is no tree to farm
 * there" (LGM_NO_TREE / ASSIST_MSG_NO_TREE) for a build they never just issued
 * (e.g. they have wall selected and are trying to build a wall). Toggling the
 * build selection in the UI appears to "fix" it only because the extra clicks
 * let the pending harvest drain.
 *
 * run_build_harvest_stale        — the defect: a harvested tree order replayed
 *                                  on a non-forest tile must NOT emit a bogus
 *                                  assistant error. Fails pre-fix, passes post.
 * run_build_harvest_valid        — the guard: a harvested order whose target is
 *                                  still valid (tree on real forest, wall on
 *                                  buildable grass) must STILL dispatch the
 *                                  correct request. Passes pre- and post-fix so
 *                                  the fix can't be "just drop every harvest".
 * run_build_harvest_busy_queues  — the other guard: a harvested order that
 *                                  arrives while the man is out is queued as his
 *                                  next order unchecked, the way a directly
 *                                  issued one is. The tank has no wood while the
 *                                  man is fetching it, so a re-check at replay
 *                                  time would wrongly throw the wall away; the
 *                                  check belongs to the moment he acts on it.
 *
 * Drives serverSimApplyInput + serverSimTick directly on ut_make_running_sim
 * (slot 0) and reads state off the ServerSim/GameSim structs (the unittests
 * profile permits internal access). serverSimTick runs two half-steps.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* pendingHarvestBuildAction, events[] etc. */
#include "game_sim.h"              /* GameSim: mp/bs/pb/mns, tanks[], lgmen[] */
#include "input_packet.h"          /* InputPacket, EVENT_ASSISTANT_MSG, ASSIST_* */
#include "lgm.h"                   /* struct lgmObj, LGM_* action/state enums */
#include "bolo_map.h"              /* mapGetPos, terrain constants via global.h */
#include "pillbox.h"               /* pillsExistPos */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* tankGiveTrees, tankSetTrees, tankGetMX/MY */
#include "test_harness.h"

#define BH_SLOT 0

/* Enqueue one input tick for slot 0 the way an arriving packet would. A
 * non-zero buildAction is 1-based on the wire (1=BsTrees .. 3=BsBuilding). */
static void bh_feed(ServerSim *sim, uint32_t tick, uint8_t buttons,
                    uint8_t buildAction, uint8_t bx, uint8_t by) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick        = tick;
    pkt.playerNum   = BH_SLOT;
    pkt.buttons     = buttons;
    pkt.buildAction = buildAction;
    pkt.buildX      = bx;
    pkt.buildY      = by;
    serverSimApplyInput(sim, &pkt);
}

/* Establish the stream: two fresh inputs per serverSimTick over six frames,
 * feeding ticks 1..12 so the jitter buffer fills without stalling. Leaves
 * lastProcessedInput == 12; returns the next unused tick (13). */
static uint32_t bh_establish(ServerSim *sim) {
    uint32_t t = 1;
    int frame;
    for (frame = 0; frame < 6; frame++) {
        bh_feed(sim, t++, 0, 0, 0, 0);
        bh_feed(sim, t++, 0, 0, 0, 0);
        serverSimTick(sim);
    }
    return t;
}

/* True if this running tick emitted the given assistant message id to slot 0.
 * serverSimTick clears eventCount at the top of each running tick, so read
 * this straight after the tick that should have produced (or suppressed) it. */
static bool bh_saw_assist(ServerSim *sim, uint8_t msgId) {
    uint8_t i;
    for (i = 0; i < sim->eventCount; i++) {
        if (sim->events[i].type == EVENT_ASSISTANT_MSG &&
            sim->events[i].data[1] == msgId) {
            return true;
        }
    }
    return false;
}

/* Find an interior map tile of the wanted terrain carrying no base/pill/mine
 * and not under the slot-0 tank. Returns false if none found. */
static bool bh_find_tile(ServerSim *sim, BYTE want, BYTE *ox, BYTE *oy) {
    BYTE tx = tankGetMX(&sim->sim.tanks[BH_SLOT]);
    BYTE ty = tankGetMY(&sim->sim.tanks[BH_SLOT]);
    int x, y;
    for (y = 2; y < 254; y++) {
        for (x = 2; x < 254; x++) {
            if ((BYTE)x == tx && (BYTE)y == ty) {
                continue;
            }
            if (mapGetPos(&sim->sim.mp, (BYTE)x, (BYTE)y) == want &&
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

/* Load a build order (1-based wire type at bx,by) into the pending-harvest
 * slot without executing it, exactly as a stall-dropped stale input would:
 * feed it at a tick already processed (staleTick <= lastProcessedInput) but
 * never acted on (staleTick > lastActionAppliedTick), so serverSimDequeueFresh
 * harvests rather than applies it. */
static int bh_stash(ServerSim *sim, uint32_t staleTick, uint8_t buildAction,
                    BYTE bx, BYTE by) {
    bh_feed(sim, staleTick, 0, buildAction, bx, by);
    serverSimTick(sim);                        /* harvested, not dispatched */

    UT_ASSERT_MSG(sim->pendingHarvestBuildAction[BH_SLOT] == buildAction,
                  "stale build should have been harvested (pending %u, want %u)",
                  sim->pendingHarvestBuildAction[BH_SLOT], buildAction);
    return 0;
}

/* Establish the stream and stash a build order, asserting the man is
 * idle+alive first so a later dispatch is a genuine primary request. Returns
 * the next fresh tick to fold on. */
static uint32_t bh_arm_harvest(ServerSim *sim, uint8_t buildAction,
                               BYTE bx, BYTE by) {
    uint32_t next = bh_establish(sim);  /* lastProcessedInput == 12, next == 13 */

    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->isDead == FALSE,
                  "precondition: builder should be alive");
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->action == LGM_IDLE,
                  "precondition: builder should be idle (was %u)",
                  sim->sim.lgmen[BH_SLOT]->action);

    if (bh_stash(sim, 10, buildAction, bx, by) != 0) {  /* 10 <= lpi 12 */
        return 0;
    }
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->action == LGM_IDLE,
                  "harvest must not dispatch the builder yet");
    return next;
}

/* Fold the pending harvest: two fresh buildAction==0 inputs so the game arm
 * runs the fold + dispatch this tick. Read events straight after. */
static void bh_fold(ServerSim *sim, uint32_t next) {
    bh_feed(sim, next,     0, 0, 0, 0);
    bh_feed(sim, next + 1, 0, 0, 0, 0);
    serverSimTick(sim);
}

/* The defect: a harvested TREE order (BsTrees == wire 1) replayed onto a tile
 * that is not forest must not nag the player with "no tree to farm there". */
int run_build_harvest_stale(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    BYTE gx = 0, gy = 0;
    UT_ASSERT_MSG(bh_find_tile(sim, GRASS, &gx, &gy),
                  "no plain grass tile found on the map");

    uint32_t next = bh_arm_harvest(sim, 1 /*BsTrees wire*/, gx, gy);
    bh_fold(sim, next);

    /* Pre-fix: the frozen tree order re-dispatches on grass and lgmCheckNewRequest
     * emits LGM_NO_TREE -> ASSIST_MSG_NO_TREE. Post-fix: the stale order is
     * dropped and no assistant message is produced. */
    UT_ASSERT_MSG(!bh_saw_assist(sim, ASSIST_MSG_NO_TREE),
                  "stale harvested tree order replayed a spurious 'no tree to "
                  "farm there' on a non-forest tile (%u,%u)", gx, gy);

    serverSimDestroy(sim);
    return 0;
}

/* The guard: harvested orders whose target is still valid must dispatch the
 * correct request — the fix drops stale/invalid replays, not every replay. */
int run_build_harvest_valid(void) {
    /* (a) Tree harvest on a genuine forest tile still runs. */
    {
        ServerSim *sim = ut_make_running_sim("Farmer");
        UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

        BYTE fx = 0, fy = 0;
        UT_ASSERT_MSG(bh_find_tile(sim, FOREST, &fx, &fy),
                      "no plain forest tile found on the map");

        uint32_t next = bh_arm_harvest(sim, 1 /*BsTrees wire*/, fx, fy);
        bh_fold(sim, next);

        UT_ASSERT_MSG(!bh_saw_assist(sim, ASSIST_MSG_NO_TREE),
                      "valid harvested tree order was wrongly rejected");
        UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->action == LGM_TREE_REQUEST,
                      "builder should be dispatched to harvest (action %u)",
                      sim->sim.lgmen[BH_SLOT]->action);
        UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->state == LGM_STATE_GOING,
                      "builder should be walking to the tree (state %u)",
                      sim->sim.lgmen[BH_SLOT]->state);

        serverSimDestroy(sim);
    }

    /* (b) Wall build on a buildable grass tile still runs — the actual
     * player-facing case: wall selected, correct request must go through. */
    {
        ServerSim *sim = ut_make_running_sim("Waller");
        UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

        BYTE gx = 0, gy = 0;
        UT_ASSERT_MSG(bh_find_tile(sim, GRASS, &gx, &gy),
                      "no plain grass tile found on the map");

        /* Give the tank wood so the wall isn't rejected for want of trees. */
        tankGiveTrees(&sim->sim, &sim->sim.tanks[BH_SLOT], LGM_COST_BUILDING * 4);

        uint32_t next = bh_arm_harvest(sim, 3 /*BsBuilding wire*/, gx, gy);
        bh_fold(sim, next);

        UT_ASSERT_MSG(!bh_saw_assist(sim, ASSIST_MSG_NO_BUILD),
                      "valid harvested wall order rejected with 'can't build here'");
        UT_ASSERT_MSG(!bh_saw_assist(sim, ASSIST_MSG_INSUFFICIENT_TREES),
                      "valid harvested wall order rejected for wood");
        UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->action == LGM_BUILDING_REQUEST,
                      "builder should be dispatched to build the wall (action %u)",
                      sim->sim.lgmen[BH_SLOT]->action);
        UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->state == LGM_STATE_GOING,
                      "builder should be walking to the wall site (state %u)",
                      sim->sim.lgmen[BH_SLOT]->state);

        serverSimDestroy(sim);
    }

    return 0;
}

/* The other guard: a harvested wall order that folds while the man is out
 * fetching wood is queued as his next order, not thrown away. The tank holds
 * no wood at that moment, so re-testing the order at replay time would refuse
 * it for want of trees; the test the player expects is the one made when the
 * man gets back in the tank, with the wood he brought. */
int run_build_harvest_busy_queues(void) {
    ServerSim *sim = ut_make_running_sim("Hauler");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    BYTE fx = 0, fy = 0, gx = 0, gy = 0;
    UT_ASSERT_MSG(bh_find_tile(sim, FOREST, &fx, &fy),
                  "no plain forest tile found on the map");
    UT_ASSERT_MSG(bh_find_tile(sim, GRASS, &gx, &gy),
                  "no plain grass tile found on the map");

    uint32_t next = bh_establish(sim);  /* lastProcessedInput == 12, next == 13 */
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->action == LGM_IDLE,
                  "precondition: builder should be idle (was %u)",
                  sim->sim.lgmen[BH_SLOT]->action);

    /* No wood, and the man sent to fetch some. The tree order rides the game
     * tick (even parity); the odd one is a keys tick and would not act. */
    tankSetTrees(&sim->sim.tanks[BH_SLOT], 0);
    bh_feed(sim, next,     0, 0, 0, 0);
    bh_feed(sim, next + 1, 0, 1 /*BsTrees wire*/, fx, fy);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->action == LGM_TREE_REQUEST,
                  "builder should be out fetching wood (action %u)",
                  sim->sim.lgmen[BH_SLOT]->action);
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->nextAction == LGM_IDLE,
                  "nothing should be queued yet (next %u)",
                  sim->sim.lgmen[BH_SLOT]->nextAction);

    /* Two blank ticks so the action marker (tick 14) falls behind the
     * processed mark (16), leaving tick 16 both stale and unexecuted. */
    bh_feed(sim, next + 2, 0, 0, 0, 0);
    bh_feed(sim, next + 3, 0, 0, 0, 0);
    serverSimTick(sim);

    /* The wall order the player clicked while the man was away, lost to a
     * stall and stashed. */
    if (bh_stash(sim, next + 3, 3 /*BsBuilding wire*/, gx, gy) != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    bh_fold(sim, next + 4);

    UT_ASSERT_MSG(!bh_saw_assist(sim, ASSIST_MSG_INSUFFICIENT_TREES),
                  "a wall queued behind a wood run was refused for wood at "
                  "replay time");
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->action == LGM_TREE_REQUEST,
                  "the wood run should still be in progress (action %u)",
                  sim->sim.lgmen[BH_SLOT]->action);
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->nextAction == LGM_BUILDING_REQUEST,
                  "the wall should be queued as the man's next order "
                  "(next %u)", sim->sim.lgmen[BH_SLOT]->nextAction);
    UT_ASSERT_MSG(sim->sim.lgmen[BH_SLOT]->nextX == gx &&
                  sim->sim.lgmen[BH_SLOT]->nextY == gy,
                  "queued wall should keep its target (%u,%u), got (%u,%u)",
                  gx, gy, sim->sim.lgmen[BH_SLOT]->nextX,
                  sim->sim.lgmen[BH_SLOT]->nextY);

    serverSimDestroy(sim);
    return 0;
}
