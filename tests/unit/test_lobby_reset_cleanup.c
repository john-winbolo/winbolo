/*
 * Lobby/leave cleanup regressions (test_lobby_reset_cleanup.c).
 *
 * Two server-side fixes for phantom players and bot-only games that
 * never returned to the lobby:
 *
 * 1. serverSimRemovePlayer must clear the departing slot's sim.plyrs
 *    entry. It used to clear only playerConnected, leaving the slot
 *    inUse with its old name; the join sync-replay re-announces every
 *    inUse slot via CTRL_PLAYER_JOIN (gated on inUse, not
 *    playerConnected), so every new client was handed the departed
 *    player/bot as a frozen phantom that outlived a full client
 *    restart (the phantom lived on the server).
 *
 * 2. The last human leaving a running game must return the server to
 *    the lobby. The empty/auto-close checks count bots, so a bot-only
 *    game ran forever. The trigger is gated on roundHadHuman so a game
 *    that legitimately starts with only bots doesn't loop
 *    start<->gameover.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "players.h"               /* playersIsInUse */
#include "game_sim.h"
#include "everard_map.h"           /* E_MAP */
#include "server_sim.h"            /* serverSimRemovePlayer, GetState, ServerState */
#include "server_sim_internal.h"   /* serverSimGetGameSim, roundHadHuman */
#include "server_sim_lifecycle.h"  /* serverSimStartGame / SetLobbyEnabled */
#include "test_harness.h"

/* Lobby-enabled ServerSim from the embedded Everard map. Caller owns it. */
static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* ---- 1. serverSimRemovePlayer clears the slot back to empty. ---- */
int run_remove_player_clears_slot(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Elvis3", false);
    UT_ASSERT_MSG(playersIsInUse(&serverSimGetGameSim(sim)->plyrs, 0) == TRUE,
                  "slot must be inUse after add");

    serverSimRemovePlayer(sim, 0);

    /* The key assertion: the slot is no longer inUse. The join
     * sync-replay's CTRL_PLAYER_JOIN loop gates on inUse, so a cleared
     * slot is no longer re-announced to new clients — no phantom. */
    UT_ASSERT_MSG(playersIsInUse(&serverSimGetGameSim(sim)->plyrs, 0) == FALSE,
                  "slot must be cleared (not inUse) after removePlayer");
    {
        char nm[PLAYER_NAME_LEN];
        playersGetPlayerName(&serverSimGetGameSim(sim)->plyrs, 0, nm, TRUE);
        UT_ASSERT_MSG(nm[0] == '\0',
                      "departed slot name must be blanked (got '%s')", nm);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ---- 2. Last human leaving a running game returns to the lobby. ---- */
int run_last_human_leave_returns_to_lobby(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Elvis3", false);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "game should be running after start");

    /* One tick with the human present arms roundHadHuman; the game must
     * not end while a human is in it. */
    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "game should stay running while a human is present");

    serverSimRemovePlayer(sim, 0);

    /* Next tick observes zero humans with roundHadHuman set -> game over. */
    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "last human leaving must end the round (got state=%d)",
                  (int)serverSimGetState(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ---- 3. A round that never had a human does not auto-end (loop guard). ---- */
int run_humanless_round_does_not_autoend(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* No human ever joins this round. roundHadHuman stays false, so the
     * last-human-left trigger must not fire — otherwise a bot-only game
     * (all bots ready) would loop start<->gameover. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "game should be running after start");

    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "a round with no humans must keep running, not auto-end "
                  "(got state=%d)", (int)serverSimGetState(sim));

    serverSimDestroy(sim);
    return 0;
}
