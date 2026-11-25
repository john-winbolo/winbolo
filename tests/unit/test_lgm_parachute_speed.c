/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The speed a dead builder parachutes in at (lgm_helicopter_speed,
 * lgmParchutingIn).
 *
 * The flight used to round its step to whole world units on each axis. That
 * left a handful of headings to fly on and a speed that went with the
 * heading: 2.83 to 3.61 world units a tick at the classic 3. The rule was a
 * whole number as well. Now the man keeps the part of a unit a step leaves
 * over, so he flies the rule's own speed on every heading, and the rule is a
 * float.
 *
 *   lgm_parachute_speed_same_on_every_heading — at the classic 3, every
 *     step on every one of 64 headings is 3 world units long, the whole
 *     units the engine reads follow the exact place, and flights of the
 *     same length take the same number of ticks.
 *
 *   lgm_parachute_speed_takes_a_fraction — the rule's range takes 1.92 and
 *     0.25, and 100 ticks at each cover 192 and 25 world units.
 *
 *   lgm_parachute_wide_step_lands — a step far wider than
 *     lgm_arrive_tolerance (90 against 16) still lands on every heading, and
 *     in no more ticks than the distance takes. It used to step over the
 *     arrival window and circle.
 *
 *   lgm_parachute_moved_in_flight — a man put somewhere else in the middle
 *     of a flight by something that writes only his whole units flies on
 *     from there and not from the place he had before.
 *
 *   lgm_parachute_put_back_starts_exact — a scenario's builder_parachute
 *     that puts a flying man back on the whole units he is already at
 *     starts him from exactly there: the part of a unit his flight had got
 *     to is not carried into the next step.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "server_sim_scenario.h"   /* serverSimApplyScenarioOp */
#include "game_sim.h"              /* GameSim: lgmen[] */
#include "lgm.h"                   /* lgmParchutingIn, struct lgmObj */
#include "sim_rules.h"             /* simRulesValidate */
#include "test_harness.h"

#define LPS_SLOT     0
#define LPS_HEADINGS 64
#define LPS_PI       3.14159265358979323846

/* Where the flights land: the middle of a square in the middle of the map,
 * so a start 2,000 world units out on any heading is well inside it. */
#define LPS_DEST_X ((WORLD) ((128 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE))
#define LPS_DEST_Y ((WORLD) ((128 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE))
#define LPS_RADIUS 2000.0

/* Put the man in the air at (sx,sy) flying to (dx,dy). Only the whole
 * units are written, as by code that knows nothing of the exact place the
 * flight keeps. */
static void lpsStart(GameSim *gs, WORLD sx, WORLD sy, WORLD dx, WORLD dy) {
    lgm *l = &gs->lgmen[LPS_SLOT];

    (*l)->x = sx;
    (*l)->y = sy;
    (*l)->destX = dx;
    (*l)->destY = dy;
    (*l)->inTank = FALSE;
    (*l)->isDead = TRUE;
    (*l)->frame = LGM_HELICOPTER_FRAME;
}

/* The start of the flight on heading i of LPS_HEADINGS, `radius` world
 * units from where it lands. */
static void lpsHeadingStart(int i, double radius, WORLD *sx, WORLD *sy) {
    double a = 2.0 * LPS_PI * (double) i / (double) LPS_HEADINGS;

    *sx = (WORLD) ((double) LPS_DEST_X + radius * cos(a) + 0.5);
    *sy = (WORLD) ((double) LPS_DEST_Y + radius * sin(a) + 0.5);
}

static double lpsDist(double ax, double ay, double bx, double by) {
    return sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

int run_lgm_parachute_speed_same_on_every_heading(void) {
    ServerSim *sim = ut_make_running_sim("Parachute");
    GameSim   *gs;
    lgm       *l;
    double     speed;
    double     tolerance;
    int        i;
    int        rc = 0;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    l = &gs->lgmen[LPS_SLOT];
    UT_ASSERT(*l != NULL);
    speed = (double) gs->rules.lgm_helicopter_speed;
    tolerance = (double) gs->rules.lgm_arrive_tolerance;
    UT_CHECK_MSG(speed == 3.0, "the classic parachute speed is %g, not 3",
                 speed);

    for (i = 0; i < LPS_HEADINGS; i++) {
        WORLD  sx, sy;
        double whole;      /* the flight's length */
        double lo, hi;     /* the ticks it may take */
        int    ticks = 0;

        lpsHeadingStart(i, LPS_RADIUS, &sx, &sy);
        lpsStart(gs, sx, sy, LPS_DEST_X, LPS_DEST_Y);
        whole = lpsDist(sx, sy, LPS_DEST_X, LPS_DEST_Y);

        while ((*l)->isDead == TRUE && ticks < 2000) {
            double px = (ticks == 0) ? (double) sx : (*l)->flyX;
            double py = (ticks == 0) ? (double) sy : (*l)->flyY;
            double step;

            lgmParchutingIn(gs, l);
            ticks++;
            step = lpsDist(px, py, (*l)->flyX, (*l)->flyY);
            UT_CHECK_MSG(fabs(step - speed) < 1e-6,
                         "heading %d, tick %d: a step of %.6f world units, "
                         "wanted %g", i, ticks, step, speed);
            /* What the rest of the engine reads is the exact place with the
               part of a unit dropped. */
            UT_CHECK_MSG((*l)->x == (WORLD) (*l)->flyX &&
                             (*l)->y == (WORLD) (*l)->flyY,
                         "heading %d, tick %d: at (%u,%u), exact place "
                         "(%.3f,%.3f)", i, ticks, (unsigned) (*l)->x,
                         (unsigned) (*l)->y, (*l)->flyX, (*l)->flyY);
        }
        UT_CHECK_MSG((*l)->isDead == FALSE,
                     "heading %d: not landed after %d ticks", i, ticks);

        /* He lands inside a box of the tolerance either way on both axes, so
           between the tolerance and the tolerance times root two short of
           the place; the whole units put a unit and a half of slack on
           that. Nothing else about the heading changes the time. At 3.61 a
           tick, which the rounded step flew on some headings, 2,000 units
           took about 548 ticks, and at 2.83 about 700. */
        lo = (whole - tolerance * sqrt(2.0) - 1.5) / speed - 1.0;
        hi = (whole - tolerance + 1.5) / speed + 1.0;
        UT_CHECK_MSG((double) ticks >= lo && (double) ticks <= hi,
                     "heading %d: %.1f world units took %d ticks, wanted "
                     "%.1f to %.1f", i, whole, ticks, lo, hi);
    }

done:
    serverSimDestroy(sim);
    return rc;
}

int run_lgm_parachute_speed_takes_a_fraction(void) {
    static const float kSpeeds[] = { 1.92f, 0.25f };
    ServerSim *sim = ut_make_running_sim("Parachute");
    GameSim   *gs;
    lgm       *l;
    int        s;
    int        rc = 0;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    l = &gs->lgmen[LPS_SLOT];
    UT_ASSERT(*l != NULL);

    for (s = 0; s < (int) (sizeof(kSpeeds) / sizeof(kSpeeds[0])); s++) {
        char why[SIM_RULES_WHY_LEN];
        int  i;

        gs->rules.lgm_helicopter_speed = kSpeeds[s];
        why[0] = '\0';
        UT_CHECK_MSG(simRulesValidate(&gs->rules, why, sizeof(why)),
                     "a parachute speed of %g was refused: %s",
                     (double) kSpeeds[s], why);

        for (i = 0; i < LPS_HEADINGS; i++) {
            WORLD  sx, sy;
            double want = 100.0 * (double) kSpeeds[s];
            double got;
            int    t;

            lpsHeadingStart(i, LPS_RADIUS, &sx, &sy);
            lpsStart(gs, sx, sy, LPS_DEST_X, LPS_DEST_Y);
            for (t = 0; t < 100; t++) {
                lgmParchutingIn(gs, l);
            }
            UT_CHECK_MSG((*l)->isDead == TRUE,
                         "speed %g, heading %d: landed inside 100 ticks",
                         (double) kSpeeds[s], i);
            /* In whole units, so a unit and a half of slack. */
            got = lpsDist(sx, sy, (*l)->x, (*l)->y);
            UT_CHECK_MSG(fabs(got - want) <= 1.5,
                         "speed %g, heading %d: 100 ticks covered %.2f "
                         "world units, wanted %.2f", (double) kSpeeds[s], i,
                         got, want);
        }
    }

done:
    serverSimDestroy(sim);
    return rc;
}

int run_lgm_parachute_wide_step_lands(void) {
    ServerSim *sim = ut_make_running_sim("Parachute");
    GameSim   *gs;
    lgm       *l;
    int        i;
    int        rc = 0;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    l = &gs->lgmen[LPS_SLOT];
    UT_ASSERT(*l != NULL);

    /* A step of 90 against an arrival window 16 either way. */
    gs->rules.lgm_helicopter_speed = 90.0f;
    UT_CHECK(gs->rules.lgm_arrive_tolerance == 16);

    for (i = 0; i < LPS_HEADINGS; i++) {
        /* A different length on each heading, so the last step falls
           everywhere from short of the window to well past it. */
        double radius = LPS_RADIUS + 7.0 * (double) i;
        WORLD  sx, sy;
        double whole;
        int    most;
        int    ticks = 0;
        long   offX, offY;

        lpsHeadingStart(i, radius, &sx, &sy);
        lpsStart(gs, sx, sy, LPS_DEST_X, LPS_DEST_Y);
        whole = lpsDist(sx, sy, LPS_DEST_X, LPS_DEST_Y);
        most = (int) ceil(whole / 90.0);

        while ((*l)->isDead == TRUE && ticks < most + 50) {
            lgmParchutingIn(gs, l);
            ticks++;
        }
        UT_CHECK_MSG((*l)->isDead == FALSE,
                     "heading %d: %.1f world units at 90 a tick, not landed "
                     "after %d ticks", i, whole, ticks);
        UT_CHECK_MSG(ticks <= most,
                     "heading %d: %.1f world units at 90 a tick took %d "
                     "ticks, wanted %d at most", i, whole, ticks, most);
        offX = (long) (*l)->x - (long) LPS_DEST_X;
        offY = (long) (*l)->y - (long) LPS_DEST_Y;
        UT_CHECK_MSG(labs(offX) <= 16 && labs(offY) <= 16,
                     "heading %d: landed (%ld,%ld) from the place, outside "
                     "the tolerance of 16", i, offX, offY);
    }

done:
    serverSimDestroy(sim);
    return rc;
}

int run_lgm_parachute_moved_in_flight(void) {
    ServerSim *sim = ut_make_running_sim("Parachute");
    GameSim   *gs;
    lgm       *l;
    WORLD      sx, sy;
    WORLD      movedX, movedY;
    double     got;
    int        t;
    int        rc = 0;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    l = &gs->lgmen[LPS_SLOT];
    UT_ASSERT(*l != NULL);

    /* A speed that leaves part of a unit over on every step. */
    gs->rules.lgm_helicopter_speed = 1.92f;
    lpsHeadingStart(5, LPS_RADIUS, &sx, &sy);
    lpsStart(gs, sx, sy, LPS_DEST_X, LPS_DEST_Y);
    for (t = 0; t < 10; t++) {
        lgmParchutingIn(gs, l);
    }
    UT_CHECK((*l)->isDead == TRUE);

    /* Moved to the other side of the place he is flying to. */
    lpsHeadingStart(5 + LPS_HEADINGS / 2, LPS_RADIUS, &movedX, &movedY);
    lpsStart(gs, movedX, movedY, LPS_DEST_X, LPS_DEST_Y);
    lgmParchutingIn(gs, l);
    got = lpsDist(movedX, movedY, (*l)->x, (*l)->y);
    UT_CHECK_MSG(got <= 1.92 + 1.5,
                 "one tick after he was moved to (%u,%u) he is at (%u,%u), "
                 "%.1f world units away", (unsigned) movedX,
                 (unsigned) movedY, (unsigned) (*l)->x, (unsigned) (*l)->y,
                 got);
    /* And nearer the place than where he was put. */
    UT_CHECK_MSG(lpsDist((*l)->flyX, (*l)->flyY, LPS_DEST_X, LPS_DEST_Y) <
                     lpsDist(movedX, movedY, LPS_DEST_X, LPS_DEST_Y),
                 "the tick after the move took him away from the place he "
                 "is flying to");

done:
    serverSimDestroy(sim);
    return rc;
}

int run_lgm_parachute_put_back_starts_exact(void) {
    ServerSim *sim = ut_make_running_sim("Parachute");
    GameSim   *gs;
    lgm       *l;
    ScenarioOp op;
    WORLD      sx, sy;
    int        t;
    int        rc = 0;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    l = &gs->lgmen[LPS_SLOT];
    UT_ASSERT(*l != NULL);

    /* A quarter of a unit a tick, so three ticks leave him on the whole
       units he started on with three quarters of a unit flown. */
    gs->rules.lgm_helicopter_speed = 0.25f;
    lpsStart(gs, LPS_DEST_X, LPS_DEST_Y, LPS_DEST_X, LPS_DEST_Y);

    /* Aimed at a square ten in from the left edge on row 128: the flight
       starts in the middle of the edge square on that row and goes due
       east. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LGM_PARACHUTE;
    op.u.lgmParachute.slot = LPS_SLOT;
    op.u.lgmParachute.x = (BYTE) (MAP_MINE_EDGE_LEFT + 10);
    op.u.lgmParachute.y = 128;
    UT_CHECK(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    sx = (*l)->x;
    sy = (*l)->y;
    UT_CHECK_MSG((*l)->destY == sy && (*l)->destX > sx,
                 "the flight from (%u,%u) to (%u,%u) is not due east",
                 (unsigned) sx, (unsigned) sy, (unsigned) (*l)->destX,
                 (unsigned) (*l)->destY);

    for (t = 0; t < 3; t++) {
        lgmParchutingIn(gs, l);
    }
    UT_CHECK_MSG((*l)->x == sx && (*l)->y == sy,
                 "three ticks at 0.25 moved him off (%u,%u) to (%u,%u)",
                 (unsigned) sx, (unsigned) sy, (unsigned) (*l)->x,
                 (unsigned) (*l)->y);
    UT_CHECK_MSG(fabs((*l)->flyX - ((double) sx + 0.75)) < 1e-9,
                 "three ticks at 0.25 flew %.6f world units, wanted 0.75",
                 (*l)->flyX - (double) sx);

    /* Put back: the same start, and the whole units he is already on. */
    UT_CHECK(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_CHECK((*l)->x == sx && (*l)->y == sy);
    lgmParchutingIn(gs, l);
    UT_CHECK_MSG(fabs((*l)->flyX - ((double) sx + 0.25)) < 1e-9,
                 "the tick after he was put back left him %.6f world units "
                 "from the start, wanted 0.25", (*l)->flyX - (double) sx);
    UT_CHECK_MSG((*l)->x == sx,
                 "the tick after he was put back moved him a whole unit, to "
                 "%u from %u", (unsigned) (*l)->x, (unsigned) sx);

done:
    serverSimDestroy(sim);
    return rc;
}
