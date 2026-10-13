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
 */

#ifndef LV_IMGUI_EVENT_OVERLAY_H
#define LV_IMGUI_EVENT_OVERLAY_H

/* Read the chosen kinds from the preferences. Call once at start-up. */
void lv_event_overlay_init(void);

/* The "Overlay events" list, as the next item of the line. With full it is
 * a combo that shows its name; without, only the arrow of one, with the
 * name as its tooltip, for a line with no room. */
void lv_event_overlay_combo(bool full);

/* The width lv_event_overlay_combo(true) takes, and that of
 * lv_event_overlay_combo(false). */
float lv_event_overlay_combo_width(bool full);

/* The lane of markers. Call it just before the seek slider, with the item
 * width of the slider already pushed: the lane takes that width, so its
 * markers are over the slider's own places. It draws nothing, and takes no
 * room, while no kind is ticked. */
void lv_event_overlay_lane(void);

/* The height lv_event_overlay_lane adds to the panel, with the gap under
 * it; 0 while no kind is ticked. */
float lv_event_overlay_lane_height(void);

#endif /* LV_IMGUI_EVENT_OVERLAY_H */
