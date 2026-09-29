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
#include "transport_udp.h"         /* transportUdpServerSetLock / GetLock */
#include "test_harness.h"

/* Lobby-enabled ServerSim from the embedded Everard map. Caller owns it. */
static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
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
    /* Raw name field, not playersGetPlayerName — the accessor returns the
     * "???" placeholder for a not-inUse slot, which would mask the clear. */
    UT_ASSERT_MSG(serverSimGetGameSim(sim)->plyrs->item[0].playerName[0] == '\0',
                  "departed slot name field must be blanked");

    serverSimDestroy(sim);
    return 0;
}

/* ---- 1b. Return-to-lobby reconciles an inUse-but-disconnected phantom. ---- */
int run_return_to_lobby_clears_phantom_slot(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 1, "Ghost", false);
    UT_ASSERT(playersIsInUse(&serverSimGetGameSim(sim)->plyrs, 1) == TRUE);

    /* Simulate a slot that lost its connection without going through the
     * leave path (e.g. the control-queue-overflow disconnect): inUse stays
     * set while playerConnected is cleared. The round-boundary sweep must
     * heal it. */
    sim->playerConnected[1] = FALSE;

    serverSimReturnToLobby(sim);

    UT_ASSERT_MSG(playersIsInUse(&serverSimGetGameSim(sim)->plyrs, 1) == FALSE,
                  "return-to-lobby must clear an inUse-but-disconnected slot");

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

/* ---- 2b. Last human leaving a locked game returns an empty, unlocked,
 *         bot-free lobby. ---- */
int run_empty_return_to_lobby_removes_bots_and_unlocks(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* gameLocked is a process-global in transport_udp_server.c that persists
     * across tests in the shared binary; normalise it before we start. */
    transportUdpServerSetLock(sim, FALSE);

    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetAutoLockOnGameStart(sim, true);

    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "game should be running after start");
    /* Auto-lock closed the server on game start. */
    UT_ASSERT(serverSimIsAcceptingJoins(sim) == false);
    UT_ASSERT(transportUdpServerGetLock() == true);

    /* One tick with the human present arms roundHadHuman, so the round counts
     * as one that had a human (distinguishing it from an idle bot-only round
     * whose bots must survive the round boundary). */
    serverSimTick(sim);

    /* Inject a bot into the running game. A real bot needs a brain file, so
     * mark the slot active directly, as run_host_reassign_skips_bots does.
     * numBots must move with the flag: botManagerAddBot/RemoveBot keep the
     * counter and the per-slot active flags in step, and serverSimGetNumBots
     * reports the counter, so setting active alone would leave the removal
     * path decrementing a counter that was never incremented. */
    sim->playerConnected[2] = TRUE;
    sim->botMgr.bots[2].active = true;
    sim->botMgr.numBots++;
    sim->lobbyPlayers[2].isBot = true;
    UT_ASSERT(serverSimIsBot(sim, 2) == true);

    /* The last human leaves mid-game. removePlayer's empty-lobby reset is
     * gated on lobby state, so during a running game it does not fire — the
     * return-to-lobby path owns the cleanup. */
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "no humans should remain after the last one leaves");

    /* Returning to the lobby (the game-over countdown elapsing) must wipe the
     * bots and unlock, never leaving an orphaned, locked bot-only lobby. */
    serverSimReturnToLobby(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "must be back in the lobby");
    UT_ASSERT_MSG(serverSimIsBot(sim, 2) == false,
                  "every bot must be removed once the last human leaves");
    UT_ASSERT_MSG(serverSimGetNumBots(sim) == 0,
                  "no bots should remain in the emptied lobby");
    UT_ASSERT_MSG(serverSimIsAcceptingJoins(sim) == true,
                  "the emptied lobby must be open to new players");
    UT_ASSERT_MSG(transportUdpServerGetLock() == false,
                  "the emptied lobby must not be left transport-locked");

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

/* ---- 4. Host departure promotes the lowest-numbered connected human. ---- */
int run_host_departs_promotes_lowest_human(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 3, "Player3", false);
    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == 0, "host starts at slot 0");

    serverSimRemovePlayer(sim, 0);

    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == 3,
                  "host must pass to the lowest connected human (got %u)",
                  (unsigned)serverSimGetHostSlot(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ---- 5. A non-host departure leaves the host slot unchanged. ---- */
int run_nonhost_departs_keeps_host(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 3, "Player3", false);

    serverSimRemovePlayer(sim, 3);

    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == 0,
                  "a non-host departure must not move the host (got %u)",
                  (unsigned)serverSimGetHostSlot(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ---- 6. Host reassignment skips bots and picks the next human. ---- */
int run_host_reassign_skips_bots(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 4, "Player4", false);

    /* Occupy a lower slot with a bot. A real bot needs a brain file, so
     * mark the slot active directly — serverSimIsBot reads exactly this
     * flag (botManagerIsBot -> botMgr.bots[i].active). */
    sim->playerConnected[2] = TRUE;
    sim->botMgr.bots[2].active = true;
    UT_ASSERT(serverSimIsBot(sim, 2) == true);

    serverSimRemovePlayer(sim, 0);

    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == 4,
                  "host reassignment must skip the bot at slot 2 and pick "
                  "slot 4 (got %u)", (unsigned)serverSimGetHostSlot(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ---- 7. The empty-lobby reset clears a stale host slot back to 0. ---- */
int run_lobby_reset_clears_host_slot(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Host", false);
    /* Point the host at a stale slot (neither the departing slot nor a
     * connected human) so the auto-reassign trigger does not fire and the
     * last-human lobby reset is solely responsible for clearing hostSlot. */
    serverSimSetHostSlot(sim, 2);
    UT_ASSERT(serverSimGetHostSlot(sim) == 2);

    serverSimRemovePlayer(sim, 0);   /* last human out -> lobby reset */

    UT_ASSERT_MSG(serverSimGetHostSlot(sim) == 0,
                  "the empty-lobby reset must zero hostSlot (got %u)",
                  (unsigned)serverSimGetHostSlot(sim));

    serverSimDestroy(sim);
    return 0;
}
