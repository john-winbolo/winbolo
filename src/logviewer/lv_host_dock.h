/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          lv_host_dock.h
 * Purpose:
 *   The part of the log viewer host that keeps the map in
 *   step with the sidebars, and the first view of a log.
 *   Both hosts with sidebars compile it: the desktop
 *   viewer (logviewer.c) and the web viewer
 *   (wasm/main_wasm.c), so the two do the same thing.
 *   The reel in the game has no sidebars and does not
 *   build it. Not for inclusion outside src/logviewer/.
 *********************************************************/

#ifndef LV_HOST_DOCK_H
#define LV_HOST_DOCK_H

#include <stdbool.h>
#include "lv_dock_fit.h"   /* LvDockMargins */

#ifdef __cplusplus
extern "C" {
#endif

/* Forget what the sidebars took when the map was last fitted. Call once
 * when the host is set up, so the first frame fits the map again. */
void lvHostDockReset(void);

/* Say that the host has just fitted the map's squares to the window with
 * dock taken by the sidebars (lv_imgui_dock_map_margins), as a resize does.
 * lvHostDockSync then refits only when the sidebars change again. */
void lvHostDockNoteFitted(const LvDockMargins *dock);

/* Once a frame, before the map is drawn: let each sidebar come in or go
 * out, and refit the map's squares when what the sidebars take changed. */
void lvHostDockSync(void);

/* The part of the map's view no solid sidebar hides, each way: where it
 * starts, in points from the view's own start on the screen, and how long
 * it is. */
void lvHostVisibleMap(float *startX, float *startY, float *lenX, float *lenY);

/* The part of the window the map shows in, in window pixels: below the menu
 * bar and inside the sidebars that are open. */
void lvHostMapArea(float *x, float *y, float *w, float *h);

/* Say whether this host places the first view of a log (lvHostFirstView).
 * The viewer, the live spectator and the web viewer do. */
void lvHostFirstViewSetHost(bool on);

/* Once a frame, after lvHostDockSync: put the map where the first look at a
 * newly loaded log is worth it, once for each load. */
void lvHostFirstView(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_HOST_DOCK_H */
