/*
 * Loopback join smoke test — clean path.
 *
 * A real UDP client joins a real, in-process UDP server over localhost
 * sockets with no impairment: it runs the JOIN_REQUEST / JOIN_ACCEPT /
 * map-download handshake to CLIENT_CONNECT_CONNECTED within a bounded
 * pump count, then exchanges at least one input packet and receives at
 * least one snapshot. Proves the loopback harness stands both transports
 * up and that the join + snapshot path works end-to-end before the loss
 * variants pile impairment on top.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "input_packet.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Clean-path join lands in well under 100 pumps in practice; the bound is
 * generous so a hang is clearly distinguishable from a slow pass. The UDP
 * client applies each snapshot inline and surfaces arrivals only through its
 * per-second (100-tick) net-stats window, so snapshotsRecv first reads
 * non-zero just after the window rolls over near pump ~100. */
#define CLEAN_CONNECT_MAX 600
#define SNAPSHOT_MAX      600

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

int run_loopback_join(void) {
    LoopbackHarness h;
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Joiner", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    int connectedAt = loopbackHarnessPumpUntil(&h, CLEAN_CONNECT_MAX,
                                               pred_connected, NULL);
    fprintf(stderr, "  loopback join (clean): connected after %d pump(s) "
                    "(cap %d)\n", connectedAt, CLEAN_CONNECT_MAX);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps",
                CLEAN_CONNECT_MAX);
    }

    /* Exchange: feed input each pump (sends up-link, drives the running
     * sim) and watch the client's snapshotsRecv net stat register the
     * down-link. The UDP client applies snapshots inline, so this per-second
     * window counter is the observable that one was received and accepted. */
    BYTE slot = clientSimGetMyPlayerNum(h.cs);
    int snapshotAt = -1;
    int i;
    for (i = 1; i <= SNAPSHOT_MAX; i++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick      = (uint32_t)i;
        pkt.playerNum = slot;
        clientSimNetSendInput(h.cs, &pkt);
        loopbackHarnessPump(&h);

        int ppsRecv = 0, ppsSent = 0, bpsRecv = 0, bpsSent = 0, numErrors = 0;
        int snapsRecv = 0, snapsLost = 0, snapsLostTotal = 0;
        clientSimGetUdpNetStats(h.cs, &ppsRecv, &ppsSent, &bpsRecv, &bpsSent,
                                &numErrors, &snapsRecv, &snapsLost,
                                &snapsLostTotal);
        if (snapsRecv > 0) {
            snapshotAt = i;
            break;
        }
    }
    fprintf(stderr, "  loopback join (clean): snapshotsRecv > 0 after %d "
                    "pump(s) (cap %d)\n", snapshotAt, SNAPSHOT_MAX);

    if (snapshotAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no snapshot registered within %d pumps", SNAPSHOT_MAX);
    }

    loopbackHarnessStop(&h);
    return 0;
}
