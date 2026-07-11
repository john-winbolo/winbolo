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
 *Name:          Log Viewer Round Stats
 *Filename:      lv_stats.h
 *Purpose:
 *  Turn a loaded log's attribution track into award and highlight
 *  lines in the events panel. Rebuilds the per-round accumulator
 *  and the notable timeline from the parsed record stream (via the
 *  shared roundStatsApplyRecord walk), computes the awards and the
 *  selected highlights, and emits one readable line each through
 *  lv_windowAddEvent. No-op when the log has no attribution track.
 *********************************************************/
#ifndef LV_STATS_H
#define LV_STATS_H

#include <stddef.h>
#include <stdint.h>

/* If the just-loaded log carries an attribution track, rebuild its per-round
 * stats and notable timeline, then emit one events-panel line per computed
 * award followed by one per selected highlight. No-op when no track is
 * present. */
void lvStatsEmitRoundSummary(void);

/* Format an absolute clip time (log playback ms) as m:ss. Pure; always
 * NUL-terminates out. */
void lvStatsFormatClipTime(uint32_t ms, char *out, size_t outSize);

#endif /* LV_STATS_H */
