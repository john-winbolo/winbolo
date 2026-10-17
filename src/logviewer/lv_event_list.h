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
 * The lines the log viewer's events panel shows.
 *
 * Playback posts a line for each newswire and status message it decodes, at
 * the time it decodes it. A seek back replays the stretch after the snapshot
 * it restores from and posts those lines again, so the list keeps every line
 * it has been given in time order and drops one it already holds; the panel
 * shows the lines at or before the playhead. Nothing is removed on a seek, so
 * seeking forward over a stretch already played shows its lines without
 * playing it, and a line lost to a clear comes back when playback passes it.
 *
 * Pinned lines (the round summary and its highlight clips) are kept apart,
 * ahead of the rest, and always shown.
 */

#ifndef LV_EVENT_LIST_H
#define LV_EVENT_LIST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char    *text;
  uint32_t timeMs;    /* absolute log ms it was posted at; 0 when pinned */
  bool     showTime;  /* prefix the displayed line with its stamp */
  bool     pinned;
  bool     seekable;  /* a highlight clip: a click seeks to seekMs */
  uint32_t seekMs;
  int      mapX;
  int      mapY;
} LvEvent;

/* Drop every line. */
void lvEventListClear(void);

/* Add a timed line posted at timeMs, unless this pass of playback already
   gave the list the same line at that time. Lines posted at the same time
   keep the order they were posted in. */
void lvEventListAdd(const char *text, uint32_t timeMs, bool showTime);

/* Add a pinned line. A highlight clip passes seekable with its target. */
void lvEventListAddPinned(const char *text, bool seekable, uint32_t seekMs,
                          int mapX, int mapY);

/* Playback is starting again from a snapshot restore. Lines it posts from
   here are matched against those already held afresh. */
void lvEventListRewound(void);

/* Lines in display order: every pinned line, then the timed ones by time. */
int lvEventListCount(void);
const LvEvent *lvEventListAt(int i);

/* The pinned lines are [0, pinned count). The timed lines in [fromMs, toMs]
   are [*first, *end). */
int lvEventListPinnedCount(void);
void lvEventListRange(uint32_t fromMs, uint32_t toMs, int *first, int *end);

#ifdef __cplusplus
}
#endif

#endif /* LV_EVENT_LIST_H */
