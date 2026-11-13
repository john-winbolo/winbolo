/*
 * imgui_chat.h - ImGui Chat window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_CHAT_H
#define IMGUI_CHAT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the Chat window: the last chat lines at the playback time, oldest
 * at the top, each sender in the colour of the sender's team. The lines
 * fade out when no new one has come for a while (lv_chat_view.h). Should be
 * called each frame; draws nothing while the window is hidden. */
void lv_imgui_chat_window(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CHAT_H */
