/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_controller_prompt.cpp
 * Purpose:       Per-loop hook that raises the shared
 *                controller dialogs (the "Controller
 *                Disconnected" alert) over whichever menu /
 *                dialog loop is currently running.  Controller
 *                mode now follows real-device presence and
 *                last-used device automatically, so there is
 *                no opt-in prompt.
 *********************************************************/

#include "imgui_controller_prompt.h"

extern "C" {
#include "imgui_controller_disconnect.h"
#include "../input_gamepad.h"
#include "../../ui_mode.h"
}

void controllerDialogsRenderMenu(void) {
    /* Active-controller-disconnect on the menus: alert the player their pad
       dropped.  The edge only fires for a real controller, so no pref gate.
       No solo-game pause — no game is running here. */
    if (inputGamepadConsumeActiveDisconnect() && !uiModeIsTablet() &&
        !controllerDisconnectIsOpen()) {
        controllerDisconnectOpen();
    }
    /* Auto-dismiss when a real controller reconnects (real presence, not the
       phantom-pinned inputGamepadIsConnected). */
    if (controllerDisconnectIsOpen() && inputGamepadRealControllerConnected()) {
        controllerDisconnectClose();
    }
    (void)controllerDisconnectRender();   /* return ignored: no solo game to unpause */
}
