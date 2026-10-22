/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The log viewer's Chat panel: what it shows at a replay time
 * (lv_chat_view.h), and the parts of a chat line the events list keeps for
 * it (lvEventListSetChat in lv_event_list.c).
 *
 *   lv_chat_view_fade    - full opacity while lines come, then a quick fade,
 *       all by replay time: a seek back or forward gives the opacity of that
 *       time.
 *   lv_chat_view_visible - the last lines at or before the playhead, never
 *       before the start of the part of the log that shows, oldest first.
 *   lv_chat_view_parts   - a chat line keeps its sender, note and words,
 *       and the list frees them.
 */

#include <math.h>
#include <string.h>

#include "lv_chat_view.h"
#include "lv_event_list.h"
#include "test_harness.h"

static int chat_near(float a, float b) { return fabsf(a - b) < 0.0001f; }

int run_lv_chat_view_fade(void) {
    const uint32_t t = 100000u;
    const uint32_t gone = t + LV_CHAT_HOLD_MS + LV_CHAT_FADE_MS;

    /* The values are open to change (lv_chat_view.h); the test uses the
     * constants, not their values. */
    UT_ASSERT_MSG(LV_CHAT_FADE_MS > 0u, "the fade takes some time");

    /* A new line is at full opacity at once, and stays so for the hold. */
    UT_ASSERT_MSG(lvChatAlpha(t, t) == 1.0f, "at the line's own time");
    UT_ASSERT_MSG(lvChatAlpha(t + 1u, t) == 1.0f, "a moment later");
    UT_ASSERT_MSG(lvChatAlpha(t + LV_CHAT_HOLD_MS, t) == 1.0f, "to the end of the hold");
    /* Then it fades, evenly. */
    UT_ASSERT_MSG(lvChatAlpha(t + LV_CHAT_HOLD_MS + 1u, t) < 1.0f, "the fade starts");
    UT_ASSERT_MSG(chat_near(lvChatAlpha(t + LV_CHAT_HOLD_MS + LV_CHAT_FADE_MS / 2u, t), 0.5f),
                  "half way through the fade");
    UT_ASSERT_MSG(lvChatAlpha(gone, t) == 0.0f, "gone at the end of the fade");
    UT_ASSERT_MSG(lvChatAlpha(gone + 100u * LV_CHAT_HOLD_MS, t) == 0.0f, "and stays gone");
    /* A line from after the time asked for is taken as just posted. */
    UT_ASSERT_MSG(lvChatAlpha(t, t + LV_CHAT_HOLD_MS + LV_CHAT_FADE_MS) == 1.0f, "no line from the future fades");

    /* Only replay time matters, so a seek back into the hold shows the lines
     * again at full opacity. */
    UT_ASSERT_MSG(lvChatAlpha(gone, t) == 0.0f && lvChatAlpha(t + LV_CHAT_HOLD_MS / 2u, t) == 1.0f,
                  "a seek back brings the lines back");

    /* The panel's opacity: full with nothing to list (its "no chat" line),
     * full with the mouse over it, else the lines'. */
    UT_ASSERT_MSG(lvChatShownAlpha(0, 0u, gone, 0) == 1.0f, "no line: the panel shows");
    UT_ASSERT_MSG(lvChatShownAlpha(3, t, gone, 1) == 1.0f, "hovered: full");
    UT_ASSERT_MSG(lvChatShownAlpha(3, t, gone, 0) == 0.0f, "not hovered: faded");
    UT_ASSERT_MSG(lvChatShownAlpha(3, t, t + 10u, 0) == 1.0f, "a new line: full");
    return 0;
}

int run_lv_chat_view_visible(void) {
    uint32_t times[14];
    int first = -1, end = -1;
    int i;

    for (i = 0; i < 14; i++) times[i] = 1000u * (uint32_t)(i + 1);  /* 1 s .. 14 s */

    /* Before the first line: none. */
    lvChatVisible(times, 14, 500u, 0u, LV_CHAT_MAX_LINES, &first, &end);
    UT_ASSERT_MSG(first == 0 && end == 0, "none before the first line: %d %d", first, end);
    /* A line counts from its own time. */
    lvChatVisible(times, 14, 1000u, 0u, LV_CHAT_MAX_LINES, &first, &end);
    UT_ASSERT_MSG(first == 0 && end == 1, "the first line at its time: %d %d", first, end);
    lvChatVisible(times, 14, 3500u, 0u, LV_CHAT_MAX_LINES, &first, &end);
    UT_ASSERT_MSG(first == 0 && end == 3, "three lines by 3.5 s: %d %d", first, end);
    /* No more than the most it lists: the newest ones. */
    lvChatVisible(times, 14, 14000u, 0u, 10, &first, &end);
    UT_ASSERT_MSG(first == 4 && end == 14, "the last ten: %d %d", first, end);
    lvChatVisible(times, 14, 9000000u, 0u, 4, &first, &end);
    UT_ASSERT_MSG(first == 10 && end == 14, "the last four: %d %d", first, end);
    /* A seek back gives the lines of that time, not the ones seen last. */
    lvChatVisible(times, 14, 2000u, 0u, 10, &first, &end);
    UT_ASSERT_MSG(first == 0 && end == 2, "after a seek back: %d %d", first, end);
    /* Nothing from before the part of the log that shows (the lobby). */
    lvChatVisible(times, 14, 6000u, 4500u, 10, &first, &end);
    UT_ASSERT_MSG(first == 4 && end == 6, "none before the start: %d %d", first, end);
    lvChatVisible(times, 14, 4000u, 4500u, 10, &first, &end);
    UT_ASSERT_MSG(first == end, "the playhead before the start: none: %d %d", first, end);
    /* Lines at one time are all in. */
    times[5] = 7000u;
    times[6] = 7000u;
    times[7] = 7000u;
    lvChatVisible(times, 14, 7000u, 0u, 10, &first, &end);
    UT_ASSERT_MSG(first == 0 && end == 8, "lines at one time: %d %d", first, end);
    /* No list, and no room. */
    lvChatVisible(NULL, 0, 7000u, 0u, 10, &first, &end);
    UT_ASSERT_MSG(first == 0 && end == 0, "no list: %d %d", first, end);
    lvChatVisible(times, 14, 7000u, 0u, 0, &first, &end);
    UT_ASSERT_MSG(first == end, "no room: %d %d", first, end);
    return 0;
}

int run_lv_chat_view_parts(void) {
    LvEvent *e;
    const LvEvent *got;

    lvEventListClear();
    e = lvEventListAdd("Message to all from Ann: hello", 1000u, true);
    UT_ASSERT_MSG(e != NULL, "the line is added");
    UT_ASSERT_MSG(e->chatFrom == NULL && e->chatNote == NULL && e->chatText == NULL,
                  "a new line is not a chat line");
    UT_ASSERT_MSG(lvEventListSetChat(e, "Ann", NULL, "hello"), "the parts are kept");
    e = lvEventListAdd("Message from Bob to Cy: hi team", 2000u, true);
    UT_ASSERT_MSG(e != NULL, "a second line");
    UT_ASSERT_MSG(lvEventListSetChat(e, "Bob", "(team)", "hi team"), "with a note");
    UT_ASSERT_MSG(!lvEventListSetChat(e, NULL, NULL, "x"), "no sender: refused");
    UT_ASSERT_MSG(!lvEventListSetChat(NULL, "a", NULL, "x"), "no line: refused");
    UT_ASSERT_MSG(lvEventListAdd("A tank was killed", 1500u, true) != NULL,
                  "a line between them moves the second one");

    got = lvEventListAt(0);
    UT_ASSERT_MSG(got != NULL && got->chatFrom != NULL && strcmp(got->chatFrom, "Ann") == 0 &&
                  got->chatNote == NULL && strcmp(got->chatText, "hello") == 0,
                  "the first line's parts");
    got = lvEventListAt(1);
    UT_ASSERT_MSG(got != NULL && got->chatFrom == NULL, "the line between is not chat");
    got = lvEventListAt(2);
    UT_ASSERT_MSG(got != NULL && got->chatFrom != NULL && strcmp(got->chatFrom, "Bob") == 0 &&
                  got->chatNote != NULL && strcmp(got->chatNote, "(team)") == 0 &&
                  strcmp(got->chatText, "hi team") == 0,
                  "the second line's parts, moved with it");
    /* The list frees the parts with the lines. */
    lvEventListClear();
    UT_ASSERT_MSG(lvEventListCount() == 0, "cleared");
    return 0;
}
