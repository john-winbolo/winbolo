/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          mapeditor_wbn_open.h
 * Purpose:       The map editor's "Open from WinBolo.net"
 *                dialog: the map chooser wired to the
 *                WinBolo.net catalogue in a modal window.
 *                Picking a map downloads it on the shared
 *                worker and hands the raw .map bytes back to
 *                the editor, which loads them from memory.
 *                Built into the WinBolo client and the
 *                standalone MapEditor (MAPEDITOR_WBN_OPEN);
 *                the WASM build has no WBN stack.
 *********************************************************/

#ifndef MAPEDITOR_WBN_OPEN_H
#define MAPEDITOR_WBN_OPEN_H

#if defined(MAPEDITOR_WBN_OPEN) && !defined(__EMSCRIPTEN__)

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Availability of the WBN backend (default true). The standalone
 * editor sets this from httpCreate()'s result at startup; the menu
 * item is disabled while false. */
void meWbnOpenSetAvailable(bool available);
bool meWbnOpenAvailable(void);

/* Open the dialog (no-op if already open or unavailable). */
void meWbnOpenShow(void);

/* True while the dialog is open. */
bool meWbnOpenIsOpen(void);

/* Render + poll, call once per frame from the editor's ImGui frame
 * (no-op while closed). Returns true when a downloaded map is ready:
 * *outBytes is an SDL_malloc'd buffer the caller owns and frees,
 * *outLen its size, outName the display name (no .map extension).
 * The dialog closes itself on a successful download. */
bool meWbnOpenFrame(SDL_Renderer *renderer,
                    unsigned char **outBytes, int *outLen,
                    char *outName, int outNameLen);

/* Editor is exiting: close if open, stop the preview worker, drop any
 * in-flight download. */
void meWbnOpenShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_WBN_OPEN && !__EMSCRIPTEN__ */

#endif /* MAPEDITOR_WBN_OPEN_H */
