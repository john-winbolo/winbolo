/*
 * Shared connect/landing wait (src/client_frontend/client_frontend_connect.c).
 *
 * The landing predicate is the contract every platform shares: accept the
 * join when the connect state reaches CONNECTED *or* the lobby latches
 * (lobby-enabled servers open the lobby while the map is still
 * downloading — a check that only accepts CONNECTED hangs every lobby
 * join until timeout).
 *
 *   await_join_connected_immediate — a synchronous local connect lands
 *       CONNECTED; the wait returns true without consuming the timeout.
 *   await_join_lobby_latch         — inLobby alone (state not CONNECTED)
 *       is accepted, and the wait settles netStatus to netLobby.
 *   await_join_timeout_and_error   — a JOINING transport that never
 *       progresses (UDP to a dead port) times out false; a terminal
 *       state mid-wait (DISCONNECTED after teardown) exits false
 *       without burning the timeout.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_frontend_connect.h"
#include "test_harness.h"

int run_await_join_connected_immediate(void) {
    ServerSim *sim = ut_make_running_sim("Lander");
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimConnectLocal(cs, sim, "Lander", "", 0, 0));
    UT_ASSERT(clientSimGetConnectState(cs) == CLIENT_CONNECT_CONNECTED);

    uint64_t t0 = SDL_GetTicks();
    UT_ASSERT(clientFrontAwaitJoin(cs, 1500));
    /* CONNECTED breaks the loop before any 20ms sleep; allow generous
     * scheduler slack but catch a wait that burned the timeout. */
    UT_ASSERT_MSG(SDL_GetTicks() - t0 < 1000,
                  "await consumed the timeout despite CONNECTED state");

    clientSimDisconnect(cs);
    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

int run_await_join_lobby_latch(void) {
    /* No transport at all: connect state reads DISCONNECTED (never
     * CONNECTED), so acceptance can only come from the lobby latch. */
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimGetConnectState(cs) == CLIENT_CONNECT_DISCONNECTED);

    UT_ASSERT_MSG(!clientFrontAwaitJoin(cs, 2),
                  "no transport and no lobby must not land");

    clientSimSetInLobby(cs, true);
    UT_ASSERT_MSG(clientFrontAwaitJoin(cs, 2),
                  "lobby latch must be accepted without CONNECTED");
    UT_ASSERT_MSG(clientSimGetNetStatus(cs) == netLobby,
                  "landing in the lobby must settle netStatus to netLobby");

    clientSimDestroy(cs);
    return 0;
}

int run_await_join_timeout_and_error(void) {
    /* UDP connect to a localhost port nothing listens on: the transport
     * binds and queues its JOIN, then sits in JOINING forever. */
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimConnectUdp(cs, "127.0.0.1", 39999, "Nobody", "",
                                  "", "", "", false, "", 0, false));
    UT_ASSERT(clientSimGetConnectState(cs) == CLIENT_CONNECT_JOINING);
    UT_ASSERT(!clientSimTransportTicksServer(cs));

    /* 3 iterations x 20ms — a real (bounded) timeout path. */
    UT_ASSERT_MSG(!clientFrontAwaitJoin(cs, 3),
                  "stuck-JOINING join reported success");

    /* A terminal state exits false without burning the timeout. (The
     * transport reaches ERROR by itself only via its 10-retry cap —
     * ~10s — so use the cheap terminal state: DISCONNECTED after
     * teardown. The loop must break on any state that is neither
     * JOINING nor DOWNLOADING_MAP.) */
    clientSimDisconnect(cs);
    UT_ASSERT(clientSimGetConnectState(cs) == CLIENT_CONNECT_DISCONNECTED);
    uint64_t t0 = SDL_GetTicks();
    UT_ASSERT(!clientFrontAwaitJoin(cs, 1500));
    UT_ASSERT_MSG(SDL_GetTicks() - t0 < 1000,
                  "terminal state still burned the timeout");

    clientSimDestroy(cs);
    return 0;
}
