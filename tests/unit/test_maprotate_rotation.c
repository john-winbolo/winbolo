/*
 * No-lobby map-rotation round restart (test_maprotate_rotation.c).
 *
 * -maprotate runs a no-lobby server that, at each round boundary (a win or
 * the server emptying), boots everyone, picks the next map and restarts a
 * fresh running round instead of quitting. serverSimMapRotateRound is the
 * sim-core half of that restart; the surrounding disconnect-all and WBN
 * session rotation live in the server lifecycle.
 *
 * Two contracts are pinned here, both at the server_sim layer where the
 * unit binary can reach them without a live transport / WBN HTTP stack:
 *
 *  1. The restart re-arms the empty-server check. serverSimStartGame latches
 *     hadPlayersEver = TRUE, but the fresh round starts with zero players —
 *     left set, the lifecycle's empty check would fire on the very next tick
 *     and rotate again forever. The rotation must clear it.
 *
 *  2. The restart opens the WBN session-rotation window before touching the
 *     map. The new round's map is picked while the just-finished round's
 *     server_key is still live, so serverSimWbnLobbyUpdate must hold any such
 *     change dirty until winbolonetBeginSession installs the new key (which
 *     the lifecycle signals by clearing wbnSessionRotating). This mirrors the
 *     guard test_wbn_session_rotation.c pins for serverSimReturnToLobby — the
 *     no-lobby rotation skips that function, so it needs its own coverage.
 *
 * Uses the WBN stub's lobby_update call counter (test_stubs.c) as the stand-in
 * for "a lobby_update went out", same as the session-rotation test.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* ServerSim fields: wbnSessionRotating, wbnLobbyDirty, hadPlayersEver */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled / WbnLobbyUpdate / ApplyInstanceConfig */
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* Test-controllable WBN stub surface from test_stubs.c. */
extern bool wbnStubRunning;
extern int  wbnStubLobbyUpdateCalls;

/* No-lobby, map-rotation-mode ServerSim from the embedded Everard map. */
static ServerSim *make_rotate_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimSetMapRotate(sim, true);
    return sim;
}

/* A rotation restarts a fresh running round and re-arms the empty-server
 * check, so an empty fresh round can't trigger an immediate second rotation. */
int run_maprotate_restarts_round_and_rearms(void) {
    ServerSim *sim = make_rotate_sim();
    UT_ASSERT(sim != NULL);

    wbnStubRunning = TRUE;
    wbnStubLobbyUpdateCalls = 0;

    /* Pretend the round that just ended had players, so the re-arm to FALSE
     * is observable (a fresh sim is already FALSE). */
    sim->hadPlayersEver = TRUE;

    serverSimMapRotateRound(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "map rotation must start a fresh running round");
    UT_ASSERT_MSG(sim->hadPlayersEver == FALSE,
                  "map rotation must re-arm the empty-server check so the "
                  "fresh empty round does not rotate again immediately");

    wbnStubRunning = FALSE;
    serverSimDestroy(sim);
    return 0;
}

/* The next round's map must never be reported to WBN on the just-finished
 * round's server_key. The rotation opens the window; the held update flushes
 * only once the lifecycle closes it after installing the new key. */
int run_maprotate_defers_wbn_update_until_key_rotated(void) {
    ServerSim *sim = make_rotate_sim();
    UT_ASSERT(sim != NULL);

    wbnStubRunning = TRUE;
    wbnStubLobbyUpdateCalls = 0;

    serverSimMapRotateRound(sim);
    UT_ASSERT_MSG(sim->wbnSessionRotating == TRUE,
                  "map rotation must open the WBN session-rotation window");

    /* A map-change-driven update inside the window must be deferred even when
     * forced — the guard outranks force, so nothing reaches the old key. */
    serverSimWbnLobbyUpdate(sim, TRUE);
    UT_ASSERT_MSG(wbnStubLobbyUpdateCalls == 0,
                  "no lobby_update may be sent on the old key while rotating "
                  "(got %d)", wbnStubLobbyUpdateCalls);
    UT_ASSERT_MSG(sim->wbnLobbyDirty == TRUE,
                  "the suppressed update must be held dirty for later flush");

    /* Lifecycle closes the window after winbolonetBeginSession installs the
     * new key; the held update now flushes against the new session. */
    sim->wbnSessionRotating = FALSE;
    serverSimWbnLobbyUpdate(sim, TRUE);
    UT_ASSERT_MSG(wbnStubLobbyUpdateCalls == 1,
                  "deferred lobby_update must flush once the window closes "
                  "(got %d)", wbnStubLobbyUpdateCalls);

    wbnStubRunning = FALSE;
    serverSimDestroy(sim);
    return 0;
}

/* Plain no-lobby ServerSim (e.g. -nolobby / -quitonwin): game-over is terminal. */
static ServerSim *make_plain_nolobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    return sim;
}

/* A win drives a map-rotation round to game-over, then the lifecycle rotates
 * instead of quitting. The dedicated server's exit poll must treat that
 * game-over as non-terminal — otherwise it races the rotation (which flips
 * state back to running within the tick) and shuts the process down, the
 * "-maprotate exits like -quitonwin" bug. A plain no-lobby server must still
 * report game-over as terminal so its existing shutdown-on-win is preserved. */
int run_maprotate_gameover_is_not_terminal(void) {
    ServerSim *sim = make_rotate_sim();
    UT_ASSERT(sim != NULL);

    serverSimStartGame(sim);
    serverSimEnterGameOver(sim);   /* what a win does in the running tick */
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "enterGameOver must leave the sim in game-over");
    UT_ASSERT_MSG(serverSimIsTerminalGameOver(sim) == FALSE,
                  "a map-rotation game-over must not terminate the server — it "
                  "rotates to the next round instead");
    serverSimDestroy(sim);

    ServerSim *plain = make_plain_nolobby_sim();
    UT_ASSERT(plain != NULL);
    serverSimStartGame(plain);
    serverSimEnterGameOver(plain);
    UT_ASSERT_MSG(serverSimGetState(plain) == serverStateGameOver,
                  "enterGameOver must leave the plain sim in game-over");
    UT_ASSERT_MSG(serverSimIsTerminalGameOver(plain) == TRUE,
                  "a plain no-lobby game-over must still terminate the server");
    serverSimDestroy(plain);
    return 0;
}

/* The first map-rotation round boots up empty (no players yet). The boot start
 * must re-arm the empty-server check, or serverSimCheckAutoClose fires on the
 * very next tick and rotates before anyone can join — the "rotates the map
 * immediately at startup" bug. Drives the real no-lobby boot path
 * (serverSimApplyInstanceConfig with skipLobby) rather than poking the flag. */
int run_maprotate_boot_does_not_rotate_while_empty(void) {
    ServerSim *sim = make_rotate_sim();
    UT_ASSERT(sim != NULL);

    ServerInstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.skipLobby = true;
    cfg.botAiType = (BYTE)aiNone;
    serverSimApplyInstanceConfig(sim, &cfg);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "no-lobby boot must start a running round");
    UT_ASSERT_MSG(sim->hadPlayersEver == FALSE,
                  "boot must re-arm the empty-server check so the empty first "
                  "round does not rotate before anyone joins");
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "an empty just-booted round must not trigger a rotation");

    serverSimDestroy(sim);
    return 0;
}
