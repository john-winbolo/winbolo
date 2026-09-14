/*
 * The six pill and base ops a scenario re-deals the estate with.
 *
 * Each arm validates, mutates through the same internal function a capture
 * goes through, and leaves the publish and the record to that function. So
 * each case here asks three things: does every refusal in the op's contract
 * come back with its own code, does the mutation land on the sim, and does
 * what the arm publishes reach the event queue.
 *
 * Item indices are 0-based in an op and 1-based in the pill and base lists the
 * arms reuse, so every arm is driven at index 0 and at the last index and the
 * item it touched is checked by name.
 *
 * Three cases carry the semantics the funnel exists to hold:
 *   - an owner may be a slot nobody is sitting in, so a map can be fought
 *     against guns with nobody behind them (pill and base set-owner);
 *   - a base handed between two real owners drains as a capture does, unless
 *     the op says to keep the stock, and announces the capture either way
 *     (run_scenario_base_owner_keep_stock);
 *   - basesSetStock says what a base holds rather than adding to it, and caps
 *     each amount at a full load (run_scenario_base_set_stock).
 *
 * run_scenario_pill_set_owner       — a pill handed over, including to nobody
 * run_scenario_pill_set_armour      — armour written, down to a dead pill
 * run_scenario_pill_set_speed       — the attack interval, carried or not
 * run_scenario_pill_move            — a pill put on another square
 * run_scenario_base_set_owner       — a base handed over, including to nobody
 * run_scenario_base_owner_keep_stock— the drain, and the flag that stops it
 * run_scenario_base_set_stock       — what a base holds, set and capped
 * run_scenario_pill_base_arm_records— the five records survive a recording
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
#include "game_sim.h"              /* GameSim: mp/pb/bs, tanks[] */
#include "bolo_map.h"              /* mapGetPos / mapSetPos */
#include "pillbox.h"               /* the pill list the pill arms write */
#include "bases.h"                 /* the base list the base arms write */
#include "mines.h"                 /* minesExistPos — the move arm's square test */
#include "tank.h"                  /* tankGetMX / tankGetMY */
#include "log.h"                   /* log_Pill* / log_Base* and the stream opcodes */
#include "input_packet.h"          /* EVENT_PILL_CAPTURED, EVENT_BASE_CAPTURED */
#include "replay_harness.h"
#include "test_harness.h"

#define PB_SLOT   0
#define PB_OTHER  1
#define PB_NOBODY 9    /* a seat inside the roster with nobody in it */

/* ── Small helpers ───────────────────────────────────────────────── */

static void pbDrainEvents(ServerSim *sim) {
    sim->eventCount = 0;
}

static int pbCountEvents(ServerSim *sim, BYTE type) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    int found = 0;
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type) found++;
    }
    return found;
}

/* A pill by its op index, which is one less than its number in the list. */
static pillbox pbPill(ServerSim *sim, BYTE opIndex) {
    pillbox item;
    memset(&item, 0, sizeof(item));
    pillsGetPill(&sim->sim.pb, &item, (BYTE)(opIndex + 1));
    return item;
}

/* A base by its op index, same rule. */
static base pbBase(ServerSim *sim, BYTE opIndex) {
    base item;
    memset(&item, 0, sizeof(item));
    basesGetBase(&sim->sim.bs, &item, (BYTE)(opIndex + 1));
    return item;
}

/* Put a pill back on the map in a known state, without going through an op. */
static void pbPlacePill(ServerSim *sim, BYTE opIndex, BYTE x, BYTE y,
                        BYTE owner, BYTE armour, bool inTank) {
    pillbox item = pbPill(sim, opIndex);
    item.x = x;
    item.y = y;
    item.owner = owner;
    item.armour = armour;
    item.inTank = inTank;
    pillsSetPill(&sim->sim, &sim->sim.pb, &item, (BYTE)(opIndex + 1));
}

/* A square inside the minable area holding nothing but the wanted terrain,
   with no pill, base or mine on it and no tank standing there — so an op
   aimed at it turns on the terrain and not on what is in the way. */
static bool pbFindTile(ServerSim *sim, BYTE want, BYTE skipX, BYTE skipY,
                       BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    BYTE ax = 0xFF, ay = 0xFF, bx = 0xFF, by = 0xFF;
    int x, y;
    if (gs->tanks[PB_SLOT] != NULL) {
        ax = tankGetMX(&gs->tanks[PB_SLOT]);
        ay = tankGetMY(&gs->tanks[PB_SLOT]);
    }
    if (gs->tanks[PB_OTHER] != NULL) {
        bx = tankGetMX(&gs->tanks[PB_OTHER]);
        by = tankGetMY(&gs->tanks[PB_OTHER]);
    }
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            if ((BYTE)x == ax && (BYTE)y == ay) continue;
            if ((BYTE)x == bx && (BYTE)y == by) continue;
            if ((BYTE)x == skipX && (BYTE)y == skipY) continue;
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

/* ── Pill: set owner ─────────────────────────────────────────────── */

int run_scenario_pill_set_owner(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE n;

    UT_ASSERT(sim != NULL);
    n = pillsGetNumPills(&sim->sim.pb);
    UT_ASSERT_MSG(n >= 2, "map has %u pills, this case needs two", (unsigned)n);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_OWNER;

    /* Op index 0 is pill 1 in the list. Handing a neutral pill to a player is
       the capture a tank driving over it would make, and it is announced. */
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, NEUTRAL,
                PILLS_MAX_ARMOUR, FALSE);
    pbDrainEvents(sim);
    op.u.pillSetOwner.pill = 0;
    op.u.pillSetOwner.owner = PB_SLOT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(pbPill(sim, 0).owner == PB_SLOT, "pill 1 owner %u",
                  (unsigned)pbPill(sim, 0).owner);
    UT_ASSERT_MSG(pbCountEvents(sim, EVENT_PILL_CAPTURED) == 1,
                  "the capture was not published");

    /* A seat nobody is sitting in is a legal owner: that is how a scenario
       leaves the wave's guns on the map with nobody behind them. */
    UT_ASSERT_MSG(!sim->playerConnected[PB_NOBODY],
                  "slot %u is occupied, so it proves nothing here",
                  (unsigned)PB_NOBODY);
    op.u.pillSetOwner.owner = PB_NOBODY;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "an empty slot was refused as a pill owner");
    UT_ASSERT_MSG(pbPill(sim, 0).owner == PB_NOBODY, "pill 1 owner %u",
                  (unsigned)pbPill(sim, 0).owner);

    /* And so is nobody at all. */
    op.u.pillSetOwner.owner = NEUTRAL;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(pbPill(sim, 0).owner == NEUTRAL);

    /* A byte that is neither a seat nor NEUTRAL is not an owner. */
    op.u.pillSetOwner.owner = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.pillSetOwner.owner = 200;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(pbPill(sim, 0).owner == NEUTRAL,
                  "a refused op still wrote the owner");

    /* A pill in a tank answers to whoever is carrying it. */
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, NEUTRAL, 0, TRUE);
    op.u.pillSetOwner.owner = PB_SLOT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_CARRIED);
    UT_ASSERT_MSG(pbPill(sim, 0).owner == NEUTRAL,
                  "a refused op still wrote the owner");
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, NEUTRAL,
                PILLS_MAX_ARMOUR, FALSE);

    /* The last index in op terms is the last pill in the list. */
    op.u.pillSetOwner.pill = (BYTE)(n - 1);
    op.u.pillSetOwner.owner = PB_SLOT;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the last pill was refused");
    UT_ASSERT_MSG(pbPill(sim, (BYTE)(n - 1)).owner == PB_SLOT,
                  "the last pill's owner is %u",
                  (unsigned)pbPill(sim, (BYTE)(n - 1)).owner);

    /* One past the last, and a long way past it. */
    op.u.pillSetOwner.pill = n;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.pillSetOwner.pill = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    serverSimDestroy(sim);
    return 0;
}

/* ── Pill: set armour ────────────────────────────────────────────── */

int run_scenario_pill_set_armour(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE n;

    UT_ASSERT(sim != NULL);
    n = pillsGetNumPills(&sim->sim.pb);
    UT_ASSERT_MSG(n >= 2, "map has %u pills, this case needs two", (unsigned)n);

    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, PB_SLOT,
                PILLS_MAX_ARMOUR, FALSE);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_ARMOUR;
    op.u.pillSetArmour.pill = 0;

    /* Zero is a dead pill lying on the ground, which is a thing a script asks
       for: a wreck somebody has to walk out and repair. */
    op.u.pillSetArmour.armour = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(pbPill(sim, 0).armour == 0, "pill 1 armour %u",
                  (unsigned)pbPill(sim, 0).armour);

    /* A full pill, and the owner it had is left alone. */
    op.u.pillSetArmour.armour = PILLS_MAX_ARMOUR;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(pbPill(sim, 0).armour == PILLS_MAX_ARMOUR);
    UT_ASSERT_MSG(pbPill(sim, 0).owner == PB_SLOT, "the owner was rewritten");

    /* Past what a pill can hold is a mistake worth reporting, and nothing is
       written when it is. */
    op.u.pillSetArmour.armour = PILLS_MAX_ARMOUR + 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.pillSetArmour.armour = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(pbPill(sim, 0).armour == PILLS_MAX_ARMOUR,
                  "a refused op still wrote the armour");

    /* A pill in a tank has no armour on the map to write. */
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, PB_SLOT, 4, TRUE);
    op.u.pillSetArmour.armour = 9;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_CARRIED);
    UT_ASSERT_MSG(pbPill(sim, 0).armour == 4,
                  "a refused op still wrote the armour");
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, PB_SLOT, 4, FALSE);

    /* The last index in op terms. */
    op.u.pillSetArmour.pill = (BYTE)(n - 1);
    op.u.pillSetArmour.armour = 7;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the last pill was refused");
    UT_ASSERT_MSG(pbPill(sim, (BYTE)(n - 1)).armour == 7,
                  "the last pill's armour is %u",
                  (unsigned)pbPill(sim, (BYTE)(n - 1)).armour);

    /* One past the last, and a long way past it. */
    op.u.pillSetArmour.pill = n;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.pillSetArmour.pill = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    serverSimDestroy(sim);
    return 0;
}

/* ── Pill: set speed ─────────────────────────────────────────────── */

int run_scenario_pill_set_speed(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE n;

    UT_ASSERT(sim != NULL);
    n = pillsGetNumPills(&sim->sim.pb);
    UT_ASSERT_MSG(n >= 2, "map has %u pills, this case needs two", (unsigned)n);

    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, PB_SLOT,
                PILLS_MAX_ARMOUR, FALSE);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_SPEED;
    op.u.pillSetSpeed.pill = 0;

    /* The fastest a pill fires. Anything under the resting rate arms the
       cooldown, which is the setter's own doing and not the arm's. */
    op.u.pillSetSpeed.speed = PILLBOX_MAX_FIRERATE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(pbPill(sim, 0).speed == PILLBOX_MAX_FIRERATE,
                  "pill 1 speed %u", (unsigned)pbPill(sim, 0).speed);
    UT_ASSERT_MSG(pbPill(sim, 0).coolDown == PILLBOX_COOLDOWN_TIME,
                  "an angry pill should have its cooldown armed, it reads %u",
                  (unsigned)pbPill(sim, 0).coolDown);

    /* And the rate an untouched one sits at. */
    op.u.pillSetSpeed.speed = PILLBOX_ATTACK_NORMAL;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(pbPill(sim, 0).speed == PILLBOX_ATTACK_NORMAL);

    /* Either side of the attack interval is out of range, and nothing is
       written when it is. */
    op.u.pillSetSpeed.speed = PILLBOX_MAX_FIRERATE - 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.pillSetSpeed.speed = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.pillSetSpeed.speed = PILLBOX_ATTACK_NORMAL + 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.pillSetSpeed.speed = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(pbPill(sim, 0).speed == PILLBOX_ATTACK_NORMAL,
                  "a refused op still wrote the speed");

    /* Unlike the other three pill arms this one takes a carried pill: the rate
       rides back out with it and is what it fires at when it is put down. */
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, PB_SLOT, 0, TRUE);
    op.u.pillSetSpeed.speed = 20;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "a carried pill should still take a rate");
    UT_ASSERT(pbPill(sim, 0).speed == 20);
    UT_ASSERT_MSG(pbPill(sim, 0).inTank, "the pill was taken out of the tank");
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, PB_SLOT,
                PILLS_MAX_ARMOUR, FALSE);

    /* The last index in op terms. */
    op.u.pillSetSpeed.pill = (BYTE)(n - 1);
    op.u.pillSetSpeed.speed = 50;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the last pill was refused");
    UT_ASSERT_MSG(pbPill(sim, (BYTE)(n - 1)).speed == 50,
                  "the last pill's speed is %u",
                  (unsigned)pbPill(sim, (BYTE)(n - 1)).speed);

    /* One past the last, and a long way past it. */
    op.u.pillSetSpeed.pill = n;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.pillSetSpeed.pill = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    serverSimDestroy(sim);
    return 0;
}

/* ── Pill: move ──────────────────────────────────────────────────── */

int run_scenario_pill_move(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE n;
    BYTE gx = 0, gy = 0, hx = 0, hy = 0;
    base firstBase;

    UT_ASSERT(sim != NULL);
    n = pillsGetNumPills(&sim->sim.pb);
    UT_ASSERT_MSG(n >= 2, "map has %u pills, this case needs two", (unsigned)n);
    UT_ASSERT_MSG(basesGetNumBases(&sim->sim.bs) >= 1, "map has no bases");

    UT_ASSERT_MSG(pbFindTile(sim, GRASS, 0xFF, 0xFF, &gx, &gy),
                  "map has no free grass");
    UT_ASSERT_MSG(pbFindTile(sim, GRASS, gx, gy, &hx, &hy),
                  "map has only one free grass square");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_MOVE;

    /* Op index 0 lands on the named square, keeping everything else. */
    pbPlacePill(sim, 0, pbPill(sim, 0).x, pbPill(sim, 0).y, PB_SLOT, 11, FALSE);
    op.u.pillMove.pill = 0;
    op.u.pillMove.x = gx;
    op.u.pillMove.y = gy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(pbPill(sim, 0).x == gx && pbPill(sim, 0).y == gy,
                  "pill 1 landed at %u,%u", (unsigned)pbPill(sim, 0).x,
                  (unsigned)pbPill(sim, 0).y);
    UT_ASSERT_MSG(pbPill(sim, 0).armour == 11, "the armour was rewritten");
    UT_ASSERT_MSG(pbPill(sim, 0).owner == PB_SLOT, "the owner was rewritten");

    /* The border reads as deep sea, so it is off the map as far as an op is
       concerned. */
    op.u.pillMove.x = 0;
    op.u.pillMove.y = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.pillMove.x = MAP_MINE_EDGE_RIGHT;
    op.u.pillMove.y = 100;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    UT_ASSERT_MSG(pbPill(sim, 0).x == gx, "a refused move still moved the pill");

    /* Ground that will not hold a pill, the same list the builder's place-pill
       arm turns down. */
    mapSetPos(&sim->sim, &sim->sim.mp, hx, hy, BUILDING, FALSE, FALSE);
    op.u.pillMove.x = hx;
    op.u.pillMove.y = hy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    mapSetPos(&sim->sim, &sim->sim.mp, hx, hy, RIVER, FALSE, FALSE);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    mapSetPos(&sim->sim, &sim->sim.mp, hx, hy, GRASS, FALSE, FALSE);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "grass under the same square should take the pill");
    UT_ASSERT(pbPill(sim, 0).x == hx && pbPill(sim, 0).y == hy);

    /* A base is in the way. */
    memset(&firstBase, 0, sizeof(firstBase));
    basesGetBase(&sim->sim.bs, &firstBase, 1);
    op.u.pillMove.x = firstBase.x;
    op.u.pillMove.y = firstBase.y;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                  "a base square should not take a pill");

    /* And so is another pill. */
    pbPlacePill(sim, 1, gx, gy, NEUTRAL, PILLS_MAX_ARMOUR, FALSE);
    op.u.pillMove.x = gx;
    op.u.pillMove.y = gy;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                  "a square with a pill on it should not take a second one");
    UT_ASSERT_MSG(pbPill(sim, 0).x == hx,
                  "a refused move still moved the pill");

    /* A pill in a tank is nowhere on the map, so there is no move to make. */
    pbPlacePill(sim, 0, hx, hy, PB_SLOT, 0, TRUE);
    op.u.pillMove.x = gx;
    op.u.pillMove.y = gy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_CARRIED);
    pbPlacePill(sim, 0, hx, hy, PB_SLOT, PILLS_MAX_ARMOUR, FALSE);

    /* The last index in op terms, onto a square of its own. */
    {
        BYTE lx = 0, ly = 0;
        UT_ASSERT_MSG(pbFindTile(sim, GRASS, 0xFF, 0xFF, &lx, &ly),
                      "map has no free grass left");
        op.u.pillMove.pill = (BYTE)(n - 1);
        op.u.pillMove.x = lx;
        op.u.pillMove.y = ly;
        UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                      "the last pill was refused");
        UT_ASSERT_MSG(pbPill(sim, (BYTE)(n - 1)).x == lx &&
                      pbPill(sim, (BYTE)(n - 1)).y == ly,
                      "the last pill landed at %u,%u",
                      (unsigned)pbPill(sim, (BYTE)(n - 1)).x,
                      (unsigned)pbPill(sim, (BYTE)(n - 1)).y);
    }

    /* One past the last, and a long way past it. */
    op.u.pillMove.pill = n;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.pillMove.pill = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    serverSimDestroy(sim);
    return 0;
}

/* ── Base: set owner ─────────────────────────────────────────────── */

int run_scenario_base_set_owner(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE nb;

    UT_ASSERT(sim != NULL);
    nb = basesGetNumBases(&sim->sim.bs);
    UT_ASSERT_MSG(nb >= 2, "map has %u bases, this case needs two",
                  (unsigned)nb);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_BASE_SET_OWNER;
    op.u.baseSetOwner.base = 0;
    op.u.baseSetOwner.keepStock = false;

    /* Start from nobody, so the first hand-over is a capture rather than a
       theft and the stock stays where the fixture put it. */
    op.u.baseSetOwner.owner = NEUTRAL;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(pbBase(sim, 0).owner == NEUTRAL);

    /* Op index 0 is base 1 in the list, and the hand-over is announced. */
    pbDrainEvents(sim);
    op.u.baseSetOwner.owner = PB_SLOT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(pbBase(sim, 0).owner == PB_SLOT, "base 1 owner %u",
                  (unsigned)pbBase(sim, 0).owner);
    UT_ASSERT_MSG(pbCountEvents(sim, EVENT_BASE_CAPTURED) == 1,
                  "the capture was not published");

    /* A seat nobody is sitting in is a legal owner here too. */
    UT_ASSERT_MSG(!sim->playerConnected[PB_NOBODY],
                  "slot %u is occupied, so it proves nothing here",
                  (unsigned)PB_NOBODY);
    op.u.baseSetOwner.owner = PB_NOBODY;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "an empty slot was refused as a base owner");
    UT_ASSERT_MSG(pbBase(sim, 0).owner == PB_NOBODY, "base 1 owner %u",
                  (unsigned)pbBase(sim, 0).owner);

    /* A byte that is neither a seat nor NEUTRAL is not an owner, and nothing
       is written when one arrives. */
    op.u.baseSetOwner.owner = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.baseSetOwner.owner = 200;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(pbBase(sim, 0).owner == PB_NOBODY,
                  "a refused op still wrote the owner");

    /* The last index in op terms is the last base in the list. */
    op.u.baseSetOwner.base = (BYTE)(nb - 1);
    op.u.baseSetOwner.owner = PB_SLOT;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the last base was refused");
    UT_ASSERT_MSG(pbBase(sim, (BYTE)(nb - 1)).owner == PB_SLOT,
                  "the last base's owner is %u",
                  (unsigned)pbBase(sim, (BYTE)(nb - 1)).owner);

    /* One past the last, and a long way past it. */
    op.u.baseSetOwner.base = nb;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.baseSetOwner.base = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    serverSimDestroy(sim);
    return 0;
}

/* ── Base: the drain, and the flag that stops it ─────────────────── */

/* Taking a base off one player and giving it to another empties it, because
 * that is what a capture does and the arm inherits the capture. keepStock is
 * the one thing the op adds: a scenario re-dealing the map every wave hands
 * bases over with what is in them. Either way the capture is announced —
 * which is the half the older migrate flag could not express, because it
 * suppressed the drain and the announcement together. */
int run_scenario_base_owner_keep_stock(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp owner;
    ScenarioOp stock;
    base b;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, PB_OTHER, "Other", false);
    UT_ASSERT(basesGetNumBases(&sim->sim.bs) >= 1);
    UT_ASSERT(sim->playerConnected[PB_SLOT] && sim->playerConnected[PB_OTHER]);

    memset(&owner, 0, sizeof(owner));
    owner.type = SCN_OP_BASE_SET_OWNER;
    owner.u.baseSetOwner.base = 0;

    memset(&stock, 0, sizeof(stock));
    stock.type = SCN_OP_BASE_SET_STOCK;
    stock.u.baseSetStock.base = 0;
    stock.u.baseSetStock.armour = 50;
    stock.u.baseSetStock.shells = 40;
    stock.u.baseSetStock.mines  = 30;

    /* One real owner, holding something. */
    owner.u.baseSetOwner.owner = PB_SLOT;
    owner.u.baseSetOwner.keepStock = false;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &owner, NULL) == SCN_OP_OK);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &stock, NULL) == SCN_OP_OK);

    /* Handed to the other one without the flag: emptied, and announced. */
    pbDrainEvents(sim);
    owner.u.baseSetOwner.owner = PB_OTHER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &owner, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.owner == PB_OTHER, "base 1 owner %u", (unsigned)b.owner);
    UT_ASSERT_MSG(b.armour == 0 && b.shells == 0 && b.mines == 0,
                  "a base stolen from a player kept %u/%u/%u",
                  (unsigned)b.armour, (unsigned)b.shells, (unsigned)b.mines);
    UT_ASSERT_MSG(pbCountEvents(sim, EVENT_BASE_CAPTURED) == 1,
                  "a drained hand-over was not announced");

    /* Filled again, then handed back with the flag: kept, and still
       announced. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &stock, NULL) == SCN_OP_OK);
    pbDrainEvents(sim);
    owner.u.baseSetOwner.owner = PB_SLOT;
    owner.u.baseSetOwner.keepStock = true;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &owner, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.owner == PB_SLOT, "base 1 owner %u", (unsigned)b.owner);
    UT_ASSERT_MSG(b.armour == 50 && b.shells == 40 && b.mines == 30,
                  "keepStock still emptied the base: %u/%u/%u",
                  (unsigned)b.armour, (unsigned)b.shells, (unsigned)b.mines);
    UT_ASSERT_MSG(pbCountEvents(sim, EVENT_BASE_CAPTURED) == 1,
                  "a kept hand-over was not announced");

    /* Neutralising a base takes nothing off it either, with the flag or
       without: only a player taking one off another player does. */
    owner.u.baseSetOwner.owner = NEUTRAL;
    owner.u.baseSetOwner.keepStock = false;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &owner, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.owner == NEUTRAL, "base 1 owner %u", (unsigned)b.owner);
    UT_ASSERT_MSG(b.armour == 50 && b.shells == 40 && b.mines == 30,
                  "neutralising emptied the base: %u/%u/%u",
                  (unsigned)b.armour, (unsigned)b.shells, (unsigned)b.mines);

    serverSimDestroy(sim);
    return 0;
}

/* ── Base: set stock ─────────────────────────────────────────────── */

int run_scenario_base_set_stock(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE nb;
    base b;

    UT_ASSERT(sim != NULL);
    nb = basesGetNumBases(&sim->sim.bs);
    UT_ASSERT_MSG(nb >= 2, "map has %u bases, this case needs two",
                  (unsigned)nb);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_BASE_SET_STOCK;
    op.u.baseSetStock.base = 0;

    /* Op index 0 is base 1 in the list. */
    op.u.baseSetStock.armour = 10;
    op.u.baseSetStock.shells = 20;
    op.u.baseSetStock.mines  = 30;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.armour == 10 && b.shells == 20 && b.mines == 30,
                  "base 1 holds %u/%u/%u", (unsigned)b.armour,
                  (unsigned)b.shells, (unsigned)b.mines);

    /* The same op again says the same thing rather than adding to it — this
       is a setter, not the refuel beside it. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.armour == 10 && b.shells == 20 && b.mines == 30,
                  "a second write added instead of setting: %u/%u/%u",
                  (unsigned)b.armour, (unsigned)b.shells, (unsigned)b.mines);

    /* More than a base can hold lands on a full load, so "fill it" can be
       written as a number nobody has to look up. */
    op.u.baseSetStock.armour = 500;
    op.u.baseSetStock.shells = 500;
    op.u.baseSetStock.mines  = 500;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.armour == BASE_FULL_ARMOUR && b.shells == BASE_FULL_SHELLS &&
                  b.mines == BASE_FULL_MINES,
                  "a full load reads %u/%u/%u", (unsigned)b.armour,
                  (unsigned)b.shells, (unsigned)b.mines);

    /* -1 leaves one alone. */
    op.u.baseSetStock.armour = 5;
    op.u.baseSetStock.shells = 6;
    op.u.baseSetStock.mines  = 7;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    op.u.baseSetStock.armour = -1;
    op.u.baseSetStock.shells = 60;
    op.u.baseSetStock.mines  = -1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.armour == 5 && b.shells == 60 && b.mines == 7,
                  "base 1 holds %u/%u/%u", (unsigned)b.armour,
                  (unsigned)b.shells, (unsigned)b.mines);

    /* Zero empties one, which is not the same as leaving it alone. */
    op.u.baseSetStock.armour = 0;
    op.u.baseSetStock.shells = -1;
    op.u.baseSetStock.mines  = -1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.armour == 0 && b.shells == 60 && b.mines == 7,
                  "base 1 holds %u/%u/%u", (unsigned)b.armour,
                  (unsigned)b.shells, (unsigned)b.mines);

    /* -1 is the only negative the payload means anything by; any other is a
       script that has worked something out wrong, and nothing is written. */
    op.u.baseSetStock.armour = -2;
    op.u.baseSetStock.shells = 1;
    op.u.baseSetStock.mines  = 1;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.baseSetStock.armour = -1;
    op.u.baseSetStock.shells = -300;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.baseSetStock.shells = -1;
    op.u.baseSetStock.mines  = -2;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    b = pbBase(sim, 0);
    UT_ASSERT_MSG(b.armour == 0 && b.shells == 60 && b.mines == 7,
                  "a refused op still wrote a stock: %u/%u/%u",
                  (unsigned)b.armour, (unsigned)b.shells, (unsigned)b.mines);

    /* The last index in op terms. */
    op.u.baseSetStock.base = (BYTE)(nb - 1);
    op.u.baseSetStock.armour = 12;
    op.u.baseSetStock.shells = 13;
    op.u.baseSetStock.mines  = 14;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the last base was refused");
    b = pbBase(sim, (BYTE)(nb - 1));
    UT_ASSERT_MSG(b.armour == 12 && b.shells == 13 && b.mines == 14,
                  "the last base holds %u/%u/%u", (unsigned)b.armour,
                  (unsigned)b.shells, (unsigned)b.mines);

    /* One past the last, and a long way past it. */
    op.u.baseSetStock.base = nb;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    op.u.baseSetStock.base = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);

    serverSimDestroy(sim);
    return 0;
}

/* ── The records the arms leave behind ───────────────────────────── */

/* The arms write no record of their own: each change rides the record the
 * function it reused already writes. This drives all five through a real
 * recording and reads the file back two ways — the .wbv's own event stream for
 * the exact logitem and payload, and the production reader for the world it
 * rebuilds — so a record that never reached the file shows up twice.
 *
 * The walk below is the writer-side wire shape (LOG_EVENT / LOG_EVENT_LONG,
 * then per event a type byte, a big-endian u16 length and that many payload
 * bytes), the same shape test_scenario_builder_arms.c walks. */

typedef struct {
    bool    found;
    uint8_t payload[8];
    int     payloadLen;
} PbLogHit;

static int pbReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills,
 * bases and starts, the map runs up to the deep-sea terminator, then
 * MAX_TANKS player blocks. Plaintext, not length-framed. */
static bool pbSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = pbReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = pbReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = pbReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    while (1) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = pbReadByte(buf, len, p);
        y    = pbReadByte(buf, len, p + 1);
        sx   = pbReadByte(buf, len, p + 2);
        ex   = pbReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = pbReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and report the LAST event of type `want` whose
 * first payload byte is `first`. The last rather than the first because
 * several of these opcodes name an item the round writes more than once —
 * pillsSetPill writes all four pill records every time it is called — and
 * what each case is asking about is the state the world was left in.
 * Returns false if the stream did not end on a clean LOG_QUIT. */
static bool pbFindLogged(const char *path, uint8_t want, uint8_t first,
                         PbLogHit *hit) {
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
        int code = pbReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (pbReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!pbSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = pbReadByte(buf, len, pos);
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
                if (ev == want && plen > 0 && buf[payloadStart] == first) {
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

int run_scenario_pill_base_arm_records(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    PbLogHit hit;
    BYTE gx = 0, gy = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnPillBaseArms", "Tester"),
                  "could not start recording");
    sim = h.sim;
    UT_ASSERT(pillsGetNumPills(&sim->sim.pb) > 0);
    UT_ASSERT(basesGetNumBases(&sim->sim.bs) > 0);

    /* Let the round settle after the opening snapshot. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(pbFindTile(sim, GRASS, 0xFF, 0xFF, &gx, &gy),
                  "map has no free grass");

    /* Pill 1: armour, then the square, then the owner. Set-owner goes last so
       the owner record the walk finds is pillsSetPillOwner's rather than the
       one pillsSetPill writes alongside every other change. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_ARMOUR;
    op.u.pillSetArmour.pill = 0;
    op.u.pillSetArmour.armour = 6;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_MOVE;
    op.u.pillMove.pill = 0;
    op.u.pillMove.x = gx;
    op.u.pillMove.y = gy;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_OWNER;
    op.u.pillSetOwner.pill = 0;
    op.u.pillSetOwner.owner = PB_SLOT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    replayHarnessTick(&h, 2);

    /* Base 1: the owner first, then the explicit stock override. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_BASE_SET_OWNER;
    op.u.baseSetOwner.base = 0;
    op.u.baseSetOwner.owner = PB_SLOT;
    op.u.baseSetOwner.keepStock = false;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_BASE_SET_STOCK;
    op.u.baseSetStock.base = 0;
    op.u.baseSetStock.armour = 40;
    op.u.baseSetStock.shells = 30;
    op.u.baseSetStock.mines  = 20;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    /* Two ticks so the last change reaches the file. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* log_PillSetOwner: index, owner, migrate. */
    UT_ASSERT_MSG(pbFindLogged(h.path, (uint8_t)log_PillSetOwner, 0, &hit),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hit.found, "log_PillSetOwner is not in the recording");
    UT_ASSERT_MSG(hit.payloadLen == 3, "log_PillSetOwner payload is %d bytes",
                  hit.payloadLen);
    UT_ASSERT_MSG(hit.payload[1] == PB_SLOT, "log_PillSetOwner names owner %u",
                  (unsigned)hit.payload[1]);
    UT_ASSERT_MSG(hit.payload[2] == FALSE,
                  "log_PillSetOwner calls a scripted hand-over a migration");

    /* log_PillSetHealth: the index, then the armour in a byte of its own. */
    UT_ASSERT(pbFindLogged(h.path, (uint8_t)log_PillSetHealth, 0, &hit));
    UT_ASSERT_MSG(hit.found, "log_PillSetHealth is not in the recording");
    UT_ASSERT_MSG(hit.payloadLen == 2, "log_PillSetHealth payload is %d bytes",
                  hit.payloadLen);
    UT_ASSERT_MSG(hit.payload[1] == 6, "log_PillSetHealth names armour %u",
                  (unsigned)hit.payload[1]);

    /* log_PillSetPlace: index, x, y. */
    UT_ASSERT(pbFindLogged(h.path, (uint8_t)log_PillSetPlace, 0, &hit));
    UT_ASSERT_MSG(hit.found, "log_PillSetPlace is not in the recording");
    UT_ASSERT_MSG(hit.payloadLen == 3, "log_PillSetPlace payload is %d bytes",
                  hit.payloadLen);
    UT_ASSERT_MSG(hit.payload[1] == gx && hit.payload[2] == gy,
                  "log_PillSetPlace names %u,%u, wanted %u,%u",
                  (unsigned)hit.payload[1], (unsigned)hit.payload[2],
                  (unsigned)gx, (unsigned)gy);

    /* log_BaseSetOwner: index, owner, migrate. */
    UT_ASSERT(pbFindLogged(h.path, (uint8_t)log_BaseSetOwner, 0, &hit));
    UT_ASSERT_MSG(hit.found, "log_BaseSetOwner is not in the recording");
    UT_ASSERT_MSG(hit.payloadLen == 3, "log_BaseSetOwner payload is %d bytes",
                  hit.payloadLen);
    UT_ASSERT_MSG(hit.payload[1] == PB_SLOT, "log_BaseSetOwner names owner %u",
                  (unsigned)hit.payload[1]);
    UT_ASSERT_MSG(hit.payload[2] == FALSE,
                  "log_BaseSetOwner calls a scripted hand-over a migration");

    /* log_BaseSetStock: index, shells, mines, armour — the order the periodic
       stock update writes them in. */
    UT_ASSERT(pbFindLogged(h.path, (uint8_t)log_BaseSetStock, 0, &hit));
    UT_ASSERT_MSG(hit.found, "log_BaseSetStock is not in the recording");
    UT_ASSERT_MSG(hit.payloadLen == 4, "log_BaseSetStock payload is %d bytes",
                  hit.payloadLen);
    UT_ASSERT_MSG(hit.payload[1] == 30 && hit.payload[2] == 20 &&
                  hit.payload[3] == 40,
                  "log_BaseSetStock names %u/%u/%u shells/mines/armour",
                  (unsigned)hit.payload[1], (unsigned)hit.payload[2],
                  (unsigned)hit.payload[3]);

    /* And the world the production reader rebuilds from those records agrees
       with the world the ops left behind. */
    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode the recording");
    UT_ASSERT_MSG(replayHarnessCompare(&h),
                  "the replay disagrees with the sim: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}
