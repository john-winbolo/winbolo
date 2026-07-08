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
 *Filename:      round_stats_derive.c
 *Author:        John Morrison
 *Purpose:
 *  The one shared derivation path. roundStatsApplyRecord folds a
 *  single attribution record into the per-round accumulator and,
 *  for combat/objective/pickup records, appends an enriched notable
 *  event. The live server (feeding records as it builds them) and
 *  the offline log viewer (replaying the parsed track) both call it,
 *  so their stats and timelines cannot diverge. computeAwards ranks
 *  the finalized accumulator into one winner per award.
 *
 *  Pure C over the public record/stats types — no sim state, no
 *  globals — so it links into the standalone log viewer, which does
 *  not link the bolo sim. The <MAX_TANKS bounds checks live here
 *  because the viewer may replay a hand-crafted file.
 *********************************************************/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "round_stats.h"
#include "round_stats_derive.h"
#include "global.h"            /* LAST_DEATH_BY_* death causes */

/* Append one enriched entry to the timeline, honouring the cap. A NULL
 * timeline (or count) means "stats only" — nothing is appended. */
static void notableAppend(NotableEvent *timeline, int *timelineCount,
                          int timelineCap, uint32_t tick, uint8_t mapX,
                          uint8_t mapY, uint8_t type, uint8_t actorA,
                          uint8_t actorB, uint8_t captureClass,
                          uint8_t deathCause, uint8_t carriedPills) {
    NotableEvent *ne;
    if (timeline == NULL || timelineCount == NULL) return;
    if (*timelineCount >= timelineCap) return;   /* full: drop silently */
    ne = &timeline[(*timelineCount)++];
    ne->tick = tick;
    ne->mapX = mapX;
    ne->mapY = mapY;
    ne->type = type;
    ne->actorA = actorA;
    ne->actorB = actorB;
    ne->captureClass = captureClass;
    ne->deathCause = deathCause;
    ne->carriedPills = carriedPills;
}

void roundStatsApplyRecord(PlayerRoundStats stats[MAX_TANKS],
                           NotableEvent *timeline, int *timelineCount,
                           int timelineCap, const void *record) {
    uint8_t recordType = *(const uint8_t *)record;

    switch (recordType) {
    case ATTR_REC_DAMAGE: {
        const AttrDamageRecord *r = (const AttrDamageRecord *)record;
        if (r->attacker >= MAX_TANKS) break;   /* owner-less/NEUTRAL splash */
        PlayerRoundStats *as = &stats[r->attacker];
        switch (r->target) {
        case ATTR_TGT_TANK: as->dmgToPlayers += r->amount; break;
        case ATTR_TGT_PILL:
            as->dmgToPills += r->amount;
            if (r->destroyed) as->pillKills++;
            break;
        case ATTR_TGT_BASE: as->dmgToBases += r->amount; break;
        default: break;
        }
        break;
    }
    case ATTR_REC_ACTION: {
        const AttrActionRecord *r = (const AttrActionRecord *)record;
        if (r->player >= MAX_TANKS) break;
        PlayerRoundStats *ps = &stats[r->player];
        switch (r->action) {
        case ATTR_ACT_FARM:  ps->treesFarmed++; break;
        case ATTR_ACT_BUILD: ps->pillsBuilt++;  break;
        case ATTR_ACT_MINE:  ps->minesLaid++;   break;
        case ATTR_ACT_SHELL: ps->shellsFired++; break;
        default: break;
        }
        break;
    }
    case ATTR_REC_KILL: {
        const AttrKillRecord *r = (const AttrKillRecord *)record;
        if (r->killed < MAX_TANKS) {
            PlayerRoundStats *vs = &stats[r->killed];
            vs->deaths++;
            if (r->deathCause == LAST_DEATH_BY_DEEPSEA) {
                vs->drowns++;
            } else if (r->deathCause == LAST_DEATH_BY_MINES) {
                vs->mineDeaths++;
            } else if (r->deathCause == LAST_DEATH_BY_SHELL) {
                if (r->killer < MAX_TANKS && r->killer != r->killed) {
                    stats[r->killer].kills++;
                    stats[r->killer].killsOf[r->killed]++;
                    vs->killedBy[r->killer]++;
                } else if (r->killer == r->killed) {
                    vs->suicides++;
                }
            }
            vs->treesWasted += r->treesWasted;
            if (r->carriedPills > vs->mostPillsDropped) {
                vs->mostPillsDropped = r->carriedPills;
            }
        }
        notableAppend(timeline, timelineCount, timelineCap, r->tick,
                      r->mapX, r->mapY, NOTABLE_KILL, r->killer, r->killed,
                      0, r->deathCause, r->carriedPills);
        break;
    }
    case ATTR_REC_CAPTURE: {
        const AttrCaptureRecord *r = (const AttrCaptureRecord *)record;
        if (r->newOwner < MAX_TANKS && r->captureClass != ATTR_CAP_ALLY) {
            PlayerRoundStats *os = &stats[r->newOwner];
            if (r->target == ATTR_CAP_TGT_PILL) os->pillCaptures++;
            else                                os->baseCaptures++;
            if (r->captureClass == ATTR_CAP_ENEMY) os->steals++;
        }
        /* An ownership change is notable even if it credits nobody. */
        notableAppend(timeline, timelineCount, timelineCap, r->tick,
                      r->mapX, r->mapY,
                      r->target == ATTR_CAP_TGT_PILL ? NOTABLE_PILL_CAPTURE
                                                     : NOTABLE_BASE_CAPTURE,
                      r->newOwner, r->prevOwner, r->captureClass, 0, 0);
        break;
    }
    case ATTR_REC_LGM: {
        const AttrLgmRecord *r = (const AttrLgmRecord *)record;
        if (r->victim < MAX_TANKS) stats[r->victim].lgmDeaths++;
        if (r->killer < MAX_TANKS) stats[r->killer].lgmKills++;
        notableAppend(timeline, timelineCount, timelineCap, r->tick,
                      r->mapX, r->mapY, NOTABLE_LGM_LOST, r->killer, r->victim,
                      0, 0, 0);
        break;
    }
    case ATTR_REC_PICKUP: {
        const AttrPickupRecord *r = (const AttrPickupRecord *)record;
        notableAppend(timeline, timelineCount, timelineCap, r->tick,
                      r->mapX, r->mapY, NOTABLE_PICKUP, r->picker, NEUTRAL,
                      0, 0, 0);
        break;
    }
    default:
        break;   /* unknown tag: ignore */
    }
}

size_t roundStatsRecordSize(uint8_t recordType) {
    switch (recordType) {
    case ATTR_REC_DAMAGE:  return sizeof(AttrDamageRecord);
    case ATTR_REC_KILL:    return sizeof(AttrKillRecord);
    case ATTR_REC_CAPTURE: return sizeof(AttrCaptureRecord);
    case ATTR_REC_LGM:     return sizeof(AttrLgmRecord);
    case ATTR_REC_ACTION:  return sizeof(AttrActionRecord);
    case ATTR_REC_PICKUP:  return sizeof(AttrPickupRecord);
    default:               return 0;   /* unknown tag: caller stops */
    }
}

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
