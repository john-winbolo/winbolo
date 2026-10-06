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

/* One territory-control shift emitted by the influence pre-pass: at `tick`,
 * `cellsFlipped` map cells that were held by a team changed hands to `gainTeam`
 * (from `loseTeam`; NEUTRAL when a pill was shot dead and the ground fell to no
 * one), centred near (mapX,mapY). Ground taken off an opponent only — a first
 * claim of previously unheld cells is not counted, so the round populating an
 * empty map registers nothing. `recentDamage` is the total armour removed by
 * damage records within a short radius and time window just before the shift —
 * how much the ground was shelled before it fell, so a collapse can be told from
 * a quiet handover. Consumed by the turning-point and front-collapse signals. */
typedef struct {
    uint32_t tick;
    uint8_t  mapX, mapY;
    uint8_t  gainTeam;
    uint8_t  loseTeam;
    uint16_t cellsFlipped;
    uint16_t recentDamage;
} TerritoryShift;

#define TERRITORY_SHIFTS_MAX 512

/* Pre-pass over the packed record stream that turns pill/base ownership changes
 * into per-team map control. Each owned object stamps a falloff disk of
 * influence into its team's grid; the controlling team of a cell is the team
 * with the most influence there. Every ownership change re-derives control near
 * the object and emits one TerritoryShift counting the cells that changed hands.
 * Writes up to maxOut entries in stream order, sets *outCount. Pure: heap and
 * locals only, no sim state. A stream with no ownership changes yields none.
 *
 * maxOut bounds the output but not the work: an ownership change that moves no
 * ground costs the same and emits nothing. The stream is untrusted — a client
 * loads one straight off a game server or WinBolo.net — so the walk also stops
 * after a fixed ceiling of ownership changes, set far above what any real round
 * produces. A stream past that point is analysed as far as the ceiling. */
void computeTerritoryShifts(const uint8_t *records, size_t len,
                            const AttrSlotIdentity slots[MAX_TANKS],
                            int slotCount,
                            TerritoryShift *out, int *outCount, int maxOut);

/* Pure award computation over the finalized accumulator. Ranks slots 0..n-1;
 * includeBots=false skips bot slots. Writes up to AWARD_COUNT results to out[],
 * sets *outCount. No sim state touched. */
void computeAwards(const PlayerRoundStats stats[], int n, bool includeBots,
                   const bool isBot[], AwardResult out[], int *outCount);

/* Choose which of a summary's won awards to show when there are more of them
 * than there is room for. Writes up to maxOut indices into summary->awards[]
 * (indices, not ids and not copies) and returns how many it wrote; a round that
 * won maxOut or fewer gets all of them, in order, with no draw at all.
 *
 * The draw is seeded from the summary's own content and nothing else, so every
 * client holding the same summary picks the same awards — players compare
 * recaps side by side, so agreement is the point, not a nicety — and a
 * re-render of the same summary never reshuffles. At least one of the
 * negative/fun awards is kept whenever the round won one. Returned indices are
 * unique and ordered by ascending award id, matching the full list's order.
 * Pure: no allocation, no I/O, no globals. */
int roundStatsPickAwardSubset(const RoundStatsSummary *summary,
                              uint8_t *outIdx, int maxOut);

/* Same draw, over everything except the awards whose ids are in pinnedIds[] —
 * for a caller that already shows some awards outright and wants the rest of
 * its room filled without any of them coming up twice. pinnedIds may be NULL
 * when pinnedCount is 0. Seeded from the whole summary exactly as the plain
 * draw is, so the pinned set cannot change which of the rest are chosen. A
 * round with maxOut or fewer left over gets all of them, in order, with no draw
 * at all; one with none left over returns 0. */
int roundStatsPickAwardSubsetExcluding(const RoundStatsSummary *summary,
                                       const uint8_t *pinnedIds, int pinnedCount,
                                       uint8_t *outIdx, int maxOut);

/* Score a round's notable timeline into a ranked, non-overlapping top-N set of
 * highlight windows. Pure: no sim, no globals. `team[s]` is slot s's team (for
 * wipe weighting). `shifts` is the territory series from computeTerritoryShifts
 * and backs the turning-point window, which is always kept when the series is
 * non-empty; pass NULL/0 to score without it. Writes up to maxOut
 * (<= HIGHLIGHTS_MAX) windows to out[], sets *outCount. Deterministic for a
 * given input. */
void computeHighlights(const NotableEvent *timeline, int timelineCount,
                       const PlayerRoundStats stats[MAX_TANKS],
                       const uint8_t team[MAX_TANKS],
                       const AwardResult *awards, int awardCount,
                       const TerritoryShift *shifts, int shiftCount,
                       HighlightWindow *out, int *outCount, int maxOut);

/* ── The recap's scenario table, grouped by team ────────────────────────
 *
 * A scenario that scores both teams and players (game.score with {team=n}
 * and with a seat) gets a recap table with a row for each team it scored and
 * the team's members under it. A scenario that scored only one of the two
 * keeps the plain table of players. */

/* Whether the recap groups this summary by team: a scenario set at least one
 * team score and at least one player score. */
bool roundStatsGroupsByTeam(const RoundStatsSummary *summary);

#define ROUND_STATS_ROW_TEAM   1
#define ROUND_STATS_ROW_PLAYER 2

typedef struct {
    uint8_t kind;   /* ROUND_STATS_ROW_TEAM or ROUND_STATS_ROW_PLAYER */
    uint8_t team;   /* the team number of a team row and of its members; 0 for
                       a player row on no scored team */
    uint8_t index;  /* a player row's index into summary->players[]; 0 for a
                       team row */
} RoundStatsGroupRow;

/* The grouped table's rows, in order. Every team the scenario scored comes
 * first, best team score first (a tie goes to the lower team number), each
 * followed by its members; players on no scored team follow, as rows of
 * their own. Players, in a team and among the rest, are ordered by their own
 * scenario score, best first, with a player the scenario never scored below
 * every scored one, and then by kills (more first), deaths (fewer first) and
 * slot. teamOfSlot[s] is slot s's team (0 for no team). Writes at most outCap
 * rows (2 * MAX_TANKS always fits) and returns how many it wrote. Pure and
 * deterministic. */
int roundStatsGroupRows(const RoundStatsSummary *summary,
                        const uint8_t teamOfSlot[MAX_TANKS],
                        RoundStatsGroupRow *out, int outCap);

#endif /* ROUND_STATS_DERIVE_H */
