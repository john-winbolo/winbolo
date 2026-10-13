/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer Tank panel sums
 *Filename:      lv_tank_status.h
 *Purpose:
 *  What the Log Viewer's Tank panel (imgui/imgui_tank.cpp)
 *  shows of the selected tank, and how tall each of its four
 *  gauges is drawn.
 *
 *  The gauges are the game's: the four bars of the status
 *  column (shells, mines, armour, trees), each filled against
 *  its own cap out of the game's rules.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer (C and C++) and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_TANK_STATUS_H
#define LV_TANK_STATUS_H

#include <stdbool.h>
#include <stddef.h>

/* The longest player name the snapshot carries, with its NUL. */
#define LV_TANK_STATUS_NAME_LEN 64

/* The four gauges, in the order the game draws them from the left. */
enum {
    LV_TANK_GAUGE_SHELLS = 0,
    LV_TANK_GAUGE_MINES,
    LV_TANK_GAUGE_ARMOUR,
    LV_TANK_GAUGE_TREES,
    LV_TANK_GAUGE_COUNT
};

/* Everything the panel draws in one frame. */
typedef struct LvTankStatus {
    unsigned char selected;   /* 0: no tank is selected; the rest is unset */
    unsigned char slot;       /* the player's slot */
    unsigned char hasTeam;    /* 0 when the player is in no team */
    unsigned char colour;     /* the palette entry the team is drawn in */
    unsigned char alive;      /* 0 while the tank is dead or not yet on the map */
    unsigned char hasStock;   /* 0: the recording holds no tank resources */
    unsigned char value[LV_TANK_GAUGE_COUNT]; /* what the tank holds now */
    unsigned char full[LV_TANK_GAUGE_COUNT];  /* the most it can hold */
    unsigned char builderOut;  /* the builder is out of the tank */
    unsigned char builderDead; /* the builder is dead (a new one is on its way) */
    unsigned char carriedPills; /* pillboxes the tank carries */
    char name[LV_TANK_STATUS_NAME_LEN];
} LvTankStatus;

/* The tank the panel shows: the selected one, when that slot has a player
 * in it. A click on a tank on the map selects it, as does a click on a tank
 * in the HUD panel (which also shows that player's view). Returns the slot,
 * or -1 for no tank. */
static inline int lvTankStatusSelected(int selfSlot, int maxTanks, bool selfInUse) {
    if (selfSlot < 0 || selfSlot >= maxTanks || !selfInUse) return -1;
    return selfSlot;
}

/* The height of a gauge in the game's pixels, as the game works it out
 * (sdl3RenderTankBarsTex): fullPixels for a tank that holds its cap, and in
 * proportion under that. A cap of 0 cannot be divided by and draws empty. A
 * value over its cap draws taller, up to the height of the gauge's well
 * (wellPixels), where the game's drawing is cut off too. */
static inline float lvTankGaugeHeight(int value, int full, int fullPixels,
                                      int wellPixels) {
    float h;
    if (full <= 0 || value <= 0) return 0.0f;
    h = (float)fullPixels / (float)full * (float)value;
    if (h > (float)wellPixels) h = (float)wellPixels;
    return h;
}

/* ---- The small pictures in place of the words --------------------------
 * Under each gauge the game's background picture has a small picture of
 * what the gauge measures: 7 pixels wide and 8 high, 2 pixels left of its
 * gauge and 6 under the foot of the gauges. The panel lifts each out with
 * one pixel of the well's black round it (a cell of 9 by 10), and in a small
 * panel draws it at the start of the line of numbers in place of the word:
 * "Shells: 12 / 40" becomes "<picture>: 12 / 40". */
#define LV_TANK_ICON_CELL_W 9
#define LV_TANK_ICON_CELL_H 10
/* The cell's left edge from the gauge's, and its top from the foot of the
 * gauges (STATUS_TANK_BARS_TOP + STATUS_TANK_BARS_HEIGHT). */
#define LV_TANK_ICON_CELL_DX (-3)
#define LV_TANK_ICON_CELL_DY 5

/* ---- What the panel shows in the room it has ---------------------------
 * Under the name the panel has the well of gauges, drawn at a whole scale,
 * and a block of text: the four values, the builder, the pillboxes. The
 * block goes beside the well or under it. It is one of three:
 *   WORDS  the four values with their words ("Shells: 12 / 40");
 *   ICONS  the four values after the small pictures;
 *   NONE   no text at all, only the gauges.
 * A block is only shown where all of it is inside the room: no line is cut
 * in width or in height, so the panel never has to scroll. */
enum {
    LV_TANK_TEXT_WORDS = 0,
    LV_TANK_TEXT_ICONS,
    LV_TANK_TEXT_NONE
};

/* The room and the sizes, all measured before anything is drawn. */
typedef struct LvTankRoom {
    float availW, availH;  /* the room under the name */
    int   wellW, wellH;    /* the well, in the game's pixels (scale 1) */
    float wellEdge;        /* what the line round the well adds each way */
    int   maxScale;        /* the largest scale the well is drawn at */
    int   wordsMinScale;   /* words are for a well at this scale or over */
    float gapBeside;       /* between the well and a block beside it */
    float gapUnder;        /* between the well and a block under it */
    float wordsW, wordsH;  /* the block with words */
    float iconsW, iconsH;  /* the block with pictures; iconsW <= 0: no
                              pictures to show (no background picture) */
} LvTankRoom;

typedef struct LvTankLayout {
    int  text;    /* LV_TANK_TEXT_* */
    bool beside;  /* the block is beside the well, else under it */
    float scale;  /* the scale of the well: a whole number, 1 or over, but
                     for a room smaller than the well (LV_TANK_TEXT_NONE
                     only), where it is a part of 1 */
} LvTankLayout;

/* The smallest the well is ever drawn, as a part of its own size. A room
 * with less than that cuts the well off; there is nothing left to tell
 * apart at that size. */
#define LV_TANK_WELL_MIN_SCALE 0.25f

/* The largest scale at which the well and a block of textW by textH are both
 * wholly inside the room, with the block beside the well or under it. 0:
 * not at any scale. textW <= 0 is the well alone. */
static inline int lvTankFitScale(const LvTankRoom *r, float textW, float textH,
                                 bool beside) {
    int s;
    for (s = r->maxScale; s >= 1; s--) {
        float w = (float)(s * r->wellW) + r->wellEdge;
        float h = (float)(s * r->wellH) + r->wellEdge;
        if (textW > 0.0f) {
            if (beside) {
                w += r->gapBeside + textW;
                if (textH > h) h = textH;
            } else {
                h += r->gapUnder + textH;
                if (textW > w) w = textW;
            }
        }
        if (w <= r->availW && h <= r->availH) return s;
    }
    return 0;
}

/* One block, both places: the place that leaves the well larger, and beside
 * when the two are equal. False when neither has the well at minScale. */
static inline bool lvTankFitText(const LvTankRoom *r, float textW, float textH,
                                 int minScale, LvTankLayout *out) {
    int beside, under;
    if (textW <= 0.0f) return false;
    beside = lvTankFitScale(r, textW, textH, true);
    under  = lvTankFitScale(r, textW, textH, false);
    if (beside < minScale && under < minScale) return false;
    out->beside = (beside >= under);
    out->scale  = (float)(out->beside ? beside : under);
    return true;
}

/* What the panel shows. In order:
 *   1. words, where they fit with the well at wordsMinScale or over (the
 *      size the panel opens at). At the smallest well the panel is small,
 *      and the pictures take the place of the words;
 *   2. pictures, where they fit with the well at any scale;
 *   3. words with the smallest well, where there are no pictures to show or
 *      the block with pictures is the wider one;
 *   4. no text. The well is then at the largest whole scale the room has
 *      space for. A room smaller than the well at scale 1 has the well
 *      shrunk to go into it, all four gauges still in view, down to
 *      LV_TANK_WELL_MIN_SCALE.
 * It reads only the room and the sizes handed in, never what the last frame
 * drew, so a panel held at one size shows one thing: at the exact size a
 * block needs it is shown, one pixel under it is not. */
static inline LvTankLayout lvTankLayoutPick(const LvTankRoom *r) {
    LvTankLayout out;
    out.text = LV_TANK_TEXT_WORDS;
    if (lvTankFitText(r, r->wordsW, r->wordsH, r->wordsMinScale, &out)) return out;
    out.text = LV_TANK_TEXT_ICONS;
    if (lvTankFitText(r, r->iconsW, r->iconsH, 1, &out)) return out;
    out.text = LV_TANK_TEXT_WORDS;
    if (lvTankFitText(r, r->wordsW, r->wordsH, 1, &out)) return out;
    out.text = LV_TANK_TEXT_NONE;
    out.beside = false;
    out.scale = (float)lvTankFitScale(r, 0.0f, 0.0f, false);
    if (out.scale < 1.0f) {
        float across = (r->availW - r->wellEdge) / (float)r->wellW;
        float down   = (r->availH - r->wellEdge) / (float)r->wellH;
        out.scale = (across < down) ? across : down;
        if (out.scale > 1.0f) out.scale = 1.0f;
        if (out.scale < LV_TANK_WELL_MIN_SCALE) out.scale = LV_TANK_WELL_MIN_SCALE;
    }
    return out;
}

/* The width a picture's cell is drawn at when it is as tall as a line of
 * text (lineH): the cell's own shape, to a whole pixel, never under 1. */
static inline float lvTankIconWidth(float lineH) {
    int w = (int)(lineH * (float)LV_TANK_ICON_CELL_W / (float)LV_TANK_ICON_CELL_H + 0.5f);
    return (float)(w < 1 ? 1 : w);
}

/* The line of one gauge written with no word ("" for the name) starts, in
 * English, with the colon that followed the word: ": 12 / 40". Returns the
 * numbers ("12 / 40"), and through hasColon whether a colon came before
 * them. The colon is drawn straight after the picture and the numbers go in
 * a column of their own, so the colons of the four lines stay under each
 * other while the numbers line up on the right. A line that starts with
 * anything else is returned whole. */
static inline const char *lvTankStockNumbers(const char *line, bool *hasColon) {
    const char *p = line;
    if (hasColon) *hasColon = false;
    if (!p) return "";
    while (*p == ' ') p++;
    if (*p != ':') return p;
    if (hasColon) *hasColon = true;
    p++;
    while (*p == ' ') p++;
    return p;
}

/* A line with a word and a value ("Pillboxes carried: 2") has the value
 * after its first colon. Returns the value ("2") with hasColon true, or the
 * whole line with hasColon false when it has no colon; such a line is not
 * written after a picture, it stays as it is. */
static inline const char *lvTankAfterColon(const char *line, bool *hasColon) {
    const char *p = line;
    if (hasColon) *hasColon = false;
    if (!p) return "";
    while (*p != '\0' && *p != ':') p++;
    if (*p != ':') return line;
    if (hasColon) *hasColon = true;
    p++;
    while (*p == ' ') p++;
    return p;
}

/* The game's 12 by 12 status picture of a pillbox, drawn at the largest
 * whole scale that is not taller than a line of text, never under 1. */
static inline float lvTankPillIconSize(float lineH, int itemSize) {
    int scale = (itemSize > 0) ? (int)(lineH / (float)itemSize) : 1;
    if (scale < 1) scale = 1;
    return (float)(scale * itemSize);
}

/* Where a line's numbers start across, from the left of the picture: after
 * the picture, the colon and a gap, and then far enough in that the end of
 * the numbers (numW wide) meets the end of the column (columnW, the widest
 * numbers of the four lines). */
static inline float lvTankIconNumberX(float iconW, float colonW, float gap,
                                      float columnW, float numW) {
    float slack = columnW - numW;
    if (slack < 0.0f) slack = 0.0f;
    return iconW + colonW + gap + slack;
}

#endif /* LV_TANK_STATUS_H */
