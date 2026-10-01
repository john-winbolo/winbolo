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

/*********************************************************
 *Name:          Spectator Ring
 *Filename:      spectator_ring.c
 *Author:        John Morrison
 *Purpose:
 *  Implementation of the delayed-stream ring buffer. See
 *  spectator_ring.h for the model.
 *
 *  Records are held oldest-first in one grown-on-demand
 *  array. recordSeq is assigned +1 per record and is dense
 *  across the retained range (eviction only ever drops a
 *  prefix), so the record at recordSeq s lives at array index
 *  (s - oldestSeq). Each record carries the segment it
 *  belongs to; a seed keyframe and its replayed events always
 *  share one segment.
 *********************************************************/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spectator_ring.h"

typedef struct {
    uint32_t gameTick;
    uint32_t recordSeq;
    uint32_t segment;
    bool     isKeyframe;
    uint8_t *payload;  /* owned copy; NULL when len == 0 */
    int      len;
} SpectatorRecord;

struct SpectatorRing {
    uint32_t keyframeCadenceTicks;
    uint32_t retentionTicks;

    SpectatorRecord *recs;  /* oldest at index 0 */
    int              count;
    int              cap;

    bool     hasAny;          /* any tick ever recorded                      */
    bool     everEvicted;     /* any record ever dropped by retention        */
    uint32_t nextSeq;         /* recordSeq to assign the next record         */
    uint32_t lastGameTick;    /* gameTick of the most recent record          */
    uint32_t curSegment;      /* segment id of the most recent record        */
    uint32_t segmentFirstGameTick; /* gameTick that opened the current segment */
    int      segmentsOpened;  /* total segments opened over the lifetime     */
};

#define SPECTATOR_RING_INITIAL_CAP 16

SpectatorRing *spectatorRingCreate(uint32_t keyframeCadenceTicks,
                                   uint32_t retentionTicks) {
    SpectatorRing *r = (SpectatorRing *)calloc(1, sizeof(SpectatorRing));
    if (r == NULL) {
        return NULL;
    }
    r->keyframeCadenceTicks = keyframeCadenceTicks;
    r->retentionTicks = retentionTicks;
    r->cap = SPECTATOR_RING_INITIAL_CAP;
    r->recs = (SpectatorRecord *)malloc((size_t)r->cap * sizeof(SpectatorRecord));
    if (r->recs == NULL) {
        free(r);
        return NULL;
    }
    return r;
}

void spectatorRingDestroy(SpectatorRing *r) {
    int i;

    if (r == NULL) {
        return;
    }
    for (i = 0; i < r->count; i++) {
        free(r->recs[i].payload);
    }
    free(r->recs);
    free(r);
}

bool spectatorRingNeedsKeyframe(const SpectatorRing *r, uint32_t gameTick) {
    if (!r->hasAny) {
        return true; /* first record ever */
    }
    if (gameTick <= r->lastGameTick) {
        return true; /* opens a new segment */
    }
    if (r->keyframeCadenceTicks == 0) {
        return false;
    }
    return ((gameTick - r->segmentFirstGameTick) % r->keyframeCadenceTicks) == 0;
}

/* Drop the oldest `n` records, freeing their payloads. */
static void spectatorRingDropOldest(SpectatorRing *r, int n) {
    int i;

    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(r->recs[i].payload);
    }
    memmove(&r->recs[0], &r->recs[n],
            (size_t)(r->count - n) * sizeof(SpectatorRecord));
    r->count -= n;
    r->everEvicted = true;
}

/* Keep the newest keyframe with recordSeq <= horizon and everything after it;
 * evict everything older. horizon is head recordSeq - retentionTicks,
 * saturating at 0. */
static void spectatorRingApplyRetention(SpectatorRing *r) {
    uint32_t head;
    uint32_t horizon;
    int keepFrom;
    int i;

    if (r->count == 0) {
        return;
    }
    head = r->recs[r->count - 1].recordSeq;
    horizon = (r->retentionTicks >= head) ? 0u : head - r->retentionTicks;

    /* Newest keyframe at or below the horizon; nothing older than it survives. */
    keepFrom = -1;
    for (i = 0; i < r->count; i++) {
        if (r->recs[i].recordSeq <= horizon && r->recs[i].isKeyframe) {
            keepFrom = i;
        }
    }
    if (keepFrom > 0) {
        spectatorRingDropOldest(r, keepFrom);
    }
}

bool spectatorRingRecordTick(SpectatorRing *r, uint32_t gameTick,
                             bool isKeyframe, const uint8_t *payload, int len) {
    bool newSegment;
    uint32_t segment;
    SpectatorRecord *rec;
    uint8_t *copy = NULL;

    if (len < 0 || (len > 0 && payload == NULL)) {
        return false;
    }

    newSegment = (!r->hasAny) || (gameTick <= r->lastGameTick);
    if (newSegment && !isKeyframe) {
        return false; /* a segment must open with a keyframe */
    }

    if (len > 0) {
        copy = (uint8_t *)malloc((size_t)len);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy, payload, (size_t)len);
    }

    if (r->count == r->cap) {
        int newCap = r->cap * 2;
        SpectatorRecord *grown =
            (SpectatorRecord *)realloc(r->recs,
                                       (size_t)newCap * sizeof(SpectatorRecord));
        if (grown == NULL) {
            free(copy);
            return false;
        }
        r->recs = grown;
        r->cap = newCap;
    }

    segment = (!r->hasAny) ? 0u : (newSegment ? r->curSegment + 1 : r->curSegment);

    rec = &r->recs[r->count];
    rec->gameTick = gameTick;
    rec->recordSeq = r->nextSeq;
    rec->segment = segment;
    rec->isKeyframe = isKeyframe;
    rec->payload = copy;
    rec->len = len;
    r->count++;

    r->nextSeq++;
    r->lastGameTick = gameTick;
    r->curSegment = segment;
    if (newSegment) {
        r->segmentFirstGameTick = gameTick;
        r->segmentsOpened++;
    }
    r->hasAny = true;

    spectatorRingApplyRetention(r);
    return true;
}

SpectatorRingSeekStatus spectatorRingSeekDelayed(const SpectatorRing *r,
                                                 uint32_t delayTicks,
                                                 SpectatorRingCursor *cur) {
    uint32_t head;
    uint32_t oldest;
    int64_t target;
    int targetIdx;
    uint32_t targetSeg;
    int seedIdx;
    int i;

    memset(cur, 0, sizeof(*cur));
    cur->ring = r;

    if (r->count == 0) {
        return SPECTATOR_RING_AGED_OUT;
    }
    head = r->recs[r->count - 1].recordSeq;
    oldest = r->recs[0].recordSeq;

    target = (int64_t)head - (int64_t)delayTicks;
    if (target < (int64_t)oldest) {
        /* Older than the retained range. AGED_OUT if such history once existed
         * and was dropped; COLD_START if nothing has ever aged out (the stream
         * simply is not yet old enough for this delay). */
        return r->everEvicted ? SPECTATOR_RING_AGED_OUT
                              : SPECTATOR_RING_COLD_START;
    }

    targetIdx = (int)((uint32_t)target - oldest);
    targetSeg = r->recs[targetIdx].segment;

    /* Newest keyframe at-or-before the target within the target's segment. */
    seedIdx = -1;
    for (i = targetIdx; i >= 0; i--) {
        if (r->recs[i].segment != targetSeg) {
            break; /* crossed into an earlier segment */
        }
        if (r->recs[i].isKeyframe) {
            seedIdx = i;
            break;
        }
    }
    if (seedIdx < 0) {
        /* The target's segment has no retained keyframe at-or-before it. */
        return SPECTATOR_RING_COLD_START;
    }

    cur->segment = targetSeg;
    cur->seedIdx = seedIdx;
    cur->targetIdx = targetIdx;
    cur->eventIdx = seedIdx + 1;
    return SPECTATOR_RING_OK;
}

const uint8_t *spectatorRingCursorKeyframe(const SpectatorRingCursor *cur,
                                           int *outLen, uint32_t *outGameTick) {
    const SpectatorRecord *rec = &cur->ring->recs[cur->seedIdx];

    if (outLen != NULL) {
        *outLen = rec->len;
    }
    if (outGameTick != NULL) {
        *outGameTick = rec->gameTick;
    }
    return rec->payload;
}

bool spectatorRingCursorNextEvents(SpectatorRingCursor *cur,
                                   const uint8_t **payload, int *outLen,
                                   uint32_t *outGameTick) {
    const SpectatorRecord *rec;

    if (cur->eventIdx > cur->targetIdx) {
        return false;
    }
    rec = &cur->ring->recs[cur->eventIdx];
    if (payload != NULL) {
        *payload = rec->payload;
    }
    if (outLen != NULL) {
        *outLen = rec->len;
    }
    if (outGameTick != NULL) {
        *outGameTick = rec->gameTick;
    }
    cur->eventIdx++;
    return true;
}

bool spectatorRingRecordAt(const SpectatorRing *r, uint32_t recordSeq,
                           bool *outIsKeyframe, const uint8_t **outPayload,
                           int *outLen, uint32_t *outGameTick,
                           uint32_t *outSegment) {
    uint32_t oldest;
    uint32_t idx;
    const SpectatorRecord *rec;

    if (r == NULL || r->count == 0) {
        return false;
    }
    oldest = r->recs[0].recordSeq;
    if (recordSeq < oldest) {
        return false;
    }
    idx = recordSeq - oldest;
    if (idx >= (uint32_t)r->count) {
        return false;
    }

    rec = &r->recs[idx];
    if (outIsKeyframe != NULL) {
        *outIsKeyframe = rec->isKeyframe;
    }
    if (outPayload != NULL) {
        *outPayload = rec->payload;
    }
    if (outLen != NULL) {
        *outLen = rec->len;
    }
    if (outGameTick != NULL) {
        *outGameTick = rec->gameTick;
    }
    if (outSegment != NULL) {
        *outSegment = rec->segment;
    }
    return true;
}

uint32_t spectatorRingCursorSeedSeq(const SpectatorRingCursor *cur) {
    if (cur == NULL || cur->ring == NULL) {
        return 0;
    }
    if (cur->seedIdx < 0 || cur->seedIdx >= cur->ring->count) {
        return 0;
    }
    return cur->ring->recs[cur->seedIdx].recordSeq;
}

uint32_t spectatorRingHeadSeq(const SpectatorRing *r) {
    if (r->count == 0) {
        return 0;
    }
    return r->recs[r->count - 1].recordSeq;
}

uint32_t spectatorRingOldestSeq(const SpectatorRing *r) {
    if (r->count == 0) {
        return 0;
    }
    return r->recs[0].recordSeq;
}

int spectatorRingSegmentCount(const SpectatorRing *r) {
    return r->segmentsOpened;
}
