/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind the log viewer's HUD panel (lv_hud.h).
 *
 *   lv_hud_sums — which icon a base, a pillbox and a tank slot shows, in the
 *       order the game decides it (dead before neutral, a carried pillbox
 *       never dead); how the three grids are laid out in the room the window
 *       has (the larger icons win, a tie goes to the row, and a window too
 *       small for either still gets scale 1); and how a status icon becomes
 *       a grey shape and a dark team colour becomes one that shows on black;
 *       that a red team is drawn orange and no other colour is; and, with a
 *       player's perspective on, which of the game's own icons each slot
 *       shows (own, allied, hostile, neutral, dead).
 *
 *   lv_team_sprite_sums — which picture a pillbox and a base on the map are
 *       drawn with when every team is shown in its colour (lv_team_sprite.h):
 *       a neutral pillbox is the game's hostile pillbox tile at each armour
 *       level, a neutral base the game's neutral base tile, and an owned one
 *       its team's cell of the items sheet.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_hud.h"
#include "lv_team_sprite.h"
#include "test_harness.h"

/* One grid of the game's panels, the name above it and the gap between two,
   as the panel uses them at an ordinary font size. */
#define PW 90
#define PH 66
#define LH 17
#define GAP 8

int run_lv_hud_sums(void) {
    LvHudFit fit;
    int w, h;
    float r, g, b;

    /* Bases: off the map shows nothing whatever else is true; no armour is
       dead before the owner is looked at. */
    UT_ASSERT_MSG(lvHudBaseIcon(false, false, 90) == LV_HUD_ICON_EMPTY, "base off map");
    UT_ASSERT_MSG(lvHudBaseIcon(true, true, 0) == LV_HUD_ICON_DEAD, "neutral base, no armour");
    UT_ASSERT_MSG(lvHudBaseIcon(true, false, 0) == LV_HUD_ICON_DEAD, "owned base, no armour");
    UT_ASSERT_MSG(lvHudBaseIcon(true, true, 90) == LV_HUD_ICON_NEUTRAL, "neutral base");
    UT_ASSERT_MSG(lvHudBaseIcon(true, false, 1) == LV_HUD_ICON_OWNED, "owned base");

    /* Pillboxes: the same, but one carried in a tank is never dead. */
    UT_ASSERT_MSG(lvHudPillIcon(false, false, 15, false) == LV_HUD_ICON_EMPTY, "pill off map");
    UT_ASSERT_MSG(lvHudPillIcon(true, false, 0, false) == LV_HUD_ICON_DEAD, "dead pill");
    UT_ASSERT_MSG(lvHudPillIcon(true, true, 0, false) == LV_HUD_ICON_DEAD, "dead neutral pill");
    UT_ASSERT_MSG(lvHudPillIcon(true, true, 15, false) == LV_HUD_ICON_NEUTRAL, "neutral pill");
    UT_ASSERT_MSG(lvHudPillIcon(true, false, 15, false) == LV_HUD_ICON_OWNED, "owned pill");
    UT_ASSERT_MSG(lvHudPillIcon(true, false, 0, true) == LV_HUD_ICON_OWNED_IN_TANK,
                  "carried pill, no armour");
    UT_ASSERT_MSG(lvHudPillIcon(true, false, 15, true) == LV_HUD_ICON_OWNED_IN_TANK,
                  "carried pill");

    /* Tanks: in the slot or not. */
    UT_ASSERT_MSG(lvHudTankIcon(true) == LV_HUD_ICON_OWNED, "tank in use");
    UT_ASSERT_MSG(lvHudTankIcon(false) == LV_HUD_ICON_EMPTY, "tank slot empty");

    /* The room a layout takes: the row is three grids and two gaps wide and
       one grid high; the column is one wide and three high. */
    lvHudContentSize(3, 1, PW, PH, LH, GAP, &w, &h);
    UT_ASSERT_MSG(w == 286 && h == 83, "row at 1: %dx%d", w, h);
    lvHudContentSize(1, 2, PW, PH, LH, GAP, &w, &h);
    UT_ASSERT_MSG(w == 180 && h == 463, "column at 2: %dx%d", w, h);

    /* A dock at its default width, given the height of one row: the row. */
    fit = lvHudFit(404, 83, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 3 && fit.scale == 1, "wide and short: %d cols x%d",
                  fit.columns, fit.scale);
    /* One pixel too short for the row and nothing else fits: still the row,
       because the window is wide enough for it. */
    fit = lvHudFit(404, 82, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 3 && fit.scale == 1, "wide, too short: %d cols x%d",
                  fit.columns, fit.scale);
    /* A narrow dock: the row does not fit across, so the column. */
    fit = lvHudFit(204, 265, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 1 && fit.scale == 1, "narrow: %d cols x%d",
                  fit.columns, fit.scale);
    fit = lvHudFit(204, 40, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 1 && fit.scale == 1, "narrow, too short: %d cols x%d",
                  fit.columns, fit.scale);
    /* The floating window's default: tall and narrow, the column at 2. */
    fit = lvHudFit(180, 463, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 1 && fit.scale == 2, "default float: %d cols x%d",
                  fit.columns, fit.scale);
    /* Room for either at the same scale: the row, the shorter of the two. */
    fit = lvHudFit(300, 300, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 3 && fit.scale == 1, "tie: %d cols x%d",
                  fit.columns, fit.scale);
    /* Wide and tall: the row at 3 (826 wide) beats the column at 2. */
    fit = lvHudFit(830, 500, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 3 && fit.scale == 3, "large: %d cols x%d",
                  fit.columns, fit.scale);
    /* No room at all is still scale 1, never 0. */
    fit = lvHudFit(0, 0, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.columns == 1 && fit.scale == 1, "no room: %d cols x%d",
                  fit.columns, fit.scale);
    /* The scale stops at the largest one tried. */
    fit = lvHudFit(100000, 100000, PW, PH, LH, GAP);
    UT_ASSERT_MSG(fit.scale == LV_HUD_MAX_SCALE, "huge: x%d", fit.scale);

    /* A status icon's pixel as grey: pure green and pure red are both white,
       black stays black, and a dimmer icon is stretched to its own brightest
       pixel. */
    UT_ASSERT_MSG(lvHudGreyValue(0, 255, 0, 255) == 255, "green");
    UT_ASSERT_MSG(lvHudGreyValue(255, 0, 0, 255) == 255, "red");
    UT_ASSERT_MSG(lvHudGreyValue(0, 0, 0, 255) == 0, "black");
    UT_ASSERT_MSG(lvHudGreyValue(0, 100, 0, 200) == 128, "half of a dim icon: %d",
                  lvHudGreyValue(0, 100, 0, 200));
    UT_ASSERT_MSG(lvHudGreyValue(0, 200, 0, 200) == 255, "top of a dim icon");
    UT_ASSERT_MSG(lvHudGreyValue(0, 100, 0, 0) == 0, "an icon with no bright pixel");

    /* A colour bright enough is left alone. */
    r = 0.9f; g = 0.8f; b = 0.1f;
    lvHudReadableColour(&r, &g, &b);
    UT_ASSERT_MSG(r == 0.9f && g == 0.8f && b == 0.1f, "bright colour changed");
    /* The palette's blue is lifted to the least brightness and stays blue:
       red and green rise together and blue is still the largest. */
    r = 0.0f; g = 0.0f; b = 189.0f / 255.0f;
    lvHudReadableColour(&r, &g, &b);
    {
        float luma = 0.299f * r + 0.587f * g + 0.114f * b;
        UT_ASSERT_MSG(luma > LV_HUD_MIN_LUMA - 0.001f && luma < LV_HUD_MIN_LUMA + 0.001f,
                      "blue lifted to %f", (double)luma);
        UT_ASSERT_MSG(r == g && b > r && b <= 1.0f, "blue is %f %f %f",
                      (double)r, (double)g, (double)b);
    }
    /* Black becomes a grey, not a hue. */
    r = g = b = 0.0f;
    lvHudReadableColour(&r, &g, &b);
    UT_ASSERT_MSG(r == g && g == b && r > 0.3f && r < 0.4f, "black lifted to %f", (double)r);

    /* All teams shown: the palette's red (191,0,0) is drawn orange
       (255,140,0), and so is a pure red. */
    r = 191.0f / 255.0f; g = 0.0f; b = 0.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == true, "palette red not changed");
    UT_ASSERT_MSG(r == 1.0f && g == 140.0f / 255.0f && b == 0.0f, "red became %f %f %f",
                  (double)r, (double)g, (double)b);
    r = 1.0f; g = 0.0f; b = 0.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == true && g == 140.0f / 255.0f, "pure red");
    /* The other colours of the palette are left as they are: its orange
       (255,206,0), brown (128,128,0), light pink (255,192,192), pink
       (253,119,255), purple (96,18,122) and blue (0,0,189). */
    r = 1.0f; g = 206.0f / 255.0f; b = 0.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false && g == 206.0f / 255.0f, "orange changed");
    r = 128.0f / 255.0f; g = 128.0f / 255.0f; b = 0.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false, "brown changed");
    r = 1.0f; g = 192.0f / 255.0f; b = 192.0f / 255.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false, "light pink changed");
    r = 253.0f / 255.0f; g = 119.0f / 255.0f; b = 1.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false, "pink changed");
    r = 96.0f / 255.0f; g = 18.0f / 255.0f; b = 122.0f / 255.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false, "purple changed");
    r = 0.0f; g = 0.0f; b = 189.0f / 255.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false && b == 189.0f / 255.0f, "blue changed");
    /* A red too dark to be called red, and one with too much green in it. */
    r = 0.4f; g = 0.0f; b = 0.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false, "dark red changed");
    r = 1.0f; g = 0.4f; b = 0.0f;
    UT_ASSERT_MSG(lvHudRedToOrange(&r, &g, &b) == false, "red-orange changed");

    /* What a thing is to the perspective player: own before allied, and
       everybody else's hostile. A thing with no owner is none of them. */
    UT_ASSERT_MSG(lvHudRelation(false, false, false) == LV_HUD_REL_NONE, "no owner");
    UT_ASSERT_MSG(lvHudRelation(false, true, true) == LV_HUD_REL_NONE, "no owner wins");
    UT_ASSERT_MSG(lvHudRelation(true, true, false) == LV_HUD_REL_OWN, "own");
    UT_ASSERT_MSG(lvHudRelation(true, true, true) == LV_HUD_REL_OWN, "own before allied");
    UT_ASSERT_MSG(lvHudRelation(true, false, true) == LV_HUD_REL_ALLIED, "allied");
    UT_ASSERT_MSG(lvHudRelation(true, false, false) == LV_HUD_REL_HOSTILE, "hostile");

    /* The game's tank icons: the player's own, an ally's, a hostile one, and
       nothing for an empty slot. */
    UT_ASSERT_MSG(lvHudGameTankIcon(LV_HUD_ICON_EMPTY, LV_HUD_REL_NONE) == LV_HUD_GAME_NONE,
                  "empty tank slot");
    UT_ASSERT_MSG(lvHudGameTankIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_OWN) == LV_HUD_GAME_TANK_SELF,
                  "own tank");
    UT_ASSERT_MSG(lvHudGameTankIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_ALLIED) ==
                      LV_HUD_GAME_TANK_ALLIED, "allied tank");
    UT_ASSERT_MSG(lvHudGameTankIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_HOSTILE) ==
                      LV_HUD_GAME_TANK_HOSTILE, "hostile tank");

    /* The game's pillbox icons. Dead and neutral do not look at the player. */
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_EMPTY, LV_HUD_REL_OWN) == LV_HUD_GAME_NONE,
                  "pill off map");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_DEAD, LV_HUD_REL_OWN) == LV_HUD_GAME_DEAD,
                  "own dead pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_DEAD, LV_HUD_REL_HOSTILE) == LV_HUD_GAME_DEAD,
                  "hostile dead pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_NEUTRAL, LV_HUD_REL_NONE) ==
                      LV_HUD_GAME_PILL_NEUTRAL, "neutral pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_OWN) == LV_HUD_GAME_PILL_OWN,
                  "own pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_ALLIED) ==
                      LV_HUD_GAME_PILL_ALLIED, "allied pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_HOSTILE) ==
                      LV_HUD_GAME_PILL_HOSTILE, "hostile pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_OWNED_IN_TANK, LV_HUD_REL_OWN) ==
                      LV_HUD_GAME_PILL_TANK_OWN, "own carried pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_OWNED_IN_TANK, LV_HUD_REL_ALLIED) ==
                      LV_HUD_GAME_PILL_TANK_ALLIED, "allied carried pill");
    UT_ASSERT_MSG(lvHudGamePillIcon(LV_HUD_ICON_OWNED_IN_TANK, LV_HUD_REL_HOSTILE) ==
                      LV_HUD_GAME_PILL_TANK_HOSTILE, "hostile carried pill");

    /* The game's base icons. */
    UT_ASSERT_MSG(lvHudGameBaseIcon(LV_HUD_ICON_EMPTY, LV_HUD_REL_OWN) == LV_HUD_GAME_NONE,
                  "base off map");
    UT_ASSERT_MSG(lvHudGameBaseIcon(LV_HUD_ICON_DEAD, LV_HUD_REL_ALLIED) == LV_HUD_GAME_DEAD,
                  "allied dead base");
    UT_ASSERT_MSG(lvHudGameBaseIcon(LV_HUD_ICON_NEUTRAL, LV_HUD_REL_NONE) ==
                      LV_HUD_GAME_BASE_NEUTRAL, "neutral base");
    UT_ASSERT_MSG(lvHudGameBaseIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_OWN) == LV_HUD_GAME_BASE_OWN,
                  "own base");
    UT_ASSERT_MSG(lvHudGameBaseIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_ALLIED) ==
                      LV_HUD_GAME_BASE_ALLIED, "allied base");
    UT_ASSERT_MSG(lvHudGameBaseIcon(LV_HUD_ICON_OWNED, LV_HUD_REL_HOSTILE) ==
                      LV_HUD_GAME_BASE_HOSTILE, "hostile base");
    return 0;
}

int run_lv_team_sprite_sums(void) {
    LvMapItemSprite s;
    int armour;

    /* The game's hostile pillbox tiles: PILL_EVIL_15 is 48, PILL_EVIL_14 is
       123 and the rest run up to PILL_EVIL_0 at 137 (tilenum.h). */
    UT_ASSERT_MSG(lvGameHostilePillTile(15) == 48, "armour 15");
    UT_ASSERT_MSG(lvGameHostilePillTile(14) == 123, "armour 14");
    UT_ASSERT_MSG(lvGameHostilePillTile(1) == 136, "armour 1");
    UT_ASSERT_MSG(lvGameHostilePillTile(0) == 137, "armour 0");
    UT_ASSERT_MSG(lvGameHostilePillTile(-3) == 137, "armour under 0");
    UT_ASSERT_MSG(lvGameHostilePillTile(200) == 48, "armour over 15");

    /* A neutral pillbox is that tile out of the atlas at every armour level,
       dead included, whatever colour is asked for: never a team's row. */
    for (armour = 0; armour <= 15; armour++) {
        s = lvMapPillSprite(true, armour, 10);
        UT_ASSERT_MSG(s.fromAtlas == 1 && s.tile == lvGameHostilePillTile(armour),
                      "neutral pill armour %d: atlas %d tile %d", armour, s.fromAtlas, s.tile);
    }
    s = lvMapPillSprite(true, 15, 2);
    UT_ASSERT_MSG(s.fromAtlas == 1 && s.tile == 48, "neutral pill, another colour");

    /* An owned pillbox: the column of its armour, the row of its team's
       colour. A dead one keeps the colour. */
    s = lvMapPillSprite(false, 15, 2);
    UT_ASSERT_MSG(s.fromAtlas == 0 && s.col == 15 && s.row == 2, "owned pill");
    s = lvMapPillSprite(false, 0, 12);
    UT_ASSERT_MSG(s.fromAtlas == 0 && s.col == 0 && s.row == 12, "owned dead pill");
    s = lvMapPillSprite(false, 99, 12);
    UT_ASSERT_MSG(s.fromAtlas == 0 && s.col == 15, "armour over 15 is 15");

    /* A neutral base is the game's neutral base tile (BASE_NEUTRAL, 122); an
       owned one is the base column in its team's row. */
    s = lvMapBaseSprite(true, 10);
    UT_ASSERT_MSG(s.fromAtlas == 1 && s.tile == 122, "neutral base");
    s = lvMapBaseSprite(false, 4);
    UT_ASSERT_MSG(s.fromAtlas == 0 && s.col == LV_ITEMS_COL_BASE && s.row == 4, "owned base");

    /* A colour the sheet has no row for cannot read past the sheet. */
    s = lvMapBaseSprite(false, 200);
    UT_ASSERT_MSG(s.row == 0, "palette entry out of range");
    s = lvMapPillSprite(false, 15, -1);
    UT_ASSERT_MSG(s.row == 0, "palette entry under 0");
    return 0;
}
