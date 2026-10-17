/*
 * lv_ping_view.h - the map pings of a log, kept by time
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * A log_Ping record holds who sent the ping, its kind and the place on the
 * map. The viewer keeps every one it decodes, in the order of their times on
 * the playback clock. What the map shows is then a question with one answer
 * for a time: the pings sent in the five seconds before it. So the marker is
 * right at any playback speed, when playback is paused part way through one,
 * and after a jump into the middle of one, and its animation is the game's
 * own, which is worked out from the ping's age alone (pingMarkerDraw).
 *
 * This header has no SDL, no ImGui and nothing of the decoder in it, so the
 * unit tests can run it.
 */

#ifndef LV_PING_VIEW_H
#define LV_PING_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The most pings kept. A sender can ping a few times a second at the very
 * most, and a round has some hundreds. When the store is full the oldest
 * goes. */
#define LV_PING_STORE_MAX 4096

/* The decoder passes the same record again on every seek, and once more in
 * the pass that lists the events of the whole log. A ping with the sender,
 * kind and place of one already kept, and a time less than this from it, is
 * that ping and is not kept twice. The server does not take two pings from
 * one sender this close together. */
#define LV_PING_SAME_MS 100u

/* The number of kinds of ping (PING_KIND_COUNT of input_packet.h; screen.c
 * checks the two at compile time). A kind past the last is drawn as the
 * first, the plain ping, as the game draws it. */
#define LV_PING_KINDS 6

typedef struct {
  uint32_t timeMs; /* the playback clock when the ping was sent */
  uint8_t  sender; /* the player slot that sent it */
  uint8_t  kind;   /* PING_KIND_*, as the record has it */
  uint16_t worldX; /* the place, 256 to a map square */
  uint16_t worldY;
} LvPingRec;

/* The most pings on the map at one time that the viewer draws. When more
 * are on the map, the newest are drawn. */
#define LV_PING_SHOWN_MAX 16

/* One ping on the map at the playback's time now, as lv_screenGetPings
 * gives it. ageMs is the time since the ping on the playback clock. */
typedef struct {
  uint8_t  sender;
  uint8_t  kind;
  uint16_t worldX;
  uint16_t worldY;
  uint32_t ageMs;
} LvPingShown;

typedef struct {
  LvPingRec rec[LV_PING_STORE_MAX]; /* in the order of their times */
  int       count;
} LvPingStore;

static inline void lvPingStoreClear(LvPingStore *s) { s->count = 0; }

static inline bool lvPingRecSame(const LvPingRec *r, uint8_t sender, uint8_t kind,
                                 uint16_t worldX, uint16_t worldY) {
  return r->sender == sender && r->kind == kind && r->worldX == worldX &&
         r->worldY == worldY;
}

/* Keep a ping. False when it is one the store already has (see
 * LV_PING_SAME_MS), or when the store is full and this ping is older than
 * all of them. The kind and the place are kept as given. */
static inline bool lvPingStoreAdd(LvPingStore *s, uint32_t timeMs, uint8_t sender,
                                  uint8_t kind, uint16_t worldX, uint16_t worldY) {
  int at = s->count;
  int i;
  /* Playback adds at the end; a seek back adds in the middle. */
  while (at > 0 && s->rec[at - 1].timeMs > timeMs) at--;
  for (i = at - 1; i >= 0 && timeMs - s->rec[i].timeMs < LV_PING_SAME_MS; i--) {
    if (lvPingRecSame(&s->rec[i], sender, kind, worldX, worldY)) return false;
  }
  for (i = at; i < s->count && s->rec[i].timeMs - timeMs < LV_PING_SAME_MS; i++) {
    if (lvPingRecSame(&s->rec[i], sender, kind, worldX, worldY)) return false;
  }
  if (s->count >= LV_PING_STORE_MAX) {
    if (at == 0) return false;
    memmove(&s->rec[0], &s->rec[1], (size_t)(s->count - 1) * sizeof(s->rec[0]));
    s->count--;
    at--;
  }
  memmove(&s->rec[at + 1], &s->rec[at], (size_t)(s->count - at) * sizeof(s->rec[0]));
  s->rec[at].timeMs = timeMs;
  s->rec[at].sender = sender;
  s->rec[at].kind   = kind;
  s->rec[at].worldX = worldX;
  s->rec[at].worldY = worldY;
  s->count++;
  return true;
}

/* The age of a ping sent at pingMs when the playback clock reads nowMs, or
 * -1 when the ping is not on the map then: it is still to come, or it was
 * sent displayMs or more ago (PING_DISPLAY_MS). The age is all the game's
 * drawing of the marker needs. */
static inline int32_t lvPingAgeMs(uint32_t nowMs, uint32_t pingMs, uint32_t displayMs) {
  if (pingMs > nowMs) return -1;
  if (nowMs - pingMs >= displayMs) return -1;
  return (int32_t)(nowMs - pingMs);
}

/* The pings on the map when the playback clock reads nowMs: rec[*first] and
 * the count - 1 after it, the oldest first. Returns that count. */
static inline int lvPingStoreShown(const LvPingStore *s, uint32_t nowMs, uint32_t displayMs,
                                   int *first) {
  int lo = 0, hi = s->count, end;
  /* The first ping later than now. */
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (s->rec[mid].timeMs <= nowMs) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  end = lo;
  while (lo > 0 && lvPingAgeMs(nowMs, s->rec[lo - 1].timeMs, displayMs) >= 0) lo--;
  if (first != NULL) *first = lo;
  return end - lo;
}

/* Does the player whose view the map has see this ping. The game's rule
 * (serverSimPingReachesClient): a ping goes to its sender, to the players
 * allied with the sender, and to the players of the sender's lobby team. A
 * sender with no lobby team (team 0) has no team mates.
 *   hasViewer   the map is one player's view; false is the view of all
 *               teams, which shows every ping
 *   sender      the slot that sent the ping
 *   viewer      the slot whose view it is
 *   allied      the two are allied
 *   senderTeam, viewerTeam  their lobby teams as the log has them
 *               (log_TeamSet); 0 for no team, and for a team the log does
 *               not give
 * The game also leaves out a player who has turned the sender's pings off.
 * The log does not record that, so it is not known here. */
static inline bool lvPingSeen(bool hasViewer, int sender, int viewer, bool allied,
                              int senderTeam, int viewerTeam) {
  if (!hasViewer) return true;
  if (sender == viewer) return true;
  if (allied) return true;
  if (senderTeam == 0) return false;
  return senderTeam == viewerTeam;
}

/* The kind a ping is drawn as: its own, or the plain ping for a kind this
 * build does not know (pingKindStyle does the same). */
static inline int lvPingDrawnKind(int kind) {
  return (kind >= 0 && kind < LV_PING_KINDS) ? kind : 0;
}

#endif /* LV_PING_VIEW_H */
