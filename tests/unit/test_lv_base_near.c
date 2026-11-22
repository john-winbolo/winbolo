/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * test_lv_base_near.c - the base the log viewer's Tank panel shows under the
 * selected tank's gauges (lv_base_near.h): picked as the game's
 * basesGetClosestForPlayer picks it, and its bars as long as the game draws
 * them. The last test is the game screen's base bars (lv_screenNearBaseStock
 * in screen.c): they show the same base as the Tank panel.
 */

#include <stdbool.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_bases.h"
#include "lv_players.h"
#include "lv_tank_status.h"
#include "lv_base_near.h"
#include "test_harness.h"

/* The world place of the middle of map square n. */
#define BN_MID(n) ((long)(n) * 256 + 128)

/* The nearest neutral or allied base on the map, strictly nearer than the
   range; an enemy's, or one off the map, is passed over even when nearer. */
int run_lv_base_near_pick(void) {
    LvBaseNearItem items[4] = {
        { true, true, 100, 100 },   /* 0: friendly, 3 squares away */
        { true, false, 98, 100 },   /* 1: an enemy's, 1 square away */
        { false, true, 97, 100 },   /* 2: not on the map, at the tank */
        { true, true, 101, 100 },   /* 3: friendly, 4 squares away */
    };
    const long tx = BN_MID(97), ty = BN_MID(100);

    UT_ASSERT(lvBaseNearPick(items, 4, tx, ty, 1792) == 0);
    /* With the near one gone, the next friendly one. */
    items[0].friendly = false;
    UT_ASSERT(lvBaseNearPick(items, 4, tx, ty, 1792) == 3);
    /* None friendly and on the map: none. */
    items[3].active = false;
    UT_ASSERT(lvBaseNearPick(items, 4, tx, ty, 1792) == LV_BASE_NEAR_NONE);
    UT_ASSERT(lvBaseNearPick(items, 0, tx, ty, 1792) == LV_BASE_NEAR_NONE);
    return 0;
}

/* The range is strict, as the game's is: a base at exactly the range is not
   shown, one a world unit nearer is. The distance is a straight line, and of
   two at the same distance the lower number wins. A range of 0 shows none. */
int run_lv_base_near_range(void) {
    LvBaseNearItem one[1] = { { true, true, 100, 100 } };
    LvBaseNearItem two[2] = { { true, true, 103, 100 }, { true, true, 97, 100 } };
    const long bx = BN_MID(100), by = BN_MID(100);

    /* 1792 = 7 squares, the classic BASE_STATUS_RANGE. */
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1792, by, 1792) == LV_BASE_NEAR_NONE);
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1791, by, 1792) == 0);
    UT_ASSERT(lvBaseNearPick(one, 1, bx, by - 1791, 1792) == 0);
    /* 1200 and 1300 across and down: 1769 in a straight line, in range,
       though each is under the range alone and their sum is not. */
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1200, by + 1300, 1792) == 0);
    /* 1300 and 1300: 1838, out of range. */
    UT_ASSERT(lvBaseNearPick(one, 1, bx + 1300, by + 1300, 1792) == LV_BASE_NEAR_NONE);
    UT_ASSERT(lvBaseNearPick(one, 1, bx, by, 0) == LV_BASE_NEAR_NONE);
    /* Three squares each way: the first wins. */
    UT_ASSERT(lvBaseNearPick(two, 2, bx, by, 1792) == 0);
    /* A 16th of a square nearer the second: the second. */
    UT_ASSERT(lvBaseNearPick(two, 2, bx - 16, by, 1792) == 1);
    return 0;
}

/* The bars: 60 pixels at the cap and in proportion under it; empty at 0, for
   a cap of 0, and never past the end of the well. */
int run_lv_base_near_bars(void) {
    UT_ASSERT(lvBaseBarWidth(90, 90, 60, 60) == 60.0f);
    UT_ASSERT(lvBaseBarWidth(45, 90, 60, 60) == 30.0f);
    UT_ASSERT(lvBaseBarWidth(0, 90, 60, 60) == 0.0f);
    UT_ASSERT(lvBaseBarWidth(10, 0, 60, 60) == 0.0f);
    UT_ASSERT(lvBaseBarWidth(200, 90, 60, 60) == 60.0f);
    UT_ASSERT(lvBaseBarWidth(20, 40, 60, 60) == 30.0f);
    return 0;
}

/* One base for the game screen test, as a recording puts it in. */
static void bnSetBase(LogViewerState *lv, BYTE num, BYTE x, BYTE y, BYTE owner,
                      BYTE shells, BYTE mines, BYTE armour) {
    base b;
    memset(&b, 0, sizeof(b));
    b.x = x; b.y = y; b.owner = owner;
    b.shells = shells; b.mines = mines; b.armour = armour;
    lv_basesSetBase(&lv->bs, &b, num);
}

/* The game screen's base bars show the stock of the base the Tank panel shows
   for the same tank: the nearest neutral or allied one in range, none while
   the tank is dead or far from every base. Player 1's tank is at the middle
   of square 97,100. Base 1 (player 2's) is 2 squares away, base 2 (neutral)
   3, base 3 (player 1's own) 5. */
int run_lv_base_near_game_screen(void) {
    LogViewerState *lv = lv_decoderCreate(false);
    LvTankStatus st;
    char location[1] = "";
    BYTE s = 99, m = 99, a = 99;
    bool near;

    UT_ASSERT(lv != NULL);
    /* The setup every load runs: the base store and an empty players table. */
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);
    lv_screenSetup();
    lv->logLoaded = TRUE;
    lv_playersSetPlayer(1, "One", location, 97, 100, 8, 8, 0, FALSE, 0, NULL, FALSE, FALSE, 0);
    lv_playersSetPlayer(2, "Two", location, 10, 10, 8, 8, 0, FALSE, 0, NULL, FALSE, FALSE, 0);
    lv_playersSetSelected(1);
    lv->gameViewHud[1].alive = TRUE;
    lv_basesSetNumBases(&lv->bs, 3);
    bnSetBase(lv, 1, 99, 100, 2, 5, 6, 7);
    bnSetBase(lv, 2, 100, 100, NEUTRAL, 40, 20, 10);
    bnSetBase(lv, 3, 102, 100, 1, 90, 90, 90);

    /* The enemy's base is nearer; the neutral one is shown. */
    near = lv_screenNearBaseStock(1, &s, &m, &a);
    UT_ASSERT_MSG(near && s == 40 && m == 20 && a == 10,
                  "neutral base: near %d, stock %u/%u/%u (want 40/20/10)", near, s, m, a);
    lv_screenTankStatusSnapshot(&st);
    UT_ASSERT_MSG(st.baseNear == 1 && st.baseNum == 2 &&
                  st.baseValue[LV_BASE_GAUGE_SHELLS] == s &&
                  st.baseValue[LV_BASE_GAUGE_MINES] == m &&
                  st.baseValue[LV_BASE_GAUGE_ARMOUR] == a,
                  "the Tank panel shows base %u near %u, not the game screen's",
                  st.baseNum, st.baseNear);

    /* Allied to player 2: player 2's base is now the nearest friendly one. */
    lv_playersAcceptAlliance(1, 2);
    near = lv_screenNearBaseStock(1, &s, &m, &a);
    UT_ASSERT_MSG(near && s == 5 && m == 6 && a == 7,
                  "allied base: near %d, stock %u/%u/%u (want 5/6/7)", near, s, m, a);
    lv_screenTankStatusSnapshot(&st);
    UT_ASSERT_MSG(st.baseNear == 1 && st.baseNum == 1, "the Tank panel shows base %u",
                  st.baseNum);

    /* Dead: no base, and the bars are empty. */
    lv->gameViewHud[1].alive = FALSE;
    s = m = a = 99;
    near = lv_screenNearBaseStock(1, &s, &m, &a);
    UT_ASSERT_MSG(!near && s == 0 && m == 0 && a == 0,
                  "dead tank: near %d, stock %u/%u/%u (want none)", near, s, m, a);
    lv_screenTankStatusSnapshot(&st);
    UT_ASSERT(st.baseNear == 0);

    /* Alive again, but 9 squares from the nearest base: none. */
    lv->gameViewHud[1].alive = TRUE;
    lv_playersUpdateTank(1, 90, 100, 8, 8, 0, FALSE);
    s = m = a = 99;
    near = lv_screenNearBaseStock(1, &s, &m, &a);
    UT_ASSERT_MSG(!near && s == 0 && m == 0 && a == 0,
                  "far tank: near %d, stock %u/%u/%u (want none)", near, s, m, a);
    lv_screenTankStatusSnapshot(&st);
    UT_ASSERT(st.baseNear == 0);

    /* A slot past the players names no tank. */
    UT_ASSERT(!lv_screenNearBaseStock(MAX_TANKS, &s, &m, &a));

    lv_decoderDestroy(lv);
    return 0;
}
