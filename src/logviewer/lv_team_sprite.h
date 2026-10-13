/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_team_sprite.h - which picture a pillbox or a base on the log viewer's
 * map is drawn with when the map shows every team in its own colour.
 *
 * The game has two sheets to draw from. The tile atlas holds the pictures
 * the game itself draws: a pillbox or a base in green (the player's own
 * side), in red (hostile) and, for a base, the neutral one. data/items.bmp
 * holds the same pillbox and base once a team colour: a row for each of the
 * seventeen palette entries, a column for each armour level of a pillbox (0
 * to 15) and one more for the base.
 *
 * A thing a player owns is drawn from the items sheet, in the colour of the
 * owner's team. A thing nobody owns is drawn as the game draws it, from the
 * atlas: a neutral base is the game's neutral base tile, and a neutral
 * pillbox is the game's hostile pillbox tile, which is what every player's
 * game shows for a pillbox that is not on the player's side.
 *
 * That makes the game's red the mark of a pillbox no team holds, so no team
 * may be drawn in it. The palette has a red (entry 11) all the same, and it
 * is the second team's colour unless the user picks another. A team with
 * that entry is drawn orange instead: its tank, its boat, its pillboxes and
 * its bases, and its icons in the HUD panel (lv_hud.h, the same orange). The
 * sheets have no row in that orange, so the viewer adds one when it loads
 * them: the red row again, with the red changed to the orange. A row of a
 * team sheet differs from the next in that one flat colour and nothing else,
 * so the orange row is made the way every other row is.
 *
 * Header only, with no includes but the C library's, so the viewer and the
 * unit tests (tests/unit/test_lv_hud.c) use the same sums.
 */

#ifndef LV_TEAM_SPRITE_H
#define LV_TEAM_SPRITE_H

#include <stdbool.h>

/* The rows of the team sheets (items.bmp, tanks.bmp, boats.bmp) as they are
 * in the files: one a palette entry. */
#define LV_TEAM_SHEET_ROWS 17

/* The palette's red, and its orange (a yellow orange, 255,206,0). */
#define LV_TEAM_PALETTE_ORANGE 6
#define LV_TEAM_PALETTE_RED    11

/* The row the viewer adds under the files' rows: the red row in orange. */
#define LV_TEAM_SHEET_ROW_ORANGE LV_TEAM_SHEET_ROWS

/* The orange a red team is drawn in. The HUD panel's (LV_HUD_ORANGE_*). */
#define LV_TEAM_ORANGE_R 255
#define LV_TEAM_ORANGE_G 140
#define LV_TEAM_ORANGE_B 0

/* The column of items.bmp that holds the base; columns 0 to 15 hold the
 * pillbox at that armour. */
#define LV_ITEMS_COL_BASE 16

/* The game's tile numbers this file answers with. They are the game's
 * PILL_EVIL_15, PILL_EVIL_14, PILL_EVIL_0 and BASE_NEUTRAL (tilenum.h), kept
 * as numbers here so the header needs no other; draw.c checks them against
 * the names when it is compiled. PILL_EVIL_15 sits away from the other
 * fifteen, which run down from PILL_EVIL_0 as the armour goes up. */
#define LV_TILE_PILL_EVIL_15 48
#define LV_TILE_PILL_EVIL_14 123
#define LV_TILE_PILL_EVIL_0  137
#define LV_TILE_BASE_NEUTRAL 122

/* Where a pillbox or a base is drawn from. */
typedef struct LvMapItemSprite {
    unsigned char fromAtlas; /* 1: the game's tile `tile`, out of the tile atlas */
    unsigned char tile;      /*    (a game tile number)                          */
    unsigned char col;       /* 0: the cell at this column and row of items.bmp  */
    unsigned char row;
} LvMapItemSprite;

/* The game's tile for a hostile pillbox with this armour (the table in
 * bolo/pillbox.c). The game draws a neutral pillbox with the same tile. */
static inline int lvGameHostilePillTile(int armour) {
    if (armour < 0) armour = 0;
    if (armour >= 15) return LV_TILE_PILL_EVIL_15;
    return LV_TILE_PILL_EVIL_0 - armour;
}

/* The row of a team sheet for a palette entry (the team's colour, tc[team]).
 * The red entry gets the orange row, so no team is drawn in the red row. An
 * entry the sheet does not have gets the first row. */
static inline int lvTeamSheetRow(int palette) {
    if (palette < 0 || palette >= LV_TEAM_SHEET_ROWS) return 0;
    if (palette == LV_TEAM_PALETTE_RED) return LV_TEAM_SHEET_ROW_ORANGE;
    return palette;
}

/* The row to read when the sheet may have no orange row (it could not be
 * built): the palette's own orange then, which is still not the red. */
static inline int lvTeamSheetRowHave(int row, bool haveOrangeRow) {
    if (row == LV_TEAM_SHEET_ROW_ORANGE && !haveOrangeRow) return LV_TEAM_PALETTE_ORANGE;
    return row;
}

/* One pixel of the red row on its way into the orange row. A red pixel (the
 * rule is lvHudRedToOrange's: red at least half way up, green and blue each
 * no more than 35% of it) becomes the orange; any other is left as it is.
 * Returns true when the pixel was changed. */
static inline bool lvTeamRedPixelToOrange(unsigned char *r, unsigned char *g,
                                          unsigned char *b) {
    if (*r < 128) return false;
    if ((int)*g * 100 > (int)*r * 35 || (int)*b * 100 > (int)*r * 35) return false;
    *r = LV_TEAM_ORANGE_R;
    *g = LV_TEAM_ORANGE_G;
    *b = LV_TEAM_ORANGE_B;
    return true;
}

/* A pillbox on the map. A neutral one is the game's hostile pillbox, dead
 * (no armour) included; an owned one keeps its team's colour at every armour
 * level, dead included, as the game keeps an owned dead pillbox in its
 * owner's colour. */
static inline LvMapItemSprite lvMapPillSprite(bool neutral, int armour, int palette) {
    LvMapItemSprite s = { 0, 0, 0, 0 };
    if (armour < 0) armour = 0;
    if (armour > 15) armour = 15;
    if (neutral) {
        s.fromAtlas = 1;
        s.tile = (unsigned char)lvGameHostilePillTile(armour);
        return s;
    }
    s.col = (unsigned char)armour;
    s.row = (unsigned char)lvTeamSheetRow(palette);
    return s;
}

/* A base on the map. A neutral one is the game's neutral base tile. */
static inline LvMapItemSprite lvMapBaseSprite(bool neutral, int palette) {
    LvMapItemSprite s = { 0, 0, 0, 0 };
    if (neutral) {
        s.fromAtlas = 1;
        s.tile = LV_TILE_BASE_NEUTRAL;
        return s;
    }
    s.col = LV_ITEMS_COL_BASE;
    s.row = (unsigned char)lvTeamSheetRow(palette);
    return s;
}

/* The side of a cell of the tile atlas and of items.bmp, in pixels (the
 * game's TILE_SIZE_X; draw.c checks it when it is compiled). */
#define LV_ITEM_PICTURE_PX 16

/* A pillbox or a base as a small picture in a panel (a marker of the
 * "Overlay events" lane, the Overview panel). It is the map's own picture
 * by the map's own rule: a thing a team holds is that team's cell of
 * items.bmp, and a thing no team holds is the game's tile (the red pillbox,
 * the neutral base). A pillbox is drawn whole: full armour. */
static inline LvMapItemSprite lvPanelItemSprite(bool isBase, bool hasTeam, int palette) {
    if (isBase) return lvMapBaseSprite(!hasTeam, palette);
    return lvMapPillSprite(!hasTeam, 15, palette);
}

/* How the picture is made smaller or larger. Drawn at a whole number of
 * times its own size (16, 32, 48 pixels of the display) each pixel of it is
 * a whole block, so it is drawn with no blending and stays sharp. At any
 * other size (12 or 13 pixels in a line of text) pixels would be dropped,
 * so it is blended. sidePx is in pixels of the display, not in points. */
static inline bool lvItemPictureSharp(float sidePx) {
    int whole = (int)(sidePx + 0.5f);
    float d = sidePx - (float)whole;
    if (d < 0.0f) d = -d;
    if (d > 0.01f) return false;
    return whole >= LV_ITEM_PICTURE_PX && (whole % LV_ITEM_PICTURE_PX) == 0;
}

/* The same rule for a picture of any size (the builder is 4 pixels high):
 * sharp at a whole number of times srcPx, 1 time or more. */
static inline bool lvPictureSharp(float sidePx, int srcPx) {
    int whole = (int)(sidePx + 0.5f);
    float d = sidePx - (float)whole;
    if (d < 0.0f) d = -d;
    if (d > 0.01f || srcPx <= 0) return false;
    return whole >= srcPx && (whole % srcPx) == 0;
}

#endif /* LV_TEAM_SPRITE_H */
