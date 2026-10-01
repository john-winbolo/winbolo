/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Name:          map_markers.c
 * Purpose:       See map_markers.h.
 */

#include "map_markers.h"

#include "map_colours.h"
#include "ring_band.h"

/* The triangle, as fractions of the radius: the nose ahead of the centre, the
   two back corners behind and to either side. Wider than it is tall would
   read as an arrowhead rather than a tank, and narrower loses its heading at
   the sizes this is drawn at. */
#define TANK_NOSE   1.2f
#define TANK_HALF   0.85f
#define TANK_TAIL   1.0f

/* A filled disc, as ring_band.h's band with no hole. */
static void fillDisc(SDL_Renderer *r, float cx, float cy, float radius,
                     SDL_FColor c) {
    Uint8 alpha = (Uint8)(c.a * 255.0f + 0.5f);
    ringBandDraw(r, cx, cy, 0.0f, radius, ringBandSegments(radius),
                 (Uint8)(c.r * 255.0f + 0.5f),
                 (Uint8)(c.g * 255.0f + 0.5f),
                 (Uint8)(c.b * 255.0f + 0.5f), alpha, alpha);
}

void mapMarkerTank(SDL_Renderer *r, float cx, float cy, float radius,
                   int facing16, SDL_FColor fill) {
    SDL_FPoint points[3];
    SDL_FColor shades[3];
    float      turn, cosine, sine, inradius, incentreY;
    int        layer, i;

    if (!r || radius <= 0.0f) return;

    /* Wrapped rather than clamped, so a caller may hand in a raw sprite frame
       whose allegiance offset is still on it. */
    facing16 %= 16;
    if (facing16 < 0) facing16 += 16;

    turn   = (float)facing16 * (2.0f * SDL_PI_F) / 16.0f;
    cosine = SDL_cosf(turn);
    sine   = SDL_sinf(turn);

    points[0].x = 0.0f;               points[0].y = -radius * TANK_NOSE;
    points[1].x =  radius * TANK_HALF; points[1].y =  radius * TANK_TAIL;
    points[2].x = -radius * TANK_HALF; points[2].y =  radius * TANK_TAIL;

    /* Scaling about the incentre — the centre of the inscribed circle, which
       is the one point every edge is the same distance from — offsets all
       three edges equally, so the stroke keeps an even width round the
       triangle. Scaling about the centroid instead would push the sharp nose
       out further than the flat base and thicken the tip. */
    inradius  = radius * (TANK_HALF * (TANK_NOSE + TANK_TAIL)) /
                (TANK_HALF + SDL_sqrtf(TANK_HALF * TANK_HALF +
                                       (TANK_NOSE + TANK_TAIL) *
                                       (TANK_NOSE + TANK_TAIL)));
    incentreY = radius * TANK_TAIL - inradius;

    mapColourMarkerShades(fill, shades);
    for (layer = 0; layer < 3; layer++) {
        float      factor = 1.0f + mapColourMarkerLayerGrow(layer) / inradius;
        SDL_Vertex vertices[3];
        for (i = 0; i < 3; i++) {
            float x = points[i].x * factor;
            float y = incentreY + (points[i].y - incentreY) * factor;
            vertices[i].position.x = cx + x * cosine - y * sine;
            vertices[i].position.y = cy + x * sine   + y * cosine;
            vertices[i].color      = shades[layer];
            vertices[i].tex_coord.x = 0.0f;
            vertices[i].tex_coord.y = 0.0f;
        }
        SDL_RenderGeometry(r, NULL, vertices, 3, NULL, 0);
    }
}

void mapMarkerPill(SDL_Renderer *r, float cx, float cy, float radius,
                   SDL_FColor fill) {
    SDL_FColor shades[3];
    int        layer;

    if (!r || radius <= 0.0f) return;

    mapColourMarkerShades(fill, shades);
    for (layer = 0; layer < 3; layer++) {
        fillDisc(r, cx, cy, radius + mapColourMarkerLayerGrow(layer),
                 shades[layer]);
    }
}

void mapMarkerBase(SDL_Renderer *r, float cx, float cy, float radius,
                   SDL_FColor fill) {
    SDL_FColor shades[3];
    int        layer;

    if (!r || radius <= 0.0f) return;

    mapColourMarkerShades(fill, shades);
    for (layer = 0; layer < 3; layer++) {
        float     size = radius + mapColourMarkerLayerGrow(layer);
        SDL_FRect rect = { cx - size, cy - size, size * 2.0f, size * 2.0f };
        SDL_SetRenderDrawColorFloat(r, shades[layer].r, shades[layer].g,
                                    shades[layer].b, shades[layer].a);
        SDL_RenderFillRect(r, &rect);
    }
}
