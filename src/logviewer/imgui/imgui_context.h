/*
 * imgui_context.h - ImGui context setup and management for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

/* Call between ImGui::Begin and ImGui::End of a panel. A floating panel
 * keeps its place by the right or bottom edge when the window is resized,
 * and is kept wholly in the window. A docked panel is left alone: the dock
 * places it, and a move here would pull it out of the dock. */
void lv_imgui_keep_window_on_screen(void);

/* Returns 1 if ImGui currently wants to capture mouse input
 * (cursor over an ImGui window/widget), 0 otherwise. Reflects the
 * state set during the last NewFrame, so it's safe to call at any
 * point during the SDL event poll. */
int lv_imgui_want_capture_mouse(void);

/* Returns 1 while a text box has the keyboard, so a key that is also a
 * shortcut is typed into the box and does nothing else. */
int lv_imgui_want_text_input(void);

/* Draw a borderless, non-interactive text label centred on the window. Call
 * between newframe and render. Used by the spectator host's pre-seed overlay
 * ("spectating begins in N" / "connecting" / "connection lost"). */
void lv_imgui_center_message(const char* text);

/* The same label, centred on a part of the window: x, y, w, h in window
 * pixels. The spectator's overview passes the part the map shows in, so the
 * label is not under a sidebar. */
void lv_imgui_center_message_at(const char* text, float x, float y, float w, float h);

/* A centred window while a log opens: title, the stage it is at (step of
 * steps) and a bar showing how far through that stage it is. */
void lv_imgui_load_progress(const char* stageText, int step, int steps, float fraction);

/* Draw a small, borderless, non-interactive phase badge ("Lobby" / "Live" /
 * "Game Over") in the top-right corner. Call between newframe and render in both
 * spectator modes (overview + game view). phase is a SPEC_PHASE_* value from
 * spectator_drain.h; SPEC_PHASE_UNKNOWN draws nothing. */
void lv_imgui_spectator_badge(int phase);

/* The same badge, in the top-right corner of a part of the window: right is
 * that part's right edge and top its top edge, in window pixels. The
 * spectator's overview passes the part the map shows in, so the badge is not
 * under a sidebar or the menu bar. */
void lv_imgui_spectator_badge_at(int phase, float right, float top);

/* Enable (non-zero) or disable (0) ImGui's gamepad UI-focus navigation. The
 * spectator overview disables it so the stick/d-pad drive map pan rather than
 * ImGui focus, and re-enables it while a modal popup is open (so the controller
 * can confirm/cancel the "Leave spectating?" prompt) and on teardown. Set
 * before newframe; ImGui reads the flag there. No-op if no context exists. */
void lv_imgui_set_gamepad_nav(int enabled);

/* Returns 1 if any ImGui popup/modal is currently open, 0 otherwise. Reads the
 * retained popup stack, so it is valid to call outside a frame. The spectator
 * host uses it to gate controller pan/zoom and the gamepad-nav toggle on the
 * "Leave spectating?" modal. */
int lv_imgui_any_popup_open(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONTEXT_H */