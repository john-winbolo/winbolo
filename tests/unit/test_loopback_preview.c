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
#include "server_sim.h"
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX  2000
#define PREVIEW_MAX  4000   /* reassembly + retransmit convergence under loss */
#define PROBE_BYTES  3000   /* preview-sized blob, spans several bulk segments */

static const char *kPreviewDir  = "wb_preview_test_tmp";
static const char *kPreviewRel  = "preview_probe.map";
static const char *kPreviewFull = "wb_preview_test_tmp/preview_probe.map";

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

    /* Fresh temp dir + map file the server will source the preview from. */
    SDL_RemovePath(kPreviewFull);
    SDL_RemovePath(kPreviewDir);
    if (!SDL_CreateDirectory(kPreviewDir)) {
        UT_FAIL("could not create temp preview dir (%s)", SDL_GetError());
    }
    if (!SDL_SaveFile(kPreviewFull, probe, sizeof(probe))) {
        SDL_RemovePath(kPreviewDir);
        UT_FAIL("could not write temp preview map (%s)", SDL_GetError());
    }

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Preview", /*lobbyMode*/ true,
                                       /*impairSpec*/ "loss=10", /*seed*/ 0x9E1Du),
                  "harness start (preview) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(kPreviewFull);
        SDL_RemovePath(kPreviewDir);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    /* Point the server's map root at the temp dir between pumps. */
    threadsWaitForMutex();
    serverSimInstallMapDirList(h.sim, NULL, 0, kPreviewDir);
    threadsReleaseMutex();

    /* Ask for the preview; the server reads the file and streams it on
     * CHANNEL_BULK. */
    clientSimNetSendLobbyMapPreviewRequest(h.cs, kPreviewRel);

    settledAt = loopbackHarnessPumpUntil(&h, PREVIEW_MAX, pred_preview_settled, NULL);

    fprintf(stderr, "  loopback preview (loss=10): connected@%d settled@%d "
                    "ready=%d error=%d len=%u\n",
            connectedAt, settledAt,
            (int)clientSimGetLobbyMapPreviewReady(h.cs),
            (int)clientSimGetLobbyMapPreviewError(h.cs),
            (unsigned)clientSimGetLobbyMapPreviewLen(h.cs));

    if (settledAt < 0) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(kPreviewFull);
        SDL_RemovePath(kPreviewDir);
        UT_FAIL("preview never completed within %d pumps", PREVIEW_MAX);
    }
    if (clientSimGetLobbyMapPreviewError(h.cs)) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(kPreviewFull);
        SDL_RemovePath(kPreviewDir);
        UT_FAIL("server reported preview error");
    }
    if (!clientSimGetLobbyMapPreviewReady(h.cs)) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(kPreviewFull);
        SDL_RemovePath(kPreviewDir);
        UT_FAIL("preview did not become ready");
    }

    /* Byte-identical reassembly under loss — the reliability win. */
    {
        uint32_t len = clientSimGetLobbyMapPreviewLen(h.cs);
        const uint8_t *bytes = clientSimGetLobbyMapPreviewBytes(h.cs);
        if (len != PROBE_BYTES) {
            loopbackHarnessStop(&h);
            SDL_RemovePath(kPreviewFull);
            SDL_RemovePath(kPreviewDir);
            UT_FAIL("preview length %u, want %d", (unsigned)len, PROBE_BYTES);
        }
        if (bytes == NULL || memcmp(bytes, probe, PROBE_BYTES) != 0) {
            loopbackHarnessStop(&h);
            SDL_RemovePath(kPreviewFull);
            SDL_RemovePath(kPreviewDir);
            UT_FAIL("preview bytes did not match the source map under loss");
        }
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        SDL_RemovePath(kPreviewFull);
        SDL_RemovePath(kPreviewDir);
        UT_FAIL("client dropped during the preview exchange");
    }

    loopbackHarnessStop(&h);
    SDL_RemovePath(kPreviewFull);
    SDL_RemovePath(kPreviewDir);
    return 0;
}
