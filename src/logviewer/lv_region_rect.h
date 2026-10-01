/*
 * lv_region_rect.h - where a declared region lands on one of the viewer's maps
 *
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The overview and the game view place a map square differently: the overview
 * draws the viewport's first square at its target's origin, the game view one
 * square further in and panned by a sub-square offset. Both reduce to where
 * square (0, 0) would sit and how big a square is, which is what this takes,
 * so the one sum serves both and a unit test can hold it.
 *
 * Plain C with no viewer headers, so game_view.c (bolo types) and draw.c
 * (viewer types) can both include it.
 */

#ifndef LV_REGION_RECT_H
#define LV_REGION_RECT_H

#include <stdbool.h>
#include <stddef.h>

/* One view's placement of the map. mapX, mapY is where the top-left corner of
   map square (0, 0) would be drawn, pan included, even when that is far off
   the view. viewX, viewY, viewW, viewH is the part of the screen the view
   shows. */
typedef struct {
  float mapX, mapY;
  float tileW, tileH;
  float viewX, viewY, viewW, viewH;
} LvRegionView;

typedef struct {
  float x, y, w, h;
} LvRegionRect;

/* How both views draw a region: one colour for every region, a magenta no
   Bolo terrain uses, so an outline reads over grass, water, road and forest
   alike; the outline's thickness as a fraction of a map square, drawn inside
   the region's own squares; and how dark the box under a name is. */
#define LV_REGION_COLOUR_R   255
#define LV_REGION_COLOUR_G   64
#define LV_REGION_COLOUR_B   255
#define LV_REGION_STROKE     0.125f
#define LV_REGION_LABEL_BACK 160

/* The screen rectangle of the region at map square (x, y), w by h squares,
   into *out, whole and unclipped. Returns whether any of it lands on the
   view; an empty region never does. */
static inline bool lvRegionScreenRect(const LvRegionView *view,
                                      int x, int y, int w, int h,
                                      LvRegionRect *out) {
  if (view == NULL || out == NULL || w <= 0 || h <= 0) return false;
  out->x = view->mapX + (float)x * view->tileW;
  out->y = view->mapY + (float)y * view->tileH;
  out->w = (float)w * view->tileW;
  out->h = (float)h * view->tileH;
  return out->x < view->viewX + view->viewW &&
         out->x + out->w > view->viewX &&
         out->y < view->viewY + view->viewH &&
         out->y + out->h > view->viewY;
}

#endif /* LV_REGION_RECT_H */
