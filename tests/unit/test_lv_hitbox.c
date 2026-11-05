/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The hit areas the log viewer draws round each tank and each builder
 * (lv_hitbox.h).
 *
 *   lv_hitbox_sums — that the tank test is the engine's circle, a shell
 *       less than tank_hit_radius from the centre, with the edge itself a
 *       miss; that the builder's catch distance is 128, or pill_range when
 *       that is less; and that the builder test catches an explosion on
 *       open ground no more than that far from him, an explosion on a
 *       building, a half building, a pillbox or a base only on his own
 *       square, and nothing off the 3 by 3 squares round his.
 *       It also checks the places the circles go round: the middle of the
 *       art pixel a recording holds, with the builder's 1 and 2 put back,
 *       and the offsets the game draws the tank and builder pictures at.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lv_hitbox.h"
#include "test_harness.h"

/* The bit of a square in the 3 by 3 mask, with his own square at 1, 1. */
#define SQ(col, row) (1u << ((row) * 3 + (col)))

int run_lv_hitbox_sums(void) {
    /* The tank: the classic radius, 112. */
    UT_ASSERT_MSG(lvHitboxTankHit(0, 0, 112), "on the centre");
    UT_ASSERT_MSG(lvHitboxTankHit(111, 0, 112), "just inside, right");
    UT_ASSERT_MSG(lvHitboxTankHit(0, -111, 112), "just inside, up");
    UT_ASSERT_MSG(!lvHitboxTankHit(112, 0, 112), "on the edge is a miss");
    UT_ASSERT_MSG(!lvHitboxTankHit(-112, 0, 112), "on the edge, left");
    UT_ASSERT_MSG(lvHitboxTankHit(79, 79, 112), "inside, on the slant");
    UT_ASSERT_MSG(!lvHitboxTankHit(80, 80, 112), "outside, on the slant");
    UT_ASSERT_MSG(!lvHitboxTankHit(111, 111, 112), "a corner of the box is a miss");
    UT_ASSERT_MSG(lvHitboxTankHit(9, 0, 10) && !lvHitboxTankHit(10, 0, 10),
                  "another radius");

    /* The builder's catch distance. */
    UT_ASSERT_MSG(lvHitboxBuilderReach(2048) == 128, "classic pill range: half a square");
    UT_ASSERT_MSG(lvHitboxBuilderReach(128) == 128, "pill range of half a square");
    UT_ASSERT_MSG(lvHitboxBuilderReach(100) == 100, "a short pill range limits it");
    UT_ASSERT_MSG(lvHitboxBuilderReach(0) == 0, "pill range 0");

    /* Open ground all round: a circle of 128, the edge itself caught. */
    UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 128.0, 128.0, 128.0, 0, 128), "on him");
    UT_ASSERT_MSG(lvHitboxBuilderCaught(256.0, 128.0, 128.0, 128.0, 0, 128), "128 away");
    UT_ASSERT_MSG(!lvHitboxBuilderCaught(257.0, 128.0, 128.0, 128.0, 0, 128), "129 away");
    UT_ASSERT_MSG(lvHitboxBuilderCaught(-60.0, -60.0, 20.0, 20.0, 0, 128),
                  "in the square up and left");
    UT_ASSERT_MSG(!lvHitboxBuilderCaught(128.0, 128.0, 0.0, 0.0, 0, 128),
                  "181 away on the slant");

    /* A building to his right: open ground stops at its edge. */
    UT_ASSERT_MSG(lvHitboxBuilderCaught(255.0, 128.0, 128.0, 128.0, SQ(2, 1), 128),
                  "his side of the edge");
    UT_ASSERT_MSG(!lvHitboxBuilderCaught(256.0, 128.0, 128.0, 128.0, SQ(2, 1), 128),
                  "on the building");
    UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 256.0, 128.0, 128.0, SQ(2, 1), 128),
                  "the square under him is still open");

    /* His own square solid (he stands on a pillbox or a base): all of it
       catches, and no further than the circle elsewhere. */
    UT_ASSERT_MSG(lvHitboxBuilderCaught(250.0, 250.0, 128.0, 128.0, SQ(1, 1), 128),
                  "a far corner of his own square");
    UT_ASSERT_MSG(lvHitboxBuilderCaught(0.0, 0.0, 128.0, 128.0, SQ(1, 1), 128),
                  "the near corner of his own square");
    UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, -10.0, 128.0, 20.0, SQ(1, 1), 128),
                  "the open square above, inside the circle");
    UT_ASSERT_MSG(!lvHitboxBuilderCaught(128.0, -1.0, 128.0, 128.0, SQ(1, 1), 128),
                  "the open square above, outside the circle");

    /* A short pill range. */
    UT_ASSERT_MSG(lvHitboxBuilderCaught(228.0, 128.0, 128.0, 128.0, 0, 100), "100 away");
    UT_ASSERT_MSG(!lvHitboxBuilderCaught(229.0, 128.0, 128.0, 128.0, 0, 100), "101 away");
    UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 128.0, 128.0, 128.0, 0, 0),
                  "pill range 0: only on him");

    /* Off the 3 by 3 squares: never. */
    UT_ASSERT_MSG(!lvHitboxBuilderCaught(-300.0, 0.0, 0.0, 0.0, 0x1FFu, 128), "far left");
    UT_ASSERT_MSG(!lvHitboxBuilderCaught(0.0, 600.0, 0.0, 0.0, 0x1FFu, 128), "far down");

    /* The places. A tank recorded in art pixel 0 is 0 to 15 units into
       it; the middle is 7.5. */
    UT_ASSERT_MSG(lvHitboxTankPlace(0) == 7.5, "tank, pixel 0");
    UT_ASSERT_MSG(lvHitboxTankPlace(8) == 135.5, "tank, pixel 8");
    UT_ASSERT_MSG(lvHitboxTankPlace(15) == 247.5, "tank, pixel 15");
    /* The tank's picture: its top left 128 units before his place, so the
       top left drawn at screen pixel p (his place less 128, to the pixel)
       puts the circle 8 and a half pixels in: the middle of the 16. */
    UT_ASSERT_MSG(LV_HITBOX_TANK_HALF == 128, "half the tank picture");
    UT_ASSERT_MSG((lvHitboxTankPlace(0) + LV_HITBOX_TANK_HALF) / 16.0 == 8.46875,
                  "the circle is in the middle of the picture");
    /* A builder recorded in pixel 0 is 1 to 16 units in across, 2 to 17
       down (lgmGetPX takes 1 off x, lgmGetPY 2 off y). */
    UT_ASSERT_MSG(lvHitboxBuilderPlaceX(0) == 8.5, "builder x, pixel 0");
    UT_ASSERT_MSG(lvHitboxBuilderPlaceY(0) == 9.5, "builder y, pixel 0");
    UT_ASSERT_MSG(lvHitboxBuilderPlaceX(15) == 248.5, "builder x, pixel 15");
    UT_ASSERT_MSG(lvHitboxBuilderPlaceY(15) == 249.5,
                  "builder y, pixel 15: still in his own square");
    /* The builder's picture, as spritePositionLgm puts it in Classic: 1.5
       off, then rounded to a whole pixel, which is 1 on both axes. His
       3 by 4 picture then covers the pixel his place is in. */
    UT_ASSERT_MSG(LV_HITBOX_LGM_PICTURE_X == -1 && LV_HITBOX_LGM_PICTURE_Y == -1,
                  "builder picture offset");
    {
        double left = (double)(5 + LV_HITBOX_LGM_PICTURE_X) * 16.0;
        double top = (double)(5 + LV_HITBOX_LGM_PICTURE_Y) * 16.0;
        UT_ASSERT_MSG(lvHitboxBuilderPlaceX(5) > left &&
                      lvHitboxBuilderPlaceX(5) < left + 3.0 * 16.0,
                      "his place is inside his picture, across");
        UT_ASSERT_MSG(lvHitboxBuilderPlaceY(5) > top &&
                      lvHitboxBuilderPlaceY(5) < top + 4.0 * 16.0,
                      "his place is inside his picture, down");
    }

    return 0;
}
