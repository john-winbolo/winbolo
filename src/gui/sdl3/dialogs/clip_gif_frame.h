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
 * Purpose:       One GIF frame: the crop of the viewer's render
 *                target (1x pixel art) drawn into a texture at
 *                the scale the screen shows it, the tank names
 *                drawn over it as the screen draws them, and
 *                that texture read back and given to the GIF
 *                encoder. The game's replay (lobby_clip_gif.cpp)
 *                and the Log Viewer (imgui_clip_gif.cpp) both
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

/* Draws the tank names over a frame: src is the part of the render target
 * the frame holds and dst where it went in the frame, both in pixels of
 * their textures, so a name at target pixel (x, y) goes at
 * dst->x + (x - src->x) * (dst->w / src->w). The frame is the render target
 * while this runs. */
typedef void (*ClipGifDrawNames)(void *user, SDL_Renderer *renderer,
                                 const SDL_FRect *src, const SDL_FRect *dst);

/* Makes one frame: crop of world (the 1x render target) drawn into all of
 * frame (a render target of frameW x frameH) with nearest-pixel scaling, as
 * the screen magnifies it, and the tank names drawn over it by drawNames at
 * that scale. The render target the caller had is put back, as a screen
 * calls this in the middle of its own frame; so is world's scale mode, which
 * the screen's own blit relies on. False when world is now too small to
 * hold all of crop (a window resize made a new one), or a draw fails. */
static inline bool clipGifComposeFrame(SDL_Renderer *renderer,
                                       SDL_Texture *world,
                                       const SDL_Rect *crop,
                                       SDL_Texture *frame, int frameW,
                                       int frameH, ClipGifDrawNames drawNames,
                                       void *user) {
    if (renderer == NULL || world == NULL || crop == NULL || frame == NULL ||
        crop->w <= 0 || crop->h <= 0 || frameW <= 0 || frameH <= 0) {
        return false;
    }
    float worldW = 0.0f, worldH = 0.0f;
    if (!SDL_GetTextureSize(world, &worldW, &worldH) || crop->x < 0 ||
        crop->y < 0 || (float)(crop->x + crop->w) > worldW ||
        (float)(crop->y + crop->h) > worldH) {
        return false;
    }
    SDL_Texture *saved = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, frame)) {
        return false;
    }
    SDL_ScaleMode savedMode = SDL_SCALEMODE_NEAREST;
    const bool hadMode = SDL_GetTextureScaleMode(world, &savedMode);
    SDL_SetTextureScaleMode(world, SDL_SCALEMODE_NEAREST);
    const SDL_FRect src = { (float)crop->x, (float)crop->y, (float)crop->w,
                            (float)crop->h };
    const SDL_FRect dst = { 0.0f, 0.0f, (float)frameW, (float)frameH };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    const bool ok = SDL_RenderTexture(renderer, world, &src, &dst);
    if (ok && drawNames != NULL) {
        drawNames(user, renderer, &src, &dst);
    }
    if (hadMode) {
        SDL_SetTextureScaleMode(world, savedMode);
    }
    SDL_SetRenderTarget(renderer, saved);
    return ok;
}

/* Reads crop out of texture (a render target of renderer) and gives it to
 * enc as the next frame. The render target the caller had is put back, as a
 * screen calls this in the middle of its own frame. False when the readback
 * or the encoder fails, or when the render target is now too small to hold
 * all of crop. */
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
    /* SDL clips the rect to the render target as it is now. When the target
     * has become smaller since the capture started (the window was resized),
     * the surface is smaller than the crop, and the encoder, which was told
     * the crop's size at msf_gif_begin, would read past its end. */
    if (raw->w != crop->w || raw->h != crop->h) {
        SDL_DestroySurface(raw);
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
