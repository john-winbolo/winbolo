/*
 * Reliable control-event queue invariants and lifecycle scenarios.
 *
 * Each test here corresponds to a real bug we shipped during the
 * reliable-control-events rollout.  They aren't theoretical — every
 * scenario reproduces a specific repro the user hit, locked in as a
 * regression test so the same shape can't slip back in.
 *
 *   queue_init_is_valid                 — fresh queue invariant
 *   queue_enqueue_advances_nextSeq      — basic enqueue
 *   queue_ack_advance_within_range      — happy-path ack
 *   queue_stale_ack_above_nextSeq       — stale ack from old space rejected
 *   queue_wipe_resets_both_seqs         — game-start wipe
 *   queue_enqueue_into_empty_after_wipe — first event lands at seq=1
 *   queue_hasspace_at_capacity          — overflow predicate
 *
 * The "stale ack rejection" test in particular captures the bug that
 * stuck the host on countdown=1: a client's ACK from the pre-wipe
 * sequence space arrives after the server has reset its queue, and
 * without rejection logic the server's ackedSeq jumps past nextSeq
 * (silently stranding every subsequent event including
 * CTRL_GAME_PHASE_RUNNING).
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "transport_udp_internal.h"  /* ClientControlEventQueue, CONTROL_EVENT_QUEUE_SIZE */
#include "test_harness.h"

/* Mirror the rejection logic from transport_udp_server.c's
 * serverHandleControlAck / serverHandleInput so the test exercises the
 * exact contract.  Returns true if the ack was applied. */
static bool applyAckRejectingStale(ClientControlEventQueue *q, uint32_t ack) {
    if (ack > q->nextSeq) {
        /* Stale future ack from pre-wipe sequence space — must be ignored. */
        return false;
    }
    if (ack > q->ackedSeq) {
        q->ackedSeq = ack;
    }
    return true;
}

/* Mirror the enqueue site's idle-clock reset behaviour: when a new
 * event lands in an otherwise-empty queue, the unacked-control timer
 * baseline restarts at "now".  Captured here as a pure function so
 * the test doesn't need a real udpServer. */
static void enqueueOne(ClientControlEventQueue *q,
                       uint32_t *outLastAckProgressTick,
                       uint32_t currentTick) {
    if (q->ackedSeq == q->nextSeq && outLastAckProgressTick != NULL) {
        *outLastAckProgressTick = currentTick;
    }
    uint32_t seq = q->nextSeq;
    q->buffer[seq % CONTROL_EVENT_QUEUE_SIZE].seq = seq;
    memset(&q->buffer[seq % CONTROL_EVENT_QUEUE_SIZE].event, 0,
           sizeof(q->buffer[seq % CONTROL_EVENT_QUEUE_SIZE].event));
    q->nextSeq++;
    controlEventQueueAssertValid(q, "test-enqueue");
}

static void queueInit(ClientControlEventQueue *q) {
    memset(q, 0, sizeof(*q));
    q->nextSeq = 1;
    q->ackedSeq = 1;
}

static void queueWipe(ClientControlEventQueue *q) {
    q->nextSeq = 1;
    q->ackedSeq = 1;
    memset(q->buffer, 0, sizeof(q->buffer));
}

/* ─────────────────────────────────────────────────────────────────── */

int run_queue_init_is_valid(void) {
    ClientControlEventQueue q;
    queueInit(&q);
    controlEventQueueAssertValid(&q, "test");
    UT_ASSERT_MSG(q.nextSeq == 1, "fresh queue: nextSeq must be 1, got %u",
                  (unsigned)q.nextSeq);
    UT_ASSERT_MSG(q.ackedSeq == 1, "fresh queue: ackedSeq must be 1, got %u",
                  (unsigned)q.ackedSeq);
    UT_ASSERT(q.nextSeq == q.ackedSeq); /* empty */
    UT_ASSERT(controlEventQueueHasSpace(&q));
    return 0;
}

int run_queue_enqueue_advances_nextSeq(void) {
    ClientControlEventQueue q;
    queueInit(&q);

    enqueueOne(&q, NULL, 0);
    UT_ASSERT_MSG(q.nextSeq == 2, "after one enqueue: nextSeq=2, got %u",
                  (unsigned)q.nextSeq);
    UT_ASSERT(q.ackedSeq == 1); /* still 1 — nothing acked */

    enqueueOne(&q, NULL, 0);
    enqueueOne(&q, NULL, 0);
    UT_ASSERT(q.nextSeq == 4);
    UT_ASSERT(q.ackedSeq == 1);
    UT_ASSERT(q.nextSeq - q.ackedSeq == 3); /* depth */
    return 0;
}

int run_queue_ack_advance_within_range(void) {
    ClientControlEventQueue q;
    queueInit(&q);
    enqueueOne(&q, NULL, 0);
    enqueueOne(&q, NULL, 0);
    enqueueOne(&q, NULL, 0); /* nextSeq=4, ackedSeq=1 */

    UT_ASSERT(applyAckRejectingStale(&q, 3));
    UT_ASSERT(q.ackedSeq == 3);
    UT_ASSERT(q.nextSeq == 4);
    controlEventQueueAssertValid(&q, "test");

    UT_ASSERT(applyAckRejectingStale(&q, 4));
    UT_ASSERT(q.ackedSeq == 4);
    UT_ASSERT(q.ackedSeq == q.nextSeq); /* queue drained */
    return 0;
}

int run_queue_stale_ack_above_nextSeq(void) {
    /* The bug that stuck the host on countdown=1:
     *   1. queue has ackedSeq=23, nextSeq=25 (ALLIANCE_LEAVE x2 in flight)
     *   2. game-start wipe → (ackedSeq=1, nextSeq=1)
     *   3. enqueue CTRL_GAME_PHASE_RUNNING → (1, 2)
     *   4. client's pre-wipe ACK(25) arrives
     *   5. without rejection: ackedSeq=25, nextSeq=2 — broken
     */
    ClientControlEventQueue q;
    queueInit(&q);
    /* Build up pre-wipe state */
    for (int i = 0; i < 24; i++) enqueueOne(&q, NULL, 0); /* nextSeq=25 */
    UT_ASSERT(applyAckRejectingStale(&q, 23));
    UT_ASSERT(q.ackedSeq == 23);

    /* Game-start wipe */
    queueWipe(&q);
    UT_ASSERT(q.nextSeq == 1 && q.ackedSeq == 1);

    /* RUNNING enqueued */
    enqueueOne(&q, NULL, 0);
    UT_ASSERT(q.nextSeq == 2 && q.ackedSeq == 1);

    /* Stale ACK from pre-wipe sequence space arrives */
    bool accepted = applyAckRejectingStale(&q, 25);
    UT_ASSERT_MSG(!accepted,
                  "stale ack=25 must be rejected (nextSeq=%u)",
                  (unsigned)q.nextSeq);
    UT_ASSERT_MSG(q.ackedSeq == 1,
                  "ackedSeq must remain 1, got %u (queue corrupted)",
                  (unsigned)q.ackedSeq);
    UT_ASSERT(q.ackedSeq <= q.nextSeq); /* invariant holds */
    controlEventQueueAssertValid(&q, "test");
    return 0;
}

int run_queue_wipe_resets_both_seqs(void) {
    ClientControlEventQueue q;
    queueInit(&q);
    for (int i = 0; i < 30; i++) enqueueOne(&q, NULL, 0);
    UT_ASSERT(q.nextSeq == 31);

    queueWipe(&q);
    UT_ASSERT(q.nextSeq == 1);
    UT_ASSERT(q.ackedSeq == 1);
    controlEventQueueAssertValid(&q, "test");
    return 0;
}

int run_queue_enqueue_into_empty_after_wipe(void) {
    /* The idle-clock reset bug: when the queue is empty (ackedSeq ==
     * nextSeq) for a long time and a new event arrives, the
     * unacked-control timer baseline must restart at "now" — otherwise
     * the next checkTimeouts compares the new event's ack-progress
     * against a stale-old baseline and false-disconnects the client. */
    ClientControlEventQueue q;
    queueInit(&q);

    uint32_t lastAckProgressTick = 0;     /* set long ago */
    uint32_t now = 100000;                /* simulate 2000 ticks later */

    UT_ASSERT(q.ackedSeq == q.nextSeq);   /* empty */
    enqueueOne(&q, &lastAckProgressTick, now);

    UT_ASSERT_MSG(lastAckProgressTick == now,
                  "enqueue-into-empty must reset timer baseline to now (%u), got %u",
                  (unsigned)now, (unsigned)lastAckProgressTick);

    /* Second enqueue does NOT reset — queue is no longer empty */
    uint32_t laterNow = 100050;
    enqueueOne(&q, &lastAckProgressTick, laterNow);
    UT_ASSERT_MSG(lastAckProgressTick == now,
                  "non-empty enqueue must not move the baseline; expected %u got %u",
                  (unsigned)now, (unsigned)lastAckProgressTick);
    return 0;
}

int run_control_ack_resend_due(void) {
    /* The dropped-lobby-ack hole: the standalone PACKET_CONTROL_ACK used
     * to fire only on a value advance, so one lost ack left the server
     * retransmitting forever until the 10s unacked-control timeout
     * disconnected the client.  The fix re-sends the current (unchanged)
     * ack while TICKs keep re-arming the pending flag, coalesced to ~3
     * ticks, and stops the moment the server catches up. */
    const uint32_t localTick = 1000;

    /* Caught-up server: no TICK pending → never send (the steady-state
     * invariant — no per-tick ack storm). */
    UT_ASSERT_MSG(!controlAckResendDue(0, localTick, 5, 5),
                  "no pending TICK must not send");
    UT_ASSERT_MSG(!controlAckResendDue(0, localTick, 9, 5),
                  "no pending TICK must not send even with un-acked advance");

    /* Armed but not yet overdue (<3 ticks since arming) and value
     * unchanged: hold for the coalesce window. */
    UT_ASSERT_MSG(!controlAckResendDue(localTick - 2, localTick, 5, 5),
                  "armed 2 ticks ago, unchanged value: must coalesce, not send");

    /* Armed and overdue (>=3 ticks) with unchanged value: this is the
     * dropped-ack recovery — re-send the same value. */
    UT_ASSERT_MSG(controlAckResendDue(localTick - 3, localTick, 5, 5),
                  "armed 3 ticks ago, unchanged value: must re-send");
    UT_ASSERT_MSG(controlAckResendDue(localTick - 10, localTick, 5, 5),
                  "long overdue, unchanged value: must re-send");

    /* Eager path: a single TICK delivered 2+ new events (ack jumped past
     * lastSent+1) — send immediately even before the coalesce window. */
    UT_ASSERT_MSG(controlAckResendDue(localTick, localTick, 7, 5),
                  "ack jumped to lastSent+2: eager send");
    UT_ASSERT_MSG(!controlAckResendDue(localTick, localTick, 6, 5),
                  "single new event (lastSent+1): not eager, not overdue → hold");
    return 0;
}

int run_control_seq_reset_detect(void) {
    /* The running-phase desync: the server wipes the control queue to seq 1
     * at game start and republishes RUNNING at seq 1, but the client's ack
     * is still high (22) from the lobby phase.  RESET-DETECT must snap the
     * ack down to the new space once — regardless of inLobby timing — and
     * never re-fire on retransmits (which would loop). */

    /* First sight of the running reset, not yet adopted: snap. */
    UT_ASSERT_MSG(controlSeqResetDetected(1, 1, 22, false, true),
                  "stale-high ack + RUNNING at baseSeq=1 + !adopted must snap");

    /* Already adopted (ack advanced to 2, retransmit of seq-1 RUNNING):
     * dedup territory, must NOT re-snap. */
    UT_ASSERT_MSG(!controlSeqResetDetected(1, 1, 2, true, true),
                  "adopted: retransmitted seq-1 RUNNING must not re-snap");

    /* Adopted, first event still RUNNING, baseSeq below ack — the exact
     * retransmit shape that the loop-free flag must block. */
    UT_ASSERT_MSG(!controlSeqResetDetected(3, 1, 5, true, true),
                  "adopted: any baseSeq<ack RUNNING tail must not re-snap");

    /* Not yet adopted but the tail does not lead with RUNNING — an ordinary
     * lobby/countdown retransmit, not a sequence-space reset. */
    UT_ASSERT_MSG(!controlSeqResetDetected(2, 1, 22, false, false),
                  "non-RUNNING first event must not be treated as a reset");

    /* Empty tail: nothing to detect. */
    UT_ASSERT_MSG(!controlSeqResetDetected(0, 1, 22, false, true),
                  "empty control tail must not snap");

    /* Forward progress (baseSeq == ack, normal new event) is not a reset. */
    UT_ASSERT_MSG(!controlSeqResetDetected(1, 22, 22, false, true),
                  "baseSeq>=ack is forward progress, not a reset");
    return 0;
}

int run_queue_hasspace_at_capacity(void) {
    ClientControlEventQueue q;
    queueInit(&q);
    UT_ASSERT(controlEventQueueHasSpace(&q));

    /* Fill to one short of capacity */
    for (int i = 0; i < CONTROL_EVENT_QUEUE_SIZE - 1; i++) {
        UT_ASSERT(controlEventQueueHasSpace(&q));
        enqueueOne(&q, NULL, 0);
    }
    UT_ASSERT(controlEventQueueHasSpace(&q));   /* exactly one slot left */
    enqueueOne(&q, NULL, 0);                    /* fill it */
    UT_ASSERT_MSG(!controlEventQueueHasSpace(&q),
                  "queue must report no space at full depth (depth=%u, size=%d)",
                  (unsigned)(q.nextSeq - q.ackedSeq), CONTROL_EVENT_QUEUE_SIZE);

    /* Invariant still holds at the boundary */
    controlEventQueueAssertValid(&q, "test-cap");
    UT_ASSERT(q.nextSeq - q.ackedSeq == CONTROL_EVENT_QUEUE_SIZE);
    return 0;
}
