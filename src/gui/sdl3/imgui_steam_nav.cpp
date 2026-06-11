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
#include "input_gamepad.h"   /* native pad: left-stick -> menu nav */
/* While the Set Keys dialog is capturing a controller binding, the held
   button must reach the capture intercept as a raw button — not be injected
   as Space/Escape nav (which would also activate the focused dialog button,
   e.g. Cancel). */
bool imguiKeySetupIsCapturingInGamePad(void);
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

/* Tab cycle (LT/RT under Steam Input).  Edge-latched like Accept/Cancel so a
   held trigger steps one tab, not every frame. */
static bool s_tabLeftArmed  = false;
static bool s_tabRightArmed = false;

extern "C" int imguiSteamNavConsumeMenuTabShift(void) {
    if (!steam_input_has_active_controller()) return 0;
    if (imguiKeySetupIsCapturingInGamePad()) return 0;
    int shift = 0;
    if (latchedPress(steam_input_is_action_pressed(SI_ACTION_MENU_TAB_RIGHT),
                     &s_tabRightArmed)) shift += 1;
    if (latchedPress(steam_input_is_action_pressed(SI_ACTION_MENU_TAB_LEFT),
                     &s_tabLeftArmed))  shift -= 1;
    return shift;
}

extern "C" void imguiSteamNavFeedCurrentContext(void) {
    /* Advance Steam Input before reading actions.  The main game loop
       pumps steam_input_run_frame() itself, but standalone dialogs
       (lobby/main menu, message boxes, welcome, etc.) run their own
       event loops that don't — so without this the active controller is
       never detected in those contexts and menu nav silently dies.
       RunFrame is level-based, so the extra in-game call is harmless. */
    steam_input_run_frame();
    ImGuiIO &io = ImGui::GetIO();

    /* Don't inject any nav while a controller binding is being captured — the
       raw button press belongs to the capture, not to menu activation. */
    if (imguiKeySetupIsCapturingInGamePad())
        return;

    if (steam_input_has_active_controller()) {
        /* Inject as KEYBOARD nav keys, not gamepad keys.  On a Steam launch
           Steam Input grabs the physical pad and hides it from SDL, so the
           ImGui SDL3 backend never sets ImGuiBackendFlags_HasGamepad (it
           clears the flag every frame and only re-sets it when SDL has an
           open gamepad).  ImGui gates gamepad nav on that flag, so injected
           ImGuiKey_Gamepad* events are ignored.  The keyboard nav path
           (arrows / Space / Escape) gates only on NavEnableKeyboard, which
           every dialog and the main context set — so these always take. */
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
        return;
    }

    /* Native SDL gamepad path.
       ImGui's built-in gamepad nav uses the D-pad for item-to-item movement
       but reserves the LEFT STICK for *window scrolling* (imgui.cpp NavUpdate)
       — so in a scrollable list the stick drags the scrollbar instead of
       moving the selection. We can't fix that by injecting ImGuiKey_Gamepad*
       events: the SDL3 backend rewrites all gamepad keys every frame from the
       physical pad, and our feed runs after ImGui::NewFrame so the backend
       always wins the next frame.
       Instead we do exactly what the Steam path does — disable ImGui's own
       gamepad nav and drive everything through the KEYBOARD nav path (which the
       backend never feeds from the pad): stick AND D-pad -> arrows, A -> Space,
       B -> Escape. Keyboard nav auto-scrolls to keep the focused item visible,
       so the stick now steps items (no scrollbar drift). Restricted to the Menu
       action set so in-game steering and ImGui gamepad nav are untouched. */

    /* Find a usable handle: prefer our promoted pad, else the first SDL
       gamepad (which is the handle ImGui's backend already opened). */
    SDL_Gamepad *gp = inputGamepadGetActiveHandle();
    if (!gp) {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        if (ids) {
            if (count > 0) gp = SDL_GetGamepadFromID(ids[0]);
            SDL_free(ids);
        }
    }

    bool menuSet = (s_steam_input_current_set &&
                    SDL_strcmp(s_steam_input_current_set, SI_SET_MENU) == 0);

    /* Toggle ImGui's gamepad nav so the LStick stops scrolling while we drive
       nav from the keyboard. Disabled only while a native pad is in a menu;
       re-enabled otherwise (in-game, or no pad) so nothing else regresses.
       Takes effect next NewFrame — fine since the state is held continuously. */
    if (gp && menuSet)
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    if (!gp || !menuSet) return;

    const float DEAD = 0.5f;
    float ax = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    float ay = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    const bool *ks = SDL_GetKeyboardState(NULL);
    /* Directions: left stick OR physical D-pad OR live keyboard arrows. */
    bool up    = ay < -DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP)    || (ks && ks[SDL_SCANCODE_UP]);
    bool down  = ay >  DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN)  || (ks && ks[SDL_SCANCODE_DOWN]);
    bool left  = ax < -DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT)  || (ks && ks[SDL_SCANCODE_LEFT]);
    bool right = ax >  DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (ks && ks[SDL_SCANCODE_RIGHT]);
    io.AddKeyEvent(ImGuiKey_UpArrow,    up);
    io.AddKeyEvent(ImGuiKey_DownArrow,  down);
    io.AddKeyEvent(ImGuiKey_LeftArrow,  left);
    io.AddKeyEvent(ImGuiKey_RightArrow, right);

    /* Accept (A / South) -> Space, Cancel (B / East) -> Escape, edge-latched
       like the Steam path so a button held across a context switch doesn't
       read as a fresh activation. */
    io.AddKeyEvent(ImGuiKey_Space,
                   latchedPress(SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH),
                                &s_acceptArmed));
    io.AddKeyEvent(ImGuiKey_Escape,
                   latchedPress(SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST),
                                &s_cancelArmed));
}
