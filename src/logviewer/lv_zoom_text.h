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

#include <stddef.h>
#include <stdio.h>

/* The pixels of the game's art on one side of a map square. */
#define LV_ZOOM_SQUARE_ART_PX 16.0f

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
