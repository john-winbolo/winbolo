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

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show the pre-game settings dialog as a blocking modal loop. */
void imguiSettingsShow(void);

/* Draw the "Graphics" CollapsingHeader (Skin picker + Apply +
 * Animation style + sprite/rotating-tank preview).  Shared between
 * the splash dialog and the in-game Settings panel.  Must be called
 * from inside an ImGui frame; the rotating preview uses the supplied
 * renderer for SDL_RenderTextureRotated. */
struct SDL_Renderer;
void imguiSettingsDrawGraphicsSection(struct SDL_Renderer *renderer);

/* Reset the Graphics section's pending preview selection back to the
 * currently-active skin.  Call when opening the Settings dialog so
 * the dropdown shows the active skin on every fresh open instead of
 * the last-previewed one from a prior session. */
void imguiSettingsResetGraphicsSelection(void);

/* True when the Graphics section's skin dropdown is on a different
 * skin than the active one — i.e. user picked but didn't click
 * Apply.  Used at close time to ask "apply previewed skin?". */
bool imguiSettingsHasUnappliedSkinPreview(void);

/* Name of the skin currently selected in the dropdown.  Returns
 * "(default)" for the empty/default skin.  Lifetime: valid until
 * the next Settings dialog open. */
const char *imguiSettingsGetPreviewedSkinName(void);

/* Commit the previewed skin (same effect as clicking Apply in the
 * Graphics section). */
void imguiSettingsApplyPreviewedSkin(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_SETTINGS_H */
