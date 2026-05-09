/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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

/* Inject Steam Input Menu-set actions as ImGui gamepad-nav key events
   into the *current* ImGui context.  Call once per frame, after
   ImGui::NewFrame for that context.  No-op when Steam Input has no
   active controller (Path B handles its own nav via SDL events). */
void imguiSteamNavFeedCurrentContext(void);

/* Idempotent activator for the Steam Input "Menu" action set.  Safe
   to call every frame; redundant calls are filtered out. */
void imguiSteamNavActivateMenuSet(void);

/* Idempotent activator for the Steam Input "InGame" action set.  Use
   from the main game pump when no popup modal is open. */
void imguiSteamNavActivateGameSet(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_STEAM_NAV_H */
