/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer overrun records
 *Filename:      lv_overrun.h
 *Purpose:
 *  The recording's log_OverrunStats records (the server's
 *  timing and each bot's overruns, overrun_record.h), each at
 *  the playback time the viewer read it. The record at a time
 *  is the last one at or before it.
 *
 *  Header only, with no includes but the C library's and the
 *  record's own header, so the viewer (C and C++) and the unit
 *  tests use the same code.
 *********************************************************/

#ifndef LV_OVERRUN_H
#define LV_OVERRUN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "overrun_record.h"

/* The most records a track keeps. The server writes one every 30 seconds,
   so this is over 34 hours of rounds; a file that holds more keeps the first
   ones. */
#define LV_OVERRUN_TRACK_MAX 4096

/* One record and the playback time it was read at. */
typedef struct {
  uint32_t      ms;
  OverrunRecord rec;
} LvOverrunEntry;

/* The records in time order. */
typedef struct {
  LvOverrunEntry *items;
  int             count;
  int             cap;
} LvOverrunTrack;

/* What the viewer's panel reads at the playhead. recorded: the recording
   holds a record somewhere. has: one is at or before the playhead, in rec,
   read at ms. */
typedef struct {
  bool          recorded;
  bool          has;
  uint32_t      ms;
  OverrunRecord rec;
} LvOverrunNow;

static inline void lv_overrunTrackClear(LvOverrunTrack *t) {
  free(t->items);
  t->items = NULL;
  t->count = 0;
  t->cap = 0;
}

/* Add rec at ms. A record the track already holds at that time, with the
   same round frame and flags, changes nothing: a loaded file's walk reads
   each record once and playback reads it again. Returns TRUE when the track
   took it. */
static inline bool lv_overrunTrackAdd(LvOverrunTrack *t, uint32_t ms, const OverrunRecord *rec) {
  int at;
  int i;

  /* Most adds come in time order, so look from the end. */
  at = t->count;
  while (at > 0 && t->items[at - 1].ms > ms) {
    at--;
  }
  for (i = at - 1; i >= 0 && t->items[i].ms == ms; i--) {
    if (t->items[i].rec.roundFrames == rec->roundFrames &&
        t->items[i].rec.flags == rec->flags) {
      return false;
    }
  }
  if (t->count >= LV_OVERRUN_TRACK_MAX) {
    return false;
  }
  if (t->count == t->cap) {
    int cap = t->cap == 0 ? 16 : t->cap * 2;
    LvOverrunEntry *items;
    if (cap > LV_OVERRUN_TRACK_MAX) {
      cap = LV_OVERRUN_TRACK_MAX;
    }
    items = (LvOverrunEntry *) realloc(t->items, (size_t) cap * sizeof(LvOverrunEntry));
    if (items == NULL) {
      return false;
    }
    t->items = items;
    t->cap = cap;
  }
  if (at < t->count) {
    memmove(&t->items[at + 1], &t->items[at],
            (size_t) (t->count - at) * sizeof(LvOverrunEntry));
  }
  t->items[at].ms = ms;
  t->items[at].rec = *rec;
  t->count++;
  return true;
}

/* The last record at or before ms, or NULL when there is none. */
static inline const LvOverrunEntry *lv_overrunTrackAt(const LvOverrunTrack *t, uint32_t ms) {
  int lo = 0;
  int hi = t->count;

  /* The first entry later than ms. */
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (t->items[mid].ms <= ms) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo > 0 ? &t->items[lo - 1] : NULL;
}

#endif /* LV_OVERRUN_H */
