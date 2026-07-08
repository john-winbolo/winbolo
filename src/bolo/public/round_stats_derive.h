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
 *Name:          Round Stats Derivation
 *Filename:      round_stats_derive.h
 *Author:        John Morrison
 *Purpose:
 *  The shared derivation seam: one pure function that turns an
 *  attribution record into accumulator + timeline updates, so the
 *  live server and the offline log viewer derive identical stats
 *  from the same records and cannot drift.
 *
 *  Dependency-light: only the public record types
 *  (attribution_track.h) and the public accumulator/timeline types
 *  (round_stats.h). No sim internals, no SDL — so it links into the
 *  standalone log viewer, which does not link the bolo sim.
 *********************************************************/
#ifndef ROUND_STATS_DERIVE_H
#define ROUND_STATS_DERIVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "round_stats.h"       /* PlayerRoundStats, NotableEvent, AwardResult */
#include "attribution_track.h" /* Attr*Record, AttrRecordType */

/* Apply one attribution record (tag-dispatched on its leading `type` byte) to
 * the accumulator, and — for KILL/CAPTURE/LGM/PICKUP — append one enriched
 * NotableEvent. Defensive: ignores out-of-range slots and unknown tags; appends
 * nothing once *timelineCount == timelineCap. `timeline`/`timelineCount` may be
 * NULL to update stats only. */
void roundStatsApplyRecord(PlayerRoundStats stats[MAX_TANKS],
                           NotableEvent *timeline, int *timelineCount,
                           int timelineCap, const void *record);

/* Byte size of a record with the given AttrRecordType tag, for walking a packed
 * stream; 0 for an unknown tag (so a caller can stop safely). */
size_t roundStatsRecordSize(uint8_t recordType);

/* Pure award computation over the finalized accumulator. Ranks slots 0..n-1;
 * includeBots=false skips bot slots. Writes up to AWARD_COUNT results to out[],
 * sets *outCount. No sim state touched. */
void computeAwards(const PlayerRoundStats stats[], int n, bool includeBots,
                   const bool isBot[], AwardResult out[], int *outCount);

/* Score a round's notable timeline into a ranked, non-overlapping top-N set of
 * highlight windows. Pure: no sim, no globals. `team[s]` is slot s's team (for
 * wipe weighting). Writes up to maxOut (<= HIGHLIGHTS_MAX) windows to out[], sets
 * *outCount. Deterministic for a given input. */
void computeHighlights(const NotableEvent *timeline, int timelineCount,
                       const PlayerRoundStats stats[MAX_TANKS],
                       const uint8_t team[MAX_TANKS],
                       const AwardResult *awards, int awardCount,
                       HighlightWindow *out, int *outCount, int maxOut);

#endif /* ROUND_STATS_DERIVE_H */
