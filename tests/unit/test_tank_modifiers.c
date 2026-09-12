/*
 * The per-tank modifier set: where it lives, how it reaches a client, and
 * the op that writes it.
 *
 * Six percentages ride on the tank, with 0 meaning classic. Nothing reads
 * them for behaviour yet — these cases hold the carrier: the set survives a
 * respawn and is cleared only by a fresh tank, the op replaces the whole set
 * and refuses the states it cannot write in, and the widened presence mask
 * carries the group without shifting any field that was already there.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server_sim_scenario.h"
#include "game_sim.h"
#include "tank.h"
#include "input_packet.h"
#include "wire_messages.h"
#include "transport_udp_internal.h"
#include "test_harness.h"

static void fillOp(ScenarioOp *op, BYTE slot,
                   uint8_t speed, uint8_t accel, uint8_t turn,
                   uint8_t reload, uint8_t dealt, uint8_t taken) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_TANK_SET_MODIFIERS;
    op->u.tankSetModifiers.slot = slot;
    op->u.tankSetModifiers.mods.speed = speed;
    op->u.tankSetModifiers.mods.accel = accel;
    op->u.tankSetModifiers.mods.turn = turn;
    op->u.tankSetModifiers.mods.reload = reload;
    op->u.tankSetModifiers.mods.dealt = dealt;
    op->u.tankSetModifiers.mods.taken = taken;
}

static int modsEqual(const TankModifiers *m,
                     uint8_t speed, uint8_t accel, uint8_t turn,
                     uint8_t reload, uint8_t dealt, uint8_t taken) {
    return m->speed == speed && m->accel == accel && m->turn == turn &&
           m->reload == reload && m->dealt == dealt && m->taken == taken;
}

/* The op writes all six values, and a second op replaces the set rather than
 * merging into it — a script that wants to change one value reads the tank
 * first. */
int run_tank_modifiers_op_writes_set(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    TankModifiers got;
    UT_ASSERT(sim != NULL);
    UT_ASSERT(sim->sim.tanks[0] != NULL);

    fillOp(&op, 0, 50, 60, 70, 80, 90, 100);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the set-modifiers op should have applied");
    tankGetModifiers(sim->sim.tanks[0], &got);
    UT_ASSERT_MSG(modsEqual(&got, 50, 60, 70, 80, 90, 100),
                  "got %u/%u/%u/%u/%u/%u, wanted 50/60/70/80/90/100",
                  got.speed, got.accel, got.turn, got.reload, got.dealt, got.taken);

    /* Zeroes in the second set must land as zeroes, not leave the old value
     * standing — that is the difference between replacing and merging. */
    fillOp(&op, 0, 200, 0, 0, 25, 0, 0);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    tankGetModifiers(sim->sim.tanks[0], &got);
    UT_ASSERT_MSG(modsEqual(&got, 200, 0, 0, 25, 0, 0),
                  "the second op merged instead of replacing: "
                  "%u/%u/%u/%u/%u/%u",
                  got.speed, got.accel, got.turn, got.reload, got.dealt, got.taken);

    serverSimDestroy(sim);
    return 0;
}

/* The state check runs ahead of the slot check, so the refusal a caller gets
 * does not depend on whether a tank happens to exist in a state that has
 * none. */
int run_tank_modifiers_op_refusals(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    tank saved;
    UT_ASSERT(sim != NULL);
    UT_ASSERT(sim->sim.tanks[0] != NULL);

    /* A slot nobody is in. */
    fillOp(&op, 15, 10, 10, 10, 10, 10, 10);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER,
                  "an unconnected slot should refuse with NO_SUCH_PLAYER");

    /* Past the end of the roster. */
    fillOp(&op, MAX_TANKS, 10, 10, 10, 10, 10, 10);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER);

    /* Connected, but no tank in the slot. */
    saved = sim->sim.tanks[0];
    sim->sim.tanks[0] = NULL;
    fillOp(&op, 0, 10, 10, 10, 10, 10, 10);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_PLAYER,
                  "a connected slot with no tank should refuse with NO_SUCH_PLAYER");
    sim->sim.tanks[0] = saved;

    /* Every state but running refuses, with a live tank in the slot. */
    {
        const ServerState notRunning[] = {
            serverStateLobby, serverStateCountdown, serverStateGameOver
        };
        size_t i;
        for (i = 0; i < sizeof(notRunning) / sizeof(notRunning[0]); i++) {
            sim->state = notRunning[i];
            fillOp(&op, 0, 10, 10, 10, 10, 10, 10);
            UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE,
                          "state %d should refuse with WRONG_STATE",
                          (int)notRunning[i]);
            /* And the state answer wins over the slot answer. */
            fillOp(&op, 15, 10, 10, 10, 10, 10, 10);
            UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE,
                          "state %d with an empty slot should still answer "
                          "WRONG_STATE", (int)notRunning[i]);
        }
    }

    sim->state = serverStateRunning;
    fillOp(&op, 0, 10, 10, 10, 10, 10, 10);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    serverSimDestroy(sim);
    return 0;
}

/* Zero the fields of every group absent from `combo`, keyed on the field list,
 * so a filled snapshot reduces to exactly the entry `combo` describes. */
#define TM_CLR_F(type, name)
#define TM_CLR_FMASK()
#define TM_CLR_FGROUP(bit, type, name)  if (!(combo & (bit))) s.name = 0;

/* Every presence combination round-trips, including the ninth bit. The point
 * is the widened mask: a field that shifted would show up as a mismatch in
 * some combination, and the core-only case pins the mask's own width. */
int run_tank_modifiers_wire_roundtrip(void) {
    TankSnapshot filled;
    int combo;

    memset(&filled, 0, sizeof(filled));
    filled.playerNum = 0x0A;   /* stub bit clear: a full entry */
    filled.worldX = 0x1234; filled.worldY = 0x5678;
    filled.angle = 0x9ABC;  filled.speed = 0x0DEF;
    filled.tankStatus = 0x21;
    filled.armour = 0x31; filled.shells = 0x32; filled.mines = 0x33;
    filled.trees = 0x34;  filled.gunsightLen = 0x35;
    filled.reload = 0x41;
    filled.deathWait = 0x5152;   /* past a byte: the group carries two */
    filled.lgmFrame = 0x61; filled.lgmMX = 0x62; filled.lgmMY = 0x63;
    filled.lgmPX = 0x64;    filled.lgmPY = 0x65;
    filled.firstLeft = 0x71; filled.firstRight = 0x72;
    filled.pingMs = 0x8081;
    filled.clientFlags = 0x91;
    filled.hiddenFlags = 0xA1;
    filled.modSpeed = 0xB1; filled.modAccel = 0xB2; filled.modTurn = 0xB3;
    filled.modReload = 0xB4; filled.modDealt = 0xB5; filled.modTaken = 0xB6;

    for (combo = 0; combo < 512; combo++) {
        TankSnapshot s, out;
        uint8_t buf[TANK_SNAPSHOT_WIRE_SIZE];
        int packed, consumed;

        s = filled;
        TANK_SNAPSHOT_FIELDS(TM_CLR_F, TM_CLR_FMASK, TM_CLR_FGROUP)

        memset(buf, 0, sizeof(buf));
        packed = packTankSnapshot(buf, &s);
        UT_ASSERT_MSG(packed > 0 && packed <= TANK_SNAPSHOT_WIRE_SIZE,
                      "combo 0x%03x packed %d bytes, outside the entry bound",
                      combo, packed);

        memset(&out, 0, sizeof(out));
        consumed = unpackTankSnapshot(buf, (size_t)packed, &out);
        UT_ASSERT_MSG(consumed == packed,
                      "combo 0x%03x unpacked %d of %d bytes",
                      combo, consumed, packed);
        UT_ASSERT_MSG(memcmp(&out, &s, sizeof(out)) == 0,
                      "combo 0x%03x did not round-trip — a field moved",
                      combo);
    }

    /* No group present: the core alone. One byte of playerNum, two of mask,
     * four U16s and the status byte. A one-byte mask would make this 11. */
    {
        TankSnapshot s, out;
        uint8_t buf[TANK_SNAPSHOT_WIRE_SIZE];
        memset(&s, 0, sizeof(s));
        s.playerNum = 0x0A;
        s.worldX = 0x1111; s.worldY = 0x2222;
        s.angle = 0x3333;  s.speed = 0x4444;
        s.tankStatus = 0x55;
        memset(buf, 0, sizeof(buf));
        UT_ASSERT_MSG(packTankSnapshot(buf, &s) == 12,
                      "a group-free entry should be the 12-byte core");
        memset(&out, 0, sizeof(out));
        UT_ASSERT(unpackTankSnapshot(buf, 12, &out) == 12);
        UT_ASSERT(memcmp(&out, &s, sizeof(out)) == 0);
    }

    /* The modifier group alone: six bytes past the core, and the values come
     * back as they went in. */
    {
        TankSnapshot s, out;
        uint8_t buf[TANK_SNAPSHOT_WIRE_SIZE];
        memset(&s, 0, sizeof(s));
        s.playerNum = 0x0A;
        s.modSpeed = 1; s.modAccel = 2; s.modTurn = 3;
        s.modReload = 4; s.modDealt = 5; s.modTaken = 255;
        memset(buf, 0, sizeof(buf));
        UT_ASSERT_MSG(packTankSnapshot(buf, &s) == 18,
                      "core plus the modifier group should be 18 bytes");
        memset(&out, 0, sizeof(out));
        UT_ASSERT(unpackTankSnapshot(buf, 18, &out) == 18);
        UT_ASSERT_MSG(out.modSpeed == 1 && out.modAccel == 2 &&
                      out.modTurn == 3 && out.modReload == 4 &&
                      out.modDealt == 5 && out.modTaken == 255,
                      "the modifier group did not survive the wire");
    }

    return 0;
}

/* A respawn reuses the tank object and resets its resources; the modifiers are
 * not resources and stay put. */
int run_tank_modifiers_survive_death(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    TankModifiers got;
    UT_ASSERT(sim != NULL);
    UT_ASSERT(sim->sim.tanks[0] != NULL);

    fillOp(&op, 0, 40, 41, 42, 43, 44, 45);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    tankSetArmour(&sim->sim.tanks[0], 0);
    tankSetDestroyed(&sim->sim.tanks[0], TRUE);
    tankDeath(&sim->sim, &sim->sim.tanks[0]);

    UT_ASSERT_MSG(!tankIsDestroyed(&sim->sim.tanks[0]),
                  "the respawn should have brought the tank back");
    tankGetModifiers(sim->sim.tanks[0], &got);
    UT_ASSERT_MSG(modsEqual(&got, 40, 41, 42, 43, 44, 45),
                  "the respawn cleared the modifiers: %u/%u/%u/%u/%u/%u",
                  got.speed, got.accel, got.turn, got.reload, got.dealt, got.taken);

    serverSimDestroy(sim);
    return 0;
}

/* A fresh tank is a classic tank. This is the only place the set is cleared —
 * the lobby return destroys every tank, so the next round's create is what
 * puts a slot back to stock. */
int run_tank_modifiers_cleared_at_create(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    TankModifiers got;
    UT_ASSERT(sim != NULL);
    UT_ASSERT(sim->sim.tanks[0] != NULL);

    tankGetModifiers(sim->sim.tanks[0], &got);
    UT_ASSERT_MSG(modsEqual(&got, 0, 0, 0, 0, 0, 0),
                  "a fresh tank should read all zeroes, got %u/%u/%u/%u/%u/%u",
                  got.speed, got.accel, got.turn, got.reload, got.dealt, got.taken);

    fillOp(&op, 0, 11, 22, 33, 44, 55, 66);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    tankDestroy(&sim->sim, &sim->sim.tanks[0]);
    sim->sim.tanks[0] = NULL;
    tankCreate(&sim->sim, &sim->sim.tanks[0]);
    UT_ASSERT(sim->sim.tanks[0] != NULL);

    tankGetModifiers(sim->sim.tanks[0], &got);
    UT_ASSERT_MSG(modsEqual(&got, 0, 0, 0, 0, 0, 0),
                  "tankCreate left a previous tank's modifiers standing: "
                  "%u/%u/%u/%u/%u/%u",
                  got.speed, got.accel, got.turn, got.reload, got.dealt, got.taken);

    serverSimDestroy(sim);
    return 0;
}
