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
 *  shipped to clients and to WinBolo.net at round end, plus the
 *  per-player accumulator (PlayerRoundStats) and the notable-event
 *  timeline both the live server and the offline log viewer rebuild
 *  from the attribution records. This header is the public surface
 *  that the wire codec, the client, the shared derivation, and WBN
 *  serialization all agree on.
 *
 *  Public leaf: includes only T4 headers (global.h, gametype.h).
 *  No sim internals here.
 *********************************************************/
#ifndef ROUND_STATS_H
#define ROUND_STATS_H

/* Post-game stats/awards: the wire publish (server) and the lobby recap
 * UI (client) are built but withheld from this release. Set to 1 to
 * surface them. */
#define POSTGAME_STATS_ENABLED 1

#include <stdint.h>
#include "global.h"    /* MAX_TANKS, NEUTRAL */
#include "gametype.h"  /* TANK_FULL_ARMOUR — backs DMG_PER_CAPTURE */

/* Per-player per-round gameplay stats. A projection of the attribution
 * records: both the live server and the offline log viewer build this by
 * feeding records through roundStatsApplyRecord (round_stats_derive.h). A
 * curated subset ships to clients (RoundPlayerSummary); the full struct also
 * backs the WBN round summary. */
typedef struct {
    uint32_t kills, deaths, drowns, suicides, mineDeaths;
    uint32_t lgmKills, lgmDeaths;
    uint32_t pillCaptures, pillKills, baseCaptures, steals;
    uint32_t treesFarmed, treesWasted, pillsBuilt, minesLaid;
    uint32_t shellsFired;
    uint8_t  mostPillsDropped;      /* max pills dumped at a single death */
    uint64_t dmgToPlayers, dmgToPills, dmgToBases;
    uint16_t killedBy[MAX_TANKS];   /* killedBy[k] = times killer slot k killed me */
    uint16_t killsOf[MAX_TANKS];    /* killsOf[v]  = times I killed victim slot v */
} PlayerRoundStats;

/* NotableEvent.type values — the ordered round timeline consumed by the
 * highlights reel and the log viewer. */
typedef enum {
    NOTABLE_KILL = 0,
    NOTABLE_PILL_CAPTURE,
    NOTABLE_BASE_CAPTURE,
    NOTABLE_LGM_LOST,
    NOTABLE_PICKUP            /* dead-pill scoop; actorA = picker */
} NotableType;

/* One entry in the per-round notable timeline. Enrichment fields are sourced
 * from the attribution records and stored as plain bytes (not the Attr* enum
 * types, to keep this a public leaf); each is 0 when it does not apply to the
 * event type. */
#define NOTABLE_EVENTS_MAX 512
typedef struct {
    uint32_t tick;           /* per-round running tick */
    uint8_t  mapX, mapY;     /* map cell of the event */
    uint8_t  type;           /* NotableType */
    uint8_t  actorA, actorB;
    uint8_t  captureClass;   /* captures: AttrCaptureClass value; 0 otherwise */
    uint8_t  deathCause;     /* kills: GameEvent deathCause; 0 otherwise      */
    uint8_t  carriedPills;   /* kills: pills dumped at death; 0 otherwise     */
} NotableEvent;

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

/* Highlight clip category. Append-only — ids are frozen; never renumber. Some
 * values are produced by later scorer passes (pickup spree, revenge, …) but are
 * defined now so the enum never renumbers. */
typedef enum {
    HL_AWARD = 1,        /* anchored on an award-winning moment; awardId set */
    HL_CLUSTER_WIPE,     /* multiple deaths close in time and space           */
    HL_OBJECTIVE_STEAL,  /* enemy-owned pill/base captured (a steal)          */
    HL_MULTI_LGM,        /* several of a team's men cut down in one sweep     */
    HL_FUMBLE,           /* a death that dumped a load of carried pills       */
    HL_RARE_DEATH,       /* a drowning                                        */
    HL_PICKUP_SPREE,     /* (later scorer pass) */
    HL_MULTI_CAPTURE,    /* (later scorer pass) */
    HL_ACTION_DENSITY,   /* (later scorer pass) */
    HL_REVENGE,          /* (later scorer pass) */
    HL_BREAKTHROUGH,     /* a front collapse: ground taken off a team under fire */
    HL_TURNING_POINT     /* the round's single largest map-control swing */
} HighlightType;

/* One selected highlight window. tick fields are per-round (log-relative). value
 * is signal-specific (deaths in a wipe, award value for an anchor, …); score is
 * the internal ranking magnitude (not shipped on the wire). awardId is the AwardId
 * when type==HL_AWARD, else 0.
 *
 * Two clocks live here and they are not the same clock. startTick/durationTicks
 * are the scorer's own units — sim->tick, which runs at two ticks per 20 ms
 * frame while a round is running — and they are what round_stats_derive.c
 * computes in and what lv_stats.c calibrates against. startMs/durationMs are
 * round-relative milliseconds for a consumer that just wants a time.
 *
 * computeHighlights fills only the tick fields and leaves the ms fields zero:
 * it has the ticks but not the origin or the rate they convert at. Whoever
 * produces a summary fills them — the server from its latched round-log start
 * tick, the log viewer from its per-log calibration — so do not assume they
 * arrive populated from anything that only ran the scorer. */
typedef struct {
    uint32_t startTick;
    uint32_t durationTicks;
    uint32_t startMs;       /* round-relative ms; producer-filled, 0 from the scorer */
    uint32_t durationMs;    /* ditto */
    uint8_t  mapX, mapY;
    uint8_t  type;          /* HighlightType */
    uint8_t  awardId;       /* AwardId when HL_AWARD, else 0 */
    uint8_t  actorA, actorB;
    uint32_t value;
    uint32_t score;
} HighlightWindow;

#define HIGHLIGHTS_MAX 32

/* Clips carried by RoundStatsSummary — what the lobby recap lists, which is
 * fewer than the scorer can select. Must stay <= HIGHLIGHTS_MAX. */
#define ROUND_STATS_HIGHLIGHTS_WIRE_MAX 12

/* End-of-round summary shipped to clients: a curated per-player scoreboard
 * row, the won awards, and the round's highlight clips. The full
 * PlayerRoundStats accumulator stays server-side; only this subset crosses
 * the wire. */
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
    uint8_t  highlightCount;                 /* <= ROUND_STATS_HIGHLIGHTS_WIRE_MAX */
    HighlightWindow highlights[ROUND_STATS_HIGHLIGHTS_WIRE_MAX];
} RoundStatsSummary;

/* Awards tuning (tunable). */
#define DMG_PER_CAPTURE        TANK_FULL_ARMOUR  /* 40 */
#define AWARD_MIN_KILLS_KD     3
#define AWARD_MIN_SHELLS_ACC   20
#define AWARD_MIN_DMG_WARMONGER 40

#endif /* ROUND_STATS_H */
