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
 *
 * Only people count, by the empty reset's rule (serverSimGetNumHumans): a
 * server bot (or a Bot Host seat, once the server has Bot Host connections)
 * is not someone playing. The check used to
 * count every connected seat, so with bots on the server -autoclose never
 * quit after the last person left, and -maprotate never rotated. The last
 * three cases pin that: the last person leaving closes the server with the
 * bots still on it, and a server that only ever had bots never closes.
 *
 * The last two cases run real server ticks on a lobby server, as WinBoloDS
 * runs them: the last person leaving a round, or every person quitting in
 * the game-over hold, ends the round, the hold runs out, the server goes
 * back to the lobby, and there -autoclose must quit it. The world reset on
 * the way back clears the empty-server latch, so the return to the lobby
 * has to set it again for a round that had a person in it.
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
#include "server_sim_scenario.h"   /* serverSimAddUnfieldedSeat */
#include "everard_map.h"           /* E_MAP */
#include "client_net.h"            /* clientSimGetConnectState */
#include "client_connect_state.h"  /* CLIENT_CONNECT_SPECTATING */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

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

/* -- Only people count ------------------------------------------------ */

#define AC_HUMAN_SLOT 0
#define AC_BOT_SLOT   1
#define AC_HELD_SLOT  2

/* Two bot seats, as test_empty_reset.c makes them: a server bot (a seat with
 * an active bot manager entry, no brain needed for the count) and a held
 * seat, which is what the sim holds for a bot not on the field yet, and what
 * a Bot Host will bind to once the server has Bot Host connections. Both
 * answer serverSimIsBot. */
static bool add_bot_seats(ServerSim *sim) {
    serverSimAddPlayer(sim, AC_BOT_SLOT, "Botty", false);
    sim->botMgr.bots[AC_BOT_SLOT].active = true;
    if (!serverSimAddUnfieldedSeat(sim, AC_HELD_SLOT, "Held", 2)) {
        return false;
    }
    return serverSimIsBot(sim, AC_BOT_SLOT) && serverSimIsBot(sim, AC_HELD_SLOT);
}

/* The tester's case: a -nolobby -autoclose server with bots. A person joins
 * and leaves; the bots play on. The server must close, as it does with no
 * bots. */
int run_autoclose_human_leaves_bots_stay_closes(void) {
    ServerSim *sim = make_autoclose_sim();
    UT_ASSERT(sim != NULL);

    boot_sim(sim, /*skipLobby*/ true);
    UT_ASSERT_MSG(add_bot_seats(sim), "setup: both bot seats must read as bots");
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "bots alone must not close the server");

    serverSimAddPlayer(sim, AC_HUMAN_SLOT, "Person", false);
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "a server with a person in it must not close");
    UT_ASSERT_MSG(sim->hadPlayersEver == TRUE,
                  "a person on the server must latch the empty-server check");

    serverSimRemovePlayer(sim, AC_HUMAN_SLOT);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 2 &&
                  serverSimGetNumHumans(sim) == 0,
                  "setup: the two bot seats stay, no person is left "
                  "(%u seats, %u humans)",
                  (unsigned)serverSimGetNumPlayers(sim),
                  (unsigned)serverSimGetNumHumans(sim));
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == TRUE,
                  "the last person left: -autoclose must close the server "
                  "with the bots still on it");

    sim->botMgr.bots[AC_BOT_SLOT].active = false;
    serverSimDestroy(sim);
    return 0;
}

/* A server that only ever had bots (a bench, a test arena, an idle server)
 * never had anybody to leave it: it must not close while the bots play, nor
 * once they have gone. */
int run_autoclose_bots_only_never_closes(void) {
    ServerSim *sim = make_autoclose_sim();
    int i;
    UT_ASSERT(sim != NULL);

    boot_sim(sim, /*skipLobby*/ true);
    UT_ASSERT_MSG(add_bot_seats(sim), "setup: both bot seats must read as bots");
    UT_ASSERT_MSG(sim->hadPlayersEver == FALSE,
                  "a bot taking its seat is not a person arriving");

    for (i = 0; i < 100; i++) {
        UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                      "a bots-only server closed on check %d", i);
    }
    UT_ASSERT_MSG(sim->hadPlayersEver == FALSE,
                  "bots alone must never latch the empty-server check");

    /* The bots go too. Still nobody has been here. */
    sim->botMgr.bots[AC_BOT_SLOT].active = false;
    serverSimRemovePlayer(sim, AC_BOT_SLOT);
    serverSimRemovePlayer(sim, AC_HELD_SLOT);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 0,
                  "setup: the bot seats must be free again, %u connected",
                  (unsigned)serverSimGetNumPlayers(sim));
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "bots leaving a server nobody joined must not close it");
    UT_ASSERT_MSG(sim->hadPlayersEver == FALSE,
                  "the check must stay armed for the first real joiner");

    serverSimDestroy(sim);
    return 0;
}

/* The old behaviour with no bots, through a lobby round: a person joins the
 * lobby, the round starts (serverSimStartGame latches with a person in it),
 * the person leaves, and the server closes. */
int run_autoclose_lobby_round_human_leaves_closes(void) {
    ServerSim *sim = make_autoclose_sim();
    UT_ASSERT(sim != NULL);

    boot_sim(sim, /*skipLobby*/ false);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);
    UT_ASSERT(serverSimCheckAutoClose(sim) == FALSE);

    serverSimAddPlayer(sim, AC_HUMAN_SLOT, "Person", false);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "setup: the round must start");
    UT_ASSERT_MSG(sim->hadPlayersEver == TRUE,
                  "a round started with a person in it must latch the check");
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == FALSE,
                  "a round with a person in it must not close");

    serverSimRemovePlayer(sim, AC_HUMAN_SLOT);
    UT_ASSERT_MSG(serverSimCheckAutoClose(sim) == TRUE,
                  "-autoclose must close the server once its last person has "
                  "left the round");

    serverSimDestroy(sim);
    return 0;
}

/* -- Real ticks on a lobby server ------------------------------------- */

#define AC_SPEC_MAX  800   /* spectator join handshake */
#define AC_RUN_MAX   1000  /* lobby readied -> countdown -> running */
#define AC_CLOSE_MAX 800   /* round end -> game-over hold -> lobby -> close */

static bool ac_pred_spectating(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

static bool ac_pred_running(LoopbackHarness *h, void *u) {
    (void)u;
    return serverSimGetState(h->sim) == serverStateRunning;
}

static bool ac_pred_game_over(LoopbackHarness *h, void *u) {
    (void)u;
    return serverSimGetState(h->sim) == serverStateGameOver;
}

/* A -autoclose lobby server watched by one spectator (the harness's wire
 * client, who holds no player seat), with one person seated in the sim and
 * the round started from the lobby. Returns NULL with the round running, or
 * a failure message (the harness is then stopped). */
static const char *ac_start_lobby_round(LoopbackHarness *h, uint64_t seed) {
    if (!loopbackHarnessStartSpectatorLobby(h, "Watcher", seed)) {
        loopbackHarnessStop(h);
        return "the spectator lobby harness did not start";
    }
    if (loopbackHarnessPumpUntil(h, AC_SPEC_MAX, ac_pred_spectating,
                                 NULL) < 0) {
        loopbackHarnessStop(h);
        return "the spectator never reached SPECTATING";
    }
    threadsWaitForMutex();
    serverSimSetAutoCloseOnEmpty(h->sim, true);
    serverSimAddPlayer(h->sim, AC_HUMAN_SLOT, "Person", false);
    serverSimSetReady(h->sim, AC_HUMAN_SLOT, true);
    serverSimLobbyCheckAllReady(h->sim);
    threadsReleaseMutex();
    if (loopbackHarnessPumpUntil(h, AC_RUN_MAX, ac_pred_running, NULL) < 0) {
        loopbackHarnessStop(h);
        return "the round never started from the lobby";
    }
    /* A few frames of play, so the round has seen its person. */
    loopbackHarnessPumpUntil(h, 10, NULL, NULL);
    return NULL;
}

/* Pump until the server is in a game over that shuts it down. Returns the
 * pump it closed on, or -1. sawLobby says whether a pump ended in the lobby;
 * it is for the log only, as the return to the lobby and the close that
 * follows it can fall in the same pump. */
static int ac_pump_until_closed(LoopbackHarness *h, bool *sawLobby) {
    int i;
    *sawLobby = false;
    for (i = 1; i <= AC_CLOSE_MAX; i++) {
        bool closed;
        loopbackHarnessPump(h);
        threadsWaitForMutex();
        if (serverSimGetState(h->sim) == serverStateLobby) *sawLobby = true;
        closed = serverSimIsTerminalGameOver(h->sim);
        threadsReleaseMutex();
        if (closed) return i;
    }
    return -1;
}

/* The last person leaves a running lobby round. The round ends at once (the
 * tick ends a round whose people have all gone), the hold runs out, and the
 * server goes back to the lobby. There -autoclose must quit it. */
int run_autoclose_lobby_round_ticks_to_lobby_closes(void) {
    LoopbackHarness h;
    const char     *err = ac_start_lobby_round(&h, 31u);
    bool            sawLobby = false;
    int             closedAt;
    ServerState     st;

    if (err != NULL) UT_FAIL("setup: %s", err);

    threadsWaitForMutex();
    serverSimRemovePlayer(h.sim, AC_HUMAN_SLOT);
    threadsReleaseMutex();

    closedAt = ac_pump_until_closed(&h, &sawLobby);
    threadsWaitForMutex();
    st = serverSimGetState(h.sim);
    threadsReleaseMutex();
    fprintf(stderr, "  closed on pump %d, through the lobby=%d, state=%d\n",
            closedAt, (int)sawLobby, (int)st);
    if (closedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the last person left a lobby round: -autoclose must quit "
                "the server once it is back in the lobby (closed on pump %d, "
                "through the lobby=%d, state=%d)", closedAt, (int)sawLobby,
                (int)st);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* The round ends with the person still in it (here by its time limit, as a
 * win would), and the person quits during the 3-second game-over hold. The
 * server goes back to the lobby with nobody in it, and -autoclose must quit
 * it there. */
int run_autoclose_lobby_all_quit_in_hold_closes(void) {
    LoopbackHarness h;
    const char     *err = ac_start_lobby_round(&h, 32u);
    bool            sawLobby = false;
    int             closedAt;
    ServerState     st;

    if (err != NULL) UT_FAIL("setup: %s", err);

    threadsWaitForMutex();
    h.sim->gameLength = 2;
    threadsReleaseMutex();
    if (loopbackHarnessPumpUntil(&h, 20, ac_pred_game_over, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("setup: the round did not end on its time limit");
    }
    /* Part of the hold has run; the person quits in it. */
    loopbackHarnessPumpUntil(&h, 10, NULL, NULL);
    threadsWaitForMutex();
    st = serverSimGetState(h.sim);
    if (st == serverStateGameOver) {
        serverSimRemovePlayer(h.sim, AC_HUMAN_SLOT);
    }
    threadsReleaseMutex();
    if (st != serverStateGameOver) {
        loopbackHarnessStop(&h);
        UT_FAIL("setup: the game-over hold ended too soon (state=%d)",
                (int)st);
    }

    closedAt = ac_pump_until_closed(&h, &sawLobby);
    threadsWaitForMutex();
    st = serverSimGetState(h.sim);
    threadsReleaseMutex();
    fprintf(stderr, "  closed on pump %d, through the lobby=%d, state=%d\n",
            closedAt, (int)sawLobby, (int)st);
    if (closedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("every person quit in the game-over hold: -autoclose must "
                "quit the server once it is back in the lobby (closed on "
                "pump %d, through the lobby=%d, state=%d)", closedAt,
                (int)sawLobby, (int)st);
    }

    loopbackHarnessStop(&h);
    return 0;
}
