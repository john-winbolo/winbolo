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

#include <stdlib.h>
#include <string.h>

#include "lv_event_list.h"

typedef struct {
  LvEvent *items;
  int      count;
  int      capacity;
} LvEventArray;

static LvEventArray s_pinned;
static LvEventArray s_timed;   /* in time order */

/* The lines this pass of playback has posted at s_passMs, the time it is
   posting at. A line is new when this pass has posted it more times at that
   time than the list holds. */
static bool     s_passValid = false;
static uint32_t s_passMs    = 0;
static char   **s_passTexts = NULL;
static int      s_passCount = 0;
static int      s_passCap   = 0;

static void lvEventArrayFree(LvEventArray *a) {
  int i;
  for (i = 0; i < a->count; i++) {
    free(a->items[i].text);
  }
  free(a->items);
  a->items = NULL;
  a->count = 0;
  a->capacity = 0;
}

/* Make room for one more entry at index at, and return it; NULL when it
   cannot grow. */
static LvEvent *lvEventArrayInsert(LvEventArray *a, int at) {
  if (a->count == a->capacity) {
    int      cap = (a->capacity == 0) ? 64 : a->capacity * 2;
    LvEvent *grown = (LvEvent *)realloc(a->items, (size_t)cap * sizeof(*grown));
    if (grown == NULL) {
      return NULL;
    }
    a->items = grown;
    a->capacity = cap;
  }
  if (at < a->count) {
    memmove(&a->items[at + 1], &a->items[at], (size_t)(a->count - at) * sizeof(*a->items));
  }
  a->count++;
  memset(&a->items[at], 0, sizeof(a->items[at]));
  return &a->items[at];
}

static char *lvEventDupText(const char *text) {
  size_t len = strlen(text);
  char  *copy = (char *)malloc(len + 1);
  if (copy != NULL) {
    memcpy(copy, text, len + 1);
  }
  return copy;
}

static void lvEventPassReset(void) {
  int i;
  for (i = 0; i < s_passCount; i++) {
    free(s_passTexts[i]);
  }
  s_passCount = 0;
  s_passValid = false;
}

/* Index of the first timed line after timeMs (or at or after, when
   inclusive is false). */
static int lvEventBound(uint32_t timeMs, bool inclusive) {
  int lo = 0;
  int hi = s_timed.count;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    uint32_t t = s_timed.items[mid].timeMs;
    if (inclusive ? (t <= timeMs) : (t < timeMs)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void lvEventListClear(void) {
  lvEventArrayFree(&s_pinned);
  lvEventArrayFree(&s_timed);
  lvEventPassReset();
  free(s_passTexts);
  s_passTexts = NULL;
  s_passCap = 0;
}

void lvEventListRewound(void) {
  lvEventPassReset();
}

LvEvent *lvEventListAdd(const char *text, uint32_t timeMs, bool showTime) {
  int      first;
  int      end;
  int      posted = 1;
  int      held = 0;
  int      i;
  char    *copy;
  LvEvent *e;

  if (text == NULL) {
    return NULL;
  }
  if (!s_passValid || s_passMs != timeMs) {
    lvEventPassReset();
    s_passValid = true;
    s_passMs = timeMs;
  }
  for (i = 0; i < s_passCount; i++) {
    if (strcmp(s_passTexts[i], text) == 0) {
      posted++;
    }
  }
  if (s_passCount == s_passCap) {
    int    cap = (s_passCap == 0) ? 8 : s_passCap * 2;
    char **grown = (char **)realloc(s_passTexts, (size_t)cap * sizeof(*grown));
    if (grown == NULL) {
      return NULL;
    }
    s_passTexts = grown;
    s_passCap = cap;
  }
  copy = lvEventDupText(text);
  if (copy == NULL) {
    return NULL;
  }
  s_passTexts[s_passCount++] = copy;

  first = lvEventBound(timeMs, false);
  end = lvEventBound(timeMs, true);
  for (i = first; i < end; i++) {
    if (strcmp(s_timed.items[i].text, text) == 0) {
      held++;
    }
  }
  if (posted <= held) {
    return NULL;   /* a replay of a line already held */
  }

  copy = lvEventDupText(text);
  if (copy == NULL) {
    return NULL;
  }
  e = lvEventArrayInsert(&s_timed, end);
  if (e == NULL) {
    free(copy);
    return NULL;
  }
  e->text = copy;
  e->timeMs = timeMs;
  e->showTime = showTime;
  return e;
}

LvEvent *lvEventListAddPinned(const char *text, bool seekable, uint32_t seekMs,
                              int mapX, int mapY) {
  char    *copy;
  LvEvent *e;

  if (text == NULL) {
    return NULL;
  }
  copy = lvEventDupText(text);
  if (copy == NULL) {
    return NULL;
  }
  e = lvEventArrayInsert(&s_pinned, s_pinned.count);
  if (e == NULL) {
    free(copy);
    return NULL;
  }
  e->text = copy;
  e->pinned = true;
  e->seekable = seekable;
  e->seekMs = seekMs;
  e->mapX = mapX;
  e->mapY = mapY;
  return e;
}

int lvEventListCount(void) {
  return s_pinned.count + s_timed.count;
}

const LvEvent *lvEventListAt(int i) {
  if (i < 0) {
    return NULL;
  }
  if (i < s_pinned.count) {
    return &s_pinned.items[i];
  }
  i -= s_pinned.count;
  if (i < s_timed.count) {
    return &s_timed.items[i];
  }
  return NULL;
}

LvEvent *lvEventListEdit(int i) {
  return (LvEvent *)lvEventListAt(i);
}

bool lvEventListAppendText(int i, const char *suffix) {
  LvEvent *e = lvEventListEdit(i);
  size_t   len;
  size_t   add;
  char    *grown;

  if (e == NULL || suffix == NULL) {
    return false;
  }
  len = strlen(e->text);
  add = strlen(suffix);
  grown = (char *)realloc(e->text, len + add + 1);
  if (grown == NULL) {
    return false;
  }
  memcpy(grown + len, suffix, add + 1);
  e->text = grown;
  return true;
}

int lvEventListPinnedCount(void) {
  return s_pinned.count;
}

void lvEventListRange(uint32_t fromMs, uint32_t toMs, int *first, int *end) {
  int lo = lvEventBound(fromMs, false);
  int hi = lvEventBound(toMs, true);
  if (hi < lo) {
    hi = lo;
  }
  *first = s_pinned.count + lo;
  *end = s_pinned.count + hi;
}
