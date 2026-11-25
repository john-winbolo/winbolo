/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer slow tick records
 *Filename:      lv_slowtick.h
 *Purpose:
 *  The recording's log_SlowTick records (one server frame
 *  that took more than 20 ms or started late, and the time of
 *  each of its parts, slow_tick_record.h), each at the
 *  playback time the viewer read it.
 *
 *  Header only, with no includes but the C library's and the
 *  record's own header, so the viewer (C and C++) and the unit
 *  tests use the same code.
 *********************************************************/

#ifndef LV_SLOWTICK_H
#define LV_SLOWTICK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "slow_tick_record.h"

/* The most records a track keeps. A file that holds more keeps the first
   ones. A server that keeps up writes none. */
#define LV_SLOWTICK_TRACK_MAX 65536

/* One record and the playback time it was read at. */
typedef struct {
  uint32_t       ms;
  SlowTickRecord rec;
} LvSlowTickEntry;

/* The records in time order. */
typedef struct {
  LvSlowTickEntry *items;
  int              count;
  int              cap;
} LvSlowTickTrack;

/* What lv_screenSlowTickNear gives. recorded: the recording holds a record
   somewhere. has: one is near the time asked, in rec, read at ms. */
typedef struct {
  bool           recorded;
  bool           has;
  uint32_t       ms;
  SlowTickRecord rec;
} LvSlowTickNow;

static inline void lv_slowTickTrackClear(LvSlowTickTrack *t) {
  free(t->items);
  t->items = NULL;
  t->count = 0;
  t->cap = 0;
}

/* Add rec at ms. A record the track already holds at that time, for the
   same server tick, changes nothing: a loaded file's walk reads each record
   once and playback reads it again. Returns TRUE when the track took it. */
static inline bool lv_slowTickTrackAdd(LvSlowTickTrack *t, uint32_t ms, const SlowTickRecord *rec) {
  int at;
  int i;

  /* Most adds come in time order, so look from the end. */
  at = t->count;
  while (at > 0 && t->items[at - 1].ms > ms) {
    at--;
  }
  for (i = at - 1; i >= 0 && t->items[i].ms == ms; i--) {
    if (t->items[i].rec.tick == rec->tick) {
      return false;
    }
  }
  if (t->count >= LV_SLOWTICK_TRACK_MAX) {
    return false;
  }
  if (t->count == t->cap) {
    int cap = t->cap == 0 ? 16 : t->cap * 2;
    LvSlowTickEntry *items;
    if (cap > LV_SLOWTICK_TRACK_MAX) {
      cap = LV_SLOWTICK_TRACK_MAX;
    }
    items = (LvSlowTickEntry *) realloc(t->items, (size_t) cap * sizeof(LvSlowTickEntry));
    if (items == NULL) {
      return false;
    }
    t->items = items;
    t->cap = cap;
  }
  if (at < t->count) {
    memmove(&t->items[at + 1], &t->items[at],
            (size_t) (t->count - at) * sizeof(LvSlowTickEntry));
  }
  t->items[at].ms = ms;
  t->items[at].rec = *rec;
  t->count++;
  return true;
}

/* The record read closest to ms, no more than withinMs before or after it,
   or NULL when there is none. At an equal distance the earlier one. */
static inline const LvSlowTickEntry *lv_slowTickTrackNear(const LvSlowTickTrack *t,
                                                          uint32_t ms, uint32_t withinMs) {
  int lo = 0;
  int hi = t->count;
  const LvSlowTickEntry *best = NULL;
  uint32_t bestDist = 0;

  /* The first entry later than ms. */
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (t->items[mid].ms <= ms) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo > 0) {
    uint32_t d = ms - t->items[lo - 1].ms;
    if (d <= withinMs) {
      best = &t->items[lo - 1];
      bestDist = d;
    }
  }
  if (lo < t->count) {
    uint32_t d = t->items[lo].ms - ms;
    if (d <= withinMs && (best == NULL || d < bestDist)) {
      best = &t->items[lo];
    }
  }
  return best;
}

#endif /* LV_SLOWTICK_H */
