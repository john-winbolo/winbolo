/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer small digits, drawn
 *Filename:      lv_builder_draw.h
 *Purpose:
 *  Draws the small square-cell pictures and digits of the
 *  builder marker (lv_builder_status.h) with an SDL renderer:
 *  each cell of a glyph is one filled square. The Log Viewer
 *  draws its builder marker and its pillbox numbers with
 *  these, and the game draws its pillbox and base numbers on
 *  the map with them (gui/sdl3/mapview_overlay.c), so the two
 *  look the same.
 *
 *  The caller sets the colour first. For a number with an
 *  edge, draw it twice: once in the edge colour with grow set
 *  to the edge's width, then once in white with grow 0.
 *********************************************************/

#ifndef LV_BUILDER_DRAW_H
#define LV_BUILDER_DRAW_H

#include <SDL3/SDL.h>

#include "lv_builder_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One glyph with its top left at x, y and cells cell pixels across. Each
 * cell is grow pixels bigger all round: the edge pass. */
static inline void lvBuilderDrawGlyph(SDL_Renderer *r, const LvBuilderGlyph *g, float x,
                                      float y, float cell, float grow) {
    int row, col;

    if (r == NULL || g == NULL) return;
    for (row = 0; row < g->height; row++) {
        for (col = 0; col < g->width; col++) {
            SDL_FRect f;
            if (!lvBuilderGlyphCell(g, col, row)) continue;
            f.x = x + (float)col * cell - grow;
            f.y = y + (float)row * cell - grow;
            f.w = cell + grow * 2.0f;
            f.h = cell + grow * 2.0f;
            SDL_RenderFillRect(r, &f);
        }
    }
}

/* A text of digits and the other characters lvBuilderChar has, from x, with
 * their tops at y. One empty cell goes between two characters. */
static inline void lvBuilderDrawText(SDL_Renderer *r, const char *text, float x, float y,
                                     float cell, float grow) {
    if (text == NULL) return;
    for (; *text != '\0'; text++) {
        const LvBuilderGlyph *g = lvBuilderChar(*text);
        if (g == NULL) continue;
        lvBuilderDrawGlyph(r, g, x, y, cell, grow);
        x += (float)(g->width + 1) * cell;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* LV_BUILDER_DRAW_H */
