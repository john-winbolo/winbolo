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
 * The sort rule of the WinBolo.net log browser's results table. See
 * wbn_browser_sort.h.
 */

#include <ctype.h>
#include <stddef.h>

#include "wbn_browser_sort.h"

/* Compares two strings with no case. A null string is the empty string. */
static int compareText(const char *a, const char *b) {
  const unsigned char *pa = (const unsigned char *)(a ? a : "");
  const unsigned char *pb = (const unsigned char *)(b ? b : "");
  int ca, cb;
  for (;;) {
    ca = tolower(*pa);
    cb = tolower(*pb);
    if (ca != cb || ca == '\0') {
      return ca - cb;
    }
    pa++;
    pb++;
  }
}

static int compareInt(int a, int b) {
  return (a > b) - (a < b);
}

static int compareFloat(float a, float b) {
  return (a > b) - (a < b);
}

int wbnSortCompare(const wbnSortRow *a, const wbnSortRow *b,
                   wbnSortKey key, bool descending) {
  int result = 0;

  switch (key) {
  case WBN_SORT_MAP:
    result = compareText(a->map, b->map);
    break;
  case WBN_SORT_TYPE:
    result = compareText(a->gameType, b->gameType);
    break;
  case WBN_SORT_PLAYERS:
    result = compareInt(a->numPlayers, b->numPlayers);
    break;
  case WBN_SORT_RATING:
    result = compareFloat(a->rating, b->rating);
    if (result == 0) {
      result = compareInt(a->numRatings, b->numRatings);
    }
    break;
  case WBN_SORT_SIZE:
    result = compareInt(a->logSize, b->logSize);
    break;
  case WBN_SORT_DATE:
    result = compareInt(a->startTime, b->startTime);
    break;
  case WBN_SORT_NONE:
  default:
    break;
  }

  if (descending) {
    result = -result;
  }
  if (result == 0) {
    /* Equal on the key: keep the server's order, in both directions. */
    result = compareInt(a->serverIndex, b->serverIndex);
  }
  return result;
}
