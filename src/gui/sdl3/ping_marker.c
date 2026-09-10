/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          ping_marker.c
 * Purpose:       See ping_marker.h.
 *********************************************************/

#include "ping_marker.h"

#include <math.h>

#include "../ping_kinds.h"
#include "ping_icons.h"

/* MSVC only defines M_PI under _USE_MATH_DEFINES. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* SDL_Renderer has no circle primitive, so both of these fan out a polygon.
 * 32 segments is smooth at every zoom the view runs at and cheap enough to
 * redraw per ping per frame. */
#define PM_CIRCLE_SEGMENTS 32

static Uint8 pmAlpha(float a) {
    if (a <= 0.0f) return 0;
    if (a >= 1.0f) return 255;
    return (Uint8)(a * 255.0f + 0.5f);
}

static void pmFillCircle(SDL_Renderer *renderer, float cx, float cy, float r,
                         Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    SDL_Vertex verts[PM_CIRCLE_SEGMENTS + 2];
    int indices[PM_CIRCLE_SEGMENTS * 3];
    SDL_FColor col;
    int i;

    if (r <= 0.5f || ca == 0) return;
    col.r = cr / 255.0f; col.g = cg / 255.0f; col.b = cb / 255.0f;
    col.a = ca / 255.0f;

    verts[0].position.x = cx;
    verts[0].position.y = cy;
    verts[0].color = col;
    verts[0].tex_coord.x = verts[0].tex_coord.y = 0.0f;
    for (i = 0; i <= PM_CIRCLE_SEGMENTS; i++) {
        float a = (float)(2.0 * M_PI) * (float)i / (float)PM_CIRCLE_SEGMENTS;
        verts[i + 1].position.x = cx + cosf(a) * r;
        verts[i + 1].position.y = cy + sinf(a) * r;
        verts[i + 1].color = col;
        verts[i + 1].tex_coord.x = verts[i + 1].tex_coord.y = 0.0f;
    }
    for (i = 0; i < PM_CIRCLE_SEGMENTS; i++) {
        indices[i * 3 + 0] = 0;
        indices[i * 3 + 1] = i + 1;
        indices[i * 3 + 2] = i + 2;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, NULL, verts, PM_CIRCLE_SEGMENTS + 2,
                       indices, PM_CIRCLE_SEGMENTS * 3);
}

static void pmStrokeCircle(SDL_Renderer *renderer, float cx, float cy, float r,
                           Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    SDL_FPoint pts[PM_CIRCLE_SEGMENTS + 1];
    int i;
    if (r <= 0.5f || ca == 0) return;
    for (i = 0; i <= PM_CIRCLE_SEGMENTS; i++) {
        float a = (float)(2.0 * M_PI) * (float)i / (float)PM_CIRCLE_SEGMENTS;
        pts[i].x = cx + cosf(a) * r;
        pts[i].y = cy + sinf(a) * r;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
    SDL_RenderLines(renderer, pts, PM_CIRCLE_SEGMENTS + 1);
}

/* A rectangle outline `line` pixels wide, as nested 1 px rects. */
static void pmStrokeRect(SDL_Renderer *renderer, float x0, float y0,
                         float x1, float y1, int line,
                         Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    int i;
    if (ca == 0) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
    for (i = 0; i < line; i++) {
        SDL_FRect r = { x0 + (float)i, y0 + (float)i,
                        (x1 - x0) - 2.0f * (float)i, (y1 - y0) - 2.0f * (float)i };
        if (r.w <= 0.0f || r.h <= 0.0f) break;
        SDL_RenderRect(renderer, &r);
    }
}

void pingMarkerDraw(SDL_Renderer *renderer, unsigned char kind,
                    float cx, float cy, float tileW, float tileH,
                    unsigned int ageMs, float alpha) {
    const PingKindStyle *style = pingKindStyle(kind);
    float hx = tileW * 0.5f, hy = tileH * 0.5f;
    int   line = (tileW >= 24.0f) ? 2 : 1;
    float a = alpha * PING_MARKER_ALPHA;
    SDL_Texture *tex;

    if (renderer == NULL || alpha <= 0.0f) return;

    /* The pulse: a ring that grows out of the square and fades over a second,
       then starts again, so the eye is drawn to it without it ever being
       solid. */
    {
        float pulse = (float)(ageMs % 1000u) / 1000.0f;
        float ringR = tileW * (0.55f + 0.45f * pulse);
        pmStrokeCircle(renderer, cx, cy, ringR, style->r, style->g, style->b,
                       pmAlpha(a * (1.0f - pulse)));
    }

    /* The tile: a dark rim just outside the coloured one so the outline reads
       on any terrain, and the coloured one a touch stronger than the icon so
       the square is the thing the marker names. */
    pmStrokeRect(renderer, cx - hx - (float)line, cy - hy - (float)line,
                 cx + hx + (float)line, cy + hy + (float)line, line,
                 0, 0, 0, pmAlpha(alpha * 0.55f));
    pmStrokeRect(renderer, cx - hx, cy - hy, cx + hx, cy + hy, line,
                 style->r, style->g, style->b, pmAlpha(alpha * 0.85f));

    /* The icon, over a faint dark disc so a pale colour still reads on sand
       or snow. Both see-through: this is a hint on the ground, not a sprite. */
    pmFillCircle(renderer, cx, cy, tileW * 0.45f, 0, 0, 0, pmAlpha(a * 0.5f));
    tex = pingIconTexture(kind);
    /* The icons belong to one renderer at a time (ping_icons.c), and the map
       overview's pop-out draws through a renderer of its own. A texture from
       another renderer would simply fail to draw, so it is dropped here and
       the marker falls back to the coloured dot below — the square, the rim
       and the pulse are all still that renderer's own geometry. */
    if (tex != NULL && SDL_GetRendererFromTexture(tex) != renderer) {
        tex = NULL;
    }
    if (tex) {
        float iconPx = tileW * 0.8f;
        SDL_FRect dst = { cx - iconPx * 0.5f, cy - iconPx * 0.5f, iconPx, iconPx };
        SDL_SetTextureColorMod(tex, style->r, style->g, style->b);
        SDL_SetTextureAlphaMod(tex, pmAlpha(a));
        SDL_RenderTexture(renderer, tex, NULL, &dst);
        SDL_SetTextureAlphaMod(tex, 255);
    } else {
        pmFillCircle(renderer, cx, cy, tileW * 0.2f,
                     style->r, style->g, style->b, pmAlpha(a));
    }
}
