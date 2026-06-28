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
 * Name:          imgui_winbolonet.h
 * Purpose:       ImGui WinBolo.net login popup drawn
 *                inline within the settings dialog.
 *********************************************************/

#ifndef IMGUI_WINBOLONET_H
#define IMGUI_WINBOLONET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reset popup state. Call when entering the settings dialog. */
void imguiWinbolonetReset(void);

/* Start async token validation if a token is stored. Call once on settings open. */
void imguiWinbolonetStartValidation(void);

/* Draw the WBN section within the settings panel.
 * Shows signed-in status / sign-in button and handles the login popup.
 * When inGame is true, sign-in/out is disabled (read-only status). */
void imguiWinbolonetDrawSection(bool inGame);

/* Draw the compact account status block (name + signed-in state +
 * Login/Logout + 1v1 ladder rank) for the welcome screen. Reuses the
 * same login popup/worker as imguiWinbolonetDrawSection. */
void imguiWinbolonetDrawStatusBlock(void);

/* Render the shared "My Stats" modal showing the signed-in player's
 * per-mode WinBolo.net stats. Driven by the My Stats buttons in the
 * welcome status block and the settings section; both draw paths call
 * this each frame so the single dialog state is rendered once. */
void imguiWinbolonetDrawStatsDialog(void);

/* Open the shared sign-in / create-account popup from any surface (e.g.
 * the WBN log browser's "My Games" tab). The opener must also call
 * imguiWinbolonetRenderLoginPopup() every frame for the popup to show. */
void imguiWinbolonetOpenLoginPopup(void);

/* Render the shared sign-in popup for this frame. No-op unless opened;
 * drives the async login worker. Call once per frame on the surface that
 * owns the popup. */
void imguiWinbolonetRenderLoginPopup(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_WINBOLONET_H */
