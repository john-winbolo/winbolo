/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Ping Icons
 *Filename:      ping_icons.h
 *Purpose:
 *  The six smart-ping glyphs, rasterised out of
 *  data/ui/ping/ onto one renderer and kept for the life of
 *  that renderer.
 *
 *  Loose SVG files rather than anything baked into the
 *  binary, so the art can be replaced without a rebuild.
 *  They are authored white on transparent and used as a
 *  shape only: every drawer tints them with the kind's
 *  colour from ping_kinds.h, so an icon file's own fill is
 *  never what ends up on screen.
 *
 *  Plain C and no ImGui, because both the game's overlay and
 *  the log viewer's replay draw need them and only one of
 *  those has an ImGui context.
 *********************************************************/

#ifndef WINBOLO_PING_ICONS_H
#define WINBOLO_PING_ICONS_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rasterise the icons onto `renderer`. Any previously loaded set is freed
 * first, so this is also the renderer-changed path. A file that will not load
 * leaves its slot NULL rather than failing the call: the drawers fall back to
 * a plain disc, so missing art costs a marker its glyph and nothing else. */
void pingIconsInit(SDL_Renderer *renderer);

/* Free the textures. Must run before the renderer they were made on. */
void pingIconsShutdown(void);

/* The texture for a PING_KIND_*, or NULL when it is out of range or its file
 * did not load. */
SDL_Texture *pingIconTexture(unsigned char kind);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_ICONS_H */
