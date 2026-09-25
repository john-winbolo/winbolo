/*
 * imgui_scenario_panel.h - a scenario's panel square in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
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

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_SCENARIO_PANEL_H */
