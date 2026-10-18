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
 * The sort rule of the WinBolo.net log browser's results table.
 *
 * The table sorts the page of rows it holds when the player clicks a column
 * header. Plain C with no ImGui headers, split out of imgui_wbn_browser.cpp
 * so test_wbn_browser_sort.c drives the rule the table runs.
 */

#ifndef WBN_BROWSER_SORT_H
#define WBN_BROWSER_SORT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The column a row is sorted on. The values are the table's column user ids. */
typedef enum {
  WBN_SORT_NONE = 0, /* the order the server sent */
  WBN_SORT_MAP,
  WBN_SORT_TYPE,
  WBN_SORT_PLAYERS,
  WBN_SORT_RATING,
  WBN_SORT_SIZE,
  WBN_SORT_DATE
} wbnSortKey;

/* The fields of one row that the sort reads. */
typedef struct {
  const char *map;
  const char *gameType;
  int   numPlayers;
  float rating;
  int   numRatings;
  int   logSize;
  int   startTime;
  int   serverIndex; /* the row's place in the page the server sent */
} wbnSortRow;

/*
 * Compares two rows on key: less than zero when a goes first, more than zero
 * when b goes first. Map and Type compare as text with no case; Rating
 * compares the rating, then the count of ratings. descending turns the key's
 * order around. Rows that are equal on the key, and all rows under
 * WBN_SORT_NONE, go in the server's order, so the result is the same each
 * time the same page is sorted.
 */
int wbnSortCompare(const wbnSortRow *a, const wbnSortRow *b,
                   wbnSortKey key, bool descending);

#ifdef __cplusplus
}
#endif

#endif /* WBN_BROWSER_SORT_H */
