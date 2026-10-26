/*
 * imgui_clip_gif.h - GIFs of the map and the Clips window in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Action > Make GIF... opens a small window that asks for a length. Make GIF
 * there plays the log on from the current time and reads each frame back off
 * the map's render target, as the game's replay does (clip_gif_frame.h). The
 * GIF goes into the same clips folder as the game's GIFs, through the same
 * gallery (clip_gif_gallery.h), and the Clips window opens on it. When the
 * GIF is made, the log goes back to the time it was at, and plays again if it
 * was playing.
 *
 * Windows > Clips opens and closes that gallery window. In the standalone
 * viewer and in the viewer the game opens it is the same window.
 *
 * A GIF is made only from the map (the overview), not from the game view: the
 * game view draws straight to the window and has no render target to read.
 * A live game has no GIFs either.
 */

#ifndef IMGUI_CLIP_GIF_H
#define IMGUI_CLIP_GIF_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when this build has GIFs and the Clips window at all (not the web,
 * iOS or Android). False makes every call below do nothing. */
bool lv_clip_gif_available(void);

/* True when Make GIF... can be used now: a log is open, the map (not the
 * game view) is shown, the log is not a live game and no GIF is being made. */
bool lv_clip_gif_can_make(void);

/* Opens the Make GIF window, when lv_clip_gif_can_make is true. */
void lv_clip_gif_request(void);

/* True while a GIF is being made. The viewer takes no keys and no zoom then:
 * the frames are read from a fixed part of the map. */
bool lv_clip_gif_active(void);

/* Makes this frame's share of the GIF. The host calls it once a frame, after
 * the map is drawn and before the ImGui frame begins. */
void lv_clip_gif_capture_step(void);

/* Draws the Make GIF window, the progress window and the Clips window. The
 * host calls it once a frame inside the ImGui frame, in both views. */
void lv_clip_gif_render(void);

/* Windows > Clips: opens or closes the Clips window. */
void lv_clip_gif_toggle_gallery(void);
bool lv_clip_gif_gallery_is_open(void);

/* Stops a GIF that is being made and drops it. The log is not moved back:
 * the host calls it when another log starts to open. */
void lv_clip_gif_cancel(void);

/* The viewer starts or ends: stops a GIF that is being made and closes the
 * Clips window. */
void lv_clip_gif_session_end(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CLIP_GIF_H */
