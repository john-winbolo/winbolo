/*
 * Coverage for CTRL_CHANNEL_RESET — the per-client game-start event that drops
 * previous-game stragglers on the reliable game (channel 0) and map (channel 1)
 * channels. transportUdpServerOnGameStart collapses each connected client's
 * unacked send tail on those channels and sends that client its new baselines;
 * the client lifts its receive baselines so a straggler (seq below the
 * baseline) dedup-drops in the new game.
 *
 *   1. Codec round-trip — the two channel baselines survive the body
 *      encoder/decoder byte-identical. CTRL_CHANNEL_RESET is carrier-only
 *      (it rides the reliable control channel, never a standalone packet), so
 *      it is exercised through the body codec, not the full-packet encoder.
 *   2. Per-client baselines — driving two fabricated slots to different ch0/ch1
 *      depths through game start makes each client's CTRL_CHANNEL_RESET carry
 *      that client's own baselines, not a shared value.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"                  /* MAX_TANKS, BYTE */
#include "control_event.h"
#include "transport_control_codec.h"
#include "channel_mux.h"             /* CHANNEL_GAME / CHANNEL_MAP */
#include "transport_udp.h"           /* test hooks + transportUdpServerOnGameStart */
#include "test_harness.h"

/* ================================================================
 * 1. Codec round-trip — both baselines survive the body codec.
 * ================================================================ */
int run_channel_reset_codec_roundtrip(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_CHANNEL_RESET);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_CHANNEL_RESET);
    UT_ASSERT_MSG(enc != NULL, "no body encoder for CTRL_CHANNEL_RESET");
    UT_ASSERT_MSG(dec != NULL, "no body decoder for CTRL_CHANNEL_RESET");

    /* A spread of values including distinct ch0/ch1/ch3, varied masks, and a
     * high-bit-set u32 to catch byte-order or sign mistakes. The wire body is
     * a mask byte plus one u32 per set bit (ascending channel order), so the
     * expected length is 1 + 4*popcount(mask). */
    static const struct { uint8_t mask; uint32_t b0, b1, b3; } cases[] = {
        { 0u,                                       0u,          0u,          0u          },
        { 1u << CHANNEL_GAME,                       7u,          0u,          0u          },
        { 1u << CHANNEL_MAP,                        0u,          9u,          0u          },
        { 1u << CHANNEL_BULK,                       0u,          0u,          512u        },
        { (1u<<CHANNEL_GAME)|(1u<<CHANNEL_MAP),     1u,          2u,          0u          },
        { (1u<<CHANNEL_GAME)|(1u<<CHANNEL_BULK),    0x01020304u, 0u,          0xFFFEFDFCu },
        { (1u<<CHANNEL_GAME)|(1u<<CHANNEL_MAP)|(1u<<CHANNEL_BULK),
                                                    0xFFFFFFFFu, 0x80000000u, 0x0BADF00Du },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ControlEvent in, out;
        uint8_t buf[64];
        size_t outLen = 0;
        size_t want;
        int bits = 0, b;
        for (b = 0; b < 8; b++) if (cases[i].mask & (1u << b)) bits++;
        want = 1 + 4 * (size_t)bits;
        memset(&in, 0, sizeof(in));
        memset(&out, 0xAB, sizeof(out));
        in.type = CTRL_CHANNEL_RESET;
        in.u.channelReset.channelMask = cases[i].mask;
        in.u.channelReset.ch0Baseline = cases[i].b0;
        in.u.channelReset.ch1Baseline = cases[i].b1;
        in.u.channelReset.ch3Baseline = cases[i].b3;

        UT_ASSERT_MSG(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                      "encode failed (case %zu)", i);
        UT_ASSERT_MSG(outLen == want, "body is %zu bytes, want %zu (case %zu)",
                      outLen, want, i);
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (case %zu)", i);
        UT_ASSERT(out.type == CTRL_CHANNEL_RESET);
        UT_ASSERT_MSG(out.u.channelReset.channelMask == cases[i].mask,
                      "mask 0x%02X != 0x%02X (case %zu)",
                      (unsigned)out.u.channelReset.channelMask,
                      (unsigned)cases[i].mask, i);
        /* Only baselines for set bits are carried; the rest decode as 0. */
        UT_ASSERT_MSG(out.u.channelReset.ch0Baseline ==
                      ((cases[i].mask & (1u << CHANNEL_GAME)) ? cases[i].b0 : 0u),
                      "ch0 baseline mismatch (case %zu)", i);
        UT_ASSERT_MSG(out.u.channelReset.ch1Baseline ==
                      ((cases[i].mask & (1u << CHANNEL_MAP)) ? cases[i].b1 : 0u),
                      "ch1 baseline mismatch (case %zu)", i);
        UT_ASSERT_MSG(out.u.channelReset.ch3Baseline ==
                      ((cases[i].mask & (1u << CHANNEL_BULK)) ? cases[i].b3 : 0u),
                      "ch3 baseline mismatch (case %zu)", i);
    }

    /* A body whose mask promises a baseline the bytes don't supply must be
     * refused (mask sets one channel but no u32 follows). */
    {
        ControlEvent out;
        uint8_t shortBuf[3] = { (uint8_t)(1u << CHANNEL_GAME), 0, 0 };
        UT_ASSERT_MSG(!dec(shortBuf, sizeof(shortBuf), &out),
                      "decoder accepted a body short of its promised baseline");
    }
    /* An empty body (zero length) is refused — the mask byte is mandatory. */
    {
        ControlEvent out;
        UT_ASSERT_MSG(!dec(NULL, 0, &out),
                      "decoder accepted a zero-length body");
    }
    return 0;
}

/* ================================================================
 * 2. Per-client baselines — two slots at different ch0/ch1 depths each get
 *    their own CTRL_CHANNEL_RESET, not a shared value.
 *
 * Runs against the file-static server transport: this test executable is
 * launched as its own process (one --test per ctest entry), so udpServer is
 * fresh and zero-initialised. Fabricate two connected slots, push distinct
 * game/map send depths through the real channel hook, drive the real
 * transportUdpServerOnGameStart, then decode each slot's queued reset.
 * ================================================================ */
int run_channel_reset_two_slot_baselines(void) {
    const int slotA = 0, slotB = 3;
    const uint32_t a0 = 3, a1 = 1;   /* slot A: 3 game, 1 map  */
    const uint32_t b0 = 1, b1 = 5;   /* slot B: 1 game, 5 map  */
    uint8_t msg[8] = {0};
    uint32_t i;

    transportUdpServerTestForceConnect(slotA, (BYTE)slotA);
    transportUdpServerTestForceConnect(slotB, (BYTE)slotB);

    for (i = 0; i < a0; i++) {
        UT_ASSERT_MSG(transportUdpServerChannelTestSend(slotA, CHANNEL_GAME,
                                                        msg, sizeof(msg)),
                      "slotA game send %u rejected", (unsigned)i);
    }
    for (i = 0; i < a1; i++) {
        UT_ASSERT_MSG(transportUdpServerChannelTestSend(slotA, CHANNEL_MAP,
                                                        msg, sizeof(msg)),
                      "slotA map send %u rejected", (unsigned)i);
    }
    for (i = 0; i < b0; i++) {
        UT_ASSERT_MSG(transportUdpServerChannelTestSend(slotB, CHANNEL_GAME,
                                                        msg, sizeof(msg)),
                      "slotB game send %u rejected", (unsigned)i);
    }
    for (i = 0; i < b1; i++) {
        UT_ASSERT_MSG(transportUdpServerChannelTestSend(slotB, CHANNEL_MAP,
                                                        msg, sizeof(msg)),
                      "slotB map send %u rejected", (unsigned)i);
    }

    /* OnGameStart ignores its sim argument (it operates on udpServer's
     * per-client transport state), so NULL is safe here. */
    transportUdpServerOnGameStart(NULL);

    uint32_t gotA0 = 0xFFFFFFFFu, gotA1 = 0xFFFFFFFFu;
    uint32_t gotB0 = 0xFFFFFFFFu, gotB1 = 0xFFFFFFFFu;
    UT_ASSERT_MSG(transportUdpServerTestPeekChannelReset(slotA, &gotA0, &gotA1),
                  "slotA queued no CTRL_CHANNEL_RESET");
    UT_ASSERT_MSG(transportUdpServerTestPeekChannelReset(slotB, &gotB0, &gotB1),
                  "slotB queued no CTRL_CHANNEL_RESET");

    UT_ASSERT_MSG(gotA0 == a0 && gotA1 == a1,
                  "slotA reset {%u,%u}, want {%u,%u}",
                  (unsigned)gotA0, (unsigned)gotA1, (unsigned)a0, (unsigned)a1);
    UT_ASSERT_MSG(gotB0 == b0 && gotB1 == b1,
                  "slotB reset {%u,%u}, want {%u,%u}",
                  (unsigned)gotB0, (unsigned)gotB1, (unsigned)b0, (unsigned)b1);

    /* The point of the per-client test: the two slots' baselines differ — a
     * shared value would have failed the equality above, but pin the
     * distinctness explicitly. */
    UT_ASSERT_MSG(gotA0 != gotB0 || gotA1 != gotB1,
                  "both slots received the same baselines — not per-client");
    return 0;
}
