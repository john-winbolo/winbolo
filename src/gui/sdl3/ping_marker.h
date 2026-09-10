/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          ping_marker.h
 * Purpose:       The smart-ping marker as it appears on the
 *                map: one map square outlined in the ping's
 *                colour, its icon inside at tile size, a ring
 *                pulsing out of it and the sender's name
 *                underneath. Drawn with the
 *                SDL_Renderer as part of the game frame,
 *                between the terrain and the sprites, so the
 *                tanks, shells and builders it points at stay
 *                on top of it. The live client and the replay
 *                viewer both call this, so a ping looks the
 *                same in a recording as it did in the game.
 *********************************************************/

#ifndef WINBOLO_PING_MARKER_H
#define WINBOLO_PING_MARKER_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "tank_label.h"   /* TankLabelCache — the name is drawn through it */

#ifdef __cplusplus
extern "C" {
#endif

/* How much of the marker's colour shows: it is a hint on the ground, not a
   sprite, so the terrain reads through it and anything drawn after it wins. */
#define PING_MARKER_ALPHA 0.62f

/* Air between the bottom of the marker square and the top of the name, as a
   fraction of a map square. */
#define PING_MARKER_NAME_GAP 0.15f

/* Who sent the ping, for the line under the marker. The host supplies all of
   it because the two things it names belong to the host: the name comes from
   the live game's ping record or the replay's player table, and the cache's
   textures belong to the renderer that host draws on.

   The cache must be one this host keeps for ping names alone — it is keyed on
   the player slot, so sharing the tank pass's cache would have the two
   rebuilding each other's texture every frame.

   Zeroed, or with a NULL font or an empty name, draws no name and nothing
   else changes. */
typedef struct {
    TankLabelCache *cache;
    TTF_Font       *font;
    const char     *name;
    unsigned char   slot;    /* the sender, which is the cache's key */
    float           scale;   /* on the font's own pixel size; 1 for none */
} PingMarkerLabel;

/* Draw one marker. (cx, cy) is the centre of the map square the ping landed
   in, in the renderer's current coordinates; tileW/tileH the square's size
   there; ageMs how long ago the ping arrived (drives the pulse); alpha the
   fade from pingDisplayAlpha, 1 while fresh and falling to 0 at expiry; label
   the sender's name or NULL for none.
   Needs the icons loaded on this renderer (pingIconsInit); without one the
   icon falls back to a filled dot. */
void pingMarkerDraw(SDL_Renderer *renderer, unsigned char kind,
                    float cx, float cy, float tileW, float tileH,
                    unsigned int ageMs, float alpha,
                    const PingMarkerLabel *label);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_MARKER_H */
