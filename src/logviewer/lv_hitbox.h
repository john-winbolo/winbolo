/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer hit boxes sums
 *Filename:      lv_hitbox.h
 *Purpose:
 *  The areas the Log Viewer draws round each tank and each
 *  builder on the map (Options > Hit Boxes; draw.c
 *  draws them). They are the engine's own tests, from the
 *  header the engine uses too (src/bolo/public/hit_rules.h),
 *  put in the viewer's terms:
 *
 *  - A tank: tankShellInHitZone (src/bolo/tank.c). A shell
 *    hits a tank when the shell's centre is less than the
 *    tank_hit_radius rule from the tank's centre. A circle.
 *    Games before the circle (1.91 and earlier), and a build
 *    with BOLO_LEGACY_SQUARE_COLLISION, used a square: less
 *    than 128 from the centre on both axes.
 *
 *  - A builder: lgmBlastCatches (src/bolo/lgm.c). An
 *    explosion catches a builder when it is on a square that
 *    is not a building, a half building, a live pillbox or a
 *    base, and no more than half a square (128) from him. A
 *    dead pillbox's square is open ground (pillsIsPillHit,
 *    armour above 0; lvHitboxPillSolid), except on the frame
 *    of the hit that killed it: that shot is a hit on the
 *    pill's square. A square that the frame's shot changed
 *    (a wall to rubble, a forest to grass, a boat to river,
 *    the shore or a bridge a boat's shell hit) is solid on
 *    that frame too (lvHitboxSquareSolid): that shot was a
 *    hit on the square. That terrain rule is only in games of
 *    after 2.2.1 (lvHitboxSquareHitForVersion, from
 *    the recording's version); in an older game the changed
 *    square is open ground on that frame, as the recording
 *    has it. On
 *    one of those squares it catches him only when it is on
 *    his own square. The distance comes from utilIsItemInRange with
 *    the pill_range rule as its limit, so when pill_range is
 *    under 128 the circle is pill_range across, not 128.
 *    The lgm_blast_shape rule changes the open-ground area: 0
 *    is that circle, 1 the Mac Bolo box (under 128 from him on
 *    each axis) and 2 a circle of radius 144. pill_range
 *    limits all three: a builder further than pill_range
 *    from the explosion is safe (hitRulesBuilderCaughtShape,
 *    hit_rules.h).
 *
 *  Sizes are in world units: 256 to a map square, 16 to a
 *  pixel of the map's art.
 *
 *  Header only, with no includes but the C library's and
 *  hit_rules.h, so the viewer and the unit tests use the same
 *  sums.
 *********************************************************/

#ifndef LV_HITBOX_H
#define LV_HITBOX_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "hit_rules.h"

/* World units in one pixel of the map's art (TANK_SHIFT_PIXELSIZE). */
#define LV_HITBOX_WU_PER_PIXEL 16
/* World units in one map square. */
#define LV_HITBOX_WU_PER_SQUARE 256
/* Half a square (MAP_SQUARE_MIDDLE): the builder's catch distance on
   open ground. */
#define LV_HITBOX_BUILDER_REACH HIT_RULES_HALF_SQUARE
/* The lgm_blast_shape rule's values (LGM_BLAST_SHAPE_* in hit_rules.h). */
#define LV_HITBOX_BUILDER_SHAPE_CIRCLE LGM_BLAST_SHAPE_HALF_SQUARE
#define LV_HITBOX_BUILDER_SHAPE_BOX    LGM_BLAST_SHAPE_MAC_BOX
#define LV_HITBOX_BUILDER_SHAPE_AREA   LGM_BLAST_SHAPE_MAC_AREA
/* The box's half side and the radius of shape 2, in world units. */
#define LV_HITBOX_BUILDER_BOX_HALF     LGM_BLAST_MAC_BOX_HALF
#define LV_HITBOX_BUILDER_AREA_REACH   LGM_BLAST_MAC_AREA_RADIUS
/* The furthest any shape reaches along an axis. */
#define LV_HITBOX_BUILDER_REACH_MAX    144
/* lgmGetMX / lgmGetPX (src/bolo/lgm.c) take 1 world unit off a walking
   builder's x and 2 off his y before they round his place down to an art
   pixel, which is the place a recording holds. */
#define LV_HITBOX_LGM_OFFSET_X 1
#define LV_HITBOX_LGM_OFFSET_Y 2
/* Half the tank picture, in world units (TANK_SUBTRACT). The game draws a
   tank's 16 by 16 picture with its top left this far up and left of the
   tank's place (playersMakeScreenTanks, tankGetScreenMX), so the picture's
   middle is the tank's place. */
#define LV_HITBOX_TANK_HALF 128
/* Where the game puts the top left of a walking builder's 3 by 4 picture, in
   art pixels from the art pixel his recorded place is in. spritePositionLgm
   (src/gui/sdl3/sprite_positions.c) takes 1.5 across and 1.5 down off and
   then rounds to a whole art pixel, which gives 1 and 1 in every drawing
   style but Smooth. The viewer draws whole art pixels, as Classic does. */
#define LV_HITBOX_LGM_PICTURE_X (-1)
#define LV_HITBOX_LGM_PICTURE_Y (-1)

/* A recording holds a place rounded down to a whole art pixel, so the true
   place is somewhere in the 16 world units from that pixel's top left. These
   give the middle of those 16, the closest guess, in world units from the
   top left of the square. pixel is the 0 to 15 art pixel in the square.

   The tank: tankGetPX gives his x to the art pixel, so he is 0 to 15 units
   into it. */
static inline double lvHitboxTankPlace(int pixel) {
    return (double)(pixel * LV_HITBOX_WU_PER_PIXEL) +
           (LV_HITBOX_WU_PER_PIXEL - 1) / 2.0;
}
/* The builder: lgmGetPX and lgmGetPY take 1 off his x and 2 off his y first,
   so he is 1 to 16 units into the pixel across and 2 to 17 down. */
static inline double lvHitboxBuilderPlaceX(int pixel) {
    return lvHitboxTankPlace(pixel) + LV_HITBOX_LGM_OFFSET_X;
}
static inline double lvHitboxBuilderPlaceY(int pixel) {
    return lvHitboxTankPlace(pixel) + LV_HITBOX_LGM_OFFSET_Y;
}

/* A walking builder's place, closer than his art pixel.
 *
 * A recording holds a builder's place to the art pixel only, so one place on
 * its own is known to 16 world units. But he walks in a straight line at a
 * steady speed most of the time: lgmUpdate moves him the same whole number
 * of world units every tick. So a run of recorded pixels from one walk fits
 * only a narrow set of lines, and those lines give his place much closer
 * than 16. On open ground he crosses about one pixel an entry, so his place
 * in the newest pixel was guessed to the middle of it, up to 8 world units
 * off, and a run of pixels from one walk takes most of that guess away.
 *
 * lvHitboxLgmFitRange takes up to LV_HITBOX_LGM_FIT_MAX recorded pixels
 * on one axis, newest first: pix[i] is the art pixel (from any fixed
 * origin, a map square's or the map's), and back[i] the entries before the
 * newest (back[0] is 0, and each later one is bigger). It finds the longest
 * run from the newest that one straight line at a steady speed can go
 * through: the place at back[i] is p - v * back[i], and it has to be in
 * pix[i]'s 16 units, give or take LV_HITBOX_LGM_FIT_SLOP. A run that turns
 * or stops does not fit, so the run is cut where the walk changed. lo and
 * hi take the places at the newest entry that the run allows, in world
 * units from the same origin, before the offset lgmGetPX takes off (so 0 to
 * 15 into the pixel pix[0]). It is never wider than pix[0]'s own 16. With one
 * pixel, or a builder standing still, it is that pixel's 16 units.
 *
 * Returns the number of pixels the run used. */
#define LV_HITBOX_LGM_FIT_MAX 24
/* How far a place may be past its pixel, in world units: lgmUpdate's angle
   is worked out again each tick, so a step can change by one now and then. */
#define LV_HITBOX_LGM_FIT_SLOP 1.0

/* Whether one line goes through the first k pixels, and if so the lowest
   and highest place at the newest entry such a line has. Each pixel is two
   lines in (p, v): p - v * back = its low edge and its high edge. The lines
   that fit all k are a closed shape, and its lowest and highest p are at
   corners where two of those lines meet. */
static inline bool lvHitboxLgmFitK(const int *back, const int *pix, int k,
                                   double *lo, double *hi) {
    const double eps = 1e-9;
    const double top = (double)(LV_HITBOX_WU_PER_PIXEL - 1) + LV_HITBOX_LGM_FIT_SLOP;
    bool any = false;
    int a, b;

    for (a = 0; a < 2 * k; a++) {
        int ia = a >> 1;
        double ca = (double)(pix[ia] * LV_HITBOX_WU_PER_PIXEL) +
                    ((a & 1) ? top : -LV_HITBOX_LGM_FIT_SLOP);
        for (b = a + 1; b < 2 * k; b++) {
            int ib = b >> 1;
            double cb, v, p;
            int i;
            bool ok = true;
            if (back[ia] == back[ib]) continue;
            cb = (double)(pix[ib] * LV_HITBOX_WU_PER_PIXEL) +
                 ((b & 1) ? top : -LV_HITBOX_LGM_FIT_SLOP);
            /* p - v * back[ia] = ca and p - v * back[ib] = cb. */
            v = (cb - ca) / (double)(back[ia] - back[ib]);
            p = ca + v * (double)back[ia];
            for (i = 0; i < k && ok; i++) {
                double at = p - v * (double)back[i];
                double edge = (double)(pix[i] * LV_HITBOX_WU_PER_PIXEL);
                ok = at >= edge - LV_HITBOX_LGM_FIT_SLOP - eps && at <= edge + top + eps;
            }
            if (!ok) continue;
            if (!any || p < *lo) *lo = p;
            if (!any || p > *hi) *hi = p;
            any = true;
        }
    }
    return any;
}

static inline int lvHitboxLgmFitRange(const int *back, const int *pix, int n,
                                      double *lo, double *hi) {
    double low0, high0, flo = 0.0, fhi = 0.0;
    int good = 1, bad, k;

    if (n > LV_HITBOX_LGM_FIT_MAX) n = LV_HITBOX_LGM_FIT_MAX;
    if (n < 1) {
        *lo = *hi = 0.0;
        return 0;
    }
    low0 = (double)(pix[0] * LV_HITBOX_WU_PER_PIXEL);
    high0 = low0 + (double)(LV_HITBOX_WU_PER_PIXEL - 1);
    *lo = low0;
    *hi = high0;
    /* A run that fits has every shorter run fit too, so the longest one is
       found by halving: good always fits, bad never does. */
    if (n >= 2 && lvHitboxLgmFitK(back, pix, n, &flo, &fhi)) {
        good = n;
    } else {
        bad = n;
        while (bad - good > 1) {
            double tlo = 0.0, thi = 0.0;
            k = (good + bad) / 2;
            if (lvHitboxLgmFitK(back, pix, k, &tlo, &thi)) {
                good = k;
                flo = tlo;
                fhi = thi;
            } else {
                bad = k;
            }
        }
    }
    if (good >= 2) {
        /* The slop lets a line run a little past the newest pixel, but the
           newest pixel itself is certain. */
        if (flo < low0) flo = low0;
        if (fhi > high0) fhi = high0;
        if (flo > fhi) flo = fhi = (flo > high0) ? high0 : low0;
        *lo = flo;
        *hi = fhi;
    }
    return good;
}

/* The builder's own square, as the engine finds it: from his true place
   (lgmWorldX >> 8, lgmWorldY >> 8). A recording's square is from his place
   less 1 across and 2 down (lgmGetMX / lgmGetMY), so when he stands less
   than that past a square's left or top edge, the recorded square is the one
   before, and his place from it is 256 or more. b is his place on one axis
   in world units from the top left of square *sq; this moves *sq along and
   takes 256 off b (or puts 256 on, for a place before the square) until b
   is 0 to 255, so b and *sq are the engine's. *sq stays 0 to 255. */
static inline void lvHitboxBuilderOwnSquare(double *b, int *sq) {
    while (*b >= (double)LV_HITBOX_WU_PER_SQUARE && *sq < 255) {
        *b -= (double)LV_HITBOX_WU_PER_SQUARE;
        (*sq)++;
    }
    while (*b < 0.0 && *sq > 0) {
        *b += (double)LV_HITBOX_WU_PER_SQUARE;
        (*sq)--;
    }
}

/* The tank's hit zone in a recording. NONE when it cannot be known: the
   viewer then draws no tank outline. */
#define LV_HITBOX_TANK_NONE   0
#define LV_HITBOX_TANK_CIRCLE 1   /* less than tank_hit_radius from the centre */
#define LV_HITBOX_TANK_SQUARE 2   /* less than 128 on both axes */

/* The zone of the game a recording is of. major, minor and revision are the
   version bytes of its header. Builds before 2.1.0 wrote one digit to a
   byte, so 1.91 is 1, 9, 1, and every later version is more: the circle came
   in 1.92 (#188). hasRules is whether the recording holds rule records
   (log_RuleSet), which only games after the circle write. A header with no
   version (all three 0) and no rule records gives NONE. A viewer built with
   BOLO_LEGACY_SQUARE_COLLISION draws the square for every game, as a server
   built with it plays. */
static inline int lvHitboxTankZoneFor(int major, int minor, int revision, bool hasRules) {
    bool old = major < 1 || (major == 1 && (minor < 9 || (minor == 9 && revision <= 1)));
    if (!hasRules && major == 0 && minor == 0 && revision == 0) {
        return LV_HITBOX_TANK_NONE;
    }
#ifdef BOLO_LEGACY_SQUARE_COLLISION
    (void)old;
    return LV_HITBOX_TANK_SQUARE;
#else
    return (old && !hasRules) ? LV_HITBOX_TANK_SQUARE : LV_HITBOX_TANK_CIRCLE;
#endif
}

/* Whether a recorded game counts a shell's hit that changes its square (the
   last hit on a wall, a forest, a boat, the shore, a bridge) as a hit on that
   square for the builder blast, from the version bytes of the recording's
   header. 2.2.1 (the Steam release) played the open-ground rule there: a
   builder within 128 of the square's middle dies. Every later version plays
   the square rule: only a builder on that square dies. The engine has no
   version check (it always plays the square rule); this is only so the
   viewer draws an older recording as it was played. A development build
   between 2.2.1 and the next release also reports 2.2.1, so its recordings
   draw with the open-ground rule. */
#define LV_HITBOX_SQUARE_HIT_MAJOR    2
#define LV_HITBOX_SQUARE_HIT_MINOR    2
#define LV_HITBOX_SQUARE_HIT_REVISION 2
static inline bool lvHitboxSquareHitForVersion(int major, int minor, int revision) {
    if (major != LV_HITBOX_SQUARE_HIT_MAJOR) return major > LV_HITBOX_SQUARE_HIT_MAJOR;
    if (minor != LV_HITBOX_SQUARE_HIT_MINOR) return minor > LV_HITBOX_SQUARE_HIT_MINOR;
    return revision >= LV_HITBOX_SQUARE_HIT_REVISION;
}

/* The tank test, as tankShellInHitZone makes it, for the zone: the shell's
   centre less than radius from the tank's centre for the circle, less than
   128 on both axes for the square, and never for NONE. dx and dy are the
   shell's place less the tank's, in world units. */
static inline bool lvHitboxTankHit(int dx, int dy, int zone, int radius) {
    if (zone == LV_HITBOX_TANK_CIRCLE) {
        return hitRulesTankCircle(dx, dy, radius);
    }
    if (zone == LV_HITBOX_TANK_SQUARE) {
        return hitRulesTankSquare(dx, dy);
    }
    return false;
}

/* The size the zone is drawn at, in world units from the tank's centre: the
   circle's radius or half the square's side. 0 for NONE. The test is "less
   than" this, so the drawn line is where the misses start (lv_hitbox_sums
   checks lvHitboxTankHit against it). */
static inline int lvHitboxTankZoneSize(int zone, int radius) {
    if (zone == LV_HITBOX_TANK_CIRCLE) {
        return radius;
    }
    if (zone == LV_HITBOX_TANK_SQUARE) {
        return HIT_RULES_HALF_SQUARE;
    }
    return 0;
}

/* The middle of a tank's picture, in art pixels from its top left: the
   game draws the picture LV_HITBOX_TANK_HALF up and left of his place, and
   his place is guessed to the middle of his art pixel (lvHitboxTankPlace). */
static inline double lvHitboxTankCentreInPicture(void) {
    return (lvHitboxTankPlace(0) + LV_HITBOX_TANK_HALF) / (double)LV_HITBOX_WU_PER_PIXEL;
}

/* The radius of the circle on the edge of the builder's area on open
   ground, for the lgm_blast_shape rule shape and the pill_range rule, or 0
   when the edge has no circle. The half-square circle (and a shape outside
   the three, which plays as it) is 128 and the area circle 144, each cut to
   pill_range when pill_range is less. The box has a circle only when
   pill_range cuts its corners off: pill_range under the corner's distance,
   128 times the square root of 2. */
static inline int lvHitboxBuilderReach(int shape, int pillRange) {
    int full;
    if (pillRange < 0) return 0;
    if (shape == LGM_BLAST_SHAPE_MAC_BOX) {
        return ((double)pillRange * pillRange <
                2.0 * LGM_BLAST_MAC_BOX_HALF * LGM_BLAST_MAC_BOX_HALF) ? pillRange : 0;
    }
    full = (shape == LGM_BLAST_SHAPE_MAC_AREA) ? LGM_BLAST_MAC_AREA_RADIUS
                                               : HIT_RULES_HALF_SQUARE;
    return pillRange < full ? pillRange : full;
}

/* Whether a pillbox makes its square solid for the builder test: it is on
   the map, not in a tank, and alive (armour above 0), as pillsIsPillHit
   asks in lgmBlastCatches. A dead pillbox's square is open ground. armour is
   the pill's armour as the recording has it. hitNow: the pill lost its last
   armour on this frame. That was a hit on the pill, and a shell's hit on a
   pill is a hit on its square whatever armour it leaves (shells.c
   outSquareHit), so the square stays solid on that frame. deadPillOpen: the
   game plays the dead-pill rule (2.2.2 and later, the same version as the
   square-hit rule below). When it is false (an older game) armour and
   hitNow are not read: every pillbox on the map, dead or alive, makes its
   square solid, as those games played. */
static inline bool lvHitboxPillSolid(bool onMap, bool inTank, unsigned armour,
                                     bool hitNow, bool deadPillOpen) {
    if (!onMap || inTank) return false;
    return !deadPillOpen || armour > 0 || hitNow;
}

/* Whether a square's terrain makes it solid for the builder test. wall: the
   square is a building or a half building as the recording has it.
   hitNow: a shell's hit on this square changed it on this frame: a wall to
   rubble (its last hit), a forest to grass, a boat to river, or a boat's
   shell on the shore or a bridge. Each is a hit on the square (shells.c
   outSquareHit), so the square is solid on that frame although the
   recording already has it as open ground. squareHitRule: the game plays
   that rule, lvHitboxSquareHitForVersion for the recording's
   version (after 2.2.1). When it is false (an older game) hitNow is
   not read: the square is solid only when it is a wall, the open-ground
   rule those games played. */
static inline bool lvHitboxSquareSolid(bool wall, bool hitNow,
                                       bool squareHitRule) {
    return wall || (squareHitRule && hitNow);
}

/* The builder test, as lgmBlastCatches makes it, for an explosion at (x, y)
   and a builder at (bx, by), both in world units measured from the top left
   corner of the square the builder is on. So the builder's square is the one
   from 0 to 255 on both axes, and an explosion's square is found by dividing
   by 256 (rounding down). solid is a mask of the 3 by 3 squares round the
   builder's: bit (row * 3 + col), with col and row 0, 1 or 2 and the
   builder's own square at col 1, row 1. A set bit is a building, a half
   building, a live pillbox (lvHitboxPillSolid), a square the frame's shot
   hit (lvHitboxSquareSolid) or a base.

   shape is the lgm_blast_shape rule and pillRange the pill_range rule, as
   hitRulesBuilderCaughtShape (hit_rules.h, the engine's own rule) takes
   them. On open ground the explosion must be
   no further than pillRange from him (none catches when pillRange is under
   0). Then shape 1 (the Mac Bolo box) asks for under 128 from him on each
   axis, shape 2 for no further than 144, and any other value for no further
   than 128.

   An explosion off the 3 by 3 squares is further than 144 from the builder
   on an axis, so no shape catches him: its square is not looked at. */
static inline bool lvHitboxBuilderCaughtShape(double x, double y, double bx,
                                              double by, unsigned solid,
                                              int shape, int pillRange) {
    int col, row;
    bool isSolid;

    col = (x < 0.0) ? 0 : (x < (double)LV_HITBOX_WU_PER_SQUARE) ? 1 :
          (x < 2.0 * LV_HITBOX_WU_PER_SQUARE) ? 2 : 3;
    row = (y < 0.0) ? 0 : (y < (double)LV_HITBOX_WU_PER_SQUARE) ? 1 :
          (y < 2.0 * LV_HITBOX_WU_PER_SQUARE) ? 2 : 3;
    if (x < -(double)LV_HITBOX_WU_PER_SQUARE || y < -(double)LV_HITBOX_WU_PER_SQUARE ||
        col > 2 || row > 2) {
        return false;
    }
    isSolid = ((solid >> (row * 3 + col)) & 1u) != 0;
    return hitRulesBuilderCaughtShape(shape, isSolid, col == 1 && row == 1, x - bx, y - by,
                                      pillRange);
}

/* The builder test for the classic shape, the half-square circle. pillRange
   is the pill_range rule; the circle's radius is lvHitboxBuilderReach's. */
static inline bool lvHitboxBuilderCaught(double x, double y, double bx, double by,
                                         unsigned solid, int pillRange) {
    return lvHitboxBuilderCaughtShape(x, y, bx, by, solid,
                                      LV_HITBOX_BUILDER_SHAPE_CIRCLE, pillRange);
}

/* The entry of a builder's notes the frame at time atMs shows: the newest
   note at or before atMs. ms, ax and ay are the notes, newest first: the
   playback time of each and his art pixel then, from the map's top left.
   For the present, atMs is the newest note's time or later. Returns the
   index, or -1 when there is no such note or he was not at (px, py) in it:
   the caller then takes the middle of the pixel. The time, not the pixel,
   picks the entry, since he can be in one pixel for two entries in a row
   and can walk back through a pixel later. */
static inline int lvHitboxLgmPickEntry(const uint32_t *ms, const int *ax, const int *ay,
                                       int count, uint32_t atMs, int px, int py) {
    int at;
    for (at = 0; at < count; at++) {
        if (ms[at] <= atMs) {
            break;
        }
    }
    if (at >= count || ax[at] != px || ay[at] != py) {
        return -1;
    }
    return at;
}

/* ---- A builder's outline ------------------------------------------------
 *
 * The border of the area lvHitboxBuilderCaught catches in: every piece of
 * line with the area on one side and not on the other. The pieces are the
 * circle's and the four square edges (x and y at 0 and 256) that cross the
 * 3 by 3 squares the circle can reach. The answer changes only where the
 * circle meets a square edge or a square edge meets another, so each
 * circle segment and each edge piece is cut at those places first and
 * then tested at its middle. So the outline goes to the true edge, not to
 * the middle of the nearest segment. For the Mac Bolo box the box's four
 * sides are pieces too.
 *
 * The outline is runs of joined points, in world units from the top left
 * of his square, ready to draw as lines (SDL_RenderLines takes one run). */
#define LV_HITBOX_OUTLINE_SEGMENTS 48     /* segments in the full circle */
#define LV_HITBOX_OUTLINE_PIECE    16     /* edge pieces, one art pixel long */
#define LV_HITBOX_OUTLINE_SIDE     1e-3   /* how far each side of a line is asked */
/* The circle: 48 segments and up to 8 places where it meets a square edge
   and 8 where it meets a side of the box. Each straight line: up to 35
   pieces and up to 6 places where something meets it. */
#define LV_HITBOX_OUTLINE_CIRCLE_MAX (LV_HITBOX_OUTLINE_SEGMENTS + 17)
#define LV_HITBOX_OUTLINE_EDGE_MAX       ((2 * LV_HITBOX_BUILDER_REACH_MAX + LV_HITBOX_WU_PER_SQUARE) / LV_HITBOX_OUTLINE_PIECE + 7)
#define LV_HITBOX_OUTLINE_MAX_RUNS   (LV_HITBOX_OUTLINE_CIRCLE_MAX + 8 * LV_HITBOX_OUTLINE_EDGE_MAX)
#define LV_HITBOX_OUTLINE_MAX_PTS    (2 * LV_HITBOX_OUTLINE_MAX_RUNS)

typedef struct {
    double x, y;
} LvHitPt;

typedef struct {
    int     nPts, nRuns;
    LvHitPt pt[LV_HITBOX_OUTLINE_MAX_PTS];
    int     runStart[LV_HITBOX_OUTLINE_MAX_RUNS + 1];   /* runStart[nRuns] == nPts */
} LvHitOutline;

/* Put the line from a to b on the outline: on the end of the last run when
   it starts where that run ends, else as a new run. */
static inline void lvHitboxOutlineAdd(LvHitOutline *o, LvHitPt a, LvHitPt b) {
    if (o->nRuns > 0 && o->nPts < LV_HITBOX_OUTLINE_MAX_PTS &&
        o->pt[o->nPts - 1].x == a.x && o->pt[o->nPts - 1].y == a.y) {
        o->pt[o->nPts++] = b;
        o->runStart[o->nRuns] = o->nPts;
        return;
    }
    if (o->nRuns >= LV_HITBOX_OUTLINE_MAX_RUNS || o->nPts + 2 > LV_HITBOX_OUTLINE_MAX_PTS) {
        return;
    }
    o->runStart[o->nRuns++] = o->nPts;
    o->pt[o->nPts++] = a;
    o->pt[o->nPts++] = b;
    o->runStart[o->nRuns] = o->nPts;
}

/* Sort a short list of numbers in place, smallest first. */
static inline void lvHitboxSortSmall(double *v, int n) {
    int i, j;
    for (i = 1; i < n; i++) {
        double t = v[i];
        for (j = i; j > 0 && v[j - 1] > t; j--) {
            v[j] = v[j - 1];
        }
        v[j] = t;
    }
}

/* The outline of the area for a builder at (bx, by), with the solid mask,
   the lgm_blast_shape rule and the pill_range rule of
   lvHitboxBuilderCaughtShape. The area's own border is a circle (shapes 0
   and 2, of radius 128 or 144, or pill_range when that is less), or for the
   box (shape 1) its four sides and, when pill_range cuts its corners, the
   circle of radius pill_range. */
static inline void lvHitboxBuilderOutlineShape(double bx, double by, unsigned solid,
                                               int shape, int pillRange,
                                               LvHitOutline *o) {
    const double twoPi = 2.0 * 3.14159265358979323846;
    const double side = LV_HITBOX_OUTLINE_SIDE;
    const double lo = -(double)LV_HITBOX_BUILDER_REACH_MAX;
    const double hi = (double)(LV_HITBOX_WU_PER_SQUARE + LV_HITBOX_BUILDER_REACH_MAX);
    const bool box = (shape == LV_HITBOX_BUILDER_SHAPE_BOX);
    const double half = (double)LV_HITBOX_BUILDER_BOX_HALF;
    double r = 0.0;     /* the circle on the border, 0 for none */
    double cut[LV_HITBOX_OUTLINE_EDGE_MAX + LV_HITBOX_OUTLINE_CIRCLE_MAX];
    int n, i, e;

    o->nPts = 0;
    o->nRuns = 0;
    o->runStart[0] = 0;
    if (pillRange <= 0) {
        /* Open ground catches nobody: only a solid square of his own. */
    } else if (box) {
        /* pill_range cuts the corners only when it is under the corner's
           distance, 128 * sqrt(2). */
        if ((double)pillRange * (double)pillRange < 2.0 * half * half) {
            r = (double)pillRange;
        }
    } else {
        r = (double)((shape == LV_HITBOX_BUILDER_SHAPE_AREA) ? LV_HITBOX_BUILDER_AREA_REACH
                                                             : LV_HITBOX_BUILDER_REACH);
        if ((double)pillRange < r) {
            r = (double)pillRange;
        }
    }

    /* The circle, cut at each segment's ends and wherever it meets a square
       edge or a side of the box. */
    if (r > 0.0) {
        n = 0;
        for (i = 0; i <= LV_HITBOX_OUTLINE_SEGMENTS; i++) {
            cut[n++] = (double)i * twoPi / LV_HITBOX_OUTLINE_SEGMENTS;
        }
        for (e = 0; e < 4; e++) {
            double xLine, yLine, c, s;
            if (e < 2) {
                xLine = (double)(e * LV_HITBOX_WU_PER_SQUARE);
                yLine = (double)(e * LV_HITBOX_WU_PER_SQUARE);
            } else if (box) {
                xLine = bx + ((e == 2) ? -half : half);
                yLine = by + ((e == 2) ? -half : half);
            } else {
                break;
            }
            c = (xLine - bx) / r;    /* x = xLine: cos(a) = c */
            s = (yLine - by) / r;    /* y = yLine: sin(a) = s */
            if (c > -1.0 && c < 1.0) {
                double a = acos(c);
                cut[n++] = a;
                cut[n++] = twoPi - a;
            }
            if (s > -1.0 && s < 1.0) {
                double a = asin(s);
                cut[n++] = (a < 0.0) ? a + twoPi : a;
                cut[n++] = 0.5 * twoPi - a;
            }
        }
        lvHitboxSortSmall(cut, n);
        for (i = 0; i + 1 < n; i++) {
            double a0 = cut[i], a1 = cut[i + 1], am = (a0 + a1) * 0.5;
            double cx = cos(am), cy = sin(am);
            LvHitPt p0, p1;
            if (a1 - a0 < 1e-12) continue;
            if (lvHitboxBuilderCaughtShape(bx + cx * (r - side), by + cy * (r - side), bx, by,
                                           solid, shape, pillRange) ==
                lvHitboxBuilderCaughtShape(bx + cx * (r + side), by + cy * (r + side), bx, by,
                                           solid, shape, pillRange)) {
                continue;
            }
            p0.x = bx + cos(a0) * r;  p0.y = by + sin(a0) * r;
            p1.x = bx + cos(a1) * r;  p1.y = by + sin(a1) * r;
            lvHitboxOutlineAdd(o, p0, p1);
        }
    }

    /* Straight lines: the square edges at 0 and 256 on each axis, from 144
       before his square to 144 after it (all any shape can reach), and for
       the box its four sides. Each is cut at each piece's ends and wherever
       the circle, a square edge or a side of the box meets it. */
    for (e = 0; e < 8; e++) {
        bool upright = ((e & 1) == 0);           /* x = line, else y = line */
        bool boxSide = (e >= 4);
        double line, across, along, from, to, t, h;
        int k;
        if (boxSide && !box) break;
        if (boxSide) {
            line = (upright ? bx : by) + ((e < 6) ? -half : half);
            from = (upright ? by : bx) - half;
            to = (upright ? by : bx) + half;
        } else {
            line = (double)((e >> 1) * LV_HITBOX_WU_PER_SQUARE);
            from = lo;
            to = hi;
        }
        across = upright ? line - bx : line - by;
        along = upright ? by : bx;
        n = 0;
        for (t = from; t < to; t += LV_HITBOX_OUTLINE_PIECE) {
            cut[n++] = t;
        }
        cut[n++] = to;
        if (r > 0.0 && across > -r && across < r) {
            h = sqrt(r * r - across * across);
            if (along - h > from && along - h < to) cut[n++] = along - h;
            if (along + h > from && along + h < to) cut[n++] = along + h;
        }
        /* The lines across this one: the square edges, and the box's
           sides. */
        for (k = 0; k < 4; k++) {
            double x;
            if (k < 2) {
                x = (double)(k * LV_HITBOX_WU_PER_SQUARE);
            } else if (box) {
                x = (upright ? by : bx) + ((k == 2) ? -half : half);
            } else {
                break;
            }
            if (x > from && x < to) cut[n++] = x;
        }
        lvHitboxSortSmall(cut, n);
        for (i = 0; i + 1 < n; i++) {
            double t0 = cut[i], t1 = cut[i + 1], m = (t0 + t1) * 0.5;
            bool a, b;
            LvHitPt p0, p1;
            if (t1 - t0 < 1e-9) continue;
            if (upright) {
                a = lvHitboxBuilderCaughtShape(line - side, m, bx, by, solid, shape, pillRange);
                b = lvHitboxBuilderCaughtShape(line + side, m, bx, by, solid, shape, pillRange);
                p0.x = line; p0.y = t0; p1.x = line; p1.y = t1;
            } else {
                a = lvHitboxBuilderCaughtShape(m, line - side, bx, by, solid, shape, pillRange);
                b = lvHitboxBuilderCaughtShape(m, line + side, bx, by, solid, shape, pillRange);
                p0.x = t0; p0.y = line; p1.x = t1; p1.y = line;
            }
            if (a != b) {
                lvHitboxOutlineAdd(o, p0, p1);
            }
        }
    }
}

/* The outline for the classic shape, the half-square circle. */
static inline void lvHitboxBuilderOutline(double bx, double by, unsigned solid, int pillRange,
                                          LvHitOutline *o) {
    lvHitboxBuilderOutlineShape(bx, by, solid, LV_HITBOX_BUILDER_SHAPE_CIRCLE, pillRange, o);
}

/* The pulsing dot at the centre of each shell explosion.
 *
 * The engine tests builders against the shell's own end place (newX, newY
 * in shellsUpdate, src/bolo/shells.c). The explosion record is put
 * TANK_SUBTRACT (128) world units up and left of that place and then
 * rounded down to an art pixel (explosionsAddItem), and the game draws the
 * explosion's 16 by 16 picture with its top left on that record. So the
 * blast point is 128 world units plus 0 to 15 more from the picture's top
 * left on each axis; the middle of those 16, 135.5, is the closest guess,
 * as for a tank (lvHitboxTankPlace). */
#define LV_HITBOX_BLAST_OFFSET 128
/* The dot is one art pixel across: 16 world units. */
#define LV_HITBOX_BLAST_DOT_WU LV_HITBOX_WU_PER_PIXEL
/* The pulse: one slow rise and fall a second, from 35% to 100% solid. */
#define LV_HITBOX_PULSE_MS   1000u
#define LV_HITBOX_PULSE_LOW  0.35
#define LV_HITBOX_PULSE_HIGH 1.0

/* The blast point, in art pixels from the top left of the explosion's
   picture (the same on both axes). */
static inline double lvHitboxBlastCentreInPicture(void) {
    return (lvHitboxTankPlace(0) + LV_HITBOX_BLAST_OFFSET) / (double)LV_HITBOX_WU_PER_PIXEL;
}

/* How solid the dot is at wall-clock time ms: a cosine, so it rises and
   falls smoothly with no hard step. It is at LV_HITBOX_PULSE_LOW at the
   start of each period and at LV_HITBOX_PULSE_HIGH half way. */
static inline double lvHitboxPulseAlpha(uint64_t ms) {
    const double twoPi = 2.0 * 3.14159265358979323846;
    double phase = (double)(ms % LV_HITBOX_PULSE_MS) / (double)LV_HITBOX_PULSE_MS;
    double rise = 0.5 - 0.5 * cos(phase * twoPi);
    return LV_HITBOX_PULSE_LOW + (LV_HITBOX_PULSE_HIGH - LV_HITBOX_PULSE_LOW) * rise;
}

/* The dot's colour cycles through the whole rainbow (red, yellow, green,
   cyan, blue, magenta and back to red) once each LV_HITBOX_RAINBOW_MS, by
   the wall clock, so it stands out on every kind of ground. */
#define LV_HITBOX_RAINBOW_MS 2000u

/* The rainbow colour at wall-clock time ms, each part 0..1: a hue that
   goes once round the colour wheel each LV_HITBOX_RAINBOW_MS, at full
   saturation and brightness. It is red at the start of each period. */
static inline void lvHitboxRainbow(uint64_t ms, double *r, double *g, double *b) {
    double h = 6.0 * (double)(ms % LV_HITBOX_RAINBOW_MS) / (double)LV_HITBOX_RAINBOW_MS;
    int sector = (int)h;
    double f = h - (double)sector;
    switch (sector) {
    case 0:  *r = 1.0;     *g = f;       *b = 0.0;     break;
    case 1:  *r = 1.0 - f; *g = 1.0;     *b = 0.0;     break;
    case 2:  *r = 0.0;     *g = 1.0;     *b = f;       break;
    case 3:  *r = 0.0;     *g = 1.0 - f; *b = 1.0;     break;
    case 4:  *r = f;       *g = 0.0;     *b = 1.0;     break;
    default: *r = 1.0;     *g = 0.0;     *b = 1.0 - f; break;
    }
}

/* The dot's size in screen pixels, for pxPerWu screen pixels to a world
   unit: one art pixel, and never less than one screen pixel, so it still
   shows at the smallest zoom. */
static inline float lvHitboxBlastDotSize(float pxPerWu) {
    float s = (float)LV_HITBOX_BLAST_DOT_WU * pxPerWu;
    return (s < 1.0f) ? 1.0f : s;
}

/* One record of the map's shell list: the square, the art pixel in it and
   the frame. Frames 1 to 8 are explosions (SHELL_EXPLOSION8 to
   SHELL_EXPLOSION1): an explosion starts at 8 and counts down. */
typedef struct LvHitBlastRec {
    uint8_t mx, my, px, py, frame;
} LvHitBlastRec;

/* The most records of the step before that lvHitboxNewBlasts can match. */
#define LV_HITBOX_BLAST_PREV_MAX 256

static inline bool lvHitboxIsBlastFrame(uint8_t frame) {
    return frame >= 1 && frame <= 8;
}

/* Which explosions of a recorded step are new, for the dot: isNew[i] is 1
   when cur[i] is an explosion with no explosion at its place in the step
   before (prev), and 0 otherwise.

   The dot cannot go by the frame. The server lowers each explosion's frame
   once every third tick, after it adds the new ones and before it writes
   the tick to the log, so an explosion can be first recorded at frame 7,
   and one at frame 8 can stay at 8 for two steps.

   An explosion does not move, and its frame never goes up. So an
   explosion of the step before at the same place, with a frame the same
   or higher, is the same one; each one of the step before matches one
   explosion only. The explosions are matched from the lowest frame up,
   each to the unused one of the step before with the closest frame, so
   that one in the middle of a stack does not take the match of a lower
   one (the step before 7, 5, 3, 1 and this step 7, 6, 4, 2: the 6 is the
   old 7, and the 7 is new). A new explosion on the place of an older one
   has a higher frame than it, and so is new.

   havePrev is false when the step before is not known (the first step,
   or a jump in time), and then no explosion is new: no dot is better than
   a wrong one. For the same reason no explosion is new when the step
   before has more records than LV_HITBOX_BLAST_PREV_MAX. */
static inline void lvHitboxNewBlasts(const LvHitBlastRec *prev, int nPrev, bool havePrev,
                                     const LvHitBlastRec *cur, int nCur, uint8_t *isNew) {
    bool used[LV_HITBOX_BLAST_PREV_MAX];
    int i, k, f;

    if (nCur <= 0 || isNew == NULL) {
        return;
    }
    if (nPrev < 0) nPrev = 0;
    if (!havePrev || nPrev > LV_HITBOX_BLAST_PREV_MAX) {
        for (i = 0; i < nCur; i++) {
            isNew[i] = 0;
        }
        return;
    }
    for (k = 0; k < nPrev; k++) {
        used[k] = false;
    }
    for (i = 0; i < nCur; i++) {
        isNew[i] = 0;
    }
    for (f = 1; f <= 8; f++) {
        for (i = 0; i < nCur; i++) {
            const LvHitBlastRec *c = &cur[i];
            int best = -1;
            if (c->frame != f) {
                continue;
            }
            for (k = 0; k < nPrev; k++) {
                const LvHitBlastRec *p = &prev[k];
                if (used[k] || !lvHitboxIsBlastFrame(p->frame) || p->mx != c->mx ||
                    p->my != c->my || p->px != c->px || p->py != c->py ||
                    p->frame < c->frame) {
                    continue;
                }
                if (best < 0 || p->frame < prev[best].frame) {
                    best = k;
                }
            }
            if (best >= 0) {
                used[best] = true;
            } else {
                isNew[i] = 1;
            }
        }
    }
}

/* The frame to draw for an explosion recorded at frame: a new explosion
   (lvHitboxNewBlasts) recorded at frame 7 is drawn with the frame 8
   picture on that step, so every explosion starts with its first picture.
   The server lowered the frame before it wrote the step; the recording is
   not changed. Every other step draws the recorded frame. */
static inline uint8_t lvHitboxDrawFrame(uint8_t frame, bool isNew) {
    return (isNew && frame == 7) ? 8 : frame;
}

#endif /* LV_HITBOX_H */
