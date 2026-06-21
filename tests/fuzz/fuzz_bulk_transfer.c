/* Tier-1 fuzz target — the bulk-transfer receive state machine.
 *
 * Drives attacker-controlled bytes through bulkReceiverFeed: the receive-side
 * reassembly the channel-mux rework introduced for map download / resync /
 * upload / preview on CHANNEL_BULK. A server (or any peer) feeds these bytes,
 * so the stream header (kind / gen / totalSize / pathLen / path) and the body
 * length are all wire-supplied and untrusted. Two things make this worth its
 * own target:
 *
 *   - bulkParseStreamHeader hand-parses a variable-length header (pathLen
 *     drives a memcpy into BulkStreamHeader.path); it is reached via the
 *     receiver, not by any existing target.
 *   - the body fill trusts the wire totalSize as its buffer bound. The sink
 *     contract requires the sink to validate totalSize and reject anything it
 *     will not allocate; this target models a correct, bounding sink and lets
 *     ASan catch any write the receiver makes past the buffer the sink handed
 *     back (the dst is malloc'd to exactly totalSize, so an off-by-one in the
 *     body fill is a heap overflow caught immediately).
 *
 * To exercise header-spanning-fragments and tail-of-one + head-of-next, the
 * input is fed in small cycling-size fragments rather than one call. The
 * receiver is reinitialised per input for determinism.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "bulk_transfer.h"

/* Largest body this harness will allocate; a header claiming more is rejected,
 * exactly as a real sink caps (e.g. the upload sink at LOBBY_MAP_UPLOAD_MAX_
 * BYTES). Keeps per-exec allocation bounded and the reject path exercised. */
#define FUZZ_BULK_CAP (64u * 1024u)

/* Tracks the one live body buffer so it is freed if the input ends mid-body. */
typedef struct {
    uint8_t *live;
} FuzzBulkCtx;

static uint8_t *fuzzOnBegin(void *ctx, const BulkStreamHeader *h) {
    FuzzBulkCtx *c = (FuzzBulkCtx *)ctx;
    /* Reject zero-size and anything over the cap — the bounding-sink contract. */
    if (h->totalSize == 0 || h->totalSize > FUZZ_BULK_CAP) {
        return NULL;
    }
    free(c->live); /* defensive: never two live buffers at once */
    c->live = (uint8_t *)malloc(h->totalSize); /* exact size: tight ASan bound */
    return c->live;
}

static void fuzzOnComplete(void *ctx, const BulkStreamHeader *h, uint8_t *buf) {
    FuzzBulkCtx *c = (FuzzBulkCtx *)ctx;
    (void)h;
    (void)buf; /* == c->live */
    free(c->live);
    c->live = NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    BulkReceiver r;
    FuzzBulkCtx  ctx = {NULL};
    BulkRecvSink sink = {fuzzOnBegin, fuzzOnComplete, &ctx};

    bulkReceiverInit(&r);

    /* Feed the whole input in cycling 1..16 byte fragments so the state machine
     * sees headers and bodies split across fragment boundaries. */
    size_t pos = 0;
    uint32_t step = 1;
    while (pos < size) {
        uint32_t frag = step;
        if (frag > size - pos) {
            frag = (uint32_t)(size - pos);
        }
        bulkReceiverFeed(&r, data + pos, frag, &sink);
        pos += frag;
        step = step % 16u + 1u;
    }

    free(ctx.live); /* input ended mid-body: release the outstanding buffer */
    return 0;
}
