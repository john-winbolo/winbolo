/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Ping Edge
 *Filename:      ping_edge.h
 *Purpose:
 *  Where the marker for an off-screen ping goes: the point
 *  at which the straight line from the viewer's tank to the
 *  ping leaves the game rectangle, the bar laid along that
 *  border so it reads as "over there", how big that bar is
 *  for a ping that far away, and where the sender's name sits
 *  beside it.
 *
 *  Split out with no SDL and no ImGui so the four edges, the
 *  corner and the on-screen case can be tested directly.
 *
 *  The sizes the callers feed in (bar length, icon size, the
 *  air between them) are knobs in src/gui/ping_kinds.h, which
 *  this header deliberately does not include: everything here
 *  is a pure function of its arguments, so a test can drive it
 *  with the real knobs or with round numbers of its own.
 *
 *  Screen axes: x right, y DOWN.
 *********************************************************/

#ifndef WINBOLO_PING_EDGE_H
#define WINBOLO_PING_EDGE_H

#include <math.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A screen rectangle, for the callers that pass one around rather than four
 * loose floats. */
typedef struct {
    float x, y, w, h;
} PingRect;

/* The part of a rectangle that nothing is drawn over: the rect less the strip
 * each edge is covered by. The map overview keeps exactly those four numbers
 * (OverviewCamera::insetL/T/R/B — the status column, the build strip and the
 * newswire in full screen), so an edge marker laid along the result sits on
 * map the player can see instead of under a panel.
 *
 * A negative inset is read as none. Insets that would leave nothing writes the
 * rectangle back unshrunk and returns false: a bar somewhere is better than no
 * bar at all, and the caller can tell the difference if it cares. A degenerate
 * rectangle on the way in is refused the same way. */
static inline bool pingRectInset(float x, float y, float w, float h,
                                 float insetL, float insetT,
                                 float insetR, float insetB,
                                 PingRect *out) {
    float nx, ny, nw, nh;
    if (out == NULL) return false;
    out->x = x; out->y = y; out->w = w; out->h = h;
    if (w <= 0.0f || h <= 0.0f) return false;
    if (insetL < 0.0f) insetL = 0.0f;
    if (insetT < 0.0f) insetT = 0.0f;
    if (insetR < 0.0f) insetR = 0.0f;
    if (insetB < 0.0f) insetB = 0.0f;
    nx = x + insetL;
    ny = y + insetT;
    nw = w - insetL - insetR;
    nh = h - insetT - insetB;
    if (nw <= 0.0f || nh <= 0.0f) return false;
    out->x = nx; out->y = ny; out->w = nw; out->h = nh;
    return true;
}

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

/* How big the off-screen marker for a ping `distTiles` map squares away
 * should be, as 0..1 — 1 at nearTiles or closer, 0 at farTiles or beyond.
 *
 * Not a straight ramp: the fraction follows 1 - sqrt(u) of the way between
 * the two distances, so the bar loses most of its length in the first stretch
 * past the near distance and then flattens out. A ping twenty squares off and
 * one forty squares off are both "a long way", and what the player wants told
 * apart is the teammate at the edge of the view from the one across the map.
 *
 * A near/far pair the wrong way round or equal returns 1 rather than dividing
 * by nothing: a marker at full size is a worse look than no marker, not a
 * crash. */
static inline float pingEdgeSizeFactor(float distTiles, float nearTiles,
                                       float farTiles) {
    float u;
    if (!(farTiles > nearTiles)) return 1.0f;
    if (distTiles <= nearTiles) return 1.0f;
    if (distTiles >= farTiles)  return 0.0f;
    u = (distTiles - nearTiles) / (farTiles - nearTiles);
    return 1.0f - sqrtf(u);
}

/* A size in pixels for that fraction: minPx at 0, maxPx at 1. Kept separate
 * from the fraction so the bar and the icon on it shrink together off one
 * distance instead of each deriving their own. */
static inline float pingEdgeSizeFor(float factor, float minPx, float maxPx) {
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    return minPx + (maxPx - minPx) * factor;
}

/* Where the icon on an edge bar goes: the bar's midpoint stepped in off the
 * border, far enough that the icon clears the bar itself — half the bar's
 * thickness, half the icon, and `gapPx` of air between the two. */
static inline void pingEdgeIconCentre(const PingEdgeMarker *m, float thickness,
                                      float iconPx, float gapPx,
                                      float *outX, float *outY) {
    float step = thickness * 0.5f + iconPx * 0.5f + gapPx;
    float x, y;
    if (m == NULL) return;
    x = m->cx;
    y = m->cy;
    if (m->side == PING_EDGE_LEFT)   x += step;
    if (m->side == PING_EDGE_RIGHT)  x -= step;
    if (m->side == PING_EDGE_TOP)    y += step;
    if (m->side == PING_EDGE_BOTTOM) y -= step;
    if (outX) *outX = x;
    if (outY) *outY = y;
}

/* The top-left corner of the sender's name beside an edge marker. */
typedef struct {
    float x, y;
    /* The rectangle moved the box from where the side asked for it — a bar
     * near a corner, or a name wider than the room beside it. Reported for a
     * caller that wants to know; the position is usable either way. */
    bool  clamped;
} PingEdgeNameBox;

/* Place a name of `textW` x `textH` against the icon at (iconCx, iconCy),
 * always on the inside of the rectangle:
 *
 *   top border    - under the icon, centred on it
 *   bottom border - above the icon, centred on it
 *   left border   - to the right of the icon, on its centre line
 *   right border  - to the left of the icon, on its centre line
 *
 * so the text always runs away from the border rather than off it. It is then
 * clamped into the rectangle, which is what a bar in a corner needs: the bar
 * itself is already slid flush into the corner, and a name centred under it
 * would hang past the end.
 *
 * The name is the point of the whole indicator — knowing WHO is on their way
 * without finding them on the map — so it is not scaled with the distance the
 * bar is: the caller passes the size it renders at and gets it placed.
 *
 * Returns false, with nothing written, for a degenerate rectangle or no out;
 * the position is still written (clamped as best it can be) when the text is
 * simply wider or taller than the rectangle. */
static inline bool pingEdgeNameAnchor(PingEdgeSide side,
                                      float iconCx, float iconCy,
                                      float iconPx, float gapPx,
                                      float textW, float textH,
                                      float rx, float ry, float rw, float rh,
                                      PingEdgeNameBox *out) {
    float x, y, loX, hiX, loY, hiY;
    float step = iconPx * 0.5f + gapPx;

    if (out == NULL || rw <= 0.0f || rh <= 0.0f) return false;

    switch (side) {
    case PING_EDGE_TOP:
        x = iconCx - textW * 0.5f;
        y = iconCy + step;
        break;
    case PING_EDGE_BOTTOM:
        x = iconCx - textW * 0.5f;
        y = iconCy - step - textH;
        break;
    case PING_EDGE_RIGHT:
        x = iconCx - step - textW;
        y = iconCy - textH * 0.5f;
        break;
    default:   /* PING_EDGE_LEFT */
        x = iconCx + step;
        y = iconCy - textH * 0.5f;
        break;
    }

    loX = rx;
    hiX = rx + rw - textW;
    loY = ry;
    hiY = ry + rh - textH;
    out->clamped = false;
    /* hi below lo means the text does not fit that way at all; pin it to the
     * near corner so at least its start is readable. */
    if (x > hiX) { x = hiX; out->clamped = true; }
    if (x < loX) { x = loX; out->clamped = true; }
    if (y > hiY) { y = hiY; out->clamped = true; }
    if (y < loY) { y = loY; out->clamped = true; }
    out->x = x;
    out->y = y;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_EDGE_H */
