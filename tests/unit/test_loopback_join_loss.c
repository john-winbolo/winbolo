/*
 * Loopback join under loss.
 *
 * Same real-transport join as test_loopback_join, but the client endpoint
 * runs with loss=5 / burst=2 impairment (both directions) under a fixed
 * bolo_srand seed. The JOIN_REQUEST / JOIN_ACCEPT / MAP_ACK retransmit
 * machinery must still converge the client to CLIENT_CONNECT_CONNECTED
 * within a (larger) bounded pump count. Loss-only spec (delay=0/jitter=0)
 * keeps surviving datagrams immediately deliverable so the tight pump loop
 * never depends on wall-clock advancing.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Join retries every JOIN_RETRY_INTERVAL (50) client ticks up to
 * JOIN_MAX_RETRIES (10), and each map chunk is re-driven on loss; 5% loss
 * recovers in a few hundred pumps. 2000 is comfortably above that while
 * staying well inside the 60 s CTest timeout. */
#define LOSS_CONNECT_MAX 2000

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

int run_loopback_join_loss(void) {
    LoopbackHarness h;
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Joiner", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=5,burst=2",
                                       /*seed*/ 0x5eed1234u),
                  "harness start failed");

    int connectedAt = loopbackHarnessPumpUntil(&h, LOSS_CONNECT_MAX,
                                               pred_connected, NULL);
    fprintf(stderr, "  loopback join (loss=5,burst=2): connected after %d "
                    "pump(s) (cap %d)\n", connectedAt, LOSS_CONNECT_MAX);

    if (connectedAt < 0) {
        ClientConnectState st = clientSimGetConnectState(h.cs);
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps "
                "(final state=%d)", LOSS_CONNECT_MAX, (int)st);
    }

    loopbackHarnessStop(&h);
    return 0;
}
