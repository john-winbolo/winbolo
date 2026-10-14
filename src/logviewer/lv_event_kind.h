/*
 * lv_event_kind.h - what kind of thing an Events panel line reports
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Every lv_messageAdd call site states the kind of its line, so the panel's
 * filter never has to guess one from the rendered (and translated) text.
 * Header-only and free of ImGui and of the decoder, so the unit tests can
 * exercise the pure helpers below.
 */

#ifndef LV_EVENT_KIND_H
#define LV_EVENT_KIND_H

#include <stdint.h>
#include <string.h>

/* The values are indexes into the panel's per-kind counts. They are not
 * stored anywhere: the saved filter is written by name (lvEventKindKey), so
 * a kind can be added or moved without breaking a saved choice. */
typedef enum {
  LV_EVK_PILL_CAPTURED = 0, /* a neutral pillbox was taken */
  LV_EVK_PILL_STOLEN,       /* a pillbox was taken from another player */
  LV_EVK_BASE_CAPTURED,     /* a neutral base was taken */
  LV_EVK_BASE_STOLEN,       /* a base was taken from another player */
  LV_EVK_BUILDER_LOST,      /* a builder (LGM) was killed */
  LV_EVK_TANK_KILLED,       /* a tank was killed, or died on its own */
  LV_EVK_PLAYER,            /* joins, leaves, renames, teams, alliances */
  LV_EVK_CHAT,              /* chat lines and map pings */
  LV_EVK_GAME,              /* scenario, server, lobby, vote and end-of-log lines */
  LV_EVK_HIGHLIGHT,         /* load-time highlight clips and summary lines */
  LV_EVK_COUNT
} lvEventKind;

/* The name a kind is saved under in the preferences. "all" is the filter
 * that shows every kind; it is what an unknown or out-of-range kind maps to. */
#define LV_EVK_KEY_ALL "all"

static inline const char *lvEventKindKey(int kind) {
  switch (kind) {
    case LV_EVK_PILL_CAPTURED: return "pill_captured";
    case LV_EVK_PILL_STOLEN:   return "pill_stolen";
    case LV_EVK_BASE_CAPTURED: return "base_captured";
    case LV_EVK_BASE_STOLEN:   return "base_stolen";
    case LV_EVK_BUILDER_LOST:  return "builder_lost";
    case LV_EVK_TANK_KILLED:   return "tank_killed";
    case LV_EVK_PLAYER:        return "player";
    case LV_EVK_CHAT:          return "chat";
    case LV_EVK_GAME:          return "game";
    case LV_EVK_HIGHLIGHT:     return "highlight";
    default:                   return LV_EVK_KEY_ALL;
  }
}

/* The kind a saved name stands for, or -1 for "all" and for any name this
 * build does not know (a preference written by a newer build). */
static inline int lvEventKindFromKey(const char *key) {
  int kind;
  if (key == NULL) return -1;
  for (kind = 0; kind < LV_EVK_COUNT; kind++) {
    if (strcmp(key, lvEventKindKey(kind)) == 0) return kind;
  }
  return -1;
}

/* How many of the first `count` times are at or before nowMs: the index of
 * the first line still in the future, which is where the panel draws its
 * "now" divider. `times` must be in ascending order. A binary search, so the
 * panel can ask every frame however long the log is. */
static inline int lvEventPastCount(const uint32_t *times, int count,
                                   uint32_t nowMs) {
  int lo = 0;
  int hi = (count > 0 && times != NULL) ? count : 0;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (times[mid] <= nowMs) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* What a plain click on a line of the panel does. target is the line it is
 * for. centre is 1 when the click also puts the line's place in the middle
 * of the map that shows, 0 when it only jumps to the line.
 *
 * row: the line under the mouse. selected: the one selected line, -1 for
 * none (and when every line is selected). lastClicked: the line the last
 * plain click jumped to, -1 for none. clicks: 1 for one click, 2 or more for
 * the later clicks of a double click. count: how many lines there are.
 *
 * A click on the line that is selected already is a centre click. So is the
 * second click of a double click, and that one is for the line the first
 * click chose, whatever row is under the mouse now: the first click's jump
 * can move the rows. Both end the same way, so a double click and two slow
 * clicks on a line do the same thing. */
typedef struct LvEventClick {
  int target;
  int centre;
} LvEventClick;

static inline LvEventClick lvEventClickPick(int row, int selected, int lastClicked,
                                            int clicks, int count) {
  LvEventClick out;
  if (clicks >= 2 && lastClicked >= 0 && lastClicked < count) {
    out.target = lastClicked;
    out.centre = 1;
    return out;
  }
  out.target = row;
  out.centre = (row >= 0 && row == selected) ? 1 : 0;
  return out;
}

/* What the jump of a click on a line of the panel does to the map view.
 * A plain click moves playback to the line and leaves the view where it is:
 * the ring round the place (or the arrow at the view's edge, when the view
 * does not show it) says where the event is. A centre click (lvEventClickPick)
 * also puts the place in the middle of the map that shows. */
typedef enum {
  LV_EVENT_JUMP_KEEP_VIEW = 0,
  LV_EVENT_JUMP_CENTRE
} LvEventJumpView;

typedef struct LvEventJump {
  LvEventJumpView view;
  int hasCell; /* the jump has a place to ring, and to centre on */
  int team;    /* the team the ring is in the colour of; -1 for none */
} LvEventJump;

/* The jump for a click on a line. centre is LvEventClick.centre. highlight
 * is 1 for a highlight clip: it always has a place and names no team.
 * hasCell and team are the line's own. */
static inline LvEventJump lvEventJumpFor(int centre, int highlight, int hasCell,
                                         int team) {
  LvEventJump out;
  out.view = centre ? LV_EVENT_JUMP_CENTRE : LV_EVENT_JUMP_KEEP_VIEW;
  out.hasCell = (highlight || hasCell) ? 1 : 0;
  out.team = highlight ? -1 : team;
  return out;
}

#endif /* LV_EVENT_KIND_H */
