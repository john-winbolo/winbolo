/*
 * imgui_main_menu.h - ImGui main menu bar for Log Viewer
 *
 * Copyright (c) 2024
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

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_MAIN_MENU_H */