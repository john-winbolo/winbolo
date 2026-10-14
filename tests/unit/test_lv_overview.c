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

#include "lv_bot_chip.h"
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

/* The chip of a bot: which one a player has, and where the names of a row
   start, so the panel can put the chip in front of the right name. */
int run_lv_overview_bots(void) {
    LvOverview o;
    const LvOverviewRow *row;
    const char *name;
    size_t len;
    int i;

    /* A person has no chip, whatever the view. With no perspective a bot is
       red; with one, the viewed player and its allies are green. */
    UT_ASSERT_MSG(lvBotChipFor(false, false, false, false) == LV_BOT_CHIP_NONE, "a person");
    UT_ASSERT_MSG(lvBotChipFor(false, true, true, true) == LV_BOT_CHIP_NONE, "a person viewed");
    UT_ASSERT_MSG(lvBotChipFor(true, false, false, false) == LV_BOT_CHIP_RED, "no perspective");
    UT_ASSERT_MSG(lvBotChipFor(true, false, true, true) == LV_BOT_CHIP_RED,
                  "no perspective: own and allied mean nothing");
    UT_ASSERT_MSG(lvBotChipFor(true, true, true, false) == LV_BOT_CHIP_GREEN, "the viewed bot");
    UT_ASSERT_MSG(lvBotChipFor(true, true, false, true) == LV_BOT_CHIP_GREEN, "an allied bot");
    UT_ASSERT_MSG(lvBotChipFor(true, true, false, false) == LV_BOT_CHIP_RED, "a hostile bot");

    /* A team of a person, a bot and a person: the names come back apart. */
    lvOverviewInit(&o);
    UT_ASSERT_MSG(lvOverviewAddPlayer(&o, 2, 5, "Alice"), "a person");
    UT_ASSERT_MSG(lvOverviewAddPlayerChip(&o, 2, 5, "Bot, the 1st", LV_BOT_CHIP_RED), "a bot");
    UT_ASSERT_MSG(lvOverviewAddPlayerChip(&o, 2, 5, "Carol", LV_BOT_CHIP_NONE), "a person");
    UT_ASSERT_MSG(lvOverviewAddPlayerChip(&o, 7, 1, "Lone", LV_BOT_CHIP_GREEN), "a bot alone");
    row = &o.rows[0];
    UT_ASSERT_MSG(strcmp(row->names, "Alice, Bot, the 1st, Carol") == 0, "the list is as before");
    UT_ASSERT_MSG(row->players == 3 && row->bots == 1, "one bot of three");
    UT_ASSERT_MSG(row->chip[0] == LV_BOT_CHIP_NONE && row->chip[1] == LV_BOT_CHIP_RED &&
                  row->chip[2] == LV_BOT_CHIP_NONE, "the chip of each");
    name = lvOverviewName(row, 0, &len);
    UT_ASSERT_MSG(name != NULL && len == 5 && memcmp(name, "Alice", 5) == 0, "first name");
    /* A name with the separator in it is still one name. */
    name = lvOverviewName(row, 1, &len);
    UT_ASSERT_MSG(name != NULL && len == 12 && memcmp(name, "Bot, the 1st", 12) == 0,
                  "second name");
    name = lvOverviewName(row, 2, &len);
    UT_ASSERT_MSG(name != NULL && len == 5 && memcmp(name, "Carol", 5) == 0, "last name");
    UT_ASSERT_MSG(lvOverviewName(row, 3, &len) == NULL && len == 0, "no fourth player");
    UT_ASSERT_MSG(lvOverviewName(row, -1, &len) == NULL, "no player before the first");
    row = &o.rows[1];
    UT_ASSERT_MSG(row->bots == 1 && row->chip[0] == LV_BOT_CHIP_GREEN, "the green chip is kept");
    name = lvOverviewName(row, 0, &len);
    UT_ASSERT_MSG(name != NULL && len == 4 && memcmp(name, "Lone", 4) == 0, "a team of one");

    /* A list too long to hold: the names that are cut have a length that
       stays inside the list, and those with no room have none. */
    lvOverviewInit(&o);
    {
        char longName[64];
        size_t total = 0;
        memset(longName, 'n', sizeof(longName) - 1);
        longName[sizeof(longName) - 1] = '\0';
        for (i = 0; i < 6; i++) {
            UT_ASSERT_MSG(lvOverviewAddPlayerChip(&o, 4, 4, longName, LV_BOT_CHIP_RED), "long");
        }
        row = &o.rows[0];
        UT_ASSERT_MSG(row->bots == 6, "six bots");
        for (i = 0; i < 6; i++) {
            name = lvOverviewName(row, i, &len);
            UT_ASSERT_MSG(name != NULL, "each has a place");
            UT_ASSERT_MSG(name >= row->names &&
                          name + len <= row->names + LV_OVERVIEW_NAMES_LEN - 1,
                          "inside the list");
            total += len;
        }
        name = lvOverviewName(row, 0, &len);
        UT_ASSERT_MSG(len == 63, "the first is whole");
        name = lvOverviewName(row, 5, &len);
        UT_ASSERT_MSG(len == 0, "the last had no room");
        UT_ASSERT_MSG(total <= LV_OVERVIEW_NAMES_LEN - 1, "never more than the list holds");
    }

    return 0;
}

/* A row of the Overview panel: the names, then a number and a picture for
   the pillboxes and a number and a picture for the bases. */
int run_lv_overview_row_layout(void) {
    float numX, iconX;
    bool ell;
    /* Three names 40, 30 and 20 wide; ", " is 8 and "..." is 12. */
    static const float w3[] = { 40.0f, 30.0f, 20.0f };
    static const float w1[] = { 40.0f };

    /* A count column: three digits (27), a gap (4), the picture (12). */
    UT_ASSERT_MSG(lvOverviewCountColW(27.0f, 12.0f, 4.0f) == 43.0f, "column width");
    UT_ASSERT_MSG(lvOverviewCountColW(-1.0f, 12.0f, 4.0f) == 16.0f, "no digits width");

    /* In the cell the picture is at the right end and the number ends a gap
       in front of it, whatever the number's width: the numbers of all rows
       end at one place. */
    lvOverviewCountPlace(43.0f, 9.0f, 12.0f, 4.0f, &numX, &iconX);
    UT_ASSERT_MSG(iconX == 31.0f && numX == 18.0f, "one digit: %g %g", (double)numX, (double)iconX);
    UT_ASSERT_MSG(numX + 9.0f == iconX - 4.0f, "one digit ends at the gap");
    lvOverviewCountPlace(43.0f, 18.0f, 12.0f, 4.0f, &numX, &iconX);
    UT_ASSERT_MSG(iconX == 31.0f && numX == 9.0f, "two digits");
    UT_ASSERT_MSG(numX + 18.0f == 27.0f, "two digits end where one digit ends");
    lvOverviewCountPlace(43.0f, 27.0f, 12.0f, 4.0f, &numX, &iconX);
    UT_ASSERT_MSG(iconX == 31.0f && numX == 0.0f, "three digits fill the column");
    UT_ASSERT_MSG(iconX + 12.0f == 43.0f, "the picture ends at the cell's end");
    /* A cell too small (it never is: the column is fixed): nothing goes
       left of the cell. */
    lvOverviewCountPlace(10.0f, 27.0f, 12.0f, 4.0f, &numX, &iconX);
    UT_ASSERT_MSG(numX == 0.0f && iconX == 0.0f, "small cell");
    lvOverviewCountPlace(43.0f, 9.0f, 12.0f, 4.0f, NULL, NULL);

    /* The whole list is 40 + 8 + 30 + 8 + 20 = 106. */
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 106.0f, &ell) == 3 && !ell,
                  "all three fit exactly");
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 500.0f, &ell) == 3 && !ell, "wide");
    /* One point short: "A, B, ..." is 40 + 8 + 30 + 8 + 12 = 98. */
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 105.0f, &ell) == 2 && ell,
                  "two names and the dots");
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 98.0f, &ell) == 2 && ell,
                  "two names and the dots, exactly");
    /* "A, ..." is 40 + 8 + 12 = 60. */
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 97.0f, &ell) == 1 && ell,
                  "one name and the dots");
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 60.0f, &ell) == 1 && ell,
                  "one name and the dots, exactly");
    /* Not even the first name whole: the caller cuts it. */
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 59.0f, &ell) == 0 && ell,
                  "no whole name");
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 0.0f, &ell) == 0 && ell, "no room");

    /* One player: the name, or the name cut. */
    UT_ASSERT_MSG(lvOverviewNamesFit(w1, 1, 8.0f, 12.0f, 40.0f, &ell) == 1 && !ell, "one fits");
    UT_ASSERT_MSG(lvOverviewNamesFit(w1, 1, 8.0f, 12.0f, 39.0f, &ell) == 0 && ell, "one is cut");

    /* No names. */
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 0, 8.0f, 12.0f, 100.0f, &ell) == 0 && !ell, "none");
    UT_ASSERT_MSG(lvOverviewNamesFit(NULL, 3, 8.0f, 12.0f, 100.0f, &ell) == 0 && !ell, "no list");
    UT_ASSERT_MSG(lvOverviewNamesFit(w3, 3, 8.0f, 12.0f, 105.0f, NULL) == 2, "no answer asked");
    return 0;
}

/* The Neutral row is in the table only while a pillbox or a base has no
   owner, and the table's height follows. */
int run_lv_overview_neutral_row(void) {
    LvOverview o;
    lvOverviewInit(&o);
    lvOverviewAddPlayer(&o, 0, 0, "A");
    lvOverviewAddPlayer(&o, 1, 1, "B");

    /* At the start: one pillbox and one base nobody owns. */
    lvOverviewCount(&o, false, false, -1);
    lvOverviewCount(&o, true, false, -1);
    UT_ASSERT_MSG(lvOverviewHasNeutral(&o), "both neutral");
    UT_ASSERT_MSG(lvOverviewTableRows(o.rowCount, lvOverviewHasNeutral(&o)) == 4,
                  "two teams, Neutral and the totals");

    /* Only a base is neutral: the row stays. */
    o.neutralPills = 0;
    UT_ASSERT_MSG(lvOverviewHasNeutral(&o), "a neutral base alone");
    /* Only a pillbox is neutral: the row stays. */
    o.neutralPills = 1;
    o.neutralBases = 0;
    UT_ASSERT_MSG(lvOverviewHasNeutral(&o), "a neutral pillbox alone");

    /* All of them taken: the row goes. */
    o.neutralPills = 0;
    UT_ASSERT_MSG(!lvOverviewHasNeutral(&o), "nothing neutral");
    UT_ASSERT_MSG(lvOverviewTableRows(o.rowCount, lvOverviewHasNeutral(&o)) == 3,
                  "two teams and the totals");

    /* A seek back to a time with a neutral base: the row is back. */
    o.neutralBases = 2;
    UT_ASSERT_MSG(lvOverviewHasNeutral(&o), "neutral again");
    UT_ASSERT_MSG(lvOverviewTableRows(o.rowCount, true) == 4, "the row is back");

    /* An empty table, and a count that cannot be. */
    lvOverviewInit(&o);
    UT_ASSERT_MSG(!lvOverviewHasNeutral(&o), "empty table");
    UT_ASSERT_MSG(lvOverviewTableRows(0, false) == 1, "the totals alone");
    UT_ASSERT_MSG(lvOverviewTableRows(-3, true) == 2, "no teams below 0");
    return 0;
}
