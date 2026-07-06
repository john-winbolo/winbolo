/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * On-disk attribution-track schema checks.
 *
 * Forces attribution_track.h to compile (so its BOLO_STATIC_ASSERTs
 * evaluate) and pins the format's frozen constants: the packed record
 * sizes, the version, the zip member name, and the record-type tag
 * values. No serialization or record construction here — the writer
 * and reader are exercised elsewhere.
 */
#include <stdint.h>
#include <string.h>

#include "attribution_track.h"
#include "wire_limits.h"     /* PACKET_MAX_PLAYER_NAME */
#include "global.h"          /* MAX_TANKS */
#include "test_harness.h"

int run_attribution_track_schema(void) {
    UT_ASSERT(sizeof(AttrDamageRecord) == 12);
    UT_ASSERT(sizeof(AttrKillRecord) == 10);
    UT_ASSERT(sizeof(AttrCaptureRecord) == 10);
    UT_ASSERT(sizeof(AttrLgmRecord) == 7);
    UT_ASSERT(sizeof(AttrActionRecord) == 7);
    UT_ASSERT(sizeof(AttrSlotIdentity) == (size_t)(2 + PACKET_MAX_PLAYER_NAME));
    UT_ASSERT(sizeof(AttrTrackHeader) ==
              (size_t)(12 + MAX_TANKS * sizeof(AttrSlotIdentity)));

    UT_ASSERT(ATTRIBUTION_TRACK_VERSION == 1);
    UT_ASSERT(strcmp(ATTRIBUTION_TRACK_MEMBER, "attribution.trk") == 0);
    UT_ASSERT(memcmp(ATTRIBUTION_TRACK_MAGIC, "WBAT", 4) == 0);

    UT_ASSERT(ATTR_REC_DAMAGE == 1);
    UT_ASSERT(ATTR_REC_KILL == 2);
    UT_ASSERT(ATTR_REC_CAPTURE == 3);
    UT_ASSERT(ATTR_REC_LGM == 4);
    UT_ASSERT(ATTR_REC_ACTION == 5);

    return 0;
}
