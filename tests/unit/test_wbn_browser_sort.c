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
 * The sort rule of the WinBolo.net log browser's results table
 * (wbn_browser_sort.c): each column's key in both directions, ties in the
 * server's order, and no sort giving the server's order back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wbn_browser_sort.h"
#include "test_harness.h"

#define ROW_COUNT 5

/* Rows in the server's order (serverIndex 0..4). */
static const wbnSortRow s_rows[ROW_COUNT] = {
  /* map            type          plyr rating nrat  size   start  idx */
  { "chew toy",    "Open",        4,   3.5f,  2,    9000,  300,   0 },
  { "Birdnest",    "tournament",  2,   4.0f,  1,    1000,  500,   1 },
  { "BoloWar",     "Open",        4,   3.5f,  6,    5000,  100,   2 },
  { "Arena",       "strict",      1,   0.0f,  0,    5000,  400,   3 },
  { "beastly",     "open",        2,   4.0f,  1,    7000,  200,   4 },
};

static wbnSortKey s_key;
static bool       s_desc;

static int compareForQsort(const void *a, const void *b) {
  return wbnSortCompare((const wbnSortRow *)a, (const wbnSortRow *)b,
                        s_key, s_desc);
}

/* Sorts a copy of s_rows and writes the server indices in order, as "a,b,..". */
static const char *sortedOrder(wbnSortKey key, bool desc) {
  static char buf[64];
  wbnSortRow rows[ROW_COUNT];
  int i;
  memcpy(rows, s_rows, sizeof(rows));
  s_key = key;
  s_desc = desc;
  qsort(rows, ROW_COUNT, sizeof(rows[0]), compareForQsort);
  buf[0] = '\0';
  for (i = 0; i < ROW_COUNT; i++) {
    char one[8];
    snprintf(one, sizeof(one), "%s%d", i ? "," : "", rows[i].serverIndex);
    strcat(buf, one);
  }
  return buf;
}

static int expectOrder(const char *what, wbnSortKey key, bool desc,
                       const char *want) {
  const char *got = sortedOrder(key, desc);
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "wbn_browser_sort: %s: got %s, want %s\n", what, got, want);
    return 1;
  }
  return 0;
}

int run_wbn_browser_sort(void) {
  int fails = 0;

  /* No sort: the server's order. */
  fails += expectOrder("none", WBN_SORT_NONE, false, "0,1,2,3,4");
  fails += expectOrder("none desc", WBN_SORT_NONE, true, "0,1,2,3,4");

  /* Map: text with no case. */
  fails += expectOrder("map asc", WBN_SORT_MAP, false, "3,4,1,2,0");
  fails += expectOrder("map desc", WBN_SORT_MAP, true, "0,2,1,4,3");

  /* Type: "Open" and "open" are equal, so they keep the server's order. */
  fails += expectOrder("type asc", WBN_SORT_TYPE, false, "0,2,4,3,1");
  fails += expectOrder("type desc", WBN_SORT_TYPE, true, "1,3,0,2,4");

  /* Players: ties in the server's order in both directions. */
  fails += expectOrder("players asc", WBN_SORT_PLAYERS, false, "3,1,4,0,2");
  fails += expectOrder("players desc", WBN_SORT_PLAYERS, true, "0,2,1,4,3");

  /* Rating, then the count of ratings. Rows 1 and 4 are equal on both. */
  fails += expectOrder("rating asc", WBN_SORT_RATING, false, "3,0,2,1,4");
  fails += expectOrder("rating desc", WBN_SORT_RATING, true, "1,4,2,0,3");

  /* Size: rows 2 and 3 are equal. */
  fails += expectOrder("size asc", WBN_SORT_SIZE, false, "1,2,3,4,0");
  fails += expectOrder("size desc", WBN_SORT_SIZE, true, "0,4,2,3,1");

  /* Date. */
  fails += expectOrder("date asc", WBN_SORT_DATE, false, "2,4,0,3,1");
  fails += expectOrder("date desc", WBN_SORT_DATE, true, "1,3,0,4,2");

  /* A null map is the empty string and goes first. */
  {
    wbnSortRow a = s_rows[0];
    wbnSortRow b = s_rows[1];
    a.map = NULL;
    if (wbnSortCompare(&a, &b, WBN_SORT_MAP, false) >= 0) {
      fprintf(stderr, "wbn_browser_sort: a null map did not go first\n");
      fails++;
    }
  }

  return fails == 0 ? 0 : 1;
}
