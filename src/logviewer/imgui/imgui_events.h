/*
 * imgui_events.h - ImGui events window for Log Viewer
 *
 * Copyright (c) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_EVENTS_H
#define IMGUI_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Render the events window
 * Should be called each frame when window is visible */
void imgui_events_window(void);

/* Initialize events state */
void imgui_events_init(void);

/* Add an event to the list
 * eventType: 0 = timestamped event, other = no timestamp
 * msg: the event message */
void imgui_events_add(int eventType, const char *msg);

/* Clear all events */
void imgui_events_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_EVENTS_H */