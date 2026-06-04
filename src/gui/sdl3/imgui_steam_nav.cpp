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

/* Edge-latch arming for the Accept/Cancel keys — see latchedPress() and
   imguiSteamNavFeedCurrentContext() for the rationale.  Disarmed = the
   button must be released before it can fire again. */
static bool s_acceptArmed = true;
static bool s_cancelArmed = true;

static void activate_set(const char *desired) {
    if (desired != s_steam_input_current_set) {
        steam_input_activate_action_set(desired);
        s_steam_input_current_set = desired;
        /* The same physical button maps to different actions per set
           (A is Fire in InGame, Accept in Menu), so a button held across
           a set switch must not read as a fresh menu press.  Force a
           release-before-fire on both activating keys. */
        s_acceptArmed = false;
        s_cancelArmed = false;
    }
}

extern "C" void imguiSteamNavActivateMenuSet(void) {
    activate_set(SI_SET_MENU);
}

extern "C" void imguiSteamNavActivateGameSet(void) {
    activate_set(SI_SET_IN_GAME);
}

/* Accept/Cancel are edge-latched globally (one shared state across every
   ImGui context the feed serves): they emit a key-down only on a fresh
   press after a release, never while the button stays held.  Without
   this, the press that activates a menu item (e.g. selecting Single
   Player) is still physically held when the next dialog opens in its own
   ImGui context — which starts with fresh key state, reads the held
   button as a brand-new press, and instantly activates that dialog's
   default-focused item (the top-left Back button → "return to menu?").
   Single-player surfaced it because its lobby opens with no network
   delay, so the button is still down on the first frame.  Directional
   nav is left continuous so hold-to-repeat still scrolls lists.  The
   arming state lives up by activate_set so a set switch can also disarm
   it (a held Fire button must not read as Accept in the Menu set). */
static bool latchedPress(bool pressed, bool *armed) {
    bool emit = pressed && *armed;
    *armed = !pressed;   /* re-arm only once the button is released */
    return emit;
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
                   latchedPress(steam_input_is_action_pressed(SI_ACTION_MENU_ACCEPT),
                                &s_acceptArmed));
    io.AddKeyEvent(ImGuiKey_Escape,
                   latchedPress(steam_input_is_action_pressed(SI_ACTION_MENU_CANCEL),
                                &s_cancelArmed));
    io.AddKeyEvent(ImGuiKey_UpArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_UP));
    io.AddKeyEvent(ImGuiKey_DownArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_DOWN));
    io.AddKeyEvent(ImGuiKey_LeftArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_LEFT));
    io.AddKeyEvent(ImGuiKey_RightArrow,
                   steam_input_is_action_pressed(SI_ACTION_MENU_NAV_RIGHT));
}
