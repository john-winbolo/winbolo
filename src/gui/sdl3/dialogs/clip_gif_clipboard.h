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

/*
 * Puts a GIF file on the system clipboard for the recap's GIF gallery.
 *
 * Windows: the file goes on as a file (CF_HDROP), which is what Explorer and
 * Discord paste, together with the GIF's bytes under the registered "GIF"
 * format that some image programs read.
 * Other platforms: the bytes go on as "image/gif" through
 * SDL_SetClipboardData, with a file:// line as "text/uri-list" for file
 * managers.
 */

#ifndef CLIP_GIF_CLIPBOARD_H
#define CLIP_GIF_CLIPBOARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gifPath is the UTF-8 path of an existing GIF file. window is the
 * SDL_Window that owns the clipboard on Windows and may be NULL. Returns
 * false when the file could not be read or the clipboard refused it. */
bool clipGifCopyFileToClipboard(const char *gifPath, void *window);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* CLIP_GIF_CLIPBOARD_H */
