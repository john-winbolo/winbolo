/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer bot host records
 *Filename:      lv_bothost.h
 *Purpose:
 *  The recording's log_BotHostData records (what the server's
 *  bot placement measured and chose for each host,
 *  bothost_record.h), each at the playback time the viewer
 *  read it, in time order. The Bot Hosts window draws its
 *  graphs from them; the per-host helpers below pick one
 *  host's records.
 *
 *  Header only, with no includes but the C library's and the
 *  record's own header, so the viewer (C and C++) and the unit
 *  tests use the same code.
 *********************************************************/

#ifndef LV_BOTHOST_H
#define LV_BOTHOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bothost_record.h"

/* The most records a track keeps. A server writes about one probe a second
   and one reading each 5 seconds for each host, so this is many hours of
   play with a few hosts; a file that holds more keeps the first ones. */
#define LV_BOTHOST_TRACK_MAX 100000

/* The most hosts a record can name (its host byte). */
#define LV_BOTHOST_HOSTS 256

/* One record and the playback time it was read at. */
typedef struct {
  uint32_t      ms;
  BotHostRecord rec;
} LvBotHostEntry;

/* The records in time order. */
typedef struct {
  LvBotHostEntry *items;
  int             count;
  int             cap;
} LvBotHostTrack;

static inline void lv_botHostTrackClear(LvBotHostTrack *t) {
  free(t->items);
  t->items = NULL;
  t->count = 0;
  t->cap = 0;
}

/* Whether two records hold the same fields: their encoded bytes match (the
   struct's padding bytes are not compared). The version byte is not part of
   the match, as the encoder writes its own. */
static inline bool lv_botHostRecordSame(const BotHostRecord *a, const BotHostRecord *b) {
  uint8_t ea[BOTHOST_RECORD_MAX_BYTES];
  uint8_t eb[BOTHOST_RECORD_MAX_BYTES];
  int na = botHostRecordEncode(a, ea, (int) sizeof(ea));
  int nb = botHostRecordEncode(b, eb, (int) sizeof(eb));
  return na == nb && na > 0 && memcmp(ea, eb, (size_t) na) == 0;
}

/* Add rec at ms. A record the track already holds at that time, the same in
   every field, changes nothing: a loaded file's walk reads each record once
   and playback reads it again. Returns TRUE when the track took it. */
static inline bool lv_botHostTrackAdd(LvBotHostTrack *t, uint32_t ms, const BotHostRecord *rec) {
  int at;
  int i;

  /* Most adds come in time order, so look from the end. */
  at = t->count;
  while (at > 0 && t->items[at - 1].ms > ms) {
    at--;
  }
  for (i = at - 1; i >= 0 && t->items[i].ms == ms; i--) {
    if (lv_botHostRecordSame(&t->items[i].rec, rec)) {
      return false;
    }
  }
  if (t->count >= LV_BOTHOST_TRACK_MAX) {
    return false;
  }
  if (t->count == t->cap) {
    int cap = t->cap == 0 ? 64 : t->cap * 2;
    LvBotHostEntry *items;
    if (cap > LV_BOTHOST_TRACK_MAX) {
      cap = LV_BOTHOST_TRACK_MAX;
    }
    items = (LvBotHostEntry *) realloc(t->items, (size_t) cap * sizeof(LvBotHostEntry));
    if (items == NULL) {
      return false;
    }
    t->items = items;
    t->cap = cap;
  }
  if (at < t->count) {
    memmove(&t->items[at + 1], &t->items[at],
            (size_t) (t->count - at) * sizeof(LvBotHostEntry));
  }
  t->items[at].ms = ms;
  t->items[at].rec = *rec;
  t->count++;
  return true;
}

/* Decode one payload of len bytes and add it at ms. FALSE when the payload
   is not a record this viewer reads (too short, version 0, unknown kind) or
   the track already holds it. */
static inline bool lv_botHostTrackAddPayload(LvBotHostTrack *t, uint32_t ms,
                                             const uint8_t *payload, int len) {
  BotHostRecord rec;
  if (!botHostRecordDecode(payload, len, &rec)) {
    return false;
  }
  return lv_botHostTrackAdd(t, ms, &rec);
}

/* The hosts the track has any record for: seen[h] is TRUE for each. Returns
   how many there are. */
static inline int lv_botHostTrackHosts(const LvBotHostTrack *t, bool seen[LV_BOTHOST_HOSTS]) {
  int i;
  int n = 0;
  memset(seen, 0, sizeof(bool) * LV_BOTHOST_HOSTS);
  for (i = 0; i < t->count; i++) {
    uint8_t h = t->items[i].rec.host;
    if (!seen[h]) {
      seen[h] = true;
      n++;
    }
  }
  return n;
}

/* Who host is, from its last HOST record at or before ms (UINT32_MAX for
   any time): the player whose own Bot Host it is, or BOTHOST_RECORD_NONE,
   and its name. FALSE, with player NONE and an empty name, when there is no
   HOST record for it. */
static inline bool lv_botHostTrackInfo(const LvBotHostTrack *t, uint8_t host, uint32_t ms,
                                       uint8_t *player, char *name, size_t nameCap) {
  int i;
  const BotHostRecord *found = NULL;
  for (i = 0; i < t->count && t->items[i].ms <= ms; i++) {
    const BotHostRecord *r = &t->items[i].rec;
    if (r->kind == BOTHOST_RECORD_HOST && r->host == host) {
      found = r;
    }
  }
  if (player != NULL) {
    *player = found != NULL ? found->player : (uint8_t) BOTHOST_RECORD_NONE;
  }
  if (name != NULL && nameCap > 0) {
    snprintf(name, nameCap, "%s", found != NULL ? found->name : "");
  }
  return found != NULL;
}

/* The index of host's next record of kind at or after from, or -1. kind 0
   takes any kind. */
static inline int lv_botHostTrackNext(const LvBotHostTrack *t, uint8_t host, uint8_t kind, int from) {
  int i;
  for (i = from < 0 ? 0 : from; i < t->count; i++) {
    const BotHostRecord *r = &t->items[i].rec;
    if (r->host == host && (kind == 0 || r->kind == kind)) {
      return i;
    }
  }
  return -1;
}

/* A probe's overrun rate in percent: its overruns over its thinks. 0 for a
   probe with no thinks. */
static inline double lv_botHostProbeRate(const BotHostRecord *r) {
  return r->thinks > 0 ? (double) r->overruns * 100.0 / (double) r->thinks : 0.0;
}

#endif /* LV_BOTHOST_H */
