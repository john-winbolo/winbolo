/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The empty reset counts only human players.
 *
 * A server with bots on it used to never be empty: the check counted every
 * connected seat, so a -bots server kept its round forever once the people
 * had gone. And a server with no lobby never ran the check at all. These
 * cases pin what "empty" means now and what a server with no lobby does
 * when its timer runs out.
 *
 *   empty_reset_bots_only_fires — a running round with a server bot and a
 *       held bot seat (what a Bot Host seat is in the sim) and no person.
 *       The timer starts on the first check and fires after exactly
 *       emptyResetMinutes of frames.
 *   empty_reset_human_holds_timer — the same round with a person in it.
 *       The timer never starts.
 *   empty_reset_human_leaves_starts_timer — the person leaves; the next
 *       check starts the timer, with the bots still on the field.
 *   empty_reset_spectator_counts_empty — a running server watched by a real
 *       spectator over the wire and played by nobody. A spectator holds no
 *       player seat, so the server is empty and the timer starts.
 *   empty_reset_nolobby_starts_new_round — a server with no lobby, a bot
 *       and a spectator. When the timer runs out the server starts the next
 *       round in place: still running, a fresh world (the tick back near 0
 *       and a changed tile back as the map has it), the bot on the field
 *       with a tank, the spectator still connected, and the timer counting
 *       again from the full length.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"            /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* emptyResetTicks, lobbyPlayers, botMgr */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType */
#include "server_sim_scenario.h"   /* serverSimAddUnfieldedSeat */
#include "bolo_map.h"              /* mapGetPos / mapSetPos */
#include "everard_map.h"           /* E_MAP */
#include "transport_udp.h"         /* transportUdpServerGetSpectatorCount */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Frames in one minute of the empty timer: it counts once a frame, 50 a
   second (serverSimCheckEmptyReset). */
#define ER_FRAMES_PER_MIN (60 * 50)
#define ER_SPEC_CONNECT_MAX 600

#define ER_HUMAN_SLOT 0
#define ER_BOT_SLOT   1
#define ER_HELD_SLOT  2

/* ── Sim-only cases ───────────────────────────────────────────────── */

/* A running lobby round with two bot seats and no person: a server bot (an
   active bot manager entry) and a held seat, which is what the sim holds
   for a bot that is not on the field yet and what a Bot Host binds to. */
static ServerSim *erMakeBotRound(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island", gameOpen,
                                               false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetQuitOnWin(sim, false);
    serverSimSetEmptyResetEnabled(sim, true);
    serverSimSetEmptyResetMinutes(sim, 1);
    serverSimStartGame(sim);

    serverSimAddPlayer(sim, ER_BOT_SLOT, "Botty", false);
    sim->botMgr.bots[ER_BOT_SLOT].active = true;
    if (!serverSimAddUnfieldedSeat(sim, ER_HELD_SLOT, "Held", 2)) {
        serverSimDestroy(sim);
        return NULL;
    }
    return sim;
}

int run_empty_reset_bots_only_fires(void) {
    ServerSim *sim = erMakeBotRound();
    int        i;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 2,
                  "setup: expected 2 connected bot seats, got %u",
                  (unsigned)serverSimGetNumPlayers(sim));
    UT_ASSERT_MSG(serverSimIsBot(sim, ER_BOT_SLOT) &&
                  serverSimIsBot(sim, ER_HELD_SLOT),
                  "setup: both seats must read as bots");

    /* The first check starts the countdown and takes its first frame. */
    UT_ASSERT_MSG(!serverSimCheckEmptyReset(sim),
                  "the first check must not fire");
    UT_ASSERT_MSG(sim->emptyResetTicks == ER_FRAMES_PER_MIN - 1,
                  "bots only: the timer must start, emptyResetTicks=%d",
                  (int)sim->emptyResetTicks);

    /* It fires on the frame that ends the minute, not before. */
    for (i = 2; i < ER_FRAMES_PER_MIN; i++) {
        UT_ASSERT_MSG(!serverSimCheckEmptyReset(sim),
                      "the timer fired early, on frame %d of %d", i,
                      ER_FRAMES_PER_MIN);
    }
    UT_ASSERT_MSG(serverSimCheckEmptyReset(sim),
                  "the timer did not fire after %d frames", ER_FRAMES_PER_MIN);

    serverSimDestroy(sim);
    return 0;
}

int run_empty_reset_human_holds_timer(void) {
    ServerSim *sim = erMakeBotRound();
    int        i;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, ER_HUMAN_SLOT, "Person", false);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 1,
                  "setup: expected 1 human, got %u",
                  (unsigned)serverSimGetNumHumans(sim));

    for (i = 0; i < ER_FRAMES_PER_MIN + 10; i++) {
        UT_ASSERT_MSG(!serverSimCheckEmptyReset(sim),
                      "a person is on the server, yet the reset fired on "
                      "frame %d", i);
    }
    UT_ASSERT_MSG(sim->emptyResetTicks == -1,
                  "a person is on the server, yet the timer runs: "
                  "emptyResetTicks=%d", (int)sim->emptyResetTicks);

    serverSimDestroy(sim);
    return 0;
}

int run_empty_reset_human_leaves_starts_timer(void) {
    ServerSim *sim = erMakeBotRound();

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, ER_HUMAN_SLOT, "Person", false);
    UT_ASSERT(!serverSimCheckEmptyReset(sim));
    UT_ASSERT_MSG(sim->emptyResetTicks == -1,
                  "setup: the timer runs with a person on the server");

    serverSimRemovePlayer(sim, ER_HUMAN_SLOT);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 2,
                  "setup: the bots must stay, %u seats connected",
                  (unsigned)serverSimGetNumPlayers(sim));

    UT_ASSERT(!serverSimCheckEmptyReset(sim));
    UT_ASSERT_MSG(sim->emptyResetTicks == ER_FRAMES_PER_MIN - 1,
                  "the last person left, yet the timer did not start: "
                  "emptyResetTicks=%d", (int)sim->emptyResetTicks);

    /* And a person coming back stops it again. */
    serverSimAddPlayer(sim, ER_HUMAN_SLOT, "Person", false);
    UT_ASSERT(!serverSimCheckEmptyReset(sim));
    UT_ASSERT_MSG(sim->emptyResetTicks == -1,
                  "a person joined, yet the timer still runs: "
                  "emptyResetTicks=%d", (int)sim->emptyResetTicks);

    serverSimDestroy(sim);
    return 0;
}

/* ── Wire cases ───────────────────────────────────────────────────── */

static bool erPredSpectating(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

/* The harness starts a running no-lobby server with the empty reset off
   (its config leaves the field zero). Watched by one spectator. */
static bool erStartWatched(LoopbackHarness *h, uint64_t seed) {
    if (!loopbackHarnessStartSpectator(h, "Watcher", seed)) return false;
    return loopbackHarnessPumpUntil(h, ER_SPEC_CONNECT_MAX, erPredSpectating,
                                    NULL) >= 0;
}

int run_empty_reset_spectator_counts_empty(void) {
    LoopbackHarness h;
    int32_t         ticks;
    unsigned        players, humans;
    int             specs;

    if (!erStartWatched(&h, 21u)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the spectator never reached SPECTATING");
    }

    threadsWaitForMutex();
    serverSimSetEmptyResetMinutes(h.sim, 1);
    serverSimSetEmptyResetEnabled(h.sim, true);
    threadsReleaseMutex();

    loopbackHarnessPump(&h);

    threadsWaitForMutex();
    ticks   = h.sim->emptyResetTicks;
    players = serverSimGetNumPlayers(h.sim);
    humans  = serverSimGetNumHumans(h.sim);
    threadsReleaseMutex();
    specs = transportUdpServerGetSpectatorCount();

    if (specs != 1 || players != 0 || humans != 0 ||
        ticks != ER_FRAMES_PER_MIN - 1) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectators=%d players=%u humans=%u emptyResetTicks=%d; a "
                "server with only a spectator must count as empty and start "
                "the timer (want ticks %d)", specs, players, humans,
                (int)ticks, ER_FRAMES_PER_MIN - 1);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* The first plain land tile in the middle of the map (no mine, no crater),
   so the test can dig it and see the reset fill it back in. */
static bool erFindTile(ServerSim *sim, BYTE *outX, BYTE *outY, BYTE *outT) {
    int x, y;
    for (y = 100; y < 156; y++) {
        for (x = 100; x < 156; x++) {
            BYTE t = mapGetPos(&sim->sim.mp, (BYTE)x, (BYTE)y);
            if (t == GRASS || t == ROAD || t == FOREST || t == SWAMP ||
                t == RUBBLE) {
                *outX = (BYTE)x;
                *outY = (BYTE)y;
                *outT = t;
                return true;
            }
        }
    }
    return false;
}

int run_empty_reset_nolobby_starts_new_round(void) {
    LoopbackHarness h;
    BYTE            tx = 0, ty = 0, tOrig = 0, tNow;
    uint32_t        tickBefore;
    int             i, fireAt = -1;
    bool            ok;

    if (!erStartWatched(&h, 22u)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the spectator never reached SPECTATING");
    }

    /* A server bot. The unit binary's brain is the fixture stub, armed to
       load and to tick without failing so the bot stays on for the run. */
    ut_brain_stub_arm(true);
    ut_brain_stub_tick_ok(true);
    threadsWaitForMutex();
    UT_ASSERT_MSG(!h.sim->lobbyEnabled, "setup: the harness server has a lobby");
    serverSimSetBotAiType(h.sim, aiFull);
    ok = serverSimAddBot(h.sim, ER_BOT_SLOT, &(ServerSimBotConfig){
                             .brainPath   = "brains/GoalHunter/init.lua",
                             .brainName   = "Botty",
                             .ai          = aiFull,
                             .gameType    = gameOpen,
                             .hiddenMines = false,
                             .teamNumber  = 1 });
    threadsReleaseMutex();
    if (!ok) {
        loopbackHarnessStop(&h);
        ut_brain_stub_arm(false);
        UT_FAIL("the bot could not be added");
    }

    /* Some play, so the tick has moved, and a tile changed so the test can
       see the world reloaded. */
    loopbackHarnessPumpUntil(&h, 60, NULL, NULL);
    threadsWaitForMutex();
    ok = erFindTile(h.sim, &tx, &ty, &tOrig);
    if (ok) {
        mapSetPos(&h.sim->sim, &h.sim->sim.mp, tx, ty, CRATER, false, false);
    }
    tNow       = mapGetPos(&h.sim->sim.mp, tx, ty);
    tickBefore = h.sim->tick;
    serverSimSetEmptyResetMinutes(h.sim, 1);
    serverSimSetEmptyResetEnabled(h.sim, true);
    threadsReleaseMutex();
    if (!ok || tNow != CRATER) {
        loopbackHarnessStop(&h);
        ut_brain_stub_arm(false);
        UT_FAIL("setup: no tile to change (found=%d now=%u)", (int)ok,
                (unsigned)tNow);
    }

    /* One frame starts the timer, though the bot is on the field. */
    loopbackHarnessPump(&h);
    threadsWaitForMutex();
    ok = (h.sim->emptyResetTicks == ER_FRAMES_PER_MIN - 1);
    i  = (int)h.sim->emptyResetTicks;
    /* Run the rest of the minute down to its last few frames. */
    h.sim->emptyResetTicks = 3;
    threadsReleaseMutex();
    if (!ok) {
        loopbackHarnessStop(&h);
        ut_brain_stub_arm(false);
        UT_FAIL("a bot-only no-lobby server did not start the timer: "
                "emptyResetTicks=%d", i);
    }

    for (i = 1; i <= 10; i++) {
        uint32_t t;
        loopbackHarnessPump(&h);
        threadsWaitForMutex();
        t = h.sim->tick;
        threadsReleaseMutex();
        if (t < tickBefore) {
            fireAt = i;
            break;
        }
    }
    if (fireAt < 0) {
        loopbackHarnessStop(&h);
        ut_brain_stub_arm(false);
        UT_FAIL("the timer ran out and no new round started (tick %u, "
                "was %u)", (unsigned)h.sim->tick, (unsigned)tickBefore);
    }

    /* One more frame: the new round's own timer has started again. */
    loopbackHarnessPump(&h);

    threadsWaitForMutex();
    {
        ServerState st      = serverSimGetState(h.sim);
        bool        botOn   = h.sim->playerConnected[ER_BOT_SLOT] &&
                              serverSimIsBot(h.sim, ER_BOT_SLOT);
        bool        tank    = h.sim->sim.tanks[ER_BOT_SLOT] != NULL;
        BYTE        tAfter  = mapGetPos(&h.sim->sim.mp, tx, ty);
        int32_t     ticks   = h.sim->emptyResetTicks;
        threadsReleaseMutex();

        fprintf(stderr, "  empty reset fired on pump %d; state=%d bot=%d "
                "tank=%d tile=%u (map %u) ticks=%d\n", fireAt, (int)st,
                (int)botOn, (int)tank, (unsigned)tAfter, (unsigned)tOrig,
                (int)ticks);

        if (st != serverStateRunning || !botOn || !tank || tAfter != tOrig ||
            ticks < ER_FRAMES_PER_MIN - 5 ||
            transportUdpServerGetSpectatorCount() != 1 ||
            clientSimGetConnectState(h.cs) != CLIENT_CONNECT_SPECTATING) {
            loopbackHarnessStop(&h);
            ut_brain_stub_arm(false);
            UT_FAIL("after the no-lobby empty reset: state=%d (want running), "
                    "bot=%d tank=%d, tile %u (want %u), ticks=%d (want the "
                    "timer restarted), spectators=%d", (int)st, (int)botOn,
                    (int)tank, (unsigned)tAfter, (unsigned)tOrig, (int)ticks,
                    transportUdpServerGetSpectatorCount());
        }
    }

    loopbackHarnessStop(&h);
    ut_brain_stub_arm(false);
    return 0;
}
