/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A scenario's map marker, drawn with the SDL renderer.
 *
 * Everything about how it looks is decided here: the outline, the pointer
 * above it, the breathing and the colour's alpha. The callers hand in where
 * the square is and how big it is in their own pixels, and nothing else.
 * That is the rule ping_marker.c already keeps, and for the same reason —
 * two views draw this and they have to agree.
 */

#include "scenario_marker.h"

#include <math.h>

#include "scenario_panel_draw.h" /* scnPanelColourRGBA — the one palette */

/* How much of the marker's colour shows at the top and the bottom of its
   breath. It is a mark on the ground, like a ping: the terrain reads through
   it and anything drawn after it wins. */
#define SCN_MARKER_ALPHA_LOW  0.45f
#define SCN_MARKER_ALPHA_HIGH 0.95f

/* One breath, in milliseconds. Slow enough to read as alive rather than as a
   blink asking to be looked at — a scenario's mark can sit on the map for a
   whole round, and a fast pulse over that long is an irritation. */
#define SCN_MARKER_BREATH_MS 1600.0f

/* The outline's thickness, and the pointer's height and half-width, as
   fractions of a map square. The pointer sits above the square with a gap of
   its own so the two read as separate marks. */
#define SCN_MARKER_STROKE    0.10f
#define SCN_MARKER_POINT_H   0.38f
#define SCN_MARKER_POINT_W   0.30f
#define SCN_MARKER_POINT_GAP 0.12f

/* Where in its breath the marker is, 0 to 1 and back. */
static float scnMarkerBreath(unsigned int nowMs) {
    float phase = fmodf((float)nowMs, SCN_MARKER_BREATH_MS) /
                  SCN_MARKER_BREATH_MS;
    /* A cosine rather than a triangle: the turn at each end is what makes it
       breathe instead of tick. */
    return 0.5f - 0.5f * cosf(phase * 6.2831853f);
}

/* A rectangle outline of a real thickness, drawn as nested one-pixel rects
   the way the ping marker's does — SDL has no stroke width of its own. */
static void scnMarkerStrokeRect(SDL_Renderer *renderer, float x0, float y0,
                                float x1, float y1, float thickness,
                                Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    int steps = (int)(thickness + 0.5f);
    int i;

    if (steps < 1) steps = 1;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
    for (i = 0; i < steps; i++) {
        SDL_FRect r;
        r.x = x0 + (float)i;
        r.y = y0 + (float)i;
        r.w = (x1 - x0) - (float)(2 * i);
        r.h = (y1 - y0) - (float)(2 * i);
        if (r.w <= 0.0f || r.h <= 0.0f) break;
        SDL_RenderRect(renderer, &r);
    }
}

/* The pointer above the square: a solid triangle with its tip down. Filled
   by scanning rows, which is what SDL_RenderLine gives without pulling in a
   geometry call for eight pixels of triangle. */
static void scnMarkerFillPointer(SDL_Renderer *renderer, float cx, float tipY,
                                 float height, float halfWidth,
                                 Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    int rows = (int)(height + 0.5f);
    int i;

    if (rows < 1) rows = 1;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
    for (i = 0; i < rows; i++) {
        /* Widest at the top, closing to the tip at the bottom. */
        float t = (float)i / (float)rows;
        float w = halfWidth * (1.0f - t);
        float y = tipY - height + (float)i;
        SDL_RenderLine(renderer, cx - w, y, cx + w, y);
    }
}

bool scnMarkerTankSquare(const screenTanks *tks, uint8_t slot,
                         uint8_t *outMx, uint8_t *outMy) {
    BYTE i;

    if (tks == NULL) return false;
    for (i = 0; i < tks->numTanksScreen && i < MAX_TANKS; i++) {
        if (tks->pos[i].playerNum != (BYTE)slot) continue;
        if (outMx) *outMx = (uint8_t)tks->pos[i].mx;
        if (outMy) *outMy = (uint8_t)tks->pos[i].my;
        return true;
    }
    return false;
}

void scnMarkerDraw(SDL_Renderer *renderer, uint8_t colour,
                   float cx, float cy, float tileW, float tileH,
                   unsigned int nowMs) {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    float   breath, alpha;
    Uint8   ca;
    float   halfW, halfH, stroke;

    if (renderer == NULL || tileW <= 0.0f || tileH <= 0.0f) return;
    /* Index 0 and the four reserved entries draw nothing here for the same
       reason they draw nothing on the panel: the palette says so. */
    if (!scnPanelColourRGBA(colour, &r, &g, &b, &a)) return;

    breath = scnMarkerBreath(nowMs);
    alpha  = SCN_MARKER_ALPHA_LOW +
             (SCN_MARKER_ALPHA_HIGH - SCN_MARKER_ALPHA_LOW) * breath;
    ca = (Uint8)((float)a * alpha);

    halfW  = tileW * 0.5f;
    halfH  = tileH * 0.5f;
    stroke = tileH * SCN_MARKER_STROKE;

    scnMarkerStrokeRect(renderer, cx - halfW, cy - halfH, cx + halfW,
                        cy + halfH, stroke, r, g, b, ca);
    scnMarkerFillPointer(renderer, cx, cy - halfH - tileH * SCN_MARKER_POINT_GAP,
                         tileH * SCN_MARKER_POINT_H,
                         tileW * SCN_MARKER_POINT_W * 0.5f, r, g, b, ca);
}
