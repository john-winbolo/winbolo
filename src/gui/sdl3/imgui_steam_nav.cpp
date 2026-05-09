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
    if (!steam_input_has_active_controller()) return;

    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,
                   steam_input_is_action_pressed(SI_ACTION_MENU_ACCEPT));
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight,
                   steam_input_is_action_pressed(SI_ACTION_MENU_CANCEL));
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_UP));
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_DOWN));
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_LEFT));
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_RIGHT));
}
