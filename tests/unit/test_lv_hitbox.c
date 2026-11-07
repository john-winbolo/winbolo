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
 *       building, a half building, a live pillbox or a base only on his own
 *       square, and nothing off the 3 by 3 squares round his. A dead
 *       pillbox's square is open ground (lvHitboxPillSolid). A square a
 *       shell's hit changed on this frame is solid (lvHitboxSquareSolid).
 *       It also checks the places the circles go round: the middle of the
 *       art pixel a recording holds, with the builder's 1 and 2 put back,
 *       and the offsets the game draws the tank and builder pictures at.
 *
 *   lv_hitbox_lgm_fit — that the closer guess at a walking builder's place
 *       (lvHitboxLgmFitRange) always holds his true place, is never wider
 *       than his newest pixel, is that whole pixel for a builder standing
 *       still, and is closer than the middle of the pixel on a walk. Also
 *       a real walk, player 5's builder in a recording, and his own square
 *       at a square's left and top edge (lvHitboxBuilderOwnSquare), which
 *       is the engine's square from his true place.
 */

#include <stdbool.h>
#include <stddef.h>
#include <math.h>

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

    /* The lgm_blast_shape rule: 0 the circle above, 1 the Mac Bolo box,
       2 a circle of 144. pill_range limits all three. */
    UT_ASSERT_MSG(lvHitboxBuilderReachForShape(0, 2048) == 128, "shape 0 reach");
    UT_ASSERT_MSG(lvHitboxBuilderReachForShape(0, 100) == 100, "shape 0, short pill range");
    UT_ASSERT_MSG(lvHitboxBuilderReachForShape(1, 2048) == 128, "shape 1 reach");
    UT_ASSERT_MSG(lvHitboxBuilderReachForShape(1, 100) == 100, "shape 1, short pill range");
    UT_ASSERT_MSG(lvHitboxBuilderReachForShape(2, 2048) == 144, "shape 2 reach");
    UT_ASSERT_MSG(lvHitboxBuilderReachForShape(2, 100) == 100, "shape 2, short pill range");
    UT_ASSERT_MSG(lvHitboxBuilderReachForShape(2, -1) == 0, "pill range under 0");
    {
        /* The builder in the middle of his square; the same points as
           test_lgm_blast_shape.c: across at 127/128/129 and 143/144/145,
           and on the diagonal at 100 and 120. */
        static const struct { double dx, dy; bool dies[3]; } rows[] = {
            { 127.0,   0.0, { true,  true,  true  } },
            { 128.0,   0.0, { true,  false, true  } },
            { 129.0,   0.0, { false, false, true  } },
            { 143.0,   0.0, { false, false, true  } },
            { 144.0,   0.0, { false, false, true  } },
            { 145.0,   0.0, { false, false, false } },
            { 100.0, 100.0, { false, true,  true  } },
            { 120.0, 120.0, { false, true,  false } },
        };
        int shape, i, q;
        for (shape = 0; shape < 3; shape++) {
            for (i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++) {
                for (q = 0; q < 4; q++) {
                    double dx = rows[i].dx * ((q & 1) ? -1.0 : 1.0);
                    double dy = rows[i].dy * ((q & 2) ? -1.0 : 1.0);
                    bool got = lvHitboxBuilderCaughtShape(128.0 + dx, 128.0 + dy,
                                                          128.0, 128.0, 0, shape,
                                                          2048);
                    UT_ASSERT_MSG(got == rows[i].dies[shape],
                                  "shape %d at (%g,%g): %s, wanted %s", shape,
                                  dx, dy, got ? "caught" : "missed",
                                  rows[i].dies[shape] ? "caught" : "missed");
                }
            }
        }
        /* A solid square still catches only on his own square. */
        UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(256.0, 128.0, 200.0, 128.0,
                                                  SQ(2, 1), 1, 2048),
                      "shape 1: a building next door catches nobody");
        UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(250.0, 250.0, 128.0, 128.0,
                                                 SQ(1, 1), 2, 2048),
                      "shape 2: a building on his square catches him");
    }
    {
        /* pill_range limits every shape, as lgmBlastOpenGroundCatches: the
           box at 150 loses its corners, shape 2 at 100 is a circle of 100. */
        UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(255.0, 128.0, 128.0, 128.0, 0, 1, 150),
                      "box, pill range 150: 127 across");
        UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(228.0, 228.0, 128.0, 128.0, 0, 1, 150),
                      "box, pill range 150: (100,100) is 141 away");
        UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(238.0, 238.0, 128.0, 128.0, 0, 1, 150),
                      "box, pill range 150: (110,110) is 156 away");
        UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(228.0, 128.0, 128.0, 128.0, 0, 2, 100),
                      "shape 2, pill range 100: 100 across");
        UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(229.0, 128.0, 128.0, 128.0, 0, 2, 100),
                      "shape 2, pill range 100: 101 across");
        UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(128.0, 128.0, 128.0, 128.0, 0, 1, -1),
                      "pill range under 0 catches nobody");
    }

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

    /* Which pillboxes make a square solid: only a live one on the map. */
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 15, false), "a live pill is solid");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 1, false), "armour 1 is solid");
    UT_ASSERT_MSG(!lvHitboxPillSolid(true, false, 0, false), "a dead pill is open ground");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 0, true),
                  "the frame of the hit that killed a pill: its square is solid");
    UT_ASSERT_MSG(!lvHitboxPillSolid(true, true, 15, false), "a pill in a tank is not there");
    UT_ASSERT_MSG(!lvHitboxPillSolid(false, false, 15, false), "a removed pill is not there");

    /* A dead pill to his right is open ground: an explosion just across the
       edge, 10 from him, catches him; the same explosion with a live pill
       there does not. */
    {
        unsigned deadMask = lvHitboxPillSolid(true, false, 0, false) ? SQ(2, 1) : 0;
        unsigned liveMask = lvHitboxPillSolid(true, false, 15, false) ? SQ(2, 1) : 0;
        UT_ASSERT_MSG(lvHitboxBuilderCaught(260.0, 128.0, 250.0, 128.0, deadMask, 128),
                      "a dead pill's square next to him catches across the edge");
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(260.0, 128.0, 250.0, 128.0, liveMask, 128),
                      "a live pill's square next to him does not");
    }

    /* The shot that kills a pill on his own square: the blast at the square's
       middle catches him in its far corner, 178 away, because the pill was
       hit on this frame. A dead pill there with no hit this frame is open
       ground and the same blast misses him. */
    {
        unsigned hitMask  = lvHitboxPillSolid(true, false, 0, true) ? SQ(1, 1) : 0;
        unsigned deadMask = lvHitboxPillSolid(true, false, 0, false) ? SQ(1, 1) : 0;
        UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, hitMask, 128),
                      "the killing shot catches him in the far corner of the pill's square");
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, deadMask, 128),
                      "a blast on a dead pill's square with no hit misses him there");
    }

    /* Which terrain makes a square solid: a building or a half building,
       and a square a shell's hit changed on this frame (a wall to rubble on
       its last hit, a forest to grass, a boat to river, the shore or a
       bridge a boat's shell hit). */
    UT_ASSERT_MSG(lvHitboxSquareSolid(true, false), "a wall is solid");
    UT_ASSERT_MSG(lvHitboxSquareSolid(false, true),
                  "the frame of a shell's hit that changed the square: it is solid");
    UT_ASSERT_MSG(!lvHitboxSquareSolid(false, false),
                  "rubble, grass or river with no hit this frame is open ground");

    /* The shot that turns a wall to rubble, on the square to his left: the
       blast at that square's middle, 128 from him on the first world unit of
       his own square, does not catch him, as
       for any hit on a wall. The same blast on rubble with no hit this frame
       does. And the same shot on his own square catches him in its far
       corner, 178 away. */
    {
        unsigned leftHit   = lvHitboxSquareSolid(false, true) ? SQ(0, 1) : 0;
        unsigned leftOpen  = lvHitboxSquareSolid(false, false) ? SQ(0, 1) : 0;
        unsigned ownHit    = lvHitboxSquareSolid(false, true) ? SQ(1, 1) : 0;
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(-128.0, 128.0, 0.0, 128.0, leftHit, 128),
                      "the rubble shot on the next square misses him 128 away");
        UT_ASSERT_MSG(lvHitboxBuilderCaught(-128.0, 128.0, 0.0, 128.0, leftOpen, 128),
                      "a blast on rubble with no hit catches him 128 away");
        UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, ownHit, 128),
                      "the rubble shot catches him in the far corner of its square");
    }

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

int run_lv_hitbox_lgm_fit(void) {
    int back[LV_HITBOX_LGM_FIT_MAX], pix[LV_HITBOX_LGM_FIT_MAX];
    double lo, hi, sumFit = 0.0, sumMiddle = 0.0;
    int i, v, x0, cases = 0;

    for (i = 0; i < LV_HITBOX_LGM_FIT_MAX; i++) {
        back[i] = i;
    }

    /* Every steady walk from -20 to 20 world units an entry, from each of
       the 16 places in a pixel: the true place is always in the range, and
       the range is always in the newest pixel. */
    for (v = -20; v <= 20; v++) {
        for (x0 = 4000; x0 < 4016; x0++) {
            for (i = 0; i < LV_HITBOX_LGM_FIT_MAX; i++) {
                int at = x0 - v * back[i];
                pix[i] = at >= 0 ? at / 16 : -((-at + 15) / 16);
            }
            UT_ASSERT(lvHitboxLgmFitRange(back, pix, LV_HITBOX_LGM_FIT_MAX, &lo, &hi) >= 1);
            UT_ASSERT_MSG(lo <= (double)x0 + 1e-6 && hi >= (double)x0 - 1e-6,
                          "speed %d place %d: range %.3f to %.3f misses it", v, x0, lo, hi);
            UT_ASSERT_MSG(lo >= (double)(pix[0] * 16) - 1e-6 &&
                          hi <= (double)(pix[0] * 16 + 15) + 1e-6,
                          "speed %d place %d: range %.3f to %.3f leaves the pixel", v, x0,
                          lo, hi);
            if (v == 0) {
                UT_ASSERT_MSG(lo == (double)(pix[0] * 16) && hi == (double)(pix[0] * 16 + 15),
                              "standing still: range %.3f to %.3f, want the pixel", lo, hi);
            }
            sumFit += fabs((lo + hi) * 0.5 - (double)x0);
            sumMiddle += fabs((double)(pix[0] * 16) + 7.5 - (double)x0);
            cases++;
        }
    }
    UT_ASSERT_MSG(sumFit / cases < 2.0 && sumMiddle / cases == 4.0,
                  "mean miss %.3f with the fit, %.3f with the middle of the pixel",
                  sumFit / cases, sumMiddle / cases);

    /* A walk that turns: 12 entries east at 10 an entry, then 12 still. Only
       the still part is one line, so the range is the whole pixel. */
    for (i = 0; i < LV_HITBOX_LGM_FIT_MAX; i++) {
        int at = (i < 12) ? 5000 : 5000 - 10 * (i - 11);
        pix[i] = at / 16;
    }
    UT_ASSERT(lvHitboxLgmFitRange(back, pix, LV_HITBOX_LGM_FIT_MAX, &lo, &hi) < LV_HITBOX_LGM_FIT_MAX);
    UT_ASSERT_MSG(lo <= 5000.0 && hi >= 5000.0, "after the turn: %.3f to %.3f", lo, hi);

    /* One pixel: that pixel. */
    pix[0] = 300;
    UT_ASSERT(lvHitboxLgmFitRange(back, pix, 1, &lo, &hi) == 1);
    UT_ASSERT(lo == 4800.0 && hi == 4815.0);

    /* Player 5's builder in 20261106t193730_BuzzSaw.wbv at tick 54330, the
       place the server tested him at for player 3's shell, and the 17
       entries before it, newest first. His art pixel across and down: */
    {
        static const int px[18] = {1972, 1973, 1974, 1975, 1976, 1977, 1978, 1979, 1979,
                                   1980, 1981, 1982, 1983, 1984, 1985, 1986, 1986, 1987};
        static const int py[18] = {2176, 2176, 2177, 2177, 2178, 2178, 2179, 2179, 2180,
                                   2181, 2181, 2182, 2182, 2183, 2183, 2184, 2184, 2185};
        double wx, wy;
        UT_ASSERT(lvHitboxLgmFitRange(back, px, 18, &lo, &hi) == 18);
        wx = (lo + hi) * 0.5 + LV_HITBOX_LGM_OFFSET_X;
        UT_ASSERT_MSG(lo > 31562.0 && lo < 31563.0 && hi == 31567.0,
                      "across: %.3f to %.3f", lo, hi);
        UT_ASSERT(lvHitboxLgmFitRange(back, py, 18, &lo, &hi) == 18);
        wy = (lo + hi) * 0.5 + LV_HITBOX_LGM_OFFSET_Y;
        UT_ASSERT_MSG(lo == 34816.0 && hi > 34824.0 && hi < 34824.2,
                      "down: %.3f to %.3f", lo, hi);
        /* The middle of the pixel was 31560.5, 34825.5. */
        UT_ASSERT_MSG(fabs(wx - 31565.85) < 0.1 && fabs(wy - 34822.03) < 0.1,
                      "his place %.3f, %.3f", wx, wy);
    }

    /* His own square at a square's left and top edge. The engine's square
       is lgmWorldX >> 8; the recorded one is from his place less 1 across
       and 2 down. A builder at x 1280 (square 5's left edge) and y 1281 is
       recorded in square 4, pixel 15, on both axes, so his place from
       square 4 is 256 and 257, and the square has to move along to 5. */
    {
        double b;
        int sq;
        b = 256.0; sq = 4;
        lvHitboxBuilderOwnSquare(&b, &sq);
        UT_ASSERT_MSG(sq == (1280 >> 8) && b == 0.0, "x at the left edge: square %d, %.3f", sq, b);
        b = 257.0; sq = 4;
        lvHitboxBuilderOwnSquare(&b, &sq);
        UT_ASSERT_MSG(sq == (1281 >> 8) && b == 1.0, "y at the top edge: square %d, %.3f", sq, b);
        b = 255.5; sq = 4;
        lvHitboxBuilderOwnSquare(&b, &sq);
        UT_ASSERT_MSG(sq == 4 && b == 255.5, "inside the square: no move");
        b = -0.5; sq = 4;
        lvHitboxBuilderOwnSquare(&b, &sq);
        UT_ASSERT_MSG(sq == 3 && b == 255.5, "before the square: square %d, %.3f", sq, b);
        b = 300.0; sq = 255;
        lvHitboxBuilderOwnSquare(&b, &sq);
        UT_ASSERT_MSG(sq == 255 && b == 300.0, "the map's last square stays");
    }

    /* Walks that end 0 to 3 units past square 5's left or top edge, east
       or south at 4 to 16 units an entry. Recorded as the engine records
       them (place less 1 across, 2 down, to the pixel), fitted, and then
       given his own square: the square is always the one the guess is in,
       the place is 0 to 255 into it, and some of the walks need the move. */
    {
        int axis, edge, moved = 0;
        for (axis = 0; axis < 2; axis++) {
            int off = axis == 0 ? LV_HITBOX_LGM_OFFSET_X : LV_HITBOX_LGM_OFFSET_Y;
            for (v = 4; v <= 16; v++) {
                for (edge = 0; edge < 4; edge++) {
                    int truth = 5 * LV_HITBOX_WU_PER_SQUARE + edge;
                    double guess, b;
                    int sq, n;
                    for (i = 0; i < LV_HITBOX_LGM_FIT_MAX; i++) {
                        pix[i] = (truth - v * back[i] - off) / 16;
                    }
                    n = lvHitboxLgmFitRange(back, pix, LV_HITBOX_LGM_FIT_MAX, &lo, &hi);
                    UT_ASSERT(n >= 2);
                    guess = (lo + hi) * 0.5 + off;
                    /* The recorded square. */
                    sq = pix[0] / 16;
                    b = guess - (double)(sq * LV_HITBOX_WU_PER_SQUARE);
                    lvHitboxBuilderOwnSquare(&b, &sq);
                    UT_ASSERT_MSG(sq == (int)floor(guess / LV_HITBOX_WU_PER_SQUARE) &&
                                  b >= 0.0 && b < (double)LV_HITBOX_WU_PER_SQUARE,
                                  "axis %d speed %d edge %d: guess %.3f, square %d, %.3f",
                                  axis, v, edge, guess, sq, b);
                    if (sq != pix[0] / 16) {
                        moved++;
                        UT_ASSERT_MSG(sq == truth >> 8, "moved to the engine's square");
                    }
                }
            }
        }
        UT_ASSERT_MSG(moved > 0, "no walk needed the move");
    }

    return 0;
}
