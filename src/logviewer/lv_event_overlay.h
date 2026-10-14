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

/* The size of a marker of the lane. It is LV_OVERLAY_MARKER_SCALE of the
 * height of a line of text, cut to a whole point, so that more markers go
 * in the lane before they are put together. With the viewer's text of 18
 * points that is 14 (it was 18). It is never under LV_OVERLAY_MARKER_MIN:
 * the builder of a "builder lost" marker is 4 pixels high and has 2 points
 * of edge on each side, so a smaller marker would cut him. */
#define LV_OVERLAY_MARKER_SCALE 0.8f
#define LV_OVERLAY_MARKER_MIN 8.0f

static inline float lvOverlayMarkerSide(float textHeight) {
  float side = (float)(int)(textHeight * LV_OVERLAY_MARKER_SCALE);
  return (side < LV_OVERLAY_MARKER_MIN) ? LV_OVERLAY_MARKER_MIN : side;
}

/* How near two events are put in one marker (the minGap of
 * lvOverlayClusters): the side of a marker, so two markers touch at the
 * nearest and never overlap. It follows the size of the marker. */
static inline float lvOverlayMergeGap(float side) {
  return side;
}

/* How far to each side of a marker's middle the mouse is on it: half its
 * side. The part that takes the mouse is as wide as the marker that is
 * drawn, and as high as the lane. */
static inline float lvOverlayHitHalf(float side) {
  return side * 0.5f;
}

/* The height of the lane: the marker, then a point of room and two points
 * for the tick of a second team under it. */
static inline float lvOverlayLaneHeight(float side) {
  return side + 3.0f;
}

/* How the lines of a marker's tooltip are set out when the marker stands for
 * several events. Every event has a line: none is left out. The lines go
 * down a column until the room is used, then start the next column. */
typedef struct LvOverlayTipColumns {
  int cols; /* how many columns */
  int rows; /* how many lines the longest column has */
  bool fits; /* false: the lines need more room than there is */
} LvOverlayTipColumns;

/* n lines, each lineH high, in columns each colW wide, in a room availW by
 * availH. A column takes as many lines as go in availH (at least one).
 * When the columns that makes are wider than availW there are only as many
 * columns as go in availW (at least one), and each takes more lines than
 * the room is high: the lines are all still there, and `fits` is false. */
static inline LvOverlayTipColumns lvOverlayTipColumns(int n, float lineH, float availH,
                                                      float colW, float availW) {
  LvOverlayTipColumns out;
  int rowsFit, maxCols;
  out.cols = 0;
  out.rows = 0;
  out.fits = true;
  if (n <= 0) return out;
  if (lineH <= 0.0f) lineH = 1.0f;
  if (colW <= 0.0f) colW = 1.0f;
  rowsFit = (int)(availH / lineH);
  maxCols = (int)(availW / colW);
  if (rowsFit < 1) {
    rowsFit = 1;
    out.fits = false;
  }
  if (maxCols < 1) {
    maxCols = 1;
    out.fits = false;
  }
  out.cols = (n + rowsFit - 1) / rowsFit;
  if (out.cols <= maxCols) {
    out.rows = (n < rowsFit) ? n : rowsFit;
    return out;
  }
  out.cols = maxCols;
  out.rows = (n + maxCols - 1) / maxCols;
  out.fits = false;
  return out;
}

/* The lines of a tooltip are drawn no smaller than this part of their own
 * size. */
#define LV_OVERLAY_TIP_SCALE_MIN 0.4f
#define LV_OVERLAY_TIP_SCALE_STEP 0.05f

/* How much smaller the lines of the tooltip are drawn so that all of them
 * go in the room: 1 when they go in at their own size in columns
 * (lvOverlayTipColumns says `fits`), else the largest step under 1 at which
 * they do, a line and a column both that much smaller. Not under
 * LV_OVERLAY_TIP_SCALE_MIN: text smaller than that cannot be read, and the
 * lines that still do not go in are then below the room, not left out. */
static inline float lvOverlayTipScale(int n, float lineH, float availH, float colW,
                                      float availW) {
  int step;
  if (n <= 0) return 1.0f;
  for (step = 0;; step++) {
    float s = 1.0f - (float)step * LV_OVERLAY_TIP_SCALE_STEP;
    if (s <= LV_OVERLAY_TIP_SCALE_MIN + 0.001f) return LV_OVERLAY_TIP_SCALE_MIN;
    if (lvOverlayTipColumns(n, lineH * s, availH, colW * s, availW).fits) return s;
  }
}

/* Where the tooltip of a marker goes: its top left corner. */
typedef struct LvOverlayTipPos {
  float x;
  float y;
} LvOverlayTipPos;

/* A tooltip w by h for a mouse at mouseX, mouseY, in a room from minX, minY
 * to maxX, maxY (the window). gap is kept between the mouse and the tooltip.
 *
 * All of the tooltip is in the room when it is no bigger than the room:
 *   - it is under the mouse and to the right of it;
 *   - with no room to the right it moves left until its right edge is in;
 *   - with no room under the mouse it goes over the mouse;
 *   - with no room there either it moves up until its bottom edge is in,
 *     and is then beside the mouse (right, else left) when it goes there,
 *     so the mouse is not on it.
 * A tooltip higher or wider than the room starts at the top or the left
 * edge: its first lines are the ones that show. */
static inline LvOverlayTipPos lvOverlayTipPlace(float mouseX, float mouseY, float w, float h,
                                                float minX, float minY, float maxX,
                                                float maxY, float gap) {
  LvOverlayTipPos p;
  if (w < 0.0f) w = 0.0f;
  if (h < 0.0f) h = 0.0f;
  if (gap < 0.0f) gap = 0.0f;
  p.x = mouseX + gap;
  if (p.x + w > maxX) p.x = maxX - w;
  if (p.x < minX) p.x = minX;
  p.y = mouseY + gap;
  if (p.y + h > maxY) {
    p.y = mouseY - gap - h;
    if (p.y < minY) {
      /* Neither under the mouse nor over it. */
      p.y = maxY - h;
      if (p.y < minY) p.y = minY;
      if (mouseX + gap + w <= maxX) {
        p.x = mouseX + gap;
      } else if (mouseX - gap - w >= minX) {
        p.x = mouseX - gap - w;
      }
    }
  }
  return p;
}

/* The column and the line of the k-th event (from 0) in columns of `rows`
 * lines: down the first column, then down the next. */
static inline void lvOverlayTipCell(int k, int rows, int *col, int *row) {
  if (rows < 1) rows = 1;
  if (k < 0) k = 0;
  if (col != NULL) *col = k / rows;
  if (row != NULL) *row = k % rows;
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

/* How the marker is drawn. */
typedef enum {
  LV_MARKER_ICON = 0,     /* the kind's drawn icon on a dark square */
  LV_MARKER_PILL,         /* the map's picture of a pillbox */
  LV_MARKER_BASE,         /* the map's picture of a base */
  LV_MARKER_BUILDER_LOST, /* the map's picture of a builder on a dark square,
                             with an X over him */
  LV_MARKER_PING          /* the game's icon for the kind of ping, in the
                             game's colour for it, on a dark square */
} LvMarkerDrawing;

/* The number of kinds of ping (PING_KIND_COUNT; lv_ping_view.h has the same
 * number and screen.c checks it against the game's). */
#define LV_MARKER_PING_KINDS 6

/* The builder of the map is 3 pixels by 4 (LGM_WIDTH, LGM_HEIGHT of
 * tiles.h). */
#define LV_MARKER_BUILDER_W 3
#define LV_MARKER_BUILDER_H 4

/* How many times his own size the builder is drawn in a square room `room`
 * on a side: the most whole times that go in, so that each of his pixels is
 * a whole block and he stays sharp; 1 at least. In the 10 points a marker of
 * 14 has inside its edge that is 2: a builder 6 wide and 8 high. */
static inline int lvMarkerBuilderScale(float room) {
  int n = (int)(room / (float)LV_MARKER_BUILDER_H);
  return (n < 1) ? 1 : n;
}

/* What the marker of one event is:
 *   drawing  how it is drawn
 *   picture  the map's picture of a pillbox or a base, or none (a drawn icon)
 *   kind     the kind the drawn icon is of (lvEventKind)
 *   team     the team whose colour the marker has: the colour of the picture
 *            (-1: the game's red pillbox, the game's neutral base), or of
 *            the line round the icon (-1: no line)
 *   ping     for LV_MARKER_PING the kind of ping whose icon it is
 *            (PING_KIND_*, 0 .. LV_MARKER_PING_KINDS - 1); -1 for any other
 *            marker */
typedef struct {
  LvMarkerDrawing drawing;
  LvOverlayPicture picture;
  int kind;
  int team;
  int ping;
} LvEventMarker;

/* The marker of an event of this kind that names this team (the new owner of
 * a pillbox or a base, the killer of a tank, the sender of a chat line; -1
 * when the event names no team). A kind out of range is a game event, as
 * the Events panel files it; a team that is not one of the 16 is no team.
 *
 * ping is the kind of ping the event is (PING_KIND_*), or -1 for an event
 * that is not a map ping. Pings are filed with the chat lines (LV_EVK_CHAT),
 * and a chat event that is a ping has the game's icon for its kind in place
 * of the speech bubble. A kind of ping this build does not know is drawn as
 * the plain ping, as the game draws it. On an event of any other kind the
 * number means nothing. */
static inline LvEventMarker lvEventMarkerFor(int kind, int team, int ping) {
  LvEventMarker m;
  if (kind < 0 || kind >= LV_EVK_COUNT) kind = LV_EVK_GAME;
  if (team < 0 || team >= LV_OVERLAY_TEAMS - 1) team = -1;
  m.picture = lvOverlayKindPicture(kind);
  m.ping = -1;
  if (m.picture == LV_OVERLAY_PIC_PILL) {
    m.drawing = LV_MARKER_PILL;
  } else if (m.picture == LV_OVERLAY_PIC_BASE) {
    m.drawing = LV_MARKER_BASE;
  } else if (kind == LV_EVK_BUILDER_LOST) {
    m.drawing = LV_MARKER_BUILDER_LOST;
  } else if (kind == LV_EVK_CHAT && ping >= 0) {
    m.drawing = LV_MARKER_PING;
    m.ping = (ping < LV_MARKER_PING_KINDS) ? ping : 0;
  } else {
    m.drawing = LV_MARKER_ICON;
  }
  m.kind = kind;
  m.team = team;
  return m;
}

/* The marker of an event that is not a map ping, and of a kind in the list
 * of kinds. */
static inline LvEventMarker lvEventMarker(int kind, int team) {
  return lvEventMarkerFor(kind, team, -1);
}

#endif /* LV_EVENT_OVERLAY_H */
