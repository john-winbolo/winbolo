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
 * Name:          clip_gif_gallery.h
 * Purpose:       The GIF gallery window (clip_gif_gallery.cpp),
 *                which the game's replay, the welcome screen
 *                and the Log Viewer all open. It lists the GIFs
 *                in <pref folder>/clips and plays the selected
 *                one. The screen that shows it gives it the
 *                renderer, the window and the open game's id
 *                each frame (ClipGifGalleryHost), so the
 *                gallery itself needs nothing of the lobby or
 *                of the viewer.
 *
 *                C++ only: it draws with ImGui.
 *********************************************************/

#ifndef CLIP_GIF_GALLERY_H
#define CLIP_GIF_GALLERY_H

#include <stdbool.h>
#include <stddef.h>

#include "clip_gif_core.h"  /* ClipGifMeta */

/* The gallery is built where a GIF can be made and kept in a folder the
 * player can open: not on iOS or Android (each saves a GIF its own way) and
 * not on the web (no clip export). */
#if !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__) && !defined(__ANDROID__)
#define CLIP_GIF_GALLERY_BUILT 1
#else
#define CLIP_GIF_GALLERY_BUILT 0
#endif

#if CLIP_GIF_GALLERY_BUILT

struct SDL_Renderer;
struct SDL_Window;

/* What the screen that shows the gallery gives it each frame. */
typedef struct ClipGifGalleryHost {
    /* The renderer the screen draws with: the preview's texture is made on
     * it. */
    struct SDL_Renderer *renderer;
    /* The window that owns the clipboard for Copy. */
    struct SDL_Window   *window;
    /* The id of the game whose replay is open (clipGifGameIdFromBytes), or
     * "" or NULL for none. The GIFs from it are listed first. */
    const char          *gameId;
    /* The lang id of the text shown when there are no GIFs, or 0 for the
     * default text, which names the replay's GIF buttons. */
    unsigned int         emptyText;
} ClipGifGalleryHost;

/* Opens the window, or brings it to the front. */
void clipGifGalleryOpen(void);
/* True while the window is open. */
bool clipGifGalleryIsOpen(void);
/* Closes the window and lets go of what it holds. */
void clipGifGalleryClose(void);

/* Keeps a finished GIF in the gallery folder, with its metadata, then opens
 * the gallery on it. Writes the bytes to a file and keeps no pointer to
 * them, so the caller still frees them. */
void clipGifGalleryAdd(const void *data, size_t len, const ClipGifMeta *meta,
                       const char *baseName);

/* Once per frame, before that frame is drawn. Lets go of the preview, a
 * decode still running and the list when the open window was not drawn on
 * the last frame; the window stays open. */
void clipGifGalleryPoll(void);

/* The window, drawn once per frame while it is open, inside the ImGui frame
 * of the screen that shows it. s is the screen's UI scale. */
void clipGifGalleryRender(float s, const ClipGifGalleryHost *host);

/* Leaving the screen that showed it: frees the preview's frames and texture,
 * drops a decode still running and closes the window. */
void clipGifGalleryShutdown(void);

/* The control that opens the gallery, for the replay's transport row. */
bool clipGifGalleryButton(const char *id);

#endif /* CLIP_GIF_GALLERY_BUILT */

#endif /* CLIP_GIF_GALLERY_H */
