/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Where the items of the log viewer's Controls panel go
 * (lv_controls_layout.h).
 *
 *   lv_controls_top_row — the playback buttons are twice as wide as high
 *       where there is room, get narrower evenly down to a square, then go
 *       on more lines; the speed and the "Overlay events" list follow; each
 *       line is in the middle; nothing goes past the edge of the panel, at
 *       every width.
 *   lv_controls_bottom_row — the pieces of text flow onto lines and each
 *       line is in the middle.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_controls_layout.h"
#include "test_harness.h"

static LvControlsTopIn cl_in(float w, int buttons) {
    LvControlsTopIn in;
    in.contentW = w;
    in.buttons = buttons;
    in.btnSide = 19.0f;
    in.btnGap = 4.0f;
    in.labelW = 50.0f;
    in.speedMin = 50.0f;
    in.speedMax = 100.0f;
    in.comboFullW = 130.0f;
    in.comboArrowW = 19.0f;
    in.gap = 15.0f;
    return in;
}

static bool cl_near(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= 1.0f;
}

/* Every item is inside the content, no two on a line overlap, and each
   line has the same room on its left as on its right (to a unit). */
static bool cl_top_sound(const LvControlsTopIn *in, const LvControlsTop *o) {
    float left[LV_CONTROLS_MAX_BUTTONS + 2], right[LV_CONTROLS_MAX_BUTTONS + 2];
    float comboW = o->comboFull ? in->comboFullW : in->comboArrowW;
    int i;

    if (o->lines < 1 || o->lines > LV_CONTROLS_MAX_BUTTONS + 2) return false;
    for (i = 0; i < o->lines; i++) {
        left[i] = 1e9f;
        right[i] = -1e9f;
    }
    for (i = 0; i < in->buttons; i++) {
        int l = o->btnLine[i];
        if (l < 0 || l >= o->lines) return false;
        if (i > 0 && o->btnLine[i - 1] == l &&
            o->btnX[i] < o->btnX[i - 1] + o->btnW + in->btnGap - 0.01f) return false;
        if (o->btnX[i] < left[l]) left[l] = o->btnX[i];
        if (o->btnX[i] + o->btnW > right[l]) right[l] = o->btnX[i] + o->btnW;
    }
    if (o->btnW < in->btnSide || o->btnW > in->btnSide * LV_CONTROLS_BTN_WIDE) return false;
    {
        int l = o->speedLine;
        float start = o->label ? o->labelX : o->speedX;
        if (l < 0 || l >= o->lines) return false;
        if (o->label && !cl_near(o->speedX, o->labelX + in->labelW)) return false;
        /* After the buttons on their line, with the gap. */
        if (right[l] > -1e8f && start < right[l] + in->gap - 0.01f) return false;
        if (start < left[l]) left[l] = start;
        if (o->speedX + o->speedW > right[l]) right[l] = o->speedX + o->speedW;
    }
    {
        int l = o->comboLine;
        if (l < 0 || l >= o->lines) return false;
        if (right[l] > -1e8f && o->comboX < right[l] + in->gap - 0.01f) return false;
        if (o->comboX < left[l]) left[l] = o->comboX;
        if (o->comboX + comboW > right[l]) right[l] = o->comboX + comboW;
    }
    for (i = 0; i < o->lines; i++) {
        float w = right[i] - left[i];
        if (left[i] > 1e8f) return false; /* an empty line */
        if (left[i] < 0.0f) return false;
        if (w <= in->contentW) {
            /* In the middle, and inside. */
            if (right[i] > in->contentW + 0.01f) return false;
            if (!cl_near(left[i], in->contentW - right[i])) return false;
        } else {
            /* Wider than the panel: it starts at the left edge. */
            if (left[i] != 0.0f) return false;
        }
    }
    return true;
}

int run_lv_controls_top_row(void) {
    LvControlsTopIn in;
    LvControlsTop o;
    int w, n;

    UT_ASSERT_MSG(lvControlsCentreX(100.0f, 300.0f) == 100.0f, "a line goes in the middle");
    UT_ASSERT_MSG(lvControlsCentreX(101.0f, 300.0f) == 99.0f, "on a whole unit");
    UT_ASSERT_MSG(lvControlsCentreX(400.0f, 300.0f) == 0.0f, "a wide line starts at the left");

    /* The floating panel's own width: every button twice as wide as high,
       all of the row on one line, the line in the middle. */
    in = cl_in(404.0f, 4);
    o = lvControlsTopLayout(&in);
    UT_ASSERT_MSG(o.btnW == 38.0f, "double width where there is room");
    UT_ASSERT_MSG(o.lines == 1 && o.label, "one line, with the label");
    UT_ASSERT_MSG(o.speedW == 100.0f, "the slider at its full width");
    UT_ASSERT_MSG(!o.comboFull, "the list as its arrow when its name does not go");
    /* 4 x 38 + 3 x 4, 15, 50 + 100, 15, 19 = 363; (404 - 363) / 2 = 20. */
    UT_ASSERT_MSG(o.btnX[0] == 20.0f && o.btnX[3] == 20.0f + 3.0f * 42.0f, "buttons from the middle");
    UT_ASSERT_MSG(o.comboX + 19.0f == 404.0f - 21.0f, "the same room at each end");

    /* A wide panel: the list has its name. */
    in = cl_in(700.0f, 4);
    o = lvControlsTopLayout(&in);
    UT_ASSERT_MSG(o.lines == 1 && o.comboFull && o.btnW == 38.0f, "wide: all of it, one line");

    /* The narrowest sidebar (110 wide, 94 of content): the buttons get
       narrower evenly and stay on one line; the speed and the list take the
       next line. */
    in = cl_in(94.0f, 4);
    o = lvControlsTopLayout(&in);
    UT_ASSERT_MSG(o.btnW == 20.0f, "(94 - 12) / 4, whole");
    UT_ASSERT_MSG(o.btnLine[3] == 0 && o.speedLine == 1, "buttons one line, speed the next");
    UT_ASSERT_MSG(!o.label, "no room for the label and the slider: the slider stays");
    UT_ASSERT_MSG(o.comboLine == 1 && !o.comboFull, "the arrow after the slider");

    /* Just room for the four squares: the old width, still one line. */
    in = cl_in(88.0f, 4);
    o = lvControlsTopLayout(&in);
    UT_ASSERT_MSG(o.btnW == 19.0f && o.btnLine[3] == 0, "down to a square before a new line");
    /* Less than that: squares on more lines. */
    in = cl_in(87.0f, 4);
    o = lvControlsTopLayout(&in);
    UT_ASSERT_MSG(o.btnW == 19.0f && o.btnLine[2] == 0 && o.btnLine[3] == 1, "then they wrap");

    /* The width between: wider than a square, narrower than double. */
    in = cl_in(130.0f, 4);
    o = lvControlsTopLayout(&in);
    UT_ASSERT_MSG(o.btnW == 29.0f, "(130 - 12) / 4, whole");

    /* A live feed has five. */
    in = cl_in(404.0f, 5);
    o = lvControlsTopLayout(&in);
    UT_ASSERT_MSG(o.btnW == 38.0f && o.btnLine[4] == 0, "five wide buttons on a line");

    /* Every width, four and five buttons: all inside, none over another,
       every line in the middle. */
    for (n = 4; n <= 5; n++) {
        for (w = 20; w <= 900; w++) {
            in = cl_in((float)w, n);
            o = lvControlsTopLayout(&in);
            if (!cl_top_sound(&in, &o)) {
                UT_ASSERT_MSG(false, "the top row is sound at every width");
            }
            /* Never narrower than the old square while one line holds them. */
            if ((float)w >= (float)n * 19.0f + (float)(n - 1) * 4.0f) {
                UT_ASSERT_MSG(o.btnLine[n - 1] == 0, "one line of buttons from the old width up");
            }
        }
    }
    return 0;
}

int run_lv_controls_bottom_row(void) {
    float w[3] = { 120.0f, 80.0f, 60.0f };
    float x[3];
    int line[3];
    int lines, width;

    /* All on one line: 120 + 15 + 80 + 15 + 60 = 290, in 400. */
    lines = lvControlsFlow(w, 3, 15.0f, 400.0f, line, x);
    UT_ASSERT_MSG(lines == 1, "one line");
    UT_ASSERT_MSG(x[0] == 55.0f && x[1] == 190.0f && x[2] == 285.0f, "in the middle");

    /* Two on the first line, the third alone and in the middle. */
    lines = lvControlsFlow(w, 3, 15.0f, 220.0f, line, x);
    UT_ASSERT_MSG(lines == 2 && line[1] == 0 && line[2] == 1, "the third goes down");
    UT_ASSERT_MSG(x[0] == 2.0f && x[1] == 137.0f, "the first line: (220 - 215) / 2");
    UT_ASSERT_MSG(x[2] == 80.0f, "the last line: (220 - 60) / 2");

    /* Each alone. */
    lines = lvControlsFlow(w, 3, 15.0f, 130.0f, line, x);
    UT_ASSERT_MSG(lines == 3, "three lines");
    UT_ASSERT_MSG(x[0] == 5.0f && x[1] == 25.0f && x[2] == 35.0f, "each in the middle");

    /* Wider than the panel: at the left edge, a line to itself. */
    lines = lvControlsFlow(w, 3, 15.0f, 94.0f, line, x);
    UT_ASSERT_MSG(lines == 3 && x[0] == 0.0f, "a wide piece starts at the left");
    UT_ASSERT_MSG(x[1] == 7.0f && x[2] == 17.0f, "the others in the middle");

    UT_ASSERT_MSG(lvControlsFlow(w, 0, 15.0f, 100.0f, line, x) == 0, "nothing: no lines");

    /* Every width: inside when it can be, in the middle. */
    for (width = 10; width <= 600; width++) {
        int i;
        lines = lvControlsFlow(w, 3, 15.0f, (float)width, line, x);
        for (i = 0; i < 3; i++) {
            UT_ASSERT_MSG(x[i] >= 0.0f, "never left of the edge");
            if (w[i] <= (float)width) {
                UT_ASSERT_MSG(x[i] + w[i] <= (float)width, "inside the panel");
            }
            if (i > 0 && line[i] == line[i - 1]) {
                UT_ASSERT_MSG(x[i] == x[i - 1] + w[i - 1] + 15.0f, "the gap between two");
            }
        }
    }
    return 0;
}
