/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          ring_band.c
 * Purpose:       See ring_band.h.
 *********************************************************/

#include "ring_band.h"

int ringBandSegments(float radiusPx) {
    int segments = (int)(2.0f * SDL_PI_F * radiusPx / RING_BAND_SIDE_PX);
    if (segments < RING_BAND_MIN_SEG) {
        return RING_BAND_MIN_SEG;
    }
    if (segments > RING_BAND_MAX_SEG) {
        return RING_BAND_MAX_SEG;
    }
    return segments;
}

void ringBandDraw(SDL_Renderer *r, float cx, float cy,
                  float innerPx, float outerPx, int segments,
                  Uint8 red, Uint8 green, Uint8 blue,
                  Uint8 innerAlpha, Uint8 outerAlpha) {
    SDL_Vertex verts[(RING_BAND_MAX_SEG + 1) * 2];
    int        indices[RING_BAND_MAX_SEG * 6];
    SDL_FColor colourIn;
    SDL_FColor colourOut;
    int        s;

    if (r == NULL) return;
    if (innerPx < 0.0f) innerPx = 0.0f;
    if (outerPx <= innerPx || segments < 3) return;
    /* ringBandSegments already clamps, so this only catches a caller that
     * counted its own sides. Clamped rather than refused: a ring a little
     * flatter than asked for beats no ring, and beats running off the arrays
     * above. */
    if (segments > RING_BAND_MAX_SEG) segments = RING_BAND_MAX_SEG;

    colourIn.r = (float)red   / 255.0f;
    colourIn.g = (float)green / 255.0f;
    colourIn.b = (float)blue  / 255.0f;
    colourIn.a = (float)innerAlpha / 255.0f;
    colourOut.r = colourIn.r;
    colourOut.g = colourIn.g;
    colourOut.b = colourIn.b;
    colourOut.a = (float)outerAlpha / 255.0f;

    /* Inner and outer vertex per step round the circle, the pair adjacent so
     * a segment's four corners are four consecutive entries. */
    for (s = 0; s <= segments; s++) {
        float a  = (float)s * (2.0f * SDL_PI_F / (float)segments);
        float dx = SDL_cosf(a);
        float dy = SDL_sinf(a);
        SDL_Vertex *vi = &verts[s * 2];
        SDL_Vertex *vo = &verts[s * 2 + 1];

        vi->position.x = cx + dx * innerPx;
        vi->position.y = cy + dy * innerPx;
        vo->position.x = cx + dx * outerPx;
        vo->position.y = cy + dy * outerPx;
        vi->color = colourIn;
        vo->color = colourOut;
        vi->tex_coord.x = 0.0f;
        vi->tex_coord.y = 0.0f;
        vo->tex_coord.x = 0.0f;
        vo->tex_coord.y = 0.0f;
    }

    /* Two triangles a segment, between this step's pair and the next one's. */
    for (s = 0; s < segments; s++) {
        int i0 = s * 2;
        indices[s * 6 + 0] = i0;
        indices[s * 6 + 1] = i0 + 1;
        indices[s * 6 + 2] = i0 + 2;
        indices[s * 6 + 3] = i0 + 1;
        indices[s * 6 + 4] = i0 + 3;
        indices[s * 6 + 5] = i0 + 2;
    }

    SDL_RenderGeometry(r, NULL, verts, (segments + 1) * 2,
                       indices, segments * 6);
}

bool ringAnimAt(const RingAnim *anim, Uint64 elapsedMs,
                float *radius, float *alpha) {
    float outRadius;
    float outAlpha;

    if (anim == NULL) return false;
    if (elapsedMs >= (Uint64)anim->closeMs + (Uint64)anim->pulseMs) return false;

    if (elapsedMs < (Uint64)anim->closeMs) {
        /* The close, eased out: away quickly and settling onto the target,
         * rather than arriving at the speed it left. */
        float t = (float)elapsedMs / (float)anim->closeMs;
        float u = 1.0f - t;
        float e = 1.0f - u * u * u;
        outRadius = anim->startRadius +
                    (anim->endRadius - anim->startRadius) * e;
        outAlpha  = 1.0f;
    } else {
        /* The pulse: out and back on a half sine, so it opens and shuts
         * without a corner at the top, and fades as it goes. */
        float t = (float)(elapsedMs - (Uint64)anim->closeMs) /
                  (float)anim->pulseMs;
        float s = SDL_sinf(t * SDL_PI_F);
        outRadius = anim->endRadius +
                    (anim->pulseRadius - anim->endRadius) * s;
        outAlpha  = 1.0f - t;
    }

    if (radius != NULL) *radius = outRadius;
    if (alpha  != NULL) *alpha  = outAlpha;
    return true;
}
