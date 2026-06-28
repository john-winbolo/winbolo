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
 *Filename:      round_stats_awards.c
 *Author:        John Morrison
 *Purpose:
 *  Pure award computation over the per-round accumulator.
 *  No sim state, no globals — given the finalized
 *  PlayerRoundStats[] it ranks slots and emits one winner
 *  per award. Deterministic: ties resolve to the lowest
 *  slot index so the output is stable for tests and clients.
 *********************************************************/
#include <stdbool.h>
#include <stdint.h>

#include "server_sim_internal.h" /* PlayerRoundStats */
#include "round_stats.h"         /* AwardId, AwardResult, tuning constants */

/* A single slot's standing for one award. `key` is the (possibly signed,
 * possibly >32-bit) ranking value; `value` is the headline number emitted;
 * `subject` is the Nemesis victim (NEUTRAL otherwise); `qualifies` gates
 * whether the slot is eligible to win this award at all. */
typedef struct {
    int64_t  key;
    uint32_t value;
    uint8_t  subject;
    bool     qualifies;
} AwardCand;

/* Evaluate slot `s`'s standing for one award. `n` bounds the Nemesis
 * victim scan. */
static AwardCand evalAward(int id, const PlayerRoundStats *s, int n) {
    AwardCand c;
    c.key = 0;
    c.value = 0;
    c.subject = NEUTRAL;
    c.qualifies = false;

    switch (id) {
        case AWARD_MOST_KILLS:
            c.key = s->kills;
            c.value = s->kills;
            c.qualifies = s->kills > 0;
            break;
        case AWARD_MOST_DEATHS:
            c.key = s->deaths;
            c.value = s->deaths;
            c.qualifies = s->deaths > 0;
            break;
        case AWARD_BEST_KD: {
            uint32_t den = s->deaths ? s->deaths : 1;
            c.key = (int64_t)s->kills * 100 / den;
            c.value = (uint32_t)c.key;
            c.qualifies = s->kills >= AWARD_MIN_KILLS_KD;
            break;
        }
        case AWARD_MOST_BASE_CAPTURES:
            c.key = s->baseCaptures;
            c.value = s->baseCaptures;
            c.qualifies = s->baseCaptures > 0;
            break;
        case AWARD_MOST_PILL_CAPTURES:
            c.key = s->pillCaptures;
            c.value = s->pillCaptures;
            c.qualifies = s->pillCaptures > 0;
            break;
        case AWARD_NEMESIS: {
            uint16_t most = 0;
            int victim = NEUTRAL;
            for (int v = 0; v < n; v++) {
                if (s->killsOf[v] > most) {
                    most = s->killsOf[v];
                    victim = v;  /* strict > keeps the lowest v on ties */
                }
            }
            c.key = most;
            c.value = most;
            c.subject = (uint8_t)(most > 0 ? victim : NEUTRAL);
            c.qualifies = most > 0;
            break;
        }
        case AWARD_DEMOLITION: {
            uint64_t structDmg = s->dmgToPills + s->dmgToBases;
            c.key = (int64_t)structDmg;
            c.value = (uint32_t)structDmg;
            c.qualifies = structDmg > 0;
            break;
        }
        case AWARD_SHARPSHOOTER: {
            uint32_t den = s->shellsFired ? s->shellsFired : 1;
            c.key = (int64_t)s->dmgToPlayers * 100 / den;
            c.value = (uint32_t)c.key;
            c.qualifies = s->shellsFired >= AWARD_MIN_SHELLS_ACC && c.key > 0;
            break;
        }
        case AWARD_WARMONGER:
            c.key = (int64_t)s->dmgToPlayers
                    - (int64_t)DMG_PER_CAPTURE * (s->baseCaptures + s->pillCaptures);
            c.value = (uint32_t)s->dmgToPlayers;
            c.qualifies = s->dmgToPlayers >= AWARD_MIN_DMG_WARMONGER;
            break;
        case AWARD_SURVIVOR: {
            uint32_t num = s->treesFarmed + s->pillsBuilt + s->minesLaid;
            c.key = (int64_t)num * 100 / (s->lgmDeaths + 1);
            c.value = (uint32_t)c.key;
            c.qualifies = num > 0;
            break;
        }
        case AWARD_ENGINEER: {
            uint32_t builds = s->pillsBuilt + s->treesFarmed;
            c.key = builds;
            c.value = builds;
            c.qualifies = builds > 0;
            break;
        }
        case AWARD_SAPPER:
            c.key = s->minesLaid;
            c.value = s->minesLaid;
            c.qualifies = s->minesLaid > 0;
            break;
        case AWARD_FISH_FOOD:
            c.key = s->drowns;
            c.value = s->drowns;
            c.qualifies = s->drowns > 0;
            break;
        case AWARD_LGM_HUNTER:
            c.key = s->lgmKills;
            c.value = s->lgmKills;
            c.qualifies = s->lgmKills > 0;
            break;
        case AWARD_CANNON_FODDER:
            c.key = s->lgmDeaths;
            c.value = s->lgmDeaths;
            c.qualifies = s->lgmDeaths > 0;
            break;
        case AWARD_LUMBERJACK:
            c.key = s->treesFarmed;
            c.value = s->treesFarmed;
            c.qualifies = s->treesFarmed > 0;
            break;
        case AWARD_WASTEFUL:
            c.key = s->treesWasted;
            c.value = s->treesWasted;
            c.qualifies = s->treesWasted > 0;
            break;
        case AWARD_BIGGEST_FUMBLE:
            c.key = s->mostPillsDropped;
            c.value = s->mostPillsDropped;
            c.qualifies = s->mostPillsDropped > 0;
            break;
        default:
            break;
    }
    return c;
}

void computeAwards(const PlayerRoundStats stats[], int n, bool includeBots,
                   const bool isBot[], AwardResult out[], int *outCount) {
    int count = 0;

    for (int id = AWARD_MOST_KILLS; id <= AWARD_BIGGEST_FUMBLE; id++) {
        int bestSlot = -1;
        AwardCand best;
        best.key = 0;
        best.value = 0;
        best.subject = NEUTRAL;
        best.qualifies = false;

        for (int p = 0; p < n; p++) {
            if (!includeBots && isBot[p]) {
                continue;
            }
            AwardCand c = evalAward(id, &stats[p], n);
            if (!c.qualifies) {
                continue;
            }
            /* Strict > so the first (lowest) slot keeps the win on a tie. */
            if (bestSlot < 0 || c.key > best.key) {
                bestSlot = p;
                best = c;
            }
        }

        if (bestSlot >= 0) {
            out[count].awardId = (uint8_t)id;
            out[count].winnerSlot = (uint8_t)bestSlot;
            out[count].subjectSlot = best.subject;
            out[count].winnerIsBot = isBot[bestSlot] ? 1 : 0;
            out[count].value = best.value;
            count++;
        }
    }

    *outCount = count;
}
