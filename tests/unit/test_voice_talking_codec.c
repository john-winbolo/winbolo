/*
 * Body-codec round-trip coverage for CTRL_VOICE_TALKING — the set of
 * slots the server says are producing voice, sent in the lobby and the
 * countdown. The event is delivered body-only (no full-packet wrapper,
 * no PACKET_* type), so the test resolves the encode/decode functions
 * through the body tables (transportControlCodecBodyEncoder /
 * BodyDecoder) the way the live control path does.
 *
 *   empty   — nobody talking; the set the server sends as a round starts.
 *   single  — one bit, and only that bit.
 *   several — a spread of bits including slot 0.
 *   top     — MAX_TANKS - 1, the bit a narrow field would lose.
 *   bound   — a 3-byte body is rejected.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* Encode one bitmap, decode it back, and return what survived. */
static PlayerBitMap roundTrip(ControlEncodeBodyFn enc, ControlDecodeBodyFn dec,
                              PlayerBitMap in) {
    ControlEvent evtIn, evtOut;
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;

    memset(&evtIn, 0, sizeof(evtIn));
    evtIn.type = CTRL_VOICE_TALKING;
    evtIn.u.voiceTalking.talking = in;

    if (enc(&evtIn, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK) return ~0u;
    if (outLen != 4) return ~0u;

    memset(&evtOut, 0, sizeof(evtOut));
    if (!dec(buf, outLen, &evtOut)) return ~0u;
    if (evtOut.type != CTRL_VOICE_TALKING) return ~0u;
    return evtOut.u.voiceTalking.talking;
}

int run_voice_talking_codec(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_VOICE_TALKING);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_VOICE_TALKING);
    UT_ASSERT_MSG(enc != NULL,
                  "no body encoder registered for CTRL_VOICE_TALKING");
    UT_ASSERT_MSG(dec != NULL,
                  "no body decoder registered for CTRL_VOICE_TALKING");

    /* empty — nobody talking. */
    {
        PlayerBitMap out = roundTrip(enc, dec, 0);
        UT_ASSERT_MSG(out == 0, "empty set became %u", (unsigned)out);
    }

    /* single — one talker, and nobody else. */
    {
        PlayerBitMap in  = (PlayerBitMap)1u << 3;
        PlayerBitMap out = roundTrip(enc, dec, in);
        UT_ASSERT_MSG(out == in, "single bit: expected %u, got %u",
                      (unsigned)in, (unsigned)out);
    }

    /* several — a spread including slot 0, so a mistake that drops the
     * low byte or shifts by one fails here. */
    {
        PlayerBitMap in = (PlayerBitMap)1u
                          | ((PlayerBitMap)1u << 1)
                          | ((PlayerBitMap)1u << 7)
                          | ((PlayerBitMap)1u << 12);
        PlayerBitMap out = roundTrip(enc, dec, in);
        UT_ASSERT_MSG(out == in, "several bits: expected %u, got %u",
                      (unsigned)in, (unsigned)out);
    }

    /* top — the highest slot there is. */
    {
        PlayerBitMap in  = (PlayerBitMap)1u << (MAX_TANKS - 1);
        PlayerBitMap out = roundTrip(enc, dec, in);
        UT_ASSERT_MSG(out == in, "top bit: expected %u, got %u",
                      (unsigned)in, (unsigned)out);
    }

    /* bound — the body is 4 bytes; a short one decodes to nothing. */
    {
        ControlEvent out;
        uint8_t shortBody[3] = { 0 };

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(shortBody, sizeof(shortBody), &out),
                      "decoder must reject a 3-byte VOICE_TALKING body");
    }

    return 0;
}
