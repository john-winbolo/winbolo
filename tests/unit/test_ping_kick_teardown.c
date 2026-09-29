/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Regression: a player removed mid-sim-frame leaves the half-step's world
 * update walking freed objects.
 *
 * simRunHalfStep snapshots the connected players into two local arrays —
 * tanksArray (tank pointers BY VALUE) and lgmPtrs (addresses of
 * sim->sim.lgmen[] slots) — plus a numTanks count, then hands all three to
 * shellsUpdate / tkExplosionUpdate / minesExpUpdate. serverSimRemovePlayer
 * frees the slot's tank and lgm and NULLs both array entries, so any removal
 * between the snapshot and the last consumer leaves the arrays dangling
 * (tanks) or pointing at a NULLed slot (lgmen), with numTanks still counting
 * the departed player.
 *
 * The ping kick was doing exactly that: transportUdpServerEnforcePing runs
 * from inside simRunHalfStep and used to call serverSimRemovePlayer inline.
 * The crash landed in shellsUpdate's shell-expiry loop at
 * `(*lgms[count])->playerNum` — a NULL deref faulting at offset 0x18, which is
 * where playerNum sits in struct lgmObj. Reported from a live 2.02 dedicated
 * server right after a player was dropped for high ping.
 *
 * Three tests — two for the crash above, one for the slot state the kick
 * leaves behind:
 *
 *  (a) shells_survive_cleared_lgm_slot — the proximate crash. Builds the same
 *      compacted arrays simRunHalfStep builds, clears one slot's lgm the way
 *      serverSimRemovePlayer does, and drives an expiring shell through
 *      shellsUpdate. Pre-fix this segfaults; the guard now tests *lgms[count],
 *      not just lgms[count] (which is a slot ADDRESS and never NULL).
 *
 *  (b) ping_kick_defers_teardown — the root cause. Drives the real
 *      enforcement path on a real connected client and asserts the kick does
 *      NOT free the player synchronously: the slot survives the enforce call
 *      and is torn down by transportUdpServerDrainPendingRemovals on the next
 *      tick, the same safe point the control-queue-overflow disconnect uses.
 *
 *  (c) ping_kick_clears_strikes_on_disconnect — the tally is per-slot, so a
 *      teardown that leaves it set hands the departing player's strikes to
 *      whoever takes the slot next. Drives the kick through the drain and
 *      reads the freed slot back.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"     /* sim->sim, playerConnected — T2 */
#include "server_sim_lifecycle.h"
#include "client_sim.h"
#include "client_net.h"              /* clientSimGetConnectState */
#include "client_connect_state.h"    /* CLIENT_CONNECT_CONNECTED */
#include "shells.h"                  /* shellsAddItem, shellsUpdate, SHELL_DEATH */
#include "lgm.h"                     /* lgmDestroy */
#include "transport_udp.h"           /* enforce/ping seam, PING_KICK_* */
#include "threads.h"
#include "everard_map.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* ── (a) shellsUpdate against a cleared lgm slot ─────────────────────── */

static ServerSim *make_sim_two_players(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimStartGame(sim);
    return sim;
}

int run_shells_survive_cleared_lgm_slot(void) {
    ServerSim *sim = make_sim_two_players();
    UT_ASSERT(sim != NULL);

    GameSim *gs = &sim->sim;
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "both tanks must exist for this test to mean anything");
    UT_ASSERT_MSG(gs->lgmen[0] != NULL && gs->lgmen[1] != NULL,
                  "both lgmen must exist before we clear one");

    /* The snapshot simRunHalfStep takes, built while BOTH players are live. */
    tank  tanksArray[MAX_TANKS];
    lgm  *lgmPtrs[MAX_TANKS];
    BYTE  numTanks = 0;
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && gs->tanks[i] != NULL) {
            tanksArray[numTanks] = gs->tanks[i];
            lgmPtrs[numTanks] = &gs->lgmen[i];
            numTanks++;
        }
    }
    UT_ASSERT_MSG(numTanks >= 2, "expected 2 snapshotted players, got %d",
                  (int)numTanks);

    /* Now the removal happens "behind" the snapshot: serverSimRemovePlayer's
     * lgm teardown, verbatim. lgmPtrs[1] still holds &lgmen[1] — a valid
     * address whose CONTENTS are now NULL — and numTanks still counts slot 1.
     * The tank is deliberately left alone so the loop below runs to the lgm
     * deref rather than dying earlier on a freed tank. */
    lgmDestroy(&gs->lgmen[1]);
    gs->lgmen[1] = NULL;

    /* A shell at end of life: length <= SHELL_DEATH takes the expiry branch on
     * the very next update, which is the branch that faulted in the report. */
    WORLD sx = 0, sy = 0;
    UT_ASSERT(serverSimGetTankState(sim, 0, &sx, &sy));
    shellsAddItem(gs, &gs->shs, sx, sy, 0, 8, /*owner*/ 0, NEUTRAL, FALSE);
    UT_ASSERT_MSG(gs->shs != NULL, "shell was not added");
    gs->shs->length = SHELL_DEATH;

    /* Pre-fix: SIGSEGV here at shells.c `(*lgms[count])->playerNum`. */
    shellsUpdate(gs, tanksArray, numTanks, lgmPtrs, &gs->ss);

    /* Not a vacuous pass: the shell must actually have reached its death
     * branch (server-side that marks it dead, then reaps it next update),
     * so the lgm loop we care about really ran. */
    UT_ASSERT_MSG(gs->shs == NULL || gs->shs->shellDead == TRUE,
                  "shell never reached its death branch — the lgm loop under "
                  "test did not run");

    /* The surviving player's lgm is untouched. */
    UT_ASSERT(gs->lgmen[0] != NULL);

    serverSimDestroy(sim);
    return 0;
}

/* ── (b) the ping kick defers its teardown ──────────────────────────── */

#define JOIN_MAX     2000  /* join + map download on the clean path */
#define DRAIN_PUMPS  4     /* ticks for the deferred drain to remove the slot */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

int run_ping_kick_defers_teardown(void) {
    LoopbackHarness h;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Laggy", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0xBAD91C6u),
                  "harness start failed");

    int connectedAt = loopbackHarnessPumpUntil(&h, JOIN_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", JOIN_MAX);
    }

    BYTE slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("client was never assigned a slot (got %d)", (int)slot);
    }

    GameSim *gs = serverSimGetGameSim(h.sim);
    if (gs->tanks[slot] == NULL || gs->lgmen[slot] == NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("joined slot %d has no tank/lgm to begin with", (int)slot);
    }
    int numBefore = (int)serverSimGetNumPlayers(h.sim);

    /* Strike the slot out. Each enforce pass only counts a ping it has not
     * already seen (lastEnforcedPingMs), so every strike needs a distinct
     * value — all over PING_KICK_THRESHOLD_MS. Enforcement broadcasts, so hold
     * the tick mutex exactly as serverInstanceTick does around the sim frame
     * this normally runs inside. */
    threadsWaitForMutex();
    for (i = 0; i < PING_KICK_COUNT; i++) {
        transportUdpServerSetClientPingForTest(
            slot, (uint16_t)(PING_KICK_THRESHOLD_MS + 50 + i));
        transportUdpServerEnforcePing(h.sim);
    }
    threadsReleaseMutex();

    /* THE ASSERTION. enforce runs mid-sim-frame with the half-step's tank and
     * lgm pointers already snapshotted, so it must not free anything here.
     * Pre-fix all three of these flip on the final strike. */
    UT_ASSERT_MSG(gs->tanks[slot] != NULL,
                  "ping kick freed the tank synchronously — the half-step's "
                  "tanksArray now dangles");
    UT_ASSERT_MSG(gs->lgmen[slot] != NULL,
                  "ping kick freed the lgm synchronously — lgmPtrs[] now points "
                  "at a NULLed slot (this is the reported 0x18 crash)");
    UT_ASSERT_MSG((int)serverSimGetNumPlayers(h.sim) == numBefore,
                  "player count dropped inside the enforce call (%d -> %d); "
                  "teardown must be deferred to the drain",
                  numBefore, (int)serverSimGetNumPlayers(h.sim));

    /* ...but the kick must still actually happen. The next tick's
     * transportUdpServerDrainPendingRemovals does the teardown. */
    for (i = 0; i < DRAIN_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }

    UT_ASSERT_MSG((int)serverSimGetNumPlayers(h.sim) < numBefore,
                  "high-ping client was never removed by the deferred drain "
                  "(numPlayers stayed %d)", (int)serverSimGetNumPlayers(h.sim));
    UT_ASSERT_MSG(gs->tanks[slot] == NULL && gs->lgmen[slot] == NULL,
                  "drain left slot %d's tank/lgm allocated", (int)slot);

    loopbackHarnessStop(&h);
    return 0;
}

/* ── (c) the freed slot carries no strikes to its next occupant ──────── */

/*
 * The strike counters live on the slot, not the connection, and a kick leaves
 * pingKickStrikes sitting at PING_KICK_COUNT. serverDisconnectClient resets the
 * rest of the slot's per-occupant state (name, claim, WBN status, channel mux);
 * the ping tally used to be left behind, so the next player to take the slot
 * inherited a full strike count and the first measurement over the threshold
 * kicked them on the spot — the "rejoin and get booted straight away" report.
 *
 * Drives the real kick to completion, then reads the freed slot back. This is
 * the invariant the reconnect depends on: a slot handed on must start at zero.
 */
int run_ping_kick_clears_strikes_on_disconnect(void) {
    LoopbackHarness h;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Laggy", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0xBAD91C7u),
                  "harness start failed");

    int connectedAt = loopbackHarnessPumpUntil(&h, JOIN_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", JOIN_MAX);
    }

    BYTE slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("client was never assigned a slot (got %d)", (int)slot);
    }

    /* Same striking loop as (b): a distinct over-threshold value each pass so
     * the lastEnforcedPingMs dedup counts every one. */
    threadsWaitForMutex();
    for (i = 0; i < PING_KICK_COUNT; i++) {
        transportUdpServerSetClientPingForTest(
            slot, (uint16_t)(PING_KICK_THRESHOLD_MS + 50 + i));
        transportUdpServerEnforcePing(h.sim);
    }
    threadsReleaseMutex();

    /* Not vacuous: the strikes must actually have accumulated before the drain
     * clears them, or this test would pass against a kick that never armed. */
    {
        uint8_t kickStrikes = 0;
        transportUdpServerGetPingStrikesForTest(slot, &kickStrikes, NULL, NULL,
                                                NULL);
        UT_ASSERT_MSG(kickStrikes >= PING_KICK_COUNT,
                      "expected the slot to be struck out before the drain "
                      "(kickStrikes=%d, need >= %d)",
                      (int)kickStrikes, (int)PING_KICK_COUNT);
    }

    for (i = 0; i < DRAIN_PUMPS; i++) {
        loopbackHarnessPump(&h);
    }
    UT_ASSERT_MSG(transportUdpServerGetClientCount() == 0,
                  "high-ping client was never disconnected by the deferred "
                  "drain — nothing to assert about the freed slot");

    /* THE ASSERTION. Pre-fix all four of these survive the teardown, and the
     * next occupant of this slot starts one bad sample from a kick. */
    {
        uint8_t  kickStrikes = 0xFF;
        uint8_t  warnStrikes = 0xFF;
        bool     warned = true;
        uint16_t lastEnforced = 0xFFFF;
        transportUdpServerGetPingStrikesForTest(slot, &kickStrikes,
                                                &warnStrikes, &warned,
                                                &lastEnforced);
        UT_ASSERT_MSG(kickStrikes == 0,
                      "freed slot %d kept %d kick strikes — the next occupant "
                      "is kicked on its first high ping",
                      (int)slot, (int)kickStrikes);
        UT_ASSERT_MSG(warnStrikes == 0,
                      "freed slot %d kept %d warn strikes",
                      (int)slot, (int)warnStrikes);
        UT_ASSERT_MSG(!warned,
                      "freed slot %d stayed flagged as already-warned — the "
                      "next occupant is kicked with no warning first",
                      (int)slot);
        UT_ASSERT_MSG(lastEnforced == 0,
                      "freed slot %d kept lastEnforcedPingMs=%u — the next "
                      "occupant's first matching sample is skipped",
                      (int)slot, (unsigned)lastEnforced);
    }

    loopbackHarnessStop(&h);
    return 0;
}
