/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_panel_set.h - which parts of the log viewer a frame of the overview draws.
 *
 * The viewer's overview (the map of squares, not the game view) draws the
 * menu bar, the sidebars and the panels. A log file and a live game watched
 * from the game (spectatorRun) use the same set, so the spectator has every
 * panel the viewer has. The differences are few, and they are all here:
 *
 *   - The Comments panel is not drawn on a live feed: its comments belong to
 *     a finished log on WinBolo.net, and a feed has none.
 *   - The badge that says the phase of the game (Lobby, Live, Game Over) is
 *     drawn only on a live feed.
 *
 * The menu bar itself is the same bar in both. On a live feed it has fewer
 * items (imgui_main_menu.cpp), but it is there.
 *
 * Header only, with no includes, so the viewer and the unit tests use the
 * same rule.
 */

#ifndef LV_PANEL_SET_H
#define LV_PANEL_SET_H

#ifdef __cplusplus
extern "C" {
#endif

/* One bit for each part of a frame. */
#define LV_PANEL_SET_MENU_BAR   (1u << 0)
#define LV_PANEL_SET_SIDEBARS   (1u << 1)
#define LV_PANEL_SET_CONTROLS   (1u << 2)
#define LV_PANEL_SET_HUD        (1u << 3)
#define LV_PANEL_SET_OVERVIEW   (1u << 4)
#define LV_PANEL_SET_TANK       (1u << 5)
#define LV_PANEL_SET_GAME_INFO  (1u << 6)
#define LV_PANEL_SET_EVENTS     (1u << 7)
#define LV_PANEL_SET_ITEM_INFO  (1u << 8)
#define LV_PANEL_SET_COMMENTS   (1u << 9)
#define LV_PANEL_SET_SCENARIO   (1u << 10)
#define LV_PANEL_SET_SPEC_BADGE (1u << 11)

/* The parts the desktop overview draws. live is not 0 on the spectator's
 * live feed. Each panel still draws only when its own Windows menu option is
 * on; this says which panels the frame offers at all. */
static inline unsigned lvPanelSetDesktop(int live) {
    unsigned set = LV_PANEL_SET_MENU_BAR | LV_PANEL_SET_SIDEBARS |
                   LV_PANEL_SET_CONTROLS | LV_PANEL_SET_HUD |
                   LV_PANEL_SET_OVERVIEW | LV_PANEL_SET_TANK |
                   LV_PANEL_SET_GAME_INFO | LV_PANEL_SET_EVENTS |
                   LV_PANEL_SET_ITEM_INFO | LV_PANEL_SET_COMMENTS |
                   LV_PANEL_SET_SCENARIO;
    if (live) {
        set &= ~LV_PANEL_SET_COMMENTS;
        set |= LV_PANEL_SET_SPEC_BADGE;
    }
    return set;
}

/* Whether the sidebars show. The host must allow them (the viewer and the
 * spectator both do), and the view must be the overview on a desktop: the
 * game view has a fixed size of its own, and the compact layout of a tablet
 * or a Steam Deck has no room. A live feed is no reason to leave them out. */
static inline int lvPanelSetSidebarsOn(int hostAllows, int gameView, int compact) {
    return (hostAllows && !gameView && !compact) ? 1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* LV_PANEL_SET_H */
