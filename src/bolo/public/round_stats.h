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
 *Name:          Round Stats / Awards
 *Filename:      round_stats.h
 *Author:        John Morrison
 *Purpose:
 *  Shared award identifiers and the per-award result record
 *  shipped to clients and to WinBolo.net at round end. The
 *  per-player accumulator (PlayerRoundStats) stays server-side;
 *  this header is the public surface that the wire codec, the
 *  client, and WBN serialization all agree on.
 *
 *  Public leaf: includes only T4 headers (global.h, gametype.h).
 *  No sim internals here.
 *********************************************************/
#ifndef ROUND_STATS_H
#define ROUND_STATS_H

#include <stdint.h>
#include "global.h"    /* MAX_TANKS, NEUTRAL */
#include "gametype.h"  /* TANK_FULL_ARMOUR — backs DMG_PER_CAPTURE */

/* Award identifiers. Append-only — never renumber; ids travel on the wire
 * and to WinBolo.net. */
typedef enum {
    AWARD_MOST_KILLS = 1, AWARD_MOST_DEATHS, AWARD_BEST_KD,
    AWARD_MOST_BASE_CAPTURES, AWARD_MOST_PILL_CAPTURES, AWARD_NEMESIS,
    AWARD_DEMOLITION, AWARD_SHARPSHOOTER, AWARD_WARMONGER, AWARD_SURVIVOR,
    AWARD_ENGINEER, AWARD_SAPPER, AWARD_FISH_FOOD, AWARD_LGM_HUNTER,
    AWARD_CANNON_FODDER, AWARD_LUMBERJACK, AWARD_WASTEFUL, AWARD_BIGGEST_FUMBLE
} AwardId;
#define AWARD_COUNT 18

/* One computed award: the winning slot and its headline number. */
typedef struct {
    uint8_t  awardId;     /* AwardId */
    uint8_t  winnerSlot;
    uint8_t  subjectSlot; /* Nemesis: the victim Y; NEUTRAL when unused */
    uint8_t  winnerIsBot;
    uint32_t value;       /* headline number; ratio awards ×100; see notes */
} AwardResult;

/* Awards tuning (tunable). */
#define DMG_PER_CAPTURE        TANK_FULL_ARMOUR  /* 40 */
#define AWARD_MIN_KILLS_KD     3
#define AWARD_MIN_SHELLS_ACC   20
#define AWARD_MIN_DMG_WARMONGER 40

#endif /* ROUND_STATS_H */
