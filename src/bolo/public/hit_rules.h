/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Hit rules
 *Filename:      hit_rules.h
 *Purpose:
 *  The two tests that decide a hit, as pure sums with no
 *  game state, so the engine and the Log Viewer use the
 *  same ones:
 *
 *  - A shell hits a tank (tankShellInHitZone in tank.c,
 *    utilIsTankHit in util.c, and the client's shell cull
 *    in client_sim.c): hitRulesTankShellInZone.
 *
 *  - An explosion catches a builder (lgmBlastCatches in
 *    lgm.c): hitRulesBuilderCaught, with the open-ground area
 *    of the lgm_blast_shape rule in lgmBlastOpenGroundCatches.
 *    The engine finds the squares and the solid ground; this
 *    header has the rule that is applied to them.
 *
 *  The Log Viewer draws its hit boxes from these same
 *  functions (src/logviewer/lv_hitbox.h), so a change here
 *  changes what it draws too.
 *
 *  Sizes are in world units: 256 to a map square.
 *
 *  Header only, with no includes but the C library's.
 *********************************************************/

#ifndef HIT_RULES_H
#define HIT_RULES_H

#include <stdbool.h>
#include <stdint.h>

/* Define BOLO_LEGACY_SQUARE_COLLISION (compile time only, there is no
 * runtime toggle) to go back to the original Bolo behaviour: the square
 * hit zone below in place of the circle, and the legacy
 * direction-dependent bounding-box grid-snap building nudge in tank.c.
 * It lives here so the engine and the Log Viewer read the same setting
 * (internal/tank.h includes this header). */
/* #define BOLO_LEGACY_SQUARE_COLLISION */

/* Half the legacy square hit zone, and the builder's catch distance on
   open ground: half a map square (MAP_SQUARE_MIDDLE). */
#define HIT_RULES_HALF_SQUARE 128

/* The circle hit zone: the shell's centre less than radius from the
   tank's centre. dx and dy are the shell's place less the tank's (either
   way round). The test on each axis first bounds dx and dy, so the squares
   cannot overflow an int for a shell and a tank far apart. It never
   rejects a real hit. */
static inline bool hitRulesTankCircle(int dx, int dy, int radius) {
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    return ax < radius && ay < radius && (dx * dx + dy * dy) < radius * radius;
}

/* The legacy square hit zone of games before the circle (1.91 and
   earlier): less than 128 from the tank's centre on both axes. */
static inline bool hitRulesTankSquare(int dx, int dy) {
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    return ax < HIT_RULES_HALF_SQUARE && ay < HIT_RULES_HALF_SQUARE;
}

/* The hit zone this build uses. radius is the tank_hit_radius rule. */
static inline bool hitRulesTankShellInZone(int dx, int dy, int radius) {
#ifdef BOLO_LEGACY_SQUARE_COLLISION
    (void)radius;
    return hitRulesTankSquare(dx, dy);
#else
    return hitRulesTankCircle(dx, dy, radius);
#endif
}

/* The lgm_blast_shape rule: the area of open ground an explosion catches a
   builder in. dx and dy are the explosion's place less the builder's, in
   world units (256 to a square).
   - LGM_BLAST_SHAPE_HALF_SQUARE: "Half-square circle". A circle of radius
     HIT_RULES_HALF_SQUARE (128, MAP_SQUARE_MIDDLE), edge included, the
     WinBolo rule.
   - LGM_BLAST_SHAPE_MAC_BOX: "Mac Bolo box". |dx| < 128 and |dy| < 128,
     edge left out. Mac Bolo tests each axis on its own in this way.
   - LGM_BLAST_SHAPE_MAC_AREA: "Mac Bolo equivalent area circle". A circle
     of radius LGM_BLAST_MAC_AREA_RADIUS, edge included: 256 / sqrt(pi) is
     about 144.4, so it covers about the same area as the box.
   Every shape also stops at the pill_range rule: a builder further than
   pill_range from the explosion is safe. That limit is the old test's, and
   it changes a shape only when pill_range is under the shape's reach (128,
   about 180 and 144). */
#define LGM_BLAST_SHAPE_HALF_SQUARE 0
#define LGM_BLAST_SHAPE_MAC_BOX     1
#define LGM_BLAST_SHAPE_MAC_AREA    2
#define LGM_BLAST_SHAPE_CLASSIC     LGM_BLAST_SHAPE_HALF_SQUARE
#define LGM_BLAST_MAC_BOX_HALF      128
#define LGM_BLAST_MAC_AREA_RADIUS   144

/*********************************************************
*NAME:          lgmBlastOpenGroundCatches
*PURPOSE:
*  Whether an explosion on open ground (not a building, a
*  half building, a pillbox or a base) catches a builder
*  dx, dy world units from it, by the lgm_blast_shape
*  rule. shape is the rule's value; a value outside the
*  three plays as the half-square circle.
*
*  The half-square circle keeps the old test exactly: it
*  read its distance from utilIsItemInRange with the
*  pill_range rule as the limit, and past that limit the
*  distance never passes. So with pill_range under 128 the
*  circle has radius pill_range. The other two shapes stop
*  at pill_range the same way: a builder further than
*  pill_range from the explosion is safe. A pill_range
*  under 0 catches no builder.
*
*  dx and dy are doubles so the Log Viewer can ask between
*  whole world units when it draws the area's edge. The
*  engine gives whole numbers, and for those the answers
*  are the same as with whole-number sums.
*
*ARGUMENTS:
*  shape     - The lgm_blast_shape rule
*  pillRange - The pill_range rule
*  dx        - Explosion X less the builder's X
*  dy        - Explosion Y less the builder's Y
*********************************************************/
static inline bool lgmBlastOpenGroundCatches(int32_t shape, int32_t pillRange,
                                             double dx, double dy) {
    double ax = dx < 0 ? -dx : dx;
    double ay = dy < 0 ? -dy : dy;
    double d2 = dx * dx + dy * dy;

    /* Every shape: within pill_range, the limit utilIsItemInRange measured
       the old test against. */
    if (pillRange < 0 || d2 > (double)pillRange * pillRange) {
        return false;
    }
    if (shape == LGM_BLAST_SHAPE_MAC_BOX) {
        return ax < LGM_BLAST_MAC_BOX_HALF && ay < LGM_BLAST_MAC_BOX_HALF;
    }
    if (shape == LGM_BLAST_SHAPE_MAC_AREA) {
        return d2 <= (double)LGM_BLAST_MAC_AREA_RADIUS * LGM_BLAST_MAC_AREA_RADIUS;
    }
    /* The half-square circle: within half a square. */
    return d2 <= (double)HIT_RULES_HALF_SQUARE * HIT_RULES_HALF_SQUARE;
}

/* The builder rule, as lgmBlastCatches (lgm.c) applies it. blastSolid: the
   explosion's square is a building, a half building, a live pillbox or a
   base, or the explosion is a shell's hit on the thing on its square.
   blastOnHisSquare: the explosion's square is his own square. On a solid
   square it catches him only on his own square; on open ground it is
   lgmBlastOpenGroundCatches. shape and pillRange are the lgm_blast_shape
   and pill_range rules. */
static inline bool hitRulesBuilderCaught(bool blastSolid, bool blastOnHisSquare,
                                         double dx, double dy, int32_t shape,
                                         int32_t pillRange) {
    if (blastSolid) {
        return blastOnHisSquare;
    }
    return lgmBlastOpenGroundCatches(shape, pillRange, dx, dy);
}

/* hitRulesBuilderCaught with the shape first. The Log Viewer's Server
   Check (lv_server_check.h) calls it by this name. */
static inline bool hitRulesBuilderCaughtShape(int shape, bool blastSolid,
                                              bool blastOnHisSquare, double dx, double dy,
                                              int pillRange) {
    return hitRulesBuilderCaught(blastSolid, blastOnHisSquare, dx, dy, shape, pillRange);
}

#endif /* HIT_RULES_H */
