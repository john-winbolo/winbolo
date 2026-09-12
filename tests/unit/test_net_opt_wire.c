/*
 * Pure unit tests for the network-optimization wire changes.
 *
 *   - pill_armour_intank_roundtrip: a pill's armour, its in-tank flag and its
 *     position-current flag ride separate bits of the wire record and survive
 *     a pack/unpack round trip independently of each other, including armour
 *     values that do not fit a nibble.
 *   - base_event_classification: the split base events have the expected
 *     wire sizes and reliability classes — owner (EVENT_BASE_UPDATE) is a
 *     reliable 2-byte event, stock (EVENT_BASE_STOCK) is a best-effort
 *     4-byte event, and the pill update is 6 bytes / best-effort.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "input_packet.h"
#include "pillbox.h"
#include "transport_udp_internal.h"  /* pack/unpackPillSnapshot */
#include "test_harness.h"

/* Armour and the two flags travel in bytes of their own, so pack then unpack
 * every armour value x both in-tank states x both position-current states and
 * require all three back unchanged. The expected bytes are written out here
 * rather than read back through unpackPillSnapshot: a packer and an unpacker
 * that drifted the same way would agree with each other and still be wrong.
 *
 * The armour list carries values a nibble cannot hold and whose bits 4 and 5
 * are set, so a reader still masking 0x0F, or a writer still folding armour
 * into the flags byte, fails here rather than passing by luck. They are past
 * PILLS_MAX_ARMOUR, which the encoding has to carry either way — what a pill
 * is allowed to hold is the sim's question, not this record's. */
int run_pill_armour_intank_roundtrip(void) {
    static const uint8_t armours[] = { 0, 1, PILLS_MAX_ARMOUR, 0x3A, 0xFF };
    int a;

    for (a = 0; a < (int)(sizeof armours / sizeof armours[0]); a++) {
        uint8_t armour = armours[a];
        int t;
        for (t = 0; t < 2; t++) {
            int c;
            for (c = 0; c < 2; c++) {
                bool inTank     = (t != 0);
                bool posCurrent = (c != 0);
                PillSnapshot in, out;
                uint8_t buf[PILL_SNAPSHOT_WIRE_SIZE];
                uint8_t wantFlags = (uint8_t)((inTank ? 0x10 : 0) |
                                              (posCurrent ? 0x20 : 0));
                int packed, consumed;

                memset(&in, 0, sizeof in);
                in.x         = 0x5B;
                in.y         = 0x91;
                in.owner     = 0x03;
                in.armour    = armour;
                in.pillFlags = pillSetPosCurrent(pillSetInTank(0, inTank),
                                                 posCurrent);

                packed = packPillSnapshot(buf, &in);
                UT_ASSERT_MSG(packed == PILL_SNAPSHOT_WIRE_SIZE,
                              "armour=%u inTank=%d posCurrent=%d: packed %d bytes",
                              (unsigned)armour, (int)inTank, (int)posCurrent,
                              packed);
                UT_ASSERT_MSG(buf[0] == 0x5B && buf[1] == 0x91 && buf[2] == 0x03,
                              "armour=%u: the square or the owner moved",
                              (unsigned)armour);
                UT_ASSERT_MSG(buf[3] == wantFlags,
                              "armour=%u inTank=%d posCurrent=%d: flags byte is "
                              "0x%02X, want 0x%02X",
                              (unsigned)armour, (int)inTank, (int)posCurrent,
                              (unsigned)buf[3], (unsigned)wantFlags);
                UT_ASSERT_MSG(buf[4] == armour,
                              "armour=%u went on the wire as 0x%02X",
                              (unsigned)armour, (unsigned)buf[4]);

                memset(&out, 0, sizeof out);
                consumed = unpackPillSnapshot(buf, sizeof buf, &out);
                UT_ASSERT_MSG(consumed == PILL_SNAPSHOT_WIRE_SIZE,
                              "armour=%u: unpack consumed %d bytes",
                              (unsigned)armour, consumed);
                UT_ASSERT_MSG(out.armour == armour,
                              "armour=%u came back as %u",
                              (unsigned)armour, (unsigned)out.armour);
                UT_ASSERT_MSG(pillInTankFromByte(out.pillFlags) == inTank,
                              "armour=%u inTank=%d: in-tank came back %d",
                              (unsigned)armour, (int)inTank,
                              (int)pillInTankFromByte(out.pillFlags));
                UT_ASSERT_MSG(pillPosCurrentFromByte(out.pillFlags) == posCurrent,
                              "armour=%u posCurrent=%d: position-current came "
                              "back %d", (unsigned)armour, (int)posCurrent,
                              (int)pillPosCurrentFromByte(out.pillFlags));
            }
        }
    }
    return 0;
}

/* Pin the wire-size and reliability classifiers for the split base events
 * and the pill update. */
int run_base_event_classification(void) {
    UT_ASSERT(gameEventDataSize(EVENT_BASE_UPDATE) == 2);
    UT_ASSERT(gameEventDataSize(EVENT_BASE_STOCK) == 4);
    UT_ASSERT(gameEventDataSize(EVENT_PILL_UPDATE) == 6);

    UT_ASSERT(gameEventIsReliable(EVENT_BASE_UPDATE) == true);
    UT_ASSERT(gameEventIsReliable(EVENT_BASE_STOCK) == false);
    UT_ASSERT(gameEventIsReliable(EVENT_PILL_UPDATE) == false);
    return 0;
}
