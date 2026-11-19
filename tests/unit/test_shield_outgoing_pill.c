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
 * GoalHunter shield scan: a pill on our own outgoing shell path blocks the
 * aim (brains/GoalHunter/c/gh_shield_stamp.c, naShieldOutgoingPillBlocks,
 * called from score_one_aim).
 *
 * Before the fix the C scan skipped this check for every aim (target_is_pill
 * was always 1), so a pill of any owner on the line never blocked an aim.
 * A shell hits a live pill of any owner, so own, allied, enemy and neutral
 * pills must all block. The target pill's own square (0,0) never blocks, and
 * squares after it are not part of the outgoing path.
 */

#include <string.h>

#include "gh_shield_stamp.h"
#include "test_harness.h"

/* Same index as lgm_cache_idx in gh_shield_stamp.c. */
static int map_idx(int dx, int dy) {
    return (dx + 8) * 17 + (dy + 8);
}

int run_shield_outgoing_pill_blocks(void) {
    uint8_t map[17 * 17];
    /* A path from the south-east into the target: (4,6) ... (1,1), then the
     * target square, then one square past it. */
    const int8_t dx[] = { 4, 3, 2, 1, 0, -1 };
    const int8_t dy[] = { 6, 4, 3, 1, 0, -1 };
    const int n = (int)(sizeof(dx) / sizeof(dx[0]));

    memset(map, 0, sizeof(map));
    UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, dx, dy, n) == 0,
                  "no pill anywhere must not block");

    /* Own or allied pill (map value 2) on the path blocks. */
    memset(map, 0, sizeof(map));
    map[map_idx(1, 1)] = 2;
    UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, dx, dy, n) == 1,
                  "own or allied pill on the path must block");

    /* Enemy or neutral pill (map value 1) on the path blocks. */
    memset(map, 0, sizeof(map));
    map[map_idx(3, 4)] = 1;
    UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, dx, dy, n) == 1,
                  "enemy or neutral pill on the path must block");

    /* The target pill's own square never blocks. */
    memset(map, 0, sizeof(map));
    map[map_idx(0, 0)] = 1;
    UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, dx, dy, n) == 0,
                  "the target pill must not block itself");

    /* A pill past the target is not on the outgoing path. */
    memset(map, 0, sizeof(map));
    map[map_idx(-1, -1)] = 2;
    UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, dx, dy, n) == 0,
                  "a pill past the target must not block");

    /* A pill next to the path but not on it does not block. */
    memset(map, 0, sizeof(map));
    map[map_idx(1, 0)] = 2;
    UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, dx, dy, n) == 0,
                  "a pill beside the path must not block");

    /* Squares outside the 17x17 window are ignored, not read out of range. */
    {
        const int8_t fx[] = { 12, 9, 2 };
        const int8_t fy[] = { 0, 0, 0 };
        memset(map, 0, sizeof(map));
        UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, fx, fy, 3) == 0,
                      "squares outside the map window must be ignored");
        map[map_idx(2, 0)] = 1;
        UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, fx, fy, 3) == 1,
                      "a pill inside the window after far squares must block");
    }

    /* Empty path. */
    memset(map, 0xFF, sizeof(map));
    UT_ASSERT_MSG(naShieldOutgoingPillBlocks(map, dx, dy, 0) == 0,
                  "an empty path must not block");
    return 0;
}
