/* Tier-1 fuzz target — the channel-mux frame parser.
 *
 * Feeds attacker-controlled bytes straight into channelRecvFrame, the single
 * bounds-checked decode site for the whole reliability layer. The channel-mux
 * rework collapsed three hand-rolled reliable queues, the CONTROL_TICK/_ACK
 * carrier, and the three bulk chunkers into one frame codec (ack list +
 * segment list); every reliable byte the transport receives now passes through
 * this one variable-length parse. channelRecvFrame bounds-checks each field
 * against len and rejects a malformed or truncated frame with a negative
 * return and no over-read, so it is safe to call with an arbitrary buffer:
 * AddressSanitizer turns any read past size into a hard abort with the
 * triggering bytes.
 *
 * After a frame is consumed we drain every channel via channelReceive, so the
 * fuzzer also exercises the in-order delivery / stream-reassembly path the
 * parsed segments feed, not just the parse. The mux is reinitialised per input
 * for determinism (a fresh sequence space each run).
 *
 * This is the architecture plan's A.5 follow-up: the hand-written codec
 * channelRecvFrame is the residue codegen did not cover, and it is net-new
 * attack surface the queue-era fuzz targets never reached.
 */
#include <stddef.h>
#include <stdint.h>

#include "channel_mux.h"

static ChannelMux g_mux;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t out[CHANNEL_MAX_SEG];

    /* libFuzzer passes int len; clamp so a huge input can't wrap negative. */
    if (size > INT32_MAX) {
        size = INT32_MAX;
    }

    channelMuxInit(&g_mux);
    if (channelRecvFrame(&g_mux, data, (int)size) < 0) {
        return 0; /* rejected as malformed — nothing delivered to drain */
    }

    /* Delivery / reassembly path for whatever the frame accepted. */
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint16_t outLen;
        while (channelReceive(&g_mux, ch, out, &outLen)) {
            /* discard: ASan guards the copies into out */
        }
    }
    return 0;
}
