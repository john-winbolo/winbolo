/*
 * imgui_overview.h - ImGui Overview window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_OVERVIEW_H
#define IMGUI_OVERVIEW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the Overview window: one table with the pillboxes and the bases
 * each team owns at the playback time, the ones nobody owns and the totals.
 * Should be called each frame; draws nothing while the window is hidden. */
void lv_imgui_overview_window(void);

/* The height the window needs in a dock dockWidth points wide to show its
 * whole table. For the dock's built layout; needs the ImGui context. */
float lv_imgui_overview_dock_height(float dockWidth);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_OVERVIEW_H */
