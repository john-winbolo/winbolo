/*
 * imgui_scenario_panel.h - a scenario's panel square in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_SCENARIO_PANEL_H
#define IMGUI_SCENARIO_PANEL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draw the panel square the followed player saw at the playhead, in its own
 * window. Draws nothing while no log is loaded, while the window is closed
 * (lv_g_show_scenario_panel_window) or while there is no list to show, which
 * is every frame of a plain recording. */
void lv_imgui_scenario_panel_window(bool logLoaded);

/* The smallest side the window's square may have, in screen pixels: one pixel
 * a panel unit, the size it had before it could be resized. The largest is
 * whatever fits the shorter side of the main viewport. */
#define LV_SCN_PANEL_SIDE_MIN 128

/* The side of the square in screen pixels, as the window last drew it or as
 * the preference set it. The setter raises a value under
 * LV_SCN_PANEL_SIDE_MIN to it; the window keeps it inside the viewport. */
int  lv_imgui_scenario_panel_side(void);
void lv_imgui_scenario_panel_set_side(int side);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_SCENARIO_PANEL_H */
