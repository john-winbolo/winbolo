/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Flags
 *Filename:      flags.h
 *Purpose:
 *  Country flag rendering from individual SVG files in
 *  data/flags/. Each flag is loaded and rasterized on
 *  demand using nanosvg, then cached as an SDL_Texture.
 *********************************************************/

#ifndef FLAGS_H
#define FLAGS_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default display size for flag icons (pixels) */
#define FLAG_WIDTH  16
#define FLAG_HEIGHT 11

/*********************************************************
 *NAME:          flagsCreate
 *PURPOSE:
 *  Register a renderer for lazy flag texture creation.
 *  Two renderers are supported: the first registered gets
 *  slot 0 (the one flagsGetTexture serves), the second
 *  slot 1.  Registering a renderer that already holds a
 *  slot does nothing, so repeat calls with the same
 *  renderer keep the cache they built.
 *ARGUMENTS:
 *  renderer - The SDL_Renderer to create textures on.
 *RETURNS:
 *  true on success, false if there is no free slot.
 *********************************************************/
bool flagsCreate(SDL_Renderer *renderer);

/*********************************************************
 *NAME:          flagsDestroy
 *PURPOSE:
 *  Free all cached flag textures and unregister both
 *  renderers.
 *********************************************************/
void flagsDestroy(void);

/*********************************************************
 *NAME:          flagsGetTexture
 *PURPOSE:
 *  Return the SDL_Texture for a 2-letter country code on
 *  the first registered renderer (slot 0). Loads and
 *  caches the SVG on first request. Returns NULL if the
 *  flag cannot be loaded.
 *ARGUMENTS:
 *  countryCode - 2-char ISO 3166-1 alpha-2 code (e.g. "US")
 *********************************************************/
SDL_Texture *flagsGetTexture(const char countryCode[2]);

/*********************************************************
 *NAME:          flagsGetTextureFor
 *PURPOSE:
 *  As flagsGetTexture, but against the cache of a specific
 *  registered renderer.  A texture may only be drawn
 *  through the renderer that created it, so a second
 *  window asks for its own copy here.
 *  Returns NULL if the renderer was never registered or
 *  the flag cannot be loaded.
 *ARGUMENTS:
 *  renderer    - A renderer passed to flagsCreate.
 *  countryCode - 2-char ISO 3166-1 alpha-2 code (e.g. "US")
 *********************************************************/
SDL_Texture *flagsGetTextureFor(SDL_Renderer *renderer,
                                const char countryCode[2]);

/*********************************************************
 *NAME:          flagsGetSurface
 *PURPOSE:
 *  Return the cached rasterization of a flag as an
 *  SDL_Surface — renderer-free, so a consumer on any
 *  renderer (the tank-label caches) can build its own
 *  texture from it. Owned by this module; do not destroy.
 *  Returns NULL if the flag cannot be loaded.
 *ARGUMENTS:
 *  countryCode - 2-char ISO 3166-1 alpha-2 code (e.g. "US")
 *********************************************************/
SDL_Surface *flagsGetSurface(const char countryCode[2]);

#ifdef __cplusplus
}
#endif

#endif /* FLAGS_H */
