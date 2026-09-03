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

/* Post-game stats/awards: the wire publish (server) and the lobby recap
 * UI (client). Were withheld (0) for the 2.02 release; enabled (1) on the
 * bot-improvements line 2026-09-03 so LAN test games end with the
 * scoreboard + awards in the lobby. The codec is compiled either way, so a
 * client built with 0 just stores the packet and shows nothing. */
#define POSTGAME_STATS_ENABLED 1

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

/* End-of-round summary shipped to clients: a curated per-player scoreboard
 * row plus the won awards. The full PlayerRoundStats accumulator stays
 * server-side; only this subset crosses the wire. */
#define ROUND_STATS_LOGKEY_LEN 33   /* mirrors WINBOLONET_KEY_LEN */

typedef struct {
    uint8_t  slot;
    uint8_t  isBot;
    uint16_t kills;
    uint16_t deaths;
    uint16_t baseCaptures;
    uint16_t pillCaptures;
    uint32_t dmgDealt;   /* dmgToPlayers + dmgToPills + dmgToBases */
    uint16_t builds;     /* pillsBuilt + treesFarmed */
    uint16_t lgmKills;
    uint16_t lgmDeaths;
} RoundPlayerSummary;

typedef struct {
    uint8_t  playerCount;                    /* present slots, <= MAX_TANKS */
    RoundPlayerSummary players[MAX_TANKS];
    uint8_t  awardCount;                     /* <= AWARD_COUNT */
    AwardResult awards[AWARD_COUNT];
    char     wbnLogKey[ROUND_STATS_LOGKEY_LEN]; /* finished round's WBN log key; "" if none */
} RoundStatsSummary;

/* Awards tuning (tunable). */
#define DMG_PER_CAPTURE        TANK_FULL_ARMOUR  /* 40 */
#define AWARD_MIN_KILLS_KD     3
#define AWARD_MIN_SHELLS_ACC   20
#define AWARD_MIN_DMG_WARMONGER 40

#endif /* ROUND_STATS_H */
