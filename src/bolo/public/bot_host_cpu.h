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

/* The estimate of how much of its machine's CPU a Bot Host (or the server,
 * for the bots it runs itself) uses for bot brains, as the lobby shows it:
 * "(Estimated 12% of the machine's CPU)".
 *
 * Each machine works out its own percent and sends only that one byte. No
 * core count, CPU name or other hardware detail leaves the machine.
 *
 * What is measured: the wall time of each brain think (BotContext
 * lastThinkMs, which the bot runners already measure for the think budget).
 * Over the last BOT_HOST_CPU_BUCKETS one-second buckets the meter adds the
 * think time up and divides it by (the wall time of those buckets x the
 * machine's logical cores). A think runs on one worker thread from start to
 * end, so its wall time is the core time it held, unless the system takes
 * the core away during the think; then the estimate is too high, never too
 * low. Thread CPU clocks were not used: on Windows GetThreadTimes moves in
 * 15.6 ms steps, which is longer than most thinks.
 *
 * Header only, so the Bot Host, the server, the client and the unit tests
 * share it without a new library. */

#ifndef WINBOLO_BOT_HOST_CPU_H
#define WINBOLO_BOT_HOST_CPU_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The byte on the wire (CTRL_BOT_HOST_CPU): 0..100 is a percent; these two
 * are the other values. Any other byte reads as unknown. */
#define BOT_HOST_CPU_UNKNOWN    0xFFu   /* not measured (or an older peer) */
#define BOT_HOST_CPU_UNDER_ONE  0xFEu   /* above 0% and below 1% */

/* The per-bot value (CTRL_BOT_HOST_CPU_BOT): the part of its machine's CPU
 * that one bot uses, in thousandths of a percent (1000 = 1%). It is the
 * think time of the last BOT_HOST_CPU_BUCKETS seconds divided by the bot
 * time of those seconds (the bots that ran, times the time they ran), so a
 * change in the bot count does not make it wrong. A machine with no bots,
 * or no thinks, keeps the last value it had (the last game's), and has
 * BOT_HOST_CPU_MILLI_UNKNOWN until it has one. */
#define BOT_HOST_CPU_MILLI_UNKNOWN 0xFFFFFFFFu
#define BOT_HOST_CPU_MILLI_MIN     1u        /* 0.001%: never 0 */
#define BOT_HOST_CPU_MILLI_MAX     100000u   /* 100% */

/* The per-bot value for busyMs of think time in botMs of bot time (one bot
 * for one ms is 1) on a machine with cores logical cores, or
 * BOT_HOST_CPU_MILLI_UNKNOWN when there is no think time or no bot time. */
static inline uint32_t botHostCpuMilliPerBot(double busyMs, double botMs,
                                             int cores) {
    double milli;
    if (!(busyMs > 0.0) || !(botMs > 0.0) || cores <= 0) {
        return BOT_HOST_CPU_MILLI_UNKNOWN;
    }
    milli = busyMs * 100.0 * 1000.0 / (botMs * (double)cores) + 0.5;
    if (milli < (double)BOT_HOST_CPU_MILLI_MIN) return BOT_HOST_CPU_MILLI_MIN;
    if (milli > (double)BOT_HOST_CPU_MILLI_MAX) return BOT_HOST_CPU_MILLI_MAX;
    return (uint32_t)milli;
}

/* The full-budget estimate of one bot's part of its machine's CPU, in the
 * same thousandths of a percent: a bot that uses its whole think budget
 * (budgetMs, the bot runners' per-bot target) once every everyMs (the time
 * between thinks) holds budgetMs / everyMs of one core, and the machine has
 * cores logical cores. The most a bot can use while the runners keep to
 * their budget, so a measured value is at or below it. Unknown when any
 * input is missing. */
static inline uint32_t botHostCpuMilliBudget(double budgetMs, double everyMs,
                                             int cores) {
    return botHostCpuMilliPerBot(budgetMs, everyMs, cores);
}

/* Where a per-bot value the lobby shows comes from, in this order: the
 * value measured from the bots thinking now; the last value measured for
 * that host in this server session; the full-budget estimate; none (the
 * lobby then says the CPU test is running). The round log records it. */
#define BOT_HOST_CPU_SRC_NONE      0u
#define BOT_HOST_CPU_SRC_MEASURED  1u
#define BOT_HOST_CPU_SRC_LAST      2u
#define BOT_HOST_CPU_SRC_BUDGET    3u

/* A per-bot value read off the wire: 0 and values past 100% read as
 * unknown. */
static inline uint32_t botHostCpuMilliFromWire(uint32_t v) {
    if (v < BOT_HOST_CPU_MILLI_MIN || v > BOT_HOST_CPU_MILLI_MAX) {
        return BOT_HOST_CPU_MILLI_UNKNOWN;
    }
    return v;
}

/* The lobby's text for a percent, never "0": 10 or more as a whole number
 * ("12"), 1 to under 10 with one decimal ("3.4"), under 1 with two
 * significant digits ("0.42", "0.035"). The smallest value shown is 0.001. */
static inline void botHostCpuFormatPct(double pct, char *out, size_t cap) {
    double r;
    int dec;
    if (out == NULL || cap == 0) return;
    if (!(pct >= 0.001)) pct = 0.001;   /* also a NaN */
    if (pct > 100.0) pct = 100.0;
    r = floor(pct + 0.5);
    if (r >= 10.0) {
        snprintf(out, cap, "%.0f", r);
        return;
    }
    r = floor(pct * 10.0 + 0.5) / 10.0;
    if (r >= 1.0) {
        if (r >= 10.0) snprintf(out, cap, "10");
        else snprintf(out, cap, "%.1f", r);
        return;
    }
    dec = pct >= 0.1 ? 2 : 3;
    r = floor(pct * pow(10.0, dec) + 0.5) / pow(10.0, dec);
    if (r >= 1.0) {
        snprintf(out, cap, "1.0");
        return;
    }
    if (dec == 3 && r >= 0.1) dec = 2;
    if (r < 0.001) r = 0.001;
    snprintf(out, cap, "%.*f", dec, r);
}

static inline void botHostCpuFormatMilli(uint32_t milli, char *out,
                                         size_t cap) {
    botHostCpuFormatPct((double)milli / 1000.0, out, cap);
}

#define BOT_HOST_CPU_BUCKET_NS  1000000000ull   /* one second */
#define BOT_HOST_CPU_BUCKETS    5               /* the last 5 s */

/* The byte for busyMs of think time in wallMs of wall time on a machine
 * with cores logical cores. */
static inline uint8_t botHostCpuPercent(double busyMs, double wallMs,
                                        int cores) {
    double pct;
    if (!(wallMs > 0.0) || cores <= 0 || !(busyMs >= 0.0)) {
        return (uint8_t)BOT_HOST_CPU_UNKNOWN;
    }
    pct = busyMs * 100.0 / (wallMs * (double)cores);
    if (pct <= 0.0) return 0;
    if (pct < 1.0) return (uint8_t)BOT_HOST_CPU_UNDER_ONE;
    if (pct >= 100.0) return 100;
    pct += 0.5;
    return (uint8_t)(pct >= 100.0 ? 100 : (int)pct);
}

/* A byte read off the wire: values the sender cannot mean read as
 * unknown. */
static inline uint8_t botHostCpuFromWire(uint8_t b) {
    if (b <= 100 || b == BOT_HOST_CPU_UNDER_ONE) return b;
    return (uint8_t)BOT_HOST_CPU_UNKNOWN;
}

typedef struct {
    bool     started;
    uint64_t bucketStartNs;
    double   bucketStartTotalMs;   /* totalThinkMs at the bucket's start */
    double   busyMs[BOT_HOST_CPU_BUCKETS];
    double   wallMs[BOT_HOST_CPU_BUCKETS];
    double   botMs[BOT_HOST_CPU_BUCKETS];  /* bots x ms in the bucket */
    double   bucketBotMs;          /* the same for the bucket now */
    uint64_t lastNs;               /* the last sample */
    int      filled;               /* buckets that hold a value */
    int      next;                 /* the bucket written next */
    uint8_t  value;                /* the last estimate (the wire byte) */
    uint32_t perBotMilli;          /* the per-bot value; kept with no bots */
} BotHostCpuMeter;

static inline void botHostCpuMeterInit(BotHostCpuMeter *m) {
    memset(m, 0, sizeof(*m));
    m->value = (uint8_t)BOT_HOST_CPU_UNKNOWN;
    m->perBotMilli = BOT_HOST_CPU_MILLI_UNKNOWN;
}

/* Call it often (each tick). nowNs is a steady clock; totalThinkMs is the
 * running sum of think time, which only grows (a smaller total, after a
 * reset, counts as no think time). bots is the number of bots the machine
 * runs now: with none the estimate is 0. Gives the current estimate. */
static inline uint8_t botHostCpuMeterSample(BotHostCpuMeter *m,
                                            uint64_t nowNs,
                                            double totalThinkMs,
                                            int cores, int bots) {
    if (!m->started) {
        m->started = true;
        m->bucketStartNs = nowNs;
        m->bucketStartTotalMs = totalThinkMs;
        m->bucketBotMs = 0.0;
    } else if (nowNs < m->bucketStartNs || nowNs < m->lastNs) {
        m->bucketStartNs = nowNs;
        m->bucketStartTotalMs = totalThinkMs;
        m->bucketBotMs = 0.0;
    } else {
        /* The bots that ran since the last sample, for the per-bot value. */
        if (bots > 0) {
            m->bucketBotMs += (double)bots * (double)(nowNs - m->lastNs) / 1.0e6;
        }
        if (nowNs - m->bucketStartNs >= BOT_HOST_CPU_BUCKET_NS) {
            double busy = totalThinkMs - m->bucketStartTotalMs;
            double sumBusy = 0.0, sumWall = 0.0, sumBot = 0.0;
            uint32_t perBot;
            int i;
            if (!(busy > 0.0)) busy = 0.0;
            m->busyMs[m->next] = busy;
            m->wallMs[m->next] = (double)(nowNs - m->bucketStartNs) / 1.0e6;
            m->botMs[m->next] = m->bucketBotMs;
            m->next = (m->next + 1) % BOT_HOST_CPU_BUCKETS;
            if (m->filled < BOT_HOST_CPU_BUCKETS) m->filled++;
            for (i = 0; i < m->filled; i++) {
                sumBusy += m->busyMs[i];
                sumWall += m->wallMs[i];
                sumBot += m->botMs[i];
            }
            m->value = botHostCpuPercent(sumBusy, sumWall, cores);
            perBot = botHostCpuMilliPerBot(sumBusy, sumBot, cores);
            if (perBot != BOT_HOST_CPU_MILLI_UNKNOWN) m->perBotMilli = perBot;
            m->bucketStartNs = nowNs;
            m->bucketStartTotalMs = totalThinkMs;
            m->bucketBotMs = 0.0;
        }
    }
    m->lastNs = nowNs;
    if (bots <= 0) return 0;
    return m->value;
}

#endif /* WINBOLO_BOT_HOST_CPU_H */
