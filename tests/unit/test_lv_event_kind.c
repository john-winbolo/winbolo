/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The pure helpers behind the log viewer's events panel (lv_event_kind.h).
 *
 *   lv_event_kind_sums — every kind has its own saved name and reads back
 *       from it; "all" and a name no build knows read as no filter; and the
 *       search that places the "now" divider counts the lines at or before
 *       the playhead, with the lines that share its tick on the past side.
 *   lv_event_click_pick — which line a click on the panel is for, and when
 *       the click also centres the map on it: a click on the selected line,
 *       and the second click of a double click.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lv_event_kind.h"
#include "test_harness.h"

int run_lv_event_kind_sums(void) {
    /* Summary lines are pinned at time 0; three lines share 400 ms. */
    static const uint32_t times[] = {0, 0, 20, 400, 400, 400, 900, 5000};
    const int n = (int)(sizeof(times) / sizeof(times[0]));
    int kind, other;

    /* Names: each kind has one, no two share one, and none is "all". */
    for (kind = 0; kind < LV_EVK_COUNT; kind++) {
        const char *key = lvEventKindKey(kind);
        UT_ASSERT_MSG(strcmp(key, LV_EVK_KEY_ALL) != 0,
                      "kind %d has no name of its own", kind);
        UT_ASSERT_MSG(lvEventKindFromKey(key) == kind,
                      "kind %d's name \"%s\" reads back as %d", kind, key,
                      lvEventKindFromKey(key));
        for (other = kind + 1; other < LV_EVK_COUNT; other++) {
            UT_ASSERT_MSG(strcmp(key, lvEventKindKey(other)) != 0,
                          "kinds %d and %d share the name \"%s\"", kind,
                          other, key);
        }
    }
    /* What is not a kind is the All filter, both ways. */
    UT_ASSERT_MSG(strcmp(lvEventKindKey(-1), LV_EVK_KEY_ALL) == 0,
                  "-1 is not saved as all");
    UT_ASSERT_MSG(strcmp(lvEventKindKey(LV_EVK_COUNT), LV_EVK_KEY_ALL) == 0,
                  "one past the last kind is not saved as all");
    UT_ASSERT_MSG(lvEventKindFromKey(LV_EVK_KEY_ALL) == -1, "all reads as %d",
                  lvEventKindFromKey(LV_EVK_KEY_ALL));
    UT_ASSERT_MSG(lvEventKindFromKey("a_kind_from_a_newer_build") == -1,
                  "an unknown name reads as a kind");
    UT_ASSERT_MSG(lvEventKindFromKey("") == -1, "an empty name reads as a kind");
    UT_ASSERT_MSG(lvEventKindFromKey(NULL) == -1, "no name reads as a kind");

    /* The divider: at 0 only the pinned lines are behind it. */
    UT_ASSERT_MSG(lvEventPastCount(times, n, 0) == 2, "at 0 ms: %d",
                  lvEventPastCount(times, n, 0));
    UT_ASSERT_MSG(lvEventPastCount(times, n, 19) == 2, "at 19 ms: %d",
                  lvEventPastCount(times, n, 19));
    UT_ASSERT_MSG(lvEventPastCount(times, n, 20) == 3, "at 20 ms: %d",
                  lvEventPastCount(times, n, 20));
    /* A click seeks to a line's own time: that line, and the others of its
       tick, are then in the past. */
    UT_ASSERT_MSG(lvEventPastCount(times, n, 399) == 3, "at 399 ms: %d",
                  lvEventPastCount(times, n, 399));
    UT_ASSERT_MSG(lvEventPastCount(times, n, 400) == 6, "at 400 ms: %d",
                  lvEventPastCount(times, n, 400));
    UT_ASSERT_MSG(lvEventPastCount(times, n, 4999) == 7, "at 4999 ms: %d",
                  lvEventPastCount(times, n, 4999));
    UT_ASSERT_MSG(lvEventPastCount(times, n, 5000) == n, "at the last line: %d",
                  lvEventPastCount(times, n, 5000));
    UT_ASSERT_MSG(lvEventPastCount(times, n, 0xFFFFFFFFu) == n,
                  "past the end: %d", lvEventPastCount(times, n, 0xFFFFFFFFu));

    /* Nothing shown: no line is in the past, and nothing is read. */
    UT_ASSERT_MSG(lvEventPastCount(times, 0, 400) == 0, "empty list");
    UT_ASSERT_MSG(lvEventPastCount(NULL, 5, 400) == 0, "no list");
    UT_ASSERT_MSG(lvEventPastCount(times, -3, 400) == 0, "negative count");
    /* One line, either side of the playhead. */
    UT_ASSERT_MSG(lvEventPastCount(times + 3, 1, 399) == 0, "one line, ahead");
    UT_ASSERT_MSG(lvEventPastCount(times + 3, 1, 400) == 1, "one line, passed");
    return 0;
}

/* The panel's state a click leaves, as imgui_events.cpp keeps it. */
typedef struct ClickState {
    int selected;
    int lastClicked;
} ClickState;

/* One plain click on a row: the pick, and the state after it. */
static LvEventClick click_row(ClickState *st, int row, int clicks, int count) {
    LvEventClick pick = lvEventClickPick(row, st->selected, st->lastClicked, clicks, count);
    st->selected = pick.target;
    st->lastClicked = pick.target;
    return pick;
}

int run_lv_event_click_pick(void) {
    const int n = 10;
    ClickState st = { -1, -1 };
    LvEventClick pick;

    /* The first click on a line only jumps to it. */
    pick = click_row(&st, 4, 1, n);
    UT_ASSERT_MSG(pick.target == 4 && pick.centre == 0, "first click: %d %d",
                  pick.target, pick.centre);
    /* A later click on the same line, however long after, centres. */
    pick = click_row(&st, 4, 1, n);
    UT_ASSERT_MSG(pick.target == 4 && pick.centre == 1, "second click: %d %d",
                  pick.target, pick.centre);
    /* And so does each click after that. */
    pick = click_row(&st, 4, 1, n);
    UT_ASSERT_MSG(pick.target == 4 && pick.centre == 1, "third click: %d %d",
                  pick.target, pick.centre);
    /* A click on another line is a first click again. */
    pick = click_row(&st, 7, 1, n);
    UT_ASSERT_MSG(pick.target == 7 && pick.centre == 0, "another line: %d %d",
                  pick.target, pick.centre);
    pick = click_row(&st, 4, 1, n);
    UT_ASSERT_MSG(pick.target == 4 && pick.centre == 0, "back to the first: %d %d",
                  pick.target, pick.centre);

    /* A double click: the first click jumps, the second centres. The end is
       the same as two slow clicks. */
    st.selected = -1;
    st.lastClicked = -1;
    pick = click_row(&st, 2, 1, n);
    UT_ASSERT_MSG(pick.target == 2 && pick.centre == 0, "double, first: %d %d",
                  pick.target, pick.centre);
    pick = click_row(&st, 2, 2, n);
    UT_ASSERT_MSG(pick.target == 2 && pick.centre == 1, "double, second: %d %d",
                  pick.target, pick.centre);
    UT_ASSERT_MSG(st.selected == 2, "double click leaves line %d selected", st.selected);

    /* The rows moved under the mouse after the first click: the second click
       of the double click is still for the line the first one chose. */
    st.selected = -1;
    st.lastClicked = -1;
    click_row(&st, 2, 1, n);
    pick = click_row(&st, 5, 2, n);
    UT_ASSERT_MSG(pick.target == 2 && pick.centre == 1, "rows moved: %d %d",
                  pick.target, pick.centre);
    /* A third quick click is for that line too. */
    pick = click_row(&st, 6, 3, n);
    UT_ASSERT_MSG(pick.target == 2 && pick.centre == 1, "triple click: %d %d",
                  pick.target, pick.centre);

    /* A double click on a selected line: each of its clicks centres. */
    st.selected = 3;
    st.lastClicked = 3;
    pick = click_row(&st, 3, 1, n);
    UT_ASSERT_MSG(pick.target == 3 && pick.centre == 1, "selected, first of two");
    pick = click_row(&st, 3, 2, n);
    UT_ASSERT_MSG(pick.target == 3 && pick.centre == 1, "selected, second of two");

    /* No line was clicked before (a Ctrl click forgets it): the count of
       clicks alone does not centre, and the row under the mouse is used. */
    pick = lvEventClickPick(5, -1, -1, 2, n);
    UT_ASSERT_MSG(pick.target == 5 && pick.centre == 0, "no line clicked before: %d %d",
                  pick.target, pick.centre);
    /* A line selected with Ctrl, then clicked: it is the selected line. */
    pick = lvEventClickPick(5, 5, -1, 1, n);
    UT_ASSERT_MSG(pick.target == 5 && pick.centre == 1, "selected with Ctrl");
    /* Every line selected (Select All) is no one selected line. */
    pick = lvEventClickPick(5, -1, 2, 1, n);
    UT_ASSERT_MSG(pick.target == 5 && pick.centre == 0, "select all");
    /* The clicked line is no longer in the list. */
    pick = lvEventClickPick(1, -1, 12, 2, n);
    UT_ASSERT_MSG(pick.target == 1 && pick.centre == 0, "clicked line gone: %d %d",
                  pick.target, pick.centre);
    /* No row is never the selected row. */
    pick = lvEventClickPick(-1, -1, -1, 1, n);
    UT_ASSERT_MSG(pick.centre == 0, "no row");
    return 0;
}
