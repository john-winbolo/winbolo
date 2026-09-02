/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Tank labels
 *Filename:      tank_label.h
 *Purpose:
 *  The one implementation of the in-game tank label: the
 *  player name split from its trailing "@location", drawn
 *  over its own shadow with a country flag or brain icon
 *  beside it. The classic view, the full screen map and
 *  the pop-out overview all draw through here — each host
 *  supplies only a renderer, a screen position and a
 *  scale, so a change to what a label renders shows up in
 *  every view at once.
 *
 *  Textures belong to the renderer that made them, so the
 *  cache is per-host rather than global: each view owns a
 *  TankLabelCache and the textures inside it are built on
 *  that view's renderer from renderer-independent sources
 *  (the shared TTF face, the flag / brain icon surfaces).
 *  The cache flushes itself when the renderer or the font
 *  it built against changes.
 *********************************************************/

#ifndef TANK_LABEL_H
#define TANK_LABEL_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "global.h" /* BYTE, MAX_TANKS — no C++ guards of its own */

/* Longest label the cache stores; longer strings are truncated the same
 * way the classic pass always has. */
#define TANK_LABEL_NAME_LEN 64

/* Zero-initialise to start empty (both hosts allocate their copy zeroed).
 * The fields are an implementation detail of tank_label.c — hosts only
 * hold the struct and hand it back. */
typedef struct TankLabelCache {
    SDL_Renderer *renderer;
    TTF_Font     *font;
    SDL_Texture  *nameTex[MAX_TANKS];
    SDL_Texture  *iconTex[MAX_TANKS];   /* flag or brain icon, NULL for none */
    char          str[MAX_TANKS][TANK_LABEL_NAME_LEN];
} TankLabelCache;

/*********************************************************
 *NAME:          tankLabelDraw
 *PURPOSE:
 *  Draws one tank label at a screen position: the name
 *  over its own shadow, then the country flag (humans
 *  with a 2-letter location) or brain icon (bots) beside
 *  it. Rebuilds the player's cached textures if the label
 *  string, the renderer or the font changed.
 *ARGUMENTS:
 *  c         - the host's label cache
 *  r         - renderer to build on and draw with
 *  font      - TTF face for the name (the newswire's)
 *  label     - the label screenTanksPrepare built, with
 *              its trailing "@location" still attached
 *  playerNum - slot the label belongs to
 *  x, y      - top-left of the name in render coordinates
 *  scale     - multiplier on the rendered glyph size; 1
 *              draws the face at the size it was opened
 *RETURNS:
 *  true if the label was drawn; false when there is
 *  nothing to draw (no font, empty label, bad slot).
 *********************************************************/
bool tankLabelDraw(TankLabelCache *c, SDL_Renderer *r, TTF_Font *font,
                   const char *label, BYTE playerNum,
                   float x, float y, float scale);

/*********************************************************
 *NAME:          tankLabelCacheFlush
 *PURPOSE:
 *  Destroys every texture in the cache and forgets the
 *  renderer and font they were built against. Call from
 *  the host's teardown; tankLabelDraw flushes on its own
 *  when the renderer or font changes under it.
 *********************************************************/
void tankLabelCacheFlush(TankLabelCache *c);

#ifdef __cplusplus
}
#endif

#endif /* TANK_LABEL_H */
