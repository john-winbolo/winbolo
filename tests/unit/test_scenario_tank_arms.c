/*
 * The six tank ops a scenario writes the world with.
 *
 * Each arm validates, mutates through the same internal function a player's
 * own action reaches, and leaves the publish and the record to that function.
 * So each case here asks three things: does every refusal in the op's
 * contract come back with its own code, does the mutation land on the sim,
 * and does what the arm publishes reach the event queue.
 *
 * Item indices are 0-based in an op and 1-based in the pill list the arms
 * reuse, so the two pill arms are driven at index 0 and at the last index and
 * the pill each one touched is checked by name.
 *
 * run_scenario_tank_set_stocks  — absolute and delta, the caps, armour on a
 *                                 destroyed tank
 * run_scenario_tank_kill        — the kill event, who is credited, and the
 *                                 WinBolo.net report
 * run_scenario_tank_teleport    — square and start modes, the terrain that
 *                                 will not take a tank
 * run_scenario_tank_set_boat    — afloat only over water
 * run_scenario_tank_give_pill   — a pill handed over, including one a drive-by
 *                                 could not take
 * run_scenario_tank_drop_pill   — a carried pill put down
 * run_scenario_tank_arm_records — the stock, owner and position records the
 *                                 arms produce survive a real recording
 *
 * Drives serverSimApplyScenarioOp at ut_make_running_sim and reads the
 * ServerSim struct directly; the unittests profile permits internal access.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, playerConnected[], events[] */
#include "server_sim_scenario.h"
#include "game_sim.h"              /* GameSim: mp/pb/bs/ss, tanks[] */
#include "bolo_map.h"              /* mapGetPos / mapSetPos */
#include "pillbox.h"               /* pillsGetPill, pillsSetPill */
#include "bases.h"                 /* basesExistPos */
#include "starts.h"                /* startsGetNumStarts */
#include "tank.h"                  /* the state the arms write */
#include "input_packet.h"          /* EVENT_TANK_KILLED, EVENT_PILL_CAPTURED */
#include "replay_harness.h"
#include "test_harness.h"

/* The winbolonetAddEvent TANK_KILL spy in test_stubs.c. */
extern int  wbnStubKillEventCalls;
extern BYTE wbnStubLastKiller;
extern BYTE wbnStubLastKilled;

#define TA_SLOT 0

/* ── Small helpers ───────────────────────────────────────────────── */

static tank *taTank(ServerSim *sim, BYTE slot) {
    return &sim->sim.tanks[slot];
}

/* Drop every event the sim has queued, so the next assertion sees only what
   the op just published. */
static void taDrainEvents(ServerSim *sim) {
    sim->eventCount = 0;
}

static int taCountEvents(ServerSim *sim, BYTE type) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    int found = 0;
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type) found++;
    }
    return found;
}

static const GameEvent *taFindEvent(ServerSim *sim, BYTE type) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type) return &evs[i];
    }
    return NULL;
}

/* A square inside the minable area holding nothing but the wanted terrain and
   not under the slot-0 tank. */
static bool taFindTile(ServerSim *sim, BYTE want, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    BYTE tx = tankGetMX(taTank(sim, TA_SLOT));
    BYTE ty = tankGetMY(taTank(sim, TA_SLOT));
    int x, y;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            if ((BYTE)x == tx && (BYTE)y == ty) continue;
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == want &&
                !pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) &&
                !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* ── Stocks ──────────────────────────────────────────────────────── */

int run_scenario_tank_set_stocks(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    tank *t;

    UT_ASSERT(sim != NULL);
    t = taTank(sim, TA_SLOT);
    UT_ASSERT(*t != NULL);

    /* Absolute: -1 leaves a stock where it was, everything else is written. */
    tankSetShells(&sim->sim, t, 5);
    tankSetMines(&sim->sim, t, 5);
    tankSetArmour(t, 5);
    tankSetTrees(&sim->sim, t, 5);
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_SET_STOCKS;
    op.u.tankSetStocks.slot = TA_SLOT;
    op.u.tankSetStocks.mode = SCN_STOCK_ABSOLUTE;
    op.u.tankSetStocks.shells = 11;
    op.u.tankSetStocks.mines  = -1;
    op.u.tankSetStocks.armour = 23;
    op.u.tankSetStocks.trees  = -1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(tankGetShells(t) == 11, "shells %u", (unsigned)tankGetShells(t));
    UT_ASSERT_MSG(tankGetMines(t) == 5, "mines %u", (unsigned)tankGetMines(t));
    UT_ASSERT_MSG(tankGetArmour(t) == 23, "armour %u", (unsigned)tankGetArmour(t));
    UT_ASSERT_MSG(tankGetTrees(t) == 5, "trees %u", (unsigned)tankGetTrees(t));

    /* Delta: 0 leaves a stock alone, a negative one takes some away, and a
       delta past an end lands on that end rather than being refused. */
    op.u.tankSetStocks.mode = SCN_STOCK_DELTA;
    op.u.tankSetStocks.shells = -4;
    op.u.tankSetStocks.mines  = 0;
    op.u.tankSetStocks.armour = 1000;
    op.u.tankSetStocks.trees  = -1000;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(tankGetShells(t) == 7, "shells %u", (unsigned)tankGetShells(t));
    UT_ASSERT_MSG(tankGetMines(t) == 5, "mines %u", (unsigned)tankGetMines(t));
    UT_ASSERT_MSG(tankGetArmour(t) == TANK_FULL_ARMOUR, "armour %u",
                  (unsigned)tankGetArmour(t));
    UT_ASSERT_MSG(tankGetTrees(t) == 0, "trees %u", (unsigned)tankGetTrees(t));

    /* Absolute past the cap is a mistake worth reporting, and nothing is
       written when it is. */
    op.u.tankSetStocks.mode = SCN_STOCK_ABSOLUTE;
    op.u.tankSetStocks.shells = 3;
    op.u.tankSetStocks.mines  = (int16_t)(TANK_FULL_MINES + 1);
    op.u.tankSetStocks.armour = -1;
    op.u.tankSetStocks.trees  = -1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(tankGetShells(t) == 7, "a refused op still wrote shells");

    /* An unknown mode is a value out of range too. */
    op.u.tankSetStocks.mode = 9;
    op.u.tankSetStocks.mines = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);

    /* A destroyed tank takes no armour, and the other three do not slip
       through on the same op. */
    tankSetDestroyed(t, true);
    op.u.tankSetStocks.mode = SCN_STOCK_ABSOLUTE;
    op.u.tankSetStocks.shells = 2;
    op.u.tankSetStocks.mines  = -1;
    op.u.tankSetStocks.armour = 9;
    op.u.tankSetStocks.trees  = -1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TANK_DEAD);
    UT_ASSERT_MSG(tankGetShells(t) == 7, "a refused op still wrote shells");
    /* Without armour the same op is fine on the same destroyed tank. */
    op.u.tankSetStocks.armour = -1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(tankGetShells(t) == 2);
    tankSetDestroyed(t, false);

    /* An empty seat, and an index past the roster. */
    op.u.tankSetStocks.slot = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.tankSetStocks.slot = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    /* Not while the round is not running. */
    op.u.tankSetStocks.slot = TA_SLOT;
    sim->state = serverStateLobby;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE);
    sim->state = serverStateRunning;

    serverSimDestroy(sim);
    return 0;
}

/* ── Kill ────────────────────────────────────────────────────────── */

int run_scenario_tank_kill(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    const GameEvent *ev;
    tank *t;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 1, "Other", false);
    t = taTank(sim, TA_SLOT);
    UT_ASSERT(*t != NULL && sim->sim.tanks[1] != NULL);

    /* An attributed kill: the named slot is credited, on the event and to
       WinBolo.net. */
    taDrainEvents(sim);
    wbnStubKillEventCalls = 0;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_KILL;
    op.u.tankKill.slot   = TA_SLOT;
    op.u.tankKill.killer = 1;
    op.u.tankKill.cause  = LAST_DEATH_BY_SCRIPT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    UT_ASSERT_MSG(tankIsDestroyed(t), "the tank should be destroyed");
    UT_ASSERT_MSG(tankGetArmour(t) == 0, "armour %u", (unsigned)tankGetArmour(t));
    UT_ASSERT_MSG(tankGetDeathWait(t) > 0, "the tank should be waiting to respawn");

    ev = taFindEvent(sim, EVENT_TANK_KILLED);
    UT_ASSERT_MSG(ev != NULL, "no kill event was published");
    UT_ASSERT_MSG(ev->data[0] == 1, "killer %u", (unsigned)ev->data[0]);
    UT_ASSERT_MSG(ev->data[1] == TA_SLOT, "killed %u", (unsigned)ev->data[1]);
    UT_ASSERT_MSG(ev->data[2] == LAST_DEATH_BY_SCRIPT, "cause %u",
                  (unsigned)ev->data[2]);
    UT_ASSERT_MSG(wbnStubKillEventCalls == 1, "%d kill reports",
                  wbnStubKillEventCalls);
    UT_ASSERT(wbnStubLastKiller == 1 && wbnStubLastKilled == TA_SLOT);

    /* A tank already destroyed is not killed twice. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TANK_DEAD);

    /* Environmental: the dying tank is named as its own killer, so the report
       never carries a slot that is not a player. */
    taDrainEvents(sim);
    wbnStubKillEventCalls = 0;
    op.u.tankKill.slot   = 1;
    op.u.tankKill.killer = SCN_NONE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    ev = taFindEvent(sim, EVENT_TANK_KILLED);
    UT_ASSERT_MSG(ev != NULL, "no kill event for the environmental case");
    UT_ASSERT_MSG(ev->data[0] == 1, "killer %u, wanted the tank's own slot",
                  (unsigned)ev->data[0]);
    UT_ASSERT(ev->data[1] == 1);
    UT_ASSERT_MSG(wbnStubKillEventCalls == 1, "%d kill reports",
                  wbnStubKillEventCalls);
    UT_ASSERT_MSG(wbnStubLastKiller == 1 && wbnStubLastKilled == 1,
                  "reported %u killing %u", (unsigned)wbnStubLastKiller,
                  (unsigned)wbnStubLastKilled);

    /* A killer that is not a player is refused, and nothing is published. */
    serverSimDestroy(sim);
    sim = ut_make_running_sim("Tester");
    UT_ASSERT(sim != NULL);
    taDrainEvents(sim);
    wbnStubKillEventCalls = 0;
    op.u.tankKill.slot   = TA_SLOT;
    op.u.tankKill.killer = 7;          /* an empty seat */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.tankKill.killer = MAX_TANKS;  /* past the roster */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    UT_ASSERT_MSG(!tankIsDestroyed(taTank(sim, TA_SLOT)),
                  "a refused kill still killed the tank");
    UT_ASSERT_MSG(taCountEvents(sim, EVENT_TANK_KILLED) == 0,
                  "a refused kill still published");
    UT_ASSERT_MSG(wbnStubKillEventCalls == 0, "a refused kill still reported");

    /* An empty victim seat. */
    op.u.tankKill.slot   = 3;
    op.u.tankKill.killer = SCN_NONE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    serverSimDestroy(sim);
    return 0;
}

/* ── Teleport ────────────────────────────────────────────────────── */

int run_scenario_tank_teleport(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    tank *t;
    BYTE gx = 0, gy = 0;
    BYTE nStarts;
    WORLD wx, wy;

    UT_ASSERT(sim != NULL);
    t = taTank(sim, TA_SLOT);
    UT_ASSERT(*t != NULL);
    UT_ASSERT_MSG(taFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    /* A square: the tank lands in the middle of it, keeping its facing. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_TELEPORT;
    op.u.tankTeleport.slot = TA_SLOT;
    op.u.tankTeleport.mode = SCN_TELEPORT_SQUARE;
    op.u.tankTeleport.x = gx;
    op.u.tankTeleport.y = gy;
    op.u.tankTeleport.dir = SCN_NONE;
    (*t)->angle = 77.0f;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    tankGetWorld(t, &wx, &wy);
    UT_ASSERT_MSG(wx == (WORLD)(((WORLD)gx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                  "world x %u", (unsigned)wx);
    UT_ASSERT_MSG(wy == (WORLD)(((WORLD)gy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                  "world y %u", (unsigned)wy);
    UT_ASSERT(tankGetMX(t) == gx && tankGetMY(t) == gy);
    UT_ASSERT_MSG(tankGet256Dir(t) == 77, "facing %u should have been kept",
                  (unsigned)tankGet256Dir(t));
    UT_ASSERT_MSG(!tankIsOnBoat(t), "a tank put on grass should not be afloat");
    UT_ASSERT_MSG((*t)->residualSpeed == 0, "residual speed survived");
    UT_ASSERT_MSG((*t)->lastBoatRiverX == 0 && (*t)->lastBoatRiverY == 0,
                  "the boat trail survived");

    /* A named facing is taken as written. */
    op.u.tankTeleport.dir = 200;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(tankGet256Dir(t) == 200, "facing %u", (unsigned)tankGet256Dir(t));

    /* Water under the tank puts it afloat. */
    {
        BYTE rx = 0, ry = 0;
        if (taFindTile(sim, RIVER, &rx, &ry)) {
            op.u.tankTeleport.x = rx;
            op.u.tankTeleport.y = ry;
            UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
            UT_ASSERT_MSG(tankIsOnBoat(t), "a tank put on a river should be afloat");
            op.u.tankTeleport.x = gx;
            op.u.tankTeleport.y = gy;
            UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
        }
    }

    /* Terrain that will not take a tank. */
    mapSetPos(&sim->sim, &sim->sim.mp, gx, gy, BUILDING, FALSE, FALSE);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    mapSetPos(&sim->sim, &sim->sim.mp, gx, gy, GRASS, FALSE, FALSE);

    /* The border reads as deep sea, so it is off the map as far as an op is
       concerned. */
    op.u.tankTeleport.x = 0;
    op.u.tankTeleport.y = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.tankTeleport.x = 255;
    op.u.tankTeleport.y = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);

    /* A live pill is solid; a dead one is not. */
    {
        pillbox pl;
        BYTE px = 0, py = 0;
        UT_ASSERT(taFindTile(sim, GRASS, &px, &py));
        UT_ASSERT(pillsGetNumPills(&sim->sim.pb) > 0);
        memset(&pl, 0, sizeof(pl));
        pillsGetPill(&sim->sim.pb, &pl, 1);
        pl.x = px; pl.y = py; pl.armour = PILLS_MAX_ARMOUR; pl.inTank = FALSE;
        pillsSetPill(&sim->sim.pb, &pl, 1);
        op.u.tankTeleport.x = px;
        op.u.tankTeleport.y = py;
        UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                      "a live pill should be solid");
        memset(&pl, 0, sizeof(pl));
        pillsGetPill(&sim->sim.pb, &pl, 1);
        pl.armour = 0;
        pillsSetPill(&sim->sim.pb, &pl, 1);
        UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                      "a dead pill should be drivable");
    }

    /* Start mode, at the first index and at the last.
       A start record sits on deep sea, and the square the tank actually
       arrives at is the one the engine's own resolver picks near it. So what
       a named start has to produce is a square a tank could spawn on — deep
       sea, unmined — with the tank afloat on it, not the record's own
       coordinates. */
    nStarts = startsGetNumStarts(&sim->sim.ss);
    UT_ASSERT_MSG(nStarts > 0, "map has no starts");
    op.u.tankTeleport.mode = SCN_TELEPORT_START;
    op.u.tankTeleport.dir = SCN_NONE;
    op.u.tankTeleport.start = 0;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "start 0 was refused");
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, tankGetMX(t), tankGetMY(t)) == DEEP_SEA,
                  "start 0 put the tank on terrain %u at %u,%u, not on sea",
                  (unsigned)mapGetPos(&sim->sim.mp, tankGetMX(t), tankGetMY(t)),
                  (unsigned)tankGetMX(t), (unsigned)tankGetMY(t));
    UT_ASSERT_MSG(!mapIsMine(&sim->sim.mp, tankGetMX(t), tankGetMY(t)),
                  "start 0 put the tank on a mine");
    UT_ASSERT_MSG(tankIsOnBoat(t),
                  "a tank put on a start should arrive afloat, or it drowns");

    op.u.tankTeleport.start = (BYTE)(nStarts - 1);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the last start was refused");
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, tankGetMX(t), tankGetMY(t)) == DEEP_SEA,
                  "the last start put the tank on terrain %u, not on sea",
                  (unsigned)mapGetPos(&sim->sim.mp, tankGetMX(t), tankGetMY(t)));
    UT_ASSERT(tankIsOnBoat(t));

    /* One past the last, and a long way past it. 0xFF is left out on
       purpose: it is the payload's "let the engine choose", not an index. */
    op.u.tankTeleport.start = nStarts;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.tankTeleport.start = MAX_STARTS + 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    /* A start reserved for this player's next respawn is not the op's to
       spend: naming a start borrows the slot and puts back what was in it. */
    sim->sim.pendingStartIdx[TA_SLOT] = (BYTE)(nStarts - 1);
    op.u.tankTeleport.start = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(sim->sim.pendingStartIdx[TA_SLOT] == (BYTE)(nStarts - 1),
                  "the reserved start was spent: slot holds %u",
                  (unsigned)sim->sim.pendingStartIdx[TA_SLOT]);
    sim->sim.pendingStartIdx[TA_SLOT] = MAX_STARTS;

    /* Letting the engine choose lands somewhere it would spawn a tank. */
    op.u.tankTeleport.start = SCN_NONE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, tankGetMX(t), tankGetMY(t)) == DEEP_SEA,
                  "the engine's own start put the tank on terrain %u",
                  (unsigned)mapGetPos(&sim->sim.mp, tankGetMX(t), tankGetMY(t)));
    UT_ASSERT(tankIsOnBoat(t));
    UT_ASSERT_MSG(sim->sim.inStartFind == FALSE,
                  "the start search was left open");

    /* A destroyed tank is not moved, and an empty seat has nothing to move. */
    tankSetDestroyed(t, true);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TANK_DEAD);
    tankSetDestroyed(t, false);
    op.u.tankTeleport.slot = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    serverSimDestroy(sim);
    return 0;
}

/* ── Boat ────────────────────────────────────────────────────────── */

int run_scenario_tank_set_boat(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    tank *t;
    BYTE gx = 0, gy = 0;
    BYTE rx = 0, ry = 0;

    UT_ASSERT(sim != NULL);
    t = taTank(sim, TA_SLOT);
    UT_ASSERT(*t != NULL);
    UT_ASSERT_MSG(taFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_SET_BOAT;
    op.u.tankSetBoat.slot = TA_SLOT;

    /* Coming off a boat needs nothing under the tank. */
    {
        ScenarioOp tp;
        memset(&tp, 0, sizeof(tp));
        tp.type = SCN_OP_TANK_TELEPORT;
        tp.u.tankTeleport.slot = TA_SLOT;
        tp.u.tankTeleport.mode = SCN_TELEPORT_SQUARE;
        tp.u.tankTeleport.x = gx;
        tp.u.tankTeleport.y = gy;
        tp.u.tankTeleport.dir = SCN_NONE;
        UT_ASSERT(serverSimApplyScenarioOp(sim, &tp, NULL) == SCN_OP_OK);
    }
    tankSetOnBoat(t, TRUE);
    op.u.tankSetBoat.onBoat = false;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(!tankIsOnBoat(t), "the tank should be off its boat");

    /* Going afloat on dry land is refused, and changes nothing. */
    op.u.tankSetBoat.onBoat = true;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    UT_ASSERT_MSG(!tankIsOnBoat(t), "a refused op still put the tank afloat");

    /* Over water it is allowed, and the square the tank was last afloat on is
       forgotten so no boat is dropped there later. */
    if (taFindTile(sim, RIVER, &rx, &ry)) {
        ScenarioOp tp;
        memset(&tp, 0, sizeof(tp));
        tp.type = SCN_OP_TANK_TELEPORT;
        tp.u.tankTeleport.slot = TA_SLOT;
        tp.u.tankTeleport.mode = SCN_TELEPORT_SQUARE;
        tp.u.tankTeleport.x = rx;
        tp.u.tankTeleport.y = ry;
        tp.u.tankTeleport.dir = SCN_NONE;
        UT_ASSERT(serverSimApplyScenarioOp(sim, &tp, NULL) == SCN_OP_OK);
        (*t)->lastBoatRiverX = 99;
        (*t)->lastBoatRiverY = 99;
        op.u.tankSetBoat.onBoat = true;
        UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
        UT_ASSERT(tankIsOnBoat(t));
        UT_ASSERT_MSG((*t)->lastBoatRiverX == 0 && (*t)->lastBoatRiverY == 0,
                      "the boat trail survived");
    }

    /* A destroyed tank, an empty seat and an index past the roster. */
    tankSetDestroyed(t, true);
    op.u.tankSetBoat.onBoat = false;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TANK_DEAD);
    tankSetDestroyed(t, false);
    op.u.tankSetBoat.slot = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);
    op.u.tankSetBoat.slot = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    serverSimDestroy(sim);
    return 0;
}

/* ── Give pill ───────────────────────────────────────────────────── */

int run_scenario_tank_give_pill(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    tank *t;
    pillbox pl;
    BYTE n;

    UT_ASSERT(sim != NULL);
    t = taTank(sim, TA_SLOT);
    UT_ASSERT(*t != NULL);
    n = pillsGetNumPills(&sim->sim.pb);
    UT_ASSERT_MSG(n >= 2, "map has %u pills, this case needs two", (unsigned)n);

    /* Op index 0 is pill 1 in the list. An armoured pill is taken, which a
       drive-by could not do — that is the point of handing one over. */
    memset(&pl, 0, sizeof(pl));
    pillsGetPill(&sim->sim.pb, &pl, 1);
    pl.armour = PILLS_MAX_ARMOUR;
    pl.inTank = FALSE;
    pl.owner = NEUTRAL;
    pillsSetPill(&sim->sim.pb, &pl, 1);

    taDrainEvents(sim);
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_GIVE_PILL;
    op.u.tankGivePill.slot = TA_SLOT;
    op.u.tankGivePill.pill = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    memset(&pl, 0, sizeof(pl));
    pillsGetPill(&sim->sim.pb, &pl, 1);
    UT_ASSERT_MSG(pl.inTank, "pill 1 should be in the tank");
    UT_ASSERT_MSG(pl.owner == TA_SLOT, "pill 1 owner %u", (unsigned)pl.owner);
    UT_ASSERT_MSG(tankGetNumCarriedPills(t) == 1, "carrying %u",
                  (unsigned)tankGetNumCarriedPills(t));
    UT_ASSERT_MSG(tankIsCarryingPill(t, 1), "pill 1 is not on the carry list");
    UT_ASSERT_MSG(taCountEvents(sim, EVENT_PILL_CAPTURED) == 1,
                  "the capture was not published");

    /* The same pill again is already carried. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_CARRIED);

    /* The last index in op terms is the last pill in the list. */
    op.u.tankGivePill.pill = (BYTE)(n - 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(tankIsCarryingPill(t, n), "the last pill is not carried");

    /* One past the last. */
    op.u.tankGivePill.pill = n;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.tankGivePill.pill = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    /* Every pill on the map fits on one tank: the carry list holds MAX_PILLS
       and a map holds at most that many, so a tank can take all of them and
       the full answer has nothing left to refuse. The arm still asks, so it
       keeps its answer if the two caps ever part company. */
    {
        BYTE k;
        for (k = 0; k < n; k++) {
            op.u.tankGivePill.pill = k;
            serverSimApplyScenarioOp(sim, &op, NULL);
        }
        UT_ASSERT_MSG(tankGetNumCarriedPills(t) == n, "carrying %u of %u",
                      (unsigned)tankGetNumCarriedPills(t), (unsigned)n);
        UT_ASSERT_MSG(n <= MAX_PILLS, "%u pills is past the carry list",
                      (unsigned)n);
        /* One already aboard answers carried, not full. */
        op.u.tankGivePill.pill = 0;
        UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_CARRIED);
    }

    /* A destroyed tank, and an empty seat. */
    tankSetDestroyed(t, true);
    op.u.tankGivePill.pill = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_TANK_DEAD);
    tankSetDestroyed(t, false);
    op.u.tankGivePill.slot = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    serverSimDestroy(sim);
    return 0;
}

/* ── Drop pill ───────────────────────────────────────────────────── */

int run_scenario_tank_drop_pill(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp give;
    ScenarioOp op;
    tank *t;
    pillbox pl;
    BYTE n, gx = 0, gy = 0;

    UT_ASSERT(sim != NULL);
    t = taTank(sim, TA_SLOT);
    UT_ASSERT(*t != NULL);
    n = pillsGetNumPills(&sim->sim.pb);
    UT_ASSERT_MSG(n >= 2, "map has %u pills, this case needs two", (unsigned)n);
    UT_ASSERT_MSG(taFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    memset(&give, 0, sizeof(give));
    give.type = SCN_OP_TANK_GIVE_PILL;
    give.u.tankGivePill.slot = TA_SLOT;

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_DROP_PILL;
    op.u.tankDropPill.slot = TA_SLOT;

    /* A pill the tank is not carrying is not the tank's to drop. */
    op.u.tankDropPill.pill = 0;
    op.u.tankDropPill.x = gx;
    op.u.tankDropPill.y = gy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    /* And so is one that is not on the map at all. */
    op.u.tankDropPill.pill = n;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    /* Op index 0 lands on the named square, dead and owned by the carrier. */
    give.u.tankGivePill.pill = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &give, NULL) == SCN_OP_OK);
    op.u.tankDropPill.pill = 0;
    op.u.tankDropPill.x = gx;
    op.u.tankDropPill.y = gy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    memset(&pl, 0, sizeof(pl));
    pillsGetPill(&sim->sim.pb, &pl, 1);
    UT_ASSERT_MSG(pl.x == gx && pl.y == gy, "pill 1 landed at %u,%u",
                  (unsigned)pl.x, (unsigned)pl.y);
    UT_ASSERT_MSG(!pl.inTank, "pill 1 is still carried");
    UT_ASSERT_MSG(pl.armour == 0, "pill 1 armour %u", (unsigned)pl.armour);
    UT_ASSERT_MSG(pl.owner == TA_SLOT, "pill 1 owner %u", (unsigned)pl.owner);
    UT_ASSERT_MSG(!tankIsCarryingPill(t, 1), "pill 1 is still on the carry list");

    /* The last index in op terms, dropped under the tank. */
    give.u.tankGivePill.pill = (BYTE)(n - 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &give, NULL) == SCN_OP_OK);
    op.u.tankDropPill.pill = (BYTE)(n - 1);
    op.u.tankDropPill.x = SCN_NONE;
    op.u.tankDropPill.y = SCN_NONE;
    {
        BYTE tx = tankGetMX(t);
        BYTE ty = tankGetMY(t);
        ScnOpResult r = serverSimApplyScenarioOp(sim, &op, NULL);
        if (r == SCN_OP_OK) {
            memset(&pl, 0, sizeof(pl));
            pillsGetPill(&sim->sim.pb, &pl, n);
            UT_ASSERT_MSG(pl.x == tx && pl.y == ty,
                          "the last pill landed at %u,%u, wanted the tank's "
                          "square %u,%u", (unsigned)pl.x, (unsigned)pl.y,
                          (unsigned)tx, (unsigned)ty);
            UT_ASSERT(!tankIsCarryingPill(t, n));
        } else {
            /* The tank is standing on something that will not hold a pill. */
            UT_ASSERT_MSG(r == SCN_OP_BAD_TERRAIN, "dropping under the tank "
                          "answered %d", (int)r);
            UT_ASSERT_MSG(tankIsCarryingPill(t, n),
                          "a refused drop still took the pill off the tank");
        }
    }

    /* The border is off the map. */
    give.u.tankGivePill.pill = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &give, NULL) == SCN_OP_OK);
    op.u.tankDropPill.pill = 0;
    op.u.tankDropPill.x = 0;
    op.u.tankDropPill.y = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    /* Not 0xFF, 0xFF: that pair is the payload's "under the tank" and never
       reaches the square test. It can never collide with a real square
       either, because the playable area stops at MAP_MINE_EDGE_RIGHT. */
    op.u.tankDropPill.x = MAP_MINE_EDGE_RIGHT;
    op.u.tankDropPill.y = 100;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    UT_ASSERT_MSG(tankIsCarryingPill(t, 1), "a refused drop still let it go");

    /* A square that will not hold a pill. */
    mapSetPos(&sim->sim, &sim->sim.mp, gx, gy, BUILDING, FALSE, FALSE);
    op.u.tankDropPill.x = gx;
    op.u.tankDropPill.y = gy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    UT_ASSERT_MSG(tankIsCarryingPill(t, 1), "a refused drop still let it go");
    mapSetPos(&sim->sim, &sim->sim.mp, gx, gy, GRASS, FALSE, FALSE);

    /* An empty seat. */
    op.u.tankDropPill.slot = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    serverSimDestroy(sim);
    return 0;
}

/* ── The records the arms leave behind ───────────────────────────── */

/* The arms write no record of their own: the stock change rides the tick's
 * own pass and the pill changes ride pillsSetPill / pillsSetPillOwner. This
 * drives all three through a real recording and reads the file back with the
 * production reader, so a record that never reached the .wbv shows up as a
 * world the replay disagrees about. */
int run_scenario_tank_arm_records(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    BYTE gx = 0, gy = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnTankArms", "Tester"),
                  "could not start recording");
    sim = h.sim;
    UT_ASSERT(sim->sim.tanks[TA_SLOT] != NULL);
    UT_ASSERT(pillsGetNumPills(&sim->sim.pb) > 0);

    /* Let the round settle after the opening snapshot. */
    replayHarnessTick(&h, 4);

    UT_ASSERT_MSG(taFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    /* Stocks. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_SET_STOCKS;
    op.u.tankSetStocks.slot = TA_SLOT;
    op.u.tankSetStocks.mode = SCN_STOCK_ABSOLUTE;
    op.u.tankSetStocks.shells = 13;
    op.u.tankSetStocks.mines  = 4;
    op.u.tankSetStocks.armour = 21;
    op.u.tankSetStocks.trees  = 6;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    /* A pill handed over and then put down somewhere else. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_GIVE_PILL;
    op.u.tankGivePill.slot = TA_SLOT;
    op.u.tankGivePill.pill = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    replayHarnessTick(&h, 2);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_DROP_PILL;
    op.u.tankDropPill.slot = TA_SLOT;
    op.u.tankDropPill.pill = 0;
    op.u.tankDropPill.x = gx;
    op.u.tankDropPill.y = gy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    /* Two ticks so the last change reaches the file. */
    replayHarnessTick(&h, 4);

    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode the recording");
    UT_ASSERT_MSG(replayHarnessCompare(&h),
                  "the replay disagrees with the sim: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}
