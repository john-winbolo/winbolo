/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer pillbox number sums
 *Filename:      lv_pill_numbers.h
 *Purpose:
 *  The number of each pillbox, as the Log Viewer shows it on
 *  the map (Options > Pillbox Numbers; draw.c draws it) and in
 *  the tooltips about pillboxes.
 *
 *  A pillbox's number is the one the game's own pillbox list
 *  gives it: its place in the map's list of pillboxes,
 *  counted from 1. The log and the viewer's stores count the
 *  same list from 0, so pillbox 1 is item 0.
 *
 *  This is the arithmetic: which pillboxes have a number on
 *  the map, where the number goes beside its pillbox, which
 *  map square is under the mouse, and the text of a list of
 *  numbers. The number is drawn in the small digits of the
 *  builder marker (lv_builder_status.h), white with a black
 *  edge, so it reads on every kind of ground at every zoom.
 *
 *  Header only, with no includes but the C library's and the
 *  builder marker's, so the viewer and the unit tests use the
 *  same sums.
 *********************************************************/

#ifndef LV_PILL_NUMBERS_H
#define LV_PILL_NUMBERS_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lv_builder_status.h"

/* The most pillboxes a map has: one bit each in a list of numbers. */
#define LV_PILL_NUMBERS_MAX 16

/* The longest text of one number: "16" and the end, with room to spare. */
#define LV_PILL_NUMBER_TEXT_MAX 4

/* The longest text of a list of numbers: all sixteen, "1, 2, ... 16", and
 * the end. */
#define LV_PILL_NUMBER_LIST_MAX 64

/* A digit is 5 cells high (lvBuilderChar). */
#define LV_PILL_NUMBER_CELLS_HIGH 5

/* The number of the pillbox the stores hold at index (from 0). */
static inline int lvPillNumber(int index) {
    return index + 1;
}

/* Whether a pillbox has its number on the map: only one that stands on the
 * map. A pillbox the round has taken off the map has none, and neither has
 * one a tank carries: the number follows the pillbox, and a carried one is
 * not on any square. */
static inline bool lvPillLabelShown(bool onMap, bool inTank) {
    return onMap && !inTank;
}

/* One pillbox whose number is on the map: its square in the view (0 is the
 * view's left column and top row) and its number. */
typedef struct LvPillSpot {
    int viewX;
    int viewY;
    int number;
} LvPillSpot;

/* The square of the view a pillbox at map square mapX, mapY stands in, when
 * the view's top left square is xOffset, yOffset and it shows lastX + 1 by
 * lastY + 1 squares (the last ones are the margin the view paints for a
 * smooth pan). False when the pillbox is not in the view. */
static inline bool lvPillViewSquare(int mapX, int mapY, int xOffset, int yOffset,
                                    int lastX, int lastY, int *viewX, int *viewY) {
    int vx = mapX - xOffset;
    int vy = mapY - yOffset;
    if (vx < 0 || vy < 0 || vx > lastX || vy > lastY) return false;
    if (viewX != NULL) *viewX = vx;
    if (viewY != NULL) *viewY = vy;
    return true;
}

/* The map square under a point of the view's picture, targetX, targetY
 * pixels from its top left, for squares tileSize pixels across and a view
 * whose top left square is xOffset, yOffset. The map is 256 squares across
 * and its numbers go round, as the click on a square has them. False for a
 * point left of or above the picture. */
static inline bool lvPillMapSquareAt(int targetX, int targetY, int tileSize, int xOffset,
                                     int yOffset, int *mapX, int *mapY) {
    if (tileSize <= 0 || targetX < 0 || targetY < 0) return false;
    if (mapX != NULL) *mapX = (xOffset + targetX / tileSize) & 0xFF;
    if (mapY != NULL) *mapY = (yOffset + targetY / tileSize) & 0xFF;
    return true;
}

/* A pillbox's number as text: "7". Nothing for a number no pillbox has.
 * Returns the length. */
static inline int lvPillNumberText(int number, char *dest, size_t size) {
    int n;
    if (dest == NULL || size == 0) return 0;
    dest[0] = '\0';
    if (number < 1 || number > 255) return 0;
    n = snprintf(dest, size, "%d", number);
    if (n < 0) { dest[0] = '\0'; return 0; }
    if ((size_t)n >= size) n = (int)size - 1;
    return n;
}

/* The bit of a pillbox's number in a list of numbers. 0 for a number no
 * pillbox has. */
static inline uint32_t lvPillNumberBit(int number) {
    if (number < 1 || number > LV_PILL_NUMBERS_MAX) return 0u;
    return 1u << (number - 1);
}

/* A list of numbers as text, smallest first: "1, 4, 7". Nothing for an
 * empty list. Returns the length. */
static inline int lvPillNumberListText(uint32_t numbers, char *dest, size_t size) {
    size_t used = 0;
    int number;

    if (dest == NULL || size == 0) return 0;
    dest[0] = '\0';
    for (number = 1; number <= LV_PILL_NUMBERS_MAX; number++) {
        int n;
        if ((numbers & lvPillNumberBit(number)) == 0) continue;
        n = snprintf(dest + used, size - used, (used == 0) ? "%d" : ", %d", number);
        if (n < 0 || (size_t)n >= size - used) {
            /* Out of room: the list stops at the last whole number. */
            dest[used] = '\0';
            break;
        }
        used += (size_t)n;
    }
    return (int)used;
}

/* ── Where the number goes ─────────────────────────────────────────── */

typedef struct LvPillLabelLayout {
    bool  visible;  /* false: there is no text */
    float cell;     /* one cell, in framebuffer pixels (whole) */
    float x;        /* the top left of the digits */
    float y;
    float w;        /* the digits, without their edge */
    float h;
} LvPillLabelLayout;

/* The number of a pillbox whose square has its top left at squareLeft,
 * squareTop in the framebuffer and is tilePx pixels across, on a display of
 * pixel density pxScale; the text is textCells cells wide
 * (lvBuilderTextCells).
 *
 * The digits are the builder marker's, one point a cell and two from
 * LV_BUILDER_BIG_TILE points to a square. They sit at the square's bottom
 * right corner and stand no more than a quarter of the square into it: the
 * rest hangs over the squares beside it. The pillbox's picture is round, so
 * that corner of its square is the ground round it, and its colour and the
 * state of its armour stay in view. On a large square, where a quarter is
 * more than the number, the number is inside the corner. Whole pixels, so
 * the cells are sharp. */
static inline LvPillLabelLayout lvPillLabelLayout(float squareLeft, float squareTop,
                                                  float tilePx, float pxScale,
                                                  int textCells) {
    LvPillLabelLayout out;
    float quarter, inX, inY, right, bottom;

    out.visible = textCells > 0 && tilePx > 0.0f;
    out.cell = lvBuilderCell(tilePx, pxScale);
    out.w = (textCells > 0 ? (float)textCells : 0.0f) * out.cell;
    out.h = (float)LV_PILL_NUMBER_CELLS_HIGH * out.cell;
    quarter = floorf(tilePx * 0.25f);
    inX = (quarter < out.w) ? quarter : out.w;
    inY = (quarter < out.h) ? quarter : out.h;
    right = floorf(squareLeft) + floorf(tilePx + 0.5f);
    bottom = floorf(squareTop) + floorf(tilePx + 0.5f);
    out.x = right - inX;
    out.y = bottom - inY;
    return out;
}

#endif /* LV_PILL_NUMBERS_H */
