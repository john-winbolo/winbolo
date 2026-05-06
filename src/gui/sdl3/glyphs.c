/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * glyphs.c — Path A glyph lookup for controller button icons.
 *
 * Wraps steam_input_get_glyph_path() with an SDL_Texture cache.
 * Steam returns the same const char * for the same binding, so the
 * cache key is the path pointer itself — pointer-equality match.
 * Cache grows monotonically up to s_cache size; with ~7 actions in
 * the pause overlay we won't approach the cap.  Rebinds during a
 * session leave the cache stale until restart (V1 limitation).
 *
 * PNG decode mirrors the crosshair load at sdl3draw.c:951-982:
 * SDL_IOFromFile -> SDL_GetIOSize -> SDL_ReadIO -> stbi_load_from_memory
 * -> SDL_CreateSurfaceFrom -> SDL_CreateTextureFromSurface.
 */

#include "glyphs.h"

#include <stdio.h>
#include <SDL3/SDL.h>

#include "stb_image.h"
#include "../../steam/steam_wrapper.h"

#define GLYPH_CACHE_MAX 64

typedef struct {
  const char  *path;
  SDL_Texture *tex;
} GlyphCacheEntry;

static SDL_Renderer    *s_renderer = NULL;
static GlyphCacheEntry  s_cache[GLYPH_CACHE_MAX];
static int              s_cache_count = 0;

bool glyphsInit(SDL_Renderer *renderer) {
  s_renderer = renderer;
  s_cache_count = 0;
  for (int i = 0; i < GLYPH_CACHE_MAX; ++i) {
    s_cache[i].path = NULL;
    s_cache[i].tex  = NULL;
  }
  return true;
}

void glyphsShutdown(void) {
  for (int i = 0; i < s_cache_count; ++i) {
    if (s_cache[i].tex) {
      SDL_DestroyTexture(s_cache[i].tex);
    }
    s_cache[i].path = NULL;
    s_cache[i].tex  = NULL;
  }
  s_cache_count = 0;
  s_renderer    = NULL;
}

static SDL_Texture *load_glyph_png(const char *path) {
  SDL_IOStream *io = SDL_IOFromFile(path, "rb");
  if (!io) return NULL;

  Sint64 sz = SDL_GetIOSize(io);
  if (sz <= 0) { SDL_CloseIO(io); return NULL; }

  unsigned char *buf = (unsigned char *)SDL_malloc((size_t)sz);
  if (!buf) { SDL_CloseIO(io); return NULL; }
  SDL_ReadIO(io, buf, (size_t)sz);
  SDL_CloseIO(io);

  int imgW = 0, imgH = 0, ch = 0;
  unsigned char *pix = stbi_load_from_memory(buf, (int)sz, &imgW, &imgH, &ch, 4);
  SDL_free(buf);
  if (!pix) return NULL;

  SDL_Surface *surf = SDL_CreateSurfaceFrom(imgW, imgH, SDL_PIXELFORMAT_RGBA32,
                                            pix, imgW * 4);
  if (!surf) { stbi_image_free(pix); return NULL; }

  SDL_Texture *tex = SDL_CreateTextureFromSurface(s_renderer, surf);
  SDL_DestroySurface(surf);
  stbi_image_free(pix);

  if (tex) {
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
  }
  return tex;
}

SDL_Texture *glyphForAction(const char *action_name) {
  if (!s_renderer || !action_name) return NULL;

  const char *path = steam_input_get_glyph_path(action_name);
  if (!path) return NULL;

  /* Pointer-equality cache lookup — Steam returns the same const char *
     for the same binding across calls. */
  for (int i = 0; i < s_cache_count; ++i) {
    if (s_cache[i].path == path) return s_cache[i].tex;
  }

  if (s_cache_count >= GLYPH_CACHE_MAX) {
    fprintf(stderr, "glyphs: cache full (%d entries), dropping '%s'\n",
            GLYPH_CACHE_MAX, action_name);
    return NULL;
  }

  SDL_Texture *tex = load_glyph_png(path);
  s_cache[s_cache_count].path = path;
  s_cache[s_cache_count].tex  = tex;
  ++s_cache_count;
  return tex;
}
