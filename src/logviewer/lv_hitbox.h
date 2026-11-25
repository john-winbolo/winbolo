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
 *  - A builder: lgmBlastCatches (src/bolo/lgm.c), which
 *    applies hitRulesBuilderCaught. An explosion catches a
 *    builder when it is on a square that is not a building, a
 *    half building, a live pillbox or a base, and he is in the
 *    lgm_blast_shape rule's area round it
 *    (lgmBlastOpenGroundCatches): a circle of half a square
 *    (128), the Mac Bolo box (less than 128 on both axes) or a
 *    circle of 144. On one of those squares it catches him
 *    only when it is on his own square. Every shape stops at
 *    the pill_range rule, so when pill_range is under the
 *    shape's reach the area is cut to a circle of pill_range.
 *
 *    The outline is for an explosion with no square hit: a
 *    mine, a tank's explosion, and a shell that bursts on open
 *    ground (it ran out, or hit a tank). A shell that hits a
 *    wall, a live pillbox, forest, a boat, the shore a boat's
 *    shell stops on or a bridge catches only a builder on that
 *    square (the squareHit of lgmBlastCatches), wherever he is
 *    on it. The outline does not show that case.
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
/* The furthest on one axis an explosion on open ground can catch a
   builder from, for any lgm_blast_shape: the circle of 144 reaches 144
   across, the box 128 and the half-square circle 128. The outline looks
   for the area's edge this far round his square. */
#define LV_HITBOX_BUILDER_REACH LGM_BLAST_MAC_AREA_RADIUS
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

/* The builder test, as lgmBlastCatches makes it, for an explosion at (x, y)
   and a builder at (bx, by), both in world units measured from the top left
   corner of the square the builder is on. So the builder's square is the one
   from 0 to 255 on both axes, and an explosion's square is found by dividing
   by 256 (rounding down). solid is a mask of the 3 by 3 squares round the
   builder's: bit (row * 3 + col), with col and row 0, 1 or 2 and the
   builder's own square at col 1, row 1. A set bit is a building, a half
   building, a live pillbox or a base. shape and pillRange are the
   lgm_blast_shape and pill_range rules. The rule is hitRulesBuilderCaught,
   the one lgmBlastCatches applies.

   An explosion off the 3 by 3 squares is further than
   LV_HITBOX_BUILDER_REACH from the builder on one axis, so it never
   catches him: its square is not looked at. */
static inline bool lvHitboxBuilderCaught(double x, double y, double bx, double by,
                                         unsigned solid, int shape, int pillRange) {
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
    return hitRulesBuilderCaught(isSolid, col == 1 && row == 1, x - bx, y - by, shape,
                                 pillRange);
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
 * circle's (lvHitboxBuilderReach), the four square edges (x and y at 0 and
 * 256) that cross the 3 by 3 squares the area can reach, and for the Mac
 * Bolo box its four sides (x and y at 128 each side of him). The answer
 * changes only where two of these meet, so each circle segment and each
 * line piece is cut at those places first and then tested at its middle.
 * So the outline goes to the true edge, not to the middle of the nearest
 * segment.
 *
 * The outline is runs of joined points, in world units from the top left
 * of his square, ready to draw as lines (SDL_RenderLines takes one run). */
#define LV_HITBOX_OUTLINE_SEGMENTS 48     /* segments in the full circle */
#define LV_HITBOX_OUTLINE_PIECE    16     /* edge pieces, one art pixel long */
#define LV_HITBOX_OUTLINE_SIDE     1e-3   /* how far each side of a line is asked */
/* The straight lines: the four square edges and the box's four sides. */
#define LV_HITBOX_OUTLINE_LINES    8
/* The circle: 48 segments and up to 2 places where it meets each line.
   Each line: its pieces, up to 2 places where it meets the circle, and the
   places where the lines across it meet it (half the lines at most). */
#define LV_HITBOX_OUTLINE_CIRCLE_MAX \
    (LV_HITBOX_OUTLINE_SEGMENTS + 1 + 2 * LV_HITBOX_OUTLINE_LINES)
#define LV_HITBOX_OUTLINE_EDGE_MAX   \
    ((2 * LV_HITBOX_BUILDER_REACH + LV_HITBOX_WU_PER_SQUARE) / LV_HITBOX_OUTLINE_PIECE + 3 + \
     LV_HITBOX_OUTLINE_LINES / 2)
#define LV_HITBOX_OUTLINE_MAX_RUNS   \
    (LV_HITBOX_OUTLINE_CIRCLE_MAX + LV_HITBOX_OUTLINE_LINES * LV_HITBOX_OUTLINE_EDGE_MAX)
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

/* Put a straight line (x = v when up, else y = v) on the list, once. */
static inline void lvHitboxOutlineLine(bool *upright, double *value, int *n, bool up,
                                       double v) {
    int i;
    for (i = 0; i < *n; i++) {
        if (upright[i] == up && value[i] == v) return;
    }
    if (*n < LV_HITBOX_OUTLINE_LINES) {
        upright[*n] = up;
        value[*n] = v;
        (*n)++;
    }
}

/* The outline of the area for a builder at (bx, by), with the solid mask
   and the rules of lvHitboxBuilderCaught. */
static inline void lvHitboxBuilderOutline(double bx, double by, unsigned solid, int shape,
                                          int pillRange, LvHitOutline *o) {
    const double twoPi = 2.0 * 3.14159265358979323846;
    const int reach = lvHitboxBuilderReach(shape, pillRange);
    const double r = (double)reach;
    const double side = LV_HITBOX_OUTLINE_SIDE;
    const double lo = -(double)LV_HITBOX_BUILDER_REACH;
    const double hi = (double)(LV_HITBOX_WU_PER_SQUARE + LV_HITBOX_BUILDER_REACH);
    double cut[LV_HITBOX_OUTLINE_EDGE_MAX + LV_HITBOX_OUTLINE_CIRCLE_MAX];
    bool upright[LV_HITBOX_OUTLINE_LINES];
    double value[LV_HITBOX_OUTLINE_LINES];
    int nLines = 0;
    int n, i, e, k;

    o->nPts = 0;
    o->nRuns = 0;
    o->runStart[0] = 0;

    /* The square edges at 0 and 256 on each axis, and the box's sides. */
    for (e = 0; e < 2; e++) {
        lvHitboxOutlineLine(upright, value, &nLines, true,
                            (double)(e * LV_HITBOX_WU_PER_SQUARE));
        lvHitboxOutlineLine(upright, value, &nLines, false,
                            (double)(e * LV_HITBOX_WU_PER_SQUARE));
    }
    if (shape == LGM_BLAST_SHAPE_MAC_BOX && pillRange >= 0) {
        for (e = -1; e <= 1; e += 2) {
            lvHitboxOutlineLine(upright, value, &nLines, true,
                                bx + (double)(e * LGM_BLAST_MAC_BOX_HALF));
            lvHitboxOutlineLine(upright, value, &nLines, false,
                                by + (double)(e * LGM_BLAST_MAC_BOX_HALF));
        }
    }

    /* The circle, cut at each segment's ends and wherever it meets a line. */
    if (reach > 0) {
        n = 0;
        for (i = 0; i <= LV_HITBOX_OUTLINE_SEGMENTS; i++) {
            cut[n++] = (double)i * twoPi / LV_HITBOX_OUTLINE_SEGMENTS;
        }
        for (k = 0; k < nLines; k++) {
            if (upright[k]) {
                double c = (value[k] - bx) / r;    /* x = line: cos(a) = c */
                if (c > -1.0 && c < 1.0) {
                    double a = acos(c);
                    cut[n++] = a;
                    cut[n++] = twoPi - a;
                }
            } else {
                double sn = (value[k] - by) / r;   /* y = line: sin(a) = sn */
                if (sn > -1.0 && sn < 1.0) {
                    double a = asin(sn);
                    cut[n++] = (a < 0.0) ? a + twoPi : a;
                    cut[n++] = 0.5 * twoPi - a;
                }
            }
        }
        lvHitboxSortSmall(cut, n);
        for (i = 0; i + 1 < n; i++) {
            double a0 = cut[i], a1 = cut[i + 1], am = (a0 + a1) * 0.5;
            double cx = cos(am), cy = sin(am);
            LvHitPt p0, p1;
            if (a1 - a0 < 1e-12) continue;
            if (lvHitboxBuilderCaught(bx + cx * (r - side), by + cy * (r - side), bx, by,
                                      solid, shape, pillRange) ==
                lvHitboxBuilderCaught(bx + cx * (r + side), by + cy * (r + side), bx, by,
                                      solid, shape, pillRange)) {
                continue;
            }
            p0.x = bx + cos(a0) * r;  p0.y = by + sin(a0) * r;
            p1.x = bx + cos(a1) * r;  p1.y = by + sin(a1) * r;
            lvHitboxOutlineAdd(o, p0, p1);
        }
    }

    /* Each line, from LV_HITBOX_BUILDER_REACH before his square to as far
       after it (all the area can reach), cut at each piece's ends and
       wherever the circle or a line across it meets it. */
    for (e = 0; e < nLines; e++) {
        bool up = upright[e];                   /* x = line, else y = line */
        double line = value[e];
        double across = up ? line - bx : line - by;
        double along = up ? by : bx;
        double t;
        n = 0;
        for (t = lo; t <= hi; t += LV_HITBOX_OUTLINE_PIECE) {
            cut[n++] = t;
        }
        if (reach > 0 && across > -r && across < r) {
            double h = sqrt(r * r - across * across);
            if (along - h > lo && along - h < hi) cut[n++] = along - h;
            if (along + h > lo && along + h < hi) cut[n++] = along + h;
        }
        for (k = 0; k < nLines; k++) {
            if (upright[k] != up && value[k] > lo && value[k] < hi) {
                cut[n++] = value[k];
            }
        }
        lvHitboxSortSmall(cut, n);
        for (i = 0; i + 1 < n; i++) {
            double t0 = cut[i], t1 = cut[i + 1], m = (t0 + t1) * 0.5;
            bool a, b;
            LvHitPt p0, p1;
            if (t1 - t0 < 1e-9) continue;
            if (up) {
                a = lvHitboxBuilderCaught(line - side, m, bx, by, solid, shape, pillRange);
                b = lvHitboxBuilderCaught(line + side, m, bx, by, solid, shape, pillRange);
                p0.x = line; p0.y = t0; p1.x = line; p1.y = t1;
            } else {
                a = lvHitboxBuilderCaught(m, line - side, bx, by, solid, shape, pillRange);
                b = lvHitboxBuilderCaught(m, line + side, bx, by, solid, shape, pillRange);
                p0.x = t0; p0.y = line; p1.x = t1; p1.y = line;
            }
            if (a != b) {
                lvHitboxOutlineAdd(o, p0, p1);
            }
        }
    }
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

/* The dot's size in screen pixels, for pxPerWu screen pixels to a world
   unit: one art pixel, and never less than one screen pixel, so it still
   shows at the smallest zoom. */
static inline float lvHitboxBlastDotSize(float pxPerWu) {
    float s = (float)LV_HITBOX_BLAST_DOT_WU * pxPerWu;
    return (s < 1.0f) ? 1.0f : s;
}

#endif /* LV_HITBOX_H */
