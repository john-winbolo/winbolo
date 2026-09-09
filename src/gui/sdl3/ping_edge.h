/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Ping Edge
 *Filename:      ping_edge.h
 *Purpose:
 *  Where the marker for an off-screen ping goes: the point
 *  at which the straight line from the viewer's tank to the
 *  ping leaves the game rectangle, and the short bar laid
 *  along that border so it reads as "over there".
 *
 *  Split out with no SDL and no ImGui so the four edges, the
 *  corner and the on-screen case can be tested directly.
 *
 *  Screen axes: x right, y DOWN.
 *********************************************************/

#ifndef WINBOLO_PING_EDGE_H
#define WINBOLO_PING_EDGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which border the marker sits on. Reported so a caller can pick the arrow
 * or the rounding it wants without re-deriving it from the coordinates. */
typedef enum {
    PING_EDGE_LEFT = 0,
    PING_EDGE_RIGHT,
    PING_EDGE_TOP,
    PING_EDGE_BOTTOM
} PingEdgeSide;

/* The bar to draw: its centre line, from (x0,y0) to (x1,y1), lying along the
 * border. The caller gives it thickness (PING_EDGE_THICKNESS_PX). */
typedef struct {
    float        x0, y0;
    float        x1, y1;
    float        cx, cy;   /* the exit point itself — the bar's midpoint */
    PingEdgeSide side;
} PingEdgeMarker;

/* Where does the line from the tank to the ping leave the game rectangle?
 *
 *   rx, ry, rw, rh  - the game rectangle in screen pixels
 *   tankX, tankY    - the viewer's tank, screen pixels
 *   pingX, pingY    - the ping's world position mapped to screen pixels;
 *                     may be far outside the rectangle
 *   barLen          - how long the marker bar is
 *
 * Returns false and writes nothing when the ping is inside the rectangle
 * (there is a world marker for it, so no edge marker is wanted) or when the
 * rectangle is degenerate. Otherwise fills *out.
 *
 * The tank is expected to be inside the rectangle — it is the view's own
 * camera — but a tank outside it (a spectator's free camera, a view being
 * handed over) is clamped in rather than refused, so the marker still points
 * somewhere sensible instead of vanishing.
 *
 * The bar is centred on the exit point and then slid along its border so it
 * stays wholly inside the rectangle: at a corner the bar sits flush into the
 * corner rather than hanging half off the screen. */
static inline bool pingEdgeMarker(float rx, float ry, float rw, float rh,
                                  float tankX, float tankY,
                                  float pingX, float pingY,
                                  float barLen, PingEdgeMarker *out) {
    float x1 = rx + rw, y1 = ry + rh;
    float dx, dy;
    float best = 2.0f;
    PingEdgeSide side = PING_EDGE_LEFT;
    float ex, ey, half, lo, hi;

    if (out == NULL || rw <= 0.0f || rh <= 0.0f) return false;
    if (pingX >= rx && pingX <= x1 && pingY >= ry && pingY <= y1) return false;

    /* A camera outside its own view has no meaningful ray; anchor to the
     * nearest point of the rectangle so direction is still preserved. */
    if (tankX < rx) tankX = rx;
    if (tankX > x1) tankX = x1;
    if (tankY < ry) tankY = ry;
    if (tankY > y1) tankY = y1;

    dx = pingX - tankX;
    dy = pingY - tankY;

    /* First crossing of each bounding plane the ray actually heads towards.
     * The smallest of them is where the ray leaves the rectangle. A ray
     * parallel to a plane never crosses it, so those arms are skipped. */
    if (dx > 0.0f) {
        float t = (x1 - tankX) / dx;
        if (t < best) { best = t; side = PING_EDGE_RIGHT; }
    } else if (dx < 0.0f) {
        float t = (rx - tankX) / dx;
        if (t < best) { best = t; side = PING_EDGE_LEFT; }
    }
    if (dy > 0.0f) {
        float t = (y1 - tankY) / dy;
        if (t < best) { best = t; side = PING_EDGE_BOTTOM; }
    } else if (dy < 0.0f) {
        float t = (ry - tankY) / dy;
        if (t < best) { best = t; side = PING_EDGE_TOP; }
    }
    /* The ping is outside and the tank is inside, so one plane is always
     * crossed; best > 1 would mean the two points coincide. */
    if (best > 1.0f) return false;

    ex = tankX + dx * best;
    ey = tankY + dy * best;
    /* Rounding can put the exit a hair outside; pin it to the border it
     * belongs to so the bar never starts off-rectangle. */
    if (ex < rx) ex = rx;
    if (ex > x1) ex = x1;
    if (ey < ry) ey = ry;
    if (ey > y1) ey = y1;
    if (side == PING_EDGE_LEFT)   ex = rx;
    if (side == PING_EDGE_RIGHT)  ex = x1;
    if (side == PING_EDGE_TOP)    ey = ry;
    if (side == PING_EDGE_BOTTOM) ey = y1;

    half = barLen * 0.5f;
    if (side == PING_EDGE_LEFT || side == PING_EDGE_RIGHT) {
        /* Vertical bar: slide it along y to keep both ends inside. A
         * rectangle shorter than the bar collapses it to the full height
         * rather than inverting it. */
        if (barLen >= rh) { lo = ry; hi = y1; }
        else {
            float c = ey;
            if (c < ry + half) c = ry + half;
            if (c > y1 - half) c = y1 - half;
            lo = c - half; hi = c + half;
        }
        out->x0 = ex; out->y0 = lo;
        out->x1 = ex; out->y1 = hi;
    } else {
        if (barLen >= rw) { lo = rx; hi = x1; }
        else {
            float c = ex;
            if (c < rx + half) c = rx + half;
            if (c > x1 - half) c = x1 - half;
            lo = c - half; hi = c + half;
        }
        out->x0 = lo; out->y0 = ey;
        out->x1 = hi; out->y1 = ey;
    }
    out->cx = (out->x0 + out->x1) * 0.5f;
    out->cy = (out->y0 + out->y1) * 0.5f;
    out->side = side;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_EDGE_H */
