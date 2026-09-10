/*
 * Build-request validity.
 *
 * lgmCheckNewRequest (lgm.c) is the one place that decides whether a build
 * order can go ahead: the terrain under the target, what the tank is
 * carrying, and whether the target already holds a visible mine. Two kinds
 * of caller ask it. lgmAddRequest asks while acting on a click, so it spends
 * the tank's stores and tells the player when the answer is no. The server's
 * replay of a stale order asks through lgmRequestIsValid, which wants the
 * verdict only — no dispatch, no spending, and no message, because the
 * player did not just issue that order.
 *
 * run_lgm_request_valid  — the verdicts, over terrain and tank stores.
 * run_lgm_request_quiet  — asking the question must not message the player
 *                          and must not spend the tank's stores, while the
 *                          acting path on the same order still does message.
 *
 * Calls lgmRequestIsValid directly on ut_make_running_sim (slot 0) and reads
 * events off the ServerSim struct; the unittests profile permits internal
 * access.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* events[] / eventCount */
#include "game_sim.h"              /* GameSim: mp/bs/pb/mns, tanks[], lgmen[] */
#include "input_packet.h"          /* EVENT_ASSISTANT_MSG, ASSIST_* */
#include "lgm.h"                   /* lgmRequestIsValid, LGM_* enums and costs */
#include "bolo_map.h"              /* mapGetPos, terrain constants via global.h */
#include "pillbox.h"               /* pillsExistPos */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* tankSetTrees / tankSetMines / tankGetMX */
#include "test_harness.h"

#define LRV_SLOT 0

/* Find a map tile of the wanted terrain carrying no base/pill/mine and not
 * under the slot-0 tank. Returns false if the map has none. */
static bool lrv_find_tile(ServerSim *sim, BYTE want, BYTE *ox, BYTE *oy) {
    BYTE tx = tankGetMX(&sim->sim.tanks[LRV_SLOT]);
    BYTE ty = tankGetMY(&sim->sim.tanks[LRV_SLOT]);
    int x, y;
    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
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

/* True if slot 0 has been sent the given assistant message id. Nothing here
 * runs a tick, so events accumulate across a whole test. */
static bool lrv_saw_assist(ServerSim *sim, uint8_t msgId) {
    uint8_t i;
    for (i = 0; i < sim->eventCount; i++) {
        if (sim->events[i].type == EVENT_ASSISTANT_MSG &&
            sim->events[i].data[1] == msgId) {
            return true;
        }
    }
    return false;
}

static bool lrv_ask(ServerSim *sim, BYTE action, BYTE x, BYTE y) {
    return lgmRequestIsValid(&sim->sim, &sim->sim.lgmen[LRV_SLOT],
                             &sim->sim.tanks[LRV_SLOT], x, y, action);
}

/* The verdicts. Each case sets the tank's stores explicitly so the answer
 * turns on the thing being tested rather than on what a fresh tank carries. */
int run_lgm_request_valid(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    tank *tnk = &sim->sim.tanks[LRV_SLOT];
    BYTE fx = 0, fy = 0, gx = 0, gy = 0, sx = 0, sy = 0;

    UT_ASSERT_MSG(lrv_find_tile(sim, FOREST, &fx, &fy),
                  "no plain forest tile found on the map");
    UT_ASSERT_MSG(lrv_find_tile(sim, GRASS, &gx, &gy),
                  "no plain grass tile found on the map");

    /* Trees: forest yes, grass no. */
    UT_ASSERT_MSG(lrv_ask(sim, LGM_TREE_REQUEST, fx, fy),
                  "tree harvest on forest (%u,%u) should be valid", fx, fy);
    UT_ASSERT_MSG(!lrv_ask(sim, LGM_TREE_REQUEST, gx, gy),
                  "tree harvest on grass (%u,%u) should be invalid", gx, gy);

    /* A road order on forest is the game's substitution to a tree harvest,
     * so it is still a request that can go ahead. */
    tankSetTrees(tnk, 0);
    UT_ASSERT_MSG(lrv_ask(sim, LGM_ROAD_REQUEST, fx, fy),
                  "road order on forest (%u,%u) should be valid as a harvest",
                  fx, fy);

    /* Walls turn on the wood the tank is carrying. */
    tankSetTrees(tnk, 0);
    UT_ASSERT_MSG(!lrv_ask(sim, LGM_BUILDING_REQUEST, gx, gy),
                  "wall with no wood should be invalid");
    tankSetTrees(tnk, LGM_COST_BUILDING);
    UT_ASSERT_MSG(lrv_ask(sim, LGM_BUILDING_REQUEST, gx, gy),
                  "wall on grass (%u,%u) with wood should be valid", gx, gy);

    /* Mines turn on the mines the tank is carrying. */
    tankSetMines(tnk, 0);
    UT_ASSERT_MSG(!lrv_ask(sim, LGM_MINE_REQUEST, gx, gy),
                  "mine order with no mines should be invalid");
    tankSetMines(tnk, LGM_COST_MINE);
    UT_ASSERT_MSG(lrv_ask(sim, LGM_MINE_REQUEST, gx, gy),
                  "mine order on grass (%u,%u) with mines should be valid",
                  gx, gy);

    /* Nothing can be built on the tank's own square. */
    tankSetTrees(tnk, LGM_COST_BUILDING * 4);
    {
        BYTE tx = tankGetMX(tnk);
        BYTE ty = tankGetMY(tnk);
        BYTE under = mapGetPos(&sim->sim.mp, tx, ty);
        if (under != FOREST && under != RIVER) {
            UT_ASSERT_MSG(!lrv_ask(sim, LGM_BUILDING_REQUEST, tx, ty),
                          "wall on the tank's own square (%u,%u) should be "
                          "invalid", tx, ty);
        }
    }

    /* Deep sea takes nothing. */
    if (lrv_find_tile(sim, DEEP_SEA, &sx, &sy)) {
        UT_ASSERT_MSG(!lrv_ask(sim, LGM_BUILDING_REQUEST, sx, sy),
                      "wall on deep sea (%u,%u) should be invalid", sx, sy);
        UT_ASSERT_MSG(!lrv_ask(sim, LGM_MINE_REQUEST, sx, sy),
                      "mine on deep sea (%u,%u) should be invalid", sx, sy);
    }

    serverSimDestroy(sim);
    return 0;
}

/* Asking must stay silent and must not spend anything, while the acting path
 * on the same order still tells the player why it was refused. */
int run_lgm_request_quiet(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    tank *tnk = &sim->sim.tanks[LRV_SLOT];
    BYTE gx = 0, gy = 0;
    UT_ASSERT_MSG(lrv_find_tile(sim, GRASS, &gx, &gy),
                  "no plain grass tile found on the map");

    UT_ASSERT_MSG(sim->sim.lgmen[LRV_SLOT]->isDead == FALSE,
                  "precondition: builder should be alive");
    UT_ASSERT_MSG(sim->sim.lgmen[LRV_SLOT]->action == LGM_IDLE,
                  "precondition: builder should be idle (was %u)",
                  sim->sim.lgmen[LRV_SLOT]->action);

    /* An order that will be refused, asked about rather than issued. */
    uint8_t before = sim->eventCount;
    UT_ASSERT_MSG(!lrv_ask(sim, LGM_TREE_REQUEST, gx, gy),
                  "tree harvest on grass should be invalid");
    UT_ASSERT_MSG(sim->eventCount == before,
                  "asking whether a request is valid emitted %u event(s); it "
                  "must not message the player",
                  (unsigned)(sim->eventCount - before));

    /* The same order issued for real still tells the player. */
    lgmAddRequest(&sim->sim, &sim->sim.lgmen[LRV_SLOT], tnk, gx, gy,
                  LGM_TREE_REQUEST);
    UT_ASSERT_MSG(lrv_saw_assist(sim, ASSIST_MSG_NO_TREE),
                  "issuing the same order should still send 'no tree to farm "
                  "there'");
    UT_ASSERT_MSG(sim->sim.lgmen[LRV_SLOT]->action == LGM_IDLE,
                  "a refused order must not dispatch the builder (action %u)",
                  sim->sim.lgmen[LRV_SLOT]->action);

    /* Asking about an order the tank can afford must not spend the stores —
     * the answer has to stay the same however many times it is asked. */
    tankSetTrees(tnk, LGM_COST_BUILDING);
    tankSetMines(tnk, LGM_COST_MINE);
    UT_ASSERT_MSG(lrv_ask(sim, LGM_BUILDING_REQUEST, gx, gy),
                  "wall with exactly enough wood should be valid");
    UT_ASSERT_MSG(tankGetTrees(tnk) == LGM_COST_BUILDING,
                  "asking spent wood: %u left, expected %u",
                  tankGetTrees(tnk), LGM_COST_BUILDING);
    UT_ASSERT_MSG(lrv_ask(sim, LGM_BUILDING_REQUEST, gx, gy),
                  "second ask about the same wall should give the same answer");

    UT_ASSERT_MSG(lrv_ask(sim, LGM_MINE_REQUEST, gx, gy),
                  "mine order with exactly enough mines should be valid");
    UT_ASSERT_MSG(tankGetMines(tnk) == LGM_COST_MINE,
                  "asking spent mines: %u left, expected %u",
                  tankGetMines(tnk), LGM_COST_MINE);

    serverSimDestroy(sim);
    return 0;
}
