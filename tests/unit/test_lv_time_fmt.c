/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The log viewer's playback times as text, and the ring it flashes on the map
 * round the place of an event (lv_time_fmt.h, lv_event_flash.h).
 *
 *   lv_time_fmt_sums — a time is written "mm:ss.cc", with the hours in front
 *       from one hour on; the hundredths are cut, not rounded; every 20 ms
 *       and every 10 ms step has a text of its own.
 *   lv_time_parse_sums — the text is read back to the same time; what is not
 *       a time is refused.
 *   lv_event_flash_sums — how solid and how wide the ring is at its start,
 *       its middle and its end; when the map has to move to show a square.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lv_event_flash.h"
#include "lv_time_fmt.h"
#include "test_harness.h"

static bool tf_is(uint32_t ms, int hundredths, const char *want) {
    char got[LV_TIME_TEXT_MAX];
    int n = lvTimeFormat(ms, hundredths, got, sizeof(got));
    return strcmp(got, want) == 0 && n == (int)strlen(want);
}

static bool tf_near(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.001f;
}

int run_lv_time_fmt_sums(void) {
    char small[6];
    char prev[LV_TIME_TEXT_MAX];
    char text[LV_TIME_TEXT_MAX];
    uint32_t ms;

    UT_ASSERT(LV_TIME_FRACTION_MARK == '.');

    UT_ASSERT_MSG(tf_is(0u, 1, "00:00.00"), "the start");
    UT_ASSERT_MSG(tf_is(0u, 0, "00:00"), "the start, to the second");
    UT_ASSERT_MSG(tf_is(20u, 1, "00:00.02"), "one 20 ms tick");
    UT_ASSERT_MSG(tf_is(10u, 1, "00:00.01"), "one 10 ms tick");
    UT_ASSERT_MSG(tf_is(59990u, 1, "00:59.99"), "59.99 s");
    UT_ASSERT_MSG(tf_is(59999u, 1, "00:59.99"), "59.999 s is cut, not rounded up");
    UT_ASSERT_MSG(tf_is(60000u, 1, "01:00.00"), "60.00 s");
    UT_ASSERT_MSG(tf_is(1188220u, 1, "19:48.22"), "19 min 48.22 s");
    UT_ASSERT_MSG(tf_is(1188220u, 0, "19:48"), "the same to the second");
    UT_ASSERT_MSG(tf_is(3599990u, 1, "59:59.99"), "the last hundredth under an hour");
    UT_ASSERT_MSG(tf_is(3600000u, 1, "1:00:00.00"), "one hour");
    UT_ASSERT_MSG(tf_is(3600000u, 0, "1:00:00"), "one hour, to the second");
    UT_ASSERT_MSG(tf_is(3727220u, 1, "1:02:07.22"), "1 h 2 min 7.22 s");
    UT_ASSERT_MSG(tf_is(0xFFFFFFFFu, 1, "1193:02:47.29"), "the longest time");
    UT_ASSERT_MSG(strlen("1193:02:47.29") < (size_t)LV_TIME_TEXT_MAX,
                  "LV_TIME_TEXT_MAX holds the longest time");

    /* A short buffer is not run over, and no buffer is not written. */
    lvTimeFormat(1188220u, 1, small, sizeof(small));
    UT_ASSERT_MSG(strcmp(small, "19:48") == 0, "a 6-character buffer got \"%s\"", small);
    UT_ASSERT(lvTimeFormat(1u, 1, NULL, 8) == 0);
    UT_ASSERT(lvTimeFormat(1u, 1, small, 0) == 0);

    /* Every step of a 20 ms log, and of a 10 ms one, has a text of its own
       across a minute's end: no two steps read the same. */
    lvTimeFormat(58980u, 1, prev, sizeof(prev));
    for (ms = 59000u; ms <= 61000u; ms += 20u) {
        lvTimeFormat(ms, 1, text, sizeof(text));
        UT_ASSERT_MSG(strcmp(text, prev) != 0, "20 ms steps: %u ms reads \"%s\" twice",
                      (unsigned)ms, text);
        memcpy(prev, text, sizeof(prev));
    }
    lvTimeFormat(58990u, 1, prev, sizeof(prev));
    for (ms = 59000u; ms <= 61000u; ms += 10u) {
        lvTimeFormat(ms, 1, text, sizeof(text));
        UT_ASSERT_MSG(strcmp(text, prev) != 0, "10 ms steps: %u ms reads \"%s\" twice",
                      (unsigned)ms, text);
        memcpy(prev, text, sizeof(prev));
    }
    return 0;
}

static bool tp_is(const char *text, uint32_t want) {
    uint32_t got = 0xDEADBEEFu;
    return lvTimeParse(text, &got) == 1 && got == want;
}

static bool tp_bad(const char *text) {
    uint32_t got = 77u;
    return lvTimeParse(text, &got) == 0 && got == 77u;
}

int run_lv_time_parse_sums(void) {
    char text[LV_TIME_TEXT_MAX];
    uint32_t ms, back;

    UT_ASSERT_MSG(tp_is("00:00.00", 0u), "the start");
    UT_ASSERT_MSG(tp_is("19:48.22", 1188220u), "mm:ss.cc");
    UT_ASSERT_MSG(tp_is("19:48", 1188000u), "mm:ss");
    UT_ASSERT_MSG(tp_is("1:02:07.22", 3727220u), "h:mm:ss.cc");
    UT_ASSERT_MSG(tp_is("1:02:07", 3727000u), "h:mm:ss");
    UT_ASSERT_MSG(tp_is("7", 7000u), "seconds alone");
    UT_ASSERT_MSG(tp_is("7.5", 7500u), "one digit after the mark is tenths");
    UT_ASSERT_MSG(tp_is("7.05", 7050u), "two digits are hundredths");
    UT_ASSERT_MSG(tp_is("7.123", 7123u), "three digits are milliseconds");
    UT_ASSERT_MSG(tp_is("90", 90000u), "seconds alone may pass 59");
    UT_ASSERT_MSG(tp_is("75:03", 4503000u), "minutes in front may pass 59");
    UT_ASSERT_MSG(tp_is("  19:48.22 ", 1188220u), "spaces round it");
    UT_ASSERT_MSG(tp_is("1193:02:47.29", 4294967290u), "the longest time written");

    UT_ASSERT_MSG(tp_bad(""), "empty");
    UT_ASSERT_MSG(tp_bad("   "), "spaces only");
    UT_ASSERT_MSG(tp_bad("abc"), "letters");
    UT_ASSERT_MSG(tp_bad("19:48x"), "a letter after it");
    UT_ASSERT_MSG(tp_bad("19:"), "nothing after a colon");
    UT_ASSERT_MSG(tp_bad(":48"), "nothing before a colon");
    UT_ASSERT_MSG(tp_bad("19:48."), "the mark with no digit");
    UT_ASSERT_MSG(tp_bad("19:48.1234"), "four digits after the mark");
    UT_ASSERT_MSG(tp_bad("19:60"), "60 seconds behind minutes");
    UT_ASSERT_MSG(tp_bad("1:60:00"), "60 minutes behind hours");
    UT_ASSERT_MSG(tp_bad("1:2:3:4"), "four parts");
    UT_ASSERT_MSG(tp_bad("-5"), "a sign");
    UT_ASSERT_MSG(tp_bad("1193:02:47.30"), "more than a uint32_t of ms");
    UT_ASSERT_MSG(tp_bad("9999999"), "a part too long");
    UT_ASSERT(lvTimeParse(NULL, &ms) == 0);
    UT_ASSERT(lvTimeParse("1", NULL) == 0);

    /* What is written is read back to the same time, at each 10 ms step
       across the end of a minute and the end of an hour. */
    for (ms = 59000u; ms <= 61000u; ms += 10u) {
        lvTimeFormat(ms, 1, text, sizeof(text));
        back = 0u;
        UT_ASSERT_MSG(lvTimeParse(text, &back) == 1 && back == ms,
                      "\"%s\" read back as %u, want %u", text, (unsigned)back, (unsigned)ms);
    }
    for (ms = 3599000u; ms <= 3601000u; ms += 10u) {
        lvTimeFormat(ms, 1, text, sizeof(text));
        back = 0u;
        UT_ASSERT_MSG(lvTimeParse(text, &back) == 1 && back == ms,
                      "\"%s\" read back as %u, want %u", text, (unsigned)back, (unsigned)ms);
    }
    return 0;
}

int run_lv_event_flash_sums(void) {
    uint32_t t;
    float lastAlpha = 2.0f, lastRadius = 0.0f;

    /* The start: full strength, at the starting radius. */
    UT_ASSERT(lvFlashActive(0u) == 1);
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(0u), 1.0f), "alpha at the start");
    UT_ASSERT_MSG(tf_near(lvFlashRadiusTiles(0u), LV_FLASH_RADIUS_TILES), "radius at the start");

    /* The end of the hold: still full strength. */
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(300u), 1.0f), "alpha at the end of the hold");

    /* The middle: a third of the fade is done (0.5 is 1/3 of the way from
       0.25 to 1), and three quarters of the growth (1 - 0.5 * 0.5). */
    UT_ASSERT(lvFlashActive(600u) == 1);
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(600u), 2.0f / 3.0f), "alpha in the middle is %f",
                  (double)lvFlashAlpha(600u));
    UT_ASSERT_MSG(tf_near(lvFlashRadiusTiles(600u),
                          LV_FLASH_RADIUS_TILES + LV_FLASH_GROW_TILES * 0.75f),
                  "radius in the middle is %f", (double)lvFlashRadiusTiles(600u));

    /* The end: gone, at the full radius; and it stays so. */
    UT_ASSERT(lvFlashActive(LV_FLASH_MS - 1u) == 1);
    UT_ASSERT(lvFlashActive(LV_FLASH_MS) == 0);
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(LV_FLASH_MS), 0.0f), "alpha at the end");
    UT_ASSERT_MSG(tf_near(lvFlashRadiusTiles(LV_FLASH_MS),
                          LV_FLASH_RADIUS_TILES + LV_FLASH_GROW_TILES), "radius at the end");
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(900000u), 0.0f), "alpha long after the end");
    UT_ASSERT(lvFlashActive(0xFFFFFFFFu) == 0);

    /* It never gets more solid or smaller on the way. */
    for (t = 0u; t <= LV_FLASH_MS; t += 10u) {
        float a = lvFlashAlpha(t), r = lvFlashRadiusTiles(t);
        UT_ASSERT_MSG(a <= lastAlpha + 0.0001f && a >= 0.0f && a <= 1.0f,
                      "alpha went up at %u ms", (unsigned)t);
        UT_ASSERT_MSG(r >= lastRadius - 0.0001f, "the ring shrank at %u ms", (unsigned)t);
        lastAlpha = a;
        lastRadius = r;
    }

    /* A view of 20 squares from square 100: squares 100 to 119. With 2
       squares of edge, 102 to 117 need no move. */
    UT_ASSERT(lvFlashAxisNeedsCentre(110, 100, 20, 2) == 0);
    UT_ASSERT(lvFlashAxisNeedsCentre(102, 100, 20, 2) == 0);
    UT_ASSERT(lvFlashAxisNeedsCentre(117, 100, 20, 2) == 0);
    UT_ASSERT(lvFlashAxisNeedsCentre(101, 100, 20, 2) == 1);
    UT_ASSERT(lvFlashAxisNeedsCentre(118, 100, 20, 2) == 1);
    UT_ASSERT(lvFlashAxisNeedsCentre(99, 100, 20, 2) == 1);
    UT_ASSERT(lvFlashAxisNeedsCentre(120, 100, 20, 2) == 1);
    UT_ASSERT(lvFlashAxisNeedsCentre(0, 100, 20, 2) == 1);
    UT_ASSERT(lvFlashAxisNeedsCentre(255, 100, 20, 2) == 1);
    /* A view with no size (not yet sized by its host) is not moved. */
    UT_ASSERT(lvFlashAxisNeedsCentre(5, 0, 0, 2) == 0);
    /* A view too small to have a middle clear of both edges always moves. */
    UT_ASSERT(lvFlashAxisNeedsCentre(101, 100, 4, 2) == 1);

    /* Both axes: either one is enough. */
    UT_ASSERT(lvFlashNeedsCentre(110, 60, 100, 50, 20, 20, 2) == 0);
    UT_ASSERT(lvFlashNeedsCentre(130, 60, 100, 50, 20, 20, 2) == 1);
    UT_ASSERT(lvFlashNeedsCentre(110, 69, 100, 50, 20, 20, 2) == 1);
    return 0;
}
