/*
 * imgui_hud.h - ImGui HUD window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_HUD_H
#define IMGUI_HUD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the HUD window: the game's three status grids (tanks, pillboxes,
 * bases), with each owned thing in the colour of the team that owns it.
 * Should be called each frame; draws nothing while the window is hidden. */
void lv_imgui_hud_window(void);

/* The height the window needs in a dock dockWidth points wide to show its
 * grids at scale 1: one row of three when they fit across, else one above
 * the other. For the dock's built layout; needs the ImGui context. */
float lv_imgui_hud_dock_height(float dockWidth);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_HUD_H */
