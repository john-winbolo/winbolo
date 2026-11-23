/*
 * imgui_game_viewport.h - Game background rendering and input handling
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

/* The tooltip of the pillbox under the mouse on the overview's map: its
 * number, its owner and its armour. Call it in the ImGui frame, before the
 * frame is drawn. */
void lv_imgui_game_viewport_hover(void);

/* True while a press that began on the map is held: the map has the mouse
 * for a pan or a click. The crop frame of recording mode does not take that
 * mouse over (imgui_clip_gif.h). */
bool lv_imgui_game_viewport_mouse_held(void);

/* The lines drawn on the overview's map with Shift and the left button,
 * each fading out a moment after the button comes up (lv_marks.h). Call it
 * in the ImGui frame, before the frame is drawn. */
void lv_imgui_game_viewport_draw_marks(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_GAME_VIEWPORT_H */