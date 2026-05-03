/*
 * imgui_game_view.h - Game-UI skin for the Log Viewer (trailer capture).
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_GAME_VIEW_H
#define IMGUI_GAME_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

struct LogViewerState;

/* Centre the viewport on the camera tank. Used on game-view enter and on
 * Tab cycle (where the camera retargets a different slot and the new tank
 * may be far outside the previous viewport). One-shot — frame-to-frame
 * tracking is the dead-zone scroll in lv_imgui_game_view_update_camera. */
void lv_imgui_game_view_init_camera(struct LogViewerState *lv);

/* Dead-zone autoscroll: if the camera tank has moved outside the inner
 * 8×8 safe zone of the 16×16 viewport, advance xOffset/yOffset by one
 * tile toward the tank. Called every frame BEFORE lv_screenUpdate so the
 * tile renderer reads the fresh offsets this frame. */
void lv_imgui_game_view_update_camera(struct LogViewerState *lv);

void lv_imgui_render_game_view(struct LogViewerState *lv);
void lv_imgui_render_game_menu_bar(struct LogViewerState *lv);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_GAME_VIEW_H */
