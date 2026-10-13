/*
 * lv_event_overlay.h - the sums behind the event markers of the seek slider
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The Controls panel can show a marker over its seek slider for each event
 * of the log ("Overlay events"). This header has the parts of that with no
 * ImGui and no decoder in them, so the unit tests can run them: where on
 * the slider a time is, which markers are too close to each other and
 * become one, and how the list of chosen kinds is written in the
 * preferences.
 */

#ifndef LV_EVENT_OVERLAY_H
#define LV_EVENT_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lv_event_kind.h"

/* The room an ImGui slider keeps between each end of its frame and its
 * handle (grab_padding in ImGui::SliderBehaviorT). */
#define LV_OVERLAY_GRAB_PAD 2.0f

/* The kinds a new install shows: what changes the score of a game. */
#define LV_OVERLAY_DEFAULT_MASK                                     \
  ((1u << LV_EVK_PILL_CAPTURED) | (1u << LV_EVK_PILL_STOLEN) |      \
   (1u << LV_EVK_BASE_CAPTURED) | (1u << LV_EVK_BASE_STOLEN) |      \
   (1u << LV_EVK_BUILDER_LOST) | (1u << LV_EVK_TANK_KILLED))

/* Every kind. */
#define LV_OVERLAY_ALL_MASK ((1u << LV_EVK_COUNT) - 1u)

/* What an empty choice is saved as. An empty text would read back as "no
 * choice was ever saved", which is the default list. */
#define LV_OVERLAY_KEY_NONE "none"

/* The most teams a marker can name: 16 teams and "no team". */
#define LV_OVERLAY_TEAMS 17

/* How far along the log a time is, 0 at the start of the shown stretch and
 * 1 at its end. It is the value the seek slider has when playback is at
 * that time (currentTime / totalTime of lv_screenGetLogProgress). */
static inline float lvOverlayRatio(uint32_t ms, uint32_t startMs,
                                   uint32_t lenMs) {
  if (lenMs == 0 || ms <= startMs) return 0.0f;
  if (ms - startMs >= lenMs) return 1.0f;
  return (float)(ms - startMs) / (float)lenMs;
}

/* The width of the slider's handle: grabMin (the style's GrabMinSize) cut
 * to what the frame has room for. */
static inline float lvOverlayGrabSize(float x0, float x1, float grabMin) {
  float room = (x1 - x0) - LV_OVERLAY_GRAB_PAD * 2.0f;
  if (room < 0.0f) room = 0.0f;
  return (grabMin < room) ? grabMin : room;
}

/* The x of the middle of the slider's handle for value t (0..1), in a frame
 * from x0 to x1. It is the sum ImGui does (SliderBehaviorT), so a marker
 * drawn here is exactly over the place the handle has when playback is at
 * the marker's time. */
static inline float lvOverlaySliderX(float x0, float x1, float grabMin,
                                     float t) {
  float grab = lvOverlayGrabSize(x0, x1, grabMin);
  float lo = x0 + LV_OVERLAY_GRAB_PAD + grab * 0.5f;
  float hi = x1 - LV_OVERLAY_GRAB_PAD - grab * 0.5f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return lo + (hi - lo) * t;
}

/* One marker: the events from `first` on, `count` of them, drawn at x. */
typedef struct {
  float x;
  int first;
  int count;
} LvOverlayCluster;

/* Put markers that are too close to each other together. xs are the places
 * of n events in ascending order. A marker starts at an event and takes in
 * every later event less than minGap from that first one, so two markers
 * are never nearer than minGap and none hides another. The marker is drawn
 * at the place of its first event, which is the one a click goes to.
 * Returns the number of markers written to out (at most outMax; the events
 * past the last marker that fits are left out). */
static inline int lvOverlayClusters(const float *xs, int n, float minGap,
                                    LvOverlayCluster *out, int outMax) {
  int made = 0;
  int i = 0;
  if (xs == NULL || out == NULL) return 0;
  while (i < n && made < outMax) {
    int j = i + 1;
    while (j < n && xs[j] - xs[i] < minGap) j++;
    out[made].x = xs[i];
    out[made].first = i;
    out[made].count = j - i;
    made++;
    i = j;
  }
  return made;
}

/* The marker the point x is on: the nearest one within halfWidth, or -1. */
static inline int lvOverlayHit(const LvOverlayCluster *clusters, int count,
                               float x, float halfWidth) {
  int best = -1;
  float bestDist = 0.0f;
  int i;
  if (clusters == NULL) return -1;
  for (i = 0; i < count; i++) {
    float d = clusters[i].x - x;
    if (d < 0.0f) d = -d;
    if (d > halfWidth) continue;
    if (best < 0 || d < bestDist) {
      best = i;
      bestDist = d;
    }
  }
  return best;
}

/* The team that most of the counted events name, or -1 when none names a
 * team. counts has LV_OVERLAY_TEAMS entries; the last is "no team" and does
 * not count. With two teams level the lower number wins, so the answer does
 * not change from frame to frame. */
static inline int lvOverlayMajorityTeam(const int *counts) {
  int best = -1;
  int team;
  if (counts == NULL) return -1;
  for (team = 0; team < LV_OVERLAY_TEAMS - 1; team++) {
    if (counts[team] > 0 && (best < 0 || counts[team] > counts[best])) {
      best = team;
    }
  }
  return best;
}

/* The chosen kinds as one bit a kind, read from the text the preferences
 * keep: the saved names of the kinds (lvEventKindKey) with commas between
 * them. Spaces round a name are ignored. A name this build does not know (a
 * list written by a newer build, or "none") adds nothing. */
static inline unsigned lvOverlayMaskFromText(const char *text) {
  unsigned mask = 0;
  const char *p = text;
  if (text == NULL) return 0;
  while (*p != '\0') {
    char name[32];
    size_t len = 0;
    const char *end;
    while (*p == ' ' || *p == ',') p++;
    end = p;
    while (*end != '\0' && *end != ',') end++;
    len = (size_t)(end - p);
    while (len > 0 && p[len - 1] == ' ') len--;
    if (len > 0 && len < sizeof(name)) {
      int kind;
      memcpy(name, p, len);
      name[len] = '\0';
      kind = lvEventKindFromKey(name);
      if (kind >= 0) mask |= 1u << kind;
    }
    p = end;
  }
  return mask;
}

/* The text the preferences keep for a choice of kinds. An empty choice is
 * "none". The text is cut at a whole name when dest is too small. */
static inline void lvOverlayMaskToText(unsigned mask, char *dest,
                                       size_t destSize) {
  size_t used = 0;
  int kind;
  if (dest == NULL || destSize == 0) return;
  dest[0] = '\0';
  for (kind = 0; kind < LV_EVK_COUNT; kind++) {
    const char *key;
    size_t need;
    if ((mask & (1u << kind)) == 0) continue;
    key = lvEventKindKey(kind);
    need = strlen(key) + (used > 0 ? 1 : 0);
    if (used + need + 1 > destSize) break;
    if (used > 0) dest[used++] = ',';
    memcpy(dest + used, key, strlen(key) + 1);
    used += strlen(key);
  }
  if (used == 0) {
    snprintf(dest, destSize, "%s", LV_OVERLAY_KEY_NONE);
  }
}

/* ---- One list of ticked kinds, two users ---------------------------------
 * The "Overlay events" list of the Controls panel and the filter of the
 * Events panel are the same control (lv_event_kinds_combo), each with its
 * own choice. Both ask the same question of each event: */

/* True when an event of this kind is one of the ticked kinds. A kind out of
 * range is never ticked. */
static inline bool lvEventKindTicked(unsigned mask, int kind) {
  if (kind < 0 || kind >= LV_EVK_COUNT) return false;
  return (mask & (1u << kind)) != 0;
}

/* How many events the ticked kinds have between them; counts has
 * LV_EVK_COUNT entries. */
static inline int lvEventKindsTickedCount(unsigned mask, const int *counts) {
  int kind, total = 0;
  if (counts == NULL) return 0;
  for (kind = 0; kind < LV_EVK_COUNT; kind++) {
    if (lvEventKindTicked(mask, kind)) total += counts[kind];
  }
  return total;
}

/* The Events panel's choice as the preferences give it. kindsText is the
 * saved list ("Events.Kinds"), empty when none was ever saved. The panel
 * used to show one kind or all, saved as one name ("Events.Filter"): with
 * no list saved that old choice is taken over, one kind as that kind alone
 * and "all" (or nothing, or a name this build does not know) as every kind.
 * A saved list always wins, also "none". */
static inline unsigned lvEventsMaskFromPrefs(const char *kindsText,
                                             const char *oldFilterText) {
  int kind;
  if (kindsText != NULL && kindsText[0] != '\0') {
    return lvOverlayMaskFromText(kindsText);
  }
  kind = lvEventKindFromKey(oldFilterText);
  if (kind >= 0) return 1u << kind;
  return LV_OVERLAY_ALL_MASK;
}

/* What the closed filter of the Events panel has on it, in the room it has:
 * its whole text ("All events (412)"), only the number ("(412)"), or no
 * text, the arrow alone. frameW is what a combo adds to its text: the
 * padding and the arrow. */
typedef enum {
  LV_EVENTS_LABEL_FULL = 0,
  LV_EVENTS_LABEL_COUNT,
  LV_EVENTS_LABEL_ARROW
} LvEventsLabel;

static inline LvEventsLabel lvEventsLabelPick(float availW, float fullW,
                                              float countW, float frameW) {
  if (fullW + frameW <= availW) return LV_EVENTS_LABEL_FULL;
  if (countW + frameW <= availW) return LV_EVENTS_LABEL_COUNT;
  return LV_EVENTS_LABEL_ARROW;
}

/* The marker of a pillbox event or a base event is the map's picture of a
 * pillbox or a base, in the colour of the team the event names (the new
 * owner). Every other kind has a drawn icon. */
typedef enum {
  LV_OVERLAY_PIC_NONE = 0, /* a drawn icon */
  LV_OVERLAY_PIC_PILL,
  LV_OVERLAY_PIC_BASE
} LvOverlayPicture;

static inline LvOverlayPicture lvOverlayKindPicture(int kind) {
  switch (kind) {
  case LV_EVK_PILL_CAPTURED:
  case LV_EVK_PILL_STOLEN:
    return LV_OVERLAY_PIC_PILL;
  case LV_EVK_BASE_CAPTURED:
  case LV_EVK_BASE_STOLEN:
    return LV_OVERLAY_PIC_BASE;
  default:
    return LV_OVERLAY_PIC_NONE;
  }
}

/* The picture of a marker that stands for several events: the pillbox when
 * every one of them is a pillbox event (captured or stolen, in any mix), the
 * base when every one is a base event, none for any other mix. kinds has
 * count entries. */
static inline LvOverlayPicture lvOverlayClusterPicture(const int *kinds, int count) {
  LvOverlayPicture pic;
  int i;
  if (count <= 0) return LV_OVERLAY_PIC_NONE;
  pic = lvOverlayKindPicture(kinds[0]);
  for (i = 1; i < count && pic != LV_OVERLAY_PIC_NONE; i++) {
    if (lvOverlayKindPicture(kinds[i]) != pic) pic = LV_OVERLAY_PIC_NONE;
  }
  return pic;
}

/* ---- One marker for an event, three users ---------------------------------
 * The lane over the seek slider, the list of kinds and each line of the
 * Events panel show the same marker for an event. All three ask this
 * function what the marker is and one function draws it
 * (lv_event_marker_draw), so they cannot differ. */

/* What the marker of one event is:
 *   picture  the map's picture of a pillbox or a base, or none (a drawn icon)
 *   kind     the kind the drawn icon is of (lvEventKind)
 *   team     the team whose colour the marker has: the colour of the picture
 *            (-1: the game's red pillbox, the game's neutral base), or of
 *            the line round the icon (-1: no line) */
typedef struct {
  LvOverlayPicture picture;
  int kind;
  int team;
} LvEventMarker;

/* The marker of an event of this kind that names this team (the new owner of
 * a pillbox or a base, the killer of a tank, the sender of a chat line; -1
 * when the event names no team). A kind out of range is a game event, as
 * the Events panel files it; a team that is not one of the 16 is no team. */
static inline LvEventMarker lvEventMarker(int kind, int team) {
  LvEventMarker m;
  if (kind < 0 || kind >= LV_EVK_COUNT) kind = LV_EVK_GAME;
  if (team < 0 || team >= LV_OVERLAY_TEAMS - 1) team = -1;
  m.picture = lvOverlayKindPicture(kind);
  m.kind = kind;
  m.team = team;
  return m;
}

#endif /* LV_EVENT_OVERLAY_H */
