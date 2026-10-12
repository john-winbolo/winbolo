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
 *       a grey shape and a dark team colour becomes one that shows on black.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_hud.h"
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
    return 0;
}
