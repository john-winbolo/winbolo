/*
 * Copyright (c) 1998-2026 John Morrison.
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

#ifndef SDL_BMP_H
#define SDL_BMP_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load a BMP from disk and create an SDL texture.
 *
 * If useColorKey is true, the BMP is converted to ARGB8888, green
 * (0,255,0) is set as the transparent color key, and the texture's
 * blend mode is set to SDL_BLENDMODE_BLEND. This is what every sprite
 * sheet in data/ uses for transparency.
 *
 * If useColorKey is false, the BMP is loaded as-is (no conversion,
 * no blend mode change) — suitable for opaque images like the
 * background or splash screen.
 *
 * Returns NULL on any failure (file missing, conversion failed,
 * texture creation failed). Caller owns the returned texture and
 * must SDL_DestroyTexture it.
 */
SDL_Texture *sdlLoadBmpAsTexture(SDL_Renderer *renderer,
                                 const char *path,
                                 bool useColorKey);

/* Load a BMP from an already-open SDL_IOStream and create an SDL
 * texture. Same conversion, color-key and blend-mode behaviour as
 * sdlLoadBmpAsTexture — this is the entry point for bytes that did not
 * come from a plain file, such as a BMP read out of a skin archive.
 *
 * If closeio is true the stream is closed whether the load succeeds or
 * fails. A stream wrapping caller-owned memory (SDL_IOFromMem) closes
 * the stream only; the caller still frees the bytes.
 *
 * Returns NULL on any failure. Caller owns the returned texture and
 * must SDL_DestroyTexture it.
 */
SDL_Texture *sdlLoadBmpStreamAsTexture(SDL_Renderer *renderer,
                                       SDL_IOStream *src, bool closeio,
                                       bool useColorKey);

#ifdef __cplusplus
}
#endif

#endif /* SDL_BMP_H */
