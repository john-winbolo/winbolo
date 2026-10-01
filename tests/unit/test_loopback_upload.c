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
#include "upload_policy.h"           /* UPLOAD_KIND_MAP */

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

/* How many pumps the listing may take once the client is CONNECTED again:
 * the request out, the server's answer, and the client reading it, with room
 * to spare. */
#define LIST_AFTER_REJOIN_PUMPS 8

/* The Server Maps tab's refresh as lobby_chooser.cpp drives it: one
 * enumerate each time the list counter moves, which asks for the root
 * listing unless the cache already holds it or the same request is in
 * flight. force is the enumerate the tab makes when it opens. */
static void chooser_frame(ClientSim *cs, uint32_t *serverMapSeq, bool force) {
    const char *want = "";
    uint32_t    seq  = clientSimGetLobbyMapListSeq(cs);
    bool        ready, inFlight, haveMatch, sameReq;

    if (!force && seq == *serverMapSeq) return;
    *serverMapSeq = seq;
    ready     = clientSimGetLobbyMapListReady(cs);
    inFlight  = clientSimGetLobbyMapListInFlight(cs);
    haveMatch = ready &&
                (SDL_strcmp(clientSimGetLobbyMapListPath(cs), want) == 0);
    sameReq   = (SDL_strcmp(clientSimGetLobbyMapListReqPath(cs), want) == 0);
    if (!haveMatch && !(inFlight && sameReq)) {
        clientSimNetSendLobbyMapListRequest(cs, want);
    }
}

static bool pred_root_list_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetLobbyMapListReady(h->cs) &&
           SDL_strcmp(clientSimGetLobbyMapListPath(h->cs), "") == 0;
}

static bool pred_cooldown_clear(LoopbackHarness *h, void *user) {
    int  slot = *(const int *)user;
    bool clear;
    (void)h;
    threadsWaitForMutex();
    clear = (udpServer.clientReqCooldownTicks[slot] == 0);
    threadsReleaseMutex();
    return clear;
}

/* A map upload the server loads changes the map, so the client re-joins and
 * DONE lands while it is not CONNECTED: the listing the chooser asks for on
 * DONE is never sent. Once the re-join completes the chooser must ask again
 * and be answered, with the server no longer holding the cooldown the
 * upload's BEGIN started. */
int run_loopback_map_upload_list_after_rejoin(void) {
    LoopbackHarness h;
    BYTE     emap[6000] = E_MAP;
    uint32_t serverMapSeq = 0;
    int      slot;
    int      i;
    int      settledAt = -1;
    int      leftAt    = -1;
    int      backAt    = -1;
    int      listedAt  = -1;
    uint8_t  status    = 0;

    UT_ASSERT(loopbackHarnessStart(&h, "ListRejoin", /*lobbyMode*/ true,
                                   NULL, 0xC0FFEEu));
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    slot = clientSimGetMyPlayerNum(h.cs);
    threadsWaitForMutex();
    serverSimSetOpenHost(h.sim, true);
    /* ALLOW loads the upload, which is the map change the re-join follows. */
    udpServer.uploadPolicy = UPLOAD_POLICY_ALLOW;
    threadsReleaseMutex();

    /* The tab opens and fills its cache, then the cooldown that request
     * started runs out so the upload's BEGIN is taken. */
    chooser_frame(h.cs, &serverMapSeq, /*force*/ true);
    if (loopbackHarnessPumpUntil(&h, UPLOAD_MAX, pred_root_list_ready,
                                 NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the first listing never arrived");
    }
    if (loopbackHarnessPumpUntil(&h, UPLOAD_MAX, pred_cooldown_clear,
                                 &slot) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the listing's cooldown never ran out");
    }

    if (!transportUdpClientStartLobbyMapUploadFromBytes(&h.cs->transport,
                                                        emap, EMAP_LEN,
                                                        "rejoin.map")) {
        loopbackHarnessStop(&h);
        UT_FAIL("upload kick rejected");
    }

    for (i = 0; i < UPLOAD_MAX; i++) {
        ClientConnectState st;
        loopbackHarnessPump(&h);
        chooser_frame(h.cs, &serverMapSeq, /*force*/ false);
        st = clientSimGetConnectState(h.cs);
        if (settledAt < 0) {
            status = clientSimGetLobbyMapUploadStatus(h.cs);
            if (status == 3 || status == 4) settledAt = i;
        }
        if (leftAt < 0 && st != CLIENT_CONNECT_CONNECTED) leftAt = i;
        if (leftAt >= 0 && backAt < 0 && st == CLIENT_CONNECT_CONNECTED) {
            backAt = i;
        }
        if (backAt >= 0 && listedAt < 0 &&
            pred_root_list_ready(&h, NULL)) {
            listedAt = i;
            break;
        }
        if (backAt >= 0 && i - backAt > LIST_AFTER_REJOIN_PUMPS) break;
    }

    fprintf(stderr, "  map upload list after re-join: settled@%d status=%d "
                    "left@%d back@%d listed@%d (cooldown %d ticks)\n",
            settledAt, (int)status, leftAt, backAt, listedAt,
            LOBBY_REQ_COOLDOWN_TICKS);
    loopbackHarnessStop(&h);

    UT_ASSERT_MSG(settledAt >= 0 && status == 3,
                  "the upload did not complete (settled@%d status=%d)",
                  settledAt, (int)status);
    UT_ASSERT_MSG(leftAt >= 0, "the loaded upload did not re-join the client");
    UT_ASSERT_MSG(backAt >= 0, "the client never reached CONNECTED again");
    UT_ASSERT_MSG(listedAt >= 0 && listedAt - backAt <= LIST_AFTER_REJOIN_PUMPS,
                  "no map listing within %d pumps of the re-join "
                  "(back@%d listed@%d)",
                  LIST_AFTER_REJOIN_PUMPS, backAt, listedAt);
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
    /* No reply is not a refusal: the chooser tells the player to try again
       rather than that the server turned the map down. */
    UT_ASSERT_MSG(clientSimGetLobbyMapUploadRejectCode(h.cs) == LOBBY_REJECT_TIMEOUT,
                  "a timed-out upload reported reject code %d, wanted the "
                  "timeout code %d",
                  (int)clientSimGetLobbyMapUploadRejectCode(h.cs),
                  LOBBY_REJECT_TIMEOUT);
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

/* A BEGIN without the bulk-sequence trailer is malformed. Every client that
 * can reach this server sends the trailer, so the server refuses a short
 * BEGIN the way it refuses any other short one, and opens no upload. */
int run_upload_begin_without_trailer_refused(void) {
    LoopbackHarness h;
    uint8_t pkt[PACKET_HEADER_SIZE + 1 + 4 + 1 + 255 + 4];
    size_t bodyLen;
    struct sockaddr_in from;
    bool armed;
    int slot;

    UT_ASSERT(loopbackHarnessStart(&h, "Short", true, NULL, 0xC0FFEEu));
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    threadsWaitForMutex();
    serverSimSetOpenHost(h.sim, true);
    threadsReleaseMutex();
    slot = clientSimGetMyPlayerNum(h.cs);

    packHeader(pkt, PACKET_LOBBY_MAP_UPLOAD_BEGIN, 0);
    bodyLen = transportUdpClientBuildUploadBeginBody(
        pkt + PACKET_HEADER_SIZE, sizeof(pkt) - PACKET_HEADER_SIZE,
        UPLOAD_KIND_MAP, EMAP_LEN, "short.map", 0);
    UT_ASSERT(bodyLen > 4);
    /* The same BEGIN with its last four bytes, the trailer, cut off, handed
       to the server as if from the client's address. */
    threadsWaitForMutex();
    from = udpServer.clients[slot].addr;
    serverProcessPacket(h.sim, pkt, (int)(PACKET_HEADER_SIZE + bodyLen - 4),
                        &from);
    armed = udpServer.clientUploadActive[slot];
    threadsReleaseMutex();

    if (armed) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server opened an upload for a BEGIN with no trailer");
    }
    loopbackHarnessStop(&h);
    return 0;
}
