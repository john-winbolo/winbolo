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
 *   lv_event_flash_sums — how solid and how wide the ring is through its
 *       two pulses.
 *   lv_event_centre_scroll — where the view goes to put a square in the
 *       middle of the visible map area: each sidebar layout, several zooms,
 *       the map's edges.
 *   lv_event_flash_arrow — the arrow at the view's edge for a place the view
 *       does not show; whose the view is after a jump.
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
    uint32_t t, p;
    float lastAlpha, lastRadius;

    /* Two pulses of one second: long enough and large enough to be seen. */
    UT_ASSERT(LV_FLASH_PULSES == 2u);
    UT_ASSERT(LV_FLASH_MS == 2000u);
    UT_ASSERT(LV_FLASH_RADIUS_TILES >= 3.0f);
    UT_ASSERT(LV_FLASH_RING_PT >= 2.0f && LV_FLASH_RING_PT <= 3.0f);

    /* The start: full strength, at the starting radius. */
    UT_ASSERT(lvFlashActive(0u) == 1);
    UT_ASSERT(lvFlashPulse(0u) == 0u);
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(0u), 1.0f), "alpha at the start");
    UT_ASSERT_MSG(tf_near(lvFlashRadiusTiles(0u), LV_FLASH_RADIUS_TILES), "radius at the start");

    /* The end of the hold, half a pulse in: still full strength, and three
       quarters of the way in (1 - 0.5 * 0.5). */
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(500u), 1.0f), "alpha at the end of the hold");
    UT_ASSERT_MSG(tf_near(lvFlashRadiusTiles(500u),
                          LV_FLASH_RADIUS_TILES -
                              (LV_FLASH_RADIUS_TILES - LV_FLASH_END_TILES) * 0.75f),
                  "radius half a pulse in is %f", (double)lvFlashRadiusTiles(500u));

    /* Three quarters through the first pulse: half its fade is done, and it
       fades only to the floor. */
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(750u), 1.0f - (1.0f - LV_FLASH_FLOOR) * 0.5f),
                  "alpha at 750 ms is %f", (double)lvFlashAlpha(750u));
    UT_ASSERT_MSG(lvFlashAlpha(999u) >= LV_FLASH_FLOOR - 0.0001f,
                  "the first pulse faded under its floor: %f", (double)lvFlashAlpha(999u));

    /* The second pulse starts again: full strength, out at the start. */
    UT_ASSERT(lvFlashPulse(1000u) == 1u);
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(1000u), 1.0f), "alpha at the second pulse");
    UT_ASSERT_MSG(tf_near(lvFlashRadiusTiles(1000u), LV_FLASH_RADIUS_TILES),
                  "radius at the second pulse");
    /* Three quarters through it: half the way to nothing. */
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(1750u), 0.5f), "alpha at 1750 ms is %f",
                  (double)lvFlashAlpha(1750u));

    /* The end: gone, closed in on the place; and it stays so. */
    UT_ASSERT(lvFlashActive(LV_FLASH_MS - 1u) == 1);
    UT_ASSERT(lvFlashActive(LV_FLASH_MS) == 0);
    UT_ASSERT(lvFlashPulse(LV_FLASH_MS) == LV_FLASH_PULSES);
    UT_ASSERT_MSG(lvFlashAlpha(LV_FLASH_MS - 1u) < 0.01f, "alpha just before the end");
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(LV_FLASH_MS), 0.0f), "alpha at the end");
    UT_ASSERT_MSG(tf_near(lvFlashRadiusTiles(LV_FLASH_MS), LV_FLASH_END_TILES),
                  "radius at the end");
    UT_ASSERT_MSG(tf_near(lvFlashAlpha(900000u), 0.0f), "alpha long after the end");
    UT_ASSERT(lvFlashActive(0xFFFFFFFFu) == 0);

    /* Inside a pulse it never gets more solid or larger on the way. */
    for (p = 0u; p < LV_FLASH_PULSES; p++) {
        lastAlpha = 2.0f;
        lastRadius = 1000.0f;
        for (t = 0u; t < LV_FLASH_PULSE_MS; t += 10u) {
            uint32_t at = p * LV_FLASH_PULSE_MS + t;
            float a = lvFlashAlpha(at), r = lvFlashRadiusTiles(at);
            UT_ASSERT_MSG(a <= lastAlpha + 0.0001f && a >= 0.0f && a <= 1.0f,
                          "alpha went up at %u ms", (unsigned)at);
            UT_ASSERT_MSG(r <= lastRadius + 0.0001f && r >= LV_FLASH_END_TILES - 0.0001f,
                          "the ring grew at %u ms", (unsigned)at);
            lastAlpha = a;
            lastRadius = r;
        }
    }

    return 0;
}

/* Where the view goes to put a square in the middle of the visible map
   area. The window is 1280 by 800 points with a 20 point menu bar; a square
   is 16 map pixels; the map is 256 squares and the grid here shows up to the
   map's edge, so the furthest the view goes is (255 - squares shown) * 16. */
int run_lv_event_centre_scroll(void) {
    static const struct { int l, t, r, b; } docks[] = {
        { 0, 0, 0, 0 },        /* none */
        { 300, 0, 0, 0 },      /* left */
        { 0, 0, 300, 0 },      /* right */
        { 0, 120, 0, 0 },      /* top */
        { 0, 0, 0, 120 },      /* bottom */
        { 300, 120, 220, 100 } /* all four */
    };
    static const float zooms[] = { 0.5f, 0.75f, 1.0f, 2.0f, 3.0f };
    size_t d, z;

    for (d = 0; d < sizeof(docks) / sizeof(docks[0]); d++) {
        for (z = 0; z < sizeof(zooms) / sizeof(zooms[0]); z++) {
            float zoom = zooms[z];
            float areaW = (float)(1280 - docks[d].l - docks[d].r);
            float areaH = (float)(800 - 20 - docks[d].t - docks[d].b);
            int shownX = (int)(areaW / (16.0f * zoom)) + 1;
            int shownY = (int)(areaH / (16.0f * zoom)) + 1;
            int maxX = (255 - shownX) * 16, maxY = (255 - shownY) * 16;
            int sx, sy;
            float midX, midY;
            if (maxX < 0) maxX = 0;
            if (maxY < 0) maxY = 0;

            /* A square in the middle of the map: its middle comes to the
               middle of the area, to within a point. The place on screen, in
               points from the area's corner, is (map pixel - scroll) * zoom. */
            sx = lvFlashCentreScroll(128, areaW / zoom, 16, maxX);
            sy = lvFlashCentreScroll(120, areaH / zoom, 16, maxY);
            midX = ((128.0f + 0.5f) * 16.0f - (float)sx) * zoom;
            midY = ((120.0f + 0.5f) * 16.0f - (float)sy) * zoom;
            if (sx > 0 && sx < maxX) {
                UT_ASSERT_MSG(midX > areaW * 0.5f - zoom && midX < areaW * 0.5f + zoom,
                              "dock %d zoom %.2f: the place is at %.1f of %.1f points across",
                              (int)d, (double)zoom, (double)midX, (double)areaW);
            }
            if (sy > 0 && sy < maxY) {
                UT_ASSERT_MSG(midY > areaH * 0.5f - zoom && midY < areaH * 0.5f + zoom,
                              "dock %d zoom %.2f: the place is at %.1f of %.1f points down",
                              (int)d, (double)zoom, (double)midY, (double)areaH);
            }
            /* At 1x and over, the middle of the map is never held by an edge. */
            if (zoom >= 1.0f) {
                UT_ASSERT_MSG(sx > 0 && sx < maxX && sy > 0 && sy < maxY,
                              "dock %d zoom %.2f: the middle of the map was held at an edge",
                              (int)d, (double)zoom);
            }

            /* The corners of the map: held at the map's edge, never past it. */
            UT_ASSERT(lvFlashCentreScroll(0, areaW / zoom, 16, maxX) == 0);
            UT_ASSERT(lvFlashCentreScroll(2, areaH / zoom, 16, maxY) == 0);
            UT_ASSERT(lvFlashCentreScroll(255, areaW / zoom, 16, maxX) == maxX);
            UT_ASSERT(lvFlashCentreScroll(254, areaH / zoom, 16, maxY) == maxY);
        }
    }

    /* By hand, 1x, no sidebars: 1280 points show 1280 map pixels. Square 128
       has its middle at pixel 2056; less half of 1280 is 1416. With a 300
       point sidebar the area is 980: 2056 - 490 = 1566. The old whole-square
       sum (square - squares shown / 2, 80 and 61 squares) gave 1408 and 1568:
       8 and 2 pixels off, and to one side. */
    UT_ASSERT(lvFlashCentreScroll(128, 1280.0f, 16, 4000) == 1416);
    UT_ASSERT(lvFlashCentreScroll(128, 980.0f, 16, 4000) == 1566);
    UT_ASSERT((128 - 80 / 2) * 16 != 1416);
    /* At 2x the same 1280 points show 640 map pixels: 2056 - 320. */
    UT_ASSERT(lvFlashCentreScroll(128, 1280.0f / 2.0f, 16, 4000) == 1736);
    /* A limit under 0 is 0. */
    UT_ASSERT(lvFlashCentreScroll(128, 640.0f, 16, -5) == 0);
    return 0;
}

/* The arrow for a place the view does not show, and whose the view is after
   a jump. The view is the box 100,50 to 500,350. */
int run_lv_event_flash_arrow(void) {
    float ex = -1.0f, ey = -1.0f, dx = 0.0f, dy = 0.0f;

    /* A place in the view: no arrow, the ring is there. */
    UT_ASSERT(lvFlashEdgeArrow(300.0f, 200.0f, 100.0f, 50.0f, 500.0f, 350.0f, 6.0f,
                               &ex, &ey, &dx, &dy) == 0);
    UT_ASSERT(lvFlashEdgeArrow(100.0f, 50.0f, 100.0f, 50.0f, 500.0f, 350.0f, 6.0f,
                               &ex, &ey, &dx, &dy) == 0);
    /* Its right and bottom edges are outside it. */
    UT_ASSERT(lvFlashEdgeArrow(500.0f, 200.0f, 100.0f, 50.0f, 500.0f, 350.0f, 6.0f,
                               &ex, &ey, &dx, &dy) == 1);

    /* Straight to the right: the point is 6 in from the right edge, at the
       place's own height, and points right. */
    UT_ASSERT(lvFlashEdgeArrow(900.0f, 200.0f, 100.0f, 50.0f, 500.0f, 350.0f, 6.0f,
                               &ex, &ey, &dx, &dy) == 1);
    UT_ASSERT_MSG(tf_near(ex, 494.0f) && tf_near(ey, 200.0f), "point at %f,%f",
                  (double)ex, (double)ey);
    UT_ASSERT_MSG(tf_near(dx, 1.0f) && tf_near(dy, 0.0f), "points %f,%f",
                  (double)dx, (double)dy);

    /* Straight up. */
    UT_ASSERT(lvFlashEdgeArrow(300.0f, -400.0f, 100.0f, 50.0f, 500.0f, 350.0f, 6.0f,
                               &ex, &ey, &dx, &dy) == 1);
    UT_ASSERT_MSG(tf_near(ex, 300.0f) && tf_near(ey, 56.0f), "point at %f,%f",
                  (double)ex, (double)ey);
    UT_ASSERT_MSG(tf_near(dx, 0.0f) && tf_near(dy, -1.0f), "points %f,%f",
                  (double)dx, (double)dy);

    /* Off the bottom left corner, 300 left and 400 down of the corner's
       point: a 3-4-5 triangle. */
    UT_ASSERT(lvFlashEdgeArrow(106.0f - 300.0f, 344.0f + 400.0f, 100.0f, 50.0f, 500.0f,
                               350.0f, 6.0f, &ex, &ey, &dx, &dy) == 1);
    UT_ASSERT_MSG(tf_near(ex, 106.0f) && tf_near(ey, 344.0f), "point at %f,%f",
                  (double)ex, (double)ey);
    UT_ASSERT_MSG(tf_near(dx, -0.6f) && tf_near(dy, 0.8f), "points %f,%f",
                  (double)dx, (double)dy);

    /* A view with no size has no arrow; a null out pointer is passed over. */
    UT_ASSERT(lvFlashEdgeArrow(900.0f, 200.0f, 100.0f, 50.0f, 100.0f, 350.0f, 6.0f,
                               &ex, &ey, &dx, &dy) == 0);
    UT_ASSERT(lvFlashEdgeArrow(900.0f, 200.0f, 100.0f, 50.0f, 500.0f, 350.0f, 6.0f,
                               0, 0, 0, 0) == 1);

    /* Only the game view's camera goes back to the followed tank. */
    UT_ASSERT(lvFlashCameraFollowsTank(0) == 0);
    UT_ASSERT(lvFlashCameraFollowsTank(1) == 1);
    return 0;
}
