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
 *  lvBuilderDrawNumbers does both passes for a list of
 *  numbers, with one fill call a pass.
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

/* A row of numbers drawn with one fill call a pass, for the game's pillbox
 * and base numbers and the Log Viewer's pillbox numbers. Each number is its
 * text and the top left and cell size from lvPillLabelLayout. */
#define LV_BUILDER_NUMBER_TEXT_MAX 8

typedef struct LvBuilderNumber {
    char  text[LV_BUILDER_NUMBER_TEXT_MAX];
    float x;
    float y;
    float cell;
} LvBuilderNumber;

/* How many cells the glyphs of a number's text have: the squares
 * lvBuilderNumberRects writes for it. */
static inline int lvBuilderNumberCells(const LvBuilderNumber *num) {
    const char *text;
    const char *end;
    int n = 0;

    if (num == NULL) return 0;
    end = num->text + LV_BUILDER_NUMBER_TEXT_MAX;
    for (text = num->text; text < end && *text != '\0'; text++) {
        const LvBuilderGlyph *g = lvBuilderChar(*text);
        int row, col;
        if (g == NULL) continue;
        for (row = 0; row < g->height; row++) {
            for (col = 0; col < g->width; col++) {
                if (lvBuilderGlyphCell(g, col, row)) n++;
            }
        }
    }
    return n;
}

/* The squares of one number's cells, in the order lvBuilderDrawText fills
 * them: character by character, then row by row, then column by column.
 * Each cell is grow pixels bigger all round. Writes at most cap squares and
 * returns how many it wrote. */
static inline int lvBuilderNumberRects(const LvBuilderNumber *num, float grow, SDL_FRect *out,
                                       int cap) {
    const char *text;
    const char *end;
    float x;
    int n = 0;

    if (num == NULL || out == NULL || cap <= 0) return 0;
    x = num->x;
    end = num->text + LV_BUILDER_NUMBER_TEXT_MAX;
    for (text = num->text; text < end && *text != '\0'; text++) {
        const LvBuilderGlyph *g = lvBuilderChar(*text);
        int row, col;
        if (g == NULL) continue;
        for (row = 0; row < g->height; row++) {
            for (col = 0; col < g->width; col++) {
                SDL_FRect *f;
                if (!lvBuilderGlyphCell(g, col, row)) continue;
                if (n >= cap) return n;
                f = &out[n++];
                f->x = x + (float)col * num->cell - grow;
                f->y = num->y + (float)row * num->cell - grow;
                f->w = num->cell + grow * 2.0f;
                f->h = num->cell + grow * 2.0f;
            }
        }
        x += (float)(g->width + 1) * num->cell;
    }
    return n;
}

/* The squares a pass holds before it is sent: room for every pillbox and
 * base number of a full map in one fill call. A number never has more than
 * (LV_BUILDER_NUMBER_TEXT_MAX - 1) full glyphs, which is less than this. */
#define LV_BUILDER_PASS_RECTS 1024

/* The squares of the numbers from *next on, as many whole numbers as fit in
 * cap, in the order lvBuilderDrawText fills them. Moves *next past the
 * numbers it took and returns how many squares it wrote. */
static inline int lvBuilderNumbersRects(const LvBuilderNumber *nums, int count, int *next,
                                        float grow, SDL_FRect *out, int cap) {
    int n = 0;

    if (nums == NULL || next == NULL || out == NULL) return 0;
    while (*next < count) {
        int k = lvBuilderNumberCells(&nums[*next]);
        if (n + k > cap) break;
        n += lvBuilderNumberRects(&nums[*next], grow, &out[n], cap - n);
        (*next)++;
    }
    return n;
}

/* The numbers in white over a black edge edge pixels wide (lvBuilderEdgeColour,
 * lvBuilderMarkColour): every edge first and every white after, so the edge
 * of one number never covers the digits of a number beside it. One fill call
 * a pass. The renderer's blend mode and colour are put back after. The
 * caller sets the clip. */
static inline void lvBuilderDrawNumbers(SDL_Renderer *r, const LvBuilderNumber *nums, int count,
                                        float edge) {
    SDL_FRect     rects[LV_BUILDER_PASS_RECTS];
    SDL_BlendMode oldBlend = SDL_BLENDMODE_NONE;
    Uint8         oldR, oldG, oldB, oldA;
    int           pass;

    if (r == NULL || nums == NULL || count <= 0) return;
    SDL_GetRenderDrawBlendMode(r, &oldBlend);
    SDL_GetRenderDrawColor(r, &oldR, &oldG, &oldB, &oldA);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (pass = 0; pass < 2; pass++) {
        LvBuilderColour c = (pass == 0) ? lvBuilderEdgeColour() : lvBuilderMarkColour(false, 0);
        float grow = (pass == 0) ? edge : 0.0f;
        int next = 0;

        SDL_SetRenderDrawColorFloat(r, c.r, c.g, c.b, c.a);
        while (next < count) {
            int before = next;
            int n = lvBuilderNumbersRects(nums, count, &next, grow, rects, LV_BUILDER_PASS_RECTS);
            if (n > 0) SDL_RenderFillRects(r, rects, n);
            if (next == before) break; /* never: one number always fits */
        }
    }
    SDL_SetRenderDrawColor(r, oldR, oldG, oldB, oldA);
    SDL_SetRenderDrawBlendMode(r, oldBlend);
}

#ifdef __cplusplus
}
#endif

#endif /* LV_BUILDER_DRAW_H */
