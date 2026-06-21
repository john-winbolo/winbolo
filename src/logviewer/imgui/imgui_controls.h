/*
 * imgui_controls.h - ImGui playback controls window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_CONTROLS_H
#define IMGUI_CONTROLS_H

struct LogViewerState;

#ifdef __cplusplus
extern "C" {
#endif

/* Render the controls window
 * Should be called each frame when window is visible */
void lv_imgui_controls_window(void);

/* Initialize controls state */
void lv_imgui_controls_init(struct LogViewerState *lv);

/* Update the speed display (called when speed changes) */
void lv_imgui_controls_update_speed(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONTROLS_H */