/* Tier-1 fuzz target — the voice segment parsers.
 *
 * Feeds attacker-controlled bytes into voiceSegmentUnpackUp and
 * voiceSegmentUnpackDown, the two parse sites for everything carried on
 * CHANNEL_VOICE. Voice is best-effort: segments arrive with no ack, no
 * reorder hold and nothing upstream vouching for them, so the bytes the
 * server unpacks in serverPumpVoice (and the client in its receive path)
 * are exactly whatever a peer chose to send. Both parsers are total —
 * they reject anything short or malformed with false and never read past
 * the length they were handed — so it is safe to call them with an
 * arbitrary buffer.
 *
 * Each successful parse hands back an opus pointer into the caller's
 * buffer plus a length, and that slice is passed straight on (re-framed
 * downstream by the server, decoded by the client). A parse that returned
 * a bad opusLen would look fine here unless something touches the bytes,
 * so every accepted payload is summed into a volatile sink: that read is
 * what turns an over-run length into an AddressSanitizer abort with the
 * triggering input, rather than a silent walk off the end of the segment.
 *
 * Nothing is asserted. A malformed input must come back false, and the
 * fuzzer's job is to find one that doesn't — or one that parses with a
 * length reaching past the buffer.
 */
#include <stddef.h>
#include <stdint.h>

#include "voice_segment.h"

/* volatile so the sum cannot be optimised away — the read is the point. */
static volatile uint64_t g_sink;

/* Touch every byte the parse claimed, so ASan sees an over-long opusLen. */
static void readPayload(const uint8_t *opus, int opusLen) {
    uint64_t sum = 0;
    int i;

    for (i = 0; i < opusLen; i++) {
        sum += opus[i];
    }
    g_sink = sum;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t seq, flags, fromPlayer;
    const uint8_t *opus;
    int opusLen;

    /* libFuzzer passes int len; clamp so a huge input can't wrap negative. */
    if (size > INT32_MAX) {
        size = INT32_MAX;
    }

    /* client -> server, as the server unpacks it off a client's ring. */
    if (voiceSegmentUnpackUp(data, (int)size, &seq, &flags, &opus, &opusLen)) {
        readPayload(opus, opusLen);
    }

    /* server -> client, as the client unpacks it before handing the frame
     * to the talker's jitter buffer. */
    if (voiceSegmentUnpackDown(data, (int)size, &fromPlayer, &seq, &flags,
                               &opus, &opusLen)) {
        readPayload(opus, opusLen);
    }
    return 0;
}
