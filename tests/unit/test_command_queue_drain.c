/*
 * Outbound command-queue send-trigger contract.
 *
 * plans/client-send-messages.md Step 3 specifies "eager-then-coalesce"
 * send timing for PACKET_COMMAND_TICK:
 *
 *   - first submit (queue empty) drains immediately — eager-send timer
 *   - subsequent submits coalesce into "the next outgoing frame"
 *   - the 80 ms retransmit timer covers loss recovery, not initial send
 *
 * Three send triggers must cooperate or some submits sit forever:
 *
 *   1. submit-while-empty       (transportUdpClientSubmitCommand)
 *   2. ack-advance-while-pending(PACKET_COMMAND_ACK handler)
 *   3. retransmit-on-stalled-head (udpClientTick periodic)
 *
 * Trigger 2 was missing — a submit landing while a prior cmd is in
 * flight sat at lastSentMs=0, and when the prior cmd's ACK advanced
 * outHeadSeq onto it, the ACK handler returned without draining; the
 * retransmit timer's `lastSentMs != 0` gate then excluded it forever.
 * In-the-wild repro: lobbySendTeamPool ships seq=2, lobbySendAddBot
 * submits seq=3 in the same frame, server processes seq=2 but seq=3
 * never reaches the wire (mp-logging-95072.txt).
 *
 * Tests mirror the queue logic in pure form so the contract is
 * captured independently of the production struct layout.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "test_harness.h"

#define UT_CMD_QUEUE_CAP 64

typedef struct {
    uint32_t lastSentMs;  /* 0 = never drained */
    uint8_t  type;        /* opaque payload tag for assertions */
} UtOutCmdEntry;

typedef struct {
    UtOutCmdEntry q[UT_CMD_QUEUE_CAP];
    uint32_t outCmdNextSeq;  /* init 1, matches production */
    uint32_t outHeadSeq;
    uint32_t outTailSeq;
} UtCmdQ;

static void ut_init(UtCmdQ *c) {
    memset(c, 0, sizeof(*c));
    c->outCmdNextSeq = 1;
    c->outHeadSeq    = 1;
    c->outTailSeq    = 1;
}

/* Mirrors udpClientDrainCommandQueue's observable effect: every entry
 * in [outHeadSeq, outTailSeq) gets lastSentMs stamped to `now`. The
 * production codec-encode-fail skip is out of scope here. */
static void ut_drain(UtCmdQ *c, uint32_t now) {
    for (uint32_t seq = c->outHeadSeq; seq != c->outTailSeq; seq++) {
        c->q[seq % UT_CMD_QUEUE_CAP].lastSentMs = now;
    }
}

/* Mirrors transportUdpClientSubmitCommand: enqueue + eager drain iff
 * the queue was previously empty. */
static void ut_submit(UtCmdQ *c, uint32_t now, uint8_t type) {
    bool wasEmpty = (c->outHeadSeq == c->outTailSeq);
    uint32_t seq = c->outCmdNextSeq++;
    c->q[seq % UT_CMD_QUEUE_CAP].lastSentMs = 0;
    c->q[seq % UT_CMD_QUEUE_CAP].type       = type;
    c->outTailSeq = seq + 1;
    if (wasEmpty) ut_drain(c, now);
}

/* Mirrors the PACKET_COMMAND_ACK handler — POST-FIX. The fix added
 * the trailing drain when outHeadSeq advances onto a non-empty tail. */
static void ut_ack_up_to(UtCmdQ *c, uint32_t now, uint32_t highest) {
    if (highest >= c->outHeadSeq) {
        c->outHeadSeq = highest + 1;
        if (c->outHeadSeq > c->outTailSeq) {
            c->outHeadSeq = c->outTailSeq;
        }
        if (c->outHeadSeq != c->outTailSeq) ut_drain(c, now);
    }
}

/* Helper: return the lastSentMs for a given submit-order seq. */
static uint32_t ut_entry_sent_at(const UtCmdQ *c, uint32_t seq) {
    return c->q[seq % UT_CMD_QUEUE_CAP].lastSentMs;
}

/* ─────────────────────────────────────────────────────────────────── */

/* Trigger 1: first submit into an empty queue drains immediately. */
int run_command_queue_first_submit_drains(void) {
    UtCmdQ c;
    ut_init(&c);
    ut_submit(&c, 100, /*type*/ 7);
    UT_ASSERT_MSG(ut_entry_sent_at(&c, 1) == 100,
                  "submit-into-empty must drain (lastSentMs got %u)",
                  (unsigned)ut_entry_sent_at(&c, 1));
    UT_ASSERT(c.outHeadSeq == 1);
    UT_ASSERT(c.outTailSeq == 2);
    return 0;
}

/* Trigger 1 negative half: submit while a prior cmd is still in
 * flight must NOT drain (otherwise every submit would force a new
 * packet, breaking the coalesce intent). This is the *cause* of the
 * gap that needs Trigger 2 to fill. */
int run_command_queue_second_submit_does_not_drain(void) {
    UtCmdQ c;
    ut_init(&c);
    ut_submit(&c, 100, /*type*/ 7);   /* seq=1: drained */
    ut_submit(&c, 110, /*type*/ 8);   /* seq=2: queue non-empty, no drain */
    UT_ASSERT_MSG(ut_entry_sent_at(&c, 1) == 100,
                  "seq=1 lastSentMs must remain 100 (got %u)",
                  (unsigned)ut_entry_sent_at(&c, 1));
    UT_ASSERT_MSG(ut_entry_sent_at(&c, 2) == 0,
                  "seq=2 must stay un-sent until next outgoing frame"
                  " (lastSentMs got %u)",
                  (unsigned)ut_entry_sent_at(&c, 2));
    return 0;
}

/* Trigger 2 — the regression. With seq=1 drained and seq=2 pending,
 * the prior head's ACK must advance outHeadSeq AND drain the new head.
 * Pre-fix: outHeadSeq advanced but ut_drain never ran, leaving seq=2
 * at lastSentMs=0 forever (retransmit timer's lastSentMs!=0 gate
 * excludes it). Post-fix: seq=2 ships in the same handler. */
int run_command_queue_ack_drains_pending_tail(void) {
    UtCmdQ c;
    ut_init(&c);
    ut_submit(&c, 100, /*type*/ 7);
    ut_submit(&c, 110, /*type*/ 8);
    /* Server acks seq=1; client sees PACKET_COMMAND_ACK{highest=1}. */
    ut_ack_up_to(&c, 200, /*highest*/ 1);
    UT_ASSERT_MSG(c.outHeadSeq == 2,
                  "head must advance past acked seq (got outHeadSeq=%u)",
                  (unsigned)c.outHeadSeq);
    UT_ASSERT_MSG(ut_entry_sent_at(&c, 2) == 200,
                  "seq=2 must be drained when ack advances head onto it"
                  " (lastSentMs got %u — pre-fix value was 0)",
                  (unsigned)ut_entry_sent_at(&c, 2));
    return 0;
}

/* ACK that clears the queue entirely (head == tail) must not invoke
 * a drain on an empty queue. Mirrors the post-fix guard
 * `if (c->outHeadSeq != c->outTailSeq) udpClientDrainCommandQueue(c)`. */
int run_command_queue_ack_clearing_queue_skips_drain(void) {
    UtCmdQ c;
    ut_init(&c);
    ut_submit(&c, 100, /*type*/ 7);
    /* Server acks seq=1; queue becomes empty. The drain trigger must
     * be a no-op — and not crash on an empty range. */
    ut_ack_up_to(&c, 200, /*highest*/ 1);
    UT_ASSERT(c.outHeadSeq == c.outTailSeq);
    UT_ASSERT(c.outHeadSeq == 2);
    /* The acked entry's lastSentMs stays at its eager-send timestamp;
     * no second drain ran for it. */
    UT_ASSERT_MSG(ut_entry_sent_at(&c, 1) == 100,
                  "acked entry should not be re-drained (got %u)",
                  (unsigned)ut_entry_sent_at(&c, 1));
    return 0;
}
