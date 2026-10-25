/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * How the log viewer writes a playback time, and reads one back.
 *
 * A time is minutes and seconds, with the hours in front from one hour on:
 * "19:48" and "1:02:07". With the hundredths it is "19:48.22" and
 * "1:02:07.22". The mark before the hundredths is LV_TIME_FRACTION_MARK, here
 * and nowhere else, so it is one line to change.
 *
 * The hundredths are cut, not rounded: 59.999 seconds reads "00:59.99". A
 * readout never shows a time playback has not reached. Playback moves in
 * steps of 20 ms, so the last digit is always even.
 *
 * Header only, no SDL and no ImGui: the unit tests include it.
 */

#ifndef LV_TIME_FMT_H
#define LV_TIME_FMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The mark between the seconds and the hundredths. */
#define LV_TIME_FRACTION_MARK '.'

/* The longest text lvTimeFormat writes, with its end: a uint32_t of
   milliseconds is under 1194 hours, "1193:02:47.29". */
#define LV_TIME_TEXT_MAX 16

/* Write ms (a length of time, from the start of what is shown) to dest.
   hundredths: nonzero adds the mark and two digits. Returns the number of
   characters written, as snprintf does. */
static inline int lvTimeFormat(uint32_t ms, int hundredths, char *dest, size_t size) {
    uint32_t secs = ms / 1000u;
    uint32_t cs = (ms % 1000u) / 10u;
    uint32_t h = secs / 3600u;
    uint32_t m = (secs / 60u) % 60u;
    uint32_t s = secs % 60u;

    if (dest == NULL || size == 0) return 0;
    if (h > 0) {
        if (hundredths) {
            return snprintf(dest, size, "%u:%02u:%02u%c%02u", (unsigned)h, (unsigned)m,
                            (unsigned)s, LV_TIME_FRACTION_MARK, (unsigned)cs);
        }
        return snprintf(dest, size, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    }
    if (hundredths) {
        return snprintf(dest, size, "%02u:%02u%c%02u", (unsigned)m, (unsigned)s,
                        LV_TIME_FRACTION_MARK, (unsigned)cs);
    }
    return snprintf(dest, size, "%02u:%02u", (unsigned)m, (unsigned)s);
}

/* Read a time back: "ss", "mm:ss" or "h:mm:ss", each with or without the mark
   and one to three digits after it ("7.5" is 7500 ms). Spaces round it are
   passed over. Returns 1 and sets *ms, or 0 for a text that is not a time:
   empty, a letter in it, more than three parts, seconds or minutes over 59
   behind a larger part, or a time too long for a uint32_t. */
static inline int lvTimeParse(const char *text, uint32_t *ms) {
    uint32_t part[3] = { 0u, 0u, 0u };
    int parts = 0;
    int digits = 0;
    uint32_t frac = 0u;
    uint64_t total;
    const char *p = text;

    if (text == NULL || ms == NULL) return 0;
    while (*p == ' ' || *p == '\t') p++;
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            if (digits >= 6) return 0;
            part[parts] = part[parts] * 10u + (uint32_t)(*p - '0');
            digits++;
            p++;
        } else if (*p == ':') {
            if (digits == 0 || parts >= 2) return 0;
            parts++;
            digits = 0;
            p++;
        } else {
            break;
        }
    }
    if (digits == 0) return 0;
    parts++;
    if (*p == LV_TIME_FRACTION_MARK) {
        uint32_t scale = 100u;
        int n = 0;
        p++;
        while (*p >= '0' && *p <= '9') {
            if (n >= 3) return 0;
            frac += (uint32_t)(*p - '0') * scale;
            scale /= 10u;
            n++;
            p++;
        }
        if (n == 0) return 0;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0') return 0;

    if (parts == 1) {
        total = (uint64_t)part[0] * 1000u;
    } else if (parts == 2) {
        if (part[1] > 59u) return 0;
        total = ((uint64_t)part[0] * 60u + part[1]) * 1000u;
    } else {
        if (part[1] > 59u || part[2] > 59u) return 0;
        total = ((uint64_t)part[0] * 3600u + (uint64_t)part[1] * 60u + part[2]) * 1000u;
    }
    total += frac;
    if (total > 0xFFFFFFFFu) return 0;
    *ms = (uint32_t)total;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* LV_TIME_FMT_H */
