/*
 * imgui_dock.h - the Log Viewer's sidebars: the docks its panels go in
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

/* A sidebar is a strip along one edge of the window, under the menu bar, that
 * the panels (Controls, Game Information, ...) can be dragged into and out
 * of. There are four, one an edge, each a Windows menu option. A new user
 * starts with the default arrangement (lv_dock_default.h): the left, right
 * and bottom ones on, with the panels in them. The left and right ones run
 * the full height under the menu bar; the top and bottom ones sit between
 * them. They only ever show in the
 * standalone viewer's desktop layout: the live spectator host, the game view
 * and the tablet / Steam Deck layout never draw them. While any shows, the
 * map keeps to the rest of the window (lv_imgui_dock_margins).
 *
 * A side is one of LV_DOCK_RIGHT, LV_DOCK_LEFT, LV_DOCK_TOP, LV_DOCK_BOTTOM
 * (lv_dock_fit.h).
 *
 * The whole layout is kept in the preferences, section LOGVIEWER:
 *   Dock.Sides        which sidebars are on and how big each is
 *   Dock.Ini          ImGui's settings text: every floating panel's place and
 *                     size, and the arrangement of each sidebar that shows
 *   Dock.Tree.<Side>  the arrangement of a sidebar that is off, for when it
 *                     comes back
 *   Dock.Enabled, Dock.Width   the right sidebar, under the names it had when
 *                     it was the only one
 * Dock.Layout, which the right-side dock used to write, is read when there
 * is no Dock.Sides and is not written any more. */

/* Load the options, the sizes and the saved layout from the preferences. Call
 * once the ImGui context exists. */
void lv_imgui_dock_init(struct LogViewerState *lv);

/* Write the options, the sizes and the layout back to the preferences. Call
 * while the ImGui context still exists. */
void lv_imgui_dock_save(void);

/* Say whether this host is the standalone viewer. lv_imgui_dock_init sets it
 * to no, so a host that says nothing (the spectator, which shares the
 * viewer's code and preferences) never shows a sidebar and never writes the
 * settings. */
void lv_imgui_dock_set_standalone(int standalone);

/* A sidebar's menu option, whether or not sidebars can show right now. */
int  lv_imgui_dock_get_side_enabled(int side);
void lv_imgui_dock_set_side_enabled(int side, int enabled);

/* The option "Make unused sidebar portions transparent" of one sidebar. With
 * it on the sidebar's panels are packed from its start, each with a length
 * of its own and a grip on its far edge, and the rest of the sidebar is
 * clear: the map shows there (lv_dock_clear.h). On for the left sidebar of
 * the default arrangement, off for the others until the user turns it on. */
int  lv_imgui_dock_get_side_clear(int side);
void lv_imgui_dock_set_side_clear(int side, int clear);

/* The right sidebar's option, for the callers from when it was the only one. */
int  lv_imgui_dock_get_enabled(void);
void lv_imgui_dock_set_enabled(int enabled);

/* Ask for the default arrangement (lv_dock_default.h) before the next frame:
 * the sidebars it has turned on at its sizes, the others off, and every
 * panel that is showing put in its place in them. Which panels show is left
 * as it is. */
void lv_imgui_dock_request_layout(void);

/* Bring each sidebar in or take it out to match its option and the mode the
 * viewer is in. Call once a frame BEFORE the ImGui frame starts and before
 * the map is drawn: the first call puts the saved layout into the context,
 * and the answer of lv_imgui_dock_margins changes here and nowhere else in
 * the frame except while the user drags a sidebar's edge. */
void lv_imgui_dock_pre_frame(void);

/* How many window points each sidebar takes along its edge, 0 for one that
 * is not showing. top is measured from under the menu bar. The map, the
 * mouse hit test and the things centred on the map all leave these alone.
 * Any pointer may be NULL. */
void lv_imgui_dock_margins(int *left, int *top, int *right, int *bottom);

/* What bounds the map as it is drawn. The same as lv_imgui_dock_margins but
 * with 0 for a sidebar whose unused part is clear: the map is drawn under
 * that one. What places the map on the screen (its corner, how many squares
 * it has, the mouse-to-square sum) uses these; what is centred on the
 * VISIBLE map still uses lv_imgui_dock_margins. Any pointer may be NULL. */
void lv_imgui_dock_map_margins(int *left, int *top, int *right, int *bottom);

/* What the solid sidebars take: the same as lv_imgui_dock_margins but with 0
 * for a sidebar whose unused part is clear. The window less these is the
 * map a double click on an event, and the first view of a log, put a place
 * in the middle of. Any pointer may be NULL. */
void lv_imgui_dock_open_margins(int *left, int *top, int *right, int *bottom);

/* True when the window point is on the map: not on the menu bar, not on a
 * solid sidebar and not on the panels of a clear one. The clear part of a
 * sidebar is map. */
int  lv_imgui_dock_point_on_map(float px, float py);

/* True when the layout has changed and has then been left alone for a
 * moment, so now is the time to write the preferences. Answers true once a
 * change. Call once a frame, after the ImGui frame. */
int  lv_imgui_dock_autosave_due(void);

/* Draw each sidebar's strip and its dock space. Call inside the ImGui frame,
 * after the menu bar and BEFORE the panels, so a panel docked in one finds
 * its place already submitted. Draws nothing while no sidebar is showing. */
void lv_imgui_dock_host(void);

/* True once, for a panel that sat in a sidebar when the sidebar was taken
 * away: the panel then goes back to its default floating place instead of
 * floating where its slot was. window_id is the panel's "###id" name. */
bool lv_imgui_dock_take_float_reset(const char *window_id);

/* True when the named window was docked the last time it drew. For the code
 * that has to know before Begin, e.g. to leave out a size constraint. */
bool lv_imgui_dock_window_is_docked(const char *window_id);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_DOCK_H */
