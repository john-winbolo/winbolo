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
 * An animated GIF decode with a limit on the memory the frames take.
 *
 * stbi_load_gif_from_memory keeps every frame in one block and grows it with
 * an int product of the frame count and the frame size, which is never range
 * checked: a large GIF with many frames wraps that product and stb then
 * writes past the end of the block. This decode reads the GIF one frame at a
 * time with stb's own frame reader (defined in stb_image_impl.c, the file that
 * holds stb's implementation), does the size arithmetic in size_t, and stops
 * before a frame would take the total over maxBytes.
 */

#ifndef STB_IMAGE_GIF_CAPPED_H
#define STB_IMAGE_GIF_CAPPED_H

#include <stddef.h>

#include "stb_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when frames frames of w x h RGBA pixels fit in maxBytes. False for a
 * size of 0 or less. The product is computed so that it cannot overflow. */
int stbiGifFramesFit(int w, int h, size_t frames, size_t maxBytes);

/* The frames' worth of memory stb's frame reader holds while it decodes, on
 * top of the frames kept: its current frame (4 bytes a pixel), the
 * background (4) and the history (1), 2.25 frames, counted as 3. */
#define STBI_GIF_WORK_FRAMES 3u

/* Asked before each frame is read; a value other than 0 stops the decode. */
typedef int (*StbiGifStopFn)(void *user);

/* Decodes every frame of the GIF in buffer as RGBA (4 bytes a pixel), one
 * frame after the other in the block returned, with the time each frame is
 * shown in *delays (ms). Free both with stbi_image_free.
 *
 * The limit counts the frames kept plus STBI_GIF_WORK_FRAMES frames for
 * stb's own buffers, so the decode never holds more than maxBytes, apart
 * from the copy a realloc may make while a frame is added (for that moment
 * the old block and the new one both exist).
 *
 * stop may be NULL. When it is not, it is called with stopUser before each
 * frame, and a value other than 0 ends the decode and returns NULL (with
 * *tooLarge 0), so a decode nobody wants any more ends within one frame.
 *
 * Returns NULL when the data is not a GIF, has no frame that decodes, was
 * stopped, or would need more than maxBytes; *tooLarge is 1 only in the
 * last case. A GIF that is cut off or corrupt after some frames gives the
 * frames before the fault, as stbi_load_gif_from_memory does. */
stbi_uc *stbiLoadGifCapped(const stbi_uc *buffer, int len, size_t maxBytes,
                           int **delays, int *x, int *y, int *frames,
                           int *tooLarge, StbiGifStopFn stop, void *stopUser);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* STB_IMAGE_GIF_CAPPED_H */
