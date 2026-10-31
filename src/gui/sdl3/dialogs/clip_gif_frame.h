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
 * Name:          clip_gif_frame.h
 * Purpose:       One GIF frame, read back off the viewer's
 *                render target and given to the GIF encoder.
 *                The game's replay (lobby_clip_gif.cpp) and
 *                the Log Viewer (imgui_clip_gif.cpp) both
 *                make their GIFs with it. Header only, so a
 *                target that builds one of them needs no other
 *                file for it.
 *********************************************************/

#ifndef CLIP_GIF_FRAME_H
#define CLIP_GIF_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

/* The single-header GIF encoder. The one implementation file is
 * src/third_party/msf_gif/msf_gif_impl.c. It has its own extern "C" guards. */
#include "../../../third_party/msf_gif/msf_gif.h"

/* Centiseconds each frame is shown (10 fps), and the encoder's own default
 * quality. */
#define CLIP_GIF_CS_PER_FRAME 10
#define CLIP_GIF_QUALITY      16

/* Reads crop out of texture (a render target of renderer) and gives it to
 * enc as the next frame. The render target the caller had is put back, as a
 * screen calls this in the middle of its own frame. False when the readback
 * or the encoder fails. */
static inline bool clipGifEncodeFrame(SDL_Renderer *renderer,
                                      SDL_Texture *texture,
                                      const SDL_Rect *crop,
                                      MsfGifState *enc) {
    if (renderer == NULL || texture == NULL || crop == NULL || enc == NULL) {
        return false;
    }
    SDL_Texture *saved = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }
    SDL_Surface *raw = SDL_RenderReadPixels(renderer, crop);
    SDL_SetRenderTarget(renderer, saved);
    if (raw == NULL) {
        return false;
    }
    /* Convert rather than assume: the readback's layout follows the render
     * target, and the encoder reads RGBA8 rows. Its own pitch goes with it,
     * as a converted surface is not promised to be tightly packed. */
    SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (rgba == NULL) {
        return false;
    }
    bool ok = msf_gif_frame(enc, (uint8_t *)rgba->pixels,
                            CLIP_GIF_CS_PER_FRAME, CLIP_GIF_QUALITY,
                            rgba->pitch) != 0;
    SDL_DestroySurface(rgba);
    return ok;
}

#endif /* CLIP_GIF_FRAME_H */
