/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The number of each pillbox the log viewer shows on its map and in its
 * tooltips (lv_pill_numbers.h).
 *
 *   lv_pill_numbers_sums — the number of a stored pillbox, which pillboxes
 *       have a number on the map, the view square a pillbox stands in, the
 *       map square under the mouse, and a number as text.
 *   lv_pill_numbers_list — a list of numbers as bits and as text, cut short
 *       when there is no room.
 *   lv_pill_numbers_layout — where the number goes beside its pillbox at
 *       small, usual and large zooms and on a display of twice the density.
 *   lv_pill_numbers_batched — the pass that draws a list of numbers with
 *       one fill call a pass (lv_builder_draw.h) gives the same squares, in
 *       the same order, and the same pixels as one call for each square.
 *   lv_pill_numbers_overview — the Overview panel notes the number of each
 *       pillbox in the row it counts it in (lv_overview.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "lv_builder_draw.h"
#include "lv_overview.h"
#include "lv_pill_numbers.h"
#include "test_harness.h"

static bool pn_close(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.001f;
}

int run_lv_pill_numbers_sums(void) {
    char text[LV_PILL_NUMBER_TEXT_MAX];
    char small[2];
    int vx = -1, vy = -1, mx = -1, my = -1, n;

    /* The game's list counts from 1; the stores from 0. */
    UT_ASSERT(lvPillNumber(0) == 1);
    UT_ASSERT(lvPillNumber(6) == 7);
    UT_ASSERT(lvPillNumber(15) == 16);

    /* A number only for a pillbox standing on the map: not for one a tank
       carries, and not for one the round took off the map. */
    UT_ASSERT(lvPillLabelShown(true, false));
    UT_ASSERT(!lvPillLabelShown(true, true));
    UT_ASSERT(!lvPillLabelShown(false, false));
    UT_ASSERT(!lvPillLabelShown(false, true));

    /* The view square: the view's top left square is 0, 0, and the margin
       column and row (lastX, lastY) count as in the view. */
    UT_ASSERT(lvPillViewSquare(100, 50, 90, 40, 20, 15, &vx, &vy) && vx == 10 && vy == 10);
    UT_ASSERT(lvPillViewSquare(90, 40, 90, 40, 20, 15, &vx, &vy) && vx == 0 && vy == 0);
    UT_ASSERT(lvPillViewSquare(110, 55, 90, 40, 20, 15, &vx, &vy) && vx == 20 && vy == 15);
    UT_ASSERT_MSG(!lvPillViewSquare(111, 50, 90, 40, 20, 15, &vx, &vy), "past the margin");
    UT_ASSERT_MSG(!lvPillViewSquare(100, 56, 90, 40, 20, 15, &vx, &vy), "under the margin");
    UT_ASSERT_MSG(!lvPillViewSquare(89, 50, 90, 40, 20, 15, &vx, &vy), "left of the view");
    UT_ASSERT_MSG(!lvPillViewSquare(100, 39, 90, 40, 20, 15, &vx, &vy), "above the view");
    UT_ASSERT(lvPillViewSquare(100, 50, 90, 40, 20, 15, NULL, NULL));

    /* The square under the mouse, as a click on the map picks it: whole
       squares of the picture from the view's top left. */
    UT_ASSERT(lvPillMapSquareAt(0, 0, 16, 90, 40, &mx, &my) && mx == 90 && my == 40);
    UT_ASSERT(lvPillMapSquareAt(15, 15, 16, 90, 40, &mx, &my) && mx == 90 && my == 40);
    UT_ASSERT(lvPillMapSquareAt(16, 31, 16, 90, 40, &mx, &my) && mx == 91 && my == 41);
    UT_ASSERT(lvPillMapSquareAt(165, 170, 16, 90, 40, &mx, &my) && mx == 100 && my == 50);
    /* The map's numbers go round at 256, as the click's do. */
    UT_ASSERT(lvPillMapSquareAt(16 * 10, 0, 16, 250, 0, &mx, &my) && mx == 4 && my == 0);
    UT_ASSERT_MSG(!lvPillMapSquareAt(-1, 0, 16, 90, 40, &mx, &my), "left of the picture");
    UT_ASSERT_MSG(!lvPillMapSquareAt(0, -1, 16, 90, 40, &mx, &my), "above the picture");
    UT_ASSERT_MSG(!lvPillMapSquareAt(0, 0, 0, 90, 40, &mx, &my), "no square size");

    /* A number as text; nothing for one no pillbox has. */
    n = lvPillNumberText(7, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "7") == 0 && n == 1, "7: \"%s\" %d", text, n);
    n = lvPillNumberText(16, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "16") == 0 && n == 2, "16: \"%s\" %d", text, n);
    strcpy(text, "x");
    UT_ASSERT(lvPillNumberText(0, text, sizeof(text)) == 0 && text[0] == '\0');
    strcpy(text, "x");
    UT_ASSERT(lvPillNumberText(-3, text, sizeof(text)) == 0 && text[0] == '\0');
    UT_ASSERT(lvPillNumberText(7, NULL, 4) == 0);
    UT_ASSERT(lvPillNumberText(7, text, 0) == 0);
    n = lvPillNumberText(16, small, sizeof(small));
    UT_ASSERT_MSG(n == 1 && strcmp(small, "1") == 0, "cut short: \"%s\" %d", small, n);
    /* Every character of a number has a picture: the text is as wide in
       cells as its digits. */
    UT_ASSERT(lvBuilderTextCells("7") == 3);
    UT_ASSERT(lvBuilderTextCells("16") == 7);
    return 0;
}

int run_lv_pill_numbers_list(void) {
    char text[LV_PILL_NUMBER_LIST_MAX];
    char small[8];
    uint32_t all = 0;
    int number, n;

    UT_ASSERT(lvPillNumberBit(1) == 1u);
    UT_ASSERT(lvPillNumberBit(16) == 0x8000u);
    UT_ASSERT(lvPillNumberBit(0) == 0u);
    UT_ASSERT(lvPillNumberBit(17) == 0u);

    n = lvPillNumberListText(lvPillNumberBit(7) | lvPillNumberBit(1) | lvPillNumberBit(4),
                             text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "1, 4, 7") == 0 && n == 7, "\"%s\" %d", text, n);
    n = lvPillNumberListText(lvPillNumberBit(12), text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "12") == 0 && n == 2, "one: \"%s\"", text);
    strcpy(text, "x");
    n = lvPillNumberListText(0, text, sizeof(text));
    UT_ASSERT_MSG(text[0] == '\0' && n == 0, "none: \"%s\"", text);

    /* All sixteen fit the list's room. */
    for (number = 1; number <= LV_PILL_NUMBERS_MAX; number++) all |= lvPillNumberBit(number);
    n = lvPillNumberListText(all, text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16") == 0 &&
                  n == (int)strlen(text), "all: \"%s\"", text);

    /* Too little room: the list stops at the last whole number. */
    n = lvPillNumberListText(all, small, sizeof(small));
    UT_ASSERT_MSG(strcmp(small, "1, 2, 3") == 0 && n == 7, "cut short: \"%s\" %d", small, n);
    n = lvPillNumberListText(lvPillNumberBit(16), small, 2);
    UT_ASSERT_MSG(small[0] == '\0' && n == 0, "no room for one: \"%s\"", small);
    UT_ASSERT(lvPillNumberListText(all, NULL, 8) == 0);
    UT_ASSERT(lvPillNumberListText(all, text, 0) == 0);
    return 0;
}

int run_lv_pill_numbers_layout(void) {
    LvPillLabelLayout l;

    /* The usual zoom, 16 pixels a square: one pixel a cell. "7" is 3 by 5
       cells and stands 3 by 4 pixels into the square's bottom right corner
       (no more than a quarter of the square, and never more than itself). */
    l = lvPillLabelLayout(100.0f, 200.0f, 16.0f, 1.0f, 3);
    UT_ASSERT(l.visible);
    UT_ASSERT(pn_close(l.cell, 1.0f));
    UT_ASSERT(pn_close(l.w, 3.0f) && pn_close(l.h, 5.0f));
    UT_ASSERT_MSG(pn_close(l.x, 113.0f) && pn_close(l.y, 212.0f), "16 px: %g, %g", l.x, l.y);

    /* "16" is 7 cells wide: a quarter of the square in, the rest outside. */
    l = lvPillLabelLayout(100.0f, 200.0f, 16.0f, 1.0f, 7);
    UT_ASSERT(pn_close(l.w, 7.0f));
    UT_ASSERT_MSG(pn_close(l.x, 112.0f) && pn_close(l.y, 212.0f), "16 px, two digits: %g, %g",
                  l.x, l.y);

    /* Zoomed out to 8 pixels a square: the digits keep their size, so they
       still read, and only 2 pixels of them are over the square. */
    l = lvPillLabelLayout(40.0f, 80.0f, 8.0f, 1.0f, 7);
    UT_ASSERT(pn_close(l.cell, 1.0f));
    UT_ASSERT_MSG(pn_close(l.x, 46.0f) && pn_close(l.y, 86.0f), "8 px: %g, %g", l.x, l.y);

    /* Zoomed in to 64 pixels a square: two pixels a cell, and a quarter of
       the square is more than the number, so it is all inside the corner. */
    l = lvPillLabelLayout(0.0f, 0.0f, 64.0f, 1.0f, 7);
    UT_ASSERT(pn_close(l.cell, 2.0f));
    UT_ASSERT(pn_close(l.w, 14.0f) && pn_close(l.h, 10.0f));
    UT_ASSERT_MSG(pn_close(l.x, 50.0f) && pn_close(l.y, 54.0f), "64 px: %g, %g", l.x, l.y);
    UT_ASSERT(pn_close(l.x + l.w, 64.0f) && pn_close(l.y + l.h, 64.0f));

    /* A display of twice the density: a 16 point square is 32 pixels, and a
       cell is still one point, two pixels. */
    l = lvPillLabelLayout(0.0f, 0.0f, 32.0f, 2.0f, 3);
    UT_ASSERT(pn_close(l.cell, 2.0f));
    UT_ASSERT_MSG(pn_close(l.x, 26.0f) && pn_close(l.y, 24.0f), "2x: %g, %g", l.x, l.y);

    /* Whole pixels from a square that does not start on one. */
    l = lvPillLabelLayout(10.6f, 20.4f, 16.0f, 1.0f, 3);
    UT_ASSERT_MSG(pn_close(l.x, 23.0f) && pn_close(l.y, 32.0f), "whole: %g, %g", l.x, l.y);

    /* No text, no number. */
    l = lvPillLabelLayout(0.0f, 0.0f, 16.0f, 1.0f, 0);
    UT_ASSERT(!l.visible);
    return 0;
}

int run_lv_pill_numbers_overview(void) {
    LvOverview o;

    lvOverviewInit(&o);
    UT_ASSERT(lvOverviewAddPlayer(&o, 2, 5, "Rider"));
    UT_ASSERT(lvOverviewAddPlayer(&o, 4, 1, "ErYan"));
    lvOverviewCountPill(&o, true, 2, 1);
    lvOverviewCountPill(&o, true, 2, 7);
    lvOverviewCountPill(&o, true, 4, 3);
    /* Nobody owns it, and an owner whose team has no row: both neutral. */
    lvOverviewCountPill(&o, false, 0, 16);
    lvOverviewCountPill(&o, true, 9, 5);
    /* A number no pillbox has is counted and not noted. */
    lvOverviewCountPill(&o, true, 4, 0);

    UT_ASSERT(o.totalPills == 6);
    UT_ASSERT(o.rows[0].team == 2 && o.rows[0].pills == 2);
    UT_ASSERT(o.rows[0].pillNumbers == (lvPillNumberBit(1) | lvPillNumberBit(7)));
    UT_ASSERT(o.rows[1].team == 4 && o.rows[1].pills == 2);
    UT_ASSERT(o.rows[1].pillNumbers == lvPillNumberBit(3));
    UT_ASSERT(o.neutralPills == 2);
    UT_ASSERT(o.neutralPillNumbers == (lvPillNumberBit(16) | lvPillNumberBit(5)));
    /* Bases have no numbers to note. */
    lvOverviewCount(&o, true, true, 2);
    UT_ASSERT(o.rows[0].bases == 1 && o.rows[0].pillNumbers == (lvPillNumberBit(1) | lvPillNumberBit(7)));
    return 0;
}

/* The squares of one text as lvBuilderDrawText fills them, one at a time:
   character by character, row by row, column by column. */
static int pn_text_rects(const char *text, float x, float y, float cell, float grow,
                         SDL_FRect *out, int cap) {
    int n = 0;
    for (; *text != '\0'; text++) {
        const LvBuilderGlyph *g = lvBuilderChar(*text);
        int row, col;
        if (g == NULL) continue;
        for (row = 0; row < g->height; row++) {
            for (col = 0; col < g->width; col++) {
                if (!lvBuilderGlyphCell(g, col, row) || n >= cap) continue;
                out[n].x = x + (float)col * cell - grow;
                out[n].y = y + (float)row * cell - grow;
                out[n].w = cell + grow * 2.0f;
                out[n].h = cell + grow * 2.0f;
                n++;
            }
        }
        x += (float)(g->width + 1) * cell;
    }
    return n;
}

static void pn_set_number(LvBuilderNumber *num, const char *text, float x, float y, float cell) {
    memset(num, 0, sizeof(*num));
    snprintf(num->text, sizeof(num->text), "%s", text);
    num->x = x;
    num->y = y;
    num->cell = cell;
}

int run_lv_pill_numbers_batched(void) {
    static SDL_FRect batched[LV_BUILDER_PASS_RECTS];
    static SDL_FRect single[LV_BUILDER_PASS_RECTS];
    LvBuilderNumber nums[4];
    float edge = lvTankBarPointsToPixels(LV_BUILDER_EDGE_POINTS, 1.0f);
    int count = 4, pass, i, next, n, want;

    /* Two numbers side by side, so the edge of one runs over the other, a
       number at two pixels a cell, and "0", the number of the first base. */
    pn_set_number(&nums[0], "7", 13.0f, 12.0f, 1.0f);
    pn_set_number(&nums[1], "8", 15.0f, 12.0f, 1.0f);
    pn_set_number(&nums[2], "16", 20.0f, 30.0f, 2.0f);
    pn_set_number(&nums[3], "0", 50.0f, 50.0f, 1.0f);

    /* The batched list is the squares of each number, in turn, as the old
       one call for each square made them. */
    for (pass = 0; pass < 2; pass++) {
        float grow = (pass == 0) ? edge : 0.0f;
        next = 0;
        n = lvBuilderNumbersRects(nums, count, &next, grow, batched, LV_BUILDER_PASS_RECTS);
        UT_ASSERT(next == count);
        want = 0;
        for (i = 0; i < count; i++) {
            want += pn_text_rects(nums[i].text, nums[i].x, nums[i].y, nums[i].cell, grow,
                                  &single[want], LV_BUILDER_PASS_RECTS - want);
        }
        UT_ASSERT_MSG(n == want && n > 0, "pass %d: %d squares, want %d", pass, n, want);
        for (i = 0; i < n; i++) {
            UT_ASSERT_MSG(pn_close(batched[i].x, single[i].x) && pn_close(batched[i].y, single[i].y)
                              && pn_close(batched[i].w, single[i].w)
                              && pn_close(batched[i].h, single[i].h),
                          "pass %d square %d: %g,%g %gx%g, want %g,%g %gx%g", pass, i,
                          batched[i].x, batched[i].y, batched[i].w, batched[i].h, single[i].x,
                          single[i].y, single[i].w, single[i].h);
        }
    }
    for (i = 0; i < count; i++) {
        UT_ASSERT(lvBuilderNumberCells(&nums[i])
                  == pn_text_rects(nums[i].text, 0.0f, 0.0f, 1.0f, 0.0f, single,
                                   LV_BUILDER_PASS_RECTS));
    }

    /* With room for only the first number, the list stops after it, and a
       list with no room takes nothing. */
    next = 0;
    want = lvBuilderNumberCells(&nums[0]);
    n = lvBuilderNumbersRects(nums, count, &next, 0.0f, batched, want + 1);
    UT_ASSERT_MSG(n == want && next == 1, "first only: %d squares, next %d", n, next);
    n = lvBuilderNumbersRects(nums, count, &next, 0.0f, batched, 0);
    UT_ASSERT(n == 0 && next == 1);

    /* Drawn, the batched pass gives the same pixels as the old one call for
       each square, and puts the blend mode and colour back. */
    {
        SDL_Surface  *a = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_ARGB8888);
        SDL_Surface  *b = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_ARGB8888);
        SDL_Renderer *ra = a ? SDL_CreateSoftwareRenderer(a) : NULL;
        SDL_Renderer *rb = b ? SDL_CreateSoftwareRenderer(b) : NULL;
        SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
        Uint8 cr = 0, cg = 0, cb = 0, ca = 0;
        Uint32 bg = 0, mid = 0;
        int same = 1, row;

        if (ra == NULL || rb == NULL) {
            if (ra) SDL_DestroyRenderer(ra);
            if (rb) SDL_DestroyRenderer(rb);
            SDL_DestroySurface(a);
            SDL_DestroySurface(b);
        }
        UT_ASSERT_MSG(ra != NULL && rb != NULL, "software renderer: %s", SDL_GetError());
        SDL_SetRenderDrawColor(ra, 90, 120, 60, 255);
        SDL_RenderClear(ra);
        SDL_SetRenderDrawColor(rb, 90, 120, 60, 255);
        SDL_RenderClear(rb);

        SDL_SetRenderDrawBlendMode(ra, SDL_BLENDMODE_BLEND);
        for (pass = 0; pass < 2; pass++) {
            LvBuilderColour c = (pass == 0) ? lvBuilderEdgeColour() : lvBuilderMarkColour(false, 0);
            SDL_SetRenderDrawColorFloat(ra, c.r, c.g, c.b, c.a);
            for (i = 0; i < count; i++) {
                lvBuilderDrawText(ra, nums[i].text, nums[i].x, nums[i].y, nums[i].cell,
                                  (pass == 0) ? edge : 0.0f);
            }
        }

        SDL_SetRenderDrawBlendMode(rb, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(rb, 10, 20, 30, 40);
        lvBuilderDrawNumbers(rb, nums, count, edge);
        SDL_GetRenderDrawBlendMode(rb, &blend);
        SDL_GetRenderDrawColor(rb, &cr, &cg, &cb, &ca);

        SDL_FlushRenderer(ra);
        SDL_FlushRenderer(rb);
        for (row = 0; row < 64; row++) {
            const Uint8 *pa = (const Uint8 *)a->pixels + (size_t)row * (size_t)a->pitch;
            const Uint8 *pb = (const Uint8 *)b->pixels + (size_t)row * (size_t)b->pitch;
            if (memcmp(pa, pb, 64 * 4) != 0) {
                same = 0;
                break;
            }
        }
        /* Something was drawn: the "7"'s top bar is no longer the ground. */
        bg = ((const Uint32 *)b->pixels)[0];
        mid = ((const Uint32 *)((const Uint8 *)b->pixels + 12 * b->pitch))[14];

        SDL_DestroyRenderer(ra);
        SDL_DestroyRenderer(rb);
        SDL_DestroySurface(a);
        SDL_DestroySurface(b);
        UT_ASSERT_MSG(same, "pixels differ from row %d", row);
        UT_ASSERT_MSG(mid != bg, "nothing drawn: %08x", (unsigned)mid);
        UT_ASSERT(blend == SDL_BLENDMODE_NONE);
        UT_ASSERT(cr == 10 && cg == 20 && cb == 30 && ca == 40);
    }
    return 0;
}
