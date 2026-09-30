/*
 * Auto-close on an empty server (test_autoclose_empty_server.c).
 *
 * -autoclose quits the server once everyone has left. The check behind it,
 * serverSimCheckAutoClose, is a latch: hadPlayersEver goes TRUE the first time
 * a player is connected, and from then on an empty roster means "they have all
 * left". Everything here turns on that latch reading only real arrivals — a
 * server that was never joined must not read as one everybody left.
 *
 * Two things set the latch without anybody joining, and both used to shut a
 * -nolobby -autoclose server down on its first tick, before a single client
 * could connect:
 *
 *  1. The no-lobby boot. serverSimApplyInstanceConfig runs serverSimStartGame
 *     for a skipLobby server, and starting a round latches the flag. The boot
 *     must re-arm it — the round begins empty and waits for joiners. The
 *     re-arm was there for -maprotate only (test_maprotate_rotation.c pins
 *     that half), so plain -nolobby -autoclose quit at startup.
 *
 *  2. A bot whose runner fails to build. botManagerAddBot takes the seat
 *     first, which latches the flag, and hands it back when the brain cannot
 *     be loaded. The seat is gone but the latch stayed set, so a server whose
 *     bots all failed to build was an empty server that read as abandoned.
 *
 * The other direction is pinned too: once somebody really has joined and left,
 * the check must still fire, in both lobby and no-lobby mode. That is the
 * flag's whole job and the re-arms must not cost it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* ServerSim fields: hadPlayersEver */
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig, serverSimSetLobbyEnabled */
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "bot_manager.h"           /* botManagerAddBot */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* A -autoclose ServerSim from the embedded Everard map, before any boot. */
static ServerSim *make_autoclose_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetAutoCloseOnEmpty(sim, true);
    return sim;
}

/* The operator's startup config, as servermain builds it for -nolobby
 * (skipLobby) or for a lobby server (lobbyEnabled). */
static void boot_sim(ServerSim *sim, bool skipLobby) {
    ServerInstanceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.skipLobby    = skipLobby;
    cfg.lobbyEnabled = !skipLobby;
    cfg.botAiType    = (BYTE)aiNone;
    serverSimApplyInstanceConfig(sim, &cfg);
}

/* -nolobby -autoclose: the round starts at boot with nobody in it. The check
 * must not read that as an abandoned server, or the process quits during
 * startup and no client ever gets the chance to connect. */
int run_autoclose_boot_does_not_close_while_empty(void) {
    ServerSim *sim = make_autoclose_sim();
    UT_ASSERT(sim != NULL);

    boot_sim(sim, /*skipLobby*/ true);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "no-lobby boot must start a running round");
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 0,
                  "the booted round starts with nobody in it");
    UT_ASSERT_MSG(sim->hadPlayersEver == FALSE,
                  "boot must re-arm the empty-server check — starting the "
                  "round is not a player arriving");
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "an empty just-booted no-lobby server must not close before "
                  "anyone can join");

    /* Still armed after repeated checks: the latch only moves on an arrival. */
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "the check must stay quiet while the server is untouched");

    serverSimDestroy(sim);
    return 0;
}

/* The flag's actual job on a -nolobby server: hold open through the round,
 * then close once the last player has gone. */
int run_autoclose_closes_after_last_player_leaves(void) {
    ServerSim *sim = make_autoclose_sim();
    UT_ASSERT(sim != NULL);

    boot_sim(sim, /*skipLobby*/ true);
    UT_ASSERT(serverSimCheckAutoClose(sim) == FALSE);

    serverSimAddPlayer(sim, 0, "Joiner", false);
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "a server with a player in it must not close");
    UT_ASSERT_MSG(sim->hadPlayersEver == TRUE,
                  "a real join must latch the empty-server check");

    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 0,
                  "the leaver's slot must be free again");
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == TRUE,
                  "-autoclose must close the server once the last player has "
                  "left");

    serverSimDestroy(sim);
    return 0;
}

/* A lobby server sits in the lobby waiting for joiners, so the same "nobody
 * has been here yet" rule applies to its boot; the close still comes once a
 * joiner has come and gone. */
int run_autoclose_lobby_boot_waits_for_a_joiner(void) {
    ServerSim *sim = make_autoclose_sim();
    UT_ASSERT(sim != NULL);

    boot_sim(sim, /*skipLobby*/ false);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "a lobby boot must come up in the lobby");
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "an empty lobby nobody has joined yet must not close");

    serverSimAddPlayer(sim, 0, "Joiner", false);
    UT_ASSERT(serverSimCheckAutoClose(sim) == FALSE);

    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == TRUE,
                  "-autoclose must close a lobby server once its last player "
                  "has left");

    serverSimDestroy(sim);
    return 0;
}

/* A bot that cannot be built takes a seat on the way in and gives it back on
 * the way out. The server is as empty afterwards as it was before, and the
 * check must read it that way — otherwise -bots with an unreadable brain
 * closes the server at startup, the same shutdown as the boot latch above and
 * from a configuration that looks nothing like it. */
int run_autoclose_failed_bot_add_leaves_check_armed(void) {
    ServerSim *sim = make_autoclose_sim();
    UT_ASSERT(sim != NULL);

    boot_sim(sim, /*skipLobby*/ true);
    UT_ASSERT(sim->hadPlayersEver == FALSE);

    /* No brain lives at this path, so the runner build fails and the add
     * unwinds the seat it took. */
    bool added = botManagerAddBot(sim, 0, "tests/unit/no_such_brain.lua",
                                  "Bot1", aiNone, gameOpen, false, 0, NULL);
    UT_ASSERT_MSG(added == false,
                  "a bot with no brain to load must fail to add");
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 0,
                  "the failed add must leave no player behind");
    UT_ASSERT_MSG(sim->hadPlayersEver == FALSE,
                  "a seat taken and handed back is not a player who joined "
                  "and left — the empty-server check must stay armed");
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "a server whose bots all failed to build must stay up and "
                  "wait for joiners");

    serverSimDestroy(sim);
    return 0;
}
