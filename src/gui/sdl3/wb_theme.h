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
 *Name:          wb_theme
 *Filename:      wb_theme.h
 *Purpose:
 *  WinBolo UI theme — central palette of identity colors used
 *  by ImGui dialogs (lobby, scoreboard, server browser, etc.).
 *
 *  Two color domains, kept separate:
 *    - UI / identity (this file): dialog chrome, status text,
 *      team colors, badges. Owned by WbTheme.
 *    - Game render (skin system, separate): tile colors, tank
 *      palette, projectiles, HUD. Owned by skins.
 *
 *  A user can pick "Light theme + Old-school skin" without
 *  either system caring about the other.
 *
 *  The theme also controls ImGui's own ImGuiCol_* slots via
 *  apply() — call once at startup, again on theme switch.
 *
 *  Initial scope: one Dark theme, populated to match what
 *  WinBolo currently renders. Additional themes (Light,
 *  Classic, High Contrast) are a future effort that doesn't
 *  require touching call sites.
 *********************************************************/

#ifndef WB_THEME_H
#define WB_THEME_H

#include "imgui.h"

struct WbTheme {
    const char *name;

    /* Apply this theme's chrome colors to ImGui's style.Colors[].
     * Called once at startup with the active theme; called again
     * after a theme switch. */
    void (*apply)(void);

    /* ── Identity palette ──────────────────────────────────── */

    /* Team identity colors — 8 distinguishable hues. Indexed by
     * the per-team color picker (red, blue, green, yellow, purple,
     * orange, cyan, pink). Used wherever team identity matters:
     * lobby team headers, scoreboard, alliance overlay. NOT used
     * for tank rendering — tanks stay on the per-player palette
     * driven by the skin system. */
    ImU32 teamColors[8];

    /* Status indicators — used for player rows, ping text,
     * connectivity dots, etc. */
    ImU32 statusOnline;        /* connected, normal */
    ImU32 statusReady;         /* "ready" green */
    ImU32 statusHighPing;      /* warning yellow */
    ImU32 statusDisconnected;  /* dropped/error red */

    /* Badge colors — small inline indicators */
    ImU32 lockBadge;           /* server-locked setting (orange) */
    ImU32 botBadge;            /* "bot" identifier */
    /* HOST tag colors — small square pill rendered next to slot 0's
     * name. Drawn as bg + 1px border + text so each layer is themable. */
    ImU32 hostTagBg;
    ImU32 hostTagBorder;
    ImU32 hostTagText;
    /* BOT tag — same bg as HOST so the row badges read as a family.
     * Border + text differ to keep the two visually distinct. */
    ImU32 botTagBg;
    ImU32 botTagBorder;
    ImU32 botTagText;
    /* Bot NAME tag ("GoalHunter") — same bg family, its own border + text so
     * it reads as a third member of the row-badge set, not a second BOT. */
    ImU32 brainTagBg;
    ImU32 brainTagBorder;
    ImU32 brainTagText;
    /* Bot DIFFICULTY tag — one colour per level, matching the Easy./Medium./
     * Hard. tagline tokens (green / amber / red) so the two readings agree. */
    ImU32 diffEasyTagBg;
    ImU32 diffEasyTagBorder;
    ImU32 diffEasyTagText;
    ImU32 diffMediumTagBg;
    ImU32 diffMediumTagBorder;
    ImU32 diffMediumTagText;
    ImU32 diffHardTagBg;
    ImU32 diffHardTagBorder;
    ImU32 diffHardTagText;
};

/* Single global theme pointer. Swapped on theme change. */
extern const WbTheme *g_theme;

/* Convenience accessors — read identity colors as ImVec4 (ImGui
 * APIs that take TextColored, PushStyleColor, etc. want ImVec4). */
static inline ImVec4 wbThemeTeamColor(int idx) {
    return ImGui::ColorConvertU32ToFloat4(g_theme->teamColors[idx & 7]);
}
static inline ImVec4 wbThemeColor(ImU32 c) {
    return ImGui::ColorConvertU32ToFloat4(c);
}

/* Initialise the theme system. Called once at startup, before
 * any ImGui frame. Sets g_theme to the default and applies it. */
void wbThemeInit(void);

#endif /* WB_THEME_H */
