/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The builder marker the log viewer can draw over each tank on its map
 * (lv_builder_status.h).
 *
 *   lv_builder_status_sums — the builder's three states; where the marker
 *       goes, over the bars and with them off; the size of a cell; which of
 *       16 ways the arrow points; the small pictures.
 *   lv_builder_countdown — the text of the time left, exact and estimated,
 *       never under "0:00"; the estimate of a flight by the game's rule.
 *   lv_builder_trips — the walk that fills the table of the times a builder
 *       was away, and the time left read back out of it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lv_builder_status.h"
#include "test_harness.h"

static bool bs_text(long ms, bool approx, const char *want) {
    char got[LV_BUILDER_TEXT_MAX];
    int n = lvBuilderCountdownText(ms, approx, got, sizeof(got));
    return strcmp(got, want) == 0 && n == (int)strlen(want);
}

static bool bs_close(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.001f;
}

/* The cells drawn in a picture. */
static int bs_cells(const LvBuilderGlyph *g) {
    int row, col, n = 0;
    for (row = 0; row < g->height; row++) {
        for (col = 0; col < g->width; col++) {
            if (lvBuilderGlyphCell(g, col, row)) n++;
        }
    }
    return n;
}

int run_lv_builder_status_sums(void) {
    LvBuilderMarkLayout m;
    LvTankBarsLayout bars;
    float x, y;
    int d, c;

    /* The states. Dead wins: in the tick of the death the log may name no
       place for the builder. */
    UT_ASSERT_MSG(lvBuilderState(false, false) == LV_BUILDER_IN, "in the tank");
    UT_ASSERT_MSG(lvBuilderState(true, false) == LV_BUILDER_OUT, "out");
    UT_ASSERT_MSG(lvBuilderState(true, true) == LV_BUILDER_FLYING, "dead, flying");
    UT_ASSERT_MSG(lvBuilderState(false, true) == LV_BUILDER_FLYING, "dead, no place");

    /* A cell is one point, two from 32 points to a square; a point is two
       pixels on a display of density 2. */
    UT_ASSERT_MSG(lvBuilderCell(16.0f, 1.0f) == 1.0f, "16 px squares: 1 px cells");
    UT_ASSERT_MSG(lvBuilderCell(28.8f, 1.0f) == 1.0f, "28.8 px squares: 1 px cells");
    UT_ASSERT_MSG(lvBuilderCell(32.0f, 1.0f) == 2.0f, "32 px squares: 2 px cells");
    UT_ASSERT_MSG(lvBuilderCell(64.0f, 1.0f) == 2.0f, "64 px squares: 2 px cells");
    UT_ASSERT_MSG(lvBuilderCell(32.0f, 2.0f) == 2.0f, "density 2, 16 pt squares");
    UT_ASSERT_MSG(lvBuilderCell(64.0f, 2.0f) == 4.0f, "density 2, 32 pt squares");

    /* Bars off: one gap above the tank's picture, 7 cells high, its middle
       over the picture's middle. */
    m = lvBuilderMarkLayout(100.0f, 200.0f, 16.0f, 1.0f, false);
    UT_ASSERT_MSG(m.visible, "16 px squares have a marker");
    UT_ASSERT_MSG(m.cell == 1.0f && m.height == 7.0f, "7 cells of 1 px");
    UT_ASSERT_MSG(m.top + m.height == 199.0f, "bars off: bottom one pixel above "
                  "the tank (%.1f)", m.top + m.height);
    UT_ASSERT_MSG(m.centreX == 108.0f, "over the middle of the tank (%.1f)", m.centreX);

    /* Bars on: one gap above the armour bar, which is the upper one. */
    bars = lvTankBarsLayout(100.0f, 200.0f, 16.0f, 1.0f);
    m = lvBuilderMarkLayout(100.0f, 200.0f, 16.0f, 1.0f, true);
    UT_ASSERT_MSG(m.top + m.height == bars.armour.y - 1.0f, "bars on: bottom "
                  "one pixel above the armour bar (%.1f, bar at %.1f)",
                  m.top + m.height, bars.armour.y);
    UT_ASSERT_MSG(m.top + m.height < bars.shells.y, "above both bars");

    /* Larger squares, larger cells; the stack is the same. */
    bars = lvTankBarsLayout(40.3f, 300.6f, 32.0f, 1.0f);
    m = lvBuilderMarkLayout(40.3f, 300.6f, 32.0f, 1.0f, true);
    UT_ASSERT_MSG(m.cell == 2.0f && m.height == 14.0f, "32 px squares: 14 px high");
    UT_ASSERT_MSG(m.top + m.height == bars.armour.y - 1.0f, "32 px: over the bars");
    UT_ASSERT_MSG(m.centreX == 56.0f, "32 px: floored, then the middle (%.1f)",
                  m.centreX);
    m = lvBuilderMarkLayout(40.3f, 300.6f, 32.0f, 1.0f, false);
    UT_ASSERT_MSG(m.top + m.height == 299.0f, "32 px, bars off (%.1f)",
                  m.top + m.height);

    /* A display of density 2: the gap is one point, two pixels. */
    m = lvBuilderMarkLayout(100.0f, 200.0f, 32.0f, 2.0f, false);
    UT_ASSERT_MSG(m.cell == 2.0f && m.top + m.height == 198.0f, "density 2 "
                  "(cell %.1f, bottom %.1f)", m.cell, m.top + m.height);

    /* Too small for bars is too small for the marker. */
    m = lvBuilderMarkLayout(0.0f, 50.0f, 11.2f, 1.0f, true);
    UT_ASSERT_MSG(!m.visible, "11.2 px squares have no marker");
    m = lvBuilderMarkLayout(0.0f, 50.0f, 12.8f, 1.0f, true);
    UT_ASSERT_MSG(m.visible, "12.8 px squares have one");

    /* The arrow: y grows down the screen. */
    UT_ASSERT_MSG(lvBuilderDirection(0.0f, 0.0f) == -1, "at the tank: no way");
    UT_ASSERT_MSG(lvBuilderDirection(0.0f, -5.0f) == 0, "up");
    UT_ASSERT_MSG(lvBuilderDirection(5.0f, -5.0f) == 2, "up and right");
    UT_ASSERT_MSG(lvBuilderDirection(5.0f, 0.0f) == 4, "right");
    UT_ASSERT_MSG(lvBuilderDirection(5.0f, 5.0f) == 6, "down and right");
    UT_ASSERT_MSG(lvBuilderDirection(0.0f, 5.0f) == 8, "down");
    UT_ASSERT_MSG(lvBuilderDirection(-5.0f, 5.0f) == 10, "down and left");
    UT_ASSERT_MSG(lvBuilderDirection(-5.0f, 0.0f) == 12, "left");
    UT_ASSERT_MSG(lvBuilderDirection(-5.0f, -5.0f) == 14, "up and left");
    UT_ASSERT_MSG(lvBuilderDirection(2.0f, -5.0f) == 1, "between up and up-right");
    UT_ASSERT_MSG(lvBuilderDirection(-0.5f, -5.0f) == 0, "a little left of up "
                  "is up, not a 17th way");
    UT_ASSERT_MSG(lvBuilderDirection(-2.0f, -5.0f) == 15, "the last way");
    /* The distance does not matter, only the way. */
    UT_ASSERT_MSG(lvBuilderDirection(4096.0f, 16.0f) == 4, "far to the right");
    for (d = 0; d < LV_BUILDER_DIRECTIONS; d++) {
        lvBuilderDirectionStep(d, &x, &y);
        UT_ASSERT_MSG(bs_close(x * x + y * y, 1.0f), "way %d is a unit step", d);
        UT_ASSERT_MSG(lvBuilderDirection(x, y) == d, "way %d points its own "
                      "way (%.3f, %.3f)", d, x, y);
    }
    lvBuilderDirectionStep(0, &x, &y);
    UT_ASSERT_MSG(bs_close(x, 0.0f) && bs_close(y, -1.0f), "way 0 is up the screen");
    lvBuilderDirectionStep(4, &x, &y);
    UT_ASSERT_MSG(bs_close(x, 1.0f) && bs_close(y, 0.0f), "way 4 is to the right");

    /* A place in the log is a square and a sixteenth of one. */
    UT_ASSERT_MSG(lvBuilderWorld(0, 0) == 0 && lvBuilderWorld(1, 0) == 256 &&
                  lvBuilderWorld(50, 15) == 50 * 256 + 240, "world units");

    /* The pictures. */
    UT_ASSERT_MSG(lvBuilderParachute()->width == LV_BUILDER_CELLS &&
                  lvBuilderParachute()->height == LV_BUILDER_CELLS,
                  "the parachute fills the marker's box");
    UT_ASSERT_MSG(lvBuilderGlyphCell(lvBuilderParachute(), 3, 0) &&
                  !lvBuilderGlyphCell(lvBuilderParachute(), 0, 0) &&
                  lvBuilderGlyphCell(lvBuilderParachute(), 0, 1) &&
                  lvBuilderGlyphCell(lvBuilderParachute(), 3, 6) &&
                  !lvBuilderGlyphCell(lvBuilderParachute(), 0, 6),
                  "the parachute: a round canopy, the builder under its middle");
    UT_ASSERT_MSG(bs_cells(lvBuilderDot()) == 9, "the dot is 3 by 3");
    UT_ASSERT_MSG(!lvBuilderGlyphCell(lvBuilderDot(), 3, 0) &&
                  !lvBuilderGlyphCell(lvBuilderDot(), -1, 0) &&
                  !lvBuilderGlyphCell(NULL, 0, 0), "outside a picture: no cell");
    for (c = '0'; c <= '9'; c++) {
        const LvBuilderGlyph *g = lvBuilderChar((char)c);
        int other;
        UT_ASSERT_MSG(g != NULL && g->width == 3 && g->height == 5,
                      "digit %c is 3 by 5", c);
        UT_ASSERT_MSG(bs_cells(g) >= 7, "digit %c has cells", c);
        for (other = c + 1; other <= '9'; other++) {
            UT_ASSERT_MSG(memcmp(g->rows, lvBuilderChar((char)other)->rows,
                                 sizeof(g->rows)) != 0,
                          "digits %c and %c are the same picture", c, other);
        }
    }
    UT_ASSERT_MSG(lvBuilderChar(':') != NULL && lvBuilderChar(':')->width == 1 &&
                  bs_cells(lvBuilderChar(':')) == 2, "the colon: two cells");
    UT_ASSERT_MSG(lvBuilderChar('~') != NULL && lvBuilderChar('~')->width == 5,
                  "the tilde");
    UT_ASSERT_MSG(lvBuilderChar(' ') == NULL && lvBuilderChar('a') == NULL,
                  "no other character has a picture");

    /* Widths in cells: characters with one cell between each two. */
    UT_ASSERT_MSG(lvBuilderTextCells("") == 0 && lvBuilderTextCells(NULL) == 0,
                  "no text");
    UT_ASSERT_MSG(lvBuilderTextCells("0:07") == 3 + 1 + 1 + 1 + 3 + 1 + 3,
                  "\"0:07\" is %d cells", lvBuilderTextCells("0:07"));
    UT_ASSERT_MSG(lvBuilderTextCells("~0:07") == 5 + 1 + 13, "\"~0:07\"");
    UT_ASSERT_MSG(lvBuilderRowCells("") == LV_BUILDER_CELLS, "the picture alone");
    UT_ASSERT_MSG(lvBuilderRowCells("0:07") == LV_BUILDER_CELLS + 1 + 13,
                  "the picture, a cell, the time");
    return 0;
}

int run_lv_builder_countdown(void) {
    char small[4];

    /* Seconds are rounded up: "0:00" only when no time is left. */
    UT_ASSERT_MSG(bs_text(0, false, "0:00"), "no time left");
    UT_ASSERT_MSG(bs_text(1, false, "0:01"), "1 ms");
    UT_ASSERT_MSG(bs_text(20, false, "0:01"), "one tick");
    UT_ASSERT_MSG(bs_text(1000, false, "0:01"), "a second");
    UT_ASSERT_MSG(bs_text(1001, false, "0:02"), "a second and a little");
    UT_ASSERT_MSG(bs_text(7000, false, "0:07"), "seven seconds");
    UT_ASSERT_MSG(bs_text(59000, false, "0:59"), "59 seconds");
    UT_ASSERT_MSG(bs_text(59001, false, "1:00"), "into the minute");
    UT_ASSERT_MSG(bs_text(83000, false, "1:23"), "1:23");
    UT_ASSERT_MSG(bs_text(600000, false, "10:00"), "ten minutes");
    /* Never under zero, never over 99:59. */
    UT_ASSERT_MSG(bs_text(-1, false, "0:00"), "under zero");
    UT_ASSERT_MSG(bs_text(-500000, true, "~0:00"), "an estimate under zero");
    UT_ASSERT_MSG(bs_text(100L * 60L * 1000L, false, "99:59"), "100 minutes");
    /* An estimate has "~" in front. */
    UT_ASSERT_MSG(bs_text(7000, true, "~0:07"), "an estimate");
    UT_ASSERT_MSG(bs_text(100L * 60L * 1000L, true, "~99:59"), "the longest text");
    UT_ASSERT_MSG(strlen("~99:59") < LV_BUILDER_TEXT_MAX, "the longest text fits");
    /* A buffer too small holds what fits, with its end. */
    UT_ASSERT_MSG(lvBuilderCountdownText(83000, false, small, sizeof(small)) == 3 &&
                  strcmp(small, "1:2") == 0, "cut to the buffer (\"%s\")", small);
    UT_ASSERT_MSG(lvBuilderCountdownText(83000, false, NULL, 8) == 0 &&
                  lvBuilderCountdownText(83000, false, small, 0) == 0, "no buffer");

    /* The flight by the game's rule: the distance less what counts as
       there, at the speed, a tick of 20 ms for each step. */
    UT_ASSERT_MSG(lvBuilderFlightMs(0, 0, 3, 16) == 0, "there already");
    UT_ASSERT_MSG(lvBuilderFlightMs(16, 0, 3, 16) == 0, "inside the tolerance");
    UT_ASSERT_MSG(lvBuilderFlightMs(19, 0, 3, 16) == 20, "one step");
    UT_ASSERT_MSG(lvBuilderFlightMs(20, 0, 3, 16) == 40, "two steps");
    UT_ASSERT_MSG(lvBuilderFlightMs(-2048, 0, 3, 16) == 13560,
                  "8 squares: %ld ms", lvBuilderFlightMs(-2048, 0, 3, 16));
    UT_ASSERT_MSG(lvBuilderFlightMs(300, 400, 5, 0) == 2000, "3-4-5: 500 units "
                  "at 5 a tick");
    /* Ten squares at the game's own values: 17 seconds. */
    UT_ASSERT_MSG(lvBuilderFlightMs(2560, 0, LV_BUILDER_FLY_SPEED,
                                    LV_BUILDER_FLY_TOLERANCE) == 16960,
                  "ten squares: %ld ms", lvBuilderFlightMs(2560, 0, 3, 16));
    /* A speed that is no speed is the game's own. */
    UT_ASSERT_MSG(lvBuilderFlightMs(2560, 0, 0, 16) == 16960, "speed 0");
    UT_ASSERT_MSG(lvBuilderFlightMs(30, 0, 3, -5) == 200, "a tolerance under zero");
    return 0;
}

int run_lv_builder_trips(void) {
    LvBuilderTrack t;
    LvBuilderTrip  done, trips[3];
    long           remain = -1;

    memset(&t, 0, sizeof(t));
    memset(&done, 0, sizeof(done));

    /* In the tank, then out and alive: no trip. */
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 20, false, false, &done) && !t.open,
                  "in the tank");
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 40, true, false, &done) && !t.open,
                  "out and alive opens no trip");
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 60, false, false, &done) && !t.open,
                  "back in with no death");
    /* Killed (the log names no place in that tick), flies, lands, is killed
       again on his walk, flies, lands, walks, is in. One trip. */
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 100, false, true, &done) && t.open,
                  "the death opens a trip");
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 120, true, true, &done), "flying");
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 140, true, false, &done), "landed");
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 160, true, true, &done), "killed again");
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 180, true, false, &done), "landed again");
    UT_ASSERT_MSG(lvBuilderTrackTick(&t, 200, false, false, &done) && !t.open,
                  "in the tank: the trip is done");
    UT_ASSERT_MSG(done.deathMs == 100 && done.backMs == 200, "the trip is "
                  "%u to %u (want 100 to 200)", (unsigned)done.deathMs,
                  (unsigned)done.backMs);
    UT_ASSERT_MSG(!lvBuilderTrackEnd(&t, &done), "no trip is open at the end");
    /* The log ends with the builder away. */
    UT_ASSERT_MSG(!lvBuilderTrackTick(&t, 300, true, true, &done) && t.open,
                  "a second death");
    UT_ASSERT_MSG(lvBuilderTrackEnd(&t, &done) && done.deathMs == 300 &&
                  done.backMs == 0 && !t.open, "the open trip has no end");

    trips[0].deathMs = 100;  trips[0].backMs = 200;
    trips[1].deathMs = 500;  trips[1].backMs = 900;
    trips[2].deathMs = 2000; trips[2].backMs = 0;

    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 0) == -1, "before any trip");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 99) == -1, "just before the first");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 100) == 0, "at the death");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 180) == 0, "in the first");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 200) == -1, "at the return: in "
                  "the tank");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 300) == -1, "between two trips");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 500) == 1 &&
                  lvBuilderTripFind(trips, 3, 899) == 1, "in the second");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 900) == -1, "after the second");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 3, 2000) == 2 &&
                  lvBuilderTripFind(trips, 3, 999999) == 2, "the trip with no "
                  "end holds every later time");
    UT_ASSERT_MSG(lvBuilderTripFind(trips, 0, 150) == -1 &&
                  lvBuilderTripFind(NULL, 3, 150) == -1, "no table");

    UT_ASSERT_MSG(lvBuilderTripRemaining(trips, 3, 100, &remain) && remain == 100,
                  "at the death: %ld left (want 100)", remain);
    UT_ASSERT_MSG(lvBuilderTripRemaining(trips, 3, 180, &remain) && remain == 20,
                  "one tick before the return: %ld left", remain);
    UT_ASSERT_MSG(lvBuilderTripRemaining(trips, 3, 520, &remain) && remain == 380,
                  "in the second: %ld left", remain);
    remain = -7;
    UT_ASSERT_MSG(!lvBuilderTripRemaining(trips, 3, 200, &remain) && remain == -7,
                  "in the tank: no time, and the answer is not written");
    UT_ASSERT_MSG(!lvBuilderTripRemaining(trips, 3, 2500, &remain),
                  "the trip with no end has no time left to give");
    UT_ASSERT_MSG(!lvBuilderTripRemaining(NULL, 0, 150, &remain), "no table");
    return 0;
}
