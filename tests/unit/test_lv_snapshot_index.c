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
 * The log viewer's snapshot index (snapshot.c): every snapshot playback
 * passes is recorded so a seek can restore the newest one at or before its
 * target. A server writes one every few seconds, so a log that ran for days
 * holds hundreds of thousands; the index was a linked list walked twice per
 * add, and a seek to the middle of a ten-day log took most of a minute.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "snapshot.h"
#include "test_harness.h"

static void addAt(snapshot *s, uint32_t time, BYTE team) {
  BYTE teams[MAX_TANKS];
  memset(teams, team, sizeof(teams));
  lv_snapshotAdd(s, (size_t)time * 10u, time, (BYTE)(time & 0xFF), teams);
}

int run_lv_snapshot_index_lookup(void) {
  snapshot s = lv_snapshotCreate();
  size_t pos = 0;
  uint32_t time = 0;
  BYTE key = 0;
  BYTE *teams = NULL;

  UT_ASSERT_MSG(lv_snapshotFindByTime(&s, 100, &pos, &time, &key, &teams) == FALSE,
                "an empty index found a snapshot");

  addAt(&s, 0, 1);
  addAt(&s, 2000, 2);
  addAt(&s, 4000, 3);
  /* A second pass over the same stretch, as after a seek back, adds nothing. */
  addAt(&s, 2000, 9);
  UT_ASSERT_MSG(lv_snapshotCount(&s) == 3, "count = %d (want 3)", lv_snapshotCount(&s));
  UT_ASSERT_MSG(lv_snapshotExist(&s, 0, 2000) == TRUE, "2000 not found");
  UT_ASSERT_MSG(lv_snapshotExist(&s, 0, 2001) == FALSE, "2001 found");

  /* One out of order goes in its place. */
  addAt(&s, 3000, 4);
  UT_ASSERT_MSG(lv_snapshotCount(&s) == 4, "count = %d (want 4)", lv_snapshotCount(&s));

  UT_ASSERT(lv_snapshotFindByTime(&s, 3500, &pos, &time, &key, &teams) == TRUE);
  UT_ASSERT_MSG(time == 3000 && pos == 30000 && teams[0] == 4,
                "find 3500 gave time %u pos %zu team %u", time, pos, teams[0]);
  UT_ASSERT(lv_snapshotFindByTime(&s, 2000, &pos, &time, &key, &teams) == TRUE);
  UT_ASSERT_MSG(time == 2000 && teams[0] == 2,
                "find 2000 gave time %u team %u (the duplicate replaced it?)", time, teams[0]);
  UT_ASSERT(lv_snapshotFindByTime(&s, 99999, &pos, &time, &key, &teams) == TRUE);
  UT_ASSERT_MSG(time == 4000, "find past the end gave %u (want 4000)", time);

  /* Backwards is the newest strictly before the time. */
  time = 3000;
  UT_ASSERT(lv_snapshotBackwards(&s, &pos, &time, &key, &teams) == TRUE);
  UT_ASSERT_MSG(time == 2000, "back from 3000 gave %u (want 2000)", time);
  time = 3001;
  UT_ASSERT(lv_snapshotBackwards(&s, &pos, &time, &key, &teams) == TRUE);
  UT_ASSERT_MSG(time == 3000, "back from 3001 gave %u (want 3000)", time);
  time = 0;
  UT_ASSERT_MSG(lv_snapshotBackwards(&s, &pos, &time, &key, &teams) == FALSE,
                "back from 0 found a snapshot");

  /* Forward is the oldest strictly after the time. */
  time = 2000;
  UT_ASSERT(lv_snapshotForward(&s, &pos, &time, &key, &teams) == TRUE);
  UT_ASSERT_MSG(time == 3000, "forward from 2000 gave %u (want 3000)", time);
  time = 4000;
  UT_ASSERT_MSG(lv_snapshotForward(&s, &pos, &time, &key, &teams) == FALSE,
                "forward from the last found a snapshot");

  lv_snapshotDestroy(&s);
  UT_ASSERT_MSG(s == NULL && lv_snapshotCount(&s) == 0, "destroy left entries");
  return 0;
}

/* Ten days of play at one snapshot every 2.54 s. The list took minutes to
 * build this many; the index takes milliseconds. */
int run_lv_snapshot_index_ten_days(void) {
  const uint32_t n = 370000;
  snapshot s = lv_snapshotCreate();
  size_t pos = 0;
  uint32_t time = 0;
  BYTE key = 0;
  BYTE *teams = NULL;
  uint32_t i;

  for (i = 0; i < n; i++) {
    addAt(&s, i * 2540u, (BYTE)(i & 0x7F));
  }
  UT_ASSERT_MSG(lv_snapshotCount(&s) == (int)n, "count = %d", lv_snapshotCount(&s));

  UT_ASSERT(lv_snapshotFindByTime(&s, (n / 2) * 2540u + 100u, &pos, &time, &key, &teams) == TRUE);
  UT_ASSERT_MSG(time == (n / 2) * 2540u && teams[0] == (BYTE)((n / 2) & 0x7F),
                "middle lookup gave time %u", time);

  lv_snapshotDestroy(&s);
  return 0;
}
