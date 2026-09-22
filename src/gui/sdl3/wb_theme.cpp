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

#include "wb_theme.h"
#include "imgui.h"
#include "../imgui_theme.h"   /* imguiApplyBoloTheme — existing chrome */

/* ── Default Dark theme ────────────────────────────────────────────
 * The Dark theme delegates chrome setup to the existing
 * imguiApplyBoloTheme() (custom rounding + blue-teal palette already
 * established in src/gui/imgui_theme.h). WbTheme adds the IDENTITY
 * layer on top — team colors, status indicators, badges — that the
 * chrome theme didn't cover.
 *
 * Self-contained on purpose: a future theme switch (g_theme = &light;
 * g_theme->apply()) re-applies everything from scratch without needing
 * call sites to remember to also call imguiApplyBoloTheme. */
static void applyDark(void) {
    imguiApplyBoloTheme();
}

static const WbTheme s_themeDark = {
    /* name  */ "Dark",
    /* apply */ applyDark,

    /* teamColors — match LobbyData.jsx TEAM_COLORS hex values.
     * Order: red, blue, green, yellow, purple, orange, cyan, pink. */
    /* teamColors */ {
        IM_COL32(216,  98,  90, 255),  /* #d8625a red    */
        IM_COL32( 74, 144, 200, 255),  /* #4a90c8 blue   */
        IM_COL32(109, 199, 122, 255),  /* #6dc77a green  */
        IM_COL32(232, 197,  71, 255),  /* #e8c547 yellow */
        IM_COL32(176, 122, 216, 255),  /* #b07ad8 purple */
        IM_COL32(232, 146,  72, 255),  /* #e89248 orange */
        IM_COL32( 95, 200, 216, 255),  /* #5fc8d8 cyan   */
        IM_COL32(232, 139, 184, 255),  /* #e88bb8 pink   */
    },

    /* Status indicators — match WinBolo's current inline colors
     * (sdl3imgui.cpp uses (0, 0.9, 0) for online and (0.9, 0, 0)
     * for disconnected). High-ping yellow lifted from the player
     * row warning gradient. */
    /* statusOnline       */ IM_COL32(  0, 230,   0, 255),
    /* statusReady        */ IM_COL32(  0, 230,   0, 255),
    /* statusHighPing     */ IM_COL32(231, 165,  75, 255),
    /* statusDisconnected */ IM_COL32(230,   0,   0, 255),

    /* Badges */
    /* lockBadge  */ IM_COL32(231, 165,  75, 255),  /* same as Layout A's lock orange */
    /* botBadge   */ IM_COL32(170, 170, 200, 255),  /* desaturated for "this is computer" */
    /* hostTagBg     */ IM_COL32( 73,  76,  72, 255),  /* near-black khaki */
    /* hostTagBorder */ IM_COL32(121, 113,  72, 255),  /* warm olive */
    /* hostTagText   */ IM_COL32(228, 194,  71, 255),  /* host gold */
    /* botTagBg      */ IM_COL32( 73,  76,  72, 255),  /* match HOST bg */
    /* botTagBorder  */ IM_COL32( 95, 110, 125, 255),  /* cool slate    */
    /* botTagText    */ IM_COL32(220, 230, 245, 255),  /* near-white    */
    /* diffEasyTagBg      */ IM_COL32( 73,  76,  72, 255),
    /* diffEasyTagBorder  */ IM_COL32( 70, 140,  80, 255),  /* Easy. green  */
    /* diffEasyTagText    */ IM_COL32(140, 220, 150, 255),
    /* diffMediumTagBg    */ IM_COL32( 73,  76,  72, 255),
    /* diffMediumTagBorder*/ IM_COL32(165, 130,  55, 255),  /* Medium. amber */
    /* diffMediumTagText  */ IM_COL32(238, 192,  90, 255),
    /* diffHardTagBg      */ IM_COL32( 73,  76,  72, 255),
    /* diffHardTagBorder  */ IM_COL32(170,  85,  60, 255),  /* Hard. red    */
    /* diffHardTagText    */ IM_COL32(242, 135, 100, 255),
    /* Mod is an ocean blue and Scenario a burnt amber — the two hues the
     * rounded pill in the Mods dialog used before it became a square chip.
     * Both fills are taken well down from those pill colours so the label on
     * top reads at 70% of the font size, and both borders are lifted well
     * above their fill, which is the same shape the triples above use.
     * Neither collides with the row tags: HOST and BOT fill with a near-black
     * khaki, ADMIN with teal and the bot mode chip with indigo, so the only
     * near neighbour is indigo against Mod, and Mod is the greener of the
     * two by a clear margin. */
    /* modTagBg           */ IM_COL32( 34,  74, 112, 255),  /* deep ocean    */
    /* modTagBorder       */ IM_COL32( 86, 156, 210, 255),  /* bright sky    */
    /* modTagText         */ IM_COL32(190, 222, 250, 255),  /* pale sky      */
    /* scenarioTagBg      */ IM_COL32( 92,  62,  30, 255),  /* burnt amber   */
    /* scenarioTagBorder  */ IM_COL32(190, 136,  62, 255),  /* bright amber  */
    /* scenarioTagText    */ IM_COL32(246, 210, 156, 255),  /* pale amber    */
};

/* The single live theme pointer. Defaults to Dark; swap-then-apply
 * for theme switches. */
const WbTheme *g_theme = &s_themeDark;

void wbThemeInit(void) {
    g_theme = &s_themeDark;
    g_theme->apply();
}
