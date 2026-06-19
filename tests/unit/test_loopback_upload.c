/*
 * Loopback integration test for client->server map upload over the bulk
 * channel (CHANNEL_BULK).
 *
 * A lobby server is stood up over the real loopback transport. The client
 * announces an upload (BEGIN), the server approves it (ACK), and the map bytes
 * then stream over CHANNEL_BULK behind a bulk-transfer stream header; the
 * server reassembles them, decodes the map, and replies MAP_UPLOAD_DONE. This
 * exercises the full live wiring end-to-end: BulkSender -> channel ->
 * BulkReceiver -> reload -> DONE. A corrupted transfer would fail to decode
 * and report a non-zero status, so reaching done (status 0) proves the bytes
 * arrived intact.
 *
 * Run clean (no impairment): the byte-identical-under-loss guarantee for the
 * data itself is proven exhaustively off-socket by the bulk_transfer test
 * (burst loss + reorder, kind-agnostic plus the UPLOAD-kind case). The upload
 * handshake (BEGIN/ACK/DONE) rides plain unreliable datagrams with no
 * retransmit — out of scope for this migration — so driving the whole upload
 * under loss would flake on a dropped handshake packet, not on the bulk path.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_sim_internal.h"   /* ClientSim::transport */
#include "transport_udp.h"         /* transportUdpClientStartLobbyMapUploadFromBytes */
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetOpenHost */
#include "everard_map.h"           /* E_MAP */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX  2000
#define UPLOAD_MAX   4000   /* handshake + bulk transfer convergence */
#define EMAP_LEN     5097   /* compressed length of E_MAP (matches harness) */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_upload_settled(LoopbackHarness *h, void *user) {
    (void)user;
    uint8_t st = clientSimGetLobbyMapUploadStatus(h->cs);
    return st == 3 || st == 4;   /* done or rejected */
}

int run_loopback_map_upload(void) {
    LoopbackHarness h;
    int connectedAt, settledAt;
    BYTE emap[6000] = E_MAP;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Uploader", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL,
                                       /*seed*/ 0xC0FFEEu),
                  "harness start (upload) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    /* Grant upload edit rights regardless of host-slot assignment. */
    threadsWaitForMutex();
    serverSimSetOpenHost(h.sim, true);
    threadsReleaseMutex();

    /* Kick the upload: BEGIN handshake, then the bytes ride CHANNEL_BULK. */
    if (!transportUdpClientStartLobbyMapUploadFromBytes(&h.cs->transport,
                                                        emap, EMAP_LEN,
                                                        "loopprobe.map")) {
        loopbackHarnessStop(&h);
        UT_FAIL("upload kick rejected");
    }

    settledAt = loopbackHarnessPumpUntil(&h, UPLOAD_MAX, pred_upload_settled, NULL);

    fprintf(stderr, "  loopback upload (clean): connected@%d settled@%d "
                    "status=%d reject=%d\n",
            connectedAt, settledAt,
            (int)clientSimGetLobbyMapUploadStatus(h.cs),
            (int)clientSimGetLobbyMapUploadRejectCode(h.cs));

    if (settledAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("upload never settled within %d pumps", UPLOAD_MAX);
    }
    if (clientSimGetLobbyMapUploadStatus(h.cs) != 3) {
        loopbackHarnessStop(&h);
        UT_FAIL("upload did not complete (status=%d reject=%d)",
                (int)clientSimGetLobbyMapUploadStatus(h.cs),
                (int)clientSimGetLobbyMapUploadRejectCode(h.cs));
    }
    if (clientSimGetLobbyMapUploadRejectCode(h.cs) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server rejected the reassembled upload (code=%d)",
                (int)clientSimGetLobbyMapUploadRejectCode(h.cs));
    }
    /* No post-upload connect-state assertion: under ALLOW the server installs
     * the uploaded map and broadcasts a map change, which legitimately moves
     * the client through a map-install transition. The success criterion is
     * that the reassembled upload was accepted (status 3, reject 0). */

    loopbackHarnessStop(&h);
    return 0;
}
