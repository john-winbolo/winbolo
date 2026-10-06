/*
 * Body-codec round-trip coverage for CTRL_VOICE_EVERYONE — whether a
 * scenario has sent the round's voice to every player rather than to the
 * talker's allies alone. The event is delivered body-only (no full-packet
 * wrapper, no PACKET_* type), so the test resolves the encode/decode
 * functions through the body tables (transportControlCodecBodyEncoder /
 * BodyDecoder) the way the live control path does.
 *
 *   on     — true survives, as one byte of 1.
 *   off    — false survives, as one byte of 0.
 *   other  — a byte other than 0 or 1 is refused, not read as on.
 *   bound  — an empty body is refused.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* Encode one value and decode it back. Returns 1 for on, 0 for off, -1 when
 * either half failed or the body was not the one byte it should be. */
static int roundTrip(ControlEncodeBodyFn enc, ControlDecodeBodyFn dec,
                     bool in) {
    ControlEvent evtIn, evtOut;
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;

    memset(&evtIn, 0, sizeof(evtIn));
    evtIn.type = CTRL_VOICE_EVERYONE;
    evtIn.u.voiceEveryone.on = in;

    if (enc(&evtIn, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK) return -1;
    if (outLen != 1 || buf[0] != (in ? 1 : 0)) return -1;

    memset(&evtOut, 0, sizeof(evtOut));
    if (!dec(buf, outLen, &evtOut)) return -1;
    if (evtOut.type != CTRL_VOICE_EVERYONE) return -1;
    return evtOut.u.voiceEveryone.on ? 1 : 0;
}

int run_voice_everyone_codec(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_VOICE_EVERYONE);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_VOICE_EVERYONE);
    UT_ASSERT_MSG(enc != NULL,
                  "no body encoder registered for CTRL_VOICE_EVERYONE");
    UT_ASSERT_MSG(dec != NULL,
                  "no body decoder registered for CTRL_VOICE_EVERYONE");

    /* on and off. */
    UT_ASSERT_MSG(roundTrip(enc, dec, true) == 1, "on did not survive");
    UT_ASSERT_MSG(roundTrip(enc, dec, false) == 0, "off did not survive");

    /* other — only 0 and 1 mean anything; a corrupt byte must not tell a
     * player their voice goes further than it does. */
    {
        ControlEvent out;
        uint8_t body[1] = { 2 };

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(body, sizeof(body), &out),
                      "decoder must reject a VOICE_EVERYONE byte of 2");
    }

    /* bound — the body is 1 byte; an empty one decodes to nothing. */
    {
        ControlEvent out;
        uint8_t none[1] = { 1 };

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(none, 0, &out),
                      "decoder must reject an empty VOICE_EVERYONE body");
    }

    return 0;
}
