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
 * An animated GIF decoded one frame at a time, within a memory limit.
 *
 * stbi_load_gif_from_memory keeps every frame in one block and grows it with
 * an int product of the frame count and the frame size, which is never range
 * checked: a large GIF with many frames wraps that product and stb then
 * writes past the end of the block, and a long GIF of a large picture takes
 * hundreds of megabytes. A stream here reads the GIF with stb's own frame
 * reader (defined in stb_image_impl.c, the file that holds stb's
 * implementation) one frame at a time as the frames are shown, as a browser
 * plays a GIF, and holds a few frames' worth of memory however long the GIF
 * is. The size arithmetic is in size_t and is checked against the limit
 * before anything is allocated.
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

/* The frames' worth of memory a stream holds, all of the GIF's screen size
 * and 4 bytes a pixel: stb's current frame, background and history (4, 4
 * and 1 byte a pixel, 2.25 frames, counted as 3) and the two frames before
 * the current one, kept for a frame whose dispose mode restores the frame
 * two back. */
#define STBI_GIF_STREAM_FRAMES 5u

/* A GIF being read a frame at a time. */
typedef struct StbiGifStream StbiGifStream;

/* Opens the GIF in buffer for reading a frame at a time. buffer stays the
 * caller's and must stay as it is until the stream is closed. The frames
 * are all the size of the GIF's screen, *w x *h, as stb decodes them. NULL
 * when the data is not a GIF, or when STBI_GIF_STREAM_FRAMES frames of that
 * size are more than maxBytes (*tooLarge is 1 then, and 0 otherwise). */
StbiGifStream *stbiGifStreamOpen(const stbi_uc *buffer, int len,
                                 size_t maxBytes, int *w, int *h,
                                 int *tooLarge);

/* Reads the next frame: *out is its w x h RGBA pixels, the stream's own,
 * which stay until the next call on the stream; *delayMs is how long it is
 * shown. 0 at the end of the GIF, or where it is cut off or corrupt, with
 * nothing in *out: the stream then gives 0 until stbiGifStreamRewind. */
int stbiGifStreamNext(StbiGifStream *s, const stbi_uc **out, int *delayMs);

/* How many frames stbiGifStreamNext has given since the open or the last
 * rewind. */
int stbiGifStreamFramesRead(const StbiGifStream *s);

/* Back to before the first frame, to play the GIF again. */
void stbiGifStreamRewind(StbiGifStream *s);

/* Ends the stream and frees what it holds. NULL is allowed. */
void stbiGifStreamClose(StbiGifStream *s);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* STB_IMAGE_GIF_CAPPED_H */
