/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * test_lv_jump_anim.c - the schedule of the animation of the log viewer's
 * Rewind and Fast Forward (lv_jump_anim.h): how many steps a jump has, the
 * log time of each, and which is due at a time into the animation.
 */

#include <stdint.h>

#include "lv_jump_anim.h"
#include "test_harness.h"

/* Every step of a jump: on a whole tick from the start, in order, between
   the two ends, and never the target's own tick before the last. */
static int ja_check(uint32_t from, uint32_t to, int maxSteps) {
    int steps = lvJumpAnimSteps(from, to, maxSteps, 0, UINT32_MAX);
    uint32_t prev = from;
    int k;

    UT_ASSERT_MSG(steps >= 1 && steps <= maxSteps, "%u to %u: %d steps", (unsigned)from,
                  (unsigned)to, steps);
    for (k = 1; k < steps; k++) {
        uint32_t t = lvJumpAnimStepMs(from, to, k, steps, 0, UINT32_MAX);
        uint32_t off = (to > from) ? (t - from) : (from - t);
        UT_ASSERT_MSG(off % LV_JUMP_ANIM_TICK_MS == 0, "%u to %u step %d: %u is not a whole tick on",
                      (unsigned)from, (unsigned)to, k, (unsigned)t);
        if (to > from) {
            UT_ASSERT_MSG(t > prev && t + LV_JUMP_ANIM_TICK_MS <= to,
                          "%u to %u step %d: %u after %u", (unsigned)from, (unsigned)to, k,
                          (unsigned)t, (unsigned)prev);
        } else {
            UT_ASSERT_MSG(t < prev && t >= to + LV_JUMP_ANIM_TICK_MS,
                          "%u to %u step %d: %u after %u", (unsigned)from, (unsigned)to, k,
                          (unsigned)t, (unsigned)prev);
        }
        prev = t;
    }
    UT_ASSERT(lvJumpAnimStepMs(from, to, steps, steps, 0, UINT32_MAX) == to);
    UT_ASSERT(lvJumpAnimStepMs(from, to, steps + 5, steps, 0, UINT32_MAX) == to);
    UT_ASSERT(lvJumpAnimStepMs(from, to, 0, steps, 0, UINT32_MAX) == from);
    UT_ASSERT(lvJumpAnimStepMs(from, to, -2, steps, 0, UINT32_MAX) == from);
    return 0;
}

int run_lv_jump_anim_steps(void) {
    static const uint32_t spans[] = { 40, 60, 100, 220, 240, 260, 1000, 2540, 2560, 60000, 1800000 };
    size_t i;

    /* How many: 12 at most, one for each tick of a short jump, and the jump
       alone when there is no tick between its ends. */
    UT_ASSERT(lvJumpAnimSteps(1000, 3540, 12, 0, UINT32_MAX) == 12);
    UT_ASSERT(lvJumpAnimSteps(3540, 1000, 12, 0, UINT32_MAX) == 12);
    UT_ASSERT(lvJumpAnimSteps(1000, 1240, 12, 0, UINT32_MAX) == 12);
    UT_ASSERT(lvJumpAnimSteps(1000, 1220, 12, 0, UINT32_MAX) == 11);
    UT_ASSERT(lvJumpAnimSteps(1000, 1040, 12, 0, UINT32_MAX) == 2);
    UT_ASSERT(lvJumpAnimSteps(1000, 1020, 12, 0, UINT32_MAX) == 1);
    UT_ASSERT(lvJumpAnimSteps(1000, 1000, 12, 0, UINT32_MAX) == 1);
    UT_ASSERT(lvJumpAnimSteps(1020, 1000, 12, 0, UINT32_MAX) == 1);
    UT_ASSERT(lvJumpAnimSteps(1000, 3540, 0, 0, UINT32_MAX) == 1);
    UT_ASSERT(lvJumpAnimSteps(0, UINT32_MAX, 12, 0, UINT32_MAX) == 12);
    /* Both ends held inside 1000..5000, as the steps are: 4000 to 5000 is
       50 ticks, not the 250 of 4000 to 9000; 4980 to 5000 is one tick. */
    UT_ASSERT(lvJumpAnimSteps(4000, 9000, 100, 1000, 5000) == 50);
    UT_ASSERT(lvJumpAnimSteps(4980, 9000, 12, 1000, 5000) == 1);
    UT_ASSERT(lvJumpAnimSteps(500, 0, 12, 1000, 5000) == 1);

    /* By hand: forward from 1000 to 3400 in 12 steps is 200 ms a step. */
    UT_ASSERT(lvJumpAnimStepMs(1000, 3400, 1, 12, 0, UINT32_MAX) == 1200);
    UT_ASSERT(lvJumpAnimStepMs(1000, 3400, 6, 12, 0, UINT32_MAX) == 2200);
    UT_ASSERT(lvJumpAnimStepMs(1000, 3400, 11, 12, 0, UINT32_MAX) == 3200);
    UT_ASSERT(lvJumpAnimStepMs(1000, 3400, 12, 12, 0, UINT32_MAX) == 3400);
    /* The same back. */
    UT_ASSERT(lvJumpAnimStepMs(3400, 1000, 1, 12, 0, UINT32_MAX) == 3200);
    UT_ASSERT(lvJumpAnimStepMs(3400, 1000, 6, 12, 0, UINT32_MAX) == 2200);
    UT_ASSERT(lvJumpAnimStepMs(3400, 1000, 11, 12, 0, UINT32_MAX) == 1200);
    UT_ASSERT(lvJumpAnimStepMs(3400, 1000, 12, 12, 0, UINT32_MAX) == 1000);
    /* 2540 ms (the time between two snapshots of a server's log) is 211.67
       ms a step: each step is cut to a whole tick from the start. */
    UT_ASSERT(lvJumpAnimStepMs(0, 2540, 1, 12, 0, UINT32_MAX) == 200);
    UT_ASSERT(lvJumpAnimStepMs(0, 2540, 2, 12, 0, UINT32_MAX) == 420);
    UT_ASSERT(lvJumpAnimStepMs(0, 2540, 11, 12, 0, UINT32_MAX) == 2320);
    /* The start does not have to be a whole tick of the log's clock. */
    UT_ASSERT(lvJumpAnimStepMs(1007, 3407, 1, 12, 0, UINT32_MAX) == 1207);
    /* A jump of three ticks has three steps, a tick each. */
    UT_ASSERT(lvJumpAnimStepMs(1000, 1060, 1, 3, 0, UINT32_MAX) == 1020);
    UT_ASSERT(lvJumpAnimStepMs(1000, 1060, 2, 3, 0, UINT32_MAX) == 1040);
    UT_ASSERT(lvJumpAnimStepMs(1060, 1000, 2, 3, 0, UINT32_MAX) == 1020);

    for (i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        if (ja_check(5000, 5000 + spans[i], 12) != 0) return 1;
        if (ja_check(2000000, 2000000 - spans[i], 12) != 0) return 1;
        if (ja_check(7, 7 + spans[i], 12) != 0) return 1;
        if (ja_check(spans[i], 0, 12) != 0) return 1;
        if (ja_check(100, 100 + spans[i], 5) != 0) return 1;
    }

    /* The ends of the log hold both ends of the jump. A target past the end
       is the end; one before the start is the start. */
    UT_ASSERT(lvJumpAnimStepMs(4000, 9000, 12, 12, 1000, 5000) == 5000);
    UT_ASSERT(lvJumpAnimStepMs(4000, 9000, 6, 12, 1000, 5000) == 4500);
    UT_ASSERT(lvJumpAnimStepMs(4000, 0, 12, 12, 1000, 5000) == 1000);
    UT_ASSERT(lvJumpAnimStepMs(4000, 0, 6, 12, 1000, 5000) == 2500);
    UT_ASSERT(lvJumpAnimStepMs(4000, 0, 11, 12, 1000, 5000) == 1260);
    /* A start outside them too. */
    UT_ASSERT(lvJumpAnimStepMs(500, 3400, 0, 12, 1000, 5000) == 1000);
    UT_ASSERT(lvJumpAnimStepMs(500, 3400, 1, 12, 1000, 5000) == 1200);
    /* Ends that are the wrong way round are one place. */
    UT_ASSERT(lvJumpAnimStepMs(4000, 0, 6, 12, 3000, 2000) == 3000);
    return 0;
}

int run_lv_jump_anim_time(void) {
    int prev = 0, k;
    uint32_t ms;

    /* Nothing is due at the start, and the target only at the end. */
    UT_ASSERT(lvJumpAnimStepAt(0, LV_JUMP_ANIM_MS, 12) == 0);
    UT_ASSERT(lvJumpAnimStepAt(LV_JUMP_ANIM_MS - 1, LV_JUMP_ANIM_MS, 12) == 11);
    UT_ASSERT(lvJumpAnimStepAt(LV_JUMP_ANIM_MS, LV_JUMP_ANIM_MS, 12) == 12);
    UT_ASSERT(lvJumpAnimStepAt(LV_JUMP_ANIM_MS + 500, LV_JUMP_ANIM_MS, 12) == 12);
    /* The steps never go back, and none after the first is passed over at
       one millisecond a look. */
    for (ms = 0; ms <= LV_JUMP_ANIM_MS; ms++) {
        k = lvJumpAnimStepAt(ms, LV_JUMP_ANIM_MS, 12);
        UT_ASSERT_MSG(k >= prev && k <= prev + 1, "at %u ms step %d after %d", (unsigned)ms, k, prev);
        prev = k;
    }
    UT_ASSERT(prev == 12);
    /* Fast first, slow near the target: half of the steps are done in under
       a third of the time (1 - (1 - u)^2 is a half at u = 0.293), and at
       half of the time three quarters are done. */
    UT_ASSERT(lvJumpAnimStepAt(74, 250, 12) == 6);
    UT_ASSERT(lvJumpAnimStepAt(72, 250, 12) == 5);
    UT_ASSERT(lvJumpAnimStepAt(125, 250, 12) == 9);
    /* A short jump takes the same time: its two steps are the moment
       between at once... */
    UT_ASSERT(lvJumpAnimStepAt(0, 250, 2) == 0);
    UT_ASSERT(lvJumpAnimStepAt(74, 250, 2) == 1);
    UT_ASSERT(lvJumpAnimStepAt(249, 250, 2) == 1);
    /* ...and the target at the end. */
    UT_ASSERT(lvJumpAnimStepAt(250, 250, 2) == 2);
    /* A jump alone is due at once only when there is no time to wait. */
    UT_ASSERT(lvJumpAnimStepAt(10, 250, 1) == 0);
    UT_ASSERT(lvJumpAnimStepAt(250, 250, 1) == 1);
    UT_ASSERT(lvJumpAnimStepAt(0, 0, 12) == 12);
    UT_ASSERT(lvJumpAnimStepAt(10, 250, 0) == 0);
    return 0;
}
