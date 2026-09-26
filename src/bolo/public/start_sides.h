/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef START_SIDES_H
#define START_SIDES_H

/* Which side of the map a start sits on, and whether a team may use it.
 * Header-only (static inline, no structs) so the sim, the server and the
 * lobby GUI all classify a start the same way: the lobby's compass label,
 * the team's side choice and start placement read the one mask computed
 * here. Integer arithmetic throughout, so every peer gets the same answer
 * for the same start. */

#include <stdbool.h>
#include "platform_types.h"  /* BYTE */

/* Team start-side choice — wire value, stored in TeamMetadata.startSide. */
#define START_SIDE_ANY   0
#define START_SIDE_N     1
#define START_SIDE_E     2
#define START_SIDE_S     3
#define START_SIDE_W     4
#define START_SIDE_COUNT 5

/* Membership bits. A start may hold two (a NE start is N|E); zero means the
 * start sits in the centre band and belongs to no side. */
#define START_SIDE_BIT_N 0x01
#define START_SIDE_BIT_E 0x02
#define START_SIDE_BIT_S 0x04
#define START_SIDE_BIT_W 0x08

/* Side mask of the start at (sx,sy) within the start bounding box
 * [minX..maxX, minY..maxY]. Map Y increases downward, so north is the
 * smaller y. A start within an eighth of the box extent of the centre on
 * both axes is in the centre band and returns 0. Otherwise the start falls
 * in one of eight 45-degree sectors around the centre: a cardinal sector
 * gives one bit, a diagonal sector gives both bits either side of it. The
 * sector test compares |dy| : |dx| against 5 : 12, which approximates
 * tan(22.5 degrees), so no floating point is needed. */
static inline BYTE startSideMaskFor(int sx, int sy,
                                    int minX, int minY, int maxX, int maxY) {
    int cx = (minX + maxX) / 2;
    int cy = (minY + maxY) / 2;
    int dx = sx - cx;
    int dy = sy - cy;
    int tolX = (maxX - minX) / 8;
    int tolY = (maxY - minY) / 8;
    int ax = (dx < 0) ? -dx : dx;
    int ay = (dy < 0) ? -dy : dy;
    BYTE ns = (dy < 0) ? START_SIDE_BIT_N : START_SIDE_BIT_S;
    BYTE ew = (dx >= 0) ? START_SIDE_BIT_E : START_SIDE_BIT_W;
    if (tolX < 1) tolX = 1;
    if (tolY < 1) tolY = 1;
    if (ax <= tolX && ay <= tolY) {
        return 0;
    }
    if (ay * 12 <= ax * 5) {
        return ew;              /* flat enough to be east or west only */
    }
    if (ax * 12 <= ay * 5) {
        return ns;              /* steep enough to be north or south only */
    }
    return (BYTE)(ns | ew);     /* diagonal: on both sides */
}

/* Membership bit for a team's chosen side. START_SIDE_ANY, and any value
 * outside the START_SIDE_* range, has no bit. */
static inline BYTE startSideBits(BYTE side) {
    switch (side) {
        case START_SIDE_N: return START_SIDE_BIT_N;
        case START_SIDE_E: return START_SIDE_BIT_E;
        case START_SIDE_S: return START_SIDE_BIT_S;
        case START_SIDE_W: return START_SIDE_BIT_W;
        default:           return 0;
    }
}

/* A centre-band start belongs to no side. */
static inline bool startSideIsCentre(BYTE mask) {
    return mask == 0;
}

/* The bits facing the ones in mask: north against south, east against
 * west. Used to point a team with no side at the far side of the map from
 * the sides the other teams chose. Being off a chosen side is not on its
 * own the other side of the map: on a map whose starts ring the island,
 * everything but the east is a horseshoe running west, north and south
 * that comes back to meet the east at both ends, and a team spread over
 * the whole of it puts somebody next to the team it is playing. */
static inline BYTE startSideOppositeBits(BYTE mask) {
    BYTE out = 0;
    if (mask & START_SIDE_BIT_N) out |= START_SIDE_BIT_S;
    if (mask & START_SIDE_BIT_S) out |= START_SIDE_BIT_N;
    if (mask & START_SIDE_BIT_E) out |= START_SIDE_BIT_W;
    if (mask & START_SIDE_BIT_W) out |= START_SIDE_BIT_E;
    return out;
}

/* Whether a start with this mask is on a team's chosen side. A team with
 * no side takes any start; a centre start is open to every team. */
static inline bool startSideAccepts(BYTE mask, BYTE side) {
    BYTE bits = startSideBits(side);
    if (bits == 0) {
        return true;
    }
    if (mask == 0) {
        return true;
    }
    return (mask & bits) != 0;
}

/* Whether a team may be placed on a start with this mask. closedMask is
 * the union of the sides other teams chose. A team with a side takes what
 * its side accepts, sharing the side with any other team that chose it.
 * A team with no side stays off every chosen side, so only starts with no
 * bit in closedMask — centre starts included — remain open to it. */
static inline bool startSideEligible(BYTE mask, BYTE side, BYTE closedMask) {
    if (startSideBits(side) != 0) {
        return startSideAccepts(mask, side);
    }
    return (mask & closedMask) == 0;
}

/* The terrain byte for deep sea, the same value as DEEP_SEA in global.h.
 * Repeated here so this header needs nothing but BYTE. */
#define START_SIDE_TERRAIN_DEEP_SEA 0xFF

/* The bounding box of every square that is not deep sea, in a row-major
 * terrain grid: terrain[(y * stride) + x] is the square at (x, y), for x in
 * 0..width-1 and y in 0..height-1. Every other terrain counts, mined squares
 * and boats included. Returns false, and leaves the outputs alone, when the
 * grid has no square that is not deep sea. */
static inline bool startSideLandBounds(const BYTE *terrain, int width,
                                       int height, int stride,
                                       int *outMinX, int *outMinY,
                                       int *outMaxX, int *outMaxY) {
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;
    int x;
    int y;
    if (terrain == 0 || width <= 0 || height <= 0 || stride < width) {
        return false;
    }
    for (y = 0; y < height; y++) {
        const BYTE *row = terrain + ((long)y * stride);
        for (x = 0; x < width; x++) {
            if (row[x] == START_SIDE_TERRAIN_DEEP_SEA) continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (maxX < 0) {
        return false;
    }
    if (outMinX) *outMinX = minX;
    if (outMinY) *outMinY = minY;
    if (outMaxX) *outMaxX = maxX;
    if (outMaxY) *outMaxY = maxY;
    return true;
}

/* Whether a fresh lobby puts teams 1 and 2 on east and west rather than on
 * north and south: true when the squares that are not deep sea span more
 * columns than rows. A tie, and a grid that is all deep sea, give false, so
 * north/south stays the default for a square or an empty map. Same grid
 * layout as startSideLandBounds. */
static inline bool startSideDefaultIsEastWest(const BYTE *terrain, int width,
                                              int height, int stride) {
    int minX;
    int minY;
    int maxX;
    int maxY;
    if (!startSideLandBounds(terrain, width, height, stride,
                             &minX, &minY, &maxX, &maxY)) {
        return false;
    }
    return (maxX - minX) > (maxY - minY);
}

#endif /* START_SIDES_H */
