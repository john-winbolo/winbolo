/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_steam_nav.h
 * Purpose:       Reusable Steam Input -> ImGui gamepad-nav
 *                bridge.  Used by the main game ImGui
 *                context and every standalone dialog that
 *                creates its own ImGui context, so menu
 *                navigation works on Steam launches where
 *                Steam Input intercepts SDL_Gamepad and the
 *                ImGui SDL3 backend never sees raw events.
 *********************************************************/

#ifndef IMGUI_STEAM_NAV_H
#define IMGUI_STEAM_NAV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Inject Steam Input Menu-set actions as ImGui keyboard-nav key events
   (arrows / Space / Escape) into the *current* ImGui context.  Call
   once per frame, after ImGui::NewFrame for that context.  Keyboard
   rather than gamepad keys because Steam Input hides the pad from SDL,
   so the backend never sets HasGamepad and gamepad-nav keys would be
   ignored; keyboard nav gates only on NavEnableKeyboard.  Pumps
   steam_input_run_frame() internally so standalone dialog loops (which
   don't pump it themselves) still detect the controller and navigate.
   No-op when Steam Input has no active controller (Path B handles its
   own nav via SDL events). */
void imguiSteamNavFeedCurrentContext(void);

/* Idempotent activator for the Steam Input "Menu" action set.  Safe
   to call every frame; redundant calls are filtered out. */
void imguiSteamNavActivateMenuSet(void);

/* Idempotent activator for the Steam Input "InGame" action set.  Use
   from the main game pump when no popup modal is open. */
void imguiSteamNavActivateGameSet(void);

/* Edge-latched tab cycle from the Steam Input Menu actions menu_tab_left /
   menu_tab_right (typically the triggers).  Returns +1 (next tab), -1
   (previous tab) on a fresh press, or 0.  Returns 0 when Steam Input isn't
   active (the native path uses ImGuiKey_GamepadL1/R1 instead).  Consume once
   per frame from a tab bar that wants trigger tab-switching under Steam. */
int imguiSteamNavConsumeMenuTabShift(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_STEAM_NAV_H */
