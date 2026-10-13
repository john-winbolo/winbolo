/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The sums behind the log viewer's Overview panel (lv_overview.h).
 *
 *   lv_overview_sums — that each team gets one row however many players it
 *       has, and the rows come out in team order whatever order the players
 *       come in; that the names of a team's players are joined, the first
 *       one kept by itself, and a list too long to hold is cut and not
 *       written past; that a pillbox or a base is counted in the row of its
 *       owner's team, and one with no owner, or whose owner's team has no
 *       row, is neutral; that the rows and the neutral count add up to the
 *       total; and the share a count is of the whole.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lv_overview.h"
#include "test_harness.h"

int run_lv_overview_sums(void) {
    LvOverview o;
    int i;
    int sum;

    lvOverviewInit(&o);
    UT_ASSERT_MSG(o.rowCount == 0 && o.totalPills == 0 && o.totalBases == 0, "empty table");

    /* Players come in slot order, which is not team order. */
    UT_ASSERT_MSG(lvOverviewAddPlayer(&o, 3, 7, "Carol"), "team 3");
    UT_ASSERT_MSG(lvOverviewAddPlayer(&o, 0, 2, "Alice"), "team 0");
    UT_ASSERT_MSG(lvOverviewAddPlayer(&o, 3, 7, "Dave"), "team 3 again");
    UT_ASSERT_MSG(lvOverviewAddPlayer(&o, 1, 5, "Bob"), "team 1");
    UT_ASSERT_MSG(o.rowCount == 3, "one row for each team");
    UT_ASSERT_MSG(o.rows[0].team == 0 && o.rows[1].team == 1 && o.rows[2].team == 3,
                  "rows in team order");
    UT_ASSERT_MSG(o.rows[0].colour == 2 && o.rows[1].colour == 5 && o.rows[2].colour == 7,
                  "each row keeps its colour");
    UT_ASSERT_MSG(o.rows[2].players == 2 && o.rows[0].players == 1, "players in a team");
    UT_ASSERT_MSG(strcmp(o.rows[2].names, "Carol, Dave") == 0, "names joined");
    UT_ASSERT_MSG(strcmp(o.rows[2].firstName, "Carol") == 0, "first name kept");
    UT_ASSERT_MSG(strcmp(o.rows[0].names, "Alice") == 0, "one name, no separator");

    /* A team number no player can have makes no row. */
    UT_ASSERT_MSG(!lvOverviewAddPlayer(&o, LV_OVERVIEW_MAX_TEAMS, 0, "X"), "team too high");
    UT_ASSERT_MSG(!lvOverviewAddPlayer(&o, -1, 0, "X"), "team below zero");
    UT_ASSERT_MSG(o.rowCount == 3, "no row made");

    /* Pillboxes: two for team 0, one for team 3, one nobody owns, and one
       whose owner is in a team with no row (the owner left). */
    lvOverviewCount(&o, false, true, 0);
    lvOverviewCount(&o, false, true, 0);
    lvOverviewCount(&o, false, true, 3);
    lvOverviewCount(&o, false, false, 0);
    lvOverviewCount(&o, false, true, 9);
    UT_ASSERT_MSG(o.rows[0].pills == 2 && o.rows[1].pills == 0 && o.rows[2].pills == 1,
                  "pills by team");
    UT_ASSERT_MSG(o.neutralPills == 2, "no owner and no row are neutral");
    UT_ASSERT_MSG(o.totalPills == 5, "pill total");
    UT_ASSERT_MSG(o.totalBases == 0 && o.neutralBases == 0, "bases untouched");

    /* Bases are counted apart from pillboxes. */
    lvOverviewCount(&o, true, true, 1);
    lvOverviewCount(&o, true, false, 1);
    UT_ASSERT_MSG(o.rows[1].bases == 1 && o.neutralBases == 1 && o.totalBases == 2, "bases");
    UT_ASSERT_MSG(o.totalPills == 5, "pills untouched");

    /* The rows and the neutral count are the whole. */
    sum = o.neutralPills;
    for (i = 0; i < o.rowCount; i++) sum += o.rows[i].pills;
    UT_ASSERT_MSG(sum == o.totalPills, "pill rows add up");
    sum = o.neutralBases;
    for (i = 0; i < o.rowCount; i++) sum += o.rows[i].bases;
    UT_ASSERT_MSG(sum == o.totalBases, "base rows add up");

    /* Shares. */
    UT_ASSERT_MSG(lvOverviewShare(0, 0) == 0.0f, "nothing of nothing");
    UT_ASSERT_MSG(lvOverviewShare(3, 0) == 0.0f, "no total");
    UT_ASSERT_MSG(lvOverviewShare(0, 8) == 0.0f, "none");
    UT_ASSERT_MSG(lvOverviewShare(2, 8) == 0.25f, "a quarter");
    UT_ASSERT_MSG(lvOverviewShare(8, 8) == 1.0f, "all");
    UT_ASSERT_MSG(lvOverviewShare(9, 8) == 1.0f, "never more than all");

    /* Sixteen teams fill the table; names too long to hold are cut. */
    lvOverviewInit(&o);
    for (i = LV_OVERVIEW_MAX_TEAMS - 1; i >= 0; i--) {
        UT_ASSERT_MSG(lvOverviewAddPlayer(&o, i, i, "P"), "fill");
    }
    UT_ASSERT_MSG(o.rowCount == LV_OVERVIEW_MAX_TEAMS, "sixteen rows");
    for (i = 0; i < LV_OVERVIEW_MAX_TEAMS; i++) {
        UT_ASSERT_MSG(o.rows[i].team == i, "sixteen rows in order");
    }
    {
        char longName[64];
        memset(longName, 'n', sizeof(longName) - 1);
        longName[sizeof(longName) - 1] = '\0';
        for (i = 0; i < 16; i++) {
            UT_ASSERT_MSG(lvOverviewAddPlayer(&o, 4, 4, longName), "long names");
        }
        UT_ASSERT_MSG(strlen(o.rows[4].names) == LV_OVERVIEW_NAMES_LEN - 1, "list cut to fit");
        UT_ASSERT_MSG(o.rows[4].players == 17, "every player counted");
        UT_ASSERT_MSG(o.rows[5].team == 5 && strcmp(o.rows[5].names, "P") == 0,
                      "the next row is not written into");
    }

    return 0;
}
