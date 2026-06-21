/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          imgui_gamebrowser.h
 * Purpose:       ImGui Game Browser dialog - replaces
 *                the old game finder with a table-based
 *                server browser for Internet and LAN.
 *********************************************************/

#ifndef IMGUI_GAMEBROWSER_H
#define IMGUI_GAMEBROWSER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show the ImGui game browser dialog as a blocking modal loop.
 * title      - Dialog title string
 * useTracker - TRUE to use the tracker, FALSE for LAN broadcast
 * Returns the openingStates value to transition to, or -1 on cancel. */
int imguiGameBrowserShow(const char *title, int useTracker);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_GAMEBROWSER_H */
