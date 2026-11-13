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
 *    lgm.c): hitRulesBuilderCaught. The engine finds the
 *    squares and the solid ground; this header has the rule
 *    that is applied to them.
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

/* Whether an explosion on open ground catches a builder: no more than 128
   from him. pillRange is the pill_range rule as the engine reads it (a
   WORLD, 0 to 65535). The engine takes the distance from utilIsItemInRange
   with pill_range as its limit, and past the limit that gives WORLD_MAX,
   which never passes; so the distance must also be no more than pillRange.
   For whole-number dx and dy and a whole-number r, d2 <= r * r gives the
   same answer as the engine's sqrt(d2) <= r (the unit test
   hit_rules_builder_matches_engine checks it). dx and dy are the
   explosion's place less his. */
static inline bool hitRulesBuilderOpenGround(double dx, double dy, int pillRange) {
    double d2 = dx * dx + dy * dy;
    if (pillRange < 0) {
        return false;
    }
    return d2 <= (double)HIT_RULES_HALF_SQUARE * (double)HIT_RULES_HALF_SQUARE &&
           d2 <= (double)pillRange * (double)pillRange;
}

/* The builder rule. blastSolid: the explosion's square is a building, a
   half building, a pillbox or a base. blastOnHisSquare: the explosion's
   square is his own square. On a solid square it catches him only on his
   own square; on open ground it is hitRulesBuilderOpenGround. */
static inline bool hitRulesBuilderCaught(bool blastSolid, bool blastOnHisSquare,
                                         double dx, double dy, int pillRange) {
    if (blastSolid) {
        return blastOnHisSquare;
    }
    return hitRulesBuilderOpenGround(dx, dy, pillRange);
}

#endif /* HIT_RULES_H */
