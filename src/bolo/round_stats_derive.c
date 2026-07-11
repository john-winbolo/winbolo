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
#include <stdlib.h>
#include <string.h>

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

/* ---- Highlight scorer ---------------------------------------------------- */

/* Tunable weights and window sizes. Ticks are per-round at 50 ticks/s. */
#define HL_WIPE_MIN_DEATHS       3
#define HL_WIPE_TICK_WINDOW      150   /* ~3 s span for a cluster of deaths */
#define HL_WIPE_TILE_RADIUS      6     /* Chebyshev radius from the first death */
#define HL_WIPE_WEIGHT           100   /* per death in the cluster */
#define HL_WIPE_TEAM_BONUS       50    /* added when all the dead share a team */
#define HL_STEAL_WEIGHT          60
#define HL_STEAL_ENDGAME_BONUS   60    /* scaled by how late in the round it lands */
#define HL_STEAL_DEDUP_TICKS     500   /* one steal clip per contested cell in this span */
#define HL_AWARD_WEIGHT          120   /* award anchors are highest-confidence */
#define HL_LEADIN_TILE_RADIUS    6
#define HL_LEADIN_MAX_GAP        60    /* stop the lead-in on a gap this big */
#define HL_LEADIN_MAX_TICKS      250   /* cap on how far a lead-in reaches back */
#define HL_CLIP_TICKS            250   /* window duration around a single anchor */

/* Upper bound on candidate windows collected before selection. Bounded by the
 * timeline length (one steal / wipe start per event) plus the awards. */
#define HL_CAND_MAX              (NOTABLE_EVENTS_MAX * 2 + AWARD_COUNT)

/* A scored candidate window before selection. `endTick` is the selection span
 * end (the last clustered event for a wipe, else the anchor tick); lead-in and
 * the display duration are derived from `anchorIdx` at finalize time. */
typedef struct {
    uint32_t startTick;
    uint32_t endTick;
    int      anchorIdx;
    uint8_t  mapX, mapY;
    uint8_t  type;
    uint8_t  awardId;
    uint8_t  actorA, actorB;
    uint32_t value;
    uint32_t score;
} HlCand;

/* Chebyshev tile distance, so a square radius reads as one threshold. */
static int hlTileDist(uint8_t ax, uint8_t ay, uint8_t bx, uint8_t by) {
    int dx = (int)ax - (int)bx; if (dx < 0) dx = -dx;
    int dy = (int)ay - (int)by; if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;
}

static void hlAppend(HlCand *cands, int *n, const HlCand *c) {
    if (*n >= HL_CAND_MAX) return;   /* full: drop silently */
    cands[(*n)++] = *c;
}

/* Timeline index of the event that best represents an award, or -1 when the
 * award has no moment to anchor (builder/aggregate stats leave no timeline
 * event). Kill-count and capture awards prefer the winner's last such moment. */
static int hlFindAwardAnchor(const NotableEvent *tl, int n, const AwardResult *aw) {
    int best = -1;
    switch (aw->awardId) {
    case AWARD_NEMESIS:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL &&
                tl[i].actorA == aw->winnerSlot && tl[i].actorB == aw->subjectSlot)
                best = i;   /* last kill of the nemesis victim */
        break;
    case AWARD_BIGGEST_FUMBLE: {
        int most = -1;
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL && tl[i].actorB == aw->winnerSlot &&
                (int)tl[i].carriedPills > most) {
                most = tl[i].carriedPills;   /* the biggest pill dump they died on */
                best = i;
            }
        break;
    }
    case AWARD_FISH_FOOD:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL && tl[i].actorB == aw->winnerSlot &&
                tl[i].deathCause == LAST_DEATH_BY_DEEPSEA)
                best = i;   /* last drowning */
        break;
    case AWARD_MOST_KILLS:
    case AWARD_BEST_KD:
        /* The winner's last kill of someone else — actorB!=winner skips their
         * own suicide/drown, which would otherwise mislocate the clip. */
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL &&
                tl[i].actorA == aw->winnerSlot && tl[i].actorB != aw->winnerSlot)
                best = i;
        break;
    case AWARD_LGM_HUNTER:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_LGM_LOST && tl[i].actorA == aw->winnerSlot)
                best = i;
        break;
    case AWARD_CANNON_FODDER:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_LGM_LOST && tl[i].actorB == aw->winnerSlot)
                best = i;
        break;
    case AWARD_MOST_BASE_CAPTURES:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_BASE_CAPTURE && tl[i].actorA == aw->winnerSlot)
                best = i;
        break;
    case AWARD_MOST_PILL_CAPTURES:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_PILL_CAPTURE && tl[i].actorA == aw->winnerSlot)
                best = i;
        break;
    default:
        break;   /* builder/aggregate awards: nothing in the timeline to point at */
    }
    return best;
}

/* Order candidates for greedy selection: strongest score first, then a fixed
 * tie-break so the result is identical every run. */
static int hlCandCmp(const void *pa, const void *pb) {
    const HlCand *a = (const HlCand *)pa;
    const HlCand *b = (const HlCand *)pb;
    if (a->score != b->score)         return a->score < b->score ? 1 : -1;  /* desc */
    if (a->startTick != b->startTick) return a->startTick < b->startTick ? -1 : 1;
    if (a->type != b->type)           return a->type < b->type ? -1 : 1;
    if (a->actorA != b->actorA)       return a->actorA < b->actorA ? -1 : 1;
    if (a->awardId != b->awardId)     return a->awardId < b->awardId ? -1 : 1;
    return 0;
}

/* Chronological order for the finished reel. */
static int hlWindowCmpByStart(const void *pa, const void *pb) {
    const HighlightWindow *a = (const HighlightWindow *)pa;
    const HighlightWindow *b = (const HighlightWindow *)pb;
    if (a->startTick != b->startTick) return a->startTick < b->startTick ? -1 : 1;
    if (a->type != b->type)           return a->type < b->type ? -1 : 1;
    if (a->actorA != b->actorA)       return a->actorA < b->actorA ? -1 : 1;
    if (a->awardId != b->awardId)     return a->awardId < b->awardId ? -1 : 1;
    return 0;
}

/* Walk earlier events contiguous to the anchor — same locale, small tick gaps —
 * to widen the window's start, bounded by the max lead-in. Also settles the end
 * tick: a wipe keeps its last clustered death, a point anchor gets a fixed clip. */
static void hlLeadIn(const NotableEvent *tl, const HlCand *c,
                     uint32_t *outStart, uint32_t *outEnd) {
    uint32_t anchorTick = tl[c->anchorIdx].tick;
    uint32_t start = c->startTick;
    uint32_t prevTick = anchorTick;

    for (int j = c->anchorIdx - 1; j >= 0; j--) {
        if (hlTileDist(c->mapX, c->mapY, tl[j].mapX, tl[j].mapY) > HL_LEADIN_TILE_RADIUS)
            break;
        if (tl[j].tick > prevTick) break;                     /* not an earlier event */
        if (prevTick - tl[j].tick >= HL_LEADIN_MAX_GAP) break; /* gap too large */
        if (anchorTick - tl[j].tick > HL_LEADIN_MAX_TICKS) break; /* lead-in capped */
        start = tl[j].tick;
        prevTick = tl[j].tick;
    }

    *outStart = start;
    uint32_t end = (c->type == HL_CLUSTER_WIPE) ? c->endTick
                                                : anchorTick + HL_CLIP_TICKS;
    if (end < start) end = start;
    *outEnd = end;
}

void computeHighlights(const NotableEvent *timeline, int timelineCount,
                       const PlayerRoundStats stats[MAX_TANKS],
                       const uint8_t team[MAX_TANKS],
                       const AwardResult *awards, int awardCount,
                       HighlightWindow *out, int *outCount, int maxOut) {
    HlCand *cands;
    int candCount = 0;
    int accepted[HIGHLIGHTS_MAX];
    int acceptCount = 0;
    uint32_t firstTick, lastTick;
    HighlightWindow tmp[HIGHLIGHTS_MAX];

    (void)stats;   /* reserved for later signal passes */

    if (outCount != NULL) *outCount = 0;
    if (out == NULL || outCount == NULL) return;
    if (maxOut > HIGHLIGHTS_MAX) maxOut = HIGHLIGHTS_MAX;
    if (maxOut <= 0) return;
    if (timeline == NULL || timelineCount <= 0) return;

    cands = (HlCand *)malloc(sizeof(HlCand) * HL_CAND_MAX);
    if (cands == NULL) return;

    /* Tick bounds — scanned, not assumed, so the endgame weighting holds even
     * if a hand-crafted timeline is out of order. */
    firstTick = lastTick = timeline[0].tick;
    for (int i = 1; i < timelineCount; i++) {
        if (timeline[i].tick < firstTick) firstTick = timeline[i].tick;
        if (timeline[i].tick > lastTick)  lastTick = timeline[i].tick;
    }

    /* Award anchors. */
    for (int a = 0; awards != NULL && a < awardCount; a++) {
        int idx = hlFindAwardAnchor(timeline, timelineCount, &awards[a]);
        HlCand c;
        if (idx < 0) continue;   /* no moment to point at: skip, not an error */
        c.anchorIdx = idx;
        c.startTick = timeline[idx].tick;
        c.endTick   = timeline[idx].tick;
        c.mapX = timeline[idx].mapX;
        c.mapY = timeline[idx].mapY;
        c.type = HL_AWARD;
        c.awardId = awards[a].awardId;
        c.actorA = awards[a].winnerSlot;
        c.actorB = (awards[a].awardId == AWARD_NEMESIS) ? awards[a].subjectSlot
                                                        : timeline[idx].actorB;
        c.value = awards[a].value;
        c.score = HL_AWARD_WEIGHT;
        hlAppend(cands, &candCount, &c);
    }

    /* Cluster wipes: for each kill that starts a fresh cluster, gather the kills
     * near it in time and space. Greedy — the next start jumps past the cluster
     * just emitted, so one dense fight yields one window, not one per death. */
    {
        uint32_t clusterEnd = 0;
        bool haveCluster = false;
        for (int i = 0; i < timelineCount; i++) {
            if (timeline[i].type != NOTABLE_KILL) continue;
            if (haveCluster && timeline[i].tick <= clusterEnd) continue;

            uint32_t winEnd = timeline[i].tick + HL_WIPE_TICK_WINDOW;
            uint32_t lastEvTick = timeline[i].tick;
            uint16_t killerFreq[MAX_TANKS];
            int teamOfFirst = -1;
            bool sameTeam = (team != NULL);
            int count = 0;
            HlCand c;

            memset(killerFreq, 0, sizeof(killerFreq));
            for (int j = i; j < timelineCount; j++) {
                if (timeline[j].type != NOTABLE_KILL) continue;
                if (timeline[j].tick < timeline[i].tick ||
                    timeline[j].tick > winEnd) continue;
                if (hlTileDist(timeline[i].mapX, timeline[i].mapY,
                               timeline[j].mapX, timeline[j].mapY) > HL_WIPE_TILE_RADIUS)
                    continue;
                count++;
                if (timeline[j].tick > lastEvTick) lastEvTick = timeline[j].tick;
                if (timeline[j].actorA < MAX_TANKS) killerFreq[timeline[j].actorA]++;
                if (team != NULL) {
                    if (timeline[j].actorB < MAX_TANKS) {
                        int t = team[timeline[j].actorB];
                        if (teamOfFirst < 0) teamOfFirst = t;
                        else if (t != teamOfFirst) sameTeam = false;
                    } else {
                        sameTeam = false;
                    }
                }
            }
            if (count < HL_WIPE_MIN_DEATHS) continue;

            /* Most frequent killer in the cluster; ties keep the lowest slot. */
            int topKiller = timeline[i].actorA;
            int topFreq = -1;
            for (int s = 0; s < MAX_TANKS; s++)
                if ((int)killerFreq[s] > topFreq) { topFreq = killerFreq[s]; topKiller = s; }

            c.anchorIdx = i;
            c.startTick = timeline[i].tick;
            c.endTick   = lastEvTick;
            c.mapX = timeline[i].mapX;
            c.mapY = timeline[i].mapY;
            c.type = HL_CLUSTER_WIPE;
            c.awardId = 0;
            c.actorA = (uint8_t)topKiller;
            c.actorB = NEUTRAL;
            c.value = (uint32_t)count;
            c.score = (uint32_t)(HL_WIPE_WEIGHT * count +
                                 (sameTeam ? HL_WIPE_TEAM_BONUS : 0));
            hlAppend(cands, &candCount, &c);
            clusterEnd = lastEvTick;
            haveCluster = true;
        }
    }

    /* Objective steals: an enemy-owned pill/base changing hands. Weight rises
     * toward the final tick, where a steal is more likely to decide the game. */
    {
        uint32_t span = lastTick > firstTick ? (lastTick - firstTick) : 1;
        for (int i = 0; i < timelineCount; i++) {
            HlCand c;
            if (timeline[i].type != NOTABLE_PILL_CAPTURE &&
                timeline[i].type != NOTABLE_BASE_CAPTURE) continue;
            if (timeline[i].captureClass != ATTR_CAP_ENEMY) continue;
            /* Collapse a rapidly re-contested objective: skip if a steal was
             * already emitted for this same cell within the dedup window, so a
             * base flipping several times in a couple of seconds is one clip. */
            bool dup = false;
            for (int k = 0; k < candCount; k++) {
                if (cands[k].type == HL_OBJECTIVE_STEAL &&
                    cands[k].mapX == timeline[i].mapX &&
                    cands[k].mapY == timeline[i].mapY &&
                    timeline[i].tick - cands[k].startTick < HL_STEAL_DEDUP_TICKS) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            uint32_t bonus = (uint32_t)((uint64_t)HL_STEAL_ENDGAME_BONUS *
                                        (timeline[i].tick - firstTick) / span);
            c.anchorIdx = i;
            c.startTick = timeline[i].tick;
            c.endTick   = timeline[i].tick;
            c.mapX = timeline[i].mapX;
            c.mapY = timeline[i].mapY;
            c.type = HL_OBJECTIVE_STEAL;
            c.awardId = 0;
            c.actorA = timeline[i].actorA;
            c.actorB = timeline[i].actorB;
            c.value = 1;
            c.score = (uint32_t)HL_STEAL_WEIGHT + bonus;
            hlAppend(cands, &candCount, &c);
        }
    }

    /* Selection: strongest first, greedily keeping windows whose tick spans do
     * not overlap an accepted one. This both de-dups and spreads the picks. */
    qsort(cands, candCount, sizeof(HlCand), hlCandCmp);
    for (int i = 0; i < candCount && acceptCount < maxOut; i++) {
        bool overlap = false;
        for (int k = 0; k < acceptCount; k++) {
            const HlCand *acc = &cands[accepted[k]];
            if (cands[i].startTick <= acc->endTick &&
                acc->startTick <= cands[i].endTick) { overlap = true; break; }
        }
        if (!overlap) accepted[acceptCount++] = i;
    }

    /* Widen each pick with its lead-in, then order the reel chronologically. */
    for (int k = 0; k < acceptCount; k++) {
        const HlCand *c = &cands[accepted[k]];
        HighlightWindow *w = &tmp[k];
        uint32_t s, e;
        hlLeadIn(timeline, c, &s, &e);
        w->startTick = s;
        w->durationTicks = e - s;
        w->mapX = c->mapX;
        w->mapY = c->mapY;
        w->type = c->type;
        w->awardId = c->awardId;
        w->actorA = c->actorA;
        w->actorB = c->actorB;
        w->value = c->value;
        w->score = c->score;
    }
    qsort(tmp, acceptCount, sizeof(HighlightWindow), hlWindowCmpByStart);
    for (int k = 0; k < acceptCount; k++) out[k] = tmp[k];
    *outCount = acceptCount;

    free(cands);
}
