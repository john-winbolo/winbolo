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
 * Log-viewer highlight clip-time formatting: an absolute playback time in ms
 * rendered as m:ss (the tick->ms mapping is calibrated per log elsewhere).
 */
#include <string.h>

#include "lv_stats.h"
#include "test_harness.h"

int run_lv_stats_clip_time_format(void) {
  char buf[16];

  lvStatsFormatClipTime(0, buf, sizeof(buf));
  UT_ASSERT_MSG(strcmp(buf, "0:00") == 0, "zero -> 0:00, got %s", buf);

  /* 204 s -> 3:24. */
  lvStatsFormatClipTime(204000, buf, sizeof(buf));
  UT_ASSERT_MSG(strcmp(buf, "3:24") == 0, "204s -> 3:24, got %s", buf);

  /* Seconds zero-pad; minutes roll over: 65 s -> 1:05. */
  lvStatsFormatClipTime(65000, buf, sizeof(buf));
  UT_ASSERT_MSG(strcmp(buf, "1:05") == 0, "65s -> 1:05, got %s", buf);

  /* Sub-second truncates: 26:26.8 reads 26:26. */
  lvStatsFormatClipTime(1586800, buf, sizeof(buf));
  UT_ASSERT_MSG(strcmp(buf, "26:26") == 0, "1586800ms -> 26:26, got %s", buf);

  return 0;
}
