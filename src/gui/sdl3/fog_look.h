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

/*********************************************************
 * Name:          fog_look.h
 * Purpose:       What fog over unseen ground looks like.
 *
 *   The two views that draw the world draw the fog their
 *   own way — the classic view washes over the tiles it has
 *   just put down, the full screen map blends a mask texture
 *   over the whole terrain — but they have to agree on the
 *   colour and the strength or the same ground reads as two
 *   different amounts of hidden. That agreement lives here.
 *
 *   The player picks one of four looks. All four wash the
 *   same squares by the same amount; only the colour changes,
 *   and None does not wash at all:
 *
 *     Grey            a blend towards mid grey. The default,
 *                     and what the game has drawn since the
 *                     dimming was dropped.
 *     Darker          a blend towards black, which is the
 *                     dimming that came before the grey.
 *     Darker + roads  the same black, plus a short grey band
 *                     drawn inside the edge of every fogged
 *                     road square that faces something other
 *                     than road. See fog_roads.h.
 *     None            no wash. The terrain reads the same
 *                     fogged or not.
 *
 *   Why grey is the default: the terrain art gives a dimming
 *   nothing to work on. road_solid is 256 black pixels out of
 *   256, and grass is 224 out of 256 with the rest green
 *   speckle. Multiplying black by anything leaves black, so a
 *   road under the dimming was pixel for pixel a road in plain
 *   sight. Blending towards grey lifts those black pixels
 *   instead, which is most of every square, and fogged ground
 *   therefore comes out lighter than lit ground rather than
 *   darker.
 *
 *   The Darker + roads look is the answer for a player who
 *   wants the darker picture back and can live with the roads
 *   only if their edges are drawn in: the band is the one part
 *   of a fogged road square that a dimming cannot swallow.
 *
 *   None hides nothing the other three show. The wash is a
 *   tint over terrain the player is remembering; which units
 *   are drawn at all is the server's business, and none of
 *   these values touches it.
 *********************************************************/

#ifndef FOG_LOOK_H
#define FOG_LOOK_H

/* The colour unseen ground is taken towards under the default look. Neutral
 * grey: the terrain that does carry colour is green, cyan and blue, and a fog
 * with any blue in it reads as water over land. */
#define FOG_LOOK_R 96
#define FOG_LOOK_G 96
#define FOG_LOOK_B 96

/* How far towards it, out of 255. At 145 a black square settles at 55 grey —
 * plainly not black — while terrain bright enough to have a colour keeps it,
 * washed. Raise it for thicker fog; raise the colour above for a lighter
 * fog that dims the bright terrain no further.
 *
 * The strength is the same for every look, so the overview's mask — one byte
 * a square, built before the look is known — needs no rebuilding when the
 * player changes their mind. */
#define FOG_LOOK_ALPHA 145

/* The player's pick. The numbers are written into prefs, so they are fixed:
 * append, never renumber. 0 is what the game drew before the setting
 * existed. */
typedef enum FogStyle {
    FOG_STYLE_GREY       = 0,
    FOG_STYLE_DARK       = 1,
    FOG_STYLE_DARK_ROADS = 2,
    FOG_STYLE_NONE       = 3
} FogStyle;

#define FOG_STYLE_COUNT 4

/* The colour the wash blends towards, for a look that washes at all. Returns
 * 0 for None, and the caller then draws no fog; the three bytes are left
 * alone in that case. Anything outside the enum is treated as the default,
 * so a hand-edited prefs file cannot produce a look nothing knows how to
 * draw. */
static inline int fogLookColour(FogStyle style, unsigned char *r,
                                unsigned char *g, unsigned char *b) {
    if (style == FOG_STYLE_NONE) return 0;
    if (style == FOG_STYLE_DARK || style == FOG_STYLE_DARK_ROADS) {
        /* Black, which is the dimming the grey replaced: a wash to black at
         * FOG_LOOK_ALPHA over a black clear leaves exactly what the old alpha
         * mod of 110/255 on the tile itself left. */
        *r = 0; *g = 0; *b = 0;
        return 1;
    }
    *r = (unsigned char)FOG_LOOK_R;
    *g = (unsigned char)FOG_LOOK_G;
    *b = (unsigned char)FOG_LOOK_B;
    return 1;
}

/* Whether this look draws the road edge bands as well as the wash. */
static inline int fogLookDrawsRoadEdges(FogStyle style) {
    return style == FOG_STYLE_DARK_ROADS;
}

#endif /* FOG_LOOK_H */
