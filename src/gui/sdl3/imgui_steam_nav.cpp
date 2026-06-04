/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_steam_nav.cpp
 * Purpose:       Implementation of the Steam Input ->
 *                ImGui gamepad-nav bridge.  See header for
 *                rationale.
 *********************************************************/

#include "imgui.h"

extern "C" {
#include "../../steam/steam_wrapper.h"
#include "../../steam/steam_input_actions.h"
#include "imgui_steam_nav.h"
}

/* Compare is pointer-equality against the static-storage SI_SET_*
   literals from steam_input_actions.h, so steady-state cost is one
   pointer compare per call. */
static const char *s_steam_input_current_set = nullptr;

static void activate_set(const char *desired) {
    if (desired != s_steam_input_current_set) {
        steam_input_activate_action_set(desired);
        s_steam_input_current_set = desired;
    }
}

extern "C" void imguiSteamNavActivateMenuSet(void) {
    activate_set(SI_SET_MENU);
}

extern "C" void imguiSteamNavActivateGameSet(void) {
    activate_set(SI_SET_IN_GAME);
}

extern "C" void imguiSteamNavFeedCurrentContext(void) {
    /* Advance Steam Input before reading actions.  The main game loop
       pumps steam_input_run_frame() itself, but standalone dialogs
       (lobby/main menu, message boxes, welcome, etc.) run their own
       event loops that don't — so without this the active controller is
       never detected in those contexts and menu nav silently dies.
       RunFrame is level-based, so the extra in-game call is harmless. */
    steam_input_run_frame();

    if (!steam_input_has_active_controller()) return;

    /* Inject as KEYBOARD nav keys, not gamepad keys.  On a Steam launch
       Steam Input grabs the physical pad and hides it from SDL, so the
       ImGui SDL3 backend never sets ImGuiBackendFlags_HasGamepad (it
       clears the flag every frame and only re-sets it when SDL has an
       open gamepad).  ImGui gates gamepad nav on that flag, so injected
       ImGuiKey_Gamepad* events are ignored.  The keyboard nav path
       (arrows / Space / Escape) gates only on NavEnableKeyboard, which
       every dialog and the main context set — so these always take. */
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Space,
                   steam_input_is_action_pressed(SI_ACTION_MENU_ACCEPT));
    io.AddKeyEvent(ImGuiKey_Escape,
                   steam_input_is_action_pressed(SI_ACTION_MENU_CANCEL));
    io.AddKeyEvent(ImGuiKey_UpArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_UP));
    io.AddKeyEvent(ImGuiKey_DownArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_DOWN));
    io.AddKeyEvent(ImGuiKey_LeftArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_LEFT));
    io.AddKeyEvent(ImGuiKey_RightArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_RIGHT));
}
