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
 *       that is less (144 for the area circle, and the box's corner cut);
 *       and that the builder test catches an explosion on open ground no
 *       more than that far from him (in the box: less than 128 on each axis), an explosion on a
 *       building, a half building, a live pillbox or a base only on his own
 *       square, and nothing off the 3 by 3 squares round his. A dead
 *       pillbox's square is open ground (lvHitboxPillSolid). A square a
 *       shell's hit changed on this frame is solid (lvHitboxSquareSolid)
 *       in a game that plays the square-hit rule.
 *       It also checks the places the circles go round: the middle of the
 *       art pixel a recording holds, with the builder's 1 and 2 put back,
 *       and the offsets the game draws the tank and builder pictures at.
 *
 *   lv_hitbox_new_blast — which explosions of a step get the dot
 *       (lvHitboxNewBlasts): each one on its first recorded step only,
 *       whether that step shows frame 8 or frame 7, and not again when
 *       frame 8 lasts two steps; a new explosion on an old one's place is
 *       new; with the step before unknown, or too big, none is new.
 *
 *   lv_hitbox_lgm_fit — that the closer guess at a walking builder's place
 *       (lvHitboxLgmFitRange) always holds his true place, is never wider
 *       than his newest pixel, is that whole pixel for a builder standing
 *       still, and is closer than the middle of the pixel on a walk. Also
 *       a real walk, player 5's builder in a recording, and his own square
 *       at a square's left and top edge (lvHitboxBuilderOwnSquare), which
 *       is the engine's square from his true place.
 *
 *   lv_hitbox_lgm_pick - that the lag view fits a builder at the entry its
 *       frame's time gives, not the newest entry in the same pixel.
 *
 *   lv_hitbox_outline - that every line of a builder's outline is on the
 *       border of his area, for each of the three lgm_blast_shape shapes,
 *       that the outline stops exactly where the circle meets the edge of a
 *       building, and that the Mac Bolo box is drawn on its four sides.
 *
 *   lv_hitbox_square_hit_by_version - that the square a shell's hit changed
 *       on this frame is solid only in a recording of 2.2.2 or later
 *       (lvHitboxSquareHitForVersion). In a 2.2.1 recording it is
 *       open ground, as the hit box drew it before that rule: only a wall
 *       is solid.
 *       A pillbox the same way: in 2.2.1 every pill on the map is solid,
 *       dead or alive; in 2.2.2 a dead pill's square is open ground.
 *
 *   The tank test is also checked for the square of games before the
 *   circle, and against the size the outline is drawn at.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include "lv_hitbox.h"
#include "test_harness.h"

#define HS   LGM_BLAST_SHAPE_HALF_SQUARE
#define BOX  LGM_BLAST_SHAPE_MAC_BOX
#define AREA LGM_BLAST_SHAPE_MAC_AREA

/* The bit of a square in the 3 by 3 mask, with his own square at 1, 1. */
#define SQ(col, row) (1u << ((row) * 3 + (col)))

int run_lv_hitbox_sums(void) {
    /* The tank: the classic radius, 112. */
    UT_ASSERT_MSG(lvHitboxTankHit(0, 0, LV_HITBOX_TANK_CIRCLE, 112), "on the centre");
    UT_ASSERT_MSG(lvHitboxTankHit(111, 0, LV_HITBOX_TANK_CIRCLE, 112), "just inside, right");
    UT_ASSERT_MSG(lvHitboxTankHit(0, -111, LV_HITBOX_TANK_CIRCLE, 112), "just inside, up");
    UT_ASSERT_MSG(!lvHitboxTankHit(112, 0, LV_HITBOX_TANK_CIRCLE, 112), "on the edge is a miss");
    UT_ASSERT_MSG(!lvHitboxTankHit(-112, 0, LV_HITBOX_TANK_CIRCLE, 112), "on the edge, left");
    UT_ASSERT_MSG(lvHitboxTankHit(79, 79, LV_HITBOX_TANK_CIRCLE, 112), "inside, on the slant");
    UT_ASSERT_MSG(!lvHitboxTankHit(80, 80, LV_HITBOX_TANK_CIRCLE, 112), "outside, on the slant");
    UT_ASSERT_MSG(!lvHitboxTankHit(111, 111, LV_HITBOX_TANK_CIRCLE, 112), "a corner of the box is a miss");
    UT_ASSERT_MSG(lvHitboxTankHit(9, 0, LV_HITBOX_TANK_CIRCLE, 10) && !lvHitboxTankHit(10, 0, LV_HITBOX_TANK_CIRCLE, 10),
                  "another radius");
    /* The square of games before the circle: less than 128 on both axes,
       the corners too, whatever the radius rule. */
    UT_ASSERT_MSG(lvHitboxTankHit(127, -127, LV_HITBOX_TANK_SQUARE, 112), "a corner of the square");
    UT_ASSERT_MSG(!lvHitboxTankHit(128, 0, LV_HITBOX_TANK_SQUARE, 112), "on the square's edge");
    UT_ASSERT_MSG(!lvHitboxTankHit(0, 0, LV_HITBOX_TANK_NONE, 112), "no zone, no hit");

    /* The drawn size is the line where the tested function starts to miss:
       every place in a whole unit inside it hits, and every place on it or
       past it misses. */
    {
        static const int zones[2] = {LV_HITBOX_TANK_CIRCLE, LV_HITBOX_TANK_SQUARE};
        int z, dx, dy;
        UT_ASSERT(lvHitboxTankZoneSize(LV_HITBOX_TANK_CIRCLE, 112) == 112);
        UT_ASSERT(lvHitboxTankZoneSize(LV_HITBOX_TANK_SQUARE, 112) == 128);
        UT_ASSERT(lvHitboxTankZoneSize(LV_HITBOX_TANK_NONE, 112) == 0);
        for (z = 0; z < 2; z++) {
            int size = lvHitboxTankZoneSize(zones[z], 112);
            for (dy = -140; dy <= 140; dy++) {
                for (dx = -140; dx <= 140; dx++) {
                    bool hit = lvHitboxTankHit(dx, dy, zones[z], 112);
                    double d = (zones[z] == LV_HITBOX_TANK_CIRCLE)
                                   ? sqrt((double)(dx * dx + dy * dy))
                                   : (double)((abs(dx) > abs(dy)) ? abs(dx) : abs(dy));
                    if (d <= (double)size - 1.0) {
                        UT_ASSERT_MSG(hit, "zone %d: %d,%d is inside the drawn line", zones[z],
                                      dx, dy);
                    }
                    if (d >= (double)size) {
                        UT_ASSERT_MSG(!hit, "zone %d: %d,%d is on or past the drawn line",
                                      zones[z], dx, dy);
                    }
                }
            }
        }
    }
    /* The zone of each game. */
#ifndef BOLO_LEGACY_SQUARE_COLLISION
    UT_ASSERT(lvHitboxTankZoneFor(1, 9, 1, false) == LV_HITBOX_TANK_SQUARE);
    UT_ASSERT(lvHitboxTankZoneFor(1, 9, 2, false) == LV_HITBOX_TANK_CIRCLE);
    UT_ASSERT(lvHitboxTankZoneFor(2, 0, 0, false) == LV_HITBOX_TANK_CIRCLE);
    UT_ASSERT(lvHitboxTankZoneFor(1, 9, 1, true) == LV_HITBOX_TANK_CIRCLE);
#endif
    UT_ASSERT(lvHitboxTankZoneFor(0, 0, 0, false) == LV_HITBOX_TANK_NONE);

    /* The builder's catch distance. */
    UT_ASSERT_MSG(lvHitboxBuilderReach(HS, 2048) == 128, "classic pill range: half a square");
    UT_ASSERT_MSG(lvHitboxBuilderReach(HS, 128) == 128, "pill range of half a square");
    UT_ASSERT_MSG(lvHitboxBuilderReach(HS, 100) == 100, "a short pill range limits it");
    UT_ASSERT_MSG(lvHitboxBuilderReach(HS, 0) == 0, "pill range 0");
    UT_ASSERT_MSG(lvHitboxBuilderReach(HS, -1) == 0, "pill range under 0");
    /* The area circle: 144, or pill_range when less. */
    UT_ASSERT_MSG(lvHitboxBuilderReach(AREA, 2048) == 144, "area circle: 144");
    UT_ASSERT_MSG(lvHitboxBuilderReach(AREA, 130) == 130, "area circle, short pill range");
    /* The box has a circle only where pill_range cuts its corners (under
       128 times the square root of 2, about 181). */
    UT_ASSERT_MSG(lvHitboxBuilderReach(BOX, 2048) == 0, "box: no circle");
    UT_ASSERT_MSG(lvHitboxBuilderReach(BOX, 182) == 0, "box: corners not cut");
    UT_ASSERT_MSG(lvHitboxBuilderReach(BOX, 160) == 160, "box: corners cut");
    UT_ASSERT_MSG(lvHitboxBuilderReach(BOX, 100) == 100, "box: inside the box");

    /* The lgm_blast_shape rule: 0 the circle above, 1 the Mac Bolo box,
       2 a circle of 144. pill_range limits all three (checked below). */
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
        UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(228.0, 128.0, 128.0, 128.0, 0, 0, 100),
                      "shape 0, pill range 100: 100 across");
        UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(229.0, 128.0, 128.0, 128.0, 0, 0, 100),
                      "shape 0, pill range 100: 101 across");
        UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(128.0, 128.0, 128.0, 128.0, 0, 1, -1),
                      "pill range under 0 catches nobody");
    }

    /* Open ground all round: a circle of 128, the edge itself caught. */
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(128.0, 128.0, 128.0, 128.0, 0, HS, 128), "on him");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(256.0, 128.0, 128.0, 128.0, 0, HS, 128), "128 away");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(257.0, 128.0, 128.0, 128.0, 0, HS, 128), "129 away");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(-60.0, -60.0, 20.0, 20.0, 0, HS, 128),
                  "in the square up and left");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(128.0, 128.0, 0.0, 0.0, 0, HS, 128),
                  "181 away on the slant");

    /* A building to his right: open ground stops at its edge. */
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(255.0, 128.0, 128.0, 128.0, SQ(2, 1), HS, 128),
                  "his side of the edge");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(256.0, 128.0, 128.0, 128.0, SQ(2, 1), HS, 128),
                  "on the building");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(128.0, 256.0, 128.0, 128.0, SQ(2, 1), HS, 128),
                  "the square under him is still open");

    /* His own square solid (he stands on a pillbox or a base): all of it
       catches, and no further than the circle elsewhere. */
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(250.0, 250.0, 128.0, 128.0, SQ(1, 1), HS, 128),
                  "a far corner of his own square");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(0.0, 0.0, 128.0, 128.0, SQ(1, 1), HS, 128),
                  "the near corner of his own square");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(128.0, -10.0, 128.0, 20.0, SQ(1, 1), HS, 128),
                  "the open square above, inside the circle");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(128.0, -1.0, 128.0, 128.0, SQ(1, 1), HS, 128),
                  "the open square above, outside the circle");

    /* Which pillboxes make a square solid: only a live one on the map. */
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 15, false, true), "a live pill is solid");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 1, false, true), "armour 1 is solid");
    UT_ASSERT_MSG(!lvHitboxPillSolid(true, false, 0, false, true), "a dead pill is open ground");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 0, true, true),
                  "the frame of the hit that killed a pill: its square is solid");
    UT_ASSERT_MSG(!lvHitboxPillSolid(true, true, 15, false, true), "a pill in a tank is not there");
    UT_ASSERT_MSG(!lvHitboxPillSolid(false, false, 15, false, true), "a removed pill is not there");

    /* A dead pill to his right is open ground: an explosion just across the
       edge, 10 from him, catches him; the same explosion with a live pill
       there does not. */
    {
        unsigned deadMask = lvHitboxPillSolid(true, false, 0, false, true) ? SQ(2, 1) : 0;
        unsigned liveMask = lvHitboxPillSolid(true, false, 15, false, true) ? SQ(2, 1) : 0;
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
        unsigned hitMask  = lvHitboxPillSolid(true, false, 0, true, true) ? SQ(1, 1) : 0;
        unsigned deadMask = lvHitboxPillSolid(true, false, 0, false, true) ? SQ(1, 1) : 0;
        UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, hitMask, 128),
                      "the killing shot catches him in the far corner of the pill's square");
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, deadMask, 128),
                      "a blast on a dead pill's square with no hit misses him there");
    }

    /* Which terrain makes a square solid: a building or a half building,
       and a square a shell's hit changed on this frame (a wall to rubble on
       its last hit, a forest to grass, a boat to river, the shore or a
       bridge a boat's shell hit). */
    UT_ASSERT_MSG(lvHitboxSquareSolid(true, false, true), "a wall is solid");
    UT_ASSERT_MSG(lvHitboxSquareSolid(false, true, true),
                  "the frame of a shell's hit that changed the square: it is solid");
    UT_ASSERT_MSG(!lvHitboxSquareSolid(false, false, true),
                  "rubble, grass or river with no hit this frame is open ground");

    /* The shot that turns a wall to rubble, on the square to his left: the
       blast at that square's middle, 128 from him on the first world unit of
       his own square, does not catch him, as
       for any hit on a wall. The same blast on rubble with no hit this frame
       does. And the same shot on his own square catches him in its far
       corner, 178 away. */
    {
        unsigned leftHit   = lvHitboxSquareSolid(false, true, true) ? SQ(0, 1) : 0;
        unsigned leftOpen  = lvHitboxSquareSolid(false, false, true) ? SQ(0, 1) : 0;
        unsigned ownHit    = lvHitboxSquareSolid(false, true, true) ? SQ(1, 1) : 0;
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(-128.0, 128.0, 0.0, 128.0, leftHit, 128),
                      "the rubble shot on the next square misses him 128 away");
        UT_ASSERT_MSG(lvHitboxBuilderCaught(-128.0, 128.0, 0.0, 128.0, leftOpen, 128),
                      "a blast on rubble with no hit catches him 128 away");
        UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, ownHit, 128),
                      "the rubble shot catches him in the far corner of its square");
    }

    /* A short pill range. */
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(228.0, 128.0, 128.0, 128.0, 0, HS, 100), "100 away");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(229.0, 128.0, 128.0, 128.0, 0, HS, 100), "101 away");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(128.0, 128.0, 128.0, 128.0, 0, HS, 0),
                  "pill range 0: only on him");

    /* The Mac Bolo box: less than 128 on each axis, the edge left out. */
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(255.0, 255.0, 128.0, 128.0, 0, BOX, 2048),
                  "box: a far corner, 127 on each axis");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(256.0, 128.0, 128.0, 128.0, 0, BOX, 2048),
                  "box: 128 away is out");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(255.0, 255.0, 128.0, 128.0, 0, BOX, 160),
                  "box: a short pill range cuts the corner");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(255.0, 128.0, 128.0, 128.0, SQ(2, 1), BOX, 2048),
                  "box: his side of a building's edge");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(256.0, 128.0, 128.0, 128.0, SQ(2, 1), BOX, 2048),
                  "box: on the building");

    /* The area circle: 144, the edge caught. */
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(272.0, 128.0, 128.0, 128.0, 0, AREA, 2048),
                  "area: 144 away");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(273.0, 128.0, 128.0, 128.0, 0, AREA, 2048),
                  "area: 145 away");
    UT_ASSERT_MSG(lvHitboxBuilderCaughtShape(229.0, 229.0, 128.0, 128.0, 0, AREA, 2048),
                  "area: about 142.8 on the slant");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(230.0, 230.0, 128.0, 128.0, 0, AREA, 2048),
                  "area: about 144.2 on the slant");

    /* Off the 3 by 3 squares: never. */
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(-300.0, 0.0, 0.0, 0.0, 0x1FFu, HS, 128), "far left");
    UT_ASSERT_MSG(!lvHitboxBuilderCaughtShape(0.0, 600.0, 0.0, 0.0, 0x1FFu, HS, 128), "far down");

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
    UT_ASSERT_MSG(lvHitboxTankCentreInPicture() == 8.46875,
                  "the drawing's centre is the same");
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

    /* The pulsing dot of a shell explosion. The record is the blast point
       less 128, rounded down to the pixel, so a blast at world 1000 is
       recorded at (1000 - 128) & ~15 = 864: 136 units, 8.5 pixels, before
       it. The guess, 8.46875 pixels from the picture's top left, is 135.5
       units, inside the 16 units the blast can be in (128 to 143). */
    UT_ASSERT_MSG(LV_HITBOX_BLAST_OFFSET == 128, "explosion record offset");
    UT_ASSERT_MSG(lvHitboxBlastCentreInPicture() == 8.46875, "blast centre in the picture");
    {
        double guess = lvHitboxBlastCentreInPicture() * 16.0;
        int wx;
        for (wx = 128; wx < 128 + 64; wx++) {
            int rec = (wx - 128) & ~15;
            double err = (double)rec + guess - (double)wx;
            UT_ASSERT_MSG(err >= -8.0 && err <= 8.0,
                          "blast at %d: the dot is on it (%.1f off)", wx, err);
        }
    }
    /* A real one, from the Oil Rig log: an explosion recorded at
       (34016, 30080) whose blast point was about (34146, 30210), 130 units
       further on each axis. The dot goes 135.5 units on from the record,
       not on the record (the picture's corner), and so is on the blast
       point to within half an art pixel. */
    {
        double guess = lvHitboxBlastCentreInPicture() * (double)LV_HITBOX_WU_PER_PIXEL;
        double dotX = 34016.0 + guess, dotY = 30080.0 + guess;
        UT_ASSERT_MSG(fabs(dotX - 34146.0) <= 8.0, "Oil Rig blast, x: %.1f", dotX);
        UT_ASSERT_MSG(fabs(dotY - 30210.0) <= 8.0, "Oil Rig blast, y: %.1f", dotY);
        UT_ASSERT_MSG(guess > 120.0, "the dot is not at the picture's corner");
    }
    /* The pulse: smooth, from 35% to 100%, once a second, from the wall
       clock alone. */
    UT_ASSERT_MSG(fabs(lvHitboxPulseAlpha(0) - 0.35) < 1e-9, "pulse starts low");
    UT_ASSERT_MSG(fabs(lvHitboxPulseAlpha(500) - 1.0) < 1e-9, "pulse is full half way");
    UT_ASSERT_MSG(fabs(lvHitboxPulseAlpha(1000) - 0.35) < 1e-9, "pulse is low again at 1 s");
    UT_ASSERT_MSG(fabs(lvHitboxPulseAlpha(250) - lvHitboxPulseAlpha(1250)) < 1e-9,
                  "pulse repeats each second");
    {
        uint64_t ms;
        double prev = lvHitboxPulseAlpha(0);
        for (ms = 1; ms <= 2000; ms++) {
            double a = lvHitboxPulseAlpha(ms);
            UT_ASSERT_MSG(a >= 0.35 - 1e-9 && a <= 1.0 + 1e-9, "pulse in range at %u ms",
                          (unsigned)ms);
            /* No hard step: at most a little change each millisecond. */
            UT_ASSERT_MSG(fabs(a - prev) < 0.005, "pulse is smooth at %u ms", (unsigned)ms);
            prev = a;
        }
    }
    /* The colour goes from blue (240 degrees) to purple (285) and back
       every 2 s, smoothly, and never leaves that part of the wheel. */
    {
        double r, g, b, pr, pg, pb, lowR = 1.0, highR = 0.0;
        uint64_t ms;
        UT_ASSERT_MSG(fabs(lvHitboxDotHue(0) - 240.0) < 1e-9, "hue starts blue");
        UT_ASSERT_MSG(fabs(lvHitboxDotHue(1000) - 285.0) < 1e-9, "hue is purple at 1 s");
        UT_ASSERT_MSG(fabs(lvHitboxDotHue(2000) - 240.0) < 1e-9, "hue is blue again at 2 s");
        UT_ASSERT_MSG(fabs(lvHitboxDotHue(500) - lvHitboxDotHue(1500)) < 1e-9,
                      "hue goes back the way it came");
        lvHitboxDotColour(0, &r, &g, &b);
        UT_ASSERT_MSG(r == 0.0 && g == 0.0 && b == 1.0, "colour starts blue");
        lvHitboxDotColour(1000, &r, &g, &b);
        UT_ASSERT_MSG(fabs(r - 0.75) < 1e-9 && g == 0.0 && b == 1.0,
                      "colour is purple at 1 s: %.3f %.3f %.3f", r, g, b);
        pr = 0.0; pg = 0.0; pb = 1.0;
        for (ms = 1; ms <= 4000; ms++) {
            double h = lvHitboxDotHue(ms);
            lvHitboxDotColour(ms, &r, &g, &b);
            UT_ASSERT_MSG(h >= 240.0 - 1e-9 && h <= 285.0 + 1e-9, "hue %.1f at %u ms", h,
                          (unsigned)ms);
            UT_ASSERT_MSG(r >= 0.0 && r <= 0.75 + 1e-9 && g == 0.0 && b == 1.0,
                          "colour blue to purple at %u ms", (unsigned)ms);
            UT_ASSERT_MSG(fabs(r - pr) < 0.01 && fabs(g - pg) < 0.01 && fabs(b - pb) < 0.01,
                          "colour is smooth at %u ms", (unsigned)ms);
            if (r < lowR) lowR = r;
            if (r > highR) highR = r;
            pr = r; pg = g; pb = b;
        }
        UT_ASSERT_MSG(lowR < 1e-6 && highR > 0.75 - 1e-6, "colour reaches blue and purple");
    }
    /* The dot's size, in screen pixels: one art pixel at 100% zoom and
       less, never less than one screen pixel; zoomed in, 4 world units but
       never less than 2 screen pixels and never more than one art pixel.
       The argument is screen pixels to a world unit: the zoom / 16. */
    UT_ASSERT_MSG(lvHitboxDotSize(0.25f / 16.0f) == 1.0f, "1 pixel at 25%%");
    UT_ASSERT_MSG(lvHitboxDotSize(0.5f / 16.0f) == 1.0f, "1 pixel at 50%%");
    UT_ASSERT_MSG(lvHitboxDotSize(1.0f / 16.0f) == 1.0f, "1 pixel (one art pixel) at 100%%");
    UT_ASSERT_MSG(lvHitboxDotSize(1.5f / 16.0f) == 1.5f, "one art pixel at 150%%");
    UT_ASSERT_MSG(lvHitboxDotSize(2.0f / 16.0f) == 2.0f, "2 pixels at 200%%");
    UT_ASSERT_MSG(lvHitboxDotSize(4.0f / 16.0f) == 2.0f, "2 pixels at 400%%");
    UT_ASSERT_MSG(lvHitboxDotSize(8.0f / 16.0f) == 2.0f, "2 pixels (4 units) at 800%%");
    UT_ASSERT_MSG(lvHitboxDotSize(12.0f / 16.0f) == 3.0f, "3 pixels (4 units) at 1200%%");
    UT_ASSERT_MSG(lvHitboxDotSize(16.0f / 16.0f) == 4.0f, "4 pixels (4 units) at 1600%%");
    {
        float z;
        for (z = 0.1f; z <= 32.0f; z += 0.1f) {
            float k = z / 16.0f, s = lvHitboxDotSize(k);
            UT_ASSERT_MSG(s >= 1.0f, "never under 1 pixel at zoom %.1f", z);
            UT_ASSERT_MSG(s <= 16.0f * k + 1e-5f || s == 1.0f,
                          "never over one art pixel at zoom %.1f", z);
            UT_ASSERT_MSG(16.0f * k < 2.0f || s >= 2.0f - 1e-5f,
                          "never under 2 pixels zoomed in, zoom %.1f", z);
        }
    }
    /* The dot is centred on the exact point, not rounded to a pixel. */
    {
        LvHitDotRect d = lvHitboxDotRect(100.3f, 57.8f, 16.0f / 16.0f);
        UT_ASSERT_MSG(fabsf(d.x + d.w * 0.5f - 100.3f) < 1e-4f &&
                      fabsf(d.y + d.h * 0.5f - 57.8f) < 1e-4f, "dot centred on the point");
        UT_ASSERT_MSG(d.w == 4.0f && d.h == 4.0f, "dot is 4 by 4 at 1600%%");
        UT_ASSERT_MSG(fabsf(d.x - 98.3f) < 1e-4f && fabsf(d.y - 55.8f) < 1e-4f,
                      "dot not rounded: %.2f %.2f", d.x, d.y);
        d = lvHitboxDotRect(10.25f, 3.75f, 1.0f / 16.0f);
        UT_ASSERT_MSG(fabsf(d.x - 9.75f) < 1e-4f && fabsf(d.y - 3.25f) < 1e-4f && d.w == 1.0f,
                      "1 pixel dot centred on the point at 100%%");
    }
    /* A builder's dot: his place in his square, at the target's scale. At
       zoom factor 2 a place 100.5 units in is 12.5625 target pixels on. */
    UT_ASSERT_MSG(fabs(lvHitboxLgmDotAt(64.0, 100.5, 2) - (64.0 + 12.5625)) < 1e-9,
                  "builder dot place");
    UT_ASSERT_MSG(lvHitboxLgmDotAt(32.0, 0.0, 1) == 32.0, "builder dot at the square's corner");

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

/* The entry the lag view fits a builder at is picked by the frame's time,
   not by his pixel. He walks east one art pixel every two entries, then
   back west through the pixels he came by. Newest note first. */
int run_lv_hitbox_lgm_pick(void) {
    /* Oldest to newest: 1000 and 1020 in pixel 10, 1040 and 1060 in 11,
       1080 in 12, then back: 1100 in 11, 1120 in 10. */
    static const uint32_t ms[7] = {1120, 1100, 1080, 1060, 1040, 1020, 1000};
    static const int ax[7] = {10, 11, 12, 11, 11, 10, 10};
    static const int ay[7] = {5, 5, 5, 5, 5, 5, 5};

    /* A frame at 1020 shows him in pixel 10: the 1020 entry, not 1120 (the
       newest in that pixel, on his walk back) and not 1000 (the other
       entry in the same pixel). */
    UT_ASSERT_MSG(lvHitboxLgmPickEntry(ms, ax, ay, 7, 1020, 10, 5) == 5,
                  "the frame at 1020 took entry %d, want 5",
                  lvHitboxLgmPickEntry(ms, ax, ay, 7, 1020, 10, 5));
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 7, 1000, 10, 5) == 6);
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 7, 1060, 11, 5) == 3);
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 7, 1100, 11, 5) == 1);
    /* A frame between two notes shows the older one. */
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 7, 1050, 11, 5) == 4);
    /* The present is the newest note. */
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 7, UINT32_MAX, 10, 5) == 0);
    /* The frame's pixel is not the note's: no fit, the middle of the pixel. */
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 7, 1080, 11, 5) == -1);
    /* Before every note. */
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 7, 900, 10, 5) == -1);
    UT_ASSERT(lvHitboxLgmPickEntry(ms, ax, ay, 0, 1000, 10, 5) == -1);
    return 0;
}

/* Every line of an outline has the area on one side and not on the other.
   A circle segment is a chord, a little inside the circle, so it is tested
   on the circle at its middle; an edge piece is tested across itself. */
static int outlineBorderHoldsShape(const LvHitOutline *o, double bx, double by,
                                   unsigned solid, int shape, int pillRange, int reach) {
    int run, p;
    for (run = 0; run < o->nRuns; run++) {
        for (p = o->runStart[run]; p + 1 < o->runStart[run + 1]; p++) {
            LvHitPt a = o->pt[p], b = o->pt[p + 1];
            double mx = (a.x + b.x) * 0.5, my = (a.y + b.y) * 0.5;
            double ux = b.x - a.x, uy = b.y - a.y, len = sqrt(ux * ux + uy * uy);
            double nx, ny, rd = sqrt((mx - bx) * (mx - bx) + (my - by) * (my - by));
            bool in, out;
            UT_ASSERT_MSG(len > 0.0, "a line of no length in run %d", run);
            if (reach > 0 && fabs(rd - (double)reach) < 1.0 && a.x != b.x && a.y != b.y) {
                nx = (mx - bx) / rd;
                ny = (my - by) / rd;
                mx = bx + nx * (double)reach;
                my = by + ny * (double)reach;
            } else {
                nx = -uy / len;
                ny = ux / len;
            }
            in = lvHitboxBuilderCaughtShape(mx - nx * 0.01, my - ny * 0.01, bx, by, solid,
                                            shape, pillRange);
            out = lvHitboxBuilderCaughtShape(mx + nx * 0.01, my + ny * 0.01, bx, by, solid,
                                             shape, pillRange);
            UT_ASSERT_MSG(in != out, "the line %.3f,%.3f to %.3f,%.3f is not on the border",
                          a.x, a.y, b.x, b.y);
        }
    }
    return 0;
}

/* The same, with the circle on the border from lvHitboxBuilderReach. */
static int outlineBorderHolds(const LvHitOutline *o, double bx, double by, unsigned solid,
                              int shape, int pillRange) {
    return outlineBorderHoldsShape(o, bx, by, solid, shape, pillRange,
                                   lvHitboxBuilderReach(shape, pillRange));
}

/* Whether the outline has a point at (x, y), to within 1e-6 world units. */
static bool outlineHasPoint(const LvHitOutline *o, double x, double y) {
    int p;
    for (p = 0; p < o->nPts; p++) {
        if (fabs(o->pt[p].x - x) < 1e-6 && fabs(o->pt[p].y - y) < 1e-6) {
            return true;
        }
    }
    return false;
}

/* The builder's outline goes to the true edge of a building. He stands at
   200, 128 with a building on the square to his right: the circle meets
   its edge, x = 256, at y = 128 plus or minus h. Both the circle and the
   edge stop there exactly, and nothing is drawn on the building. */
int run_lv_hitbox_outline(void) {
    static LvHitOutline o;
    const double h = sqrt(128.0 * 128.0 - 56.0 * 56.0);
    static const struct {
        double bx, by;
        unsigned solid;
        int shape, pillRange;
    } more[] = {
        {128.0, 128.0, 0, HS, 128},
        {20.0, 230.0, SQ(0, 1) | SQ(1, 2), HS, 128},
        {128.0, 128.0, SQ(1, 1), HS, 128},          /* on a pillbox */
        {240.0, 10.0, SQ(2, 0) | SQ(1, 0), HS, 100},
        {64.0, 64.0, 0x1FFu, HS, 128},
        {64.0, 64.0, SQ(1, 1), HS, 0},
        {128.0, 128.0, 0, BOX, 2048},
        {200.0, 40.0, SQ(2, 1) | SQ(1, 0), BOX, 2048},
        {128.0, 128.0, 0, BOX, 160},                /* the corners cut */
        {30.0, 220.0, SQ(0, 2), BOX, 150},
        {128.0, 128.0, 0, AREA, 2048},
        {250.0, 5.0, SQ(2, 0), AREA, 2048},
        {128.0, 128.0, SQ(1, 1), AREA, 130},
    };
    int p;
    size_t c;

    lvHitboxBuilderOutlineShape(200.0, 128.0, SQ(2, 1), HS, 128, &o);
    UT_ASSERT(o.nRuns > 0 && o.nPts > 0);
    for (p = 0; p < o.nPts; p++) {
        UT_ASSERT_MSG(o.pt[p].x <= 256.0 + 1e-9, "point %d at %.3f,%.3f is on the building", p,
                      o.pt[p].x, o.pt[p].y);
    }
    UT_ASSERT_MSG(outlineHasPoint(&o, 256.0, 128.0 - h) &&
                  outlineHasPoint(&o, 256.0, 128.0 + h),
                  "the outline does not stop where the circle meets the building");
    if (outlineBorderHolds(&o, 200.0, 128.0, SQ(2, 1), HS, 128) != 0) return 1;

    for (c = 0; c < sizeof(more) / sizeof(more[0]); c++) {
        lvHitboxBuilderOutlineShape(more[c].bx, more[c].by, more[c].solid, more[c].shape,
                               more[c].pillRange, &o);
        UT_ASSERT_MSG(o.nRuns > 0, "case %u has no outline", (unsigned)c);
        if (outlineBorderHolds(&o, more[c].bx, more[c].by, more[c].solid, more[c].shape,
                               more[c].pillRange) != 0) {
            return 1;
        }
    }
    /* Open ground all round: one circle, in two runs at most (it starts and
       ends at angle 0). */
    lvHitboxBuilderOutlineShape(128.0, 128.0, 0, HS, 128, &o);
    UT_ASSERT_MSG(o.nRuns <= 2, "%d runs for a plain circle", o.nRuns);

    /* The Mac Bolo box on open ground: its four sides, at 128 each side of
       him, and nothing else. */
    lvHitboxBuilderOutlineShape(128.0, 128.0, 0, BOX, 2048, &o);
    UT_ASSERT(o.nPts > 0);
    for (p = 0; p < o.nPts; p++) {
        UT_ASSERT_MSG((fabs(o.pt[p].x) < 1e-9 || fabs(o.pt[p].x - 256.0) < 1e-9 ||
                       fabs(o.pt[p].y) < 1e-9 || fabs(o.pt[p].y - 256.0) < 1e-9),
                      "box point %d at %.3f,%.3f is off its sides", p, o.pt[p].x, o.pt[p].y);
    }
    UT_ASSERT_MSG(outlineHasPoint(&o, 0.0, 0.0) && outlineHasPoint(&o, 256.0, 256.0),
                  "the box outline does not reach its corners");
    /* The other lgm_blast_shape values: the Mac Bolo box, with its corners
       cut by a short pill_range or not, and the circle of 144. circle is
       the circle on the border (0 for none). */
    {
        static const struct {
            double bx, by;
            unsigned solid;
            int shape, pillRange, circle;
        } shapes[] = {
            {128.0, 128.0, 0, 1, 2048, 0},
            {128.0, 128.0, 0, 1, 150, 150},
            {200.0, 60.0, SQ(2, 1) | SQ(1, 0), 1, 2048, 0},
            {200.0, 60.0, SQ(2, 1) | SQ(1, 0), 1, 150, 150},
            {128.0, 128.0, 0, 2, 2048, 144},
            {30.0, 220.0, SQ(0, 1) | SQ(1, 2), 2, 2048, 144},
            {30.0, 220.0, SQ(0, 1), 2, 100, 100},
        };
        for (c = 0; c < sizeof(shapes) / sizeof(shapes[0]); c++) {
            lvHitboxBuilderOutlineShape(shapes[c].bx, shapes[c].by, shapes[c].solid,
                                        shapes[c].shape, shapes[c].pillRange, &o);
            UT_ASSERT_MSG(o.nRuns > 0, "shape case %u has no outline", (unsigned)c);
            if (outlineBorderHoldsShape(&o, shapes[c].bx, shapes[c].by, shapes[c].solid,
                                        shapes[c].shape, shapes[c].pillRange,
                                        shapes[c].circle) != 0) {
                return 1;
            }
        }
        /* The box with its corners cut: the side x = 256 stops where the
           circle of 150 meets it, at 128 plus or minus sqrt(150^2 - 128^2). */
        lvHitboxBuilderOutlineShape(128.0, 128.0, 0, 1, 150, &o);
        UT_ASSERT_MSG(outlineHasPoint(&o, 256.0, 128.0 - sqrt(150.0 * 150.0 - 128.0 * 128.0)),
                      "the box side does not stop where the pill_range circle meets it");
        /* The box's bottom side, y = 60 + 128 = 188, cuts the building's
           edge x = 256: the run along that edge stops at the corner. */
        lvHitboxBuilderOutlineShape(200.0, 60.0, SQ(2, 1) | SQ(1, 0), 1, 2048, &o);
        UT_ASSERT_MSG(outlineHasPoint(&o, 256.0, 188.0),
                      "the edge x = 256 does not stop at the box's bottom side");
        /* With the cut on the wrong axis the run went on to y = 192. */
        UT_ASSERT_MSG(!outlineHasPoint(&o, 256.0, 192.0),
                      "the edge x = 256 runs past the box's bottom side");
    }
    return 0;
}

/* The square-hit rule by the recording's version: 2.2.1 and older play the
   open-ground rule on a square a shell's hit changed, 2.2.2 and later the
   square rule. draw.c asks lvHitboxSquareHitForVersion for the recording's
   header version. */
int run_lv_hitbox_square_hit_by_version(void) {
    bool oldRule = lvHitboxSquareHitForVersion(2, 2, 1);
    bool newRule = lvHitboxSquareHitForVersion(2, 2, 2);
    bool olderRule = lvHitboxSquareHitForVersion(1, 17, 0);
    bool laterRule = lvHitboxSquareHitForVersion(2, 3, 0);

    UT_ASSERT_MSG(!oldRule, "2.2.1 plays the open-ground rule");
    UT_ASSERT_MSG(newRule, "2.2.2 plays the square rule");
    UT_ASSERT_MSG(!olderRule, "1.17 plays the open-ground rule");
    UT_ASSERT_MSG(laterRule, "2.3.0 plays the square rule");

    /* A wall is solid in both, with or without a hit this frame. */
    UT_ASSERT_MSG(lvHitboxSquareSolid(true, false, oldRule), "2.2.1: a wall is solid");
    UT_ASSERT_MSG(lvHitboxSquareSolid(true, true, oldRule),
                  "2.2.1: a wall a shell hit and left a wall is solid");
    UT_ASSERT_MSG(lvHitboxSquareSolid(true, false, newRule), "2.2.2: a wall is solid");
    UT_ASSERT_MSG(lvHitboxSquareSolid(true, true, newRule),
                  "2.2.2: a wall a shell hit and left a wall is solid");

    /* The frame of a shell's hit that changed the square (a wall to rubble,
       a forest to grass, a boat to river, the shore, a bridge): open ground
       in 2.2.1, solid in 2.2.2. */
    UT_ASSERT_MSG(!lvHitboxSquareSolid(false, true, oldRule),
                  "2.2.1: the square a shell's hit changed this frame is open ground");
    UT_ASSERT_MSG(lvHitboxSquareSolid(false, true, newRule),
                  "2.2.2: the square a shell's hit changed this frame is solid");

    /* No hit this frame: open ground in both. */
    UT_ASSERT_MSG(!lvHitboxSquareSolid(false, false, oldRule),
                  "2.2.1: rubble with no hit this frame is open ground");
    UT_ASSERT_MSG(!lvHitboxSquareSolid(false, false, newRule),
                  "2.2.2: rubble with no hit this frame is open ground");

    /* The rubble shot on the square to his left, 128 from him on the first
       world unit of his own square: in 2.2.1 it catches him, as any blast
       on open ground 128 away does; in 2.2.2 it misses him, as a hit on a
       wall does. The same shot on his own square, 178 from him in its far
       corner: in 2.2.2 it catches him, as a hit on his square does; in
       2.2.1 that is past 128 on open ground, so it misses him. */
    {
        unsigned leftOld = lvHitboxSquareSolid(false, true, oldRule) ? SQ(0, 1) : 0;
        unsigned leftNew = lvHitboxSquareSolid(false, true, newRule) ? SQ(0, 1) : 0;
        unsigned ownOld  = lvHitboxSquareSolid(false, true, oldRule) ? SQ(1, 1) : 0;
        unsigned ownNew  = lvHitboxSquareSolid(false, true, newRule) ? SQ(1, 1) : 0;
        UT_ASSERT_MSG(lvHitboxBuilderCaught(-128.0, 128.0, 0.0, 128.0, leftOld, 128),
                      "2.2.1: the rubble shot on the next square catches him 128 away");
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(-128.0, 128.0, 0.0, 128.0, leftNew, 128),
                      "2.2.2: the rubble shot on the next square misses him 128 away");
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, ownOld, 128),
                      "2.2.1: the rubble shot misses him in the far corner of its square");
        UT_ASSERT_MSG(lvHitboxBuilderCaught(128.0, 128.0, 2.0, 2.0, ownNew, 128),
                      "2.2.2: the rubble shot catches him in the far corner of its square");
    }

    /* A pillbox by version: in 2.2.1 every pill on the map makes its square
       solid, dead or alive; in 2.2.2 a dead pill's square is open ground
       except on the frame of the hit that killed it. A pill in a tank or
       off the map is not there in either. */
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 15, false, oldRule), "2.2.1: a live pill is solid");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 0, false, oldRule), "2.2.1: a dead pill is solid");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 0, true, oldRule),
                  "2.2.1: the frame of the hit that killed a pill is solid");
    UT_ASSERT_MSG(!lvHitboxPillSolid(true, true, 15, false, oldRule), "2.2.1: a pill in a tank is not there");
    UT_ASSERT_MSG(!lvHitboxPillSolid(false, false, 15, false, oldRule), "2.2.1: a removed pill is not there");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 15, false, newRule), "2.2.2: a live pill is solid");
    UT_ASSERT_MSG(!lvHitboxPillSolid(true, false, 0, false, newRule), "2.2.2: a dead pill is open ground");
    UT_ASSERT_MSG(lvHitboxPillSolid(true, false, 0, true, newRule),
                  "2.2.2: the frame of the hit that killed a pill is solid");
    UT_ASSERT_MSG(!lvHitboxPillSolid(true, true, 15, false, newRule), "2.2.2: a pill in a tank is not there");

    /* A dead pill to his right, an explosion just across the edge, 10 from
       him: in 2.2.1 the square is solid, so it misses him; in 2.2.2 it is
       open ground, so it catches him. */
    {
        unsigned deadOld = lvHitboxPillSolid(true, false, 0, false, oldRule) ? SQ(2, 1) : 0;
        unsigned deadNew = lvHitboxPillSolid(true, false, 0, false, newRule) ? SQ(2, 1) : 0;
        UT_ASSERT_MSG(!lvHitboxBuilderCaught(260.0, 128.0, 250.0, 128.0, deadOld, 128),
                      "2.2.1: a dead pill's square next to him does not catch across the edge");
        UT_ASSERT_MSG(lvHitboxBuilderCaught(260.0, 128.0, 250.0, 128.0, deadNew, 128),
                      "2.2.2: a dead pill's square next to him catches across the edge");
    }
    return 0;
}

/* One step's record, for lv_hitbox_new_blast. */
static LvHitBlastRec newBlastRec(uint8_t mx, uint8_t my, uint8_t px, uint8_t py,
                                 uint8_t frame) {
    LvHitBlastRec r;
    r.mx = mx;
    r.my = my;
    r.px = px;
    r.py = py;
    r.frame = frame;
    return r;
}

int run_lv_hitbox_new_blast(void) {
    static LvHitBlastRec prev[LV_HITBOX_BLAST_PREV_MAX + 1];
    LvHitBlastRec cur[4];
    uint8_t isNew[4];
    int i;

    /* Frames 1 to 8 are explosions; 0 and 9 up are not. */
    UT_ASSERT(!lvHitboxIsBlastFrame(0));
    UT_ASSERT(lvHitboxIsBlastFrame(1));
    UT_ASSERT(lvHitboxIsBlastFrame(8));
    UT_ASSERT(!lvHitboxIsBlastFrame(9));

    /* First recorded at frame 8, with nothing there the step before: new. */
    cur[0] = newBlastRec(10, 20, 3, 4, 8);
    isNew[0] = 9;
    lvHitboxNewBlasts(prev, 0, true, cur, 1, isNew);
    UT_ASSERT_MSG(isNew[0] == 1, "a frame 8 explosion with nothing before is not new");

    /* First recorded at frame 7 (the server lowered it before it wrote the
       step): new too. */
    cur[0] = newBlastRec(10, 20, 3, 4, 7);
    lvHitboxNewBlasts(prev, 0, true, cur, 1, isNew);
    UT_ASSERT_MSG(isNew[0] == 1, "a frame 7 explosion with nothing before is not new");

    /* Frame 8 for two steps: the second step is not new. */
    prev[0] = newBlastRec(10, 20, 3, 4, 8);
    cur[0] = newBlastRec(10, 20, 3, 4, 8);
    lvHitboxNewBlasts(prev, 1, true, cur, 1, isNew);
    UT_ASSERT_MSG(isNew[0] == 0, "frame 8 on its second step is new");

    /* 8 then 7, and 7 then 7: the same explosion, not new. */
    cur[0] = newBlastRec(10, 20, 3, 4, 7);
    lvHitboxNewBlasts(prev, 1, true, cur, 1, isNew);
    UT_ASSERT(isNew[0] == 0);
    prev[0] = newBlastRec(10, 20, 3, 4, 7);
    lvHitboxNewBlasts(prev, 1, true, cur, 1, isNew);
    UT_ASSERT(isNew[0] == 0);

    /* An explosion's frame never goes up, so a frame 8 where a frame 5
       was is a new explosion on the old one's place. */
    prev[0] = newBlastRec(10, 20, 3, 4, 5);
    cur[0] = newBlastRec(10, 20, 3, 4, 8);
    lvHitboxNewBlasts(prev, 1, true, cur, 1, isNew);
    UT_ASSERT_MSG(isNew[0] == 1, "a new explosion on an old one's place is not new");

    /* Both: the old one goes on at 4 and the new one starts at 8, in
       either order. Each old one matches one only. */
    cur[0] = newBlastRec(10, 20, 3, 4, 4);
    cur[1] = newBlastRec(10, 20, 3, 4, 8);
    lvHitboxNewBlasts(prev, 1, true, cur, 2, isNew);
    UT_ASSERT(isNew[0] == 0 && isNew[1] == 1);
    cur[0] = newBlastRec(10, 20, 3, 4, 8);
    cur[1] = newBlastRec(10, 20, 3, 4, 5);
    lvHitboxNewBlasts(prev, 1, true, cur, 2, isNew);
    UT_ASSERT(isNew[0] == 1 && isNew[1] == 0);

    /* Two old ones at one place (8 and 3), both going on (8 and 2): the 8
       takes the 8 and the 2 takes the 3, so neither is new. */
    prev[0] = newBlastRec(10, 20, 3, 4, 8);
    prev[1] = newBlastRec(10, 20, 3, 4, 3);
    cur[0] = newBlastRec(10, 20, 3, 4, 2);
    cur[1] = newBlastRec(10, 20, 3, 4, 8);
    lvHitboxNewBlasts(prev, 2, true, cur, 2, isNew);
    UT_ASSERT(isNew[0] == 0 && isNew[1] == 0);

    /* A stack at one place in the list order of the log: the step before
       7, 5, 3, 1 and this step 7, 6, 4, 2. The 1 has ended, the others went
       down one, and the 7 is new. A match in list order would give the 7
       to the old 7 and call the 6 new. */
    prev[0] = newBlastRec(10, 20, 3, 4, 7);
    prev[1] = newBlastRec(10, 20, 3, 4, 5);
    prev[2] = newBlastRec(10, 20, 3, 4, 3);
    prev[3] = newBlastRec(10, 20, 3, 4, 1);
    cur[0] = newBlastRec(10, 20, 3, 4, 7);
    cur[1] = newBlastRec(10, 20, 3, 4, 6);
    cur[2] = newBlastRec(10, 20, 3, 4, 4);
    cur[3] = newBlastRec(10, 20, 3, 4, 2);
    lvHitboxNewBlasts(prev, 4, true, cur, 4, isNew);
    UT_ASSERT_MSG(isNew[0] == 1 && isNew[1] == 0 && isNew[2] == 0 && isNew[3] == 0,
                  "stack: new %d %d %d %d", isNew[0], isNew[1], isNew[2], isNew[3]);

    /* Another place, by one art pixel or one square: new. */
    prev[0] = newBlastRec(10, 20, 3, 4, 8);
    cur[0] = newBlastRec(10, 20, 3, 5, 7);
    cur[1] = newBlastRec(11, 20, 3, 4, 7);
    cur[2] = newBlastRec(10, 20, 2, 4, 7);
    cur[3] = newBlastRec(10, 21, 3, 4, 7);
    lvHitboxNewBlasts(prev, 1, true, cur, 4, isNew);
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(isNew[i] == 1, "explosion %d, at another place, is not new", i);
    }

    /* A shell (frame 9 up) is never new, and a shell of the step before
       does not hide an explosion. */
    prev[0] = newBlastRec(10, 20, 3, 4, 12);
    cur[0] = newBlastRec(10, 20, 3, 4, 12);
    cur[1] = newBlastRec(10, 20, 3, 4, 8);
    lvHitboxNewBlasts(prev, 1, true, cur, 2, isNew);
    UT_ASSERT(isNew[0] == 0 && isNew[1] == 1);

    /* The step before not known (the first step, or a jump): none is new. */
    cur[0] = newBlastRec(10, 20, 3, 4, 8);
    isNew[0] = 9;
    lvHitboxNewBlasts(prev, 0, false, cur, 1, isNew);
    UT_ASSERT_MSG(isNew[0] == 0, "new with the step before unknown");

    /* The step before has more records than can be matched: none is new,
       as one of them could be this explosion. */
    for (i = 0; i <= LV_HITBOX_BLAST_PREV_MAX; i++) {
        prev[i] = newBlastRec(1, 1, 0, 0, 12);
    }
    isNew[0] = 9;
    lvHitboxNewBlasts(prev, LV_HITBOX_BLAST_PREV_MAX + 1, true, cur, 1, isNew);
    UT_ASSERT_MSG(isNew[0] == 0, "new with too many records the step before");
    lvHitboxNewBlasts(prev, LV_HITBOX_BLAST_PREV_MAX, true, cur, 1, isNew);
    UT_ASSERT_MSG(isNew[0] == 1, "not new with a full step before that has no explosion");

    /* The picture: a new explosion recorded at frame 7 draws the frame 8
       picture on that step only; every other frame and step draws the
       recorded frame. */
    UT_ASSERT(lvHitboxDrawFrame(7, true) == 8);
    UT_ASSERT(lvHitboxDrawFrame(8, true) == 8);
    UT_ASSERT(lvHitboxDrawFrame(7, false) == 7);
    UT_ASSERT(lvHitboxDrawFrame(6, true) == 6);
    UT_ASSERT(lvHitboxDrawFrame(1, true) == 1);
    UT_ASSERT(lvHitboxDrawFrame(12, true) == 12);

    /* A run of steps: an explosion first recorded at 8 for two steps, then
       7 down to 1, gets the dot on its first step only; one first recorded
       at 7 too. */
    {
        static const uint8_t runA[] = {8, 8, 7, 7, 7, 6, 6, 6, 5, 5, 5, 4, 4, 4,
                                       3, 3, 3, 2, 2, 2, 1, 1, 1};
        static const uint8_t runB[] = {7, 7, 7, 6, 6, 6, 5, 5, 5, 4, 4, 4,
                                       3, 3, 3, 2, 2, 2, 1, 1, 1};
        const uint8_t *runs[2];
        int lens[2];
        int r;
        runs[0] = runA;
        lens[0] = (int)sizeof(runA);
        runs[1] = runB;
        lens[1] = (int)sizeof(runB);
        for (r = 0; r < 2; r++) {
            int dots = 0, firstDot = -1, k;
            int nPrev = 0;
            for (k = 0; k < lens[r]; k++) {
                cur[0] = newBlastRec(30, 40, 7, 9, runs[r][k]);
                lvHitboxNewBlasts(prev, nPrev, true, cur, 1, isNew);
                /* Every run starts with the frame 8 picture, and after
                   that draws the recorded frame. */
                UT_ASSERT_MSG(lvHitboxDrawFrame(cur[0].frame, isNew[0] != 0) ==
                                  (k == 0 ? 8 : runs[r][k]),
                              "run %d step %d draws frame %d", r, k,
                              lvHitboxDrawFrame(cur[0].frame, isNew[0] != 0));
                if (isNew[0]) {
                    dots++;
                    if (firstDot < 0) firstDot = k;
                }
                prev[0] = cur[0];
                nPrev = 1;
            }
            UT_ASSERT_MSG(dots == 1 && firstDot == 0, "run %d: %d dots, first at step %d",
                          r, dots, firstDot);
        }
    }
    return 0;
}
