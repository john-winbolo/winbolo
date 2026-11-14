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

/*
 * The time left for a Bot Host's recording upload, for the lobby row:
 * "uploading recordings 42%, 1 of 2 bots, 0:03 left". The client takes a
 * sample each time CTRL_BOT_HOST_REC_BYTES comes in; the rate is the bytes
 * sent over about the last BOT_HOST_REC_RATE_WINDOW_MS, and the time left
 * is the bytes still to send at that rate, as H:MM, rounded up to the next
 * minute. "--:--" until the rate is known, and when no bytes came in for a
 * whole window.
 */

#ifndef BOT_HOST_REC_ETA_H
#define BOT_HOST_REC_ETA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BOT_HOST_REC_RATE_WINDOW_MS 10000u
/* The rate is not known until the samples cover this much time. */
#define BOT_HOST_REC_RATE_MIN_MS    1000u
#define BOT_HOST_REC_RATE_SAMPLES   32
/* The text while the time left is not known. */
#define BOT_HOST_REC_ETA_UNKNOWN    "--:--"
/* H:MM with H up to 99, and its NUL. */
#define BOT_HOST_REC_ETA_LEN        8

typedef struct {
    uint32_t ms[BOT_HOST_REC_RATE_SAMPLES];
    uint32_t kib[BOT_HOST_REC_RATE_SAMPLES];
    int      count;
    uint32_t totalKiB;
} BotHostRecRate;

static inline void botHostRecRateReset(BotHostRecRate *r) {
    memset(r, 0, sizeof(*r));
}

static inline void botHostRecRateDropFirst(BotHostRecRate *r) {
    memmove(r->ms, r->ms + 1, (size_t)(r->count - 1) * sizeof(r->ms[0]));
    memmove(r->kib, r->kib + 1, (size_t)(r->count - 1) * sizeof(r->kib[0]));
    r->count--;
}

/* Take sentKiB of totalKiB at nowMs. A count below the last one or another
 * total is a new upload: the samples start over. The oldest sample kept is
 * the newest one that is a whole window old, so the rate covers the
 * window. */
static inline void botHostRecRateSample(BotHostRecRate *r, uint32_t nowMs,
                                        uint32_t sentKiB, uint32_t totalKiB) {
    if (r->count > 0 && (totalKiB != r->totalKiB ||
                         sentKiB < r->kib[r->count - 1] ||
                         nowMs < r->ms[r->count - 1])) {
        botHostRecRateReset(r);
    }
    r->totalKiB = totalKiB;
    if (r->count == BOT_HOST_REC_RATE_SAMPLES) botHostRecRateDropFirst(r);
    r->ms[r->count] = nowMs;
    r->kib[r->count] = sentKiB;
    r->count++;
    while (r->count > 2 && nowMs - r->ms[1] >= BOT_HOST_REC_RATE_WINDOW_MS) {
        botHostRecRateDropFirst(r);
    }
}

/* The rate in KiB a second into *kibPerSec, or false while it is not known:
 * fewer than two samples, under BOT_HOST_REC_RATE_MIN_MS between them, no
 * bytes between them, or no sample in the last window. */
static inline bool botHostRecRateKiBps(const BotHostRecRate *r,
                                       uint32_t nowMs, double *kibPerSec) {
    uint32_t span, last;
    if (r->count < 2) return false;
    last = (uint32_t)(r->count - 1);
    if (nowMs - r->ms[last] >= BOT_HOST_REC_RATE_WINDOW_MS) return false;
    span = r->ms[last] - r->ms[0];
    if (span < BOT_HOST_REC_RATE_MIN_MS) return false;
    if (r->kib[last] <= r->kib[0]) return false;
    *kibPerSec = (double)(r->kib[last] - r->kib[0]) * 1000.0 / (double)span;
    return true;
}

/* remainingKiB at kibPerSec as H:MM, the minutes rounded up; 99:59 at most.
 * BOT_HOST_REC_ETA_UNKNOWN when kibPerSec is not above 0. */
static inline void botHostRecEtaFormat(uint32_t remainingKiB, double kibPerSec,
                                       char *out, size_t cap) {
    double minutes;
    uint32_t m;
    if (cap == 0) return;
    if (!(kibPerSec > 0.0)) {
        snprintf(out, cap, "%s", BOT_HOST_REC_ETA_UNKNOWN);
        return;
    }
    minutes = (double)remainingKiB / kibPerSec / 60.0;
    if (minutes >= 99.0 * 60.0 + 59.0) {
        m = 99u * 60u + 59u;
    } else {
        m = (uint32_t)minutes;
        if ((double)m < minutes) m++;
    }
    snprintf(out, cap, "%u:%02u", (unsigned)(m / 60u), (unsigned)(m % 60u));
}

/* The time left for r's upload at nowMs, as botHostRecEtaFormat. */
static inline void botHostRecRateEta(const BotHostRecRate *r, uint32_t nowMs,
                                     char *out, size_t cap) {
    double rate = 0.0;
    uint32_t sent;
    if (!botHostRecRateKiBps(r, nowMs, &rate)) {
        botHostRecEtaFormat(0, 0.0, out, cap);
        return;
    }
    sent = r->kib[r->count - 1];
    botHostRecEtaFormat(r->totalKiB > sent ? r->totalKiB - sent : 0, rate, out,
                        cap);
}

#endif /* BOT_HOST_REC_ETA_H */
