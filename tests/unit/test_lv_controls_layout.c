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
 *   lv_zoom_percent — the zoom of the map as a percentage and as text
 *       (lv_zoom_text.h), on an ordinary and on a dense display; how long
 *       the label on the map shows; and when the zoom on the bottom row is
 *       whole, only its number, or left out.
 *   lv_controls_wheel_step — Shift and the wheel over the seek bar: one
 *       tick a notch, wheel up back in time, held inside the log.
 *   lv_controls_fine_drag — Shift and a drag of the seek bar: no move when
 *       the anchor is set, two pixels a tick, held inside the log.
 */

#include <stdbool.h>
#include <stddef.h>

#include <string.h>

#include "lv_controls_layout.h"
#include "lv_zoom_text.h"
#include "lv_fullscreen_key.h"
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

static int zp_text_is(float squarePx, float scale, const char *want) {
    char buf[24];
    lvZoomPercentText(lvZoomPercent(squarePx, scale), buf, sizeof(buf));
    return strcmp(buf, want) == 0;
}

int run_lv_zoom_percent(void) {
    char buf[24];
    static const float steps[] = { 0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
                                   1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
    static const char *const stepText[] = { "50%", "60%", "70%", "80%", "90%",
                                            "100%", "150%", "200%", "300%", "400%" };
    const int stepCount = (int)(sizeof(steps) / sizeof(steps[0]));
    int i;

    /* 100% is one square on 16 points: 16 pixels of an ordinary display,
       32 of one with two pixels a point, 24 at one and a half. */
    UT_ASSERT_MSG(lvZoomPercent(16.0f, 1.0f) == 100.0f, "16 px at 1x");
    UT_ASSERT_MSG(lvZoomPercent(32.0f, 2.0f) == 100.0f, "32 px at 2x is still 100");
    UT_ASSERT_MSG(lvZoomPercent(24.0f, 1.5f) == 100.0f, "24 px at 1.5x");
    UT_ASSERT_MSG(lvZoomPercent(32.0f, 1.0f) == 200.0f, "32 px at 1x is 200");
    /* The step between 100 and 200: a square on 24 points. */
    UT_ASSERT_MSG(lvZoomPercent(24.0f, 1.0f) == 150.0f, "24 px at 1x is 150");
    UT_ASSERT_MSG(lvZoomPercent(48.0f, 2.0f) == 150.0f, "48 px at 2x is 150");
    UT_ASSERT_MSG(zp_text_is(24.0f, 1.0f, "150%"), "the text of the 1.5 step");
    UT_ASSERT_MSG(lvZoomPercent(8.0f, 1.0f) == 50.0f, "8 px at 1x is 50");
    UT_ASSERT_MSG(lvZoomPercent(16.0f, 2.0f) == 50.0f, "16 px at 2x is 50");
    UT_ASSERT_MSG(lvZoomPercent(0.0f, 1.0f) == 0.0f, "no square");
    UT_ASSERT_MSG(lvZoomPercent(16.0f, 0.0f) == 100.0f, "no scale given: 1");

    /* Each step of the viewer, on three displays: the same text. */
    for (i = 0; i < stepCount; i++) {
        UT_ASSERT_MSG(zp_text_is(16.0f * steps[i], 1.0f, stepText[i]), "step %d at 1x", i);
        UT_ASSERT_MSG(zp_text_is(16.0f * steps[i] * 2.0f, 2.0f, stepText[i]), "step %d at 2x", i);
        UT_ASSERT_MSG(zp_text_is(16.0f * steps[i] * 1.25f, 1.25f, stepText[i]), "step %d at 1.25x", i);
    }

    /* The game view: a whole number of times, on a display of 1.5 drawn at
       2 pixels a pixel of the art: 2 * 2 * 16 px over 16 * 1.5. */
    UT_ASSERT_MSG(zp_text_is(64.0f, 1.5f, "267%"), "game view 2x on a 1.5 display");
    UT_ASSERT_MSG(zp_text_is(48.0f, 1.0f, "300%"), "game view 3x");

    /* The text: whole, but for a value under 100 that is not whole. */
    lvZoomPercentText(150.0f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "150%") == 0, "150: \"%s\"", buf);
    lvZoomPercentText(62.5f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "62.5%") == 0, "62.5: \"%s\"", buf);
    lvZoomPercentText(62.0f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "62%") == 0, "62: \"%s\"", buf);
    lvZoomPercentText(133.3f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "133%") == 0, "133.3 over 100 is whole: \"%s\"", buf);
    lvZoomPercentText(187.5f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "188%") == 0, "187.5: \"%s\"", buf);
    lvZoomPercentText(99.96f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "100%") == 0, "99.96 is 100: \"%s\"", buf);
    lvZoomPercentText(99.94f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "99.9%") == 0, "99.94: \"%s\"", buf);
    lvZoomPercentText(66.666f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "66.7%") == 0, "two thirds: \"%s\"", buf);
    lvZoomPercentText(0.0f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "0%") == 0, "nothing: \"%s\"", buf);
    lvZoomPercentText(-5.0f, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "0%") == 0, "under nothing: \"%s\"", buf);
    buf[0] = 'x';
    lvZoomPercentText(50.0f, buf, 0);
    UT_ASSERT_MSG(buf[0] == 'x', "no room: not written");

    /* The label on the map: whole for a second, gone at a second and a
       half, and not there before the zoom was ever changed. */
    UT_ASSERT_MSG(lvZoomLabelAlpha(0.0f) == 1.0f, "at the change");
    UT_ASSERT_MSG(lvZoomLabelAlpha(1.0f) == 1.0f, "after a second");
    UT_ASSERT_MSG(lvZoomLabelAlpha(1.25f) == 0.5f, "half way out");
    UT_ASSERT_MSG(lvZoomLabelAlpha(1.5f) == 0.0f, "gone");
    UT_ASSERT_MSG(lvZoomLabelAlpha(1000.0f) == 0.0f, "long after");
    UT_ASSERT_MSG(lvZoomLabelAlpha(-1.0f) == 0.0f, "before");

    /* The bottom row: three pieces of 100, 80 and 60 with gaps of 15 are
       270 wide. The zoom is 70 whole and 30 as a number. */
    {
        static const float w[3] = { 100.0f, 80.0f, 60.0f };
        int width;
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 400.0f) == LV_ZOOM_FORM_FULL,
                      "room for all of it");
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 355.0f) == LV_ZOOM_FORM_FULL,
                      "exactly: 270 + 15 + 70");
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 354.0f) == LV_ZOOM_FORM_SHORT,
                      "one less: the number alone");
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 315.0f) == LV_ZOOM_FORM_SHORT,
                      "exactly: 270 + 15 + 30");
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 314.0f) == LV_ZOOM_FORM_NONE,
                      "one less: left out");
        /* Two lines already (100 + 80, then 60): the zoom goes after the 60
           while that adds no third line. */
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 220.0f) == LV_ZOOM_FORM_FULL,
                      "on the last of two lines");
        /* Each piece alone on its line at 130: 60 + 15 + 30 goes in. */
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 130.0f) == LV_ZOOM_FORM_SHORT,
                      "three lines, the number after the last piece");
        UT_ASSERT_MSG(lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, 100.0f) == LV_ZOOM_FORM_NONE,
                      "no room after the last piece");
        UT_ASSERT_MSG(lvControlsZoomForm(w, 0, 70.0f, 30.0f, 15.0f, 100.0f) == LV_ZOOM_FORM_FULL,
                      "alone on the row");
        UT_ASSERT_MSG(lvControlsZoomForm(w, 0, 70.0f, 30.0f, 15.0f, 20.0f) == LV_ZOOM_FORM_NONE,
                      "alone, wider than the panel");

        /* Every width: the zoom never adds a line, is never past the edge,
           and gives way in order as the panel gets narrower. */
        {
            LvZoomForm prev = LV_ZOOM_FORM_FULL;
            for (width = 600; width >= 10; width--) {
                float all[4], x[4];
                int line[4];
                const int lines3 = lvControlsFlow(w, 3, 15.0f, (float)width, line, x);
                const LvZoomForm f = lvControlsZoomForm(w, 3, 70.0f, 30.0f, 15.0f, (float)width);
                if (f != LV_ZOOM_FORM_NONE) {
                    all[0] = w[0]; all[1] = w[1]; all[2] = w[2];
                    all[3] = (f == LV_ZOOM_FORM_FULL) ? 70.0f : 30.0f;
                    UT_ASSERT_MSG(lvControlsFlow(all, 4, 15.0f, (float)width, line, x) == lines3,
                                  "width %d: no line more", width);
                    UT_ASSERT_MSG(x[3] >= 0.0f && x[3] + all[3] <= (float)width,
                                  "width %d: inside the panel", width);
                    UT_ASSERT_MSG(line[3] == line[2], "width %d: after the last piece", width);
                }
                if (lines3 == 1) {
                    UT_ASSERT_MSG(f >= prev, "width %d: gives way in order", width);
                    prev = f;
                }
            }
        }
    }
    return 0;
}

/* Alt+Enter is the full screen key: Enter or the Enter of the number pad
   with either Alt key. A repeat of it does nothing but is still kept from the
   viewer. Any other key is not this command's. */
int run_lv_fullscreen_key(void) {
    const unsigned int ret = LV_FSKEY_SCANCODE_RETURN, kp = LV_FSKEY_SCANCODE_KP_ENTER;
    const unsigned int lalt = LV_FSKEY_MOD_LALT, ralt = LV_FSKEY_MOD_RALT;
    const unsigned int shift = 0x0001u, ctrl = 0x0040u, gui = 0x0400u, num = 0x1000u, caps = 0x2000u;
    unsigned int sc;

    /* The numbers are SDL's (SDL_scancode.h, SDL_keycode.h). */
    UT_ASSERT(ret == 40u && kp == 88u && lalt == 0x0100u && ralt == 0x0200u);

    /* Both Alt keys, both Enters. */
    UT_ASSERT(lvFullScreenKey(false, lalt, ret) == LV_FSKEY_TOGGLE);
    UT_ASSERT(lvFullScreenKey(false, ralt, ret) == LV_FSKEY_TOGGLE);
    UT_ASSERT(lvFullScreenKey(false, lalt, kp) == LV_FSKEY_TOGGLE);
    UT_ASSERT(lvFullScreenKey(false, ralt, kp) == LV_FSKEY_TOGGLE);
    UT_ASSERT(lvFullScreenKey(false, lalt | ralt, ret) == LV_FSKEY_TOGGLE);

    /* Num Lock and Caps Lock are "held" all the time on many keyboards, and
       the other modifier keys make no difference, as in the game. */
    UT_ASSERT(lvFullScreenKey(false, lalt | num | caps, ret) == LV_FSKEY_TOGGLE);
    UT_ASSERT(lvFullScreenKey(false, lalt | shift, ret) == LV_FSKEY_TOGGLE);
    UT_ASSERT(lvFullScreenKey(false, ralt | ctrl, kp) == LV_FSKEY_TOGGLE);

    /* Held down: one command. The repeats are swallowed, not passed on. */
    UT_ASSERT(lvFullScreenKey(true, lalt, ret) == LV_FSKEY_SWALLOW);
    UT_ASSERT(lvFullScreenKey(true, ralt, kp) == LV_FSKEY_SWALLOW);

    /* Enter with no Alt is an ordinary Enter, repeated or not. */
    UT_ASSERT(lvFullScreenKey(false, 0u, ret) == LV_FSKEY_NOT_OURS);
    UT_ASSERT(lvFullScreenKey(true, 0u, ret) == LV_FSKEY_NOT_OURS);
    UT_ASSERT(lvFullScreenKey(false, 0u, kp) == LV_FSKEY_NOT_OURS);
    UT_ASSERT(lvFullScreenKey(false, shift | ctrl | gui | num | caps, ret) == LV_FSKEY_NOT_OURS);

    /* Alt with any other key is not the command: every scancode but the two. */
    for (sc = 0; sc < 512u; sc++) {
        if (sc == ret || sc == kp) continue;
        UT_ASSERT_MSG(lvFullScreenKey(false, lalt, sc) == LV_FSKEY_NOT_OURS, "scancode %u", sc);
        UT_ASSERT_MSG(lvFullScreenKey(true, ralt, sc) == LV_FSKEY_NOT_OURS, "scancode %u, repeat", sc);
    }
    return 0;
}

int run_lv_controls_wheel_step(void) {
    LvWheelStep st;
    float carry = 0.0f;
    uint32_t now;
    int i;

    /* One notch up is one tick back, one notch down one tick forward. */
    st = lvControlsWheelStep(1.0f, &carry, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(st.moved && st.ticks == -1 && st.targetMs == 980,
                  "up: %d ticks to %u", st.ticks, (unsigned)st.targetMs);
    st = lvControlsWheelStep(-1.0f, &carry, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(st.moved && st.ticks == 1 && st.targetMs == 1020,
                  "down: %d ticks to %u", st.ticks, (unsigned)st.targetMs);
    UT_ASSERT_MSG(carry == 0.0f, "a whole notch left %f", (double)carry);

    /* Several notches in one frame are that many ticks. */
    st = lvControlsWheelStep(3.0f, &carry, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(st.ticks == -3 && st.targetMs == 940, "three up: %d to %u", st.ticks,
                  (unsigned)st.targetMs);
    st = lvControlsWheelStep(-5.0f, &carry, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(st.ticks == 5 && st.targetMs == 1100, "five down: %d to %u", st.ticks,
                  (unsigned)st.targetMs);

    /* No wheel: nothing. */
    st = lvControlsWheelStep(0.0f, &carry, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(!st.moved && st.ticks == 0 && st.targetMs == 1000, "no wheel moved");

    /* The ends of the log hold it. */
    st = lvControlsWheelStep(1.0f, &carry, 0, 0, 60000, 20);
    UT_ASSERT_MSG(!st.moved && st.targetMs == 0, "back at the start: %u",
                  (unsigned)st.targetMs);
    st = lvControlsWheelStep(4.0f, &carry, 40, 0, 60000, 20);
    UT_ASSERT_MSG(st.moved && st.targetMs == 0, "back past the start: %u",
                  (unsigned)st.targetMs);
    st = lvControlsWheelStep(-1.0f, &carry, 60000, 0, 60000, 20);
    UT_ASSERT_MSG(!st.moved && st.targetMs == 60000, "forward at the end: %u",
                  (unsigned)st.targetMs);
    st = lvControlsWheelStep(-4.0f, &carry, 59960, 0, 60000, 20);
    UT_ASSERT_MSG(st.moved && st.targetMs == 60000, "forward past the end: %u",
                  (unsigned)st.targetMs);
    /* The lobby is hidden: the round starts at 12000 ms. */
    st = lvControlsWheelStep(2.0f, &carry, 12020, 12000, 60000, 20);
    UT_ASSERT_MSG(st.moved && st.targetMs == 12000, "into the hidden lobby: %u",
                  (unsigned)st.targetMs);
    st = lvControlsWheelStep(1.0f, &carry, 12000, 12000, 60000, 20);
    UT_ASSERT_MSG(!st.moved, "back at the start of the round");

    /* A time between two ticks goes to a tick: the one before it for back,
       the one after it for forward. */
    st = lvControlsWheelStep(1.0f, &carry, 1010, 0, 60000, 20);
    UT_ASSERT_MSG(st.targetMs == 1000, "back off a tick: %u", (unsigned)st.targetMs);
    st = lvControlsWheelStep(-1.0f, &carry, 1010, 0, 60000, 20);
    UT_ASSERT_MSG(st.targetMs == 1020, "forward off a tick: %u", (unsigned)st.targetMs);
    /* An end of the log that is between two ticks is still the end. */
    st = lvControlsWheelStep(-1.0f, &carry, 59990, 0, 59995, 20);
    UT_ASSERT_MSG(st.moved && st.targetMs == 59995, "an end off a tick: %u",
                  (unsigned)st.targetMs);

    /* A wheel that gives parts of a notch: a tick for each whole one. */
    carry = 0.0f;
    now = 1000;
    for (i = 0; i < 8; i++) {
        st = lvControlsWheelStep(0.25f, &carry, now, 0, 60000, 20);
        now = st.targetMs;
    }
    UT_ASSERT_MSG(now == 960, "eight quarters of a notch: %u (want 960)", (unsigned)now);
    /* A part one way and then the other way is no tick. */
    carry = 0.0f;
    st = lvControlsWheelStep(0.5f, &carry, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(!st.moved, "half a notch moved");
    st = lvControlsWheelStep(-0.5f, &carry, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(!st.moved && carry == 0.0f, "half up and half down moved");

    /* No place for the parts, no tick length, the ends the wrong way round. */
    st = lvControlsWheelStep(1.0f, NULL, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(st.targetMs == 980, "no carry: %u", (unsigned)st.targetMs);
    st = lvControlsWheelStep(1.0f, NULL, 1000, 0, 60000, 0);
    UT_ASSERT_MSG(st.targetMs == 980, "no tick length: %u", (unsigned)st.targetMs);
    st = lvControlsWheelStep(1.0f, NULL, 1000, 500, 100, 20);
    UT_ASSERT_MSG(st.targetMs == 500, "ends the wrong way round: %u", (unsigned)st.targetMs);
    /* A wheel that is no wheel does not run off the numbers. */
    st = lvControlsWheelStep(1.0e30f, NULL, 1000, 0, 60000, 20);
    UT_ASSERT_MSG(st.moved && st.targetMs == 0, "a huge wheel: %u", (unsigned)st.targetMs);
    st = lvControlsWheelStep(-1.0e30f, NULL, 0xFFFFFF00u, 0, 0xFFFFFFF0u, 20);
    UT_ASSERT_MSG(st.targetMs == 0xFFFFFFF0u, "a huge wheel at the top of the clock: %u",
                  (unsigned)st.targetMs);
    return 0;
}

int run_lv_controls_fine_drag(void) {
    const float fine = LV_CONTROLS_FINE_DRAG_PX_PER_TICK;
    uint32_t got;
    int px;

    UT_ASSERT_MSG(fine == 2.0f, "pixels for a tick: %f", (double)fine);

    /* No jump when the anchor is set: with the mouse at the anchor the
       answer is the anchor's own time, also one between two ticks, at any
       x, and at any rate. */
    got = lvControlsDragTargetMs(30000, 412.0f, 412.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 30000, "at the anchor: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30007, 0.5f, 0.5f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 30007, "at an anchor between two ticks: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30007, 900.0f, 900.0f, 1.0f, 3600.0f, 0, 60000);
    UT_ASSERT_MSG(got == 30007, "at the anchor, without Shift: %u", (unsigned)got);

    /* With Shift: two pixels are one tick, either way. A part of a step
       does not count. */
    got = lvControlsDragTargetMs(30000, 400.0f, 401.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 30000, "one pixel right: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30000, 400.0f, 402.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 30020, "two pixels right: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30000, 400.0f, 399.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 30000, "one pixel left: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30000, 400.0f, 398.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 29980, "two pixels left: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30000, 400.0f, 500.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 31000, "100 pixels right: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30000, 400.0f, 300.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 29000, "100 pixels left: %u", (unsigned)got);
    /* Every pixel of a slow drag: never more than one tick at a time. */
    for (px = -200; px < 200; px++) {
        uint32_t a = lvControlsDragTargetMs(30000, 400.0f, 400.0f + (float)px, fine, 20.0f, 0,
                                            60000);
        uint32_t b = lvControlsDragTargetMs(30000, 400.0f, 401.0f + (float)px, fine, 20.0f, 0,
                                            60000);
        UT_ASSERT_MSG(b == a || b == a + 20, "pixel %d: %u then %u", px, (unsigned)a,
                      (unsigned)b);
    }
    /* An anchor between two ticks stays that far from the ticks. */
    got = lvControlsDragTargetMs(30007, 400.0f, 404.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 30047, "from between two ticks: %u", (unsigned)got);

    /* Held at the two ends of the log that shows. */
    got = lvControlsDragTargetMs(60, 400.0f, 380.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 0, "past the start: %u", (unsigned)got);
    got = lvControlsDragTargetMs(60, 400.0f, 394.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 0, "to the start: %u", (unsigned)got);
    got = lvControlsDragTargetMs(59960, 400.0f, 420.0f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 60000, "past the end: %u", (unsigned)got);
    got = lvControlsDragTargetMs(12040, 400.0f, 300.0f, fine, 20.0f, 12000, 60000);
    UT_ASSERT_MSG(got == 12000, "past the start of the round: %u", (unsigned)got);
    got = lvControlsDragTargetMs(0xFFFFFFE0u, 0.0f, 4000.0f, fine, 20.0f, 0, 0xFFFFFFF0u);
    UT_ASSERT_MSG(got == 0xFFFFFFF0u, "at the top of the clock: %u", (unsigned)got);

    /* Without Shift, after it was let go: each pixel is the length of the
       log one pixel of the bar stands for. */
    got = lvControlsDragTargetMs(30000, 400.0f, 410.0f, 1.0f, 3600.0f, 0, 3600000);
    UT_ASSERT_MSG(got == 66000, "ten pixels of the bar: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30000, 400.0f, 390.0f, 1.0f, 3600.0f, 0, 3600000);
    UT_ASSERT_MSG(got == 0, "ten pixels back, past the start: %u", (unsigned)got);
    got = lvControlsDragTargetMs(30000, 400.0f, 410.5f, 1.0f, 12.5f, 0, 3600000);
    UT_ASSERT_MSG(got == 30125, "a step that is no whole number: %u", (unsigned)got);

    /* Numbers that make no sense do no harm. */
    got = lvControlsDragTargetMs(1000, 0.0f, 10.0f, 0.0f, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 1200, "no pixels for a step: %u", (unsigned)got);
    got = lvControlsDragTargetMs(1000, 0.0f, 10.0f, fine, -5.0f, 0, 60000);
    UT_ASSERT_MSG(got == 1000, "a step under zero: %u", (unsigned)got);
    got = lvControlsDragTargetMs(1000, 0.0f, 10.0f, fine, 20.0f, 500, 100);
    UT_ASSERT_MSG(got == 500, "ends the wrong way round: %u", (unsigned)got);
    got = lvControlsDragTargetMs(1000, 0.0f, 1.0e30f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 60000, "a mouse far off to the right: %u", (unsigned)got);
    got = lvControlsDragTargetMs(1000, 0.0f, -1.0e30f, fine, 20.0f, 0, 60000);
    UT_ASSERT_MSG(got == 0, "a mouse far off to the left: %u", (unsigned)got);

    /* The drag step both seek bars use. A drag with no Shift is not
       measured. */
    {
        LvFineDrag d = { false, false, 0, 0.0f, 0 };
        bool anchored = true;
        UT_ASSERT_MSG(!lvFineDragStep(&d, false, 400.0f, 30000, 0, 60000, 600.0f, &anchored),
                      "no Shift: not measured");
        UT_ASSERT_MSG(!anchored && !d.on, "no Shift: no anchor");

        /* Shift goes down: the anchor is where the handle is, no jump. */
        UT_ASSERT_MSG(lvFineDragStep(&d, true, 400.0f, 30007, 0, 60000, 600.0f, &anchored),
                      "Shift: measured");
        UT_ASSERT_MSG(anchored && d.targetMs == 30007, "Shift down: %u", (unsigned)d.targetMs);
        lvFineDragStep(&d, true, 404.0f, 12345, 0, 60000, 600.0f, &anchored);
        UT_ASSERT_MSG(!anchored && d.targetMs == 30047, "four pixels with Shift: %u",
                      (unsigned)d.targetMs);

        /* Shift let go: a new anchor at the time the drag asked for, then
           the bar's own rate, 100 ms a pixel here. */
        lvFineDragStep(&d, false, 404.0f, 12345, 0, 60000, 600.0f, &anchored);
        UT_ASSERT_MSG(anchored && d.targetMs == 30047, "Shift up: %u", (unsigned)d.targetMs);
        lvFineDragStep(&d, false, 414.0f, 12345, 0, 60000, 600.0f, &anchored);
        UT_ASSERT_MSG(!anchored && d.targetMs == 31047, "ten pixels without Shift: %u",
                      (unsigned)d.targetMs);

        /* Shift down again: fine from where it is. */
        lvFineDragStep(&d, true, 414.0f, 12345, 0, 60000, 600.0f, NULL);
        lvFineDragStep(&d, true, 410.0f, 12345, 0, 60000, 600.0f, NULL);
        UT_ASSERT_MSG(d.fine && d.targetMs == 31007, "Shift down again: %u",
                      (unsigned)d.targetMs);
    }
    return 0;
}

/* lv_play_toggle: the one play and pause button. The pause button while the
 * replay plays; else the play button, dimmed with nothing to play. */
int run_lv_play_toggle(void) {
    LvPlayToggle t = lvPlayToggle(true, true, false);
    UT_ASSERT_MSG(t.pause && t.enabled, "playing: pause, on");
    t = lvPlayToggle(true, false, false);
    UT_ASSERT_MSG(!t.pause && t.enabled, "paused: play, on");
    t = lvPlayToggle(true, false, true);
    UT_ASSERT_MSG(!t.pause && !t.enabled, "at the end: play, dimmed");
    t = lvPlayToggle(true, true, true);
    UT_ASSERT_MSG(t.pause && t.enabled, "playing at the end: pause, on");
    t = lvPlayToggle(false, false, false);
    UT_ASSERT_MSG(!t.pause && !t.enabled, "no replay: play, dimmed");
    t = lvPlayToggle(false, true, false);
    UT_ASSERT_MSG(!t.pause && !t.enabled, "no replay, a stale play flag: play, dimmed");
    return 0;
}
