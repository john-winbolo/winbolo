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
 * Name:          imgui_nethint.h
 * Purpose:       First-time net-play setup hint, rendered
 *                as a BeginPopupModal INSIDE the host
 *                window (the Internet / LAN game browser)
 *                rather than as its own dialog window.
 *                Suggests setting keys + player name;
 *                "Set Keys" opens the in-game key-setup
 *                popup inside the same window.
 *
 *                Host loop responsibilities each frame:
 *                  - imguiNetHintOpen() once to trigger it
 *                  - imguiNetHintRenderPopup() every frame
 *                  - feed key-capture scancodes via the
 *                    imgui_keysetup in-game capture hooks
 *                    (the popup hosts the key-setup popup).
 *********************************************************/

#ifndef IMGUI_NETHINT_H
#define IMGUI_NETHINT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Request the hint popup to open on the next render. */
void imguiNetHintOpen(void);

/* Render the hint popup (and the nested in-game Set Keys popup) for the
 * current frame. Call from inside the host window's ImGui frame. No-op
 * until imguiNetHintOpen() has been called. */
void imguiNetHintRenderPopup(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_NETHINT_H */
