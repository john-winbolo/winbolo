/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * imgui_ios.h — iOS ImGui settings overlay for WinBolo.
 * Provides a gear-icon button that opens a touch-friendly
 * settings panel (zoom, sound, FPS).
 */

#ifndef IMGUI_IOS_H
#define IMGUI_IOS_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise ImGui with SDL3 + SDL_Renderer backends.
   Call after SDL_CreateWindow / SDL_CreateRenderer. */
bool imguiIosSetup(SDL_Window *window, SDL_Renderer *renderer);

/* Forward an SDL event to ImGui. Returns true if ImGui
   consumed the event (overlay is active — suppress game input). */
bool imguiIosProcessEvent(SDL_Event *event);

/* Build and render the ImGui frame. Call each frame
   before SDL_RenderPresent. */
void imguiIosRender(void);

/* Returns true when ImGui wants mouse/touch input
   (settings panel is open). Game input should be suppressed. */
bool imguiIosWantsInput(void);

/* Returns a pending zoom value (1/2/4) if the user changed
   zoom in settings, or 0 if no change. Resets after reading. */
unsigned char imguiIosConsumePendingZoom(void);

/* Shutdown ImGui backends and destroy context. */
void imguiIosCleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_IOS_H */
