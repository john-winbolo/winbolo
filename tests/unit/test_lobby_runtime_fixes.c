/*
 * Regressions for the lobby runtime fixes landed on the lobby-fixes
 * branch and follow-ups (commits 4cc7a1d…3f5aa14):
 *
 *   - serverSimAbortCountdown must publish CTRL_GAME_PHASE_LOBBY so
 *     remote clients drop the "Game starting in N…" overlay. The
 *     unready-during-countdown branch and serverSimRemovePlayer
 *     both reach this via the helper.
 *
 *   - lobbyAutoUnreadyOnChange must clear humans' ready flags while
 *     leaving bots ready, so a join/leave that fires the helper
 *     during an active countdown ends up back in lobby with the bot
 *     roster intact.
 *
 *   - serverSimGetMapDirRoot falls back to "data/maps" when -mapdir
 *     was not configured, and returns the captured path otherwise.
 *     Every server-side map I/O site reads through this accessor.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->state, lobbyPlayers, mapDirPath */
#include "server_sim_lifecycle.h"  /* serverSimGetMapDirRoot, lobbyAutoUnreadyOnChange */
#include "control_event.h"
#include "everard_map.h"
#include "test_harness.h"

/* ================================================================
 * serverSimAbortCountdown publishes CTRL_GAME_PHASE_LOBBY.
 *
 * Before the fix that moved the publish into the helper, the abort
 * mutated sim->state inline and clients stayed at netLobbyCountdown
 * with the overlay drawn. Subscribe a counting deliver callback,
 * push the sim into countdown by hand, then call the helper and
 * assert that one CTRL_GAME_PHASE_LOBBY event reached the
 * subscriber.
 * ================================================================ */

typedef struct {
    int phaseLobbyCount;
    int phaseCountdownCount;
    int totalEvents;
} PhaseCounter;

static void count_phase_events(void *ctx, const ControlEvent *evt) {
    PhaseCounter *c = (PhaseCounter *)ctx;
    c->totalEvents++;
    if (evt->type == CTRL_GAME_PHASE_LOBBY) c->phaseLobbyCount++;
    else if (evt->type == CTRL_GAME_PHASE_COUNTDOWN) c->phaseCountdownCount++;
}

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

int run_countdown_abort_publishes_phase(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    PhaseCounter counter;
    memset(&counter, 0, sizeof(counter));

    SubscriberHandle h =
        serverSimRegisterSubscriber(sim, count_phase_events, &counter);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Drain the sync replay — it includes a CTRL_GAME_PHASE_LOBBY for
     * the current state plus settings / brain list / etc. The test
     * only cares about events fired AFTER abortCountdown, so snapshot
     * the post-sync count and compare deltas. */
    int afterSyncLobbyCount = counter.phaseLobbyCount;

    /* Force countdown state directly — the only place that normally
     * sets it (serverSimLobbyCheckAllReady) needs a connected ready
     * player; this test isn't about that path. */
    sim->state = serverStateCountdown;
    sim->countdownTicks = 250;

    serverSimAbortCountdown(sim);

    UT_ASSERT_MSG(sim->state == serverStateLobby,
                  "abortCountdown must leave state == Lobby");
    UT_ASSERT_MSG(sim->countdownTicks == 0,
                  "abortCountdown must zero countdownTicks");
    UT_ASSERT_MSG(counter.phaseLobbyCount == afterSyncLobbyCount + 1,
                  "abortCountdown must publish exactly one "
                  "CTRL_GAME_PHASE_LOBBY (got delta=%d)",
                  counter.phaseLobbyCount - afterSyncLobbyCount);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * lobbyAutoUnreadyOnChange clears humans, keeps bots ready.
 *
 * The bot pool is process-global and the brain runtime is heavy, so
 * the test sidesteps botManagerAddBot and seeds the lobbyPlayers
 * struct directly — same surface the helper iterates over.
 * ================================================================ */
int run_lobby_auto_unready_clears_humans_keeps_bots(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* Slot 0 + 1: humans, ready. */
    serverSimAddPlayer(sim, 0, "Alice", false);
    serverSimAddPlayer(sim, 1, "Bob", false);
    serverSimSetReady(sim, 0, true);
    serverSimSetReady(sim, 1, true);

    /* Slot 2: bot, ready. Directly seeded so the test doesn't depend
     * on the real bot manager (loads a Lua brain, allocates a
     * ClientSim, registers a subscriber — none of which this test
     * needs). The helper reads playerConnected + lobbyPlayers fields
     * and that's all the state it cares about. */
    sim->playerConnected[2] = TRUE;
    sim->lobbyPlayers[2].isBot      = true;
    sim->lobbyPlayers[2].ready      = true;
    sim->lobbyPlayers[2].teamNumber = 3;

    UT_ASSERT(serverSimGetLobbyPlayer(sim, 0)->ready);
    UT_ASSERT(serverSimGetLobbyPlayer(sim, 1)->ready);
    UT_ASSERT(serverSimGetLobbyPlayer(sim, 2)->ready);

    lobbyAutoUnreadyOnChange(sim);

    UT_ASSERT_MSG(!serverSimGetLobbyPlayer(sim, 0)->ready,
                  "human slot 0 must be unreadied");
    UT_ASSERT_MSG(!serverSimGetLobbyPlayer(sim, 1)->ready,
                  "human slot 1 must be unreadied");
    UT_ASSERT_MSG(serverSimGetLobbyPlayer(sim, 2)->ready,
                  "bot slot 2 must stay ready — the helper skips bots "
                  "so the auto-unready doesn't strand the lobby with "
                  "a never-ready bot");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * serverSimGetMapDirRoot — fallback and capture.
 *
 * The accessor is the single substitution point every server-side
 * map I/O site reads through. With mapDirPath unset (no -mapdir),
 * it falls back to the legacy "data/maps". With mapDirPath set, it
 * returns the captured path verbatim. Empty string also falls back
 * — defensively guards against a botched build that strdup'd "".
 * ================================================================ */
int run_mapdir_root_fallback(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(sim->mapDirPath == NULL,
                  "fresh sim must start with mapDirPath = NULL");
    UT_ASSERT_MSG(strcmp(serverSimGetMapDirRoot(sim), "data/maps") == 0,
                  "no -mapdir → fallback to \"data/maps\" (got '%s')",
                  serverSimGetMapDirRoot(sim));

    sim->mapDirPath = SDL_strdup("/srv/winbolo/maps");
    UT_ASSERT(sim->mapDirPath != NULL);
    UT_ASSERT_MSG(strcmp(serverSimGetMapDirRoot(sim),
                         "/srv/winbolo/maps") == 0,
                  "configured path must be returned verbatim (got '%s')",
                  serverSimGetMapDirRoot(sim));

    SDL_free(sim->mapDirPath);
    sim->mapDirPath = SDL_strdup("");
    UT_ASSERT(sim->mapDirPath != NULL);
    UT_ASSERT_MSG(strcmp(serverSimGetMapDirRoot(sim), "data/maps") == 0,
                  "empty-string mapDirPath must fall back to "
                  "\"data/maps\" (defensive — accessor should never "
                  "return \"\" as a base for concatenation)");

    /* NULL accessor input must also fall back without crashing. */
    UT_ASSERT_MSG(strcmp(serverSimGetMapDirRoot(NULL), "data/maps") == 0,
                  "NULL sim must fall back to \"data/maps\"");

    SDL_free(sim->mapDirPath);
    sim->mapDirPath = NULL;
    serverSimDestroy(sim);
    return 0;
}
