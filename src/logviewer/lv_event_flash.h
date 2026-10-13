/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums of the ring the log viewer flashes on the map round the place of
 * an event the user went to (a click on a line of the Events panel or on a
 * marker over the seek bar).
 *
 * The ring closes in on the place: it starts LV_FLASH_RADIUS_TILES map
 * squares out from the middle of the event's square and shrinks to
 * LV_FLASH_END_TILES, LV_FLASH_PULSES times over, each time in
 * LV_FLASH_PULSE_MS of the clock on the wall: it runs the same with playback
 * stopped. Each pulse is at full strength for its first half; the first
 * then fades part of the way and the last fades to nothing.
 *
 * Also here: where the view goes to put the square in the middle of the
 * visible map area, where the arrow goes when the square is outside the view
 * and the view cannot move (it follows a tank), and whose the view is after
 * the jump.
 *
 * Header only, no SDL and no ImGui: the unit tests include it.
 */

#ifndef LV_EVENT_FLASH_H
#define LV_EVENT_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long one pulse of the ring takes, in ms of real time, and how many
   pulses there are. */
#define LV_FLASH_PULSE_MS 1000u
#define LV_FLASH_PULSES 2u
/* How long the ring is on the map, in ms of real time. */
#define LV_FLASH_MS (LV_FLASH_PULSE_MS * LV_FLASH_PULSES)
/* The part of a pulse, from its start, the ring is at full strength. */
#define LV_FLASH_HOLD 0.5f
/* How solid the ring is at the end of a pulse that is not the last. */
#define LV_FLASH_FLOOR 0.45f
/* The radius at the start of a pulse and at its end, in map squares. */
#define LV_FLASH_RADIUS_TILES 3.0f
#define LV_FLASH_END_TILES 1.0f
/* The width of the ring, in points: white alone, or a white band outside a
   band in the team's colour. A dark line LV_FLASH_DARK_PT wide is on each
   side of it. */
#define LV_FLASH_RING_PT 3.0f
#define LV_FLASH_WHITE_PT 2.0f
#define LV_FLASH_TEAM_PT 2.0f
#define LV_FLASH_DARK_PT 1.0f
#define LV_FLASH_DARK_ALPHA 0.85f
/* The arrow at the edge of the view, for a place outside it: how far its
   point is from the edge, its length and its width, in points. */
#define LV_FLASH_ARROW_INSET_PT 6.0f
#define LV_FLASH_ARROW_LEN_PT 18.0f
#define LV_FLASH_ARROW_WIDTH_PT 16.0f

/* 1 while the ring is on the map: from its start up to, not at, its end. */
static inline int lvFlashActive(uint32_t elapsedMs) {
    return elapsedMs < LV_FLASH_MS;
}

/* Which pulse the ring is in, from 0; LV_FLASH_PULSES when it is over. */
static inline uint32_t lvFlashPulse(uint32_t elapsedMs) {
    if (elapsedMs >= LV_FLASH_MS) return LV_FLASH_PULSES;
    return elapsedMs / LV_FLASH_PULSE_MS;
}

/* How far through its pulse the ring is, 0 to 1; 1 when it is over. */
static inline float lvFlashProgress(uint32_t elapsedMs) {
    if (elapsedMs >= LV_FLASH_MS) return 1.0f;
    return (float)(elapsedMs % LV_FLASH_PULSE_MS) / (float)LV_FLASH_PULSE_MS;
}

/* How solid the ring is, 1 to 0: full for the first half of a pulse, then a
   straight line down, to LV_FLASH_FLOOR in a pulse another follows and to
   nothing in the last. */
static inline float lvFlashAlpha(uint32_t elapsedMs) {
    float u, end;
    if (elapsedMs >= LV_FLASH_MS) return 0.0f;
    u = lvFlashProgress(elapsedMs);
    if (u <= LV_FLASH_HOLD) return 1.0f;
    end = (lvFlashPulse(elapsedMs) + 1u < LV_FLASH_PULSES) ? LV_FLASH_FLOOR : 0.0f;
    return 1.0f - (1.0f - end) * (u - LV_FLASH_HOLD) / (1.0f - LV_FLASH_HOLD);
}

/* The radius to the middle of the ring, in map squares. It closes in fast at
   first and slows to a stop. */
static inline float lvFlashRadiusTiles(uint32_t elapsedMs) {
    float u = lvFlashProgress(elapsedMs);
    float eased = 1.0f - (1.0f - u) * (1.0f - u);
    return LV_FLASH_RADIUS_TILES - (LV_FLASH_RADIUS_TILES - LV_FLASH_END_TILES) * eased;
}

/* Whose the view is after a jump to an event. In the game view (one
   player's own screen) it is the followed tank's, and the jump puts the
   camera back on that tank. In the overview it is the user's: the jump puts
   the event's place in the middle of it (lvFlashCentreScroll), and nothing
   may then take it to a tank. */
static inline int lvFlashCameraFollowsTank(int gameView) {
    return gameView ? 1 : 0;
}

/* The arrow for a place the view does not show. (cx, cy) is the middle of
   the event's square on the screen and the view is the box x0, y0 to x1, y1.
   Returns 0 when the middle is inside the box: no arrow, the ring is there.
   Otherwise 1, with the arrow's point in *ex, *ey (the place pulled in to
   `inset` inside the box's edge) and the way it points in *dx, *dy (length
   1, from the point toward the place). */
static inline int lvFlashEdgeArrow(float cx, float cy, float x0, float y0,
                                   float x1, float y1, float inset,
                                   float *ex, float *ey, float *dx, float *dy) {
    float px = cx, py = cy, vx, vy, len2, len, guess;
    int i;
    if (x1 <= x0 || y1 <= y0) return 0;
    if (cx >= x0 && cx < x1 && cy >= y0 && cy < y1) return 0;
    if (inset * 2.0f > x1 - x0) inset = (x1 - x0) * 0.5f;
    if (inset * 2.0f > y1 - y0) inset = (y1 - y0) * 0.5f;
    if (px < x0 + inset) px = x0 + inset;
    if (px > x1 - inset) px = x1 - inset;
    if (py < y0 + inset) py = y0 + inset;
    if (py > y1 - inset) py = y1 - inset;
    vx = cx - px;
    vy = cy - py;
    len2 = vx * vx + vy * vy;
    if (len2 <= 0.0f) {
        vx = 1.0f;
        vy = 0.0f;
        len = 1.0f;
    } else {
        /* A square root by Newton's steps: no maths library here. */
        guess = (len2 > 1.0f) ? len2 : 1.0f;
        for (i = 0; i < 24; i++) guess = 0.5f * (guess + len2 / guess);
        len = guess;
    }
    if (ex != 0) *ex = px;
    if (ey != 0) *ey = py;
    if (dx != 0) *dx = vx / len;
    if (dy != 0) *dy = vy / len;
    return 1;
}

/* Where the view goes, on one axis, to put a map square in the middle of
   the part of the window the map shows in. The answer is the map pixel (at
   zoom 1, tilePx to a square) at the view's left or top edge.

   cell: the square. viewMapPx: how many map pixels of that axis the visible
   map area shows: its length in points over the zoom. It is the area the
   sidebars leave, not the window. maxPx: the furthest the view may go (the
   map's edge); the answer is held to 0..maxPx, so near an edge of the map
   the square is as near the middle as the map allows.

   The middle of the square goes to the middle of the area, to the pixel:
   the view scrolls in pixels, not in whole squares. */
static inline int lvFlashCentreScroll(int cell, float viewMapPx, int tilePx, int maxPx) {
    float want = ((float)cell + 0.5f) * (float)tilePx - viewMapPx * 0.5f;
    int px;
    if (maxPx < 0) maxPx = 0;
    if (want <= 0.0f) return 0;
    px = (int)(want + 0.5f);
    if (px > maxPx) px = maxPx;
    return px;
}

#ifdef __cplusplus
}
#endif

#endif /* LV_EVENT_FLASH_H */
