/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * GoalHunter shield scan: shield cover from a friendly pill by its health
 * (knob PILL_COVER_BY_HEALTH, brains/GoalHunter/c/gh_shield_stamp.c).
 *
 *   naShieldPillCoverWalls   walls = floor(health / 5), the same rule as
 *                            brains/GoalHunter/pill_cover.lua
 *   naShieldPillSlotClass    a return-fire square with a pill on it: an
 *                            existing blocker, or skipped
 *   naShieldActualPillWalls  the walls the friendly pills in a blocker
 *                            subset give
 *
 * With pill_walls NULL (knob off) the scan must classify and count exactly
 * as before: every friendly pill is an existing blocker, nothing is skipped.
 */

#include <string.h>

#include "gh_shield_stamp.h"
#include "test_harness.h"

/* Same index as lgm_cache_idx in gh_shield_stamp.c. */
static int map_idx(int dx, int dy) {
    return (dx + 8) * 17 + (dy + 8);
}

int run_shield_pill_cover_walls(void) {
    uint8_t pmap[17 * 17];
    uint8_t walls[17 * 17];

    /* ── naShieldPillCoverWalls ── */
    UT_ASSERT_MSG(naShieldPillCoverWalls(15) == 3, "health 15 must give 3 walls");
    UT_ASSERT_MSG(naShieldPillCoverWalls(14) == 2, "health 14 must give 2 walls");
    UT_ASSERT_MSG(naShieldPillCoverWalls(10) == 2, "health 10 must give 2 walls");
    UT_ASSERT_MSG(naShieldPillCoverWalls(9)  == 1, "health 9 must give 1 wall");
    UT_ASSERT_MSG(naShieldPillCoverWalls(7)  == 1, "health 7 must give 1 wall");
    UT_ASSERT_MSG(naShieldPillCoverWalls(5)  == 1, "health 5 must give 1 wall");
    UT_ASSERT_MSG(naShieldPillCoverWalls(4)  == 0, "health 4 must give 0 walls");
    UT_ASSERT_MSG(naShieldPillCoverWalls(3)  == 0, "health 3 must give 0 walls");
    UT_ASSERT_MSG(naShieldPillCoverWalls(0)  == 0, "health 0 must give 0 walls");
    UT_ASSERT_MSG(naShieldPillCoverWalls(-1) == 0, "health -1 must give 0 walls");

    /* A map the way l_scan_c builds it with the knob on:
     *   (1,1) own/allied pill health 15 -> 3 walls
     *   (2,1) own/allied pill health 10 -> 2 walls
     *   (3,1) own/allied pill health 7  -> 1 wall
     *   (4,1) own/allied pill health 3  -> 0 walls
     *   (5,1) enemy pill health 15      -> 0 walls (owner, not health)
     *   (6,1) neutral pill health 15    -> 0 walls
     *   (7,1) dead pill (not in pill_map, arg 26) -> 0 walls
     *   everything else: no pill */
    memset(pmap, 0, sizeof(pmap));
    memset(walls, NA_SHIELD_NO_PILL, sizeof(walls));
    pmap[map_idx(1, 1)] = 2; walls[map_idx(1, 1)] = (uint8_t)naShieldPillCoverWalls(15);
    pmap[map_idx(2, 1)] = 2; walls[map_idx(2, 1)] = (uint8_t)naShieldPillCoverWalls(10);
    pmap[map_idx(3, 1)] = 2; walls[map_idx(3, 1)] = (uint8_t)naShieldPillCoverWalls(7);
    pmap[map_idx(4, 1)] = 2; walls[map_idx(4, 1)] = (uint8_t)naShieldPillCoverWalls(3);
    pmap[map_idx(5, 1)] = 1; walls[map_idx(5, 1)] = 0;
    pmap[map_idx(6, 1)] = 1; walls[map_idx(6, 1)] = 0;
    walls[map_idx(7, 1)] = 0;

    /* ── naShieldPillSlotClass, knob on ── */
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 1, 1) == 1, "friendly 15 must be a blocker");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 2, 1) == 1, "friendly 10 must be a blocker");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 3, 1) == 1, "friendly 7 must be a blocker");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 4, 1) == 2,
                  "friendly 3 (0 walls) must be skipped, not a blocker");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 5, 1) == 2, "enemy pill must be skipped");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 6, 1) == 2, "neutral pill must be skipped");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 7, 1) == 2, "dead pill must be skipped");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 0, 3) == 0,
                  "a square with no pill goes to the terrain test");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, walls, 12, 0) == 0,
                  "a square outside the window goes to the terrain test");

    /* ── naShieldPillSlotClass, knob off (pill_walls NULL): the old rule ── */
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, NULL, 1, 1) == 1, "off: friendly 15 is a blocker");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, NULL, 4, 1) == 1, "off: friendly 3 is a blocker");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, NULL, 5, 1) == 0,
                  "off: enemy pill goes to the terrain test, as before");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, NULL, 7, 1) == 0,
                  "off: dead pill goes to the terrain test, as before");
    UT_ASSERT_MSG(naShieldPillSlotClass(pmap, NULL, 0, 3) == 0, "off: no pill, terrain test");

    /* ── naShieldActualPillWalls ── */
    {
        /* Subset: friendly 15 (actual), friendly 7 (actual), a wall at
         * (0,3) (actual), enemy pill (5,1) (actual, as the old scan could
         * list it via terrain), friendly 10 listed as potential (ignored). */
        const int8_t dx[]  = { 1, 3, 0, 5, 2 };
        const int8_t dy[]  = { 1, 1, 3, 1, 1 };
        const int    act[] = { 1, 1, 1, 1, 0 };
        int np = -1;
        int w = naShieldActualPillWalls(pmap, walls, dx, dy, act, 5, &np);
        UT_ASSERT_MSG(np == 2, "two friendly pills among the existing blockers");
        UT_ASSERT_MSG(w == 4, "friendly 15 + friendly 7 = 3 + 1 walls");

        /* Knob off: the same pill count as the old loop, no walls. */
        np = -1;
        w = naShieldActualPillWalls(pmap, NULL, dx, dy, act, 5, &np);
        UT_ASSERT_MSG(np == 2, "off: same friendly pill count as before");
        UT_ASSERT_MSG(w == 0, "off: no walls sum");

        /* So the C scorer's welt for this subset (4 existing blockers, 2 of
         * them friendly pills) with pill_wall_equiv 3 is, knob off,
         * 2 + 2 * 3 = 8 walls and, knob on, 2 + 4 = 6 walls. */

        /* A single 10-health pill: 2 walls (10 shots), one wall short of
         * the 15-shot cover target. */
        {
            const int8_t ex[] = { 2 };
            const int8_t ey[] = { 1 };
            const int    ea[] = { 1 };
            w = naShieldActualPillWalls(pmap, walls, ex, ey, ea, 1, &np);
            UT_ASSERT_MSG(np == 1 && w == 2, "friendly 10 alone = 2 walls");
        }
        /* NULL n_pill is allowed. */
        w = naShieldActualPillWalls(pmap, walls, dx, dy, act, 5, NULL);
        UT_ASSERT_MSG(w == 4, "NULL n_pill must still sum the walls");
    }
    return 0;
}
