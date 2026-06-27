/*
 * Body-codec round-trip coverage for CTRL_SPECTATOR_SLOT — the
 * per-spectator roster ControlEvent. Unlike the lobby variants this
 * event is delivered body-only (no full-packet wrapper, no PACKET_*
 * type), so the test resolves the encode/decode functions through the
 * body tables (transportControlCodecBodyEncoder / BodyDecoder) the way
 * the live control path does.
 *
 *   connected     — every field survives encode -> decode.
 *   disconnected  — connected=false produces the 2-byte minimum body
 *                   and decodes back to connected=false.
 *   bound         — a body whose specIdx == MAX_SPECTATORS is rejected.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "transport_udp.h"   /* MAX_SPECTATORS */
#include "test_harness.h"

int run_spectator_slot_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SPECTATOR_SLOT);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SPECTATOR_SLOT);
    UT_ASSERT_MSG(enc != NULL, "no body encoder registered for CTRL_SPECTATOR_SLOT");
    UT_ASSERT_MSG(dec != NULL, "no body decoder registered for CTRL_SPECTATOR_SLOT");

    /* connected — full field round-trip. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_SPECTATOR_SLOT;
        in.u.spectatorSlot.specIdx = 7;
        in.u.spectatorSlot.slot.connected = true;
        strncpy(in.u.spectatorSlot.slot.playerName, "Watcher",
                sizeof(in.u.spectatorSlot.slot.playerName) - 1);
        in.u.spectatorSlot.slot.countryCode[0] = 'U';
        in.u.spectatorSlot.slot.countryCode[1] = 'S';
        in.u.spectatorSlot.slot.countryCode[2] = '\0';
        in.u.spectatorSlot.slot.clientType  = 1;
        in.u.spectatorSlot.slot.clientFlags = 0x05;

        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (connected)");
        UT_ASSERT(out.type == CTRL_SPECTATOR_SLOT);
        UT_ASSERT(out.u.spectatorSlot.specIdx == 7);
        UT_ASSERT(out.u.spectatorSlot.slot.connected == true);
        UT_ASSERT(strcmp(out.u.spectatorSlot.slot.playerName, "Watcher") == 0);
        UT_ASSERT(strcmp(out.u.spectatorSlot.slot.countryCode, "US") == 0);
        UT_ASSERT(out.u.spectatorSlot.slot.clientType  == 1);
        UT_ASSERT(out.u.spectatorSlot.slot.clientFlags == 0x05);
    }

    /* disconnected — 2-byte minimum body, decodes to connected=false. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_SPECTATOR_SLOT;
        in.u.spectatorSlot.specIdx = 3;
        in.u.spectatorSlot.slot.connected = false;

        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT_MSG(outLen == 2, "disconnected body should be 2 bytes, got %zu", outLen);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (disconnected)");
        UT_ASSERT(out.u.spectatorSlot.specIdx == 3);
        UT_ASSERT(out.u.spectatorSlot.slot.connected == false);
    }

    /* bound — specIdx == MAX_SPECTATORS is out of range and rejected. */
    {
        ControlEvent out;
        uint8_t body[2] = { (uint8_t)MAX_SPECTATORS, 0 };
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(body, sizeof(body), &out),
                      "decoder must reject specIdx == MAX_SPECTATORS");
    }

    return 0;
}
