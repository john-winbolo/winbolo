/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_controller_disconnect.h
 * Purpose:       "Controller Disconnected" dialog.  Shown
 *                when the active gamepad drops while the
 *                game is in controller mode.  Offers a
 *                switch to keyboard/mouse; auto-dismisses
 *                when a controller is reconnected.  The
 *                open/close lifecycle and any solo-game
 *                pause are driven by the caller (sdl3imgui);
 *                this module only renders.
 *********************************************************/

#ifndef IMGUI_CONTROLLER_DISCONNECT_H
#define IMGUI_CONTROLLER_DISCONNECT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void controllerDisconnectOpen(void);   /* request open */
void controllerDisconnectClose(void);  /* force close (e.g. on reconnect) */
bool controllerDisconnectIsOpen(void);

/* Render the modal.  Call from the main ImGui render path every frame.
 * Returns true on the frame the "Continue with keyboard and mouse" button
 * is pressed — the dialog has already switched controller mode off and
 * closed itself; the caller should unpause any solo game. */
bool controllerDisconnectRender(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONTROLLER_DISCONNECT_H */
