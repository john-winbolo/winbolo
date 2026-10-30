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
 * A team is drawn in the colour the Team Colours dialog has for it
 * (lv_team_colour.h), and that is the row of the sheet with the same number.
 * The palette has a red (entry 11), and a team that has it is drawn red. Its
 * pillboxes are then close to the game's red pillbox that no team holds, so
 * while such a team is in the game the neutral pillboxes are drawn grey
 * (lvNeutralPillEntry, below).
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

/* The palette's red. */
#define LV_TEAM_PALETTE_RED    11

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

/* The row of a team sheet for a palette entry (the team's colour, as the
 * Team Colours dialog has it): the row with the same number. An entry the
 * sheet does not have gets the first row. */
static inline int lvTeamSheetRow(int palette) {
    if (palette < 0 || palette >= LV_TEAM_SHEET_ROWS) return 0;
    return palette;
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

/* A neutral pillbox when a team is red.
 *
 * The game's neutral pillbox is red: its coloured pixels are 0xde0000. A
 * team in the palette's red (entry 11, 0xc00000 on items.bmp) has pillboxes
 * the viewer cannot tell from the neutral ones. So, when a team in the game
 * has a colour that close to the neutral pillbox's red, the neutral
 * pillboxes are drawn grey instead: from the items sheet, in a grey row.
 *
 * "Close" is an RGB distance under LV_NEUTRAL_PILL_CLASH_DIST between the
 * team's row of items.bmp and the neutral pillbox's red. Entry 11 is 30
 * away. The nearest other entry, brown (13), is 159 away, so only the red
 * team clashes.
 *
 * The grey is grey (entry 0, 0x6f6f6f) when no team in the game has it,
 * and light grey (entry 10, 0xa5a5a5) when one does. Both are far from the
 * ground and the road, which are dark. If both greys are taken, the first
 * entry no team has and that does not clash is used. */

/* The coloured pixels of the game's neutral (hostile) pillbox tile. */
#define LV_NEUTRAL_PILL_RGB 0xde0000u

/* The RGB distance under which a team colour is too close to that red. */
#define LV_NEUTRAL_PILL_CLASH_DIST 96

/* The two greys, in the order they are tried. */
#define LV_NEUTRAL_PILL_GREY       0
#define LV_NEUTRAL_PILL_GREY_SPARE 10

/* The coloured pixels of each row of items.bmp (the pixels that differ from
 * row to row), as 0xRRGGBB. */
static inline unsigned int lvItemsRowRgb(int entry) {
    static const unsigned int rgb[LV_TEAM_SHEET_ROWS] = {
        0x6f6f6fu, 0xc8df00u, 0x18b510u, 0xfd77ffu, 0xffff31u, 0x008f9fu,
        0xffce00u, 0xbf00bfu, 0x00ffffu, 0x31ce31u, 0xa5a5a5u, 0xc00000u,
        0x0000bdu, 0x808000u, 0xffc0c0u, 0x96ff96u, 0x60127au
    };
    return rgb[lvTeamSheetRow(entry)];
}

/* Whether a team in this palette entry is too close to the neutral
 * pillbox's red. */
static inline bool lvTeamClashesNeutralPill(int entry) {
    unsigned int a = lvItemsRowRgb(entry);
    unsigned int b = LV_NEUTRAL_PILL_RGB;
    int dr = (int)((a >> 16) & 0xffu) - (int)((b >> 16) & 0xffu);
    int dg = (int)((a >> 8) & 0xffu) - (int)((b >> 8) & 0xffu);
    int db = (int)(a & 0xffu) - (int)(b & 0xffu);
    return dr * dr + dg * dg + db * db <
           LV_NEUTRAL_PILL_CLASH_DIST * LV_NEUTRAL_PILL_CLASH_DIST;
}

/* The palette entry neutral pillboxes are drawn in, given the entries of
 * the teams in the game (entries[0..count-1]). -1 when no team clashes:
 * the neutral pillbox is then the game's red tile, as before. */
static inline int lvNeutralPillEntry(const unsigned char *entries, int count) {
    bool used[LV_TEAM_SHEET_ROWS] = { false };
    bool clash = false;
    int i;
    for (i = 0; entries != 0 && i < count; i++) {
        int e = lvTeamSheetRow(entries[i]);
        used[e] = true;
        if (lvTeamClashesNeutralPill(e)) clash = true;
    }
    if (!clash) return -1;
    if (!used[LV_NEUTRAL_PILL_GREY]) return LV_NEUTRAL_PILL_GREY;
    if (!used[LV_NEUTRAL_PILL_GREY_SPARE]) return LV_NEUTRAL_PILL_GREY_SPARE;
    for (i = 0; i < LV_TEAM_SHEET_ROWS; i++) {
        if (!used[i] && !lvTeamClashesNeutralPill(i)) return i;
    }
    return -1;
}

/* A pillbox on the map, with neutralEntry from lvNeutralPillEntry: a
 * neutral pillbox is the game's red tile when it is -1, and the cell of that
 * entry's row of items.bmp when it is not. */
static inline LvMapItemSprite lvMapPillSpriteNeutralAs(bool neutral, int armour, int palette,
                                                       int neutralEntry) {
    if (neutral && neutralEntry >= 0) return lvMapPillSprite(false, armour, neutralEntry);
    return lvMapPillSprite(neutral, armour, palette);
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

/* The same, with the grey of a neutral pillbox (lvNeutralPillEntry), so a
 * panel shows a neutral pillbox in the colour the map does. */
static inline LvMapItemSprite lvPanelItemSpriteNeutralAs(bool isBase, bool hasTeam, int palette,
                                                         int neutralEntry) {
    if (isBase) return lvMapBaseSprite(!hasTeam, palette);
    return lvMapPillSpriteNeutralAs(!hasTeam, 15, palette, neutralEntry);
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
