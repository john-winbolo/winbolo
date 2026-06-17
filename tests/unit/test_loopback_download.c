/*
 * Loopback integration tests for the map join-download and live resync now
 * carried on the reliable bulk channel (CHANNEL_BULK), driven over the real
 * loopback transport under loss.
 *
 * Three cases:
 *   1. Join download (lobby server): a fresh client joins a lobby server under
 *      loss and must reassemble the compressed map on CHANNEL_BULK to reach
 *      CONNECTED. The client only acks CHANNEL_BULK once the standalone
 *      PACKET_CHANNEL carrier fires during DOWNLOADING_MAP (the carrier-gate
 *      fix); without it the server window would stall a window in and a map
 *      would wedge the join. Reaching CONNECTED proves the download completed
 *      and fired CTRL_MAP_DOWNLOAD_COMPLETE (which flips the connect state).
 *   2. Mid-game joiner (running server): the deadlock case. The server is
 *      Running, so the snapshot trailer that normally carries the bulk stream
 *      is itself gated behind downloadComplete. The fix has the server emit a
 *      standalone PACKET_CHANNEL carrier to a not-yet-complete client even while
 *      Running; without it the joiner never receives the map and never connects.
 *   3. Resync end-to-end (running server): once connected, the client reports a
 *      map-checksum mismatch, which sends a MAP_RESYNC_REQUEST. The server
 *      streams the live map on CHANNEL_BULK as a RESYNC transfer; the client
 *      hot-swaps it in and bumps its resync count — proof the resync installed.
 *
 * Snapshot gating until completion is exercised implicitly: a running joiner
 * that received snapshots before its map installed would corrupt its sim and
 * never settle to CONNECTED.
 */
#include <stdint.h>
#include <stdio.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_sim.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX  8000   /* join + bulk download convergence under loss */
#define RESYNC_MAX   8000   /* request + bulk resync convergence under loss */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_resynced(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetMapResyncCount(h->cs) >= 1;
}

/* Case 1: a lobby join download completes on CHANNEL_BULK under loss. */
int run_loopback_download_join(void) {
    LoopbackHarness h;
    int connectedAt;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "DlJoin", /*lobbyMode*/ true,
                                       /*impairSpec*/ "loss=10", /*seed*/ 0x0D10A1u),
                  "harness start (download join) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    fprintf(stderr, "  loopback download join (loss=10): connected@%d\n",
            connectedAt);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("join download never completed within %d pumps", CONNECT_MAX);
    }
    loopbackHarnessStop(&h);
    return 0;
}

/* Case 2: a mid-game joiner downloads while the server is Running — the carrier
 * deadlock case. The server must emit a standalone bulk carrier to the
 * incomplete client even though snapshots (the usual carrier) are gated. */
int run_loopback_download_midgame(void) {
    LoopbackHarness h;
    int connectedAt;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "DlMid", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10", /*seed*/ 0x31DEADu),
                  "harness start (mid-game download) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    fprintf(stderr, "  loopback mid-game download (running, loss=10): connected@%d\n",
            connectedAt);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("mid-game joiner never completed its download within %d pumps "
                "(running-phase bulk carrier deadlock?)", CONNECT_MAX);
    }
    loopbackHarnessStop(&h);
    return 0;
}

/* Case 3: a live resync streams on CHANNEL_BULK and installs, bumping the
 * client's resync count. */
int run_loopback_resync(void) {
    LoopbackHarness h;
    int connectedAt, resyncedAt;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Resync", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10", /*seed*/ 0x5E54C0u),
                  "harness start (resync) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    /* Report a terrain checksum mismatch — the client sends MAP_RESYNC_REQUEST
     * and the server streams the live map back as a RESYNC bulk transfer. */
    clientSimNetReportMapChecksum(h.cs, false);

    resyncedAt = loopbackHarnessPumpUntil(&h, RESYNC_MAX, pred_resynced, NULL);
    fprintf(stderr, "  loopback resync (running, loss=10): connected@%d resynced@%d count=%d\n",
            connectedAt, resyncedAt, clientSimGetMapResyncCount(h.cs));
    if (resyncedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("resync never installed within %d pumps", RESYNC_MAX);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during the resync exchange");
    }
    loopbackHarnessStop(&h);
    return 0;
}
