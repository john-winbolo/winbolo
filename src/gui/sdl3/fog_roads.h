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
 * Purpose:       Which sides of a fogged road square get a
 *                band drawn inside them.
 *
 *   A road is 256 black pixels out of 256, so the Darker fog
 *   leaves it exactly as it was: the player can read the fog
 *   line over grass and cannot read it over road at all. The
 *   Darker + roads look answers that by drawing the road's
 *   own outline back in — a short grey band inside each edge
 *   of a fogged road square that faces something that is not
 *   road. The band fades out as it goes in, so the road keeps
 *   a soft edge rather than gaining a drawn border.
 *
 *   Only the edges facing non-road are banded, so a wide road
 *   or a crossroads is outlined round the outside and not
 *   ruled into squares.
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

/* How many bands the fade is cut into, and how strong the one at the very
 * edge is out of 255. Band i carries alpha FOG_ROAD_ALPHA * (BANDS - i) /
 * BANDS, so with three bands at 153 the fade runs 153, 102, 51 and is gone by
 * the fourth pixel in. Three bands at the classic tile size is three
 * pixels. */
#define FOG_ROAD_BANDS 3
#define FOG_ROAD_ALPHA 153

/* The band's colour. Lighter than the fog's own grey: it is drawn over a
 * square that has just been taken to black, and it has three pixels to say
 * where the road is in. */
#define FOG_ROAD_R 128
#define FOG_ROAD_G 128
#define FOG_ROAD_B 128

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
 * which is outside the run and therefore not road, so the caller needs no
 * special case for it. */
static inline int fogRoadIsRoadTile(unsigned char tile) {
    return tile >= (unsigned char)ROAD_HORZ && tile <= (unsigned char)ROAD_SIDE4;
}

/* The sides of one square that want a band: none at all unless the square
 * itself is road, and then one bit for each neighbour that is not.
 *
 * The neighbours are tile numbers, in the same numbering as `tile`. A caller
 * with no neighbour to offer — the square is at the edge of the map, or of
 * the grid it is walking — passes any non-road number, and the edge is banded
 * as if it faced open ground. */
static inline unsigned char fogRoadEdges(unsigned char tile,
                                         unsigned char left,
                                         unsigned char right,
                                         unsigned char up,
                                         unsigned char down) {
    unsigned char edges = 0;
    if (!fogRoadIsRoadTile(tile)) return 0;
    if (!fogRoadIsRoadTile(left))  edges |= FOG_ROAD_EDGE_LEFT;
    if (!fogRoadIsRoadTile(right)) edges |= FOG_ROAD_EDGE_RIGHT;
    if (!fogRoadIsRoadTile(up))    edges |= FOG_ROAD_EDGE_TOP;
    if (!fogRoadIsRoadTile(down))  edges |= FOG_ROAD_EDGE_BOTTOM;
    return edges;
}

/* The same answer for every square of a w by h grid of tile numbers, both
 * laid out row by row — tiles[y * w + x], and out the same. A square at the
 * grid's own edge is given open ground beyond it, which is what the classic
 * view wants: its grid is the back buffer, and a square outside that is off
 * the screen.
 *
 * Whole-grid rather than per-square so the one walk a caller makes is the one
 * this file is tested on. */
static inline void fogRoadEdgeMasks(const unsigned char *tiles, int w, int h,
                                    unsigned char *out) {
    int x, y; /* Looping variables */
    /* Any number outside the road run stands for "no neighbour"; 0 is the
     * first tank frame and is never a terrain square. */
    const unsigned char none = 0;

    if (tiles == NULL || out == NULL) return;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char tile  = tiles[y * w + x];
            unsigned char left  = (x > 0)     ? tiles[y * w + (x - 1)] : none;
            unsigned char right = (x < w - 1) ? tiles[y * w + (x + 1)] : none;
            unsigned char up    = (y > 0)     ? tiles[(y - 1) * w + x] : none;
            unsigned char down  = (y < h - 1) ? tiles[(y + 1) * w + x] : none;
            out[y * w + x] = fogRoadEdges(tile, left, right, up, down);
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
