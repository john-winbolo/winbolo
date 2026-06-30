/*
 * Body-codec round-trip coverage for CTRL_SPECTATOR_CHAT — the lobby chat
 * line a spectator types. Like the other live-control events it is delivered
 * body-only (no full-packet wrapper, no PACKET_* type), so the test resolves
 * the encode/decode functions through the body tables
 * (transportControlCodecBodyEncoder / BodyDecoder) the live path uses.
 *
 *   normal   — specIdx + message survive encode -> decode.
 *   empty    — a zero-length message produces the 2-byte minimum body and
 *              decodes back to bodyLen 0.
 *   maxlen   — a PACKET_MAX_CHAT_MESSAGE message round-trips intact.
 *   bound    — a body whose specIdx == MAX_SPECTATORS is rejected.
 *   overrun  — a body claiming more message bytes than it carries is rejected.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "transport_udp.h"   /* MAX_SPECTATORS */
#include "wire_limits.h"     /* PACKET_MAX_CHAT_MESSAGE */
#include "test_harness.h"

int run_spectator_chat_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SPECTATOR_CHAT);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SPECTATOR_CHAT);
    UT_ASSERT_MSG(enc != NULL, "no body encoder registered for CTRL_SPECTATOR_CHAT");
    UT_ASSERT_MSG(dec != NULL, "no body decoder registered for CTRL_SPECTATOR_CHAT");

    /* normal — specIdx + message round-trip. */
    {
        ControlEvent in, out;
        const char *text = "hi all";
        memset(&in, 0, sizeof(in));
        in.type = CTRL_SPECTATOR_CHAT;
        in.u.spectatorChat.specIdx = 5;
        in.u.spectatorChat.bodyLen = (uint16_t)strlen(text);
        memcpy(in.u.spectatorChat.body, text, strlen(text));

        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT_MSG(outLen == 2 + strlen(text),
                      "body len = %zu (want %zu)", outLen, 2 + strlen(text));

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (normal)");
        UT_ASSERT(out.type == CTRL_SPECTATOR_CHAT);
        UT_ASSERT(out.u.spectatorChat.specIdx == 5);
        UT_ASSERT(out.u.spectatorChat.bodyLen == strlen(text));
        UT_ASSERT(memcmp(out.u.spectatorChat.body, text, strlen(text)) == 0);
    }

    /* empty — zero-length message is a 2-byte body, decodes to bodyLen 0. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_SPECTATOR_CHAT;
        in.u.spectatorChat.specIdx = 0;
        in.u.spectatorChat.bodyLen = 0;

        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT_MSG(outLen == 2, "empty body should be 2 bytes, got %zu", outLen);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (empty)");
        UT_ASSERT(out.u.spectatorChat.specIdx == 0);
        UT_ASSERT(out.u.spectatorChat.bodyLen == 0);
    }

    /* maxlen — a full PACKET_MAX_CHAT_MESSAGE message round-trips. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_SPECTATOR_CHAT;
        in.u.spectatorChat.specIdx = MAX_SPECTATORS - 1;
        in.u.spectatorChat.bodyLen = PACKET_MAX_CHAT_MESSAGE;
        for (int i = 0; i < PACKET_MAX_CHAT_MESSAGE; i++) {
            in.u.spectatorChat.body[i] = (uint8_t)('A' + (i % 26));
        }

        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (maxlen)");
        UT_ASSERT(out.u.spectatorChat.specIdx == MAX_SPECTATORS - 1);
        UT_ASSERT(out.u.spectatorChat.bodyLen == PACKET_MAX_CHAT_MESSAGE);
        UT_ASSERT(memcmp(out.u.spectatorChat.body, in.u.spectatorChat.body,
                         PACKET_MAX_CHAT_MESSAGE) == 0);
    }

    /* bound — specIdx == MAX_SPECTATORS is out of range and rejected. */
    {
        ControlEvent out;
        uint8_t body[2] = { (uint8_t)MAX_SPECTATORS, 0 };
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(body, sizeof(body), &out),
                      "decoder must reject specIdx == MAX_SPECTATORS");
    }

    /* overrun — msgLen claims 4 bytes but only 1 follows; reject. */
    {
        ControlEvent out;
        uint8_t body[3] = { 0, 4, 'x' };
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(body, sizeof(body), &out),
                      "decoder must reject a msgLen that overruns the body");
    }

    return 0;
}
