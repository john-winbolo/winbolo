/*
 * Body-codec round-trip coverage for CTRL_STATS_SEED — the running per-slot
 * scoreboard the server hands a mid-round joiner inside its sync replay. Like
 * the other live-control events it is delivered body-only (no full-packet
 * wrapper, no PACKET_* type), so the test resolves the encode/decode functions
 * through the body tables (transportControlCodecBodyEncoder / BodyDecoder) the
 * live path uses.
 *
 *   full      — a MAX_TANKS roster with edge values in every column survives
 *               encode -> decode, and the body is the expected 1 + 20*n bytes.
 *   empty     — a zero-row event produces the 1-byte minimum body and decodes
 *               back to playerCount 0.
 *   short     — a zero-length body is rejected rather than read.
 *   truncated — a body claiming three rows but carrying two and a half is
 *               rejected rather than read past.
 *   badslot   — a row whose slot == MAX_TANKS is rejected; the client indexes
 *               its per-slot board by that byte.
 *   clamp     — a count above MAX_TANKS, fully backed by bytes, clamps to
 *               MAX_TANKS rather than writing past the row array.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "control_event.h"
#include "round_stats.h"
#include "transport_control_codec.h"
#include "test_harness.h"

#define SEED_ROW_BYTES 20

int run_stats_seed_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_STATS_SEED);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_STATS_SEED);
    UT_ASSERT_MSG(enc != NULL, "no body encoder registered for CTRL_STATS_SEED");
    UT_ASSERT_MSG(dec != NULL, "no body decoder registered for CTRL_STATS_SEED");

    /* full — every slot present, edge values in every column. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_STATS_SEED;
        in.u.statsSeed.playerCount = MAX_TANKS;
        for (int i = 0; i < MAX_TANKS; i++) {
            RoundPlayerSummary *p = &in.u.statsSeed.players[i];
            p->slot         = (uint8_t)i;
            p->isBot        = (uint8_t)(i & 1);
            p->kills        = (i == 0) ? 0 : 0xFFFF;
            p->deaths       = (uint16_t)(i * 7);
            p->baseCaptures = (i == 1) ? 0xFFFF : (uint16_t)i;
            p->pillCaptures = (uint16_t)(0xFFFF - i);
            p->dmgDealt     = (i == 2) ? 0xFFFFFFFFu : (uint32_t)(i * 100000);
            p->builds       = (uint16_t)(i * 3);
            p->lgmKills     = (uint16_t)(i * 11);
            p->lgmDeaths    = (i == 3) ? 0xFFFF : 0;
        }

        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT_MSG(outLen == (size_t)1 + MAX_TANKS * SEED_ROW_BYTES,
                      "body len = %zu (want %zu)", outLen,
                      (size_t)1 + MAX_TANKS * SEED_ROW_BYTES);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (full)");
        UT_ASSERT(out.type == CTRL_STATS_SEED);
        UT_ASSERT(out.u.statsSeed.playerCount == MAX_TANKS);
        for (int i = 0; i < MAX_TANKS; i++) {
            const RoundPlayerSummary *a = &in.u.statsSeed.players[i];
            const RoundPlayerSummary *b = &out.u.statsSeed.players[i];
            UT_ASSERT_MSG(a->slot == b->slot, "row %d slot", i);
            UT_ASSERT_MSG(a->isBot == b->isBot, "row %d isBot", i);
            UT_ASSERT_MSG(a->kills == b->kills, "row %d kills", i);
            UT_ASSERT_MSG(a->deaths == b->deaths, "row %d deaths", i);
            UT_ASSERT_MSG(a->baseCaptures == b->baseCaptures, "row %d base", i);
            UT_ASSERT_MSG(a->pillCaptures == b->pillCaptures, "row %d pill", i);
            UT_ASSERT_MSG(a->dmgDealt == b->dmgDealt, "row %d dmg", i);
            UT_ASSERT_MSG(a->builds == b->builds, "row %d builds", i);
            UT_ASSERT_MSG(a->lgmKills == b->lgmKills, "row %d lgmKills", i);
            UT_ASSERT_MSG(a->lgmDeaths == b->lgmDeaths, "row %d lgmDeaths", i);
        }
    }

    /* empty — a round with nobody connected is a 1-byte body. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_STATS_SEED;
        in.u.statsSeed.playerCount = 0;

        uint8_t buf[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT_MSG(outLen == 1, "empty body should be 1 byte, got %zu", outLen);

        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (empty)");
        UT_ASSERT(out.type == CTRL_STATS_SEED);
        UT_ASSERT(out.u.statsSeed.playerCount == 0);
    }

    /* short — no count byte at all. */
    {
        ControlEvent out;
        uint8_t body[1] = { 0 };
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(body, 0, &out),
                      "decoder must reject a zero-length body");
    }

    /* truncated — claims 3 rows, carries 2.5; reject rather than read past. */
    {
        ControlEvent out;
        uint8_t body[1 + 3 * SEED_ROW_BYTES];
        memset(body, 0, sizeof(body));
        body[0] = 3;
        body[1 + 0 * SEED_ROW_BYTES] = 0;
        body[1 + 1 * SEED_ROW_BYTES] = 1;
        body[1 + 2 * SEED_ROW_BYTES] = 2;
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(body, sizeof(body) - 10, &out),
                      "decoder must reject a body shorter than its row count");
    }

    /* badslot — the slot byte indexes the client's per-slot board. */
    {
        ControlEvent out;
        uint8_t body[1 + SEED_ROW_BYTES];
        memset(body, 0, sizeof(body));
        body[0] = 1;
        body[1] = (uint8_t)MAX_TANKS;   /* one past the last valid slot */
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(!dec(body, sizeof(body), &out),
                      "decoder must reject slot == MAX_TANKS");
    }

    /* clamp — a count above MAX_TANKS keeps only MAX_TANKS rows. */
    {
        ControlEvent out;
        const int claimed = MAX_TANKS + 4;
        uint8_t body[1 + (MAX_TANKS + 4) * SEED_ROW_BYTES];
        memset(body, 0, sizeof(body));
        body[0] = (uint8_t)claimed;
        for (int i = 0; i < claimed; i++) {
            /* Every stored row needs an in-range slot; the surplus rows are
             * never read, so their slot byte is irrelevant. */
            body[1 + i * SEED_ROW_BYTES] = (uint8_t)(i % MAX_TANKS);
            body[1 + i * SEED_ROW_BYTES + 3] = (uint8_t)(i + 1);  /* kills lo */
        }
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(dec(body, sizeof(body), &out),
                      "decode failed (clamp)");
        UT_ASSERT_MSG(out.u.statsSeed.playerCount == MAX_TANKS,
                      "playerCount = %u (want %d)",
                      (unsigned)out.u.statsSeed.playerCount, MAX_TANKS);
        for (int i = 0; i < MAX_TANKS; i++) {
            UT_ASSERT_MSG(out.u.statsSeed.players[i].kills == (uint16_t)(i + 1),
                          "row %d kills = %u", i,
                          (unsigned)out.u.statsSeed.players[i].kills);
        }
    }

    return 0;
}
