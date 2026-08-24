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
#include <math.h>
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

/* ---- Award subset draw ---------------------------------------------------- */

/* The negative awards — the ones a round gets teased about rather than bragged
 * about. The subset draw always keeps one of these when the round won one, so a
 * shortened list never reads as a flat roll of winners. */
static bool awardIsFun(uint8_t awardId) {
    return awardId == AWARD_MOST_DEATHS   || awardId == AWARD_FISH_FOOD ||
           awardId == AWARD_CANNON_FODDER || awardId == AWARD_WASTEFUL  ||
           awardId == AWARD_BIGGEST_FUMBLE;
}

/* xorshift32: three shifts, no state beyond the seed, and the same sequence on
 * every compiler and platform — which is the entire requirement here. It is
 * stuck at zero, so callers force a non-zero seed before the first draw. */
static uint32_t pickRand(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int roundStatsPickAwardSubset(const RoundStatsSummary *summary,
                              uint8_t *outIdx, int maxOut) {
    if (summary == NULL || outIdx == NULL || maxOut <= 0) {
        return 0;
    }

    int n = summary->awardCount;
    if (n > AWARD_COUNT) {
        n = AWARD_COUNT;
    }
    if (n <= 0) {
        return 0;
    }
    if (n <= maxOut) {
        /* Everything fits: nothing to choose, so nothing is drawn. */
        for (int i = 0; i < n; i++) {
            outIdx[i] = (uint8_t)i;
        }
        return n;
    }

    /* The seed folds the summary's own content and reads nothing else — no
     * clock, no call counter, no address — because two clients showing the same
     * summary have to land on the same picks. FNV-1a over each won
     * award's id and value plus each highlight's start tick; whole integers go
     * into the mix rather than their bytes, so the fold is endian-independent. */
    uint32_t seed = 2166136261u;   /* FNV-1a offset basis */
    for (int i = 0; i < n; i++) {
        seed = (seed ^ summary->awards[i].awardId) * 16777619u;
        seed = (seed ^ summary->awards[i].value)   * 16777619u;
    }
    int hc = summary->highlightCount;
    if (hc > ROUND_STATS_HIGHLIGHTS_WIRE_MAX) {
        hc = ROUND_STATS_HIGHLIGHTS_WIRE_MAX;
    }
    for (int i = 0; i < hc; i++) {
        seed = (seed ^ summary->highlights[i].startTick) * 16777619u;
    }
    if (seed == 0) {
        seed = 2654435769u;   /* xorshift32 would never leave zero */
    }

    bool taken[AWARD_COUNT];
    for (int i = 0; i < AWARD_COUNT; i++) {
        taken[i] = false;
    }
    int picked = 0;

    /* One negative award first, drawn from those the round actually won; the
     * rest of the slots then fill from everything still unpicked, which is what
     * keeps the guarantee from costing more than a single slot. */
    int fun[AWARD_COUNT];
    int funCount = 0;
    for (int i = 0; i < n; i++) {
        if (awardIsFun(summary->awards[i].awardId)) {
            fun[funCount++] = i;
        }
    }
    if (funCount > 0) {
        int f = (int)(pickRand(&seed) % (uint32_t)funCount);
        taken[fun[f]] = true;
        outIdx[picked++] = (uint8_t)fun[f];
    }

    /* Without replacement: each draw walks to the k'th award still untaken, so
     * an index can never come up twice. n > maxOut here, so there is always one
     * left to take. */
    while (picked < maxOut) {
        int k = (int)(pickRand(&seed) % (uint32_t)(n - picked));
        int chosen = -1;
        for (int i = 0; i < n; i++) {
            if (taken[i]) {
                continue;
            }
            if (k == 0) {
                chosen = i;
                break;
            }
            k--;
        }
        if (chosen < 0) {
            break;   /* unreachable while any award is untaken; bounds the loop */
        }
        taken[chosen] = true;
        outIdx[picked++] = (uint8_t)chosen;
    }

    /* Sorted by award id so the shown picks read in the same order the full list
     * does, and so the order is a property of the summary rather than of the
     * order the draw happened to visit them in. */
    for (int i = 1; i < picked; i++) {
        uint8_t v = outIdx[i];
        uint8_t vid = summary->awards[v].awardId;
        int j = i;
        while (j > 0 && summary->awards[outIdx[j - 1]].awardId > vid) {
            outIdx[j] = outIdx[j - 1];
            j--;
        }
        outIdx[j] = v;
    }

    return picked;
}

/* ---- Territory influence pre-pass ---------------------------------------- */

/* Record mapX/mapY are bytes, so the influence grid is 256x256 regardless of the
 * map the round was played on. Kept local so this module stays free of the sim's
 * map headers. */
#define HL_MAP_DIM             256
#define HL_MAP_CELLS           (HL_MAP_DIM * HL_MAP_DIM)
/* Pill/base indices are a byte in the record stream. */
#define HL_TERR_OBJ_MAX        256
/* Influence a held objective projects, and how far it reaches. A base anchors a
 * wider, stronger area than a pill. */
#define HL_INF_BASE_STRENGTH   100
#define HL_INF_BASE_RADIUS     12
#define HL_INF_PILL_STRENGTH   60
#define HL_INF_PILL_RADIUS     8
/* Control map sentinel: the cell is uncontrolled or contested. */
#define HL_CTRL_NONE           0xFF
/* How much recent shelling a shift carries: damage records within this many tiles
 * (Chebyshev) and this many prior ticks of the shift's cell are summed into its
 * recentDamage. The window bounds the rolling damage buffer's live size too. */
#define HL_COLLAPSE_DMG_RADIUS 8
#define HL_COLLAPSE_DMG_WINDOW 500   /* ~10 s of prior fire at 50 ticks/s */
/* Hard cap on buffered damage events; oldest is dropped if it fills. Pruning by
 * tick usually keeps it far below this, so the cap is only a runaway backstop. */
#define HL_DMG_BUF_MAX         4096

/* One tracked objective: who holds it and where it sits. `known` stays 0 until a
 * record tells us its cell, so an objective nobody has touched stamps nothing. */
typedef struct {
    uint8_t owner;        /* owning slot, NEUTRAL when unowned */
    uint8_t mapX, mapY;
    uint8_t known;
} TerrObject;

/* One buffered damage event, kept only long enough to attribute recent fire to a
 * territory shift near it. */
typedef struct {
    uint32_t tick;
    uint8_t  mapX, mapY;
    uint16_t amount;
} TerrDamage;

/* Ring buffer of the recent damage events, oldest at `head`, `count` live. */
typedef struct {
    TerrDamage *buf;
    int head;
    int count;
} TerrDmgRing;

/* Append one damage event, dropping the oldest if the ring is full. */
static void terrDmgPush(TerrDmgRing *ring, uint32_t tick, uint8_t x, uint8_t y,
                        uint16_t amount) {
    int idx;
    if (ring->count == HL_DMG_BUF_MAX) {
        ring->head = (ring->head + 1) % HL_DMG_BUF_MAX;   /* drop oldest */
        ring->count--;
    }
    idx = (ring->head + ring->count) % HL_DMG_BUF_MAX;
    ring->buf[idx].tick = tick;
    ring->buf[idx].mapX = x;
    ring->buf[idx].mapY = y;
    ring->buf[idx].amount = amount;
    ring->count++;
}

/* Drop buffered events older than the damage window ending at `now`. Events are
 * pushed in tick order, so the stale ones are always at the head. */
static void terrDmgPrune(TerrDmgRing *ring, uint32_t now) {
    uint32_t cutoff = (now > HL_COLLAPSE_DMG_WINDOW) ? now - HL_COLLAPSE_DMG_WINDOW : 0;
    while (ring->count > 0 && ring->buf[ring->head].tick < cutoff) {
        ring->head = (ring->head + 1) % HL_DMG_BUF_MAX;
        ring->count--;
    }
}

/* Sum buffered damage within HL_COLLAPSE_DMG_RADIUS tiles and the window before
 * `tick` of the cell (x,y); clamped to uint16. */
static uint16_t terrDmgSum(const TerrDmgRing *ring, uint32_t tick, uint8_t x,
                           uint8_t y) {
    uint32_t lo = (tick > HL_COLLAPSE_DMG_WINDOW) ? tick - HL_COLLAPSE_DMG_WINDOW : 0;
    uint32_t total = 0;
    for (int k = 0; k < ring->count; k++) {
        int idx = (ring->head + k) % HL_DMG_BUF_MAX;
        uint32_t dt = ring->buf[idx].tick;
        int ddx, ddy, cheb;
        if (dt < lo || dt > tick) continue;
        ddx = (int)ring->buf[idx].mapX - (int)x; if (ddx < 0) ddx = -ddx;
        ddy = (int)ring->buf[idx].mapY - (int)y; if (ddy < 0) ddy = -ddy;
        cheb = ddx > ddy ? ddx : ddy;
        if (cheb > HL_COLLAPSE_DMG_RADIUS) continue;
        total += ring->buf[idx].amount;
    }
    return (uint16_t)(total > 65535u ? 65535u : total);
}

/* Team of an owner slot; NEUTRAL for unowned or an out-of-range slot. */
static uint8_t terrTeamOfSlot(const AttrSlotIdentity *slots, int slotCount,
                              uint8_t slot) {
    if ((int)slot >= slotCount || slot >= MAX_TANKS) return NEUTRAL;
    return slots[slot].team;
}

/* Position of a team id in the compacted team list, or -1 when absent. */
static int terrTeamIndex(const uint8_t *teamIds, int teamCount, uint8_t team) {
    for (int k = 0; k < teamCount; k++)
        if (teamIds[k] == team) return k;
    return -1;
}

/* Stamp a falloff disk of influence centred on (cx,cy) into one team's grid:
 * full strength at the centre, tapering linearly to the rim, nothing outside the
 * Euclidean radius. Stamps accumulate, so two nearby objectives reinforce. */
static void terrStamp(int16_t *grid, int cx, int cy, int radius, int strength) {
    int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; dy++) {
        int ny = cy + dy;
        if (ny < 0 || ny >= HL_MAP_DIM) continue;
        for (int dx = -radius; dx <= radius; dx++) {
            int nx = cx + dx;
            int d2, delta, idx, val;
            float dist, proximity;
            if (nx < 0 || nx >= HL_MAP_DIM) continue;
            d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            dist = sqrtf((float)d2);
            proximity = 1.0f - dist / (float)(radius + 1);
            delta = (int)(strength * proximity);
            if (delta == 0) continue;
            idx = ny * HL_MAP_DIM + nx;
            val = (int)grid[idx] + delta;
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;
            grid[idx] = (int16_t)val;
        }
    }
}

/* Restamp every held objective into freshly cleared per-team grids. Cheap enough
 * to redo per ownership change (at most a few hundred small disks), and it keeps
 * the grids exactly derivable from the current ownership tables. */
static void terrRestamp(int16_t *grids, int teamCount, const uint8_t *teamIds,
                        const TerrObject *pills, const TerrObject *bases,
                        const AttrSlotIdentity *slots, int slotCount) {
    memset(grids, 0, (size_t)teamCount * HL_MAP_CELLS * sizeof(int16_t));

    for (int i = 0; i < HL_TERR_OBJ_MAX; i++) {
        const TerrObject *objs[2];
        objs[0] = &bases[i];
        objs[1] = &pills[i];
        for (int k = 0; k < 2; k++) {
            const TerrObject *o = objs[k];
            int ti;
            if (!o->known || o->owner == NEUTRAL) continue;
            ti = terrTeamIndex(teamIds, teamCount,
                               terrTeamOfSlot(slots, slotCount, o->owner));
            if (ti < 0) continue;
            terrStamp(grids + (size_t)ti * HL_MAP_CELLS, o->mapX, o->mapY,
                      k == 0 ? HL_INF_BASE_RADIUS : HL_INF_PILL_RADIUS,
                      k == 0 ? HL_INF_BASE_STRENGTH : HL_INF_PILL_STRENGTH);
        }
    }
}

/* Re-derive the controlling team of every cell in [x0,x1]x[y0,y1] and return how
 * many of them were taken from a team that already held them. Only this box is
 * touched: an ownership change moves influence nowhere else, so control outside
 * it is still valid from the previous pass. A cell is controlled by the single
 * team with the most influence there; zero influence, or a tie for the lead,
 * leaves it uncontrolled. The control map is updated for every change so it stays
 * accurate, but a first claim of unheld ground (HL_CTRL_NONE -> team) is not
 * counted: that is the round populating an empty map, not ground being contested. */
static uint32_t terrDiffControl(const int16_t *grids, int teamCount,
                                uint8_t *control, int x0, int y0, int x1, int y1) {
    uint32_t flipped = 0;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int cell = y * HL_MAP_DIM + x;
            int bestT = -1;
            int16_t bestV = 0;
            bool tie = false;
            uint8_t now, was;
            for (int t = 0; t < teamCount; t++) {
                int16_t v = grids[(size_t)t * HL_MAP_CELLS + cell];
                if (v <= 0) continue;
                if (bestT < 0 || v > bestV) { bestT = t; bestV = v; tie = false; }
                else if (v == bestV)        { tie = true; }
            }
            now = (bestT < 0 || tie) ? HL_CTRL_NONE : (uint8_t)bestT;
            was = control[cell];
            if (now != was) {
                control[cell] = now;
                if (was != HL_CTRL_NONE) flipped++;  /* only ground taken off a team */
            }
        }
    }
    return flipped;
}

void computeTerritoryShifts(const uint8_t *records, size_t len,
                            const AttrSlotIdentity slots[MAX_TANKS],
                            int slotCount,
                            TerritoryShift *out, int *outCount, int maxOut) {
    uint8_t teamIds[MAX_TANKS];
    int teamCount = 0;
    int16_t *grids = NULL;
    uint8_t *control = NULL;
    TerrDmgRing dmg;
    TerrObject pills[HL_TERR_OBJ_MAX];
    TerrObject bases[HL_TERR_OBJ_MAX];
    size_t off = 0;
    int count = 0;

    dmg.buf = NULL;
    dmg.head = 0;
    dmg.count = 0;

    if (outCount != NULL) *outCount = 0;
    if (out == NULL || outCount == NULL || maxOut <= 0) return;
    if (records == NULL || len == 0 || slots == NULL) return;
    if (slotCount > MAX_TANKS) slotCount = MAX_TANKS;
    if (slotCount <= 0) return;

    /* Only the team ids actually present get a grid — a round is usually two or
     * three sided, and the ids are not guaranteed to be small or contiguous. */
    for (int s = 0; s < slotCount; s++) {
        if (terrTeamIndex(teamIds, teamCount, slots[s].team) < 0)
            teamIds[teamCount++] = slots[s].team;
    }
    if (teamCount <= 0) return;

    grids = (int16_t *)calloc((size_t)teamCount * HL_MAP_CELLS, sizeof(int16_t));
    control = (uint8_t *)malloc(HL_MAP_CELLS);
    dmg.buf = (TerrDamage *)malloc(sizeof(TerrDamage) * HL_DMG_BUF_MAX);
    if (grids == NULL || control == NULL || dmg.buf == NULL) {
        free(grids);
        free(control);
        free(dmg.buf);
        return;
    }
    memset(control, HL_CTRL_NONE, HL_MAP_CELLS);   /* nothing held at round start */

    for (int i = 0; i < HL_TERR_OBJ_MAX; i++) {
        pills[i].owner = NEUTRAL; pills[i].mapX = 0; pills[i].mapY = 0; pills[i].known = 0;
        bases[i].owner = NEUTRAL; bases[i].mapX = 0; bases[i].mapY = 0; bases[i].known = 0;
    }

    /* Walk by offset: read the tag, size it, and stop on an unknown tag or an
     * overrun so we never read past the stream. */
    while (off < len && count < maxOut) {
        uint8_t tag = records[off];
        size_t sz = roundStatsRecordSize(tag);
        TerrObject *obj = NULL;
        uint8_t oldX = 0, oldY = 0, oldKnown = 0;
        uint8_t gainTeam = NEUTRAL, loseTeam = NEUTRAL;
        uint8_t evX = 0, evY = 0;
        uint32_t evTick = 0;
        int radius = HL_INF_PILL_RADIUS;
        uint32_t flipped;
        int x0, y0, x1, y1;

        if (sz == 0 || off + sz > len) break;

        if (tag == ATTR_REC_CAPTURE) {
            const AttrCaptureRecord *r = (const AttrCaptureRecord *)(records + off);
            obj = (r->target == ATTR_CAP_TGT_BASE) ? &bases[r->targetIndex]
                                                   : &pills[r->targetIndex];
            radius = (r->target == ATTR_CAP_TGT_BASE) ? HL_INF_BASE_RADIUS
                                                      : HL_INF_PILL_RADIUS;
            evTick = r->tick;
            evX = r->mapX;
            evY = r->mapY;
            /* The losing side is whoever we had holding it, not the record's
             * prevOwner: our tables are what the grids were stamped from. */
            loseTeam = terrTeamOfSlot(slots, slotCount, obj->owner);
            gainTeam = terrTeamOfSlot(slots, slotCount, r->newOwner);
            oldX = obj->mapX; oldY = obj->mapY; oldKnown = obj->known;
            obj->owner = r->newOwner;
            obj->mapX = r->mapX;
            obj->mapY = r->mapY;
            obj->known = 1;
        } else if (tag == ATTR_REC_DAMAGE) {
            const AttrDamageRecord *r = (const AttrDamageRecord *)(records + off);
            /* Every hit feeds the recent-fire buffer, whatever it landed on, so a
             * later nearby shift can tell it was shelled first. */
            terrDmgPrune(&dmg, r->tick);
            terrDmgPush(&dmg, r->tick, r->mapX, r->mapY, r->amount);
            if (r->destroyed && r->target == ATTR_TGT_PILL) {
                /* A pill shot to zero armour holds nothing until it is placed
                 * again; a pill riding in a tank projects no influence either. */
                obj = &pills[r->targetIndex];
                radius = HL_INF_PILL_RADIUS;
                evTick = r->tick;
                evX = r->mapX;
                evY = r->mapY;
                loseTeam = terrTeamOfSlot(slots, slotCount, obj->owner);
                gainTeam = NEUTRAL;
                oldX = obj->mapX; oldY = obj->mapY; oldKnown = obj->known;
                obj->owner = NEUTRAL;
                obj->mapX = r->mapX;
                obj->mapY = r->mapY;
                obj->known = 1;
            }
        }

        off += sz;
        if (obj == NULL) continue;

        terrRestamp(grids, teamCount, teamIds, pills, bases, slots, slotCount);

        /* Control can only have moved within the object's reach — around where it
         * sits now, and around where it sat before if a carried pill was replaced
         * somewhere else. */
        x0 = (int)evX - radius; x1 = (int)evX + radius;
        y0 = (int)evY - radius; y1 = (int)evY + radius;
        if (oldKnown) {
            if ((int)oldX - radius < x0) x0 = (int)oldX - radius;
            if ((int)oldX + radius > x1) x1 = (int)oldX + radius;
            if ((int)oldY - radius < y0) y0 = (int)oldY - radius;
            if ((int)oldY + radius > y1) y1 = (int)oldY + radius;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= HL_MAP_DIM) x1 = HL_MAP_DIM - 1;
        if (y1 >= HL_MAP_DIM) y1 = HL_MAP_DIM - 1;

        flipped = terrDiffControl(grids, teamCount, control, x0, y0, x1, y1);
        if (flipped == 0) continue;   /* e.g. a handover inside one team */

        out[count].tick = evTick;
        out[count].mapX = evX;
        out[count].mapY = evY;
        out[count].gainTeam = gainTeam;
        out[count].loseTeam = loseTeam;
        out[count].cellsFlipped = (uint16_t)(flipped > 65535u ? 65535u : flipped);
        out[count].recentDamage = terrDmgSum(&dmg, evTick, evX, evY);
        count++;
    }

    *outCount = count;
    free(grids);
    free(control);
    free(dmg.buf);
}

/* ---- Highlight scorer ---------------------------------------------------- */

/* Tunable weights and window sizes. Ticks are per-round at 50 ticks/s. */
#define HL_WIPE_MIN_DEATHS       3
#define HL_WIPE_TICK_WINDOW      150   /* ~3 s span for a cluster of deaths */
#define HL_WIPE_TILE_RADIUS      6     /* Chebyshev radius from the first death */
#define HL_WIPE_WEIGHT           100   /* per death in the cluster */
#define HL_WIPE_TEAM_BONUS       50    /* added when all the dead share a team */
#define HL_STEAL_WEIGHT          60
#define HL_STEAL_ENDGAME_BONUS   20    /* mild late edge (eyeball-tunable); scaled by
                                        * how late in the round the steal lands */
#define HL_STEAL_DEDUP_TICKS     500   /* one steal clip per contested cell in this span */
#define HL_AWARD_WEIGHT          120   /* award anchors are highest-confidence */
#define HL_LEADIN_TILE_RADIUS    6
#define HL_LEADIN_MAX_GAP        60    /* stop the lead-in on a gap this big */
#define HL_LEADIN_MAX_TICKS      250   /* cap on how far a lead-in reaches back */
#define HL_CLIP_TICKS            250   /* window duration around a single anchor */
#define HL_ANCHOR_DENSITY_TICKS  HL_CLIP_TICKS  /* count-award anchor: events this
                                                 * close would share a clip */
#define HL_TIME_BUCKETS          4     /* split the round into this many spread buckets */
#define HL_TURN_WINDOW           500   /* ~10 s span the territory swing is summed over */
#define HL_TURN_WEIGHT           140   /* ranking magnitude; the pick is seeded, not earned */
#define HL_TEAM_ID_MAX           256   /* team ids are a byte, so this bounds them */
#define HL_COLLAPSE_WINDOW       300   /* ~6 s span a front collapse is summed over */
#define HL_COLLAPSE_TILE_RADIUS  6     /* Chebyshev cluster radius from the first shift */
#define HL_COLLAPSE_MIN_CELLS    150   /* summed swing a collapse must reach */
#define HL_COLLAPSE_MIN_DAMAGE   200   /* summed recent fire that gates a collapse */
#define HL_COLLAPSE_WEIGHT       110   /* below the seeded turning point, above a steal */

/* The death-flavoured signals. Starting values chosen by eye, not derived from
 * anything — expect them to move once real logs have been watched. */
#define HL_LGM_MIN_KILLS         2     /* LGM kills that make a sweep */
#define HL_LGM_TICK_WINDOW       250   /* ~5 s span for one sweep */
#define HL_LGM_TILE_RADIUS       8     /* Chebyshev radius from the first kill */
#define HL_LGM_WEIGHT            45    /* per LGM killed in the sweep */
#define HL_FUMBLE_MIN_PILLS      3     /* pills a death must dump to count */
#define HL_FUMBLE_WEIGHT         55
#define HL_FUMBLE_PILL_BONUS     15    /* per pill above HL_FUMBLE_MIN_PILLS */
#define HL_DROWN_WEIGHT          50
#define HL_DROWN_PILL_BONUS      15    /* per pill carried into the sea */
#define HL_AWARD_DEDUP_TICKS     HL_CLIP_TICKS  /* same moment as the award clip */

/* Upper bound on candidate windows collected before selection. Five passes can
 * each emit at most one window per timeline event (steals, wipes, LGM sweeps,
 * fumbles, drownings), plus the awards, the one turning point, and up to one
 * collapse per territory shift. */
#define HL_CAND_MAX              (NOTABLE_EVENTS_MAX * 5 + AWARD_COUNT + 1 + \
                                  TERRITORY_SHIFTS_MAX)

/* A scored candidate window before selection. `endTick` is the selection span
 * end (the last clustered event for a wipe, else the anchor tick); lead-in and
 * the display duration are derived from `anchorIdx` at finalize time.
 * `anchorIdx` is -1 for a candidate with no timeline event to point at (the
 * turning point, which is derived from the territory series): it keeps the span
 * it was built with and gets no lead-in. */
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

/* True when an award anchor for `awardId` has already been appended at almost
 * this tick. The award pass runs first, so a signal pass that re-proposes the
 * moment an award already points at (the biggest fumble, the last drowning, the
 * LGM Hunter's densest cluster) skips it and lets the award clip stand for both.
 * The candidate may fall either side of the anchor, so the gap is absolute. */
static bool hlNearAwardAnchor(const HlCand *cands, int n, uint8_t awardId,
                              uint32_t tick) {
    for (int k = 0; k < n; k++) {
        uint32_t at;
        if (cands[k].type != HL_AWARD || cands[k].awardId != awardId) continue;
        at = cands[k].startTick;
        if ((tick > at ? tick - at : at - tick) < HL_AWARD_DEDUP_TICKS) return true;
    }
    return false;
}

/* Of the timeline events in idx[0..m), the one whose tick has the most other
 * idx ticks within +/-HL_ANCHOR_DENSITY_TICKS — the moment where the winner's
 * qualifying events pile up, not the last stray one. Ties keep the earliest, so
 * a late lone event never beats an equally-dense earlier cluster. Returns the
 * chosen timeline index, or -1 when idx is empty. */
static int hlDensest(const int *idx, int m, const NotableEvent *tl) {
    int best = -1, bestDensity = -1;
    uint32_t bestTick = 0;
    for (int a = 0; a < m; a++) {
        uint32_t ta = tl[idx[a]].tick;
        int density = 0;
        for (int b = 0; b < m; b++) {
            uint32_t tb = tl[idx[b]].tick;
            uint32_t gap = ta > tb ? ta - tb : tb - ta;
            if (gap <= HL_ANCHOR_DENSITY_TICKS) density++;
        }
        if (density > bestDensity || (density == bestDensity && ta < bestTick)) {
            bestDensity = density;
            bestTick = ta;
            best = idx[a];
        }
    }
    return best;
}

/* Timeline index of the event that best represents an award, or -1 when the
 * award has no moment to anchor (builder/aggregate stats leave no timeline
 * event). Count/repeated-event awards anchor on the winner's densest qualifying
 * moment rather than their last, which is systematically late; the two
 * single-moment awards keep pointing at their one specific event. */
static int hlFindAwardAnchor(const NotableEvent *tl, int n, const AwardResult *aw) {
    int idx[NOTABLE_EVENTS_MAX];
    int m = 0;
    switch (aw->awardId) {
    case AWARD_NEMESIS:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL &&
                tl[i].actorA == aw->winnerSlot && tl[i].actorB == aw->subjectSlot)
                idx[m++] = i;   /* every kill of the nemesis victim */
        return hlDensest(idx, m, tl);
    case AWARD_BIGGEST_FUMBLE: {
        int best = -1, most = -1;
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL && tl[i].actorB == aw->winnerSlot &&
                (int)tl[i].carriedPills > most) {
                most = tl[i].carriedPills;   /* the biggest pill dump they died on */
                best = i;
            }
        return best;
    }
    case AWARD_FISH_FOOD: {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL && tl[i].actorB == aw->winnerSlot &&
                tl[i].deathCause == LAST_DEATH_BY_DEEPSEA)
                best = i;   /* last drowning */
        return best;
    }
    case AWARD_MOST_KILLS:
    case AWARD_BEST_KD:
        /* The winner's kills of someone else — actorB!=winner skips their own
         * suicide/drown, which would otherwise mislocate the clip. */
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_KILL &&
                tl[i].actorA == aw->winnerSlot && tl[i].actorB != aw->winnerSlot)
                idx[m++] = i;
        return hlDensest(idx, m, tl);
    case AWARD_LGM_HUNTER:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_LGM_LOST && tl[i].actorA == aw->winnerSlot)
                idx[m++] = i;
        return hlDensest(idx, m, tl);
    case AWARD_CANNON_FODDER:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_LGM_LOST && tl[i].actorB == aw->winnerSlot)
                idx[m++] = i;
        return hlDensest(idx, m, tl);
    case AWARD_MOST_BASE_CAPTURES:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_BASE_CAPTURE && tl[i].actorA == aw->winnerSlot)
                idx[m++] = i;
        return hlDensest(idx, m, tl);
    case AWARD_MOST_PILL_CAPTURES:
        for (int i = 0; i < n; i++)
            if (tl[i].type == NOTABLE_PILL_CAPTURE && tl[i].actorA == aw->winnerSlot)
                idx[m++] = i;
        return hlDensest(idx, m, tl);
    default:
        return -1;   /* builder/aggregate awards: nothing in the timeline to point at */
    }
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
 * tick, which holds one invariant: every anchored clip runs at least one clip
 * length, and longer when the cluster it was built from ran longer. A signal
 * anchored on a single moment has no cluster to outrun the floor, so it gets
 * exactly one clip length. */
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
    uint32_t end = anchorTick + HL_CLIP_TICKS;
    if (c->endTick > end) end = c->endTick;
    if (end < start) end = start;
    *outEnd = end;
}

/* Which time-bucket a start tick falls in, over [firstTick, firstTick+span].
 * span must be > 0. Clamped, so a territory-derived tick outside the timeline's
 * own span still lands in an end bucket rather than out of range. */
static int hlBucket(uint32_t startTick, uint32_t firstTick, uint32_t span) {
    int b;
    if (startTick <= firstTick) return 0;
    b = (int)((uint64_t)(startTick - firstTick) * HL_TIME_BUCKETS / span);
    if (b >= HL_TIME_BUCKETS) b = HL_TIME_BUCKETS - 1;
    return b;
}

void computeHighlights(const NotableEvent *timeline, int timelineCount,
                       const PlayerRoundStats stats[MAX_TANKS],
                       const uint8_t team[MAX_TANKS],
                       const AwardResult *awards, int awardCount,
                       const TerritoryShift *shifts, int shiftCount,
                       HighlightWindow *out, int *outCount, int maxOut) {
    HlCand *cands;
    int candCount = 0;
    int accepted[HIGHLIGHTS_MAX];
    int acceptCount = 0;
    int turnIdx = -1;
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

    /* Multi-LGM sweeps: one player cutting down several men in a single short
     * push. Greedy like the wipe loop — the next start jumps past the sweep just
     * emitted, so a rampage yields one window, not one per man. NOTABLE_LGM_LOST
     * names the killer in actorA and the man's owner in actorB. */
    {
        uint32_t clusterEnd = 0;
        bool haveCluster = false;
        for (int i = 0; i < timelineCount; i++) {
            if (timeline[i].type != NOTABLE_LGM_LOST) continue;
            if (haveCluster && timeline[i].tick <= clusterEnd) continue;

            uint32_t winEnd = timeline[i].tick + HL_LGM_TICK_WINDOW;
            uint32_t lastEvTick = timeline[i].tick;
            int count = 0;
            HlCand c;

            for (int j = i; j < timelineCount; j++) {
                if (timeline[j].type != NOTABLE_LGM_LOST) continue;
                if (timeline[j].actorA != timeline[i].actorA) continue;
                if (timeline[j].tick < timeline[i].tick ||
                    timeline[j].tick > winEnd) continue;
                if (hlTileDist(timeline[i].mapX, timeline[i].mapY,
                               timeline[j].mapX, timeline[j].mapY) > HL_LGM_TILE_RADIUS)
                    continue;
                count++;
                if (timeline[j].tick > lastEvTick) lastEvTick = timeline[j].tick;
            }
            if (count < HL_LGM_MIN_KILLS) continue;
            if (hlNearAwardAnchor(cands, candCount, AWARD_LGM_HUNTER,
                                  timeline[i].tick)) continue;

            c.anchorIdx = i;
            c.startTick = timeline[i].tick;
            c.endTick   = lastEvTick;
            c.mapX = timeline[i].mapX;
            c.mapY = timeline[i].mapY;
            c.type = HL_MULTI_LGM;
            c.awardId = 0;
            c.actorA = timeline[i].actorA;
            c.actorB = NEUTRAL;
            c.value = (uint32_t)count;
            c.score = (uint32_t)(HL_LGM_WEIGHT * count);
            hlAppend(cands, &candCount, &c);
            clusterEnd = lastEvTick;
            haveCluster = true;
        }
    }

    /* Fumbles: a death that dumped a big load of carried pills. The clip is about
     * the player who lost them, so they are actorA and their killer is actorB.
     * Drownings are left to the pass below, which pays its own pill bonus. */
    for (int i = 0; i < timelineCount; i++) {
        HlCand c;
        uint32_t pills;
        if (timeline[i].type != NOTABLE_KILL) continue;
        if (timeline[i].carriedPills < HL_FUMBLE_MIN_PILLS) continue;
        if (timeline[i].deathCause == LAST_DEATH_BY_DEEPSEA) continue;
        if (hlNearAwardAnchor(cands, candCount, AWARD_BIGGEST_FUMBLE,
                              timeline[i].tick)) continue;
        pills = timeline[i].carriedPills;
        c.anchorIdx = i;
        c.startTick = timeline[i].tick;
        c.endTick   = timeline[i].tick;
        c.mapX = timeline[i].mapX;
        c.mapY = timeline[i].mapY;
        c.type = HL_FUMBLE;
        c.awardId = 0;
        c.actorA = timeline[i].actorB;   /* the one who died and dropped them */
        c.actorB = timeline[i].actorA;   /* the killer */
        c.value = pills;
        c.score = (uint32_t)(HL_FUMBLE_WEIGHT +
                             HL_FUMBLE_PILL_BONUS * (pills - HL_FUMBLE_MIN_PILLS));
        hlAppend(cands, &candCount, &c);
    }

    /* Drownings, worth more the more pills went into the sea with the tank. The
     * type check is load-bearing: deathCause is 0 on every non-kill event and
     * LAST_DEATH_BY_MINES is also 0, so an unset field and a mine death are the
     * same byte and only NOTABLE_KILL entries may be read for a cause. */
    for (int i = 0; i < timelineCount; i++) {
        HlCand c;
        uint32_t pills;
        if (timeline[i].type != NOTABLE_KILL) continue;
        if (timeline[i].deathCause != LAST_DEATH_BY_DEEPSEA) continue;
        if (hlNearAwardAnchor(cands, candCount, AWARD_FISH_FOOD,
                              timeline[i].tick)) continue;
        pills = timeline[i].carriedPills;
        c.anchorIdx = i;
        c.startTick = timeline[i].tick;
        c.endTick   = timeline[i].tick;
        c.mapX = timeline[i].mapX;
        c.mapY = timeline[i].mapY;
        c.type = HL_RARE_DEATH;
        c.awardId = 0;
        c.actorA = timeline[i].actorB;   /* the one who drowned */
        c.actorB = NEUTRAL;              /* the sea is not a killer */
        c.value = pills;
        c.score = (uint32_t)(HL_DROWN_WEIGHT + HL_DROWN_PILL_BONUS * pills);
        hlAppend(cands, &candCount, &c);
    }

    /* Turning point: the HL_TURN_WINDOW-tick span over which one team gained the
     * most map control. One per round — the moment the map stopped being even. */
    if (shifts != NULL && shiftCount > 0) {
        uint32_t bestSum = 0;
        int bestFirst = -1, bestLast = -1, bestDom = -1;
        uint8_t bestTeam = NEUTRAL;

        for (int i = 0; i < shiftCount; i++) {
            uint32_t sums[HL_TEAM_ID_MAX];
            uint32_t winEnd = shifts[i].tick + HL_TURN_WINDOW;
            memset(sums, 0, sizeof(sums));
            for (int j = i; j < shiftCount; j++) {
                if (shifts[j].tick < shifts[i].tick) continue;  /* out of order */
                if (shifts[j].tick > winEnd) break;
                if (shifts[j].gainTeam == NEUTRAL) continue;    /* lost to nobody */
                sums[shifts[j].gainTeam] += shifts[j].cellsFlipped;
            }
            for (int t = 0; t < HL_TEAM_ID_MAX; t++) {
                if (t == NEUTRAL || sums[t] <= bestSum) continue;
                bestSum = sums[t];
                bestTeam = (uint8_t)t;
                bestFirst = i;
            }
        }

        if (bestFirst >= 0 && bestSum > 0) {
            /* Re-walk the winning window for its span and its biggest single
             * flip, which is the cell worth pointing the clip at. */
            uint32_t winEnd = shifts[bestFirst].tick + HL_TURN_WINDOW;
            uint16_t domCells = 0;
            for (int j = bestFirst; j < shiftCount; j++) {
                if (shifts[j].tick < shifts[bestFirst].tick) continue;
                if (shifts[j].tick > winEnd) break;
                if (shifts[j].gainTeam != bestTeam) continue;
                bestLast = j;
                if (bestDom < 0 || shifts[j].cellsFlipped > domCells) {
                    domCells = shifts[j].cellsFlipped;
                    bestDom = j;
                }
            }
        }

        if (bestDom >= 0) {
            HlCand c;
            uint8_t rep = NEUTRAL;
            /* Name the swing after the lowest slot on the gaining team; a team
             * with nobody on it (or no roster at all) stays anonymous. */
            if (team != NULL) {
                for (int s = 0; s < MAX_TANKS; s++)
                    if (team[s] == bestTeam) { rep = (uint8_t)s; break; }
            }
            c.anchorIdx = -1;
            c.startTick = shifts[bestFirst].tick;
            c.endTick   = shifts[bestLast].tick;
            c.mapX = shifts[bestDom].mapX;
            c.mapY = shifts[bestDom].mapY;
            c.type = HL_TURNING_POINT;
            c.awardId = 0;
            c.actorA = rep;
            c.actorB = NEUTRAL;   /* a swing is a team's, not one player's */
            c.value = bestSum;
            c.score = HL_TURN_WEIGHT;
            hlAppend(cands, &candCount, &c);
        }
    }

    /* Front collapses: a team taking a cluster of ground off another over a short
     * window, gated on sustained fire in that area beforehand — so an assault that
     * was actually fought for is picked and a quiet handover is not. Greedy on the
     * shift series like the wipe loop: the next start jumps past the cluster just
     * emitted, so one assault yields one window. These are scored, not seeded; the
     * overlap cull drops any that land on the turning point's ticks. */
    if (shifts != NULL && shiftCount > 0) {
        uint32_t clusterEnd = 0;
        bool haveCluster = false;
        for (int i = 0; i < shiftCount; i++) {
            uint8_t gTeam = shifts[i].gainTeam;
            uint32_t winEnd = shifts[i].tick + HL_COLLAPSE_WINDOW;
            uint32_t sumCells = 0, sumDmg = 0;
            uint32_t lastTickC = shifts[i].tick;
            uint16_t domCells = 0;
            int domIdx = i;
            HlCand c;

            if (gTeam == NEUTRAL) continue;   /* ground must fall TO a team */
            if (haveCluster && shifts[i].tick <= clusterEnd) continue;

            for (int j = i; j < shiftCount; j++) {
                if (shifts[j].tick < shifts[i].tick) continue;   /* out of order */
                if (shifts[j].tick > winEnd) break;
                if (shifts[j].gainTeam != gTeam) continue;
                if (hlTileDist(shifts[i].mapX, shifts[i].mapY,
                               shifts[j].mapX, shifts[j].mapY) > HL_COLLAPSE_TILE_RADIUS)
                    continue;
                sumCells += shifts[j].cellsFlipped;
                sumDmg += shifts[j].recentDamage;
                if (shifts[j].tick > lastTickC) lastTickC = shifts[j].tick;
                if (shifts[j].cellsFlipped >= domCells) {
                    domCells = shifts[j].cellsFlipped;
                    domIdx = j;
                }
            }
            if (sumCells < HL_COLLAPSE_MIN_CELLS) continue;
            if (sumDmg < HL_COLLAPSE_MIN_DAMAGE) continue;   /* the damage gate */

            {
                uint8_t rep = NEUTRAL;
                if (team != NULL) {
                    for (int s = 0; s < MAX_TANKS; s++)
                        if (team[s] == gTeam) { rep = (uint8_t)s; break; }
                }
                c.anchorIdx = -1;
                c.startTick = shifts[i].tick;
                c.endTick   = lastTickC;
                c.mapX = shifts[domIdx].mapX;
                c.mapY = shifts[domIdx].mapY;
                c.type = HL_BREAKTHROUGH;
                c.awardId = 0;
                c.actorA = rep;
                c.actorB = NEUTRAL;
                c.value = sumCells;
                c.score = HL_COLLAPSE_WEIGHT;
                hlAppend(cands, &candCount, &c);
            }
            clusterEnd = lastTickC;
            haveCluster = true;
        }
    }

    /* Selection: strongest first, greedily keeping windows whose tick spans do
     * not overlap an accepted one, and no more than a few per time-bucket so the
     * reel spans the round instead of bunching where the scores run hottest. The
     * turning point is seeded ahead of the loop rather than left to win on score,
     * so the one window that explains how the round was decided always survives.
     * It carries its own type and at most one is ever produced, so the seed finds
     * it by type; the sort order it lands in does not matter. */
    {
        uint32_t span = lastTick > firstTick ? (lastTick - firstTick) : 0;
        bool bucketed = (span > 0);
        int bucketCount[HL_TIME_BUCKETS];
        /* Roughly even split of maxOut across the buckets, plus one slot of slack
         * so a hot bucket is trimmed, not starved. */
        int bucketCap = (maxOut + HL_TIME_BUCKETS - 1) / HL_TIME_BUCKETS + 1;

        for (int b = 0; b < HL_TIME_BUCKETS; b++) bucketCount[b] = 0;

        qsort(cands, candCount, sizeof(HlCand), hlCandCmp);
        for (int i = 0; i < candCount; i++)
            if (cands[i].type == HL_TURNING_POINT) { turnIdx = i; break; }
        if (turnIdx >= 0) {
            /* The seed is exempt from the cap but still fills its bucket's tally. */
            accepted[acceptCount++] = turnIdx;
            if (bucketed)
                bucketCount[hlBucket(cands[turnIdx].startTick, firstTick, span)]++;
        }
        for (int i = 0; i < candCount && acceptCount < maxOut; i++) {
            bool overlap = false;
            int b;
            if (i == turnIdx) continue;
            for (int k = 0; k < acceptCount; k++) {
                const HlCand *acc = &cands[accepted[k]];
                if (cands[i].startTick <= acc->endTick &&
                    acc->startTick <= cands[i].endTick) { overlap = true; break; }
            }
            if (overlap) continue;
            if (bucketed) {
                b = hlBucket(cands[i].startTick, firstTick, span);
                if (bucketCount[b] >= bucketCap) continue;   /* bucket full: spread */
                bucketCount[b]++;
            }
            accepted[acceptCount++] = i;
        }
    }

    /* Widen each pick with its lead-in, then order the reel chronologically. */
    for (int k = 0; k < acceptCount; k++) {
        const HlCand *c = &cands[accepted[k]];
        HighlightWindow *w = &tmp[k];
        uint32_t s = c->startTick, e = c->endTick;
        if (c->anchorIdx >= 0) {
            hlLeadIn(timeline, c, &s, &e);
        } else if (e < s + HL_CLIP_TICKS) {
            e = s + HL_CLIP_TICKS;   /* anchorless: still play as a clip, not an instant */
        }
        if (e < s) e = s;
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
