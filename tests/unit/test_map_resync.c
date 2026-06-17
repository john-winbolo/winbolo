/* test_map_resync.c — server-side map-desync resync queue logic.
 *
 * When a client detects its terrain has diverged (a dropped
 * EVENT_MAP_CHANGE), it asks the server to re-send the live map. The server
 * recompresses the map, re-inits the slot's download, and CUTS the slot's
 * reliable map-event queue (ackedSeq = nextSeq) so the freshly compressed
 * blob and the queued terrain changes can't both carry — and double-apply —
 * the same change. While the resync transfers, the snapshot builder holds
 * (packs zero) that slot's map events; they flow once the download completes.
 *
 * These are pure tests against the real ClientEventQueue / eventQueueHasSpace
 * (transport_udp_internal.h). The server's cut, enqueue, and send-gate bodies
 * are static inside transport_udp_server.c and not linkable here, so the three
 * one-line operations are mirrored exactly below — same approach as
 * test_control_event_queue.c. Each mirror cites the real site it copies.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "input_packet.h"           /* GameEvent, EVENT_MAP_CHANGE */
#include "transport_udp_internal.h" /* ClientEventQueue, eventQueueHasSpace */
#include "test_harness.h"

/* Mirror of the join-time map-event-queue init (transport_udp_server.c
 * "udpServer.mapEventQueues[slot].nextSeq = 1; ... ackedSeq = 1;"). */
static void mapQueueInit(ClientEventQueue *mq) {
    memset(mq, 0, sizeof(*mq));
    mq->nextSeq = 1;
    mq->ackedSeq = 1;
}

/* Mirror of the map-event enqueue in transportUdpServerDrainEvents: guarded by
 * eventQueueHasSpace, stamps the event at nextSeq, advances nextSeq. The
 * terrain byte stands in for a distinct EVENT_MAP_CHANGE payload so the test
 * can prove which changes were delivered. Returns false on a full-queue drop. */
static bool mapQueueEnqueue(ClientEventQueue *mq, uint8_t terrain) {
    uint32_t idx;
    if (!eventQueueHasSpace(mq)) return false;
    idx = mq->nextSeq % RELIABLE_EVENT_BUFFER_SIZE;
    mq->buffer[idx].event.type = EVENT_MAP_CHANGE;
    mq->buffer[idx].event.data[0] = terrain;
    mq->buffer[idx].seq = mq->nextSeq;
    mq->nextSeq++;
    return true;
}

/* Mirror of the resync cut in the PACKET_MAP_RESYNC_REQUEST handler:
 * the recompressed blob carries every change up to nextSeq-1, so the queue is
 * emptied by setting ackedSeq = nextSeq. */
static void mapQueueResyncCut(ClientEventQueue *mq) {
    mq->ackedSeq = mq->nextSeq;
}

/* Mirror of the snapshot map-event pack loop + the resync send gate
 * (serverSendSnapshot). While resyncInProgress the slot packs zero map events
 * (the held events sit at seq >= cut); otherwise it packs ackedSeq..nextSeq.
 * Fills outTerrain with the delivered payloads and returns the count. */
static int mapQueueSnapshotPack(const ClientEventQueue *mq, bool resyncInProgress,
                                uint8_t *outTerrain, int maxOut) {
    int count = 0;
    uint32_t seq;
    if (resyncInProgress) return 0; /* send gate */
    for (seq = mq->ackedSeq; seq < mq->nextSeq; seq++) {
        uint32_t idx = seq % RELIABLE_EVENT_BUFFER_SIZE;
        if (mq->buffer[idx].seq != seq) break; /* wrapped */
        if (count < maxOut) outTerrain[count] = mq->buffer[idx].event.data[0];
        count++;
        if (count >= 255) break;
    }
    return count;
}

/* (a) the cut empties the queue (ackedSeq == nextSeq); (b) changes baked into
 * the blob are not re-sent, and a change at seq >= cut is delivered exactly
 * once after the gate lifts. */
int run_map_resync_cut_and_deliver_once(void) {
    ClientEventQueue mq;
    uint8_t got[8];
    int n;

    mapQueueInit(&mq);

    /* Three terrain changes that occurred before the resync — all baked into
     * the freshly compressed blob. */
    UT_ASSERT(mapQueueEnqueue(&mq, 11));
    UT_ASSERT(mapQueueEnqueue(&mq, 22));
    UT_ASSERT(mapQueueEnqueue(&mq, 33));
    UT_ASSERT(mq.nextSeq == 4);
    UT_ASSERT(mq.ackedSeq == 1);

    /* The cut. */
    mapQueueResyncCut(&mq);
    UT_ASSERT_MSG(mq.ackedSeq == mq.nextSeq,
                  "after cut queue must be empty: ackedSeq=%u nextSeq=%u",
                  (unsigned)mq.ackedSeq, (unsigned)mq.nextSeq);

    /* The baked-in changes (seq 1-3) must not be re-sent. */
    n = mapQueueSnapshotPack(&mq, false, got, 8);
    UT_ASSERT_MSG(n == 0, "baked-in changes must not re-send, got %d", n);

    /* A change during the transfer lands at seq == cut (4). */
    UT_ASSERT(mapQueueEnqueue(&mq, 44));

    /* Held while the resync is in flight. */
    n = mapQueueSnapshotPack(&mq, true, got, 8);
    UT_ASSERT_MSG(n == 0, "send gate must hold map events during resync, got %d", n);

    /* Delivered exactly once after completion — and only the post-cut change,
     * not the baked-in ones. */
    n = mapQueueSnapshotPack(&mq, false, got, 8);
    UT_ASSERT_MSG(n == 1, "post-cut change must deliver exactly once, got %d", n);
    UT_ASSERT_MSG(got[0] == 44, "delivered wrong change: %u", (unsigned)got[0]);

    return 0;
}

/* (c) a duplicate request while a resync is already in progress must NOT
 * re-cut: re-cutting would advance ackedSeq past changes enqueued since the
 * first cut and drop them. */
int run_map_resync_duplicate_request_no_recut(void) {
    ClientEventQueue mq;
    bool resyncInProgress;
    uint8_t got[8];
    int n;

    mapQueueInit(&mq);

    UT_ASSERT(mapQueueEnqueue(&mq, 11)); /* baked in */
    UT_ASSERT(mapQueueEnqueue(&mq, 22)); /* baked in */

    /* First request: cut + mark in progress. */
    mapQueueResyncCut(&mq);
    resyncInProgress = true;
    UT_ASSERT(mq.ackedSeq == 3);

    /* A change arrives during the transfer (seq 3). */
    UT_ASSERT(mapQueueEnqueue(&mq, 33));
    UT_ASSERT(mq.nextSeq == 4);

    /* Duplicate request while resyncInProgress: the handler short-circuits
     * with no re-cut. ackedSeq must be unchanged. */
    if (!resyncInProgress) {
        mapQueueResyncCut(&mq); /* must NOT run */
    }
    UT_ASSERT_MSG(mq.ackedSeq == 3,
                  "duplicate request must not re-cut: ackedSeq=%u",
                  (unsigned)mq.ackedSeq);

    /* The in-transfer change survived: delivered once after completion. A
     * re-cut would have set ackedSeq=4 and lost it. */
    n = mapQueueSnapshotPack(&mq, false, got, 8);
    UT_ASSERT_MSG(n == 1, "no event loss across duplicate request, got %d", n);
    UT_ASSERT_MSG(got[0] == 33, "lost the in-transfer change, got %u",
                  (unsigned)got[0]);

    return 0;
}

/* (d) while resyncInProgress the snapshot builder yields zero map events for
 * the slot, however many sit queued; it resumes after the gate lifts. */
int run_map_resync_send_gate_holds(void) {
    ClientEventQueue mq;
    uint8_t got[8];
    int n;

    mapQueueInit(&mq);
    mapQueueResyncCut(&mq); /* empty queue at cut */

    /* Several changes during the transfer. */
    UT_ASSERT(mapQueueEnqueue(&mq, 1));
    UT_ASSERT(mapQueueEnqueue(&mq, 2));
    UT_ASSERT(mapQueueEnqueue(&mq, 3));

    /* Gate engaged: zero packed regardless of queue depth. */
    n = mapQueueSnapshotPack(&mq, true, got, 8);
    UT_ASSERT_MSG(n == 0, "send gate must yield zero while in progress, got %d", n);

    /* Gate lifted: all three flow in seq order. */
    n = mapQueueSnapshotPack(&mq, false, got, 8);
    UT_ASSERT_MSG(n == 3, "all held events flow after gate lifts, got %d", n);
    UT_ASSERT(got[0] == 1 && got[1] == 2 && got[2] == 3);

    return 0;
}

/* Mirror of the client-side resync acceptance decision in clientBulkOnBegin
 * (transport_udp_client.c) for a BULK_KIND_RESYNC stream header: the resync
 * blob is accepted only when a resync is outstanding and the header's
 * generation matches the active request; a stale/superseded gen is dropped (the
 * gen gate). A join download arrives under BULK_KIND_DOWNLOAD, not as a resync,
 * modelled here as chunkGen 0. The dispatch is static in the transport, so the
 * decision is modelled here (no sockets). */
static bool resyncChunkAccepted(uint32_t chunkGen, bool resyncActive,
                                uint32_t activeGen) {
    if (chunkGen == 0) return false;   /* join download, not a resync */
    if (!resyncActive) return false;   /* no resync outstanding */
    return chunkGen == activeGen;      /* reject a stale/superseded generation */
}

int run_map_resync_stale_gen_rejected(void) {
    /* Outstanding resync uses generation 7. */
    UT_ASSERT_MSG(resyncChunkAccepted(7, true, 7),
                  "matching gen must be accepted");
    UT_ASSERT_MSG(!resyncChunkAccepted(6, true, 7),
                  "stale (superseded) gen must be rejected");
    UT_ASSERT_MSG(!resyncChunkAccepted(8, true, 7),
                  "wrong (newer) gen must be rejected");
    UT_ASSERT_MSG(!resyncChunkAccepted(7, false, 7),
                  "no resync active -> chunk rejected");
    UT_ASSERT_MSG(!resyncChunkAccepted(0, true, 7),
                  "gen 0 is the join path, not a resync chunk");
    return 0;
}
