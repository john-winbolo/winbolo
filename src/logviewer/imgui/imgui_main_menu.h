/*
 * imgui_main_menu.h - ImGui main menu bar for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_MAIN_MENU_H
#define IMGUI_MAIN_MENU_H

#include <stdbool.h>

struct LogViewerState;

#ifdef __cplusplus
extern "C" {
#endif

/* Window visibility flags - accessible from other modules */
extern bool lv_g_show_controls_window;
extern bool lv_g_show_game_info_window;
extern bool lv_g_show_events_window;
extern bool lv_g_show_item_info_window;
extern bool lv_g_show_comments_window;
extern bool lv_g_reset_window_positions;

/* Render the main menu bar
 * Returns true if any menu item was clicked */
int lv_imgui_main_menu_bar(void);

/* Initialize menu state from preferences */
void lv_imgui_main_menu_init(struct LogViewerState *lv);

/* Save menu state to preferences */
void lv_imgui_main_menu_save(void);

/* Get the height of the main menu bar in pixels.
 * Returns 0.0f if menu bar hasn't been rendered yet. */
float lv_imgui_get_menu_bar_height(void);

/* Cache the menu bar height after rendering.
 * Call this after lv_imgui_main_menu_bar() to ensure accurate height. */
void lv_imgui_cache_menu_bar_height(void);

/* Get the current mode state.
 * Returns true if Information mode, false if Select Team mode. */
int lv_imgui_get_mode_information(void);

/* Mutators / accessors used by the macOS native menu shim. The in-window
 * ImGui menu owns this state today; the native menu needs to read and
 * flip the same flags so checkmarks and behaviour stay aligned. */
void lv_imgui_set_mode_information(int isInformation);
int  lv_imgui_get_tank_centred(void);
void lv_imgui_toggle_tank_centred(void);
int  lv_imgui_get_dns_lookups(void);
void lv_imgui_toggle_dns_lookups(void);

/* Wrappers around static helpers in imgui_main_menu.cpp that the macOS
 * shim needs to invoke. zoom_at_center centres the zoom on the current
 * window. lv_imgui_open_wbn_browser blocks until the modal closes;
 * reused by the compact-mode popup menu so it shares the desktop File
 * menu flow. */
void lv_imgui_zoom_at_center(int stepIndex);
void lv_imgui_open_wbn_browser(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_MAIN_MENU_H */