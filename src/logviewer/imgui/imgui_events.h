/*
 * imgui_events.h - ImGui events window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
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
 * msg: the event message */
void lv_imgui_events_add(int eventType, const char *msg);

/* Clear all events */
void lv_imgui_events_clear(void);

/* Remove events with timestamp strictly after the given time (ms) */
void lv_imgui_events_remove_after(unsigned int timeMs);

/* Game-view newswire accessors. Lifetimes are valid only for the
 * current frame; callers must not retain the returned pointer. */
int          lv_imgui_events_get_count(void);
const char  *lv_imgui_events_get_text(int i);
uint32_t     lv_imgui_events_get_time(int i);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_EVENTS_H */