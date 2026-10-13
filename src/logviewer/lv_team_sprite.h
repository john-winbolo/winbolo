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
 * Header only, with no includes but the C library's, so the viewer and the
 * unit tests (tests/unit/test_lv_hud.c) use the same sums.
 */

#ifndef LV_TEAM_SPRITE_H
#define LV_TEAM_SPRITE_H

#include <stdbool.h>

/* The rows of the team sheets (items.bmp, tanks.bmp, boats.bmp): one a
 * palette entry. */
#define LV_TEAM_SHEET_ROWS 17

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
 * An entry the sheet does not have gets the first row. */
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

#endif /* LV_TEAM_SPRITE_H */
