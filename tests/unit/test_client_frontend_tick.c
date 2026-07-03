/*
 * Shared client-frontend tick core (src/client_frontend/client_frontend_tick.c)
 * and the clientSimTransportTicksServer accessor it keys on.
 *
 * The tick core was extracted so every platform runs the same cadence
 * instead of a copy that can drift; these tests pin the two behaviours a
 * drift actually broke:
 *
 *   tick_core_active_local_no_double_step — on the ACTIVE local transport
 *       (wasm SP: tick() runs serverSimTick) the keys half must NOT pump
 *       the transport, or the server steps twice per frame. Asserts the
 *       server tick advances on the game half only.
 *   tick_core_passive_local_pumps_keys_half — on the PASSIVE local
 *       transport (desktop SP: host timer ticks the server; tick() only
 *       pulls a snapshot) the keys half MUST pump, preserving the ~10ms
 *       oversampling of the ~20ms server tick that keeps one-shot snapshot
 *       events from being missed. The ClientSim carries the single-player
 *       flag here exactly like desktop SP does — regression net for the
 *       predicate once being clientSimIsSinglePlayer, which wrongly
 *       dropped desktop SP's keys-half pump.
 *   tick_core_lobby_flips_cadence — lobby steps run no game/keys tick but
 *       keep the half-step flag alternating, so the first running step
 *       after an odd number of lobby steps is the keys half.
 *   transport_ticks_server_lifecycle — the accessor: true only for the
 *       active local connect, false for passive/UDP/none, preserved across
 *       clientSimCreate's field save/restore, cleared by disconnect.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_sim_internal.h"  /* cs->lastServerTick — snapshot-apply observable */
#include "threads.h"
#include "input.h"                /* keyItems, tankButton */
#include "client_frontend_tick.h"
#include "test_harness.h"

/* ---- gui-global stubs the tick core links against ------------------- */
/* isInMenu and the clientMutex no-ops live in test_stubs.c; the rest of
 * the frontend surface the core touches is provided here. */

keyItems keys;

tankButton inputGetKeys(struct ClientSim *cs, keyItems *setKeys, bool isMenu) {
    (void)cs; (void)setKeys; (void)isMenu;
    return 0;
}
void inputScroll(struct ClientSim *cs, keyItems *setKeys, bool isMenu) {
    (void)cs; (void)setKeys; (void)isMenu;
}
bool inputIsFireKeyPressed(keyItems *setKeys, bool isMenu) {
    (void)setKeys; (void)isMenu;
    return false;
}
bool inputIsMineKeyPressed(keyItems *setKeys, bool isMenu) {
    (void)setKeys; (void)isMenu;
    return false;
}
uint8_t inputConsumeGunsightAdj(void) { return 0; }
BYTE gameFrontGetPlayerNum(void)      { return 0; }
bool brainHandlerIsBrainRunning(void) { return false; }

/* ---------------------------------------------------------------------- */

int run_tick_core_active_local_no_double_step(void) {
    ServerSim *sim = ut_make_running_sim("Cadence");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimConnectLocal(cs, sim, "Cadence", "", 0, 0));
    UT_ASSERT(clientSimTransportTicksServer(cs));

    clientFrontTickReset();

    /* First step after reset is the game half; it may advance the server
     * (localTick runs serverSimTick on the active transport). */
    uint32_t before = serverSimGetTick(sim);
    bool wasGame = clientFrontRunTickStep(cs);
    UT_ASSERT_MSG(wasGame, "first step after reset must be the game half");
    uint32_t afterGame = serverSimGetTick(sim);
    UT_ASSERT_MSG(afterGame > before,
                  "game half did not advance the active-local server");
    uint32_t gameDelta = afterGame - before;

    /* The keys half must not pump — the server tick must hold still. */
    bool wasGame2 = clientFrontRunTickStep(cs);
    UT_ASSERT_MSG(!wasGame2, "second step must be the keys half");
    uint32_t afterKeys = serverSimGetTick(sim);
    UT_ASSERT_MSG(afterKeys == afterGame,
                  "keys half advanced the server by %u — double-step",
                  (unsigned)(afterKeys - afterGame));

    /* Over N full cycles the server advances exactly N game-half deltas. */
    int cycle;
    uint32_t start = serverSimGetTick(sim);
    for (cycle = 0; cycle < 20; cycle++) {
        UT_ASSERT(clientFrontRunTickStep(cs));   /* game */
        UT_ASSERT(!clientFrontRunTickStep(cs));  /* keys */
    }
    UT_ASSERT_MSG(serverSimGetTick(sim) - start == 20u * gameDelta,
                  "cadence drifted over 20 cycles: %u vs %u",
                  (unsigned)(serverSimGetTick(sim) - start),
                  (unsigned)(20u * gameDelta));

    clientSimDisconnect(cs);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

int run_tick_core_passive_local_pumps_keys_half(void) {
    UT_ASSERT_MSG(threadsCreate(TRUE), "threadsCreate failed");

    ServerSim *sim = ut_make_running_sim("Desktop");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimConnectLocalPassive(cs, sim, "Desktop", "", 0, 0));
    UT_ASSERT(!clientSimTransportTicksServer(cs));

    /* Desktop non-tutorial SP sets this flag; the keys-half pump must
     * survive it (the old predicate keyed the skip off this and starved
     * desktop SP's snapshot sampling). */
    clientSimSetIsSinglePlayer(cs, true);

    clientFrontTickReset();

    /* Game half: pumps, applying the current server tick's snapshot. */
    UT_ASSERT(clientFrontRunTickStep(cs));
    uint32_t seenAfterGame = cs->lastServerTick;

    /* Advance the server externally, as the desktop host timer thread
     * does. The client hasn't pumped since, so it can't have seen it. */
    threadsWaitForMutex();
    serverSimTick(sim);
    threadsReleaseMutex();
    uint32_t serverNow = serverSimGetTick(sim);
    UT_ASSERT(serverNow > seenAfterGame);

    /* Keys half: MUST pump and apply the fresh snapshot. */
    UT_ASSERT(!clientFrontRunTickStep(cs));
    UT_ASSERT_MSG(cs->lastServerTick == serverNow,
                  "keys half did not pump the passive transport "
                  "(saw tick %u, server at %u) — desktop SP oversampling lost",
                  (unsigned)cs->lastServerTick, (unsigned)serverNow);

    clientSimDisconnect(cs);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    threadsDestroy();
    return 0;
}

int run_tick_core_lobby_flips_cadence(void) {
    ServerSim *sim = ut_make_running_sim("Lobbyist");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimConnectLocal(cs, sim, "Lobbyist", "", 0, 0));

    clientFrontTickReset();

    /* One lobby step: returns false (no game tick ran) and consumes the
     * game half, so the first running step lands on the keys half. */
    clientSimSetNetStatus(cs, netLobby);
    UT_ASSERT(!clientFrontRunTickStep(cs));

    clientSimSetNetStatus(cs, netRunning);
    UT_ASSERT_MSG(!clientFrontRunTickStep(cs),
                  "first running step after one lobby step must be keys");
    UT_ASSERT_MSG(clientFrontRunTickStep(cs),
                  "second running step must be the game half");

    clientSimDisconnect(cs);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

int run_transport_ticks_server_lifecycle(void) {
    ServerSim *sim = ut_make_running_sim("Flags");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    /* NULL-safe, false with no transport. */
    UT_ASSERT(!clientSimTransportTicksServer(NULL));
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(!clientSimTransportTicksServer(cs));

    /* Active local → true; preserved across clientSimCreate's memset
     * save/restore (the map-reload path); cleared by disconnect. */
    UT_ASSERT(clientSimConnectLocal(cs, sim, "Flags", "", 0, 0));
    UT_ASSERT(clientSimTransportTicksServer(cs));
    clientSimCreate(cs);
    UT_ASSERT_MSG(clientSimTransportTicksServer(cs),
                  "flag lost across clientSimCreate save/restore");
    clientSimDisconnect(cs);
    UT_ASSERT(!clientSimTransportTicksServer(cs));
    clientSimDestroy(cs);

    /* Passive local → false. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimConnectLocalPassive(cs, sim, "Flags", "", 0, 0));
    UT_ASSERT(!clientSimTransportTicksServer(cs));
    clientSimDisconnect(cs);
    clientSimDestroy(cs);

    serverSimDestroy(sim);
    return 0;
}
