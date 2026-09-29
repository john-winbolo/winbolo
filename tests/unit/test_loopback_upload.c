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
 * retransmit, so the timeout cases below drop one chosen reply and check
 * recovery explicitly rather than requiring success under random loss.
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
#include "transport_udp_server_internal.h"

#define CONNECT_MAX  2000
#define UPLOAD_MAX   4000   /* handshake + bulk transfer convergence */
#define EMAP_LEN     E_MAP_LEN   /* compressed length of E_MAP (matches harness) */

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

/* Watches an upload for the status that means the server took a BEGIN and
 * the bytes are going over the bulk channel. */
static bool pred_upload_settled_note_bulk(LoopbackHarness *h, void *user) {
    uint8_t st = clientSimGetLobbyMapUploadStatus(h->cs);
    if (st == 2) *(bool *)user = true;
    return st == 3 || st == 4;
}

/* A map picked from the Workshop directory is offered as "Workshop/<name>"
 * before it is uploaded. The server and the client share one scratch
 * Workshop directory here, so the server holds the same file: the upload
 * finishes through USE_LOCAL, the bulk channel carries nothing, and the
 * server's map is the one it read from that directory. */
int run_workshop_use_local(void) {
    LoopbackHarness h;
    char     ws[FILENAME_MAX];
    char     mapPath[FILENAME_MAX];
    uint32_t ackedBefore = 0;
    uint32_t ackedAfter  = 0;
    bool     sawBulk     = false;
    bool     saved;
    int      settledAt;
    char     mapName[MAP_STR_SIZE];

    UT_ASSERT(utScratchPath(ws, sizeof(ws), "workshop"));
    UT_ASSERT(SDL_CreateDirectory(ws));
    SDL_snprintf(mapPath, sizeof(mapPath), "%s/a.map", ws);

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Uploader", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL,
                                       /*seed*/ 0xC0FFEEu),
                  "harness start (workshop use-local) failed");
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    /* The server's own map written out as a.map, so the file is a whole map
       both sides read the same way. */
    threadsWaitForMutex();
    serverSimSetOpenHost(h.sim, true);
    serverSimSetWorkshopMapDir(h.sim, ws);
    saved = serverSimSaveMap(h.sim, mapPath);
    threadsReleaseMutex();
    if (!saved) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not write %s", mapPath);
    }
    clientSimSetWorkshopMapDir(h.cs, ws);

    transportUdpClientChannelTestStats(&h.cs->transport, CHANNEL_BULK, NULL,
                                       &ackedBefore, NULL);
    if (!clientSimNetSendLobbyMapUpload(h.cs, mapPath)) {
        loopbackHarnessStop(&h);
        UT_FAIL("upload kick rejected for %s", mapPath);
    }
    settledAt = loopbackHarnessPumpUntil(&h, UPLOAD_MAX,
                                         pred_upload_settled_note_bulk,
                                         &sawBulk);
    transportUdpClientChannelTestStats(&h.cs->transport, CHANNEL_BULK, NULL,
                                       &ackedAfter, NULL);
    threadsWaitForMutex();
    SDL_strlcpy(mapName, serverSimGetMapName(h.sim), sizeof(mapName));
    threadsReleaseMutex();

    fprintf(stderr, "  workshop use-local: settled@%d status=%d reject=%d "
                    "bulk=%d acked %u->%u map='%s' path='%s'\n",
            settledAt, (int)clientSimGetLobbyMapUploadStatus(h.cs),
            (int)clientSimGetLobbyMapUploadRejectCode(h.cs), (int)sawBulk,
            (unsigned)ackedBefore, (unsigned)ackedAfter, mapName,
            clientSimGetLobbyMapUploadFinalPath(h.cs));

    if (settledAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("upload never settled within %d pumps", UPLOAD_MAX);
    }
    if (clientSimGetLobbyMapUploadStatus(h.cs) != 3 ||
        clientSimGetLobbyMapUploadRejectCode(h.cs) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("upload did not complete (status=%d reject=%d)",
                (int)clientSimGetLobbyMapUploadStatus(h.cs),
                (int)clientSimGetLobbyMapUploadRejectCode(h.cs));
    }
    if (sawBulk || ackedAfter != ackedBefore) {
        loopbackHarnessStop(&h);
        UT_FAIL("the map went over the bulk channel (status 2 seen=%d, "
                "acked %u->%u) rather than through USE_LOCAL",
                (int)sawBulk, (unsigned)ackedBefore, (unsigned)ackedAfter);
    }
    if (strcmp(clientSimGetLobbyMapUploadFinalPath(h.cs), "Workshop/a.map") != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server answered for '%s', not Workshop/a.map",
                clientSimGetLobbyMapUploadFinalPath(h.cs));
    }
    if (strcmp(mapName, "a") != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server's map is '%s', not a", mapName);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Drop a specific handshake reply while every liveness packet still flows.
 * Advance only the watchdog clock, rather than sleeping through its timeout. */
static int upload_timeout_recovery(bool lose_done, bool other_player, bool partial) {
    LoopbackHarness h;
    BYTE emap[6000] = E_MAP;
    int slot;
    ClientSim *retry_client;
    int i;
    UT_ASSERT(loopbackHarnessStart(&h, "Timeout", true, NULL, 0xC0FFEEu));
    UT_ASSERT(loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) > 0);
    if (other_player) {
        UT_ASSERT(loopbackHarnessAddClient(&h, "Retry"));
        for (i = 0; i < CONNECT_MAX &&
            clientSimGetConnectState(h.cs2) != CLIENT_CONNECT_CONNECTED; i++) {
            loopbackHarnessPump(&h);
        }
        UT_ASSERT(clientSimGetConnectState(h.cs2) == CLIENT_CONNECT_CONNECTED);
    }
    serverSimSetOpenHost(h.sim, true);
    slot = clientSimGetMyPlayerNum(h.cs);
    transportUdpClientTestDropUploadReply(&h.cs->transport,
        lose_done ? PACKET_LOBBY_MAP_UPLOAD_DONE : PACKET_LOBBY_MAP_UPLOAD_ACK);
    UT_ASSERT(transportUdpClientStartLobbyMapUploadFromBytes(&h.cs->transport,
        emap, EMAP_LEN, "timeout.map"));
    for (i = 0; i < 400; i++) loopbackHarnessPump(&h);
    UT_ASSERT(clientSimGetLobbyMapUploadStatus(h.cs) == (lose_done ? 2 : 1));
    if (!lose_done) {
        UT_ASSERT(udpServer.clientUploadActive[slot]);
        /* Regular pings keep the player connected, not their upload alive. */
        UT_ASSERT(udpServer.clients[slot].connected);
        udpServerExpireUploads(SDL_GetTicks());
        UT_ASSERT(udpServer.clientUploadActive[slot]);
    }
    if (partial) {
        /* Model an ACK arriving but the bulk stream stalling before delivery.
         * The pump below stages real stream segments into the send window,
         * but nothing transmits them (frames are only built by the tick), so
         * the watchdog then abandons them via channelResetSend. */
        h.cs->lobbyMapUploadStatus = 2;
    }
    transportUdpClientTestUploadTimeout(&h.cs->transport);
    UT_ASSERT(clientSimGetLobbyMapUploadStatus(h.cs) == 4);
    UT_ASSERT(clientSimGetLobbyMapUploadRejectCode(h.cs) != 0);
    if (partial) {
        /* After channelResetSend the sender's ackedSeq == nextSeq, i.e. the
         * boundary the retry's BEGIN will carry; the staged-but-unsent
         * segments sit below it, above the server's expectedSeq. */
        uint32_t client_acked_seq;
        transportUdpClientChannelTestStats(&h.cs->transport, CHANNEL_BULK, NULL, &client_acked_seq, NULL);
        UT_ASSERT_MSG(client_acked_seq > udpServer.channelMux[slot].ch[CHANNEL_BULK].expectedSeq,
            "the abandoned stream must leave a sequence gap for the retry to repair");
    }
    udpServerExpireUploads(SDL_GetTicks() + SERVER_UPLOAD_IDLE_TIMEOUT_MS + 1);
    UT_ASSERT(!udpServer.clientUploadActive[slot]);
    UT_ASSERT(!lobbyAnyOtherUploadActive(udpServer.clientUploadActive, -1));
    transportUdpClientTestDropUploadReply(&h.cs->transport, 0);
    /* An approval arriving after the watchdog must not put the UI back into
     * its in-flight state when there is no longer a buffer to send. */
    {
        uint8_t late_ack[PACKET_HEADER_SIZE + 1];
        packHeader(late_ack, PACKET_LOBBY_MAP_UPLOAD_ACK, 0);
        late_ack[PACKET_HEADER_SIZE] = 0;
        srvSendTo(late_ack, sizeof(late_ack), &udpServer.clients[slot].addr);
    }
    for (i = 0; i < 100; i++) loopbackHarnessPump(&h);
    UT_ASSERT(clientSimGetLobbyMapUploadStatus(h.cs) == 4);
    retry_client = other_player ? h.cs2 : h.cs;
    UT_ASSERT(transportUdpClientStartLobbyMapUploadFromBytes(&retry_client->transport,
        emap, EMAP_LEN, "retry.map"));
    for (i = 0; i < UPLOAD_MAX && clientSimGetLobbyMapUploadStatus(retry_client) < 3; i++) {
        loopbackHarnessPump(&h);
    }
    UT_ASSERT_MSG(clientSimGetLobbyMapUploadStatus(retry_client) == 3,
        "retry failed: status=%d reject=%d", clientSimGetLobbyMapUploadStatus(retry_client),
        clientSimGetLobbyMapUploadRejectCode(retry_client));
    loopbackHarnessStop(&h);
    return 0;
}

int run_upload_lost_ack_retry(void) {
    return upload_timeout_recovery(false, false, false);
}

int run_upload_timeout_releases_other_player(void) {
    return upload_timeout_recovery(false, true, false);
}

int run_upload_lost_done_retry(void) {
    return upload_timeout_recovery(true, false, false);
}

int run_upload_partial_timeout_retry(void) {
    return upload_timeout_recovery(false, false, true);
}
