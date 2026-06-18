/*
 * Auto-lock-on-game-start behaviour (test_autolock_on_game_start.c).
 *
 * "Auto-lock on game start" must let players join while the server is in
 * the lobby, then close the server to new joiners the instant the round
 * begins — and that closure must reach both the join gate and WinBolo.net.
 *
 * The join gate has two predicates (transport_udp_server.c):
 *     udpServer.gameLocked || !serverSimIsAcceptingJoins(sim)
 * serverSimIsAcceptingJoins is sim->allowNewPlayers; gameLocked is the
 * transport admin lock, which is also what fires winboloNetSendLock().
 * Auto-lock-on-start must set both so the local-join path (allowNewPlayers
 * only) and the UDP path (either predicate) agree, and so WBN is told.
 *
 * These tests exercise the two game-start entry points:
 *   - serverSimStartGame        — countdown/wire path (networked games,
 *                                 dedicated servers, map rotation: the
 *                                 only games that talk to WBN)
 *   - serverSimStartGameInPlace — no-countdown in-process start
 *
 * The unit-test binary links the real transport_udp_server.c, so
 * transportUdpServerGetLock() reflects the actual lock, and the
 * winboloNetSendLock() stub records what the lock path reported.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "everard_map.h"           /* E_MAP */
#include "player_flags.h"          /* CLIENT_TYPE_UNKNOWN */
#include "server_sim.h"            /* serverSimIsAcceptingJoins, GetState */
#include "server_sim_lifecycle.h"  /* serverSimStartGame[InPlace], SetAutoLock */
#include "server_sim_join.h"       /* serverSimLocalJoin, LocalJoinResult */
#include "transport_udp.h"         /* transportUdpServerSetLock / GetLock */
#include "test_harness.h"

/* WBN send-lock spy, defined in test_stubs.c. */
extern int  wbnStubSendLockCalls;
extern bool wbnStubLastLockReported;

/* Lobby-enabled ServerSim from the embedded Everard map. Caller owns it. */
static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* gameLocked is a process-global in transport_udp_server.c that persists
 * across tests in the shared binary. Normalise it to unlocked and zero the
 * WBN spy so each scenario starts from a known baseline. */
static void reset_lock_baseline(ServerSim *sim) {
    transportUdpServerSetLock(sim, FALSE);
    wbnStubSendLockCalls = 0;
    wbnStubLastLockReported = FALSE;
}

/* ---- 1. Auto-lock leaves the lobby open to joiners. ---- */
int run_autolock_lobby_stays_open(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetAutoLockOnGameStart(sim, true);
    reset_lock_baseline(sim);

    /* Still in the lobby — the setting must not pre-emptively close it. */
    UT_ASSERT_MSG(serverSimIsAcceptingJoins(sim) == true,
                  "auto-lock lobby must accept joins before the game starts");
    UT_ASSERT_MSG(transportUdpServerGetLock() == false,
                  "transport lock must stay off in the lobby");

    /* A real join in the lobby succeeds. */
    BYTE slot = 0xFF;
    LocalJoinResult r = serverSimLocalJoin(sim, "Lobbyist", "",
                                           CLIENT_TYPE_UNKNOWN, 0, &slot);
    UT_ASSERT_MSG(r == LOCAL_JOIN_OK,
                  "lobby join must be accepted (got %d)", (int)r);

    serverSimDestroy(sim);
    return 0;
}

/* ---- 2. Wire/countdown start locks the game and reports it to WBN. ---- */
int run_autolock_locks_on_start_wire(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetAutoLockOnGameStart(sim, true);
    reset_lock_baseline(sim);

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    /* Both join predicates now say "no": local-join gate + transport lock. */
    UT_ASSERT_MSG(serverSimIsAcceptingJoins(sim) == false,
                  "auto-lock must close the join gate at game start");
    UT_ASSERT_MSG(transportUdpServerGetLock() == true,
                  "auto-lock must raise the transport lock at game start");

    /* And the lock was reported to WinBolo.net as locked=true. */
    UT_ASSERT_MSG(wbnStubSendLockCalls >= 1,
                  "auto-lock must report the lock to WBN (calls=%d)",
                  wbnStubSendLockCalls);
    UT_ASSERT_MSG(wbnStubLastLockReported == true,
                  "WBN must be told locked=true on auto-lock");

    serverSimDestroy(sim);
    return 0;
}

/* ---- 3. In-place start locks the game and reports it to WBN. ---- */
int run_autolock_locks_on_start_inplace(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetAutoLockOnGameStart(sim, true);
    reset_lock_baseline(sim);

    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    UT_ASSERT_MSG(serverSimIsAcceptingJoins(sim) == false,
                  "in-place auto-lock must close the join gate");
    UT_ASSERT_MSG(transportUdpServerGetLock() == true,
                  "in-place auto-lock must raise the transport lock");
    UT_ASSERT_MSG(wbnStubLastLockReported == true && wbnStubSendLockCalls >= 1,
                  "in-place auto-lock must report locked=true to WBN "
                  "(calls=%d, last=%d)",
                  wbnStubSendLockCalls, (int)wbnStubLastLockReported);

    serverSimDestroy(sim);
    return 0;
}

/* ---- 4. A join that arrives once the game is locked is rejected. ---- */
int run_autolock_join_blocked_after_start(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetAutoLockOnGameStart(sim, true);
    reset_lock_baseline(sim);

    /* Lobby: a late joiner gets in. */
    BYTE slot = 0xFF;
    UT_ASSERT_MSG(serverSimLocalJoin(sim, "Early", "", CLIENT_TYPE_UNKNOWN,
                                     0, &slot) == LOCAL_JOIN_OK,
                  "join must succeed while still in the lobby");

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    /* In-game: the next joiner is turned away by the lock. */
    LocalJoinResult r = serverSimLocalJoin(sim, "Latecomer", "",
                                           CLIENT_TYPE_UNKNOWN, 0, &slot);
    UT_ASSERT_MSG(r == LOCAL_JOIN_GAME_LOCKED,
                  "join after an auto-locked start must be rejected (got %d)",
                  (int)r);

    serverSimDestroy(sim);
    return 0;
}

/* ---- 5. Without auto-lock, the game stays open and WBN is not told. ---- */
int run_no_autolock_stays_open_on_start(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetAutoLockOnGameStart(sim, false);
    reset_lock_baseline(sim);

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    UT_ASSERT_MSG(serverSimIsAcceptingJoins(sim) == true,
                  "no auto-lock: the running game must still accept joins");
    UT_ASSERT_MSG(transportUdpServerGetLock() == false,
                  "no auto-lock: the transport lock must stay off");
    UT_ASSERT_MSG(wbnStubLastLockReported == false,
                  "no auto-lock: WBN must not be told the game is locked");

    /* And a join during the running game is admitted. */
    BYTE slot = 0xFF;
    UT_ASSERT_MSG(serverSimLocalJoin(sim, "Walkup", "", CLIENT_TYPE_UNKNOWN,
                                     0, &slot) == LOCAL_JOIN_OK,
                  "no auto-lock: an in-game join must be accepted");

    serverSimDestroy(sim);
    return 0;
}
