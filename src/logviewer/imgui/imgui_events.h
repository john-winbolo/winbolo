/*
 * imgui_events.h - ImGui events window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_EVENTS_H
#define IMGUI_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the events window
 * Should be called each frame when window is visible */
void lv_imgui_events_window(void);

/* Initialize events state */
void lv_imgui_events_init(void);

/* Add an event to the list
 * eventType: 0 = timestamped event, other = no timestamp
 * kind: what the line reports (lvEventKind, lv_event_kind.h), for the filter
 * mapX, mapY: the map cell the line is about, or both -1 for none; a click
 *             on the line centres the view there after it seeks
 * msg: the event message
 * Does nothing once the list is complete (see below). */
void lv_imgui_events_add(int eventType, int kind, int mapX, int mapY,
                         const char *msg);

/* Add a clickable highlight line (no timestamp prefix; msg carries its own
 * absolute time). Clicking it seeks a few seconds before seekMs and centres the
 * game view on (mapX,mapY). */
void lv_imgui_events_add_highlight(const char *msg, uint32_t seekMs, int mapX,
                                   int mapY);

/* Add a load-time round-summary line (awards, section headers): no timestamp,
 * and pinned so the presentation window never hides it. */
void lv_imgui_events_add_summary(const char *msg);

/* Clear all events */
void lv_imgui_events_clear(void);

/* Remove events with timestamp strictly after the given time (ms). Does
 * nothing once the list is complete. */
void lv_imgui_events_remove_after(unsigned int timeMs);

/* Tell the panel it now holds every event of the log. A host that has read
 * the whole file ahead (lv_windowCollectAllEvents) calls this once the lines
 * are in. From then until the next lv_imgui_events_clear the list is fixed:
 * add and remove_after do nothing, the lines after the playhead are drawn
 * dim, and a "now" divider marks the playhead. A host that never calls it (a
 * live feed, the web viewer) keeps the list that grows with playback. */
void lv_imgui_events_set_complete(void);

/* Game-view newswire accessors. Lifetimes are valid only for the
 * current frame; callers must not retain the returned pointer. */
int          lv_imgui_events_get_count(void);
const char  *lv_imgui_events_get_text(int i);
uint32_t     lv_imgui_events_get_time(int i);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_EVENTS_H */