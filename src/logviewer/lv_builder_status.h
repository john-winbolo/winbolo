/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer tank builder status sums
 *Filename:      lv_builder_status.h
 *Purpose:
 *  The small marker the Log Viewer can draw over each tank
 *  on the map for its builder (Options > Tank Builder
 *  Status; draw.c draws it):
 *
 *    builder in the tank      nothing
 *    builder out              an arrow from the tank toward
 *                             him (a dot when he is on the
 *                             tank's own square)
 *    builder dead             a parachute and the time until
 *                             the new builder is in the tank
 *
 *  This is the arithmetic: where the marker goes, which way
 *  the arrow points, the text of the countdown, the small
 *  pictures and digits the marker is made of, and the table
 *  of the times each builder was away that makes the
 *  countdown exact.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_BUILDER_STATUS_H
#define LV_BUILDER_STATUS_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lv_tank_bars.h"

/* The marker is made of square cells. The arrow and the parachute are 7
 * cells high and wide, a digit is 5 cells high and 3 wide. A cell is one
 * point, and two points from LV_BUILDER_BIG_TILE points to a map square: 7
 * points high at the usual zooms, 14 at the large ones. Like the bars, no
 * marker is drawn when a square is under LV_TANK_BARS_MIN_TILE points. */
#define LV_BUILDER_CELLS    7
#define LV_BUILDER_BIG_TILE 32.0f

/* The space between the marker and what is under it (the armour bar, or
 * the tank when the bars are off), in points: the bars' own. */
#define LV_BUILDER_GAP LV_TANK_BARS_GAP

/* Drawn 75% solid, like the bars. */
#define LV_BUILDER_ALPHA LV_TANK_BARS_ALPHA

/* The arrow points one of 16 ways. */
#define LV_BUILDER_DIRECTIONS 16

/* A map square is 256 world units and a builder's place in the log is to a
 * sixteenth of a square. The game moves builders once in each 20 ms tick. */
#define LV_BUILDER_SQUARE_WORLD 256
#define LV_BUILDER_STEP_WORLD   16
#define LV_BUILDER_TICK_MS      20

/* The game's own speed of a new builder on his way in, in world units a
 * tick, and how near his goal counts as there (sim rules
 * lgm_helicopter_speed and lgm_arrive_tolerance). */
#define LV_BUILDER_FLY_SPEED     3
#define LV_BUILDER_FLY_TOLERANCE 16

/* The longest countdown text: "~99:59" and the end. */
#define LV_BUILDER_TEXT_MAX 8

typedef enum LvBuilderState {
    LV_BUILDER_IN = 0,  /* in the tank: no marker */
    LV_BUILDER_OUT,     /* out of the tank, alive: the arrow */
    LV_BUILDER_FLYING   /* dead; the new one is on his way: the parachute */
} LvBuilderState;

/* isOut: the log named a place for the builder in this tick. isDead: he
 * was killed and the new one has not landed. Dead wins: in the tick of the
 * death the log may name no place. */
static inline LvBuilderState lvBuilderState(bool isOut, bool isDead) {
    if (isDead) return LV_BUILDER_FLYING;
    return isOut ? LV_BUILDER_OUT : LV_BUILDER_IN;
}

/* ── Where the marker goes ─────────────────────────────────────────── */

typedef struct LvBuilderMarkLayout {
    bool  visible;  /* false: the squares are too small, draw none */
    float cell;     /* one cell, in framebuffer pixels (whole) */
    float centreX;  /* the middle of the tank's picture */
    float top;      /* the top of the marker */
    float height;   /* LV_BUILDER_CELLS cells */
} LvBuilderMarkLayout;

/* One cell, in framebuffer pixels, for squares tilePx framebuffer pixels
 * across on a display of pixel density pxScale. */
static inline float lvBuilderCell(float tilePx, float pxScale) {
    if (pxScale <= 0.0f) pxScale = 1.0f;
    return lvTankBarPointsToPixels(
        (tilePx >= LV_BUILDER_BIG_TILE * pxScale) ? 2.0f : 1.0f, pxScale);
}

/* The marker for a tank whose picture has its top left at tankLeft, tankTop
 * in the framebuffer. It is one gap above the armour bar when the bars are
 * drawn (barsOn) and one gap above the picture when they are not, and its
 * middle is over the middle of the picture. */
static inline LvBuilderMarkLayout lvBuilderMarkLayout(float tankLeft, float tankTop,
                                                      float tilePx, float pxScale,
                                                      bool barsOn) {
    LvBuilderMarkLayout out;
    LvTankBarsLayout bars = lvTankBarsLayout(tankLeft, tankTop, tilePx, pxScale);
    float gap = lvTankBarPointsToPixels(LV_BUILDER_GAP, pxScale);
    float under = barsOn ? bars.armour.y : floorf(tankTop);

    out.visible = bars.visible;
    out.cell = lvBuilderCell(tilePx, pxScale);
    out.height = out.cell * (float)LV_BUILDER_CELLS;
    out.top = under - gap - out.height;
    out.centreX = floorf(tankLeft) + floorf(tilePx + 0.5f) * 0.5f;
    return out;
}

/* ── The arrow ─────────────────────────────────────────────────────── */

/* Which of the 16 ways the arrow points for a builder dx to the right of
 * the tank and dy below it (any unit). 0 is up, 4 right, 8 down, 12 left.
 * -1 when he is at the tank's own place. */
static inline int lvBuilderDirection(float dx, float dy) {
    const float kTurn = 6.28318530718f;
    float a;
    int   bucket;

    if (dx == 0.0f && dy == 0.0f) return -1;
    a = atan2f(dx, -dy);    /* 0 up, a quarter turn right */
    if (a < 0.0f) a += kTurn;
    bucket = (int)floorf(a / (kTurn / (float)LV_BUILDER_DIRECTIONS) + 0.5f);
    return bucket % LV_BUILDER_DIRECTIONS;
}

/* The way a direction points on the screen, as a unit step: x to the right,
 * y down. */
static inline void lvBuilderDirectionStep(int direction, float *x, float *y) {
    const float kTurn = 6.28318530718f;
    float a = kTurn * (float)direction / (float)LV_BUILDER_DIRECTIONS;
    *x = sinf(a);
    *y = -cosf(a);
}

/* A place in world units out of the log's square and sixteenth. */
static inline int lvBuilderWorld(int square, int sixteenth) {
    return square * LV_BUILDER_SQUARE_WORLD + sixteenth * LV_BUILDER_STEP_WORLD;
}

/* ── The countdown ─────────────────────────────────────────────────── */

/* The time left as text: "0:07", "1:23". Seconds are rounded up, so "0:00"
 * is shown only when no time is left; a time under zero reads "0:00". An
 * estimate has "~" in front. Minutes stop at 99. Returns the length. */
static inline int lvBuilderCountdownText(long remainMs, bool approx, char *dest,
                                         size_t size) {
    long secs;
    int  n;

    if (dest == NULL || size == 0) return 0;
    if (remainMs < 0) remainMs = 0;
    secs = (remainMs + 999) / 1000;
    if (secs > 99L * 60L + 59L) secs = 99L * 60L + 59L;
    n = snprintf(dest, size, "%s%ld:%02ld", approx ? "~" : "", secs / 60, secs % 60);
    if (n < 0) { dest[0] = '\0'; return 0; }
    if ((size_t)n >= size) n = (int)size - 1;
    return n;
}

/* How long a new builder still flies, estimated by the game's rule: he
 * moves in a straight line toward his goal at speed world units a tick and
 * has landed when he is within tolerance of it. dx, dy: from him to the
 * goal, in world units. For a live feed, where the log does not hold the
 * landing yet. */
static inline long lvBuilderFlightMs(long dx, long dy, int speed, int tolerance) {
    double dist, ticks;

    if (speed <= 0) speed = LV_BUILDER_FLY_SPEED;
    if (tolerance < 0) tolerance = 0;
    dist = sqrt((double)dx * (double)dx + (double)dy * (double)dy) - (double)tolerance;
    if (dist <= 0.0) return 0;
    ticks = ceil(dist / (double)speed);
    return (long)ticks * LV_BUILDER_TICK_MS;
}

/* ── The table of the times a builder was away ─────────────────────── */

/* From the tick a builder was killed to the tick the new one was in the
 * tank. backMs is 0 when the log ended first. */
typedef struct LvBuilderTrip {
    uint32_t deathMs;
    uint32_t backMs;
} LvBuilderTrip;

/* The walk that fills the table, for one player. */
typedef struct LvBuilderTrack {
    bool          open;
    LvBuilderTrip trip;
} LvBuilderTrack;

/* One tick of the walk: the builder's state at nowMs. Returns true, with
 * the trip in done, in the tick the new builder is in the tank. A builder
 * killed again on his walk back stays in the same trip. */
static inline bool lvBuilderTrackTick(LvBuilderTrack *t, uint32_t nowMs, bool isOut,
                                      bool isDead, LvBuilderTrip *done) {
    if (!t->open) {
        if (isDead) {
            t->open = true;
            t->trip.deathMs = nowMs;
            t->trip.backMs = 0;
        }
        return false;
    }
    if (isOut || isDead) return false;
    t->trip.backMs = nowMs;
    t->open = false;
    if (done != NULL) *done = t->trip;
    return true;
}

/* The end of the walk: a trip still open (the log ended with the builder
 * away). Returns true with it in done; its backMs is 0. */
static inline bool lvBuilderTrackEnd(LvBuilderTrack *t, LvBuilderTrip *done) {
    if (!t->open) return false;
    t->open = false;
    t->trip.backMs = 0;
    if (done != NULL) *done = t->trip;
    return true;
}

/* The trip nowMs is in: killed at or before it, back after it (or never).
 * The trips are in the order of time. -1 when there is none. */
static inline int lvBuilderTripFind(const LvBuilderTrip *trips, int count,
                                    uint32_t nowMs) {
    int lo = 0, hi = count - 1, found = -1;

    if (trips == NULL) return -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (trips[mid].deathMs <= nowMs) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (found < 0) return -1;
    if (trips[found].backMs != 0 && trips[found].backMs <= nowMs) return -1;
    return found;
}

/* The time left at nowMs until the builder is in the tank, out of the
 * table. False when nowMs is in no trip, or the trip has no end. */
static inline bool lvBuilderTripRemaining(const LvBuilderTrip *trips, int count,
                                          uint32_t nowMs, long *remainMs) {
    int i = lvBuilderTripFind(trips, count, nowMs);
    if (i < 0 || trips[i].backMs == 0) return false;
    if (remainMs != NULL) *remainMs = (long)(trips[i].backMs - nowMs);
    return true;
}

/* ── The small pictures ────────────────────────────────────────────── */

/* A picture is rows of cells; bit (width - 1 - column) of a row is set
 * where the cell is drawn. */
typedef struct LvBuilderGlyph {
    unsigned char width;
    unsigned char height;
    unsigned char rows[LV_BUILDER_CELLS];
} LvBuilderGlyph;

/* The parachute: the canopy, its lines, and the builder under them. */
static inline const LvBuilderGlyph *lvBuilderParachute(void) {
    static const LvBuilderGlyph k = { 7, 7, {
        0x3E,   /* .#####. */
        0x7F,   /* ####### */
        0x7F,   /* ####### */
        0x49,   /* #..#..# */
        0x2A,   /* .#.#.#. */
        0x1C,   /* ..###.. */
        0x08    /* ...#... */
    } };
    return &k;
}

/* The dot for a builder on the tank's own square. */
static inline const LvBuilderGlyph *lvBuilderDot(void) {
    static const LvBuilderGlyph k = { 3, 3, { 0x7, 0x7, 0x7, 0, 0, 0, 0 } };
    return &k;
}

/* A character of the countdown: the digits, ":" and "~". NULL for any
 * other. A digit is 3 cells wide and 5 high. */
static inline const LvBuilderGlyph *lvBuilderChar(char c) {
    static const LvBuilderGlyph kDigits[10] = {
        { 3, 5, { 0x7, 0x5, 0x5, 0x5, 0x7, 0, 0 } },
        { 3, 5, { 0x2, 0x6, 0x2, 0x2, 0x7, 0, 0 } },
        { 3, 5, { 0x7, 0x1, 0x7, 0x4, 0x7, 0, 0 } },
        { 3, 5, { 0x7, 0x1, 0x7, 0x1, 0x7, 0, 0 } },
        { 3, 5, { 0x5, 0x5, 0x7, 0x1, 0x1, 0, 0 } },
        { 3, 5, { 0x7, 0x4, 0x7, 0x1, 0x7, 0, 0 } },
        { 3, 5, { 0x7, 0x4, 0x7, 0x5, 0x7, 0, 0 } },
        { 3, 5, { 0x7, 0x1, 0x1, 0x1, 0x1, 0, 0 } },
        { 3, 5, { 0x7, 0x5, 0x7, 0x5, 0x7, 0, 0 } },
        { 3, 5, { 0x7, 0x5, 0x7, 0x1, 0x7, 0, 0 } }
    };
    static const LvBuilderGlyph kColon = { 1, 5, { 0x0, 0x1, 0x0, 0x1, 0x0, 0, 0 } };
    /* .#..#
       #.##.  */
    static const LvBuilderGlyph kTilde = { 5, 5, { 0x00, 0x09, 0x16, 0x00, 0x00, 0, 0 } };

    if (c >= '0' && c <= '9') return &kDigits[c - '0'];
    if (c == ':') return &kColon;
    if (c == '~') return &kTilde;
    return NULL;
}

/* Whether the cell at column, row of a picture is drawn. */
static inline bool lvBuilderGlyphCell(const LvBuilderGlyph *g, int column, int row) {
    if (g == NULL || column < 0 || row < 0 || column >= g->width || row >= g->height) {
        return false;
    }
    return ((g->rows[row] >> (g->width - 1 - column)) & 1u) != 0;
}

/* How wide a text is, in cells: its characters with one cell between each
 * two. A character with no picture takes no room. */
static inline int lvBuilderTextCells(const char *text) {
    int w = 0;
    if (text == NULL) return 0;
    for (; *text != '\0'; text++) {
        const LvBuilderGlyph *g = lvBuilderChar(*text);
        if (g == NULL) continue;
        if (w > 0) w += 1;
        w += g->width;
    }
    return w;
}

/* How wide the whole marker is, in cells: the picture (the arrow, the dot's
 * box or the parachute, LV_BUILDER_CELLS wide), then, when there is a
 * countdown, one cell and the text. */
static inline int lvBuilderRowCells(const char *text) {
    int t = lvBuilderTextCells(text);
    return LV_BUILDER_CELLS + ((t > 0) ? 1 + t : 0);
}

#endif /* LV_BUILDER_STATUS_H */
