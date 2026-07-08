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
 *  Turn a loaded log's attribution track into award lines in
 *  the events panel. Rebuilds the per-round accumulator from the
 *  parsed record stream (via the shared roundStatsApplyRecord
 *  walk), computes the awards, and emits one readable line per
 *  won award through lv_windowAddEvent. No-op when the log has
 *  no attribution track.
 *********************************************************/
#ifndef LV_STATS_H
#define LV_STATS_H

/* If the just-loaded log carries an attribution track, rebuild its per-round
 * stats and emit one events-panel line per computed award. No-op when no track
 * is present. */
void lvStatsEmitAwards(void);

#endif /* LV_STATS_H */
