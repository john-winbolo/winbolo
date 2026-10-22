/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_chat_view.h - what the log viewer's Chat panel shows at a replay time.
 *
 * The panel (imgui/imgui_chat.cpp) lists the last chat lines at or before the
 * playhead, oldest at the top. While new lines come, it shows them at full
 * opacity. When no new line has come for LV_CHAT_HOLD_MS of replay time, the
 * lines fade out over LV_CHAT_FADE_MS. A new line puts them back at full
 * opacity at once.
 *
 * Everything here is a function of the replay time and the times of the
 * lines, never of the clock on the wall. So a pause stops the fade, a speed
 * change speeds it up or slows it down, and a seek (forward or back) shows
 * at once what that replay time shows.
 *
 * Header only and free of ImGui, so the unit tests use the same rule
 * (tests/unit/test_lv_chat_view.c).
 */

#ifndef LV_CHAT_VIEW_H
#define LV_CHAT_VIEW_H

#include <stdint.h>

#include "lv_event_kind.h"   /* lvEventPastCount */

#ifdef __cplusplus
extern "C" {
#endif

/* How long the lines stay at full opacity after the newest one, in ms of
 * replay time. */
#define LV_CHAT_HOLD_MS 8000u

/* How long the fade out takes after that, in ms of replay time. */
#define LV_CHAT_FADE_MS 250u

/* The most lines the panel lists. */
#define LV_CHAT_MAX_LINES 10

/* The opacity of the lines at nowMs, from 0 (gone) to 1 (full), when the
 * newest line shown was posted at newestMs. A line from after nowMs is not
 * shown, so a newestMs past nowMs is taken as a line posted just now. */
static inline float lvChatAlpha(uint32_t nowMs, uint32_t newestMs) {
  uint32_t age;
  if (nowMs <= newestMs) return 1.0f;
  age = nowMs - newestMs;
  if (age <= LV_CHAT_HOLD_MS) return 1.0f;
  age -= LV_CHAT_HOLD_MS;
  if (age >= LV_CHAT_FADE_MS) return 0.0f;
  return 1.0f - (float)age / (float)LV_CHAT_FADE_MS;
}

/* The lines the panel lists at nowMs: [*first, *end) of the count times
 * (in ascending order). Those are the last maxLines lines at or before nowMs
 * that are not before fromMs (the start of the part of the log that shows;
 * 0 for all of it). *first == *end when there is none. */
static inline void lvChatVisible(const uint32_t *times, int count, uint32_t nowMs,
                                 uint32_t fromMs, int maxLines, int *first, int *end) {
  int lo = 0;
  int hi;
  int e;
  int f;
  if (times == NULL || count < 0) count = 0;
  e = lvEventPastCount(times, count, nowMs);
  /* The first line at or after fromMs. */
  hi = e;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (times[mid] < fromMs) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (maxLines < 0) maxLines = 0;
  f = e - maxLines;
  if (f < lo) f = lo;
  if (f > e) f = e;
  *first = f;
  *end = e;
}

/* The opacity the panel draws its lines with. shown is how many lines it
 * lists, newestMs the time of the last of them. hovered is not 0 while the
 * mouse is over the panel: the lines are then at full opacity, so they can
 * be read. With no line to list, the panel's "no chat yet" line shows at
 * full opacity, so an opened panel is never empty and unseen. */
static inline float lvChatShownAlpha(int shown, uint32_t newestMs, uint32_t nowMs,
                                     int hovered) {
  if (shown <= 0 || hovered) return 1.0f;
  return lvChatAlpha(nowMs, newestMs);
}

#ifdef __cplusplus
}
#endif

#endif /* LV_CHAT_VIEW_H */
