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
 * Name:          overview_hud_layout.cpp
 * Purpose:       Implementation of the in-window overview's
 *                HUD geometry — see overview_hud_layout.h.
 *                Arithmetic only: positions.h sizes and a
 *                window size in, rectangles out.
 *********************************************************/

#include "overview_hud_layout.h"

/* Widening applied to the positions.h rectangles. The classic frame draws a
 * bevel around each grid panel and a letter beside each bar, neither of which
 * the constant covers, so a slice taken at the bare constant would cut them
 * off. */
#define HUD_PANEL_BEVEL       2   /* raised border around a grid panel */
#define HUD_SLICE_PAD         4   /* breathing room around a slice */
#define HUD_KD_ICON_W        18   /* skull/tombstone icons in the art, left of the numbers */
#define HUD_KD_PAD            3   /* above the kills line */
#define HUD_KD_ROW_H         15   /* one row of the 13 px kills/deaths font */
#define HUD_BASE_BARS_LABEL_W 15  /* the S/M/A letters left of the base bars */
#define HUD_TANK_BARS_FOOT_H  18  /* the shells/mines/armour/trees icons below the bars
                                     (they end 13 px down; the panel's bottom bevel
                                     starts 19 px down and must stay out of the slice) */
#define HUD_BUILD_ITEMS        5  /* trees, road, building, pillbox, mine */

/* The right-hand panel well in the background art: black interior from
 * HUD_PANEL_INNER_LEFT up to (not including) HUD_PANEL_INNER_RIGHT, with the
 * light bevel and the window chrome outside it. Slices stop at these so they
 * never carry a stray strip of chrome onto the map. */
#define HUD_PANEL_INNER_LEFT  438
#define HUD_PANEL_INNER_RIGHT 493

/* The build strip's slice starts left of the items so the selected-item dot
 * (BS_DOT_*, drawn at x 8 in the classic layout) is inside it. */
#define HUD_BUILD_SRC_X (BS_DOT_TREE_OFFSET_X - HUD_SLICE_PAD)

/* Column geometry, in source pixels and window pixels respectively. */
#define HUD_ROW_GAP           4   /* between stacked rows, and between a pair */
#define HUD_DIVIDER_H         3   /* the ridge between the base and tank bars */
#define HUD_MARGIN            8   /* between the column and the map rect's edge */
#define HUD_STRIP_PAD         4   /* around the newswire slice on its strip */

/* Below this the panel artwork and the digits stop being readable, so the
 * caller draws no HUD rather than a smeared one. */
#define HUD_MIN_SCALE         0.75f
/* A short, wide window would otherwise let a height-fitted column eat the
 * map, so the column never takes more than this share of the width. */
#define HUD_MAX_WIDTH_SHARE   0.30f

static int hudMaxInt(int a, int b) { return (a > b) ? a : b; }

static void hudSetSrc(OverviewHudElement *e, int x, int y, int w, int h) {
    e->srcX = x;
    e->srcY = y;
    e->srcW = w;
    e->srcH = h;
}

/* Where each piece sits in the classic frame. Every rectangle is a
 * positions.h constant plus the widening the comments above name. */
static void hudSourceRects(OverviewHudElement *el) {
    /* The LGM circle. sdl3RenderStatusPanels blits its texture one pixel
       proud on every edge, so the slice takes the same two extra pixels. */
    hudSetSrc(&el[OVERVIEW_HUD_MANSTATUS],
              MAN_STATUS_X, MAN_STATUS_Y,
              MAN_STATUS_WIDTH + 2, MAN_STATUS_HEIGHT + 2);

    /* Kills over deaths: the skull and tombstone icons from the background
       art on the left, the TTF numbers (13 px, not the 10 px
       STATUS_KILLS_HEIGHT box) on the right. Stops at the panel interior's
       edge — running to the layout edge would bring the bevel and a strip of
       window chrome along. */
    {
        int x = STATUS_KILLS_LEFT - HUD_KD_ICON_W;
        int y = STATUS_KILLS_TOP - HUD_KD_PAD;
        hudSetSrc(&el[OVERVIEW_HUD_KILLSDEATHS], x, y,
                  HUD_PANEL_INNER_RIGHT - x,
                  (STATUS_DEATHS_TOP + HUD_KD_ROW_H) - y);
    }

    /* The three item grids, bevel included. */
    hudSetSrc(&el[OVERVIEW_HUD_TANKS],
              STATUS_TANKS_LEFT - HUD_PANEL_BEVEL,
              STATUS_TANKS_TOP - HUD_PANEL_BEVEL,
              STATUS_TANKS_WIDTH + 2 * HUD_PANEL_BEVEL,
              STATUS_TANKS_HEIGHT + 2 * HUD_PANEL_BEVEL);
    hudSetSrc(&el[OVERVIEW_HUD_PILLS],
              STATUS_PILLS_LEFT - HUD_PANEL_BEVEL,
              STATUS_PILLS_TOP - HUD_PANEL_BEVEL,
              STATUS_PILLS_WIDTH + 2 * HUD_PANEL_BEVEL,
              STATUS_PILLS_HEIGHT + 2 * HUD_PANEL_BEVEL);
    hudSetSrc(&el[OVERVIEW_HUD_BASES],
              STATUS_BASES_LEFT - HUD_PANEL_BEVEL,
              STATUS_BASES_TOP - HUD_PANEL_BEVEL,
              STATUS_BASES_WIDTH + 2 * HUD_PANEL_BEVEL,
              STATUS_BASES_HEIGHT + 2 * HUD_PANEL_BEVEL);

    /* The horizontal base bars, widened left for their S/M/A labels. */
    hudSetSrc(&el[OVERVIEW_HUD_BASEBARS],
              STATUS_BASE_BARS_LEFT - HUD_BASE_BARS_LABEL_W,
              STATUS_BASE_BARS_TOP - HUD_SLICE_PAD,
              HUD_BASE_BARS_LABEL_W + STATUS_BASE_BARS_MAX_WIDTH + HUD_SLICE_PAD,
              STATUS_BASE_BARS_TOTALHEIGHT + 2 * HUD_SLICE_PAD);

    /* The vertical tank bars plus the shells/mines/armour/trees icon row the
       background art draws under them; the icons span the panel well's full
       width, so the slice does too. */
    hudSetSrc(&el[OVERVIEW_HUD_TANKBARS],
              HUD_PANEL_INNER_LEFT,
              STATUS_TANK_BARS_TOP - HUD_SLICE_PAD,
              HUD_PANEL_INNER_RIGHT - HUD_PANEL_INNER_LEFT,
              HUD_SLICE_PAD + STATUS_TANK_BARS_HEIGHT
                  + HUD_TANK_BARS_FOOT_H);

    /* All five build-select items in one strip, starting left of the items at
       the selected-item dot's column, so the indent and the dot come along
       wherever they are. */
    hudSetSrc(&el[OVERVIEW_HUD_BUILDSELECT],
              HUD_BUILD_SRC_X,
              BS_TREE_OFFSET_Y - HUD_SLICE_PAD,
              (BS_TREE_OFFSET_X + BS_ITEM_SIZE_X + HUD_SLICE_PAD) - HUD_BUILD_SRC_X,
              HUD_BUILD_ITEMS * BS_ITEM_SIZE_Y + 2 * HUD_SLICE_PAD);

    /* Both newswire lines and the box they sit in. */
    hudSetSrc(&el[OVERVIEW_HUD_NEWSWIRE],
              MESSAGE_LEFT - HUD_SLICE_PAD,
              MESSAGE_TOP - HUD_SLICE_PAD,
              MESSAGE_BOX_WIDTH + 2 * HUD_SLICE_PAD,
              MESSAGE_HEIGHT + 2 * HUD_SLICE_PAD);
}

bool overviewHudLayout(int viewW, int viewH, OverviewHudLayout *out) {
    if (!out || viewW < 1 || viewH < 1) return false;

    OverviewHudLayout lay;
    hudSourceRects(lay.el);

    /* Stack the rows top to bottom in the classic order, left-aligned in the
       column. The top row is a pair side by side, sharing the row's top edge
       and separated by the same gap that separates the rows. */
    int colX[OVERVIEW_HUD_COUNT];
    int colY[OVERVIEW_HUD_COUNT];
    int y = 0;

    colX[OVERVIEW_HUD_MANSTATUS]   = 0;
    colY[OVERVIEW_HUD_MANSTATUS]   = y;
    colX[OVERVIEW_HUD_KILLSDEATHS] = lay.el[OVERVIEW_HUD_MANSTATUS].srcW + HUD_ROW_GAP;
    colY[OVERVIEW_HUD_KILLSDEATHS] = y;
    y += hudMaxInt(lay.el[OVERVIEW_HUD_MANSTATUS].srcH,
                   lay.el[OVERVIEW_HUD_KILLSDEATHS].srcH) + HUD_ROW_GAP;

    static const int kStackedRows[] = {
        OVERVIEW_HUD_TANKS, OVERVIEW_HUD_PILLS,
        OVERVIEW_HUD_BASES, OVERVIEW_HUD_BASEBARS
    };
    for (int i = 0; i < (int)(sizeof(kStackedRows) / sizeof(kStackedRows[0])); i++) {
        colX[kStackedRows[i]] = 0;
        colY[kStackedRows[i]] = y;
        y += lay.el[kStackedRows[i]].srcH + HUD_ROW_GAP;
    }

    /* The divider ridge sits in its own slot between the base bars above and
       the tank bars below. */
    int dividerYSrc = y;
    y += HUD_DIVIDER_H + HUD_ROW_GAP;

    colX[OVERVIEW_HUD_TANKBARS]    = 0;
    colY[OVERVIEW_HUD_TANKBARS]    = y;
    y += lay.el[OVERVIEW_HUD_TANKBARS].srcH;

    /* Neither the build strip nor the newswire is in the column: the strip
       stands on the left edge and the newswire gets its own full-width strip
       along the bottom. */
    colX[OVERVIEW_HUD_BUILDSELECT] = 0;
    colY[OVERVIEW_HUD_BUILDSELECT] = 0;
    colX[OVERVIEW_HUD_NEWSWIRE] = 0;
    colY[OVERVIEW_HUD_NEWSWIRE] = 0;

    int columnHSrc = y;
    int columnWSrc = 0;
    for (int i = 0; i < OVERVIEW_HUD_COUNT; i++) {
        if (i == OVERVIEW_HUD_NEWSWIRE || i == OVERVIEW_HUD_BUILDSELECT) continue;
        columnWSrc = hudMaxInt(columnWSrc, colX[i] + lay.el[i].srcW);
    }
    if (columnWSrc < 1 || columnHSrc < 1) return false;

    /* The tank bars' panel well is narrower than the grids above it, so
       centre that row in the column rather than leaving it against the
       left edge. */
    colX[OVERVIEW_HUD_TANKBARS] =
        hudMaxInt(0, (columnWSrc - lay.el[OVERVIEW_HUD_TANKBARS].srcW) / 2);

    /* One scale fits the column and the newswire strip together. Solving

           columnHSrc * scale + newswireSrcH * scale + 2 * HUD_STRIP_PAD
               + 2 * HUD_MARGIN = viewH

       for scale keeps the pair inside the view height even stacked one above
       the other, so the bottom-anchored column always leaves at least the
       strip's own height clear above it. The width cap only ever makes the
       scale smaller, which widens that clearance.

       The build strip is left out of the fit because the column is the taller
       of the two, so a scale that fits the column leaves the strip room to
       spare. */
    float scale = (float)(viewH - 2 * HUD_MARGIN - 2 * HUD_STRIP_PAD) /
                  (float)(columnHSrc + lay.el[OVERVIEW_HUD_NEWSWIRE].srcH);
    float widthCap = HUD_MAX_WIDTH_SHARE * (float)viewW / (float)columnWSrc;
    if (scale > widthCap) scale = widthCap;
    if (scale < HUD_MIN_SCALE) return false;

    lay.scale   = scale;
    lay.columnW = (float)columnWSrc * scale;
    lay.columnH = (float)columnHSrc * scale;
    /* Anchored to the bottom-right corner, with the margin keeping the chrome
       frame on screen on every side. */
    lay.columnX = (float)viewW - (float)HUD_MARGIN - lay.columnW;
    lay.columnY = (float)viewH - (float)HUD_MARGIN - lay.columnH;

    for (int i = 0; i < OVERVIEW_HUD_COUNT; i++) {
        if (i == OVERVIEW_HUD_NEWSWIRE || i == OVERVIEW_HUD_BUILDSELECT) continue;
        lay.el[i].dstX = lay.columnX + (float)colX[i] * scale;
        lay.el[i].dstY = lay.columnY + (float)colY[i] * scale;
        lay.el[i].dstW = (float)lay.el[i].srcW * scale;
        lay.el[i].dstH = (float)lay.el[i].srcH * scale;
    }

    lay.dividerX = lay.columnX;
    lay.dividerY = lay.columnY + (float)dividerYSrc * scale;
    lay.dividerW = lay.columnW;
    lay.dividerH = (float)HUD_DIVIDER_H * scale;

    /* The build strip stands on the left edge, vertically centred, so it and
       the column bracket the map. Its slice already carries HUD_SLICE_PAD
       around the items, so the backing is the slice itself. */
    OverviewHudElement *build = &lay.el[OVERVIEW_HUD_BUILDSELECT];
    build->dstX = (float)HUD_MARGIN;
    build->dstW = (float)build->srcW * scale;
    build->dstH = (float)build->srcH * scale;
    build->dstY = ((float)viewH - build->dstH) * 0.5f;
    lay.buildX = build->dstX;
    lay.buildY = build->dstY;
    lay.buildW = build->dstW;
    lay.buildH = build->dstH;

    /* The strip sits on the map rect's bottom edge, centred, just wide enough
       for the slice; the slice keeps the column's scale. */
    OverviewHudElement *news = &lay.el[OVERVIEW_HUD_NEWSWIRE];
    news->dstW = (float)news->srcW * scale;
    news->dstH = (float)news->srcH * scale;

    /* A map rect narrower than the slice would push its ends off the view,
       and the column now shares the bottom band, so the centred strip must
       also stop short of the column's left edge. Take fewer source pixels
       rather than squashing the text: the newswire is left-aligned, so the
       right end is the part that can go. */
    float newsMaxW = (float)viewW - 2.0f * (float)HUD_MARGIN;
    {
        float clearW = 2.0f * (lay.columnX - (float)HUD_MARGIN) - (float)viewW
                       - 2.0f * (float)HUD_STRIP_PAD;
        if (clearW > 0.0f && clearW < newsMaxW) newsMaxW = clearW;
    }
    if (news->dstW > newsMaxW) {
        news->srcW = (int)((float)news->srcW * (newsMaxW / news->dstW));
        news->dstW = newsMaxW;
    }

    lay.newswireW = news->dstW + 2.0f * (float)HUD_STRIP_PAD;
    lay.newswireH = news->dstH + 2.0f * (float)HUD_STRIP_PAD;
    lay.newswireX = ((float)viewW - lay.newswireW) * 0.5f;
    lay.newswireY = (float)viewH - lay.newswireH;
    news->dstX = lay.newswireX + (float)HUD_STRIP_PAD;
    news->dstY = lay.newswireY + (float)HUD_STRIP_PAD;

    *out = lay;
    return true;
}

bool overviewHudBuildItemRect(const OverviewHudLayout *lay, int index,
                              float *outX, float *outY,
                              float *outW, float *outH) {
    if (!lay || index < 0 || index >= HUD_BUILD_ITEMS) return false;

    /* The items sit right of the dot column, one BS_ITEM_SIZE_Y apart, in the
       order the classic layout stacks them. Measured off the strip's own
       destination so the rect follows wherever the strip was placed. */
    const OverviewHudElement *build = &lay->el[OVERVIEW_HUD_BUILDSELECT];
    if (outX) *outX = build->dstX +
                      (float)(BS_TREE_OFFSET_X - HUD_BUILD_SRC_X) * lay->scale;
    if (outY) *outY = build->dstY +
                      (float)(HUD_SLICE_PAD + index * BS_ITEM_SIZE_Y) * lay->scale;
    if (outW) *outW = (float)BS_ITEM_SIZE_X * lay->scale;
    if (outH) *outH = (float)BS_ITEM_SIZE_Y * lay->scale;
    return true;
}
