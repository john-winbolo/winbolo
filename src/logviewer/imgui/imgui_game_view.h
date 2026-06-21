/*
 * imgui_game_view.h - Game-UI skin for the Log Viewer (trailer capture).
 *
 * Copyright (c) 1998-2026 John Morrison.
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

/* Centre the viewport on the camera tank at pixel precision. Used on
 * game-view enter and on Tab cycle (where the camera retargets a
 * different slot and the new tank may be far outside the previous
 * viewport). One-shot snap — frame-to-frame tracking is the eased
 * dead-zone follower in lv_imgui_game_view_update_camera. */
void lv_imgui_game_view_init_camera(struct LogViewerState *lv);

/* Pixel-precise dead-zone autoscroll. If the camera tank has moved
 * outside the inner 128×128 px safe zone of the 256×256 px viewport,
 * ease the camera (xOffset+subPxX, yOffset+subPxY) toward a target
 * that puts the tank just inside the safe-zone boundary. Called every
 * frame BEFORE lv_screenUpdate so the tile renderer reads the fresh
 * offsets this frame. */
void lv_imgui_game_view_update_camera(struct LogViewerState *lv);

void lv_imgui_render_game_view(struct LogViewerState *lv);
void lv_imgui_render_game_menu_bar(struct LogViewerState *lv);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_GAME_VIEW_H */
