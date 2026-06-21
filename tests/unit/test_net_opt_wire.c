/*
 * Pure unit tests for the network-optimization wire changes.
 *
 *   - pill_armour_intank_roundtrip: the pill armour/inTank pack/unpack
 *     helpers (pillPackArmourInTank / pillArmourFromByte /
 *     pillInTankFromByte) round-trip across the full 0..PILLS_MAX_ARMOUR
 *     armour range and both inTank states, and never touch any byte above
 *     the low nibble + inTank bit.
 *   - base_event_classification: the split base events have the expected
 *     wire sizes and reliability classes — owner (EVENT_BASE_UPDATE) is a
 *     reliable 2-byte event, stock (EVENT_BASE_STOCK) is a best-effort
 *     4-byte event, and the packed pill update is 5 bytes / best-effort.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "input_packet.h"
#include "pillbox.h"
#include "test_harness.h"

/* Pack then unpack every armour value × both inTank states. The packed
 * byte must round-trip and stay within the low nibble + inTank bit so a
 * future field could share the high bits. The out-of-range armour path is
 * deliberately not exercised — it fires the helper's assert(). */
int run_pill_armour_intank_roundtrip(void) {
    for (int armour = 0; armour <= PILLS_MAX_ARMOUR; armour++) {
        for (int t = 0; t < 2; t++) {
            bool inTank = (t != 0);
            uint8_t b = pillPackArmourInTank((uint8_t)armour, inTank);

            UT_ASSERT_MSG(pillArmourFromByte(b) == (uint8_t)armour,
                          "armour=%d inTank=%d: unpacked armour %u",
                          armour, (int)inTank, (unsigned)pillArmourFromByte(b));
            UT_ASSERT_MSG(pillInTankFromByte(b) == inTank,
                          "armour=%d inTank=%d: unpacked inTank %d",
                          armour, (int)inTank, (int)pillInTankFromByte(b));
            UT_ASSERT_MSG(b < 0x20,
                          "armour=%d inTank=%d: packed byte 0x%02X uses high bits",
                          armour, (int)inTank, (unsigned)b);
        }
    }
    return 0;
}

/* Pin the wire-size and reliability classifiers for the split base events
 * and the packed pill update. */
int run_base_event_classification(void) {
    UT_ASSERT(gameEventDataSize(EVENT_BASE_UPDATE) == 2);
    UT_ASSERT(gameEventDataSize(EVENT_BASE_STOCK) == 4);
    UT_ASSERT(gameEventDataSize(EVENT_PILL_UPDATE) == 5);

    UT_ASSERT(gameEventIsReliable(EVENT_BASE_UPDATE) == true);
    UT_ASSERT(gameEventIsReliable(EVENT_BASE_STOCK) == false);
    UT_ASSERT(gameEventIsReliable(EVENT_PILL_UPDATE) == false);
    return 0;
}
