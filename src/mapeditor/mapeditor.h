/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor.h
 * Purpose:
 *   Public API for the WinBolo map editor.
 *********************************************************/

#ifndef MAPEDITOR_H
#define MAPEDITOR_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
 * mapEditorRun — Core editor loop.
 *
 * Takes ownership of the event loop until the user exits.
 * mapPath may be NULL to create a blank 256x256 map.
 * Window and renderer are borrowed, not owned.
 *********************************************************/
void mapEditorRun(SDL_Window *window, SDL_Renderer *renderer, const char *mapPath, bool fromMainMenu);

/* Did the run that just returned end because the player quit, rather than
 * leaving the editor?  Only the embedded caller asks; it hands the answer to
 * windowSetQuitting(). */
bool mapEditorAppQuitRequested(void);

/* The host's settings window, run from the editor's Settings menu item.
 * WinBolo sets this before mapEditorRun; the standalone MapEditor has no
 * settings window, leaves it NULL, and so shows no menu item. The function
 * runs its own loop and must return with no ImGui context current. */
typedef void (*MapEditorSettingsFn)(void);
void mapEditorSetSettingsHandler(MapEditorSettingsFn fn);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_H */
