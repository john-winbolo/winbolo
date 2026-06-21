/* test_resync_finalize.c — the client map-resync finalize must not let a failed
 * install masquerade as success.
 *
 * On a checksum mismatch the client downloads a fresh map blob and installs it.
 * If that install fails (a truncated/corrupt blob), the generation must NOT
 * advance and the resync must NOT count as done — otherwise the client adopts a
 * stale map, the next full-sync mismatches again, and it loops resync requests
 * until MAP_RESYNC_MAX_ATTEMPTS self-kicks it. A successful install advances the
 * generation exactly as before.
 *
 * These drive the real udpClientFinalizeResync through the test-only
 * transportUdpClientTestFinalizeResync hook against a live loopback client, and
 * read the generation/count back through transportUdpClientTestMapState.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_connect_state.h"
#include "client_sim_internal.h"   /* ClientSim::transport, installCompressedMap */
#include "transport_udp.h"         /* test-only resync hooks */
#include "server_sim.h"            /* serverSimGetCompressedMap */
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX 2000

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Connect a loopback client to an already-running server. Returns the client
 * Transport handle (into h->cs) or fails the test. */
static Transport *connect_client(LoopbackHarness *h, const char *name) {
    UT_ASSERT_MSG(loopbackHarnessStart(h, name, /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x5151u),
                  "harness start failed");
    if (loopbackHarnessPumpUntil(h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    return &h->cs->transport;
}

/* A corrupt/truncated resync blob must leave installedMapGen and mapResyncCount
 * untouched and re-arm the resync (resyncActive cleared) so a later mismatch
 * can retry. */
int run_resync_finalize_corrupt_keeps_gen(void) {
    LoopbackHarness h;
    Transport *ct;
    static BYTE blob[131072];
    int n;
    uint32_t gen0 = 0, count0 = 0, gen1 = 0, count1 = 0;

    ct = connect_client(&h, "ResyncBad");

    transportUdpClientTestMapState(ct, &gen0, &count0);

    /* A real blob truncated to a short prefix: too few bytes to decode to a full
     * map, so installCompressedMap fails the decode — the realistic shape of a
     * resync corrupted by loss. */
    n = serverSimGetCompressedMap(h.sim, blob);
    UT_ASSERT_MSG(n > 16, "server compressed map too small to truncate (%d)", n);

    UT_ASSERT_MSG(transportUdpClientTestFinalizeResync(ct, blob, 12),
                  "finalize hook rejected the truncated blob");

    transportUdpClientTestMapState(ct, &gen1, &count1);
    UT_ASSERT_MSG(gen1 == gen0,
                  "failed resync install advanced installedMapGen %u -> %u",
                  (unsigned)gen0, (unsigned)gen1);
    UT_ASSERT_MSG(count1 == count0,
                  "failed resync install counted as success %u -> %u",
                  (unsigned)count0, (unsigned)count1);

    /* Re-armed: resyncActive was cleared on failure, so a fresh resync can be
     * begun (the request the next checksum mismatch would send). */
    UT_ASSERT_MSG(transportUdpClientTestBeginResync(ct),
                  "resync was not re-armed after a failed install");

    loopbackHarnessStop(&h);
    return 0;
}

/* The positive companion: a valid blob through the same hook DOES advance the
 * generation and count — proving it is the corruption, not the hook, that gates
 * the advance above. */
int run_resync_finalize_valid_advances_gen(void) {
    LoopbackHarness h;
    Transport *ct;
    static BYTE blob[131072];
    int n;
    uint32_t gen0 = 0, count0 = 0, gen1 = 0, count1 = 0;

    ct = connect_client(&h, "ResyncOk");

    transportUdpClientTestMapState(ct, &gen0, &count0);

    n = serverSimGetCompressedMap(h.sim, blob);
    UT_ASSERT_MSG(n > 0, "server compressed map empty (%d)", n);

    UT_ASSERT_MSG(transportUdpClientTestFinalizeResync(ct, blob, n),
                  "finalize hook rejected a valid blob");

    transportUdpClientTestMapState(ct, &gen1, &count1);
    UT_ASSERT_MSG(gen1 > gen0,
                  "valid resync install did not advance installedMapGen %u -> %u",
                  (unsigned)gen0, (unsigned)gen1);
    UT_ASSERT_MSG(count1 == count0 + 1,
                  "valid resync install not counted once %u -> %u",
                  (unsigned)count0, (unsigned)count1);

    loopbackHarnessStop(&h);
    return 0;
}

/* The cheap complement to the integration cases: installCompressedMap itself
 * reports failure on an undecodable buffer (the predicate Fix A's failure
 * branch keys off). */
int run_install_compressed_map_rejects_garbage(void) {
    LoopbackHarness h;
    static const BYTE garbage[24] = { 0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03 };

    (void)connect_client(&h, "InstallBad");

    UT_ASSERT_MSG(!installCompressedMap(h.cs, garbage, (int)sizeof(garbage),
                                        NULL, /*initViewport=*/false),
                  "installCompressedMap accepted a garbage buffer");

    loopbackHarnessStop(&h);
    return 0;
}
