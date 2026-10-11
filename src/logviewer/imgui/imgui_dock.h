/*
 * imgui_dock.h - the Log Viewer's right-side dock for its panels
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_DOCK_H
#define IMGUI_DOCK_H

#include <stdbool.h>

struct LogViewerState;

#ifdef __cplusplus
extern "C" {
#endif

/* The dock is a strip down the right of the window, under the menu bar, that
 * the panels (Controls, Game Information, ...) can be dragged into and out
 * of. It is a Windows menu option, off until asked for, and it only ever
 * shows in the standalone viewer's desktop layout: the live spectator host,
 * the game view and the tablet / Steam Deck layout never draw it. While it
 * shows, the map keeps to the rest of the window (lv_imgui_dock_reserved_width). */

/* Load the option, the width and the saved layout from the preferences. Call
 * once the ImGui context exists. */
void lv_imgui_dock_init(struct LogViewerState *lv);

/* Write the option, the width and the layout back to the preferences. Call
 * while the ImGui context still exists. */
void lv_imgui_dock_save(void);

/* Say whether this host is the standalone viewer. lv_imgui_dock_init sets it
 * to no, so a host that says nothing (the spectator, which shares the
 * viewer's code and preferences) never shows the dock and never writes its
 * settings. */
void lv_imgui_dock_set_standalone(int standalone);

/* The menu option, whether or not the dock can show right now. */
int  lv_imgui_dock_get_enabled(void);
void lv_imgui_dock_set_enabled(int enabled);

/* Ask for every panel that is showing to be put in the dock, one above the
 * other, the next time the dock draws. Does nothing while the option is off. */
void lv_imgui_dock_request_layout(void);

/* Bring the dock in or take it out to match the option and the mode the
 * viewer is in. Call once a frame BEFORE the ImGui frame starts and before
 * the map is drawn: it may load a layout into the context, and the answer of
 * lv_imgui_dock_reserved_width changes here and nowhere else in the frame
 * except while the user drags the dock's edge. */
void lv_imgui_dock_pre_frame(void);

/* How many window points down the right of the window the dock takes, or 0
 * while it is not showing. The map, the mouse hit test and the things centred
 * on the map all leave this much alone. */
int  lv_imgui_dock_reserved_width(void);

/* Draw the dock's strip and its dock space. Call inside the ImGui frame,
 * after the menu bar and BEFORE the panels, so a panel docked here finds its
 * place already submitted. Draws nothing while the dock is not showing. */
void lv_imgui_dock_host(void);

/* True once, for a panel that sat in the dock when the dock was taken away:
 * the panel then goes back to its default floating place instead of floating
 * where its slot in the dock was. window_id is the panel's "###id" name. */
bool lv_imgui_dock_take_float_reset(const char *window_id);

/* True when the named window was docked the last time it drew. For the code
 * that has to know before Begin, e.g. to leave out a size constraint. */
bool lv_imgui_dock_window_is_docked(const char *window_id);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_DOCK_H */
