/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          imgui_settings.h
 * Purpose:       ImGui pre-game settings dialog.
 *********************************************************/

#ifndef IMGUI_SETTINGS_H
#define IMGUI_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show the pre-game settings dialog as a blocking modal loop. */
void imguiSettingsShow(void);

/* Draw the "Graphics" CollapsingHeader (Theme picker + Apply +
 * Animation style + sprite/rotating-tank preview).  Shared between
 * the splash dialog and the in-game Settings panel.  Must be called
 * from inside an ImGui frame; the rotating preview uses the supplied
 * renderer for SDL_RenderTextureRotated. */
struct SDL_Renderer;
void imguiSettingsDrawGraphicsSection(struct SDL_Renderer *renderer);

/* Reset the Graphics section's pending preview selection back to the
 * currently-active theme.  Call when opening the Settings dialog so
 * the dropdown shows the active theme on every fresh open instead of
 * the last-previewed one from a prior session. */
void imguiSettingsResetGraphicsSelection(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_SETTINGS_H */
