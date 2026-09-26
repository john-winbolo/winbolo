/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_settings_workshop
 * Filename:      imgui_settings_workshop.h
 * Purpose:
 *   The Workshop section of the Settings dialog's Display
 *   tab, drawn under the skin picker's Workshop buttons.
 *   Two views switched by two buttons: what the player is
 *   subscribed to, and the mods, scenarios and scenario
 *   maps of their own they can publish through the shared
 *   publish window.
 *
 *   Desktop only. The .cpp is compiled into the desktop
 *   client alone; this header only declares, so it can be
 *   included by a file every build compiles as long as the
 *   call sits behind !BOLO_MOBILE && !__EMSCRIPTEN__.
 *********************************************************/

#ifndef IMGUI_SETTINGS_WORKSHOP_H
#define IMGUI_SETTINGS_WORKSHOP_H

/* Draw the section. Nothing at all unless Steam's Workshop is available.
   Call every frame the Display tab draws, so the publish window it opens is
   drawn every frame too. */
void imguiSettingsWorkshopSection(void);

#endif /* IMGUI_SETTINGS_WORKSHOP_H */
