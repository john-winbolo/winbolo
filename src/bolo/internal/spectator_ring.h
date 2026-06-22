/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Spectator Ring
 *Filename:      spectator_ring.h
 *Author:        John Morrison
 *Purpose:
 *  A payload-agnostic ring buffer that stores a server's
 *  delayed game stream as opaque per-tick byte blobs and
 *  serves it at a delayed offset.
 *
 *  One entry is recorded per game tick. Some ticks are
 *  keyframes (a full-state blob); the rest are events (a
 *  delta blob, possibly empty). A reader positioned delay
 *  ticks behind the live head seeks to the newest keyframe
 *  at-or-before its target, then replays the events forward
 *  to the target.
 *
 *  The world periodically resets (round / map change), which
 *  restarts the game tick at 0. That boundary splits the
 *  stream into segments; a seek and its replay never cross a
 *  segment boundary. Records are dropped by age (recordSeq),
 *  never by game tick or "at reset".
 *
 *  The module is opaque about payload contents: it copies the
 *  bytes on record and hands them back unmodified. It knows
 *  nothing of the log / snapshot format, the sim, or any
 *  transport.
 *
 *  T2 (sim internals): includable within src/bolo/,
 *  src/server/, and tests/unit/ only.
 *********************************************************/

#ifndef SPECTATOR_RING_H
#define SPECTATOR_RING_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SpectatorRing SpectatorRing;

/* Create a ring.
 *   keyframeCadenceTicks - force a keyframe every N game-ticks within a
 *     segment (a keyframe is also always required at segment start,
 *     independent of N). 0 disables the periodic forcing — only segment
 *     starts then need a keyframe.
 *   retentionTicks - keep at least this many recordSeq of history behind the
 *     head.
 * Returns NULL on allocation failure. */
SpectatorRing *spectatorRingCreate(uint32_t keyframeCadenceTicks,
                                   uint32_t retentionTicks);

/* Free the ring and every payload it still holds. NULL-safe. */
void spectatorRingDestroy(SpectatorRing *r);

/* True if the caller should make THIS gameTick a keyframe: the first record
 * ever; a tick that opens a new segment (gameTick <= the last recorded
 * gameTick); or a cadence boundary within the current segment
 * ((gameTick - segmentFirstGameTick) % keyframeCadenceTicks == 0, when the
 * cadence is non-zero). */
bool spectatorRingNeedsKeyframe(const SpectatorRing *r, uint32_t gameTick);

/* Record one tick. payload is copied (the ring owns its copy); len == 0 is a
 * valid empty event and stores no payload. isKeyframe must be true when this
 * tick opens a new segment (gameTick <= the last recorded gameTick, or the
 * first record ever). Returns false without changing state when that rule is
 * violated, when len < 0, or when len > 0 with a NULL payload, or on
 * allocation failure. On success advances recordSeq and applies retention
 * eviction. */
bool spectatorRingRecordTick(SpectatorRing *r, uint32_t gameTick,
                             bool isKeyframe, const uint8_t *payload, int len);

typedef enum {
    SPECTATOR_RING_OK,         /* cursor seeded at a keyframe <= target       */
    SPECTATOR_RING_COLD_START, /* target predates the retained history and no
                                * record has ever aged out — the stream is not
                                * yet old enough to watch at this delay        */
    SPECTATOR_RING_AGED_OUT    /* target is older than the retained history,
                                * which previously held it but dropped it by
                                * age                                          */
} SpectatorRingSeekStatus;

/* A position into the stream produced by spectatorRingSeekDelayed. Treat the
 * fields as opaque; they index the ring's current storage and stay valid only
 * until the next spectatorRingRecordTick / spectatorRingDestroy. */
typedef struct {
    const SpectatorRing *ring;
    uint32_t segment;  /* segment the seed and replay stay within */
    int      seedIdx;  /* storage index of the seed keyframe      */
    int      targetIdx;/* storage index of the target record      */
    int      eventIdx; /* storage index of the next event to yield */
} SpectatorRingCursor;

/* Seek to (head recordSeq - delayTicks), saturating when delayTicks reaches
 * past the start of the stream. On SPECTATOR_RING_OK *cur is positioned at the
 * seed keyframe (the newest keyframe at-or-before the target, in the target's
 * segment); query it via the accessors below. On any other status *cur is
 * cleared and must not be queried. */
SpectatorRingSeekStatus spectatorRingSeekDelayed(const SpectatorRing *r,
                                                 uint32_t delayTicks,
                                                 SpectatorRingCursor *cur);

/* The seed keyframe payload at the cursor. outLen / outGameTick may be NULL.
 * The returned pointer is NULL only for a zero-length payload. */
const uint8_t *spectatorRingCursorKeyframe(const SpectatorRingCursor *cur,
                                           int *outLen, uint32_t *outGameTick);

/* Yield the events strictly after the seed keyframe, up to and including the
 * target, in record order, within the seed's segment. Returns false when the
 * events are exhausted. outLen / outGameTick may be NULL; *payload is NULL for
 * a zero-length event. */
bool spectatorRingCursorNextEvents(SpectatorRingCursor *cur,
                                   const uint8_t **payload, int *outLen,
                                   uint32_t *outGameTick);

/* Look up the record at an absolute recordSeq. Returns false (and leaves the
 * out-params untouched) when the ring is empty or recordSeq is outside the
 * retained range [oldestSeq, headSeq]. On true, fills the requested out-params;
 * *outPayload is NULL for a zero-length event. Any out-param may be NULL.
 * O(1): recordSeq maps directly to a storage index. */
bool spectatorRingRecordAt(const SpectatorRing *r, uint32_t recordSeq,
                           bool *outIsKeyframe, const uint8_t **outPayload,
                           int *outLen, uint32_t *outGameTick,
                           uint32_t *outSegment);

/* The recordSeq of the cursor's seed keyframe (the point a forward feed
 * resumes from after the seed). Valid on the same terms as the cursor:
 * only until the next spectatorRingRecordTick. */
uint32_t spectatorRingCursorSeedSeq(const SpectatorRingCursor *cur);

/* Newest recorded recordSeq (0 when the ring is empty). */
uint32_t spectatorRingHeadSeq(const SpectatorRing *r);

/* Oldest retained recordSeq (0 when the ring is empty). */
uint32_t spectatorRingOldestSeq(const SpectatorRing *r);

/* Total number of segments opened over the ring's lifetime. */
int spectatorRingSegmentCount(const SpectatorRing *r);

#endif /* SPECTATOR_RING_H */
