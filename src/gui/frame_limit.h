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

/*********************************************************
 *Name:          Frame limit
 *Filename:      frame_limit.h
 *Purpose:
 *  The Frame Rate choice (#312): how often the game window
 *  is drawn. It changes drawing only. The game tick runs on
 *  its own timer and does not change.
 *
 *  The choices are "Match display" (vsync, the default),
 *  fixed limits of 30, 60, 120, 144 and 240 frames per
 *  second (vsync stays on), and "Unlimited" (vsync off, no
 *  limit).
 *
 *  It is saved as [MENU] "Frame Limit" with a word for each
 *  choice. Older builds saved [MENU] "Frame Rate" as one of
 *  the old FRAME_RATE_* numbers; frameLimitLoad reads that
 *  when there is no new value, and moves it to the nearest
 *  new choice. See docs/FRAMERATE_DECISIONS.md.
 *
 *  Header only, with no SDL calls, so every frontend and
 *  the unit tests can include it without a new source file
 *  in each build.
 *********************************************************/

#ifndef FRAME_LIMIT_H
#define FRAME_LIMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum {
  FRAME_LIMIT_VSYNC = 0,    /* one frame per display refresh */
  FRAME_LIMIT_30,
  FRAME_LIMIT_60,
  FRAME_LIMIT_120,
  FRAME_LIMIT_144,
  FRAME_LIMIT_240,
  FRAME_LIMIT_UNLIMITED,    /* vsync off, no limit */
  FRAME_LIMIT_COUNT
} FrameLimit;

#define FRAME_LIMIT_DEFAULT FRAME_LIMIT_VSYNC

/* The preferences file section and keys. */
#define FRAME_LIMIT_PREF_SECTION "MENU"
#define FRAME_LIMIT_PREF_KEY     "Frame Limit"
#define FRAME_LIMIT_OLD_PREF_KEY "Frame Rate"

/* The old FRAME_RATE_* values that older builds saved. They were never
 * the rates on their labels: with the old timer the "30" setting drew at
 * about 58 frames per second. Kept here only to read an old value. */
#define FRAME_LIMIT_OLD_10 19
#define FRAME_LIMIT_OLD_12 23
#define FRAME_LIMIT_OLD_15 28
#define FRAME_LIMIT_OLD_20 37
#define FRAME_LIMIT_OLD_30 55   /* the old default */
#define FRAME_LIMIT_OLD_50 82
#define FRAME_LIMIT_OLD_60 111

/* True when value is one of the choices. */
static inline bool frameLimitValid(int value) {
  return value >= 0 && value < FRAME_LIMIT_COUNT;
}

/* The limit in frames per second, or 0 for none (Match display and
 * Unlimited). */
static inline int frameLimitCapHz(int limit) {
  switch (limit) {
  case FRAME_LIMIT_30:  return 30;
  case FRAME_LIMIT_60:  return 60;
  case FRAME_LIMIT_120: return 120;
  case FRAME_LIMIT_144: return 144;
  case FRAME_LIMIT_240: return 240;
  default:              return 0;
  }
}

/* True when the choice keeps vsync on. Only Unlimited turns it off, so
 * only Unlimited can tear. */
static inline bool frameLimitUsesVsync(int limit) {
  return limit != FRAME_LIMIT_UNLIMITED;
}

/* The word saved in the preferences file. Not shown to the player. */
static inline const char *frameLimitPrefString(int limit) {
  switch (limit) {
  case FRAME_LIMIT_30:        return "30";
  case FRAME_LIMIT_60:        return "60";
  case FRAME_LIMIT_120:       return "120";
  case FRAME_LIMIT_144:       return "144";
  case FRAME_LIMIT_240:       return "240";
  case FRAME_LIMIT_UNLIMITED: return "Unlimited";
  default:                    return "Match display";
  }
}

/* The choice a saved word stands for, or -1 when it is none of them. */
static inline int frameLimitFromPrefString(const char *s) {
  int i;
  if (s == NULL) return -1;
  for (i = 0; i < FRAME_LIMIT_COUNT; i++) {
    if (strcmp(s, frameLimitPrefString(i)) == 0) return i;
  }
  return -1;
}

/* The new choice nearest to what an old FRAME_RATE_* value really did.
 *
 * The old "60" and "50" drew at the display refresh (vsync was the real
 * limit), so they become Match display. The old "30" was the default and
 * drew at about 58, which is the display refresh on most screens; almost
 * everyone has it only because it was the default, so it becomes the new
 * default, Match display. The old "20" and lower drew at 19 to 38 frames
 * per second; a player picked those on purpose to draw less, so they
 * become the lowest new limit, 30. Any other number becomes the default. */
static inline int frameLimitFromOldFrameRate(int old) {
  switch (old) {
  case FRAME_LIMIT_OLD_10:
  case FRAME_LIMIT_OLD_12:
  case FRAME_LIMIT_OLD_15:
  case FRAME_LIMIT_OLD_20:
    return FRAME_LIMIT_30;
  case FRAME_LIMIT_OLD_30:
  case FRAME_LIMIT_OLD_50:
  case FRAME_LIMIT_OLD_60:
  default:
    return FRAME_LIMIT_DEFAULT;
  }
}

/* Read the choice from the two saved strings: newValue is [MENU]
 * "Frame Limit" and oldValue is [MENU] "Frame Rate", each "" (or NULL)
 * when the file does not have it. A valid new value wins. Otherwise an
 * old value that is a whole number is moved across. Otherwise the
 * default. */
static inline int frameLimitLoad(const char *newValue, const char *oldValue) {
  int v = frameLimitFromPrefString(newValue);
  if (v >= 0) return v;
  if (oldValue != NULL && oldValue[0] != '\0') {
    int n = 0;
    const char *p = oldValue;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return FRAME_LIMIT_DEFAULT;
    while (*p >= '0' && *p <= '9' && n < 100000) {
      n = n * 10 + (*p - '0');
      p++;
    }
    return frameLimitFromOldFrameRate(n);
  }
  return FRAME_LIMIT_DEFAULT;
}

/* How many display refreshes apart the frames are for a limit, at a
 * refresh period of periodNs: 1 when there is no limit or the limit is
 * at or above the refresh rate. The menu's background game uses it. */
static inline int frameLimitRefreshesPerFrame(int limit, long long periodNs) {
  int hz = frameLimitCapHz(limit);
  long long frameNs, n;
  if (hz <= 0 || periodNs <= 0) return 1;
  frameNs = 1000000000LL / hz;
  n = (frameNs + periodNs / 2) / periodNs;   /* nearest whole number */
  return n < 1 ? 1 : (int)n;
}

#endif /* FRAME_LIMIT_H */
