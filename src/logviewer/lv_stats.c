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
 *Name:          Log Viewer Round Stats
 *Filename:      lv_stats.c
 *Purpose:
 *  Rebuild a loaded log's per-round stats and notable timeline
 *  from its attribution track, then emit the computed awards and
 *  the selected highlights as events-panel lines.
 *********************************************************/

#include "lv_stats.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "attribution_track.h"       /* AttrTrackHeader, AttrSlotIdentity */
#include "backend.h"                 /* lv_windowAddSummary, lv_windowAddHighlight */
#include "lv_attribution.h"          /* lvAttributionGetHeader / GetRecords */
#include "round_stats.h"             /* PlayerRoundStats, AwardResult, HighlightWindow */
#include "round_stats_derive.h"      /* roundStatsApplyRecord, computeAwards, computeHighlights */

/* Cap on highlight lines shown per loaded log. */
#define LV_HIGHLIGHTS_SHOWN 12

/* Award id -> display name, indexed by AwardId (1..AWARD_COUNT); slot 0 unused.
 * A local table keeps this self-contained: the standalone viewer does not link
 * the client's STR_* localization. */
static const char *const kAwardNames[AWARD_COUNT + 1] = {
    "",                 /* 0: unused */
    "Most Kills",       /* AWARD_MOST_KILLS */
    "Most Deaths",      /* AWARD_MOST_DEATHS */
    "Best K/D",         /* AWARD_BEST_KD */
    "Most Base Captures",/* AWARD_MOST_BASE_CAPTURES */
    "Most Pill Captures",/* AWARD_MOST_PILL_CAPTURES */
    "Nemesis",          /* AWARD_NEMESIS */
    "Demolition",       /* AWARD_DEMOLITION */
    "Sharpshooter",     /* AWARD_SHARPSHOOTER */
    "Warmonger",        /* AWARD_WARMONGER */
    "Survivor",         /* AWARD_SURVIVOR */
    "Engineer",         /* AWARD_ENGINEER */
    "Sapper",           /* AWARD_SAPPER */
    "Fish Food",        /* AWARD_FISH_FOOD */
    "LGM Hunter",       /* AWARD_LGM_HUNTER */
    "Cannon Fodder",    /* AWARD_CANNON_FODDER */
    "Lumberjack",       /* AWARD_LUMBERJACK */
    "Wasteful",         /* AWARD_WASTEFUL */
    "Biggest Fumble"    /* AWARD_BIGGEST_FUMBLE */
};

/* Best K/D, Sharpshooter and Survivor carry their headline number as a ×100
 * fixed-point ratio (see computeAwards); the rest are plain counts. */
static bool awardValueIsRatio(uint8_t awardId) {
  return awardId == AWARD_BEST_KD || awardId == AWARD_SHARPSHOOTER ||
         awardId == AWARD_SURVIVOR;
}

/* Write a display name for a slot into out (always NUL-terminated). Slot names
 * from the track are bounded by PACKET_MAX_PLAYER_NAME and may not be
 * NUL-terminated, so print length-bounded; an empty name falls back to the
 * name the log itself recorded for the slot, then to "Slot N".
 *
 * `slot` is a record or highlight actor byte, so it reaches 255 whatever the
 * track says. slots[] is MAX_TANKS long, so the index is held to that as well
 * as to slotCount — the parser clamps slotCount, and this stays correct on its
 * own if a second reader of these headers ever forgets to. */
static void formatSlotName(const AttrTrackHeader *header, int slot, char *out,
                           size_t outSize) {
  char fromLog[PLAYER_NAME_LEN];
  if (slot >= 0 && slot < MAX_TANKS && slot < header->slotCount &&
      header->slots[slot].name[0] != '\0') {
    snprintf(out, outSize, "%.*s", (int)PACKET_MAX_PLAYER_NAME,
             header->slots[slot].name);
    return;
  }
  /* The track's identity table is a game-over snapshot, so a player who left
   * mid-round has no entry in it. The log's own join events still name them.
   * This reports the last name the log recorded for that slot, so a slot
   * reused by a second player names the later one. */
  if (slot >= 0 && slot < MAX_TANKS &&
      lv_screenGetLoggedPlayerName((BYTE)slot, fromLog, sizeof(fromLog))) {
    snprintf(out, outSize, "%s", fromLog);
    return;
  }
  snprintf(out, outSize, "Slot %d", slot);
}

void lvStatsFormatClipTime(uint32_t ms, char *out, size_t outSize) {
  uint32_t secs = ms / 1000u;
  snprintf(out, outSize, "%u:%02u", secs / 60u, secs % 60u);
}

void lvStatsEmitRoundSummary(void) {
  const AttrTrackHeader *header;
  const uint8_t *records;
  size_t len;
  uint32_t count;
  PlayerRoundStats stats[MAX_TANKS];
  NotableEvent timeline[NOTABLE_EVENTS_MAX];
  int timelineCount;
  bool isBot[MAX_TANKS];
  uint8_t team[MAX_TANKS];
  AwardResult awards[AWARD_COUNT];
  int awardCount;
  HighlightWindow hl[HIGHLIGHTS_MAX];
  int hlCount;
  TerritoryShift shifts[TERRITORY_SHIFTS_MAX];
  int shiftCount;
  uint32_t gameStartMs;
  uint32_t windowStartMs;
  double calA, calB;
  size_t off;
  int i;

  header = lvAttributionGetHeader();
  if (header == NULL) {
    /* Old log or logging was off: no track, nothing to show. */
    return;
  }

  records = lvAttributionGetRecords(&len, &count);

  /* Rebuild the per-round accumulator and the notable timeline in one pass over
   * the packed record stream. Walk by offset: read the tag, size it, and stop on
   * an unknown tag or an overrun so we never read past the stream. */
  memset(stats, 0, sizeof(stats));
  timelineCount = 0;
  off = 0;
  while (records != NULL && off < len) {
    uint8_t tag = records[off];
    size_t sz = roundStatsRecordSize(tag);
    if (sz == 0 || off + sz > len) {
      break;
    }
    roundStatsApplyRecord(stats, timeline, &timelineCount, NOTABLE_EVENTS_MAX,
                          records + off);
    off += sz;
  }

  for (i = 0; i < MAX_TANKS; i++) {
    isBot[i] = (i < header->slotCount) ? (header->slots[i].isBot != 0) : false;
    team[i] = (i < header->slotCount) ? header->slots[i].team : 0;
  }

  /* Second walk over the same records, tracking pill/base ownership as per-team
   * map influence, so the highlight scorer can see when the map swung. */
  shiftCount = 0;
  computeTerritoryShifts(records, len, header->slots, header->slotCount, shifts,
                         &shiftCount, TERRITORY_SHIFTS_MAX);

  awardCount = 0;
  computeAwards(stats, MAX_TANKS, /*includeBots*/ true, isBot, awards,
                &awardCount);

  for (i = 0; i < awardCount; i++) {
    const AwardResult *a = &awards[i];
    const char *name = (a->awardId <= AWARD_COUNT) ? kAwardNames[a->awardId]
                                                   : "Award";
    char winner[PACKET_MAX_PLAYER_NAME + 16];
    char line[256];

    formatSlotName(header, a->winnerSlot, winner, sizeof(winner));

    if (a->awardId == AWARD_NEMESIS) {
      char victim[PACKET_MAX_PLAYER_NAME + 16];
      formatSlotName(header, a->subjectSlot, victim, sizeof(victim));
      snprintf(line, sizeof(line), "%s: %s%s \xe2\x80\x94 %u kills of %s", name,
               winner, a->winnerIsBot ? " (bot)" : "", a->value, victim);
    } else if (awardValueIsRatio(a->awardId)) {
      snprintf(line, sizeof(line), "%s: %s%s \xe2\x80\x94 %u.%02u", name, winner,
               a->winnerIsBot ? " (bot)" : "", a->value / 100u, a->value % 100u);
    } else {
      snprintf(line, sizeof(line), "%s: %s%s \xe2\x80\x94 %u", name, winner,
               a->winnerIsBot ? " (bot)" : "", a->value);
    }

    lv_windowAddSummary(line);
  }

  /* Score the timeline into a ranked, non-overlapping set of clips and emit one
   * line per selection; the panel prefixes each with its own clip time. Only
   * HL_AWARD, HL_CLUSTER_WIPE, HL_OBJECTIVE_STEAL, HL_MULTI_LGM, HL_FUMBLE,
   * HL_RARE_DEATH, HL_PICKUP_SPREE, HL_ACTION_DENSITY, HL_BREAKTHROUGH and
   * HL_TURNING_POINT are produced today; other types get a safe generic label
   * until their signals come online. */
  hlCount = 0;
  computeHighlights(timeline, timelineCount, stats, team, awards, awardCount,
                    shifts, shiftCount, hl, &hlCount, LV_HIGHLIGHTS_SHOWN);

  gameStartMs = lv_screenGameStartMs();
  calA = 10.0;
  calB = (double)gameStartMs;

  /* Map an attribution tick to scrubber ms as a linear clip ms = calA*tick+calB.
   * The sim tick is game-relative and does not run at a clean ratio to the log
   * clock, so calibrate it per log: pair the first and last base captures to
   * their real base-ownership changes in the log and fit the line. Fall back to
   * the raw game-start offset (tick as 10 ms) when calibration isn't available
   * (e.g. an old-format log or no base captures). */
  {
    int firstBase = -1, lastBase = -1;
    for (i = 0; i < timelineCount; i++) {
      if (timeline[i].type == NOTABLE_BASE_CAPTURE) {
        if (firstBase < 0) firstBase = i;
        lastBase = i;
      }
    }
    if (firstBase >= 0 && lastBase > firstBase) {
      /* Identify each anchor to the walker as the ordinal-th ownership gain
       * at its cell by its capturer — the capture records and the log's
       * owner-gain events are 1:1 up to game over (allied captures are
       * recorded too), so counting occurrences in the timeline pins the
       * exact log event even when the cell changes hands again later (the
       * game-over handover re-assigns every base with no capture record). */
      const NotableEvent *fb = &timeline[firstBase];
      const NotableEvent *lb = &timeline[lastBase];
      int ordE = 0, ordL = 0;
      uint32_t msE = 0, msL = 0;
      for (i = 0; i <= lastBase; i++) {
        if (timeline[i].type != NOTABLE_BASE_CAPTURE) continue;
        if (i <= firstBase && timeline[i].mapX == fb->mapX &&
            timeline[i].mapY == fb->mapY && timeline[i].actorA == fb->actorA) {
          ordE++;
        }
        if (timeline[i].mapX == lb->mapX && timeline[i].mapY == lb->mapY &&
            timeline[i].actorA == lb->actorA) {
          ordL++;
        }
      }
      if (lv_walkFindBaseOwnerTimes(fb->mapX, fb->mapY, fb->actorA, ordE,
                                    lb->mapX, lb->mapY, lb->actorA, ordL,
                                    &msE, &msL)) {
        uint32_t tE = fb->tick, tL = lb->tick;
        if (tL > tE && msL > msE) {
          calA = (double)(msL - msE) / (double)(tL - tE);
          calB = (double)msE - calA * (double)tE;
        }
      }
    }
  }

  if (hlCount > 0) {
    lv_windowAddSummary("Highlights:");
  }

  /* The fit above lands in absolute log ms; the clip's own field is measured
   * from the start of the presented window, the same origin the server's
   * round-relative ms use. Fill it here and read it back below, so the two
   * producers of a HighlightWindow hand their consumers one field with one
   * meaning. windowStart is 0 whenever the window is the whole file, which
   * makes the two forms the same number. */
  windowStartMs = lv_screenWindowStartMs();

  for (i = 0; i < hlCount; i++) {
    HighlightWindow *h = &hl[i];
    char actorA[PACKET_MAX_PLAYER_NAME + 16];
    char line[256];
    uint32_t clipMs = (uint32_t)(calA * (double)h->startTick + calB);

    h->startMs = (clipMs > windowStartMs) ? (clipMs - windowStartMs) : 0u;
    h->durationMs = (uint32_t)(calA * (double)h->durationTicks);

    formatSlotName(header, h->actorA, actorA, sizeof(actorA));

    switch (h->type) {
    case HL_AWARD: {
      const char *name = (h->awardId <= AWARD_COUNT) ? kAwardNames[h->awardId]
                                                     : "Award";
      if (h->awardId == AWARD_NEMESIS) {
        char actorB[PACKET_MAX_PLAYER_NAME + 16];
        formatSlotName(header, h->actorB, actorB, sizeof(actorB));
        snprintf(line, sizeof(line), "%s (%s vs %s)", name, actorA, actorB);
      } else {
        snprintf(line, sizeof(line), "%s (%s)", name, actorA);
      }
      break;
    }
    case HL_CLUSTER_WIPE:
      snprintf(line, sizeof(line), "Team wipe: %u down (%s)", h->value, actorA);
      break;
    case HL_OBJECTIVE_STEAL: {
      char actorB[PACKET_MAX_PLAYER_NAME + 16];
      formatSlotName(header, h->actorB, actorB, sizeof(actorB));
      snprintf(line, sizeof(line), "Steal (%s from %s)", actorA, actorB);
      break;
    }
    case HL_MULTI_LGM:
      snprintf(line, sizeof(line), "LGM sweep: %u down (%s)", h->value, actorA);
      break;
    case HL_FUMBLE:
      snprintf(line, sizeof(line), "Fumble: %u pills dropped (%s)", h->value,
               actorA);
      break;
    case HL_RARE_DEATH:
      if (h->value > 0) {
        snprintf(line, sizeof(line), "Drowned with %u pills (%s)", h->value,
                 actorA);
      } else {
        snprintf(line, sizeof(line), "Drowned (%s)", actorA);
      }
      break;
    case HL_PICKUP_SPREE:
      snprintf(line, sizeof(line), "Pill sweep: %u grabbed (%s)", h->value,
               actorA);
      break;
    case HL_ACTION_DENSITY:
      /* A busy stretch of the round is nobody's, so the line names no player —
       * both actors are NEUTRAL on this type. */
      snprintf(line, sizeof(line), "All-out action: %u events", h->value);
      break;
    case HL_BREAKTHROUGH:
      /* Ground taken off a team belongs to the team that took it; actorA only
       * names a slot standing in for it, and is NEUTRAL when the gaining team
       * has no one to point at. */
      if (h->actorA < header->slotCount) {
        snprintf(line, sizeof(line), "Front collapse: %u tiles taken (%s)",
                 h->value, actorA);
      } else {
        snprintf(line, sizeof(line), "Front collapse: %u tiles taken", h->value);
      }
      break;
    case HL_TURNING_POINT:
      /* A swing belongs to a team; actorA only names a slot standing in for it,
       * and is NEUTRAL when the gaining team has no one to point at. */
      if (h->actorA < header->slotCount) {
        snprintf(line, sizeof(line), "Turning point: %u tiles swung (%s)",
                 h->value, actorA);
      } else {
        snprintf(line, sizeof(line), "Turning point: %u tiles swung", h->value);
      }
      break;
    default:
      snprintf(line, sizeof(line), "Highlight");
      break;
    }

    /* Emit as a clickable clip: clicking jumps the scrubber a few seconds ahead
     * of the moment and centres the map on it. The panel seeks in absolute log
     * ms, so the window origin goes back on here. */
    lv_windowAddHighlight(line, h->startMs + windowStartMs, h->mapX, h->mapY);
  }
}
