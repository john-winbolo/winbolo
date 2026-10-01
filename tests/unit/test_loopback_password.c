/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * test_loopback_password.c — a password-protected server over loopback.
 *
 * The client side used to send whatever password it happened to hold and
 * show the server's reject with no way to answer it. The frontends now ask
 * before the join (from the INFO pre-flight) or after an incorrect-password
 * reject (spectate, wasm), and both need what this test pins down:
 *
 *   1. discoveryPingServer reports the password flag: clear before a
 *      password is set on the sim, set after.
 *   2. A join with the wrong password ends in CLIENT_CONNECT_ERROR and the
 *      reject's langid, read back through clientSimGetConnectErrorLangId,
 *      is STR_REJECT_INCORRECT_PASSWORD. (A join with no password at all
 *      is the same reject.)
 *   3. A join with the right password reaches the lobby.
 *
 * The password is set on the running sim through serverSimSetPassword, the
 * same call the in-lobby password toggle uses, after the harness's own
 * client has joined with none — that client is what keeps the server up;
 * the joins under test are second clients.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "discovery.h"              /* discoveryPingServer */
#include "server_sim_lifecycle.h"   /* serverSimSetPassword, serverSimSetHasPassword */
#include "server_lifecycle.h"       /* serverInstanceTick */
#include "gui/lang.h"               /* STR_REJECT_INCORRECT_PASSWORD */
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX 600
#define PASSWORD    "secret"

static bool pred_first_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsInLobby(h->cs);
}

static bool pred_second_error(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_ERROR;
}

static bool pred_second_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsInLobby(h->cs2);
}

/* Connect a second client with the given password and hand it to the
 * harness as cs2 so the pump ticks it. */
static bool second_client_join(LoopbackHarness *h, const char *name,
                               const char *password) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return false;
    clientSimCreate(cs);
    if (!clientSimConnectUdp(cs, "127.0.0.1", h->port, name,
                             /*fallbackCountry*/ "", password,
                             /*wbnApiToken*/ NULL, /*wbnServerKey*/ NULL,
                             /*wantRejoin*/ false, /*trackerAddr*/ "",
                             /*trackerPort*/ 0, /*spectator*/ false)) {
        clientSimDestroy(cs);
        return false;
    }
    h->cs2 = cs;
    h->client2Up = true;
    return true;
}

static void second_client_drop(LoopbackHarness *h) {
    if (h->cs2 == NULL) return;
    clientSimDisconnect(h->cs2);
    clientSimDestroy(h->cs2);
    h->cs2 = NULL;
    h->client2Up = false;
}

/* discoveryPingServer blocks in recvfrom until the INFO reply lands, and
 * the reply is only produced when serverInstanceTick drains the server's
 * receive queue. Tick from a second thread for the length of the ping.
 * serverInstanceTick takes the threads mutex itself (the hosted-server
 * timer on the desktop calls it from an SDL timer thread the same way). */
typedef struct {
    LoopbackHarness *h;
    volatile bool    stop;
} TickThreadArg;

static int SDLCALL tick_thread(void *userdata) {
    TickThreadArg *arg = (TickThreadArg *)userdata;
    while (!arg->stop) {
        serverInstanceTick(arg->h->sim);
        SDL_Delay(2);
    }
    return 0;
}

static bool ping_password_flag(LoopbackHarness *h, bool *flagOut) {
    DiscoveryPingResult dpr;
    TickThreadArg arg;
    SDL_Thread *th;
    bool ok;
    arg.h = h;
    arg.stop = false;
    th = SDL_CreateThread(tick_thread, "pwd-ping-tick", &arg);
    if (th == NULL) return false;
    ok = discoveryPingServer("127.0.0.1", h->port, &dpr);
    arg.stop = true;
    SDL_WaitThread(th, NULL);
    if (!ok) return false;
    *flagOut = dpr.password;
    return true;
}

int run_loopback_password(void) {
    LoopbackHarness h;
    bool flag = true;
    int at;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Host", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 7u),
                  "harness start failed");

    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_first_in_lobby, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("first client never reached the lobby within %d pumps",
                CONNECT_MAX);
    }

    /* 1a. No password yet: the INFO reply says so. */
    if (!ping_password_flag(&h, &flag)) {
        loopbackHarnessStop(&h);
        UT_FAIL("INFO ping (no password) got no reply");
    }
    if (flag) {
        loopbackHarnessStop(&h);
        UT_FAIL("INFO ping reports a password before one was set");
    }

    /* Set the password the way the lobby toggle does. */
    serverSimSetPassword(h.sim, PASSWORD, strlen(PASSWORD));
    serverSimSetHasPassword(h.sim, true);

    /* 1b. Now the INFO reply carries the flag. */
    if (!ping_password_flag(&h, &flag)) {
        loopbackHarnessStop(&h);
        UT_FAIL("INFO ping (password set) got no reply");
    }
    if (!flag) {
        loopbackHarnessStop(&h);
        UT_FAIL("INFO ping does not report the password that was set");
    }

    /* 2a. Empty password: rejected, and the reject names the reason. */
    if (!second_client_join(&h, "Empty", "")) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client (empty password) connect call failed");
    }
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_second_error, NULL);
    fprintf(stderr, "  loopback password: empty password rejected after %d "
                    "pump(s) (cap %d)\n", at, CONNECT_MAX);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("empty-password join was not rejected within %d pumps",
                CONNECT_MAX);
    }
    if (clientSimGetConnectErrorLangId(h.cs2) != STR_REJECT_INCORRECT_PASSWORD) {
        unsigned got = clientSimGetConnectErrorLangId(h.cs2);
        loopbackHarnessStop(&h);
        UT_FAIL("empty-password reject langid %u, expected %u (%s)",
                got, (unsigned)STR_REJECT_INCORRECT_PASSWORD,
                clientSimGetConnectErrorReason(h.cs2) ?
                    clientSimGetConnectErrorReason(h.cs2) : "no reason");
    }
    second_client_drop(&h);

    /* 2b. Wrong password: the same reject. */
    if (!second_client_join(&h, "Wrong", "nope")) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client (wrong password) connect call failed");
    }
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_second_error, NULL);
    fprintf(stderr, "  loopback password: wrong password rejected after %d "
                    "pump(s) (cap %d)\n", at, CONNECT_MAX);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("wrong-password join was not rejected within %d pumps",
                CONNECT_MAX);
    }
    if (clientSimGetConnectErrorLangId(h.cs2) != STR_REJECT_INCORRECT_PASSWORD) {
        unsigned got = clientSimGetConnectErrorLangId(h.cs2);
        loopbackHarnessStop(&h);
        UT_FAIL("wrong-password reject langid %u, expected %u",
                got, (unsigned)STR_REJECT_INCORRECT_PASSWORD);
    }
    second_client_drop(&h);

    /* 3. Right password: reaches the lobby. */
    if (!second_client_join(&h, "Right", PASSWORD)) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client (right password) connect call failed");
    }
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_second_in_lobby, NULL);
    fprintf(stderr, "  loopback password: right password in lobby after %d "
                    "pump(s) (cap %d)\n", at, CONNECT_MAX);
    if (at < 0) {
        const char *reason = clientSimGetConnectErrorReason(h.cs2);
        loopbackHarnessStop(&h);
        UT_FAIL("right-password join never reached the lobby within %d "
                "pumps (%s)", CONNECT_MAX, reason ? reason : "no reason");
    }

    loopbackHarnessStop(&h);
    return 0;
}
