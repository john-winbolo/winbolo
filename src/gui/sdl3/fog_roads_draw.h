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
 * Name:          fog_roads_draw.h
 * Purpose:       Puts the fog edge bands of fog_roads.h on
 *                the screen, for both views that draw them.
 *
 *   The band's colour and its three alphas are set in one
 *   place, at the top of fog_roads.h: FOG_ROAD_R/G/B,
 *   FOG_ROAD_ALPHA and FOG_ROAD_BANDS. They are there rather
 *   than here because that is the half with no SDL in it and
 *   the unit tests read them. Tune the look there.
 *
 *   A painter rather than a function per square, because the
 *   bands come in FOG_ROAD_BANDS strengths and a renderer
 *   takes one colour for a run of rectangles. The painter
 *   keeps a short queue per strength; the caller hands it one
 *   square at a time in whatever order its own walk produces,
 *   and each queue is emptied in a single fill when it is
 *   full and again at the end. So the whole pass costs three
 *   fills for a screen's worth of road, not one per band.
 *
 *   Header-only and static, so neither view has to link a new
 *   object and the classic view can include it from C while
 *   the full screen map includes it from C++.
 *********************************************************/

#ifndef FOG_ROADS_DRAW_H
#define FOG_ROADS_DRAW_H

#include <SDL3/SDL.h>

#include "fog_roads.h"

/* Rectangles held back per strength before a fill is forced. One square can
 * add four, so this is sixty-four squares of the worst case; a screen of the
 * classic view is under three hundred squares altogether. */
#define FOG_ROAD_QUEUE 256

typedef struct FogRoadPainter {
    SDL_Renderer *r;
    SDL_BlendMode wasBlend;
    SDL_FRect     queue[FOG_ROAD_BANDS][FOG_ROAD_QUEUE];
    int           count[FOG_ROAD_BANDS];
} FogRoadPainter;

/* Empties one strength's queue. The colour is set per fill rather than once
 * per pass because the three strengths interleave as the walk goes. */
static inline void fogRoadPainterFlush(FogRoadPainter *p, int band) {
    if (p->count[band] <= 0) return;
    SDL_SetRenderDrawColor(p->r, FOG_ROAD_R, FOG_ROAD_G, FOG_ROAD_B,
                           fogRoadBandAlpha(band));
    SDL_RenderFillRects(p->r, p->queue[band], p->count[band]);
    p->count[band] = 0;
}

/* The blend mode is taken and put back the way the fog wash takes it: the
 * callers that draw rectangles after this one set the colour they want but
 * not always the mode, and one of them runs with blending off. */
static inline void fogRoadPainterBegin(FogRoadPainter *p, SDL_Renderer *r) {
    int i; /* Looping variable */
    p->r = r;
    p->wasBlend = SDL_BLENDMODE_NONE;
    for (i = 0; i < FOG_ROAD_BANDS; i++) p->count[i] = 0;
    SDL_GetRenderDrawBlendMode(r, &p->wasBlend);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

/* One square's bands. `edges` is what fogRoadEdges answered for it and x, y,
 * w, h is where the square is on the screen. A square with no edges costs a
 * compare and nothing else, which is most of them.
 *
 * The band is a share of the square rather than a fixed three pixels, so the
 * outline keeps its weight as the player zooms: at the classic 16-pixel tile
 * one band is one pixel and the three together are the three the look is
 * named for. Never thinner than a pixel, or a zoomed-out map would ask for
 * rectangles too thin to land on anything.
 *
 * The bands of two edges overlap at a corner, which leaves the corner a
 * little stronger than the sides. That is what a corner of a road looks
 * like, so it is left alone. */
static inline void fogRoadPainterSquare(FogRoadPainter *p, unsigned char edges,
                                        float x, float y, float w, float h) {
    int band; /* Looping variable */
    float t;  /* One band's thickness */

    if (edges == 0) return;

    t = w / (float)FOG_ROAD_CLASSIC_TILE;
    if (t < 1.0f) t = 1.0f;
    /* A square too small to hold the whole fade thins its bands rather than
     * banding over its far side: the outline of a road has to leave road
     * between its two sides or it is a block. A third of the square is kept
     * whatever the zoom, which at the classic tile size and above costs
     * nothing — three one-pixel bands out of sixteen are well inside it. */
    {
        float widest = w / (3.0f * (float)FOG_ROAD_BANDS);
        if (t > widest) t = widest;
    }
    if (t <= 0.0f) return;

    for (band = 0; band < FOG_ROAD_BANDS; band++) {
        float off = t * (float)band;
        SDL_FRect *slot;

        if (p->count[band] + 4 > FOG_ROAD_QUEUE) fogRoadPainterFlush(p, band);

        if ((edges & FOG_ROAD_EDGE_LEFT) != 0) {
            slot = &p->queue[band][p->count[band]++];
            slot->x = x + off; slot->y = y; slot->w = t; slot->h = h;
        }
        if ((edges & FOG_ROAD_EDGE_RIGHT) != 0) {
            slot = &p->queue[band][p->count[band]++];
            slot->x = x + w - off - t; slot->y = y; slot->w = t; slot->h = h;
        }
        if ((edges & FOG_ROAD_EDGE_TOP) != 0) {
            slot = &p->queue[band][p->count[band]++];
            slot->x = x; slot->y = y + off; slot->w = w; slot->h = t;
        }
        if ((edges & FOG_ROAD_EDGE_BOTTOM) != 0) {
            slot = &p->queue[band][p->count[band]++];
            slot->x = x; slot->y = y + h - off - t; slot->w = w; slot->h = t;
        }
    }
}

/* Everything still queued, then the blend mode back. */
static inline void fogRoadPainterEnd(FogRoadPainter *p) {
    int i; /* Looping variable */
    for (i = 0; i < FOG_ROAD_BANDS; i++) fogRoadPainterFlush(p, i);
    SDL_SetRenderDrawBlendMode(p->r, p->wasBlend);
}

#endif /* FOG_ROADS_DRAW_H */
