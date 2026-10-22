/*
 * imgui_tank.h - ImGui Tank window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_TANK_H
#define IMGUI_TANK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the Tank window: the selected tank's shells, mines, armour and
 * trees as the game's own four gauges, with its builder and the pillboxes it
 * carries. Should be called each frame; draws nothing while the window is
 * hidden. */
void lv_imgui_tank_window(void);

/* The height the window needs in a dock dockWidth points wide. For the
 * dock's built layout; needs the ImGui context. */
float lv_imgui_tank_dock_height(float dockWidth);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_TANK_H */
