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

#endif /* LV_EVENT_OVERLAY_H */
