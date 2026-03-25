/*
 * imgui_dialogs.h - ImGui modal dialogs for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This provides ImGui-based modal dialogs to replace Win32 dialogs:
 * - Team Colours dialog
 * - About dialog
 */

#ifndef IMGUI_DIALOGS_H
#define IMGUI_DIALOGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the dialogs module.
 * Loads saved dialog state from preferences.
 */
void imgui_dialogs_init(void);

/**
 * Render all open dialogs.
 * Call this every frame after ImGui newframe.
 */
void imgui_dialogs_render(void);

/**
 * Save dialog state to preferences.
 * Call this on application shutdown.
 */
void imgui_dialogs_save(void);

/**
 * Show the Team Colours dialog.
 * Allows user to assign colours to each player team.
 */
void imgui_show_team_colours_dialog(void);

/**
 * Show the About dialog.
 * Displays version and copyright information.
 */
void imgui_show_about_dialog(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_DIALOGS_H */