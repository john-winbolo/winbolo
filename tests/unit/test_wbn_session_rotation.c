/*
 * WBN session-rotation guard (test_wbn_session_rotation.c).
 *
 * At a round boundary the server picks the next map, tears down the
 * finished round's WBN session (server/quit), uploads that round's log
 * against the still-set server_key, then registers a fresh session and
 * swaps in a new key. The hazard: the next round's map is chosen while
 * the OLD key is still live, so any server/lobby_update fired by the
 * map change would rename the just-finished game's map on the tracker.
 *
 * serverSimReturnToLobby opens a rotation window (wbnSessionRotating);
 * while it is open serverSimWbnLobbyUpdate must hold the change as dirty
 * instead of sending. The lifecycle closes the window only after
 * winbolonetBeginSession installs the new key, at which point the
 * deferred update flushes against the right session.
 *
 * These pin that contract at the server_sim layer, where the guard
 * lives, using the WBN stub's lobby_update call counter (see
 * test_stubs.c). The real winbolonet HTTP layer isn't linked into the
 * unit binary, so the counter stands in for "a lobby_update went out".
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* ServerSim fields: wbnSessionRotating, wbnLobbyDirty */
#include "server_sim_lifecycle.h"  /* serverSimReturnToLobby / SetLobbyEnabled / WbnLobbyUpdate */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* Test-controllable WBN stub surface from test_stubs.c. */
extern bool wbnStubRunning;
extern int  wbnStubLobbyUpdateCalls;

/* Lobby-enabled ServerSim from the embedded Everard map with one human. */
static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    return sim;
}

/* The map change during the rotation window must not emit a
 * lobby_update; it is held dirty and only flushes once the window
 * closes (mimicking the lifecycle's post-BeginSession clear). */
int run_wbn_lobby_update_deferred_during_rotation(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    wbnStubRunning = TRUE;
    wbnStubLobbyUpdateCalls = 0;

    /* Return-to-lobby opens the rotation window. */
    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(sim->wbnSessionRotating == TRUE,
                  "return-to-lobby must open the WBN rotation window");

    /* A map-change-driven update arriving inside the window must be
     * deferred even when forced — the guard outranks force. With the
     * window open, nothing may reach the (old) key. */
    serverSimWbnLobbyUpdate(sim, TRUE);
    UT_ASSERT_MSG(wbnStubLobbyUpdateCalls == 0,
                  "no lobby_update may be sent on the old key while rotating "
                  "(got %d)", wbnStubLobbyUpdateCalls);
    UT_ASSERT_MSG(sim->wbnLobbyDirty == TRUE,
                  "the suppressed update must be held dirty for later flush");

    /* Lifecycle closes the window after the new key is installed. The
     * held update now flushes against the new session. */
    sim->wbnSessionRotating = FALSE;
    serverSimWbnLobbyUpdate(sim, TRUE);
    UT_ASSERT_MSG(wbnStubLobbyUpdateCalls == 1,
                  "deferred lobby_update must flush once after rotation closes "
                  "(got %d)", wbnStubLobbyUpdateCalls);

    wbnStubRunning = FALSE;
    serverSimDestroy(sim);
    return 0;
}

/* Outside a rotation window a normal lobby edit sends immediately, so
 * the guard isn't silently swallowing ordinary lobby_updates. */
int run_wbn_lobby_update_sends_when_not_rotating(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    wbnStubRunning = TRUE;
    wbnStubLobbyUpdateCalls = 0;

    UT_ASSERT_MSG(sim->wbnSessionRotating == FALSE,
                  "a fresh sim must not be mid-rotation");

    serverSimWbnLobbyUpdate(sim, TRUE);
    UT_ASSERT_MSG(wbnStubLobbyUpdateCalls == 1,
                  "a forced lobby_update outside rotation must send (got %d)",
                  wbnStubLobbyUpdateCalls);

    wbnStubRunning = FALSE;
    serverSimDestroy(sim);
    return 0;
}
