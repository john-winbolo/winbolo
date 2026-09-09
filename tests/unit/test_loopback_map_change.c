/*
 * Mid-lobby map change and join-download recovery over the real loopback
 * transport.
 *
 * The join map streams as one blob on CHANNEL_BULK, reassembled by the
 * client's single BulkReceiver. Before the PACKET_MAP_DL_READY readiness
 * round-trip, the server began that stream unsolicited the moment the
 * download was armed, so the stream's head raced the JOIN_ACCEPT that sizes
 * the client's buffers. Losing that race wedged the lobby permanently:
 *
 *   - Accept lost/late: the stream header drained while the client was still
 *     JOINING, so the whole body was consumed with nowhere to put it. The
 *     channel had acked the bytes, the server considered the transfer done
 *     and never re-sent, and the client sat in DOWNLOADING_MAP forever —
 *     "Downloading map…" with the Ready button dead.
 *   - Duplicate accept mid-stream: the server re-sends the accept for every
 *     JOIN it hears from a connected address, and the accept handler re-armed
 *     unconditionally — resetting the BulkReceiver's framing mid-body, after
 *     which the rest of the stream parsed as a garbage header and was
 *     silently swallowed. Same wedge.
 *
 * Both windows widen with latency/loss, and a mid-lobby map change (which
 * forces a re-JOIN while a fresh stream is being staged) reopens them every
 * time. These tests drive exactly those shapes and require convergence:
 *
 *   1. Clean map change: join, change the lobby map, and the client must
 *      re-download and return to CONNECTED with the download complete.
 *   2. Map changes under loss+duplication, across a spread of seeds: each
 *      run joins and then changes the map twice; every run must reconverge.
 *   3. Join under loss+duplication, across a spread of seeds: the initial
 *      accept is frequently dropped or duplicated; every run must connect.
 *      Run against both a lobby server and a running one — a mid-game
 *      joiner walks the identical accept/download path.
 *
 * Impairment draws come from the seeded bolo_rand stream, but the server's
 * recv thread adds scheduling variance, so (per the harness contract) the
 * assertions are "converges within N pumps", never an exact packet trace.
 * The seed spread is what makes the old wedges near-certain to fire at least
 * once per test run on a build without the recovery machinery.
 */
#include <stdint.h>
#include <stdio.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "everard_map.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Join + one full map download under impairment. Matches the caps the other
 * loopback download tests use. */
#define MC_CONNECT_MAX   8000
/* One map-change cycle: notice the change (mapDownloadComplete drops), then
 * re-join + re-download back to complete. */
#define MC_NOTICE_MAX    2000
#define MC_RECOVER_MAX   8000

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Latches once the map change has discarded the installed map. u points at
 * the invalidate count sampled before the change.
 *
 * Replaces a predicate that waited to observe !clientSimIsMapDownloadComplete.
 * That is a transient — incomplete only until the new map lands — and
 * loopbackHarnessPumpUntil samples only between pumps, so it is missable in
 * principle. It has not been seen to fail here (0/25 measured), because a
 * player's full map re-download spans many pumps and a sample always falls
 * inside; the same shape one layer over, in the spectator lobby test, has a
 * window about one pump wide and failed ~24-36% of runs. Latch a monotonic
 * counter instead so this one does not depend on that margin either. */
static bool pred_dl_invalidated(LoopbackHarness *h, void *user) {
    return clientSimGetMapInvalidateCount(h->cs) > *(const uint32_t *)user;
}

static bool pred_reconverged(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsMapDownloadComplete(h->cs);
}

/* Change the lobby map (an in-memory reload of the same Everard blob — the
 * wire flow is identical for any map) and require the joined client to notice
 * it and reconverge to CONNECTED + download-complete. Prints its outcome and
 * returns 0 on success, nonzero on failure — the caller stops the harness and
 * fails the test, so no server threads outlive a failed run. */
static int mc_change_and_reconverge(LoopbackHarness *h, const char *tag) {
    BYTE emap[6000] = E_MAP;
    int noticedAt, recoveredAt;
    uint32_t invalidatesBefore = clientSimGetMapInvalidateCount(h->cs);

    if (!serverSimReloadCompressedInMemory(h->sim, emap, 5097,
                                           "Everard Island")) {
        fprintf(stderr, "  map change (%s): in-memory reload failed\n", tag);
        return 1;
    }
    noticedAt = loopbackHarnessPumpUntil(h, MC_NOTICE_MAX,
                                         pred_dl_invalidated, &invalidatesBefore);
    if (noticedAt < 0) {
        fprintf(stderr, "  map change (%s): never noticed within %d pumps\n",
                tag, MC_NOTICE_MAX);
        return 1;
    }
    recoveredAt = loopbackHarnessPumpUntil(h, MC_RECOVER_MAX,
                                           pred_reconverged, NULL);
    fprintf(stderr, "  map change (%s): noticed@%d reconverged@%d state=%d "
            "complete=%d\n",
            tag, noticedAt, recoveredAt,
            (int)clientSimGetConnectState(h->cs),
            (int)clientSimIsMapDownloadComplete(h->cs));
    if (recoveredAt < 0) {
        return 1;
    }
    return 0;
}

/* Case 1: clean-path mid-lobby map change re-downloads and reconverges. */
int run_loopback_map_change_clean(void) {
    LoopbackHarness h;
    int connectedAt;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "MapChg", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 0xC1EA9u),
                  "harness start (map change clean) failed");
    connectedAt = loopbackHarnessPumpUntil(&h, MC_CONNECT_MAX,
                                           pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("initial join never completed within %d pumps", MC_CONNECT_MAX);
    }
    if (mc_change_and_reconverge(&h, "clean") != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("clean map-change cycle failed (see lines above)");
    }
    loopbackHarnessStop(&h);
    return 0;
}

/* Case 2: map changes under loss + duplication across a seed spread. Each
 * seed runs a join and two map-change cycles; a build without the READY
 * round-trip / duplicate-accept guard / download watchdog wedges the moment a
 * seed drops the re-sent accept (stream head swallowed while JOINING) or
 * lands a duplicated accept mid-stream (receiver framing wiped). */
int run_loopback_map_change_loss(void) {
    static const uint64_t kSeeds[] = {
        0x101u, 0x202u, 0x303u, 0x404u, 0x505u,
        0x606u, 0x707u, 0x808u, 0x909u, 0xA0Au,
    };
    for (size_t s = 0; s < sizeof(kSeeds) / sizeof(kSeeds[0]); s++) {
        LoopbackHarness h;
        int connectedAt;
        char tag[48];

        UT_ASSERT_MSG(loopbackHarnessStart(&h, "MapChgL", /*lobbyMode*/ true,
                                           /*impairSpec*/ "loss=15,dup=15",
                                           kSeeds[s]),
                      "harness start (map change loss, seed %zu) failed", s);
        connectedAt = loopbackHarnessPumpUntil(&h, MC_CONNECT_MAX,
                                               pred_connected, NULL);
        if (connectedAt < 0) {
            ClientConnectState st = clientSimGetConnectState(h.cs);
            loopbackHarnessStop(&h);
            UT_FAIL("seed %zu: initial join never completed within %d pumps "
                    "(final state=%d)", s, MC_CONNECT_MAX, (int)st);
        }
        for (int cycle = 0; cycle < 2; cycle++) {
            snprintf(tag, sizeof(tag), "loss seed %zu cycle %d", s, cycle);
            if (mc_change_and_reconverge(&h, tag) != 0) {
                loopbackHarnessStop(&h);
                UT_FAIL("map-change cycle failed (%s — wedged re-download?)",
                        tag);
            }
        }
        loopbackHarnessStop(&h);
    }
    return 0;
}

/* Cases 3+4: initial joins under loss + duplication across a seed spread,
 * against a lobby server and against a running one. A dropped first accept
 * previously meant the unsolicited stream drained while the client was still
 * JOINING and the join wedged in DOWNLOADING_MAP; with the readiness
 * round-trip the stream only ever starts after the client is armed, and the
 * watchdog re-asks if anything is lost mid-way. The running-server sweep also
 * exercises the standalone bulk carrier that feeds a not-yet-complete joiner
 * while snapshots to it are still held. */
static int join_accept_loss_sweep(bool lobbyMode, const char *label) {
    static const uint64_t kSeeds[] = {
        0x1111u, 0x2222u, 0x3333u, 0x4444u, 0x5555u,
        0x6666u, 0x7777u, 0x8888u, 0x9999u, 0xAAAAu,
    };
    for (size_t s = 0; s < sizeof(kSeeds) / sizeof(kSeeds[0]); s++) {
        LoopbackHarness h;
        int connectedAt;

        UT_ASSERT_MSG(loopbackHarnessStart(&h, "AccLoss", lobbyMode,
                                           /*impairSpec*/ "loss=15,dup=15",
                                           kSeeds[s]),
                      "harness start (%s accept loss, seed %zu) failed",
                      label, s);
        connectedAt = loopbackHarnessPumpUntil(&h, MC_CONNECT_MAX,
                                               pred_connected, NULL);
        fprintf(stderr,
                "  %s join under loss=15,dup=15 (seed %zu): connected@%d\n",
                label, s, connectedAt);
        if (connectedAt < 0) {
            ClientConnectState st = clientSimGetConnectState(h.cs);
            loopbackHarnessStop(&h);
            UT_FAIL("%s seed %zu: never reached CONNECTED within %d pumps "
                    "(final state=%d — wedged join download?)",
                    label, s, MC_CONNECT_MAX, (int)st);
        }
        loopbackHarnessStop(&h);
    }
    return 0;
}

int run_loopback_join_accept_loss(void) {
    return join_accept_loss_sweep(/*lobbyMode*/ true, "lobby");
}

int run_loopback_join_accept_loss_midgame(void) {
    return join_accept_loss_sweep(/*lobbyMode*/ false, "mid-game");
}
