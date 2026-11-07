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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The byte on the wire (CTRL_BOT_HOST_CPU): 0..100 is a percent; these two
 * are the other values. Any other byte reads as unknown. */
#define BOT_HOST_CPU_UNKNOWN    0xFFu   /* not measured (or an older peer) */
#define BOT_HOST_CPU_UNDER_ONE  0xFEu   /* above 0% and below 1% */

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
    int      filled;               /* buckets that hold a value */
    int      next;                 /* the bucket written next */
    uint8_t  value;                /* the last estimate (the wire byte) */
} BotHostCpuMeter;

static inline void botHostCpuMeterInit(BotHostCpuMeter *m) {
    memset(m, 0, sizeof(*m));
    m->value = (uint8_t)BOT_HOST_CPU_UNKNOWN;
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
    } else if (nowNs < m->bucketStartNs) {
        m->bucketStartNs = nowNs;
        m->bucketStartTotalMs = totalThinkMs;
    } else if (nowNs - m->bucketStartNs >= BOT_HOST_CPU_BUCKET_NS) {
        double busy = totalThinkMs - m->bucketStartTotalMs;
        double sumBusy = 0.0, sumWall = 0.0;
        int i;
        if (!(busy > 0.0)) busy = 0.0;
        m->busyMs[m->next] = busy;
        m->wallMs[m->next] = (double)(nowNs - m->bucketStartNs) / 1.0e6;
        m->next = (m->next + 1) % BOT_HOST_CPU_BUCKETS;
        if (m->filled < BOT_HOST_CPU_BUCKETS) m->filled++;
        for (i = 0; i < m->filled; i++) {
            sumBusy += m->busyMs[i];
            sumWall += m->wallMs[i];
        }
        m->value = botHostCpuPercent(sumBusy, sumWall, cores);
        m->bucketStartNs = nowNs;
        m->bucketStartTotalMs = totalThinkMs;
    }
    if (bots <= 0) return 0;
    return m->value;
}

#endif /* WINBOLO_BOT_HOST_CPU_H */
