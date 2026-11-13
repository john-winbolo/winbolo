/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The log viewer's aids for recording the screen.
 *
 *   lv_marks_fade — the lines Shift and a drag draw on the map
 *       (lv_marks.h): that a line is whole while it is drawn and for the
 *       hold after, fades over the fade and is then dropped; that a mouse
 *       held still adds no points; that a drag longer than one line carries
 *       on in a new line from the same point and both end together; and
 *       that one line more than the store holds drops the oldest.
 *
 *   lv_window_size_parse — "-size WxH" on the command line
 *       (lv_window_size.h): the sizes it takes and refuses, and pixels
 *       turned into window units at a pixel density.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lv_marks.h"
#include "lv_window_size.h"
#include "test_harness.h"

static LvMarks s_testMarks;

int run_lv_marks_fade(void) {
    LvMarks      *m = &s_testMarks;
    LvMarkStroke *s;
    int           i;

    lvMarksClear(m);
    UT_ASSERT_MSG(lvMarksOpen(m) == NULL, "nothing drawn");

    /* A line drawn, then the button up at 1000 ms. */
    lvMarksBegin(m, 10.0f, 20.0f);
    s = lvMarksOpen(m);
    UT_ASSERT_MSG(s != NULL && s->count == 1, "the first point");
    lvMarksAdd(m, 11.0f, 20.0f, 2.0f);
    UT_ASSERT_MSG(s->count == 1, "a point too near is dropped");
    lvMarksAdd(m, 12.0f, 20.0f, 2.0f);
    UT_ASSERT_MSG(s->count == 2, "a point a step away is kept");
    UT_ASSERT_MSG(lvMarksAlpha(s, 999999u) == 1.0f, "whole while drawn");
    lvMarksEnd(m, 1000u);
    UT_ASSERT_MSG(lvMarksOpen(m) == NULL, "ended");
    lvMarksAdd(m, 30.0f, 20.0f, 2.0f);
    UT_ASSERT_MSG(s->count == 2, "no points once ended");

    UT_ASSERT_MSG(lvMarksAlpha(s, 1000u) == 1.0f, "whole when the button comes up");
    UT_ASSERT_MSG(lvMarksAlpha(s, 1000u + LV_MARKS_HOLD_MS - 1u) == 1.0f, "whole to the end of the hold");
    UT_ASSERT_MSG(lvMarksAlpha(s, 1000u + LV_MARKS_HOLD_MS + LV_MARKS_FADE_MS / 2u) == 0.5f,
                  "half way through the fade");
    UT_ASSERT_MSG(lvMarksAlpha(s, 1000u + LV_MARKS_HOLD_MS + LV_MARKS_FADE_MS) == 0.0f, "faded out");
    UT_ASSERT_MSG(lvMarksAlpha(s, 500u) == 1.0f, "a clock before the end is no fade");

    lvMarksPrune(m, 1000u + LV_MARKS_HOLD_MS);
    UT_ASSERT_MSG(m->count == 1, "kept while fading");
    lvMarksPrune(m, 1000u + LV_MARKS_HOLD_MS + LV_MARKS_FADE_MS);
    UT_ASSERT_MSG(m->count == 0, "dropped once faded");

    /* A drag longer than one line: the second line starts at the last point
       of the first, and both end with the button. */
    lvMarksClear(m);
    lvMarksBegin(m, 0.0f, 0.0f);
    for (i = 1; i <= LV_MARKS_MAX_POINTS; i++) {
        lvMarksAdd(m, (float)i * 3.0f, 0.0f, 2.0f);
    }
    UT_ASSERT_MSG(m->count == 2, "the drag goes on in a second line");
    UT_ASSERT_MSG(lvMarksAt(m, 0)->count == LV_MARKS_MAX_POINTS, "the first is full");
    UT_ASSERT_MSG(lvMarksAt(m, 1)->count == 2, "the second has the joint and the new point");
    UT_ASSERT_MSG(lvMarksAt(m, 1)->pts[0].x == (float)(LV_MARKS_MAX_POINTS - 1) * 3.0f,
                  "the second starts where the first ends");
    UT_ASSERT_MSG(lvMarksAt(m, 0)->open && lvMarksAt(m, 1)->open, "both open while the button is down");
    lvMarksEnd(m, 5000u);
    UT_ASSERT_MSG(!lvMarksAt(m, 0)->open && lvMarksAt(m, 0)->endedMs == 5000u, "the first ends with the button");
    UT_ASSERT_MSG(!lvMarksAt(m, 1)->open && lvMarksAt(m, 1)->endedMs == 5000u, "the second too");

    /* One line more than the store holds drops the oldest. */
    lvMarksClear(m);
    for (i = 0; i < LV_MARKS_MAX_STROKES + 1; i++) {
        lvMarksBegin(m, (float)i, 0.0f);
        lvMarksEnd(m, 100u + (uint64_t)i);
    }
    UT_ASSERT_MSG(m->count == LV_MARKS_MAX_STROKES, "the store is full");
    UT_ASSERT_MSG(lvMarksAt(m, 0)->pts[0].x == 1.0f, "the oldest went");
    UT_ASSERT_MSG(lvMarksAt(m, LV_MARKS_MAX_STROKES - 1)->pts[0].x == (float)LV_MARKS_MAX_STROKES,
                  "the newest is last");
    return 0;
}

int run_lv_window_size_parse(void) {
    int w = -1, h = -1;

    UT_ASSERT_MSG(lvWindowSizeParse("1920x1080", &w, &h) && w == 1920 && h == 1080, "1920x1080");
    UT_ASSERT_MSG(lvWindowSizeParse("1280X720", &w, &h) && w == 1280 && h == 720, "a capital X");
    UT_ASSERT_MSG(lvWindowSizeParse("320x240", &w, &h) && w == 320 && h == 240, "the smallest");

    w = h = -1;
    UT_ASSERT_MSG(!lvWindowSizeParse(NULL, &w, &h), "no text");
    UT_ASSERT_MSG(!lvWindowSizeParse("", &w, &h), "empty");
    UT_ASSERT_MSG(!lvWindowSizeParse("1920", &w, &h), "no height");
    UT_ASSERT_MSG(!lvWindowSizeParse("1920x", &w, &h), "an empty height");
    UT_ASSERT_MSG(!lvWindowSizeParse("x1080", &w, &h), "no width");
    UT_ASSERT_MSG(!lvWindowSizeParse("1920*1080", &w, &h), "the wrong mark between");
    UT_ASSERT_MSG(!lvWindowSizeParse("1920x1080p", &w, &h), "something after");
    UT_ASSERT_MSG(!lvWindowSizeParse("-1920x1080", &w, &h), "a sign");
    UT_ASSERT_MSG(!lvWindowSizeParse("1920x-1080", &w, &h), "a sign on the height");
    UT_ASSERT_MSG(!lvWindowSizeParse("319x240", &w, &h), "too narrow");
    UT_ASSERT_MSG(!lvWindowSizeParse("320x239", &w, &h), "too short");
    UT_ASSERT_MSG(!lvWindowSizeParse("16385x1080", &w, &h), "too wide");
    UT_ASSERT_MSG(!lvWindowSizeParse("99999999999999999999x1080", &w, &h), "far too wide");
    UT_ASSERT_MSG(w == -1 && h == -1, "a refused size leaves the output alone");

    UT_ASSERT_MSG(lvWindowSizeUnits(1920, 1.0f) == 1920, "density 1");
    UT_ASSERT_MSG(lvWindowSizeUnits(1920, 2.0f) == 960, "density 2");
    UT_ASSERT_MSG(lvWindowSizeUnits(1080, 1.5f) == 720, "density 1.5");
    UT_ASSERT_MSG(lvWindowSizeUnits(1080, 1.25f) == 864, "density 1.25");
    UT_ASSERT_MSG(lvWindowSizeUnits(1081, 2.0f) == 541, "rounded to the nearest");
    UT_ASSERT_MSG(lvWindowSizeUnits(1920, 0.0f) == 1920, "a density of 0 is 1");
    return 0;
}
