/*
 * imgui_event_overlay.h - event markers over the Log Viewer's seek slider
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * "Overlay events" of the Controls panel. The user ticks kinds of event in
 * a list; each event of a ticked kind then has a small marker in a lane
 * just above the seek slider, at the place the slider's handle has when
 * playback is at that event. The mouse over a marker says what happened; a
 * click goes there.
 *
 * The markers show the lines of the Events panel (imgui_events.h) and keep
 * no list of their own: there is one list of events, and one set of kinds
 * with one set of names.
 *
 * The Controls panel calls the two draw functions; nothing else does.
 *
 * The list of kinds itself (lv_event_kinds_combo) is also the filter of the
 * Events panel: one function draws both, each with its own choice.
 */

#ifndef LV_IMGUI_EVENT_OVERLAY_H
#define LV_IMGUI_EVENT_OVERLAY_H

#include "imgui.h"

/* Read the chosen kinds from the preferences. Call once at start-up. */
void lv_event_overlay_init(void);

/* The "Overlay events" list, as the next item of the line. With full it is
 * a combo that shows its name; without, only the arrow of one, with the
 * name as its tooltip, for a line with no room. */
void lv_event_overlay_combo(bool full);

/* The width lv_event_overlay_combo(true) takes, and that of
 * lv_event_overlay_combo(false). */
float lv_event_overlay_combo_width(bool full);

/* The list of the ten kinds of event with a check box each: the one control
 * for "tick some kinds" in the viewer. It is a combo, as wide as the item
 * width set for it (ImGui::SetNextItemWidth). preview is the text on it
 * while it is closed, NULL for the arrow alone. Open, it has an "All" and a
 * "None" button and then a row a kind, in the order of lvEventKind: the
 * marker the kind has in the "Overlay events" lane, its name, and how many
 * events of it the log has (counts, LV_EVK_COUNT entries). The list stays
 * open while kinds are ticked. tip is the tooltip of the closed combo (NULL
 * for none). mask has one bit a kind; returns true when it was changed. */
bool lv_event_kinds_combo(const char *id, const char *preview, const char *tip,
                          unsigned *mask, const int *counts);

/* The marker of one event, in the square p0..p1: the one function that
 * draws it, for the lane, for the list of kinds and for the lines of the
 * Events panel, so the three always show the same thing. kind is the
 * event's kind (lvEventKind) and team the team the event names (the new
 * owner of a pillbox or a base, the killer of a tank, the sender of a chat
 * line), -1 for none; lvEventMarker says what that makes the marker. A
 * pillbox event or a base event is the map's own picture in the colour of
 * that team (no team: the game's red pillbox, the game's neutral base). Any
 * other kind is its icon on a dark square, with a line in the team's colour
 * round it when there is a team. The picture is made larger or smaller as
 * lv_item_picture does it. alpha dims the marker. It creates no texture. */
void lv_event_marker_draw(ImDrawList *dl, int kind, int team, ImVec2 p0, ImVec2 p1,
                          float alpha);

/* The lane of markers. Call it just before the seek slider, with the item
 * width of the slider already pushed: the lane takes that width, so its
 * markers are over the slider's own places. It draws nothing, and takes no
 * room, while no kind is ticked. */
void lv_event_overlay_lane(void);

/* The height lv_event_overlay_lane adds to the panel, with the gap under
 * it; 0 while no kind is ticked. */
float lv_event_overlay_lane_height(void);

#endif /* LV_IMGUI_EVENT_OVERLAY_H */
