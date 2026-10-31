/*
 * imgui_clip_gif.h - GIFs of the map and the Clips window in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Recording mode, as the game's replay has it (lobby_reel.cpp): the movie
 * camera button on the Controls panel, Action > Recording Mode and Ctrl+G
 * turn it on and off. In recording mode the Controls panel shows the GIF
 * controls under its top row: Preview clip, Save clip, the length in
 * seconds and the Clips button. The timeline shows where the GIF starts
 * and ends, with a marker at the end that can be dragged to change the
 * length. A frame over the map always shows the part of the map the GIF
 * takes; its edges, corners and inside can be dragged. The
 * controls come from clip_gif_controls.h, the same ones the game draws.
 *
 * Preview clip plays the log from the current time for the length, at the
 * normal speed, and makes no file. At the end of the clip the log stops and
 * goes back to the start of the clip, so Save clip then makes the GIF of the
 * same clip. Pressed again, it stops the preview. A pause or a seek ends the
 * preview where the log is.
 *
 * Save clip plays the log on from the current time (or from the start of the
 * preview that plays) and reads each frame back off the map's render target,
 * as the game's replay does. The GIF goes into the same clips folder as the
 * game's GIFs, through the same gallery (clip_gif_gallery.h), and the Clips
 * window opens on it. When the GIF is made, the log goes back to the time it
 * was at, and plays again if it was playing. Esc or Cancel stops it. The
 * preview, the capture and the progress window are the game's own code
 * (clip_gif_capture.h).
 *
 * Recording mode, the crop frame and the length go back to their start
 * values with each log that opens, as the game's do with each replay.
 *
 * Windows > Clips opens and closes that gallery window. In the standalone
 * viewer and in the viewer the game opens it is the same window.
 *
 * A GIF is made only from the map (the overview), not from the game view: the
 * game view draws straight to the window and has no render target to read.
 * A live game has no recording mode either.
 */

#ifndef IMGUI_CLIP_GIF_H
#define IMGUI_CLIP_GIF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when this build has GIFs and the Clips window at all (not the web,
 * iOS or Android). False makes every call below do nothing. */
bool lv_clip_gif_available(void);

/* True when recording mode can be turned on or off now: a log is open, the
 * log is not a live game and no GIF is being made. */
bool lv_clip_gif_can_toggle_mode(void);

/* True while recording mode is on. */
bool lv_clip_gif_mode_on(void);

/* Turns recording mode on or off, when lv_clip_gif_can_toggle_mode is
 * true. */
void lv_clip_gif_toggle_mode(void);

/* True when the GIF controls show this frame: a log that is not live is
 * open, and recording mode is on or a GIF is being made (clipGifToolsShown).
 * The controls of a GIF being made stay until it ends. */
bool lv_clip_gif_tools_shown(void);

/* True when Preview clip and Save clip can be used now: the same as
 * lv_clip_gif_can_toggle_mode, and the map (not the game view) is shown. */
bool lv_clip_gif_can_make(void);

/* True while a GIF is being made. The viewer takes no keys and no zoom then:
 * the frames are read from a fixed part of the map. */
bool lv_clip_gif_active(void);

/* Makes this frame's share of the GIF. The host calls it once a frame, after
 * the map is drawn and before the ImGui frame begins. */
void lv_clip_gif_capture_step(void);

/* Draws the crop frame over the map, the progress window and the Clips
 * window. The host calls it once a frame inside the ImGui frame, in both
 * views. */
void lv_clip_gif_render(void);

/* True when the crop frame has the mouse now: the mouse is on the frame
 * and no ImGui window is under it, or the frame is being dragged. The map
 * then does not pan, pick a tank or show a tooltip for that mouse. */
bool lv_clip_gif_frame_has_mouse(void);

/* The GIF controls in the Controls panel, on a line of their own in the
 * middle of the panel: Preview clip, Save clip, Cancel, the length in
 * seconds, the Clips button (clipGifToolsRow). The panel calls it when
 * lv_clip_gif_tools_shown is true. */
void lv_clip_gif_tools_row(void);

/* The zoomed timeline (clipGifZoomRow), on a line of its own as wide as the
 * panel, over the main timeline: the GIF's span with its two handles, and a
 * press on it moves the log. Nothing unless lv_clip_gif_tools_shown. */
void lv_clip_gif_zoom_row(void);

/* The GIF on the main timeline (clipGifSpanTint). While the slider is
 * pressed or dragged (seeking), the span goes round seekMs, the slider's
 * time (clipGifSpanCentre). minX..maxY is the frame of the seek slider,
 * which maps 0..totalMs; the panel calls it after the slider. */
void lv_clip_gif_seek_marker(float minX, float minY, float maxX, float maxY,
                             bool seeking, uint32_t seekMs, uint32_t totalMs);

/* Windows > Clips: opens or closes the Clips window. */
void lv_clip_gif_toggle_gallery(void);
bool lv_clip_gif_gallery_is_open(void);

/* Stops a GIF that is being made and drops it, and puts recording mode back
 * to its start values. The log is not moved back: the host calls it when
 * another log starts to open. */
void lv_clip_gif_cancel(void);

/* The viewer starts or ends: stops a GIF that is being made and closes the
 * Clips window. */
void lv_clip_gif_session_end(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CLIP_GIF_H */
