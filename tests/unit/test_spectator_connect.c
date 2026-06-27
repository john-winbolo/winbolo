/*
 * Spectator connect test — client side, clean path.
 *
 * The server-side spectator JOIN accept is covered by test_spectator_join.c
 * (raw hand-built sockets). This test drives the REAL client transport: a
 * ClientSim connects with the spectator flag set (clientSimConnectUdp's
 * spectator arg), runs the JOIN_REQUEST / challenge / JOIN_ACCEPT handshake,
 * and must land in CLIENT_CONNECT_SPECTATING — the tankless awaiting-seed
 * state — rather than CONNECTED.
 *
 * Asserts the connect is genuinely tankless: the client reaches SPECTATING
 * (never CONNECTED or DOWNLOADING_MAP), the server counts exactly one
 * spectator, and no tank slot was consumed (the server's player roster stays
 * empty — the harness connects no peer player). No seed/feed bytes are
 * exercised here; that is a later slice.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "transport_udp.h"     /* transportUdpServerGetSpectatorCount */
#include "server_sim.h"        /* serverSimGetNumPlayers */
#include "test_harness.h"
#include "loopback_harness.h"

/* The spectator handshake (join → challenge → cookie echo → accept) has no map
 * download, so it lands well inside this bound; the generous cap keeps a hang
 * distinguishable from a slow pass. */
#define SPEC_CONNECT_MAX 600

static bool pred_spectating(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

int run_spectator_connect(void) {
    LoopbackHarness h;
    ClientConnectState st;

    UT_ASSERT_MSG(loopbackHarnessStartSpectator(&h, "Spectator", /*seed*/ 1u),
                  "spectator harness start failed");

    int spectatingAt = loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX,
                                                 pred_spectating, NULL);
    fprintf(stderr, "  spectator connect: reached SPECTATING after %d pump(s) "
                    "(cap %d)\n", spectatingAt, SPEC_CONNECT_MAX);
    if (spectatingAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator client never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }

    /* Exactly SPECTATING — not the player-join terminal states. The accept
     * branch must not have funnelled the spectator into the live pipeline. */
    st = clientSimGetConnectState(h.cs);
    UT_ASSERT_MSG(st == CLIENT_CONNECT_SPECTATING,
                  "spectator must rest in SPECTATING, not a player state");
    UT_ASSERT_MSG(st != CLIENT_CONNECT_CONNECTED,
                  "spectator must not reach CONNECTED");
    UT_ASSERT_MSG(st != CLIENT_CONNECT_DOWNLOADING_MAP,
                  "spectator must not start a map download");

    /* Server side: one tankless viewer registered, no tank slot consumed. The
     * harness connects no peer player, so the player roster must stay empty. */
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "server must count exactly one spectator");
    UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == 0,
                  "spectator connect must consume no tank slot");

    loopbackHarnessStop(&h);
    return 0;
}
