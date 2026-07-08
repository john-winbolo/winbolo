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
 *Filename:      lv_stats.c
 *Purpose:
 *  Rebuild a loaded log's per-round stats from its attribution
 *  track and emit the computed awards as events-panel lines.
 *********************************************************/

#include "lv_stats.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "attribution_track.h"       /* AttrTrackHeader, AttrSlotIdentity */
#include "backend.h"                 /* lv_windowAddEvent */
#include "lv_attribution.h"          /* lvAttributionGetHeader / GetRecords */
#include "round_stats.h"             /* PlayerRoundStats, AwardResult, AwardId */
#include "round_stats_derive.h"      /* roundStatsApplyRecord, computeAwards */

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
 * NUL-terminated, so print length-bounded; an empty name falls back to "Slot N". */
static void formatSlotName(const AttrTrackHeader *header, int slot, char *out,
                           size_t outSize) {
  if (slot >= 0 && slot < header->slotCount &&
      header->slots[slot].name[0] != '\0') {
    snprintf(out, outSize, "%.*s", (int)PACKET_MAX_PLAYER_NAME,
             header->slots[slot].name);
  } else {
    snprintf(out, outSize, "Slot %d", slot);
  }
}

void lvStatsEmitAwards(void) {
  const AttrTrackHeader *header;
  const uint8_t *records;
  size_t len;
  uint32_t count;
  PlayerRoundStats stats[MAX_TANKS];
  bool isBot[MAX_TANKS];
  AwardResult awards[AWARD_COUNT];
  int awardCount;
  size_t off;
  int i;

  header = lvAttributionGetHeader();
  if (header == NULL) {
    /* Old log or logging was off: no track, nothing to show. */
    return;
  }

  records = lvAttributionGetRecords(&len, &count);

  /* Rebuild the per-round accumulator from the packed record stream. Walk by
   * offset: read the tag, size it, and stop on an unknown tag or an overrun so
   * we never read past the stream. */
  memset(stats, 0, sizeof(stats));
  off = 0;
  while (records != NULL && off < len) {
    uint8_t tag = records[off];
    size_t sz = roundStatsRecordSize(tag);
    if (sz == 0 || off + sz > len) {
      break;
    }
    roundStatsApplyRecord(stats, NULL, NULL, 0, records + off);
    off += sz;
  }

  for (i = 0; i < MAX_TANKS; i++) {
    isBot[i] = (i < header->slotCount) ? (header->slots[i].isBot != 0) : false;
  }

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

    lv_windowAddEvent(0, line);
  }
}
