/*
 * The five builder ops a scenario writes the world with.
 *
 * Each arm validates, mutates through the same internal function a player's
 * own action reaches, and leaves the publish and the record to that function.
 * So each case here asks three things: does every refusal in the op's
 * contract come back with its own code, does the mutation land on the sim,
 * and does what the arm publishes reach the event queue.
 *
 * The dispatch cases carry the weight, because dispatch is the arm that must
 * not be a second copy of the build rules: one order that costs wood proves
 * the cost was taken, one refused on the square and one refused for want of
 * wood prove the two answers come from the engine's own validator, and none
 * of the refusals may send the player an assistant line, because the player
 * did not ask for anything.
 *
 * run_scenario_lgm_dispatch     — the refusals, the order landing, and the
 *                                 wood the order cost
 * run_scenario_lgm_recall       — turned round, with the queued order dropped
 * run_scenario_lgm_kill         — the newswire event, who is credited, and
 *                                 the WinBolo.net reports
 * run_scenario_lgm_parachute    — a flight aimed at a square or at the tank
 * run_scenario_lgm_set_carried  — what he carries, and the caps
 * run_scenario_lgm_kill_record  — log_LostMan survives a real recording
 *
 * Drives serverSimApplyScenarioOp at ut_make_running_sim and reads the
 * ServerSim struct directly; the unittests profile permits internal access.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, playerConnected[], events[] */
#include "server_sim_scenario.h"
#include "game_sim.h"              /* GameSim: mp/pb/bs, tanks[], lgmen[] */
#include "bolo_map.h"              /* mapGetPos */
#include "pillbox.h"               /* pillsExistPos */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* the tank stores the orders spend */
#include "lgm.h"                   /* the builder state the arms write */
#include "gametype.h"              /* TANK_FULL_TREES / TANK_FULL_MINES */
#include "log.h"                   /* log_LostMan and the stream opcodes */
#include "input_packet.h"          /* EVENT_LGM_LOST, EVENT_ASSISTANT_MSG */
#include "replay_harness.h"
#include "test_harness.h"

/* The winbolonetAddEvent builder spies in test_stubs.c. */
extern int  wbnStubLgmLostCalls;
extern BYTE wbnStubLastLgmLost;
extern int  wbnStubLgmKillCalls;
extern BYTE wbnStubLastLgmKiller;
extern BYTE wbnStubLastLgmKilled;

#define BA_SLOT  0
#define BA_OTHER 1

/* ── Small helpers ───────────────────────────────────────────────── */

static lgm *baMan(ServerSim *sim, BYTE slot) {
    return &sim->sim.lgmen[slot];
}

static tank *baTank(ServerSim *sim, BYTE slot) {
    return &sim->sim.tanks[slot];
}

static void baDrainEvents(ServerSim *sim) {
    sim->eventCount = 0;
}

static int baCountEvents(ServerSim *sim, BYTE type) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    int found = 0;
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type) found++;
    }
    return found;
}

static const GameEvent *baFindEvent(ServerSim *sim, BYTE type) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type) return &evs[i];
    }
    return NULL;
}

/* A square inside the minable area holding nothing but the wanted terrain and
   not under either tank, so an order aimed at it turns on the terrain rather
   than on what happens to be standing there. */
static bool baFindTile(ServerSim *sim, BYTE want, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    BYTE ax = tankGetMX(baTank(sim, BA_SLOT));
    BYTE ay = tankGetMY(baTank(sim, BA_SLOT));
    BYTE bx = 0xFF, by = 0xFF;
    int x, y;
    if (gs->tanks[BA_OTHER] != NULL) {
        bx = tankGetMX(baTank(sim, BA_OTHER));
        by = tankGetMY(baTank(sim, BA_OTHER));
    }
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            if ((BYTE)x == ax && (BYTE)y == ay) continue;
            if ((BYTE)x == bx && (BYTE)y == by) continue;
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == want &&
                !pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) &&
                !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) &&
                !minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* Send a builder out on a wall, which is the cheapest way to get one out of
   its tank through the op itself. Returns what the op answered. */
static ScnOpResult baSendOut(ServerSim *sim, BYTE slot, BYTE x, BYTE y) {
    ScenarioOp op;
    tankSetTrees(&sim->sim, baTank(sim, slot), TANK_FULL_TREES);
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_DISPATCH;
    op.u.lgmDispatch.slot   = slot;
    op.u.lgmDispatch.action = (BYTE)builderJobBuilding;
    op.u.lgmDispatch.x      = x;
    op.u.lgmDispatch.y      = y;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* Kill a builder who is already out, so a later arm has a dead one to work
   on. Returns what the op answered. */
static ScnOpResult baKill(ServerSim *sim, BYTE slot, BYTE killer) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_KILL;
    op.u.lgmKill.slot   = slot;
    op.u.lgmKill.killer = killer;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* ── Dispatch ────────────────────────────────────────────────────── */

int run_scenario_lgm_dispatch(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    ScenarioOp op;
    lgm *l;
    tank *t;
    BYTE gx = 0, gy = 0;

    UT_ASSERT(sim != NULL);
    l = baMan(sim, BA_SLOT);
    t = baTank(sim, BA_SLOT);
    UT_ASSERT(*l != NULL && *t != NULL);
    UT_ASSERT_MSG(baFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    baDrainEvents(sim);
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_DISPATCH;
    op.u.lgmDispatch.slot   = BA_SLOT;
    op.u.lgmDispatch.action = (BYTE)builderJobBuilding;
    op.u.lgmDispatch.x      = gx;
    op.u.lgmDispatch.y      = gy;

    /* An empty seat, and an index past the roster. */
    op.u.lgmDispatch.slot = BA_OTHER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmDispatch.slot = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmDispatch.slot = BA_SLOT;

    /* No job at all, and a value that is no job either. */
    op.u.lgmDispatch.action = (BYTE)builderJobNone;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.lgmDispatch.action = 200;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.lgmDispatch.action = (BYTE)builderJobBuilding;

    /* The mine border is not a square anyone builds on. */
    op.u.lgmDispatch.x = MAP_MINE_EDGE_LEFT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.lgmDispatch.x = gx;
    op.u.lgmDispatch.y = MAP_MINE_EDGE_BOTTOM;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.lgmDispatch.y = gy;

    /* The square will not take the job: there is no tree on grass to farm.
       This answer comes from the engine's own validator, not from a copy of
       its rules living in the arm. */
    tankSetTrees(&sim->sim, t, TANK_FULL_TREES);
    op.u.lgmDispatch.action = (BYTE)builderJobTrees;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                  "a tree harvest on grass should be refused on the square");

    /* The square is fine and the tank cannot pay. Same validator, other
       answer, which is what proves the two are told apart. Asking is a dry
       run, so the wood it could not spend is all still there. */
    tankSetTrees(&sim->sim, t, (BYTE)(LGM_COST_BUILDING - 1));
    op.u.lgmDispatch.action = (BYTE)builderJobBuilding;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_STOCK,
                  "a wall with too little wood should be refused for stock");
    UT_ASSERT_MSG(tankGetTrees(t) == LGM_COST_BUILDING - 1,
                  "a refused order left %u wood, wanted %u",
                  (unsigned)tankGetTrees(t), (unsigned)(LGM_COST_BUILDING - 1));

    /* None of that was a click, so none of it may talk to the player. */
    UT_ASSERT_MSG(baCountEvents(sim, EVENT_ASSISTANT_MSG) == 0,
                  "a refused op sent the player %d assistant line(s)",
                  baCountEvents(sim, EVENT_ASSISTANT_MSG));
    UT_ASSERT_MSG((*l)->inTank, "a refused order still sent the man out");

    /* The order lands: the man leaves the tank on the job he was given, and
       the wood it costs has come out of the tank and gone with him. */
    tankSetTrees(&sim->sim, t, LGM_COST_BUILDING);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(!(*l)->inTank, "the man should have left the tank");
    UT_ASSERT_MSG((*l)->state == LGM_STATE_GOING, "state %u",
                  (unsigned)(*l)->state);
    UT_ASSERT_MSG((*l)->action == LGM_BUILDING_REQUEST, "action %u",
                  (unsigned)(*l)->action);
    UT_ASSERT_MSG(tankGetTrees(t) == 0, "the wall cost no wood: %u left",
                  (unsigned)tankGetTrees(t));
    UT_ASSERT_MSG((*l)->numTrees == LGM_COST_BUILDING,
                  "the man carries %u trees, wanted %u",
                  (unsigned)(*l)->numTrees, (unsigned)LGM_COST_BUILDING);
    UT_ASSERT_MSG(baCountEvents(sim, EVENT_ASSISTANT_MSG) == 0,
                  "an accepted op still talked to the player");

    /* A second order while he is out is the one a click would hold until he
       is back. The op says so rather than answering as though it started. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);

    /* A destroyed tank is answered before that, since there is no builder to
       send anywhere without one. */
    tankSetDestroyed(t, true);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TANK_DEAD);
    tankSetDestroyed(t, false);

    /* Not while the round is not running. */
    sim->state = serverStateLobby;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    sim->state = serverStateRunning;

    serverSimDestroy(sim);
    return 0;
}

/* ── Recall ──────────────────────────────────────────────────────── */

int run_scenario_lgm_recall(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    ScenarioOp op;
    lgm *l;
    BYTE gx = 0, gy = 0;

    UT_ASSERT(sim != NULL);
    l = baMan(sim, BA_SLOT);
    UT_ASSERT(*l != NULL);
    UT_ASSERT_MSG(baFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_RECALL;
    op.u.lgmRecall.slot = BA_SLOT;

    /* An empty seat, and an index past the roster. */
    op.u.lgmRecall.slot = BA_OTHER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmRecall.slot = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmRecall.slot = BA_SLOT;

    /* Nobody to call back while he is sitting in the tank. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);

    /* Out on a wall, with a second order waiting behind it. */
    UT_ASSERT(baSendOut(sim, BA_SLOT, gx, gy) == SCN_OP_OK);
    (*l)->nextAction = LGM_MINE_REQUEST;
    (*l)->nextX = gx;
    (*l)->nextY = gy;

    baDrainEvents(sim);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG((*l)->state == LGM_STATE_RETURN, "state %u",
                  (unsigned)(*l)->state);
    UT_ASSERT_MSG((*l)->nextAction == LGM_IDLE,
                  "the queued order survived the recall: %u",
                  (unsigned)(*l)->nextAction);
    UT_ASSERT_MSG(!(*l)->inTank, "a recall should not teleport him home");
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 0,
                  "a recall published %u event(s); it should publish none",
                  (unsigned)serverSimGetEventCount(sim));

    /* Calling him back twice is harmless, and calling a dead one back is not
       something to answer yes to. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(baKill(sim, BA_SLOT, SCN_NONE) == SCN_OP_OK);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);

    /* Not while the round is not running. */
    sim->state = serverStateLobby;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    sim->state = serverStateRunning;

    serverSimDestroy(sim);
    return 0;
}

/* ── Kill ────────────────────────────────────────────────────────── */

int run_scenario_lgm_kill(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    ScenarioOp op;
    const GameEvent *ev;
    lgm *l;
    BYTE gx = 0, gy = 0;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, BA_OTHER, "Other", false);
    l = baMan(sim, BA_SLOT);
    UT_ASSERT(*l != NULL && sim->sim.lgmen[BA_OTHER] != NULL);
    UT_ASSERT_MSG(baFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_KILL;
    op.u.lgmKill.slot   = BA_SLOT;
    op.u.lgmKill.killer = BA_OTHER;

    /* An empty seat, and an index past the roster. */
    op.u.lgmKill.slot = 5;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmKill.slot = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmKill.slot = BA_SLOT;

    /* A man in his tank is out of reach, as he is to an explosion. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);

    UT_ASSERT(baSendOut(sim, BA_SLOT, gx, gy) == SCN_OP_OK);

    /* A killer that is not a player is refused, and nothing is published or
       reported — the builder-lost report fires on every death with no test of
       its own, so this is the only thing keeping a bogus slot out of it. */
    baDrainEvents(sim);
    wbnStubLgmLostCalls = 0;
    wbnStubLgmKillCalls = 0;
    op.u.lgmKill.killer = 7;           /* an empty seat */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmKill.killer = MAX_TANKS;   /* past the roster */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    UT_ASSERT_MSG(!(*l)->isDead, "a refused kill still killed the man");
    UT_ASSERT_MSG(baCountEvents(sim, EVENT_LGM_LOST) == 0,
                  "a refused kill still published");
    UT_ASSERT_MSG(wbnStubLgmLostCalls == 0 && wbnStubLgmKillCalls == 0,
                  "a refused kill still reported");

    /* An attributed kill: the named slot is credited, on the event and to
       WinBolo.net, and the man is flying back in with nothing in his hands. */
    op.u.lgmKill.killer = BA_OTHER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG((*l)->isDead, "the man should be dead");
    UT_ASSERT_MSG((*l)->frame == LGM_HELICOPTER_FRAME, "frame %u",
                  (unsigned)(*l)->frame);
    UT_ASSERT_MSG((*l)->numTrees == 0 && (*l)->numMines == 0,
                  "he kept %u trees and %u mines", (unsigned)(*l)->numTrees,
                  (unsigned)(*l)->numMines);

    ev = baFindEvent(sim, EVENT_LGM_LOST);
    UT_ASSERT_MSG(ev != NULL, "no builder-lost event was published");
    UT_ASSERT_MSG(ev->data[0] == BA_SLOT, "victim %u", (unsigned)ev->data[0]);
    UT_ASSERT_MSG(ev->data[1] == BA_OTHER, "killer %u", (unsigned)ev->data[1]);
    UT_ASSERT_MSG(wbnStubLgmLostCalls == 1, "%d loss reports",
                  wbnStubLgmLostCalls);
    UT_ASSERT_MSG(wbnStubLastLgmLost == BA_SLOT, "loss reported for %u",
                  (unsigned)wbnStubLastLgmLost);
    UT_ASSERT_MSG(wbnStubLgmKillCalls == 1, "%d kill reports",
                  wbnStubLgmKillCalls);
    UT_ASSERT_MSG(wbnStubLastLgmKiller == BA_OTHER &&
                      wbnStubLastLgmKilled == BA_SLOT,
                  "reported %u killing %u", (unsigned)wbnStubLastLgmKiller,
                  (unsigned)wbnStubLastLgmKilled);

    /* A man already in the air is not killed twice. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);

    /* A death nobody caused is reported as a loss and credits no kill, which
       is what a mine already produces. */
    UT_ASSERT(baSendOut(sim, BA_OTHER, gx, gy) == SCN_OP_OK);
    baDrainEvents(sim);
    wbnStubLgmLostCalls = 0;
    wbnStubLgmKillCalls = 0;
    op.u.lgmKill.slot   = BA_OTHER;
    op.u.lgmKill.killer = SCN_NONE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    ev = baFindEvent(sim, EVENT_LGM_LOST);
    UT_ASSERT_MSG(ev != NULL, "no builder-lost event for the unowned death");
    UT_ASSERT(ev->data[0] == BA_OTHER);
    UT_ASSERT_MSG(ev->data[1] == NEUTRAL, "killer %u, wanted nobody",
                  (unsigned)ev->data[1]);
    UT_ASSERT_MSG(wbnStubLgmLostCalls == 1, "%d loss reports",
                  wbnStubLgmLostCalls);
    UT_ASSERT_MSG(wbnStubLgmKillCalls == 0,
                  "a death nobody caused credited %d kill(s)",
                  wbnStubLgmKillCalls);

    /* Not while the round is not running. */
    op.u.lgmKill.slot = BA_SLOT;
    sim->state = serverStateLobby;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    sim->state = serverStateRunning;

    serverSimDestroy(sim);
    return 0;
}

/* ── Parachute ───────────────────────────────────────────────────── */

int run_scenario_lgm_parachute(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    ScenarioOp op;
    lgm *l;
    tank *t;
    BYTE gx = 0, gy = 0;
    BYTE near_x = (BYTE)(MAP_MINE_EDGE_LEFT + 3);
    BYTE near_y = 128;
    WORLD twx, twy;

    UT_ASSERT(sim != NULL);
    l = baMan(sim, BA_SLOT);
    t = baTank(sim, BA_SLOT);
    UT_ASSERT(*l != NULL && *t != NULL);
    UT_ASSERT_MSG(baFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_PARACHUTE;
    op.u.lgmParachute.slot = BA_SLOT;
    op.u.lgmParachute.x = near_x;
    op.u.lgmParachute.y = near_y;

    /* An empty seat, and an index past the roster. */
    op.u.lgmParachute.slot = BA_OTHER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmParachute.slot = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmParachute.slot = BA_SLOT;

    /* Nobody in the air: in the tank, and then out walking. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);
    UT_ASSERT(baSendOut(sim, BA_SLOT, gx, gy) == SCN_OP_OK);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);

    UT_ASSERT(baKill(sim, BA_SLOT, SCN_NONE) == SCN_OP_OK);
    UT_ASSERT((*l)->isDead);

    /* The mine border is not a square to land on. */
    op.u.lgmParachute.x = MAP_MINE_EDGE_RIGHT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.lgmParachute.x = near_x;
    op.u.lgmParachute.y = MAP_MINE_EDGE_TOP;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.lgmParachute.y = near_y;

    /* Aimed at a square near the left edge: that is where the flight starts,
       so the helicopter has a handful of ticks to fly rather than a map. */
    baDrainEvents(sim);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG((*l)->destX == (WORLD)(((WORLD)near_x << TANK_SHIFT_MAPSIZE) +
                                         MAP_SQUARE_MIDDLE),
                  "destX %u", (unsigned)(*l)->destX);
    UT_ASSERT_MSG((*l)->destY == (WORLD)(((WORLD)near_y << TANK_SHIFT_MAPSIZE) +
                                         MAP_SQUARE_MIDDLE),
                  "destY %u", (unsigned)(*l)->destY);
    UT_ASSERT_MSG(lgmGetMX(l) == MAP_MINE_EDGE_LEFT,
                  "started at column %u, wanted the left edge",
                  (unsigned)lgmGetMX(l));
    UT_ASSERT_MSG(lgmGetMY(l) == near_y, "started at row %u, wanted %u",
                  (unsigned)lgmGetMY(l), (unsigned)near_y);
    UT_ASSERT_MSG((*l)->isDead, "aiming the flight should not land him");
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 0,
                  "aiming a flight published %u event(s); it should publish none",
                  (unsigned)serverSimGetEventCount(sim));

    /* Aimed at the tank, which is what a death does. */
    op.u.lgmParachute.x = SCN_NONE;
    op.u.lgmParachute.y = SCN_NONE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    tankGetWorld(t, &twx, &twy);
    UT_ASSERT_MSG((*l)->destX == twx && (*l)->destY == twy,
                  "heading for (%u,%u), tank is at (%u,%u)",
                  (unsigned)(*l)->destX, (unsigned)(*l)->destY,
                  (unsigned)twx, (unsigned)twy);
    UT_ASSERT_MSG(lgmGetMX(l) == MAP_MINE_EDGE_LEFT ||
                      lgmGetMX(l) == MAP_MINE_EDGE_RIGHT ||
                      lgmGetMY(l) == MAP_MINE_EDGE_TOP ||
                      lgmGetMY(l) == MAP_MINE_EDGE_BOTTOM,
                  "started at (%u,%u), which is on no edge",
                  (unsigned)lgmGetMX(l), (unsigned)lgmGetMY(l));

    /* Not while the round is not running. */
    sim->state = serverStateLobby;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    sim->state = serverStateRunning;

    serverSimDestroy(sim);
    return 0;
}

/* ── What he carries ─────────────────────────────────────────────── */

int run_scenario_lgm_set_carried(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    ScenarioOp op;
    lgm *l;

    UT_ASSERT(sim != NULL);
    l = baMan(sim, BA_SLOT);
    UT_ASSERT(*l != NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_SET_CARRIED;
    op.u.lgmSetCarried.slot  = BA_SLOT;
    op.u.lgmSetCarried.trees = 7;
    op.u.lgmSetCarried.mines = 3;

    /* An empty seat, and an index past the roster. */
    op.u.lgmSetCarried.slot = BA_OTHER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmSetCarried.slot = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.lgmSetCarried.slot = BA_SLOT;

    /* Both amounts written. */
    baDrainEvents(sim);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG((*l)->numTrees == 7, "trees %u", (unsigned)(*l)->numTrees);
    UT_ASSERT_MSG((*l)->numMines == 3, "mines %u", (unsigned)(*l)->numMines);
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 0,
                  "writing what he carries published %u event(s)",
                  (unsigned)serverSimGetEventCount(sim));

    /* Nothing there leaves that amount alone. */
    op.u.lgmSetCarried.trees = SCN_NONE;
    op.u.lgmSetCarried.mines = 9;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG((*l)->numTrees == 7, "trees %u", (unsigned)(*l)->numTrees);
    UT_ASSERT_MSG((*l)->numMines == 9, "mines %u", (unsigned)(*l)->numMines);

    /* Zero is an amount, not an absence. */
    op.u.lgmSetCarried.trees = 0;
    op.u.lgmSetCarried.mines = SCN_NONE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG((*l)->numTrees == 0, "trees %u", (unsigned)(*l)->numTrees);
    UT_ASSERT_MSG((*l)->numMines == 9, "mines %u", (unsigned)(*l)->numMines);

    /* Past a tank's cap is a mistake worth reporting, and neither amount is
       written when either is out of range. */
    op.u.lgmSetCarried.trees = 5;
    op.u.lgmSetCarried.mines = (BYTE)(TANK_FULL_MINES + 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG((*l)->numTrees == 0 && (*l)->numMines == 9,
                  "a refused op wrote %u trees and %u mines",
                  (unsigned)(*l)->numTrees, (unsigned)(*l)->numMines);
    op.u.lgmSetCarried.trees = (BYTE)(TANK_FULL_TREES + 1);
    op.u.lgmSetCarried.mines = 2;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG((*l)->numMines == 9, "a refused op wrote %u mines",
                  (unsigned)(*l)->numMines);

    /* Exactly the cap is not past it. */
    op.u.lgmSetCarried.trees = TANK_FULL_TREES;
    op.u.lgmSetCarried.mines = TANK_FULL_MINES;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT((*l)->numTrees == TANK_FULL_TREES);
    UT_ASSERT((*l)->numMines == TANK_FULL_MINES);

    /* Not while the round is not running. */
    sim->state = serverStateLobby;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    sim->state = serverStateRunning;

    serverSimDestroy(sim);
    return 0;
}

/* ── The record ──────────────────────────────────────────────────── */

/* A builder death writes log_LostMan and nothing else that a replay's world
 * capture can see — a recording carries no builder — so the record is read
 * straight out of the .wbv's framed event stream. The walk below is the
 * writer-side wire shape (LOG_EVENT / LOG_EVENT_LONG, then per event a type
 * byte, a big-endian u16 length and that many payload bytes), the same shape
 * test_spectator_log.c walks, cut down to the one opcode this case wants. */

typedef struct {
    bool    found;
    uint8_t payload[8];
    int     payloadLen;
} BaLogHit;

static int baReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills,
 * bases and starts, the map runs up to the deep-sea terminator, then
 * MAX_TANKS player blocks. Plaintext, not length-framed. */
static bool baSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = baReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = baReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = baReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    while (1) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = baReadByte(buf, len, p);
        y    = baReadByte(buf, len, p + 1);
        sx   = baReadByte(buf, len, p + 2);
        ex   = baReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = baReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and report the first event of type `want`.
 * Returns false if the stream did not end on a clean LOG_QUIT. */
static bool baFindLogged(const char *path, uint8_t want, BaLogHit *hit) {
    uint8_t *buf = NULL;
    size_t   len = 0;
    size_t   pos;
    bool     ok = false;

    memset(hit, 0, sizeof(*hit));
    if (!extractLogDat(path, &buf, &len)) return false;
    /* Header: WBOLOMOV(8) + version(1) + mapname pstr + game(8) + addr(4) +
       port(2) + time(4) + WBN key(32). */
    if (len < 10 || memcmp(buf, "WBOLOMOV", 8) != 0 || buf[8] != LOG_VERSION) {
        free(buf);
        return false;
    }
    pos = 8 + 1;
    pos += 1 + buf[pos];
    pos += 8 + 4 + 2 + 4 + 32;

    while (pos < len) {
        int code = baReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (baReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!baSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = baReadByte(buf, len, pos);
                pos += 1;
                if (n < 0) break;
            } else {
                if (pos + 2 > len) break;
                /* The writer stores data[1]=low, data[2]=high and the reader
                   rebuilds the count as (lo << 8) | hi. */
                n = (buf[pos + 1] << 8) | buf[pos];
                pos += 2;
            }
            for (i = 0; i < n; i++) {
                int ev, plen;
                size_t payloadStart;
                if (pos + 3 > len) { n = -1; break; }
                ev   = buf[pos];
                plen = (buf[pos + 1] << 8) | buf[pos + 2];
                payloadStart = pos + 3;
                if (payloadStart + (size_t)plen > len) { n = -1; break; }
                if (ev == want && !hit->found) {
                    int copy = plen;
                    if (copy > (int)sizeof(hit->payload)) {
                        copy = (int)sizeof(hit->payload);
                    }
                    hit->found = true;
                    hit->payloadLen = plen;
                    memcpy(hit->payload, buf + payloadStart, (size_t)copy);
                }
                pos = payloadStart + (size_t)plen;
            }
            if (n < 0) break;
        } else {
            break;
        }
    }

    free(buf);
    return ok;
}

int run_scenario_lgm_kill_record(void) {
    ReplayHarness h;
    ServerSim *sim;
    BaLogHit hit;
    BYTE gx = 0, gy = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnBuilderArms", "Builder"),
                  "could not start recording");
    sim = h.sim;
    UT_ASSERT(sim->sim.tanks[BA_SLOT] != NULL);
    UT_ASSERT(sim->sim.lgmen[BA_SLOT] != NULL);

    /* Let the round settle after the opening snapshot. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(baFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    /* Out on a wall, then killed where he stands. */
    UT_ASSERT(baSendOut(sim, BA_SLOT, gx, gy) == SCN_OP_OK);
    UT_ASSERT(baKill(sim, BA_SLOT, SCN_NONE) == SCN_OP_OK);

    /* Two ticks so the change reaches the file. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    UT_ASSERT_MSG(baFindLogged(h.path, (uint8_t)log_LostMan, &hit),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hit.found, "log_LostMan is not in the recording");
    UT_ASSERT_MSG(hit.payloadLen == 1, "log_LostMan payload is %d bytes",
                  hit.payloadLen);
    UT_ASSERT_MSG(hit.payload[0] == BA_SLOT, "log_LostMan names slot %u",
                  (unsigned)hit.payload[0]);

    replayHarnessStop(&h);
    return 0;
}
