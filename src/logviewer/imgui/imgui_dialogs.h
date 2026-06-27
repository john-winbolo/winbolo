/*
 * imgui_dialogs.h - ImGui modal dialogs for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
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

struct LogViewerState;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the dialogs module.
 * Loads saved dialog state from preferences.
 */
void lv_imgui_dialogs_init(struct LogViewerState *lv);

/**
 * Render all open dialogs.
 * Call this every frame after ImGui newframe.
 */
void lv_imgui_dialogs_render(void);

/**
 * Save dialog state to preferences.
 * Call this on application shutdown.
 */
void lv_imgui_dialogs_save(void);

/**
 * Show the Team Colours dialog.
 * Allows user to assign colours to each player team.
 */
void lv_imgui_show_team_colours_dialog(void);

/**
 * Show the About dialog.
 * Displays version and copyright information.
 */
void lv_imgui_show_about_dialog(void);

/**
 * Arm the live-spectator "Leave spectating?" confirm modal.
 * Idempotent; rendered by lv_imgui_dialogs_render() in both spectator
 * modes. Plain entry point so a controller B-button can arm it too.
 */
void lv_imgui_spectator_leave_request(void);

/**
 * Returns true once after the user confirms the leave modal (Yes), then
 * clears. Returns false while the modal is open, on cancel (No / Esc /
 * dismiss), or when no modal is armed.
 */
bool lv_imgui_spectator_leave_confirmed(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_DIALOGS_H */