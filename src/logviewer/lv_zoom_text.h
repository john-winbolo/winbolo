/*
 * lv_zoom_text.h - the zoom of the Log Viewer's map as a percentage
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The viewer says how large the map is drawn as a percentage: on the
 * Controls panel, in the Zoom menu, and for a moment on the map when the
 * zoom changes. This header has the sums with no ImGui and no SDL in them,
 * so the unit tests can run them.
 *
 * 100% is the game's own size: one map square, which is 16 pixels of the
 * game's art, on 16 points of the window. A point is one pixel of an
 * ordinary display and more than one of a dense one (2 on a "200%" or
 * Retina display), so 100% is the same size to the eye on both: 16 pixels
 * there, 32 here.
 */

#ifndef LV_ZOOM_TEXT_H
#define LV_ZOOM_TEXT_H

#include <math.h>
#include <stddef.h>
#include <stdio.h>

/* The pixels of the game's art on one side of a map square. */
#define LV_ZOOM_SQUARE_ART_PX 16.0f

/* The zoom steps of the map, smallest first (g_zoomSteps in draw.c). The
 * big steps let one pixel of the art be seen: at 16, one pixel of the art is
 * 16 points and one map square is 256 points. */
#define LV_ZOOM_STEP_LIST   0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f

/* The largest zoom at which a resize of a window snaps it to whole squares
 * (lvHostHandleResize). Above it one square is more than 64 points, so the
 * window is left the size the user made it, and the map covers it. */
#define LV_ZOOM_SNAP_MAX 4.0f

/* The view's new start after a zoom step, in pixels of the art from the
 * map's left (or top) edge, so the point under the cursor stays under it.
 * anchorSquares is the place under the cursor in map squares (with the part
 * of a square), cursorPx the cursor's distance from the map's edge in
 * points, and squarePx one square at the new zoom in points. The part of a
 * square is kept: rounding the start to a whole square would move the
 * point by up to half a square (128 points at 16). */
static inline int lvZoomStartArtPx(float anchorSquares, float cursorPx, float squarePx) {
  float start;
  if (squarePx <= 0.0f) squarePx = LV_ZOOM_SQUARE_ART_PX;
  start = anchorSquares - cursorPx / squarePx;
  return (int)floorf(start * LV_ZOOM_SQUARE_ART_PX + 0.5f);
}

/* One frame's pan step, in whole pixels of the art, for a wanted step of
 * want art pixels. A big zoom makes the wanted step less than one pixel
 * (0.75 at 16 for the controller), so the part that is cut off is kept in
 * *carry and added to the next frame's step. Set *carry to 0 when the pan
 * stops. */
static inline int lvZoomPanStep(float want, float *carry) {
  float step = want + *carry;
  int whole = (int)step;
  *carry = step - (float)whole;
  return whole;
}

/* How long the label on the map stays after a change of the zoom, and how
 * much of that, at the end, it takes to fade out; seconds. */
#define LV_ZOOM_LABEL_SHOW_S 1.5f
#define LV_ZOOM_LABEL_FADE_S 0.5f

/* The zoom as a percentage. squarePx is the side of one map square as it is
 * drawn, in pixels of the display; displayScale is how many pixels of the
 * display one point of the window is (1 on an ordinary display). */
static inline float lvZoomPercent(float squarePx, float displayScale) {
  if (displayScale <= 0.0f) displayScale = 1.0f;
  if (squarePx <= 0.0f) return 0.0f;
  return squarePx / (LV_ZOOM_SQUARE_ART_PX * displayScale) * 100.0f;
}

/* The percentage as text: "150%". A whole number, but for a value under 100
 * that is not whole, which has one decimal ("62.5%"): the steps under 100
 * are close to each other, and a size fitted to a window is seldom whole. */
static inline void lvZoomPercentText(float percent, char *out, size_t outSize) {
  long tenths;
  if (out == NULL || outSize == 0) return;
  if (percent < 0.0f) percent = 0.0f;
  tenths = (long)(percent * 10.0f + 0.5f);
  if (tenths < 1000 && (tenths % 10) != 0) {
    snprintf(out, outSize, "%ld.%ld%%", tenths / 10, tenths % 10);
  } else {
    snprintf(out, outSize, "%ld%%", (long)(percent + 0.5f));
  }
}

/* How strong the label on the map is, `seconds` after the zoom last
 * changed: 1 at first, then down to 0 over the last LV_ZOOM_LABEL_FADE_S of
 * LV_ZOOM_LABEL_SHOW_S; 0 after that, and for a time before the change. */
static inline float lvZoomLabelAlpha(float seconds) {
  if (seconds < 0.0f || seconds >= LV_ZOOM_LABEL_SHOW_S) return 0.0f;
  if (seconds <= LV_ZOOM_LABEL_SHOW_S - LV_ZOOM_LABEL_FADE_S) return 1.0f;
  return (LV_ZOOM_LABEL_SHOW_S - seconds) / LV_ZOOM_LABEL_FADE_S;
}

#endif /* LV_ZOOM_TEXT_H */
