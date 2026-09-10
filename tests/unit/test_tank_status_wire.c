/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * The destroyed state on the wire (test_tank_status_wire.c).
 *
 * A snapshot carries a tank's death in TankSnapshot.tankStatus, as two bits
 * that answer different questions: TANK_STATUS_DEAD is "in the respawn wait"
 * and TANK_STATUS_DESTROYED is "destroyed and not yet respawned". The wait
 * ends on its own, the respawn waits for a start position, so a tank can be
 * destroyed with the wait already over. Both bits go to every recipient.
 * armour is a plain 0..TANK_FULL_ARMOUR value that only the owner receives
 * and never carries a death sentinel.
 *
 * bits:       the encoder sets each bit from the tank's own state, for the
 *             owner's row and for the row another player receives, and the
 *             armour byte stays inside 0..TANK_FULL_ARMOUR throughout — in
 *             particular for a destroyed tank whose wait has run out, which
 *             the old armour sentinel was the only way to send.
 * round trip: a tank held destroyed with its wait over reads as dead on the
 *             client after a real snapshot, with armour 0 rather than a
 *             sentinel, and the respawn edge fires when the bit clears.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "input_packet.h"
#include "tank.h"
#include "test_harness.h"

/* tankStatus and armour of `player`'s row in a snapshot built for
 * `recipient`. Returns false when the row is missing or a stub, so a caller
 * cannot mistake an absent row for a cleared byte. */
static bool tsw_row(ServerSim *sim, BYTE recipient, BYTE player,
                    uint8_t *status, uint8_t *armour) {
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];
    int i;

    serverSimBuildSnapshot(sim, recipient, &hdr, tk, MAX_TANKS,
                           sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS,
                           ev, MAX_SNAPSHOT_EVENTS, true);
    for (i = 0; i < hdr.tankCount; i++) {
        if ((tk[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK) == player &&
            (tk[i].playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0) {
            *status = tk[i].tankStatus;
            *armour = tk[i].armour;
            return true;
        }
    }
    return false;
}

/* Assert the row player 0 gets for itself, and the row player 1 gets for
 * player 0, both carry exactly `wantBits` of the two death bits, and that
 * the owner's armour byte is `wantArmour` while the non-owner's is 0. */
#define TSW_DEATH_BITS (TANK_STATUS_DEAD | TANK_STATUS_DESTROYED)
static const char *tsw_check(ServerSim *sim, uint8_t wantBits, uint8_t wantArmour) {
    static char why[160];
    uint8_t status = 0;
    uint8_t armour = 0;

    if (!tsw_row(sim, 0, 0, &status, &armour)) {
        return "player 0's own row is missing";
    }
    if ((status & TSW_DEATH_BITS) != wantBits) {
        snprintf(why, sizeof(why), "owner row death bits 0x%02x, wanted 0x%02x",
                 status & TSW_DEATH_BITS, wantBits);
        return why;
    }
    if (armour != wantArmour) {
        snprintf(why, sizeof(why), "owner row armour %u, wanted %u",
                 (unsigned) armour, (unsigned) wantArmour);
        return why;
    }
    if (armour > TANK_FULL_ARMOUR) {
        snprintf(why, sizeof(why), "owner row armour %u is outside 0..%u",
                 (unsigned) armour, (unsigned) TANK_FULL_ARMOUR);
        return why;
    }
    if (!tsw_row(sim, 1, 0, &status, &armour)) {
        return "player 0's row in player 1's snapshot is missing";
    }
    if ((status & TSW_DEATH_BITS) != wantBits) {
        snprintf(why, sizeof(why), "non-owner row death bits 0x%02x, wanted 0x%02x",
                 status & TSW_DEATH_BITS, wantBits);
        return why;
    }
    if (armour != 0) {
        snprintf(why, sizeof(why), "non-owner row armour %u, wanted 0 (owner only)",
                 (unsigned) armour);
        return why;
    }
    return NULL;
}

int run_tank_status_wire_bits(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    const char *why;
    uint8_t status = 0;
    uint8_t armour = 0;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    /* Alive: neither bit, full armour to the owner. */
    tankSetDestroyed(&gs->tanks[0], FALSE);
    tankSetArmour(&gs->tanks[0], TANK_FULL_ARMOUR);
    tankSetDeathWait(&gs->tanks[0], 0);
    why = tsw_check(sim, 0, TANK_FULL_ARMOUR);
    UT_ASSERT_MSG(why == NULL, "alive: %s", why);

    /* Just destroyed: both bits, armour a real 0. */
    tankSetDestroyed(&gs->tanks[0], TRUE);
    tankSetArmour(&gs->tanks[0], 0);
    tankSetDeathWait(&gs->tanks[0], TANK_DEATH_WAIT);
    why = tsw_check(sim, TANK_STATUS_DEAD | TANK_STATUS_DESTROYED, 0);
    UT_ASSERT_MSG(why == NULL, "destroyed, waiting: %s", why);

    /* Wait over, no start found yet: destroyed but not dead. This is the
     * state the armour sentinel used to be the only way to send, and the one
     * a non-owner could never see at all. */
    tankSetDeathWait(&gs->tanks[0], 0);
    why = tsw_check(sim, TANK_STATUS_DESTROYED, 0);
    UT_ASSERT_MSG(why == NULL, "destroyed, wait over: %s", why);

    /* Respawned: neither bit again, so the encoding is not a one-way door. */
    tankSetDestroyed(&gs->tanks[0], FALSE);
    tankSetArmour(&gs->tanks[0], TANK_FULL_ARMOUR);
    why = tsw_check(sim, 0, TANK_FULL_ARMOUR);
    UT_ASSERT_MSG(why == NULL, "respawned: %s", why);

    /* The boat bit keeps the low nibble to itself. */
    tankSetOnBoat(&gs->tanks[0], TRUE);
    UT_ASSERT_MSG(tsw_row(sim, 0, 0, &status, &armour), "own row missing");
    UT_ASSERT_MSG((status & TANK_STATUS_ON_BOAT) != 0, "on-boat bit not set");
    UT_ASSERT_MSG((status & TSW_DEATH_BITS) == 0,
                  "boarding a boat set a death bit (0x%02x)", status);

    serverSimDestroy(sim);
    return 0;
}

/* Rounds of the input-pair warm-up before the client's tank is expected, and
 * the ceiling on how long to keep pumping. The local transport runs two server
 * half-steps per clientSimNetTick, so two tick numbers go per tick. */
#define TSW_WARMUP_TICKS     4
#define TSW_MAX_WARMUP_TICKS 32

static void tsw_pump(ClientSim *cs, BYTE me, uint32_t *inputTick) {
    InputPacket a;
    InputPacket b;

    memset(&a, 0, sizeof(a));
    a.tick      = *inputTick;
    a.playerNum = me;
    memset(&b, 0, sizeof(b));
    b.tick      = *inputTick + 1;
    b.playerNum = me;
    clientSimNetSendInput(cs, &a);
    clientSimNetSendInput(cs, &b);
    clientSimNetTick(cs);
    *inputTick += 2;
}

int run_tank_status_wire_start_find_round_trip(void) {
    ServerSim *sim = NULL;
    ClientSim *cs = NULL;
    GameSim *sgs;
    BYTE me;
    BYTE mx;
    BYTE my;
    BYTE shells, mines, armour, trees;
    uint32_t inputTick = 1;
    int i;

    sim = ut_make_running_sim("Host");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);
    UT_ASSERT_MSG(clientSimConnectLocal(cs, sim, "Joiner", "", 0, 0) == TRUE,
                  "clientSimConnectLocal failed");

    me = clientSimGetMyPlayerNum(cs);
    for (i = 0; i < TSW_MAX_WARMUP_TICKS; i++) {
        tsw_pump(cs, me, &inputTick);
        if (i + 1 >= TSW_WARMUP_TICKS &&
            clientSimGetMyTankMapPos(cs, &mx, &my) == TRUE) {
            break;
        }
    }
    UT_ASSERT_MSG(clientSimGetMyTankMapPos(cs, &mx, &my) == TRUE,
                  "the client still has no tank after the warm-up");
    UT_ASSERT_MSG(clientSimTankIsDead(cs) == FALSE,
                  "the client's freshly joined tank already reads as dead");

    /* Destroy the server's copy with its wait already over, and hold the
     * start search open so the server does not respawn it. Only the
     * destroyed bit can carry this state: the wait bit is clear and the
     * armour byte is an ordinary 0. */
    sgs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(sgs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(sgs->tanks[me] != NULL, "server tank for the joiner is NULL");
    tankSetArmour(&sgs->tanks[me], 0);
    tankSetDestroyed(&sgs->tanks[me], TRUE);
    tankSetDeathWait(&sgs->tanks[me], 0);
    sgs->inStartFind = TRUE;

    for (i = 0; i < TSW_WARMUP_TICKS; i++) {
        tsw_pump(cs, me, &inputTick);
    }
    UT_ASSERT_MSG(tankIsDestroyed(&sgs->tanks[me]) == TRUE,
                  "the server respawned the tank despite the open start search");
    UT_ASSERT_MSG(clientSimTankIsDead(cs) == TRUE,
                  "a destroyed tank with its wait over did not read as dead on "
                  "the client");
    UT_ASSERT_MSG(clientSimIsMyTankAlive(cs) == FALSE,
                  "a destroyed tank with its wait over still read as alive");
    clientSimGetTankStats(cs, &shells, &mines, &armour, &trees);
    UT_ASSERT_MSG(armour == 0,
                  "client armour for a destroyed tank was %u, wanted 0",
                  (unsigned) armour);

    /* Let the server respawn it: the bit clears, and the client follows. */
    sgs->inStartFind = FALSE;
    for (i = 0; i < TSW_WARMUP_TICKS; i++) {
        tsw_pump(cs, me, &inputTick);
    }
    UT_ASSERT_MSG(tankIsDestroyed(&sgs->tanks[me]) == FALSE,
                  "the server did not respawn the tank once the start search "
                  "closed");
    UT_ASSERT_MSG(clientSimTankIsDead(cs) == FALSE,
                  "the client still reads as dead after the respawn snapshot");
    UT_ASSERT_MSG(clientSimIsMyTankAlive(cs) == TRUE,
                  "the client still reads as not alive after the respawn "
                  "snapshot");
    clientSimGetTankStats(cs, &shells, &mines, &armour, &trees);
    UT_ASSERT_MSG(armour > 0 && armour <= TANK_FULL_ARMOUR,
                  "client armour after respawn was %u, wanted 1..%u",
                  (unsigned) armour, (unsigned) TANK_FULL_ARMOUR);

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}
