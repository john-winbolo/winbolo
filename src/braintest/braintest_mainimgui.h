/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Main-window ImGui context for BrainTest.
 *
 * Sub-windows (V dialog, panel/pool, per-bot panels) each
 * own their OWN ImGui context bound to their OWN SDL window
 * — that pattern stays. This module adds a fourth context
 * bound to the MAIN map window so we can render generic
 * floating panels (shot sim, future tools) on top of the
 * map without needing a separate SDL window per tool.
 *
 * Lifecycle:
 *   mainImGuiInit(window, renderer)            once at startup
 *   mainImGuiProcessEvent(ev)                  per SDL event
 *   mainImGuiBeginFrame() / mainImGuiEndFrame()
 *                                              wrap each render
 *   mainImGuiShutdown()                        once at shutdown
 *
 * Between BeginFrame + EndFrame the main ImGui context is
 * the active context, so any ImGui:: calls land in this
 * window's draw list.
 *********************************************************/

#ifndef BRAINTEST_MAINIMGUI_H
#define BRAINTEST_MAINIMGUI_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void mainImGuiInit(SDL_Window *window, SDL_Renderer *renderer);
void mainImGuiShutdown(void);
void mainImGuiProcessEvent(SDL_Event *ev);
void mainImGuiBeginFrame(void);
void mainImGuiEndFrame(SDL_Renderer *renderer);

/* True when ImGui currently has the mouse — used by the host's
 * left-click handler to suppress map clicks that landed on a panel. */
bool mainImGuiWantsMouse(void);

/* True when ImGui currently has a text-input field active — used by
 * the host's keydown handler to skip BrainTest hotkeys while the
 * user is typing into a panel filter / search box / etc. */
bool mainImGuiWantsTextInput(void);

/* Floating ImGui window listing every BrainTest keyboard shortcut.
 * Toggled by the host (F1). *visible flips false when the user
 * closes the window via its [X]. Must be called between
 * mainImGuiBeginFrame and mainImGuiEndFrame. */
void mainImGuiRenderShortcuts(bool *visible);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_MAINIMGUI_H */
