/*
 * imgui_game_viewport.h - Game background rendering and input handling
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_GAME_VIEWPORT_H
#define IMGUI_GAME_VIEWPORT_H

#include <stdbool.h>

struct LogViewerState;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the game viewport module */
void lv_imgui_game_viewport_init(struct LogViewerState *lv);

/* Render the game texture as background.
 * Call this BEFORE ImGui::NewFrame() to ensure game is behind UI. */
void lv_imgui_game_viewport_render_background(void);

/* Process mouse and keyboard input for the game.
 * Call this AFTER ImGui events are processed to check if input was not captured by UI.
 * Returns 1 if input was processed, 0 otherwise. */
int lv_imgui_game_viewport_process_input(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_GAME_VIEWPORT_H */