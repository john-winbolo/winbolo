/*
 * imgui_context.h - ImGui context setup and management for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_CONTEXT_H
#define IMGUI_CONTEXT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ImGui context and SDL3 backends
 * Returns 1 on success, 0 on failure */
int lv_imgui_context_init(SDL_Window* window, SDL_Renderer* renderer);

/* Shutdown ImGui context and cleanup resources */
void lv_imgui_context_shutdown(void);

/* Begin a new ImGui frame - call at start of each frame */
void lv_imgui_context_newframe(void);

/* Render ImGui draw data - call after all ImGui calls */
void lv_imgui_context_render(void);

/* Handle SDL event for ImGui
 * Returns 1 if ImGui consumed the event, 0 otherwise */
int lv_imgui_context_handle_event(SDL_Event* event);

/* Get the ImGui context (for advanced use) */
struct ImGuiContext* lv_imgui_context_get(void);

/* Get display size change since last frame.
 * Returns 1 if the display was resized, 0 otherwise.
 * dx/dy are set to the pixel change in width/height. */
int lv_imgui_context_get_resize_delta(float* dx, float* dy);

/* Returns 1 if ImGui currently wants to capture mouse input
 * (cursor over an ImGui window/widget), 0 otherwise. Reflects the
 * state set during the last NewFrame, so it's safe to call at any
 * point during the SDL event poll. */
int lv_imgui_want_capture_mouse(void);

/* Draw a borderless, non-interactive text label centred on the window. Call
 * between newframe and render. Used by the spectator host's pre-seed overlay
 * ("spectating begins in N" / "connecting" / "connection lost"). */
void lv_imgui_center_message(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONTEXT_H */