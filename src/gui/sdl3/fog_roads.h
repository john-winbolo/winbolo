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
 * Name:          fog_roads.h
 * Purpose:       Which sides of a fogged square get a band
 *                drawn inside them, to show the fog line.
 *
 *   A road is 256 black pixels out of 256, so the Darker fog
 *   leaves it exactly as it was: the player can read the fog
 *   line over grass and cannot read it over road at all. The
 *   Darker with fog edge look answers that by drawing the fog
 *   line itself back in — a faint lift inside each edge of a
 *   fogged square that faces a square in plain sight. It is a
 *   little white mixed in, not a grey stripe: the edge comes
 *   out dark grey rather than pitch black, and fades back to
 *   the fog over three pixels, so the fog keeps a soft edge
 *   rather than gaining a drawn border.
 *
 *   The band is on the fogged side of the line, and only on
 *   the fogged side: the square in plain sight next to it is
 *   drawn as itself and needs nothing. Ground well inside the
 *   fog gets no band either, because there is no fog line
 *   there to show.
 *
 *   A neighbour the caller cannot name — off the map, or off
 *   the grid it is walking — counts as fogged, so no band is
 *   drawn along the edge of the screen.
 *
 *   FOG_EDGE_ALL_TERRAIN says which terrain the band lands
 *   on. Andrew chose all terrain on 15 Sep 2026, so it is 1
 *   and every fogged square at the fog line is banded. Set it
 *   to 0 to go back to banding road only, which is the case
 *   the look was first written for.
 *
 *   Arithmetic only — no SDL, so it compiles into the unit
 *   test binary. The drawing half is fog_roads_draw.h.
 *********************************************************/

#ifndef FOG_ROADS_H
#define FOG_ROADS_H

#include <stddef.h> /* NULL */

#include "tilenum.h" /* ROAD_HORZ .. ROAD_SIDE4 */

/* One bit a side, in the order the drawing half walks them. */
#define FOG_ROAD_EDGE_LEFT   0x01
#define FOG_ROAD_EDGE_RIGHT  0x02
#define FOG_ROAD_EDGE_TOP    0x04
#define FOG_ROAD_EDGE_BOTTOM 0x08

/* ---- The one place the band's look is set. Tune here. ----
 *
 * Andrew wants this subtle: a slight lifting of the fog, not a grey stripe
 * ruled along it. So the band is white at a low alpha over ground that has
 * just been taken to black, and the fade is short.
 *
 * How many bands the fade is cut into, and how strong the one at the very edge
 * is out of 255. Band i carries alpha FOG_ROAD_ALPHA * (BANDS - i) / BANDS, so
 * three bands at 40 run 40, 26, 13 — about 15%, 10% and 5% — and are gone by
 * the fourth pixel in. Three bands at the classic tile size is three pixels.
 *
 * What that comes to on the screen: a fogged road is black, so its edge
 * settles at about 40 out of 255, a dark grey that reads as an edge without
 * reading as a line, and falls back to black over the three pixels. Fogged
 * ground that is not black only lifts a touch, which is all that is wanted
 * there — the darkening already shows the fog line over it, and the band is
 * only firming up a line the player can read already.
 *
 * These live here rather than in fog_roads_draw.h because this is the half
 * with no SDL in it, and the unit tests read them.
 *
 * Raise FOG_ROAD_ALPHA for a stronger edge; drop it towards 20 for one barely
 * there. The colour is what the band is mixed in; white is the only thing
 * that lifts a black square at all. */
#define FOG_ROAD_BANDS 3
#define FOG_ROAD_ALPHA 40

#define FOG_ROAD_R 255
#define FOG_ROAD_G 255
#define FOG_ROAD_B 255

/* The tile the band is cut out of, at the classic 16-pixel tile. A view
 * drawing bigger tiles scales this with them so the outline keeps its share
 * of the square at every zoom. */
#define FOG_ROAD_CLASSIC_TILE 16

/* Every road piece the screen calculator can produce is one number run:
 * ROAD_HORZ through ROAD_SIDE4, which takes in the plain roads, the corners,
 * the crossings and T junctions, the pieces that meet water and the single
 * sides. Nothing else lives in that run — the next number after ROAD_SIDE4 is
 * unused, and the one after that is a base.
 *
 * A square that has never been seen is handed over as OVERVIEW_UNSEEN (255),
 * which is outside the run and therefore not road. That is enough to keep a
 * band off it only while FOG_EDGE_ALL_TERRAIN is 0, which is not how the game
 * is built: with the switch at 1 this function is not consulted at all, so a
 * caller that can produce an unseen square has to drop it itself. The full
 * screen map does, beside the same test its terrain pass makes. */
static inline int fogRoadIsRoadTile(unsigned char tile) {
    return tile >= (unsigned char)ROAD_HORZ && tile <= (unsigned char)ROAD_SIDE4;
}

/* Which terrain the band lands on. 1, the default, bands every fogged square
 * at the fog line, whatever is on it: Andrew looked at both on 15 Sep 2026 and
 * picked this one. 0 bands fogged road only, which is where the darkening
 * cannot show the fog line at all. The constant is kept so roads only is one
 * switch away. Not a setting and not in the dialog: define it on the
 * compiler's command line to build the other way. */
#ifndef FOG_EDGE_ALL_TERRAIN
#define FOG_EDGE_ALL_TERRAIN 1
#endif

/* Whether a square of this terrain is one the band is wanted on at all. Road
 * is the case the look exists for — a darkening cannot mark a black square —
 * and the switch above takes in everything else as well. */
static inline int fogEdgeTileWantsBand(unsigned char tile) {
#if FOG_EDGE_ALL_TERRAIN
    (void)tile;
    return 1;
#else
    return fogRoadIsRoadTile(tile);
#endif
}

/* Which sides of a square face out of the fog, given only its neighbours —
 * the half of the rule that has nothing to do with what is on the square.
 * Split out from fogEdges below so a caller walking a whole map can ask the
 * cheap question first: the answer is 0 for a square well inside the fog,
 * which is most of a map, and such a square needs no band whatever its
 * terrain. That lets the walk drop it before reading the terrain at all.
 *
 * The four flags are true for a neighbour the fog covers. A caller with no
 * neighbour to offer — the square is at the edge of the map, or of the grid it
 * is walking — passes it as fogged, and no band is drawn along that side.
 *
 * This is the only place the side bits are worked out. fogEdges is this plus
 * the two questions about the square itself, so a caller that uses this
 * directly cannot drift from one that uses fogEdges. */
static inline unsigned char fogEdgeSides(int leftFogged, int rightFogged,
                                         int upFogged, int downFogged) {
    unsigned char edges = 0;
    if (!leftFogged)  edges |= FOG_ROAD_EDGE_LEFT;
    if (!rightFogged) edges |= FOG_ROAD_EDGE_RIGHT;
    if (!upFogged)    edges |= FOG_ROAD_EDGE_TOP;
    if (!downFogged)  edges |= FOG_ROAD_EDGE_BOTTOM;
    return edges;
}

/* The sides of one square that want a band. Three things have to hold: the
 * square is fogged, its terrain is one the band is wanted on, and the side
 * faces a square that is not fogged. So the band lands on the fogged side of
 * the fog line and nowhere else.
 *
 * `fogged` and the four neighbour flags are true for a square the fog covers.
 * A caller with no neighbour to offer — the square is at the edge of the map,
 * or of the grid it is walking — passes it as fogged, and no band is drawn
 * along that side.
 *
 * Nothing here rules out a square the player has never seen. The two views
 * differ on whether they can hand one over: the classic view's buffer always
 * carries a real tile, and the full screen map has OVERVIEW_UNSEEN for a
 * square it has drawn nothing on, which it drops before it gets here. See the
 * note on fogRoadIsRoadTile. */
static inline unsigned char fogEdges(unsigned char tile, int fogged,
                                     int leftFogged, int rightFogged,
                                     int upFogged, int downFogged) {
    if (!fogged) return 0;
    if (!fogEdgeTileWantsBand(tile)) return 0;
    return fogEdgeSides(leftFogged, rightFogged, upFogged, downFogged);
}

/* The same answer for every square of a w by h grid, laid out row by row —
 * tiles[y * w + x], fogged[y * w + x] non-zero for a fogged square, and out
 * the same. A square at the grid's own edge is given fog beyond it, which is
 * what both views want: a square outside the grid is off the screen, and the
 * fog line does not run along the edge of the screen.
 *
 * Whole-grid rather than per-square so the one walk a caller makes is the one
 * this file is tested on. */
static inline void fogEdgeMasks(const unsigned char *tiles,
                                const unsigned char *fogged, int w, int h,
                                unsigned char *out) {
    int x, y; /* Looping variables */

    if (tiles == NULL || fogged == NULL || out == NULL) return;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int slot = y * w + x;
            int lf = (x > 0)     ? (fogged[slot - 1] != 0) : 1;
            int rf = (x < w - 1) ? (fogged[slot + 1] != 0) : 1;
            int uf = (y > 0)     ? (fogged[slot - w] != 0) : 1;
            int df = (y < h - 1) ? (fogged[slot + w] != 0) : 1;
            out[slot] = fogEdges(tiles[slot], fogged[slot] != 0,
                                 lf, rf, uf, df);
        }
    }
}

/* The alpha band `band` carries, counting 0 at the square's edge. The fade is
 * linear and reaches 0 at FOG_ROAD_BANDS, so the last band drawn is one step
 * above nothing rather than nothing. */
static inline unsigned char fogRoadBandAlpha(int band) {
    int a;
    if (band < 0 || band >= FOG_ROAD_BANDS) return 0;
    a = (FOG_ROAD_ALPHA * (FOG_ROAD_BANDS - band)) / FOG_ROAD_BANDS;
    return (unsigned char)a;
}

#endif /* FOG_ROADS_H */
