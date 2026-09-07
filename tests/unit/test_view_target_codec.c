/*
 * Body-codec round-trip coverage for CTRL_VIEW_TARGET — the server's
 * answer to a CMD_VIEW_CYCLE request. The event is delivered body-only
 * (no full-packet wrapper, no PACKET_* type), so the test resolves the
 * encode/decode functions through the body tables
 * (transportControlCodecBodyEncoder / BodyDecoder) the way the live
 * control path does.
 *
 *   found      — every field survives encode -> decode, with distinct
 *                values so a field-order mistake fails.
 *   not found  — found=0 with an echoed 0xFF from and a zero target
 *                round-trips intact.
 *   bound      — a 6-byte and an 8-byte body are both rejected.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"  /* VIEW_KIND_ALLY */
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

int run_view_target_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_VIEW_TARGET);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_VIEW_TARGET);
    UT_ASSERT_MSG(enc != NULL, "no body encoder registered for CTRL_VIEW_TARGET");
    UT_ASSERT_MSG(dec != NULL, "no body decoder registered for CTRL_VIEW_TARGET");

    /* found — full field round-trip, every field a different value. */
    {
        ControlEvent in, out;
        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;

        memset(&in, 0, sizeof(in));
        in.type = CTRL_VIEW_TARGET;
        in.u.viewTarget.origSlot = 2;
        in.u.viewTarget.kind     = VIEW_KIND_ALLY;
        in.u.viewTarget.target   = 5;
        in.u.viewTarget.mapX     = 17;
        in.u.viewTarget.mapY     = 200;
        in.u.viewTarget.found    = 1;
        in.u.viewTarget.fromEcho = 9;

        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT_MSG(outLen == 7, "body should be 7 bytes, got %zu", outLen);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (found)");
        UT_ASSERT(out.type == CTRL_VIEW_TARGET);
        UT_ASSERT(out.u.viewTarget.origSlot == 2);
        UT_ASSERT(out.u.viewTarget.kind     == VIEW_KIND_ALLY);
        UT_ASSERT(out.u.viewTarget.target   == 5);
        UT_ASSERT(out.u.viewTarget.mapX     == 17);
        UT_ASSERT(out.u.viewTarget.mapY     == 200);
        UT_ASSERT(out.u.viewTarget.found    == 1);
        UT_ASSERT(out.u.viewTarget.fromEcho == 9);
    }

    /* not found — nothing to watch: found=0, the request's from echoed back. */
    {
        ControlEvent in, out;
        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;

        memset(&in, 0, sizeof(in));
        in.type = CTRL_VIEW_TARGET;
        in.u.viewTarget.origSlot = 4;
        in.u.viewTarget.kind     = VIEW_KIND_ALLY;
        in.u.viewTarget.fromEcho = 0xFF;

        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT_MSG(outLen == 7, "body should be 7 bytes, got %zu", outLen);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (not found)");
        UT_ASSERT(out.u.viewTarget.origSlot == 4);
        UT_ASSERT(out.u.viewTarget.kind     == VIEW_KIND_ALLY);
        UT_ASSERT(out.u.viewTarget.found    == 0);
        UT_ASSERT(out.u.viewTarget.target   == 0);
        UT_ASSERT(out.u.viewTarget.mapX     == 0);
        UT_ASSERT(out.u.viewTarget.mapY     == 0);
        UT_ASSERT(out.u.viewTarget.fromEcho == 0xFF);
    }

    /* bound — the body is a fixed 7 bytes; nothing else decodes. */
    {
        ControlEvent out;
        uint8_t shortBody[6] = { 0 };
        uint8_t longBody[8]  = { 0 };

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(shortBody, sizeof(shortBody), &out),
                      "decoder must reject a 6-byte VIEW_TARGET body");
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(longBody, sizeof(longBody), &out),
                      "decoder must reject an 8-byte VIEW_TARGET body");
    }

    return 0;
}
