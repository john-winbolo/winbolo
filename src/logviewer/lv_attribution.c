/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include "lv_attribution.h"

/* Parsed track for the currently-loaded .wbv. `present` is false until a valid
 * member is parsed; the accessors report absent in that case. */
static struct {
    AttrTrackHeader header;
    uint8_t        *records;    /* private copy of the trailing record stream */
    size_t          recordLen;  /* bytes in `records` */
    uint32_t        recordCount;/* from the header */
    bool            present;
} s_track = { 0 };

void lvAttributionClear(void) {
    free(s_track.records);
    memset(&s_track, 0, sizeof(s_track));
}

bool lvAttributionParseMember(const uint8_t *data, size_t len) {
    lvAttributionClear();

    if (data == NULL || len < sizeof(AttrTrackHeader)) {
        return false;
    }
    if (memcmp(data, ATTRIBUTION_TRACK_MAGIC, 4) != 0) {
        return false;
    }
    /* The version byte follows the 4 magic bytes. Reject a newer format
     * forward-compatibly rather than misreading it. */
    if (data[4] > ATTRIBUTION_TRACK_VERSION) {
        return false;
    }

    memcpy(&s_track.header, data, sizeof(AttrTrackHeader));

    /* slotCount is a uint16 off disk and the file is not ours — a log arrives
     * from wherever the user got it, and the client now loads them straight
     * from a game server and from WinBolo.net. The writer always puts
     * MAX_TANKS there, but nothing downstream re-checks: readers bound a slot
     * index against slotCount and then index the fixed MAX_TANKS slots[],
     * which a crafted 65535 turns into a read off the end of this struct for
     * any slot byte above 15. Clamp it once here, where the value enters,
     * rather than at each of those. Clamped and not rejected so a member
     * written with fewer identities than we compile for still reads. */
    if (s_track.header.slotCount > MAX_TANKS) {
        s_track.header.slotCount = MAX_TANKS;
    }

    size_t recLen = len - sizeof(AttrTrackHeader);
    if (recLen > 0) {
        s_track.records = (uint8_t *)malloc(recLen);
        if (s_track.records == NULL) {
            lvAttributionClear();  /* OOM: stay absent, never crash */
            return false;
        }
        memcpy(s_track.records, data + sizeof(AttrTrackHeader), recLen);
    }
    s_track.recordLen   = recLen;
    s_track.recordCount = s_track.header.recordCount;
    s_track.present     = true;
    return true;
}

const AttrTrackHeader *lvAttributionGetHeader(void) {
    return s_track.present ? &s_track.header : NULL;
}

const uint8_t *lvAttributionGetRecords(size_t *outLen, uint32_t *outCount) {
    if (outLen != NULL)   *outLen   = s_track.present ? s_track.recordLen : 0;
    if (outCount != NULL) *outCount = s_track.present ? s_track.recordCount : 0;
    return s_track.present ? s_track.records : NULL;
}
