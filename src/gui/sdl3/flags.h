/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *  Initialize the flags system, storing the renderer for
 *  lazy texture creation.
 *ARGUMENTS:
 *  renderer - The SDL_Renderer to create textures on.
 *RETURNS:
 *  true on success.
 *********************************************************/
bool flagsCreate(SDL_Renderer *renderer);

/*********************************************************
 *NAME:          flagsDestroy
 *PURPOSE:
 *  Free all cached flag textures.
 *********************************************************/
void flagsDestroy(void);

/*********************************************************
 *NAME:          flagsGetTexture
 *PURPOSE:
 *  Return the SDL_Texture for a 2-letter country code.
 *  Loads and caches the SVG on first request.
 *  Returns NULL if the flag cannot be loaded.
 *ARGUMENTS:
 *  countryCode - 2-char ISO 3166-1 alpha-2 code (e.g. "US")
 *********************************************************/
SDL_Texture *flagsGetTexture(const char countryCode[2]);

#ifdef __cplusplus
}
#endif

#endif /* FLAGS_H */
