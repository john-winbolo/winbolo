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

/* The banner over the map while the lag view ("see what this player saw")
 * is on: whose screen the map shows, how far behind the present it is, and
 * the button that turns the view off. x, y and w are the map's left, top
 * and width in the main viewport. Draws nothing while the view is off;
 * needs the ImGui context. */
void lv_imgui_lag_banner_at(float x, float y, float w);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_TANK_H */
