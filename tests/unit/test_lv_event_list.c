/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * The log viewer's events panel list (lv_event_list.c). It used to delete the
 * lines after a seek back and have playback post them again; it now keeps
 * every line, drops a replayed one it already holds, and the panel shows the
 * lines up to the playhead.
 */

#include <stdio.h>
#include <string.h>

#include "lv_event_list.h"
#include "test_harness.h"

/* The timed lines in [fromMs, toMs], joined with '|'. */
static const char *shown(uint32_t fromMs, uint32_t toMs) {
  static char buf[512];
  int first, end, i;
  buf[0] = '\0';
  lvEventListRange(fromMs, toMs, &first, &end);
  for (i = first; i < end; i++) {
    if (buf[0] != '\0') strcat(buf, "|");
    strcat(buf, lvEventListAt(i)->text);
  }
  return buf;
}

int run_lv_event_list_replay(void) {
  lvEventListClear();

  /* First pass: two lines at 20 (the same text twice), one at 40, one at 60. */
  lvEventListAdd("A", 20, true);
  lvEventListAdd("A", 20, true);
  lvEventListAdd("B", 40, true);
  lvEventListAdd("C", 60, true);
  UT_ASSERT_MSG(strcmp(shown(0, 100), "A|A|B|C") == 0, "first pass: %s", shown(0, 100));
  UT_ASSERT_MSG(strcmp(shown(0, 40), "A|A|B") == 0, "up to 40: %s", shown(0, 40));
  UT_ASSERT_MSG(strcmp(shown(0, 19), "") == 0, "before 20: %s", shown(0, 19));

  /* A seek back to 0 replays the same lines: none is added twice. */
  lvEventListRewound();
  lvEventListAdd("A", 20, true);
  lvEventListAdd("A", 20, true);
  lvEventListAdd("B", 40, true);
  UT_ASSERT_MSG(lvEventListCount() == 4, "replay added lines: %d", lvEventListCount());

  /* A line at a held time that the first pass did not post is new, and goes
     after the ones already there. */
  lvEventListAdd("D", 60, true);
  UT_ASSERT_MSG(strcmp(shown(0, 100), "A|A|B|C|D") == 0, "new at 60: %s", shown(0, 100));

  /* Seeking back to the same snapshot twice running still adds nothing. */
  lvEventListRewound();
  lvEventListAdd("A", 20, true);
  lvEventListRewound();
  lvEventListAdd("A", 20, true);
  lvEventListAdd("A", 20, true);
  UT_ASSERT_MSG(lvEventListCount() == 5, "repeated rewinds added lines: %d", lvEventListCount());

  /* After a clear, playing a stretch again brings its lines back. */
  lvEventListClear();
  lvEventListRewound();
  lvEventListAdd("A", 20, true);
  UT_ASSERT_MSG(strcmp(shown(0, 100), "A") == 0, "after clear: %s", shown(0, 100));

  lvEventListClear();
  return 0;
}

int run_lv_event_list_pinned(void) {
  int first, end;

  lvEventListClear();
  lvEventListAdd("A", 20, true);
  lvEventListAddPinned("Summary", false, 0, 0, 0);
  lvEventListAddPinned("Clip", true, 5000, 3, 4);
  lvEventListAdd("B", 40, true);

  /* Pinned lines come first whatever the order they were added in, and the
     timed range indexes past them. */
  UT_ASSERT(lvEventListPinnedCount() == 2);
  UT_ASSERT(lvEventListCount() == 4);
  UT_ASSERT(strcmp(lvEventListAt(0)->text, "Summary") == 0);
  UT_ASSERT(lvEventListAt(1)->seekable && lvEventListAt(1)->seekMs == 5000 &&
            lvEventListAt(1)->mapX == 3 && lvEventListAt(1)->mapY == 4);
  lvEventListRange(30, 100, &first, &end);
  UT_ASSERT_MSG(first == 3 && end == 4, "range [30,100] = [%d,%d) (want [3,4))", first, end);
  UT_ASSERT(lvEventListAt(4) == NULL);

  lvEventListClear();
  UT_ASSERT(lvEventListCount() == 0 && lvEventListPinnedCount() == 0);
  return 0;
}
