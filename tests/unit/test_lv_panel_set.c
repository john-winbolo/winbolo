/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Which parts of the log viewer a frame of the overview draws
 * (lv_panel_set.h).
 *
 *   lv_panel_set_live — a live game watched from the game has every part a
 *       log file has, except the Comments panel, and has the phase badge
 *       as well. The sidebars show in both; only the game view and the
 *       compact layout leave them out.
 */

#include "lv_panel_set.h"
#include "test_harness.h"

int run_lv_panel_set_live(void) {
    const unsigned file = lvPanelSetDesktop(0);
    const unsigned live = lvPanelSetDesktop(1);
    int hostAllows, gameView, compact;

    UT_ASSERT_MSG((file & LV_PANEL_SET_COMMENTS) != 0,
                  "a log file has no Comments panel");
    UT_ASSERT_MSG((file & LV_PANEL_SET_SPEC_BADGE) == 0,
                  "a log file has the phase badge");
    UT_ASSERT_MSG((live & LV_PANEL_SET_COMMENTS) == 0,
                  "a live feed has the Comments panel");
    UT_ASSERT_MSG((live & LV_PANEL_SET_SPEC_BADGE) != 0,
                  "a live feed has no phase badge");
    UT_ASSERT_MSG(live == ((file & ~LV_PANEL_SET_COMMENTS) | LV_PANEL_SET_SPEC_BADGE),
                  "a live feed is not a log file's set without Comments and "
                  "with the badge: file 0x%x, live 0x%x", file, live);

    /* The panels the spectator did not have before: each is in both sets. */
    UT_ASSERT_MSG((live & LV_PANEL_SET_MENU_BAR) && (live & LV_PANEL_SET_SIDEBARS) &&
                  (live & LV_PANEL_SET_CONTROLS) && (live & LV_PANEL_SET_HUD) &&
                  (live & LV_PANEL_SET_OVERVIEW) && (live & LV_PANEL_SET_TANK) &&
                  (live & LV_PANEL_SET_GAME_INFO) && (live & LV_PANEL_SET_EVENTS) &&
                  (live & LV_PANEL_SET_ITEM_INFO) && (live & LV_PANEL_SET_SCENARIO) &&
                  (live & LV_PANEL_SET_CHAT),
                  "a live feed is missing a panel: 0x%x", live);

    /* The sidebars: on only when the host allows them, in the overview, and
       not in the compact layout. A live feed is not an input. */
    for (hostAllows = 0; hostAllows <= 1; hostAllows++) {
        for (gameView = 0; gameView <= 1; gameView++) {
            for (compact = 0; compact <= 1; compact++) {
                const int want = (hostAllows && !gameView && !compact) ? 1 : 0;
                const int got = lvPanelSetSidebarsOn(hostAllows, gameView, compact);
                UT_ASSERT_MSG(got == want,
                              "sidebars with host %d, game view %d, compact %d: "
                              "%d, not %d", hostAllows, gameView, compact, got, want);
            }
        }
    }
    return 0;
}
