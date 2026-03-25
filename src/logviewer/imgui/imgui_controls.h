/*
 * imgui_controls.h - ImGui playback controls window for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_CONTROLS_H
#define IMGUI_CONTROLS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Render the controls window
 * Should be called each frame when window is visible */
void imgui_controls_window(void);

/* Initialize controls state */
void imgui_controls_init(void);

/* Update the speed display (called when speed changes) */
void imgui_controls_update_speed(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONTROLS_H */