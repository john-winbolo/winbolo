/*
 * Loopback integration test for server-map preview over the reliable bulk
 * channel (CHANNEL_BULK).
 *
 * A lobby server is stood up over the real loopback transport under loss. A
 * small .map file is written into a temp directory and the server's map root
 * is pointed at it, so a PACKET_LOBBY_MAP_PREVIEW_REQ resolves to a real file.
 * The reply now rides CHANNEL_BULK behind a bulk-transfer stream header rather
 * than the retired BEGIN/CHUNK datagrams, so the client must reassemble it
 * byte-identical even when datagrams drop — the reliability gain over the old
 * fire-once-per-chunk path, which could silently lose a chunk and stall.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_sim_internal.h"  /* LOBBY_MAP_PREVIEW_TRIES */
#include "server_sim.h"
#include "threads.h"
#include "transport_udp.h"        /* PACKET_LOBBY_MAP_PREVIEW_REQ,
                                     udpClientHandleLobbyMapPreviewErr */
#include "transport_udp_internal.h" /* packHeader, PACKET_HEADER_SIZE */
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX  2000
#define PREVIEW_MAX  4000   /* reassembly + retransmit convergence under loss */
#define PROBE_BYTES  3000   /* preview-sized blob, spans several bulk segments */

static const char *kPreviewRel  = "preview_probe.map";

/* The server's map root and the probe map inside it, in this test's own
 * scratch directory (utScratchPath) so tests running at once never remove
 * each other's file. Filled by previewPathsInit. */
static char s_previewDir[1024];
static char s_previewFull[1024];

static bool previewPathsInit(void) {
    if (!utScratchPath(s_previewDir, sizeof(s_previewDir), "maps")) {
        return false;
    }
    SDL_snprintf(s_previewFull, sizeof(s_previewFull), "%s/%s", s_previewDir,
                 kPreviewRel);
    return true;
}

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_preview_settled(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetLobbyMapPreviewReady(h->cs) ||
           clientSimGetLobbyMapPreviewError(h->cs);
}

int run_loopback_map_preview(void) {
    LoopbackHarness h;
    int connectedAt, settledAt;
    uint8_t probe[PROBE_BYTES];
    int i;

    /* Deterministic probe bytes the client must reassemble exactly. */
    for (i = 0; i < PROBE_BYTES; i++) {
        probe[i] = (uint8_t)((i * 31 + 7) & 0xFF);
    }

    UT_ASSERT_MSG(previewPathsInit(), "could not make the scratch directory");

    /* Fresh temp dir + map file the server will source the preview from. */
    SDL_RemovePath(s_previewFull);
    SDL_RemovePath(s_previewDir);
    if (!SDL_CreateDirectory(s_previewDir)) {
        UT_FAIL("could not create temp preview dir (%s)", SDL_GetError());
    }
    if (!SDL_SaveFile(s_previewFull, probe, sizeof(probe))) {
        SDL_RemovePath(s_previewDir);
        UT_FAIL("could not write temp preview map (%s)", SDL_GetError());
    }

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Preview", /*lobbyMode*/ true,
                                       /*impairSpec*/ "loss=5,burst=2",
                                       /*seed*/ 0xC0FFEEu),
                  "harness start (preview) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(s_previewFull);
        SDL_RemovePath(s_previewDir);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    /* Point the server's map root at the temp dir between pumps. */
    threadsWaitForMutex();
    serverSimInstallMapDirList(h.sim, NULL, 0, s_previewDir);
    threadsReleaseMutex();

    /* Ask for the preview; the server reads the file and streams it on
     * CHANNEL_BULK. */
    clientSimNetSendLobbyMapPreviewRequest(h.cs, kPreviewRel);

    settledAt = loopbackHarnessPumpUntil(&h, PREVIEW_MAX, pred_preview_settled, NULL);

    fprintf(stderr, "  loopback preview (loss=5,burst=2): connected@%d settled@%d "
                    "ready=%d error=%d len=%u\n",
            connectedAt, settledAt,
            (int)clientSimGetLobbyMapPreviewReady(h.cs),
            (int)clientSimGetLobbyMapPreviewError(h.cs),
            (unsigned)clientSimGetLobbyMapPreviewLen(h.cs));

    if (settledAt < 0) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(s_previewFull);
        SDL_RemovePath(s_previewDir);
        UT_FAIL("preview never completed within %d pumps", PREVIEW_MAX);
    }
    if (clientSimGetLobbyMapPreviewError(h.cs)) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(s_previewFull);
        SDL_RemovePath(s_previewDir);
        UT_FAIL("server reported preview error");
    }
    if (!clientSimGetLobbyMapPreviewReady(h.cs)) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(s_previewFull);
        SDL_RemovePath(s_previewDir);
        UT_FAIL("preview did not become ready");
    }

    /* Byte-identical reassembly under loss — the reliability win. */
    {
        uint32_t len = clientSimGetLobbyMapPreviewLen(h.cs);
        const uint8_t *bytes = clientSimGetLobbyMapPreviewBytes(h.cs);
        if (len != PROBE_BYTES) {
            loopbackHarnessStop(&h);
            SDL_RemovePath(s_previewFull);
            SDL_RemovePath(s_previewDir);
            UT_FAIL("preview length %u, want %d", (unsigned)len, PROBE_BYTES);
        }
        if (bytes == NULL || memcmp(bytes, probe, PROBE_BYTES) != 0) {
            loopbackHarnessStop(&h);
            SDL_RemovePath(s_previewFull);
            SDL_RemovePath(s_previewDir);
            UT_FAIL("preview bytes did not match the source map under loss");
        }
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(s_previewFull);
        SDL_RemovePath(s_previewDir);
        UT_FAIL("client dropped during the preview exchange");
    }

    loopbackHarnessStop(&h);
    SDL_RemovePath(s_previewFull);
    SDL_RemovePath(s_previewDir);
    return 0;
}

/* ---- A lost preview request.
 *
 * The request is a bare datagram. These run on a clean link and drop the
 * client's PACKET_LOBBY_MAP_PREVIEW_REQ on purpose, so the resend is tested
 * every run rather than only when the seeded loss above happens to land on
 * it. */

/* Write the probe map, start a clean lobby harness, connect, and point the
 * server's map root at the probe's directory. Returns false (and has cleaned
 * up) on failure. */
static bool previewLostSetUp(LoopbackHarness *h) {
    uint8_t probe[PROBE_BYTES];
    int i;

    if (!previewPathsInit()) return false;
    for (i = 0; i < PROBE_BYTES; i++) {
        probe[i] = (uint8_t)((i * 31 + 7) & 0xFF);
    }
    SDL_RemovePath(s_previewFull);
    SDL_RemovePath(s_previewDir);
    if (!SDL_CreateDirectory(s_previewDir)) return false;
    if (!SDL_SaveFile(s_previewFull, probe, sizeof(probe))) {
        SDL_RemovePath(s_previewDir);
        return false;
    }
    if (!loopbackHarnessStart(h, "Preview", /*lobbyMode*/ true,
                              /*impairSpec*/ NULL, /*seed*/ 0xC0FFEEu)) {
        SDL_RemovePath(s_previewFull);
        SDL_RemovePath(s_previewDir);
        return false;
    }
    if (loopbackHarnessPumpUntil(h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(h);
        SDL_RemovePath(s_previewFull);
        SDL_RemovePath(s_previewDir);
        return false;
    }
    threadsWaitForMutex();
    serverSimInstallMapDirList(h->sim, NULL, 0, s_previewDir);
    threadsReleaseMutex();
    return true;
}

static void previewLostTearDown(LoopbackHarness *h) {
    loopbackHarnessStop(h);
    SDL_RemovePath(s_previewFull);
    SDL_RemovePath(s_previewDir);
}

/* The first request is lost; the resend brings the whole preview. */
int run_loopback_map_preview_request_lost(void) {
    LoopbackHarness h;
    int settledAt, left;
    bool ready, error;
    uint32_t len;

    UT_ASSERT_MSG(previewLostSetUp(&h), "harness start (preview) failed");

    loopbackHarnessDropNextFromClient(&h, h.cs, PACKET_LOBBY_MAP_PREVIEW_REQ, 1);
    clientSimNetSendLobbyMapPreviewRequest(h.cs, kPreviewRel);
    settledAt = loopbackHarnessPumpUntil(&h, PREVIEW_MAX, pred_preview_settled,
                                         NULL);
    left  = loopbackHarnessDropNextFromClientLeft(&h, h.cs);
    ready = clientSimGetLobbyMapPreviewReady(h.cs);
    error = clientSimGetLobbyMapPreviewError(h.cs);
    len   = clientSimGetLobbyMapPreviewLen(h.cs);
    previewLostTearDown(&h);

    UT_ASSERT_MSG(left == 0, "the first request was not dropped (%d left)",
                  left);
    UT_ASSERT_MSG(settledAt >= 0,
                  "preview never completed within %d pumps after a lost "
                  "request", PREVIEW_MAX);
    UT_ASSERT_MSG(ready && !error, "preview ready=%d error=%d, want 1 0",
                  (int)ready, (int)error);
    UT_ASSERT_MSG(len == PROBE_BYTES, "preview length %u, want %d",
                  (unsigned)len, PROBE_BYTES);
    return 0;
}

/* Every send is lost: the client gives up with an error instead of waiting
 * forever. */
int run_loopback_map_preview_request_gives_up(void) {
    LoopbackHarness h;
    int settledAt, left;
    bool ready, error, inFlight;

    UT_ASSERT_MSG(previewLostSetUp(&h), "harness start (preview) failed");

    loopbackHarnessDropNextFromClient(&h, h.cs, PACKET_LOBBY_MAP_PREVIEW_REQ,
                                      LOBBY_MAP_PREVIEW_TRIES);
    clientSimNetSendLobbyMapPreviewRequest(h.cs, kPreviewRel);
    settledAt = loopbackHarnessPumpUntil(&h, PREVIEW_MAX, pred_preview_settled,
                                         NULL);
    left     = loopbackHarnessDropNextFromClientLeft(&h, h.cs);
    ready    = clientSimGetLobbyMapPreviewReady(h.cs);
    error    = clientSimGetLobbyMapPreviewError(h.cs);
    inFlight = clientSimGetLobbyMapPreviewInFlight(h.cs);
    previewLostTearDown(&h);

    UT_ASSERT_MSG(left == 0, "%d of %d requests were not sent", left,
                  LOBBY_MAP_PREVIEW_TRIES);
    UT_ASSERT_MSG(settledAt >= 0,
                  "client still waiting after %d pumps with every request lost",
                  PREVIEW_MAX);
    UT_ASSERT_MSG(error && !ready && !inFlight,
                  "preview error=%d ready=%d inFlight=%d, want 1 0 0",
                  (int)error, (int)ready, (int)inFlight);
    return 0;
}

/* A busy reply (PACKET_LOBBY_MAP_PREVIEW_ERR code 3) that arrives after the
 * preview has finished, but before the chooser clears it, leaves the finished
 * preview alone. The server answers a resend that crossed the first stream
 * with code 3 while that stream is still being sent, so the reply can land
 * after the last segment. */
int run_loopback_map_preview_late_busy_kept(void) {
    LoopbackHarness h;
    int settledAt;
    bool readyBefore, ready, error, inFlight;
    uint32_t lenBefore, len;
    uint8_t before[PROBE_BYTES];
    bool bytesSame = false;
    uint8_t err[PACKET_HEADER_SIZE + 1 + 256 + 1];
    size_t n = strlen(kPreviewRel);
    int pos;

    UT_ASSERT_MSG(previewLostSetUp(&h), "harness start (preview) failed");

    clientSimNetSendLobbyMapPreviewRequest(h.cs, kPreviewRel);
    settledAt = loopbackHarnessPumpUntil(&h, PREVIEW_MAX, pred_preview_settled,
                                         NULL);
    readyBefore = clientSimGetLobbyMapPreviewReady(h.cs);
    lenBefore   = clientSimGetLobbyMapPreviewLen(h.cs);
    if (readyBefore && lenBefore == PROBE_BYTES) {
        memcpy(before, clientSimGetLobbyMapPreviewBytes(h.cs), PROBE_BYTES);
    }

    /* [header 8] [pathLen 1] [path N] [code 1], code 3 = busy. */
    packHeader(err, PACKET_LOBBY_MAP_PREVIEW_ERR, 0);
    pos = PACKET_HEADER_SIZE;
    err[pos++] = (uint8_t)n;
    memcpy(err + pos, kPreviewRel, n);
    pos += (int)n;
    err[pos++] = 3;
    udpClientHandleLobbyMapPreviewErr(h.cs, err, pos);

    ready    = clientSimGetLobbyMapPreviewReady(h.cs);
    error    = clientSimGetLobbyMapPreviewError(h.cs);
    inFlight = clientSimGetLobbyMapPreviewInFlight(h.cs);
    len      = clientSimGetLobbyMapPreviewLen(h.cs);
    if (ready && len == PROBE_BYTES && lenBefore == PROBE_BYTES) {
        const uint8_t *bytes = clientSimGetLobbyMapPreviewBytes(h.cs);
        bytesSame = bytes != NULL && memcmp(bytes, before, PROBE_BYTES) == 0;
    }
    previewLostTearDown(&h);

    UT_ASSERT_MSG(settledAt >= 0 && readyBefore && lenBefore == PROBE_BYTES,
                  "preview did not complete on a clean link (settled@%d "
                  "ready=%d len=%u)", settledAt, (int)readyBefore,
                  (unsigned)lenBefore);
    UT_ASSERT_MSG(ready && !error && !inFlight,
                  "after a late busy reply ready=%d error=%d inFlight=%d, "
                  "want 1 0 0", (int)ready, (int)error, (int)inFlight);
    UT_ASSERT_MSG(len == PROBE_BYTES, "preview length %u after a late busy "
                  "reply, want %d", (unsigned)len, PROBE_BYTES);
    UT_ASSERT_MSG(bytesSame, "preview bytes changed after a late busy reply");
    return 0;
}
