/*
 * imgui_bothosts.h - ImGui Bot Hosts window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_BOTHOSTS_H
#define IMGUI_BOTHOSTS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the Bot Hosts window: for each host of the server's bot placement
 * (the server's own process and each Bot Host), one graph over the replay's
 * time of the recording's log_BotHostData records (lv_bothost.h), with the
 * play head. Should be called each frame; draws nothing while the window is
 * hidden. */
void lv_imgui_bothosts_window(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_BOTHOSTS_H */
