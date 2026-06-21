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

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_H */
