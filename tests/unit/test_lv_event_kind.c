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
