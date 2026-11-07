/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer hit boxes sums
 *Filename:      lv_hitbox.h
 *Purpose:
 *  The areas the Log Viewer draws round each tank and each
 *  builder on the map (Options > Toggle Hit Boxes; draw.c
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
 *    pill's square. On one of
 *    those squares it catches him only when it is on his own
 *    square. The distance comes from utilIsItemInRange with
 *    the pill_range rule as its limit, so when pill_range is
 *    under 128 the circle is pill_range across, not 128.
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

/* The distance an explosion on open ground catches a builder at. The engine
   asks for distance <= 128, but the distance it reads is the one
   utilIsItemInRange gives with pill_range as its limit: past the limit it
   gives WORLD_MAX, which never passes. So the catch distance is the smaller
   of the two. */
static inline int lvHitboxBuilderReach(int pillRange) {
    if (pillRange < 0) return 0;
    return pillRange < LV_HITBOX_BUILDER_REACH ? pillRange : LV_HITBOX_BUILDER_REACH;
}

/* Whether a pillbox makes its square solid for the builder test: it is on
   the map, not in a tank, and alive (armour above 0), as pillsIsPillHit
   asks in lgmBlastCatches. A dead pillbox's square is open ground. armour is
   the pill's armour as the recording has it. hitNow: the pill lost its last
   armour on this frame. That was a hit on the pill, and a shell's hit on a
   pill is a hit on its square whatever armour it leaves (shells.c
   outSquareHit), so the square stays solid on that frame. */
static inline bool lvHitboxPillSolid(bool onMap, bool inTank, unsigned armour,
                                     bool hitNow) {
    return onMap && !inTank && (armour > 0 || hitNow);
}

/* The builder test, as lgmBlastCatches makes it, for an explosion at (x, y)
   and a builder at (bx, by), both in world units measured from the top left
   corner of the square the builder is on. So the builder's square is the one
   from 0 to 255 on both axes, and an explosion's square is found by dividing
   by 256 (rounding down). solid is a mask of the 3 by 3 squares round the
   builder's: bit (row * 3 + col), with col and row 0, 1 or 2 and the
   builder's own square at col 1, row 1. A set bit is a building, a half
   building, a live pillbox (lvHitboxPillSolid) or a base. reach is
   lvHitboxBuilderReach's. The rule is hitRulesBuilderCaught's, the
   engine's own. reach is given to it as the pill range, which gives the
   same answer, since reach is pill_range or 128, whichever is less.

   An explosion off the 3 by 3 squares is further than 128 from the builder,
   so it never catches him: its square is not looked at. */
static inline bool lvHitboxBuilderCaught(double x, double y, double bx, double by,
                                         unsigned solid, int reach) {
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
    return hitRulesBuilderCaught(isSolid, col == 1 && row == 1, x - bx, y - by, reach);
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
 * the middle of the nearest segment.
 *
 * The outline is runs of joined points, in world units from the top left
 * of his square, ready to draw as lines (SDL_RenderLines takes one run). */
#define LV_HITBOX_OUTLINE_SEGMENTS 48     /* segments in the full circle */
#define LV_HITBOX_OUTLINE_PIECE    16     /* edge pieces, one art pixel long */
#define LV_HITBOX_OUTLINE_SIDE     1e-3   /* how far each side of a line is asked */
/* The circle: 48 segments and up to 8 places where it meets an edge. Each
   edge: 32 pieces and up to 2 places where it meets the circle. */
#define LV_HITBOX_OUTLINE_CIRCLE_MAX (LV_HITBOX_OUTLINE_SEGMENTS + 9)
#define LV_HITBOX_OUTLINE_EDGE_MAX   \
    ((2 * LV_HITBOX_BUILDER_REACH + LV_HITBOX_WU_PER_SQUARE) / LV_HITBOX_OUTLINE_PIECE + 3)
#define LV_HITBOX_OUTLINE_MAX_RUNS   (LV_HITBOX_OUTLINE_CIRCLE_MAX + 4 * LV_HITBOX_OUTLINE_EDGE_MAX)
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

/* The outline of the area for a builder at (bx, by), with the solid mask
   and the reach of lvHitboxBuilderCaught. */
static inline void lvHitboxBuilderOutline(double bx, double by, unsigned solid, int reach,
                                          LvHitOutline *o) {
    const double twoPi = 2.0 * 3.14159265358979323846;
    const double r = (double)reach;
    const double side = LV_HITBOX_OUTLINE_SIDE;
    const double lo = -(double)LV_HITBOX_BUILDER_REACH;
    const double hi = (double)(LV_HITBOX_WU_PER_SQUARE + LV_HITBOX_BUILDER_REACH);
    double cut[LV_HITBOX_OUTLINE_EDGE_MAX + LV_HITBOX_OUTLINE_CIRCLE_MAX];
    int n, i, e;

    o->nPts = 0;
    o->nRuns = 0;
    o->runStart[0] = 0;

    /* The circle, cut at each segment's ends and wherever it meets an edge. */
    if (reach > 0) {
        n = 0;
        for (i = 0; i <= LV_HITBOX_OUTLINE_SEGMENTS; i++) {
            cut[n++] = (double)i * twoPi / LV_HITBOX_OUTLINE_SEGMENTS;
        }
        for (e = 0; e < 2; e++) {
            double line = (double)(e * LV_HITBOX_WU_PER_SQUARE);
            double c = (line - bx) / r;    /* x = line: cos(a) = c */
            double s = (line - by) / r;    /* y = line: sin(a) = s */
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
            if (lvHitboxBuilderCaught(bx + cx * (r - side), by + cy * (r - side), bx, by,
                                      solid, reach) ==
                lvHitboxBuilderCaught(bx + cx * (r + side), by + cy * (r + side), bx, by,
                                      solid, reach)) {
                continue;
            }
            p0.x = bx + cos(a0) * r;  p0.y = by + sin(a0) * r;
            p1.x = bx + cos(a1) * r;  p1.y = by + sin(a1) * r;
            lvHitboxOutlineAdd(o, p0, p1);
        }
    }

    /* The edges at 0 and 256 on each axis, from half a square before his
       square to half a square after it (all the circle can reach), cut at
       each piece's ends and wherever the circle meets them. */
    for (e = 0; e < 4; e++) {
        bool upright = (e < 2);                 /* x = line, else y = line */
        double line = (double)((e & 1) * LV_HITBOX_WU_PER_SQUARE);
        double across = upright ? line - bx : line - by;
        double along = upright ? by : bx;
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
        lvHitboxSortSmall(cut, n);
        for (i = 0; i + 1 < n; i++) {
            double t0 = cut[i], t1 = cut[i + 1], m = (t0 + t1) * 0.5;
            bool a, b;
            LvHitPt p0, p1;
            if (t1 - t0 < 1e-9) continue;
            if (upright) {
                a = lvHitboxBuilderCaught(line - side, m, bx, by, solid, reach);
                b = lvHitboxBuilderCaught(line + side, m, bx, by, solid, reach);
                p0.x = line; p0.y = t0; p1.x = line; p1.y = t1;
            } else {
                a = lvHitboxBuilderCaught(m, line - side, bx, by, solid, reach);
                b = lvHitboxBuilderCaught(m, line + side, bx, by, solid, reach);
                p0.x = t0; p0.y = line; p1.x = t1; p1.y = line;
            }
            if (a != b) {
                lvHitboxOutlineAdd(o, p0, p1);
            }
        }
    }
}

#endif /* LV_HITBOX_H */
