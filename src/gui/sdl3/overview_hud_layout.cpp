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
#define HUD_LAYOUT_EDGE       2   /* unused margin at the layout's right edge */
#define HUD_KD_LABEL_W        7   /* "K"/"D" label, left of the number */
#define HUD_KD_PAD            3   /* above the kills line */
#define HUD_KD_ROW_H         15   /* one row of the 13 px kills/deaths font */
#define HUD_BASE_BARS_LABEL_W 15  /* the S/M/A letters left of the base bars */
#define HUD_TANK_BARS_LABEL_H 14  /* the S M A T letters above the tank bars */
#define HUD_TANK_BARS_FOOT_H   6  /* the bar well's floor below them */
#define HUD_BUILD_ITEMS        5  /* trees, road, building, pillbox, mine */

/* Column geometry, in source pixels and window pixels respectively. */
#define HUD_ROW_GAP           4   /* between stacked rows, and between a pair */
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

    /* Kills over deaths. The numbers are TTF at 13 px rather than the 10 px
       STATUS_KILLS_HEIGHT box, and run right to the edge of the layout. */
    {
        int x = STATUS_KILLS_LEFT - HUD_KD_LABEL_W;
        int y = STATUS_KILLS_TOP - HUD_KD_PAD;
        hudSetSrc(&el[OVERVIEW_HUD_KILLSDEATHS], x, y,
                  OVERVIEW_HUD_SRC_W - HUD_LAYOUT_EDGE - x,
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

    /* The vertical tank bars, widened up for their S M A T labels. */
    hudSetSrc(&el[OVERVIEW_HUD_TANKBARS],
              STATUS_TANK_SHELLS - HUD_SLICE_PAD,
              STATUS_TANK_BARS_TOP - HUD_TANK_BARS_LABEL_H,
              STATUS_TANK_BARS_TOTALWIDTH + 2 * HUD_SLICE_PAD,
              HUD_TANK_BARS_LABEL_H + STATUS_TANK_BARS_HEIGHT
                  + HUD_TANK_BARS_FOOT_H);

    /* All five build-select items in one strip, so the selected one's indent
       and dot come along wherever they are. */
    hudSetSrc(&el[OVERVIEW_HUD_BUILDSELECT],
              BS_TREE_OFFSET_X - HUD_SLICE_PAD,
              BS_TREE_OFFSET_Y - HUD_SLICE_PAD,
              BS_ITEM_SIZE_X + 2 * HUD_SLICE_PAD,
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
       column. Two rows are a pair side by side, sharing the row's top edge
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

    colX[OVERVIEW_HUD_TANKBARS]    = 0;
    colY[OVERVIEW_HUD_TANKBARS]    = y;
    colX[OVERVIEW_HUD_BUILDSELECT] = lay.el[OVERVIEW_HUD_TANKBARS].srcW + HUD_ROW_GAP;
    colY[OVERVIEW_HUD_BUILDSELECT] = y;
    y += hudMaxInt(lay.el[OVERVIEW_HUD_TANKBARS].srcH,
                   lay.el[OVERVIEW_HUD_BUILDSELECT].srcH);

    /* The newswire is not in the column; it gets its own full-width strip. */
    colX[OVERVIEW_HUD_NEWSWIRE] = 0;
    colY[OVERVIEW_HUD_NEWSWIRE] = 0;

    int columnHSrc = y;
    int columnWSrc = 0;
    for (int i = 0; i < OVERVIEW_HUD_COUNT; i++) {
        if (i == OVERVIEW_HUD_NEWSWIRE) continue;
        columnWSrc = hudMaxInt(columnWSrc, colX[i] + lay.el[i].srcW);
    }
    if (columnWSrc < 1 || columnHSrc < 1) return false;

    /* One scale fits the column and the newswire strip together, so the strip
       is not laid on top of the column's last row. Solving

           columnHSrc * scale + newswireSrcH * scale + 2 * HUD_STRIP_PAD
               + 2 * HUD_MARGIN = viewH

       for scale gives the line below, and with it columnH + newswireH comes to
       exactly viewH - 2 * HUD_MARGIN. The column then starts at HUD_MARGIN and
       the strip ends on the bottom edge, which leaves HUD_MARGIN of clearance
       between them — a consequence of the fit, not a gap anyone applies. The
       width cap only ever makes the scale smaller, which widens that
       clearance. */
    float scale = (float)(viewH - 2 * HUD_MARGIN - 2 * HUD_STRIP_PAD) /
                  (float)(columnHSrc + lay.el[OVERVIEW_HUD_NEWSWIRE].srcH);
    float widthCap = HUD_MAX_WIDTH_SHARE * (float)viewW / (float)columnWSrc;
    if (scale > widthCap) scale = widthCap;
    if (scale < HUD_MIN_SCALE) return false;

    lay.scale   = scale;
    lay.columnW = (float)columnWSrc * scale;
    lay.columnH = (float)columnHSrc * scale;
    lay.columnX = (float)viewW - (float)HUD_MARGIN - lay.columnW;
    lay.columnY = (float)HUD_MARGIN;

    for (int i = 0; i < OVERVIEW_HUD_COUNT; i++) {
        if (i == OVERVIEW_HUD_NEWSWIRE) continue;
        lay.el[i].dstX = lay.columnX + (float)colX[i] * scale;
        lay.el[i].dstY = lay.columnY + (float)colY[i] * scale;
        lay.el[i].dstW = (float)lay.el[i].srcW * scale;
        lay.el[i].dstH = (float)lay.el[i].srcH * scale;
    }

    /* The strip runs the full width of the map rect and sits on its bottom
       edge; the slice keeps the column's scale and the column's left margin. */
    OverviewHudElement *news = &lay.el[OVERVIEW_HUD_NEWSWIRE];
    lay.newswireH = (float)news->srcH * scale + 2.0f * (float)HUD_STRIP_PAD;
    lay.newswireX = 0.0f;
    lay.newswireW = (float)viewW;
    lay.newswireY = (float)viewH - lay.newswireH;
    news->dstX = (float)HUD_MARGIN;
    news->dstY = lay.newswireY + (float)HUD_STRIP_PAD;
    news->dstW = (float)news->srcW * scale;
    news->dstH = (float)news->srcH * scale;

    /* A map rect narrower than the slice would push its right end off the
       view. Take fewer source pixels rather than squashing the text: the
       newswire is left-aligned, so the right end is the part that can go. */
    float newsMaxW = (float)viewW - 2.0f * (float)HUD_MARGIN;
    if (news->dstW > newsMaxW) {
        news->srcW = (int)((float)news->srcW * (newsMaxW / news->dstW));
        news->dstW = newsMaxW;
    }

    *out = lay;
    return true;
}
