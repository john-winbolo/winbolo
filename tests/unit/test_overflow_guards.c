/*
 * Send-side overflow guards on the two generic stream primitives.
 *
 * Both channelStreamSend (channel_mux.c) and bulkSenderBegin (bulk_transfer.c)
 * derive an allocation/capacity bound from caller-supplied lengths. Every
 * in-tree caller passes bounded lengths, so these guards never trip in normal
 * play — this is defense-in-depth on a reusable primitive. The cases below
 * prove that a length large enough to wrap the 32-bit arithmetic is rejected
 * via the existing failure path (return false), with no state change, rather
 * than wrapping into a too-small bound.
 *
 * No sockets, no threads, no randomness.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bulk_transfer.h"
#include "channel_mux.h"
#include "test_harness.h"

/* channelStreamSend's capacity check is `len > CHANNEL_STREAM_BUF - streamCount`.
 * The pre-hardening form, `streamCount + len > CHANNEL_STREAM_BUF`, wraps when
 * streamCount > 0 and len is near UINT32_MAX, wrongly accepting a request that
 * would then run the copy loop for ~4 GB. This drives streamCount above zero,
 * then offers such a length and asserts the reject leaves the staging buffer
 * untouched. */
static int t_stream_send_overflow_rejected(void) {
    ChannelMux *m = (ChannelMux *)malloc(sizeof(*m));
    uint8_t *src = (uint8_t *)malloc(CHANNEL_STREAM_BUF);
    int rc = 1;
    if (!m || !src) {
        goto done;
    }
    channelMuxInit(m);
    memset(src, 0xA5, CHANNEL_STREAM_BUF);

    /* Fill the staging buffer. Refill drains one window's worth into segments
     * (CHANNEL_BULK_WINDOW * CHANNEL_BULK_SEG bytes), so a non-zero remainder
     * stays pending — exactly the streamCount > 0 state the wrap needs. */
    if (!channelStreamSend(m, CHANNEL_BULK, src, CHANNEL_STREAM_BUF)) {
        goto done; /* the whole buffer must be accepted from empty */
    }
    uint32_t pending = m->streamCount;
    if (pending == 0) {
        goto done; /* expected a pending remainder to exercise the wrap */
    }

    /* A length that overflows pending + len (wraps to a small value) must be
     * rejected, not accepted off the wrapped sum. */
    if (channelStreamSend(m, CHANNEL_BULK, NULL, UINT32_MAX - 1000u)) {
        goto done; /* wrap-prone length must be refused */
    }
    if (m->streamCount != pending) {
        goto done; /* a rejected send must not change state */
    }

    /* The ordinary just-too-big reject (no wrap) must also hold: one byte past
     * the remaining capacity is refused. */
    uint32_t remaining = (uint32_t)CHANNEL_STREAM_BUF - pending;
    if (channelStreamSend(m, CHANNEL_BULK, NULL, remaining + 1u)) {
        goto done;
    }
    if (m->streamCount != pending) {
        goto done;
    }
    rc = 0;
done:
    free(m);
    free(src);
    if (rc) {
        UT_FAIL("channelStreamSend did not reject a wrap-prone length cleanly");
    }
    return 0;
}

/* bulkSenderBegin sizes its malloc as BULK_STREAM_HEADER_FIXED + pathLen +
 * blobLen. A blobLen near UINT32_MAX wraps that total to a tiny allocation; the
 * guard must reject it via the same false/no-state-change path as the
 * serializer guard, leaving the sender idle and allocating nothing. */
static int t_sender_begin_overflow_rejected(void) {
    BulkSender s;
    BulkStreamHeader h;
    bulkSenderInit(&s);
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_UPLOAD;
    h.pathLen = 0;

    /* blobLen large enough that header + blob overflows uint32_t. */
    if (bulkSenderBegin(&s, &h, NULL, UINT32_MAX)) {
        UT_FAIL("bulkSenderBegin accepted an overflowing blob length");
    }
    if (bulkSenderBusy(&s)) {
        UT_FAIL("a rejected bulkSenderBegin left the sender busy");
    }

    /* A bounded transfer still begins normally — the guard only rejects the
     * wrap, it does not change behaviour for in-range callers. */
    uint8_t blob[32];
    memset(blob, 0x5A, sizeof(blob));
    h.totalSize = sizeof(blob);
    if (!bulkSenderBegin(&s, &h, blob, sizeof(blob))) {
        UT_FAIL("bulkSenderBegin rejected an in-range transfer");
    }
    if (!bulkSenderBusy(&s)) {
        UT_FAIL("a started transfer did not mark the sender busy");
    }
    bulkSenderReset(&s);
    return 0;
}

int run_overflow_guards(void) {
    if (t_stream_send_overflow_rejected()) {
        return 1;
    }
    if (t_sender_begin_overflow_rejected()) {
        return 1;
    }
    return 0;
}
