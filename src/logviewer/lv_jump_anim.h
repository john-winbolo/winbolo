/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_jump_anim.h - the short animation of Rewind and Fast Forward.
 *
 * Each of the two buttons takes playback to a snapshot of the log. With the
 * animation the map does not jump there: for LV_JUMP_ANIM_MS it shows a small
 * number of moments between the time playback is at and the target, in
 * order, and then the target. A step forward is a seek forward, which only
 * decodes the ticks between; a step back is a seek back, which restores the
 * last snapshot before the moment and decodes on to it.
 *
 * This file is the schedule: how many moments, the log time of each, and
 * which one is due at a time into the animation. Header only, with no
 * includes but the C library's, so the viewer and the unit tests use the
 * same sums.
 */

#ifndef LV_JUMP_ANIM_H
#define LV_JUMP_ANIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long the animation is, whatever the length of the jump. */
#define LV_JUMP_ANIM_MS 250

/* The most moments it shows; the last of them is the target. */
#define LV_JUMP_ANIM_STEPS 12

/* The log time of one tick of the decoder. */
#define LV_JUMP_ANIM_TICK_MS 20

/* A step that takes the decoder longer than this ends the animation: the
   rest of the jump is done at once, as with no animation. Half a frame at
   60 frames a second. */
#define LV_JUMP_ANIM_STEP_MAX_MS 8

static inline uint32_t lvJumpAnimClamp(uint32_t ms, uint32_t loMs, uint32_t hiMs) {
    if (hiMs < loMs) hiMs = loMs;
    if (ms < loMs) return loMs;
    if (ms > hiMs) return hiMs;
    return ms;
}

/* How many steps a jump from fromMs to toMs has, the target counted. One
   for each tick when the jump is fewer ticks than maxSteps, so no two steps
   are the same moment. 1 is the jump alone, with nothing to show before it:
   a jump of under two ticks.

   loMs and hiMs are the start and the end of what the log shows. Both ends
   of the jump are held inside them, as lvJumpAnimStepMs holds them, so the
   count is of the ticks the steps really cross. */
static inline int lvJumpAnimSteps(uint32_t fromMs, uint32_t toMs, int maxSteps,
                                  uint32_t loMs, uint32_t hiMs) {
    uint32_t span, ticks;
    fromMs = lvJumpAnimClamp(fromMs, loMs, hiMs);
    toMs = lvJumpAnimClamp(toMs, loMs, hiMs);
    span = (toMs > fromMs) ? (toMs - fromMs) : (fromMs - toMs);
    ticks = span / LV_JUMP_ANIM_TICK_MS;
    if (maxSteps < 1) maxSteps = 1;
    if (ticks < 2) return 1;
    if (ticks < (uint32_t)maxSteps) return (int)ticks;
    return maxSteps;
}

/* The log time of step k (1..steps) of a jump from fromMs to toMs, forward
   or back. The steps are the same length of log time apart, in whole ticks
   from fromMs. Step `steps` and any after it is toMs itself. A step before
   it is never the target's own tick and never past it: it is at least one
   tick short of toMs, so the last move is always the jump the button makes.
   Step 0 and any before it is fromMs.

   loMs and hiMs are the start and the end of what the log shows; both ends
   of the jump are held inside them. */
static inline uint32_t lvJumpAnimStepMs(uint32_t fromMs, uint32_t toMs, int k, int steps,
                                        uint32_t loMs, uint32_t hiMs) {
    uint32_t span, off;
    fromMs = lvJumpAnimClamp(fromMs, loMs, hiMs);
    toMs = lvJumpAnimClamp(toMs, loMs, hiMs);
    if (steps < 1) steps = 1;
    if (k >= steps) return toMs;
    if (k <= 0) return fromMs;
    span = (toMs > fromMs) ? (toMs - fromMs) : (fromMs - toMs);
    if (span < 2 * LV_JUMP_ANIM_TICK_MS) return fromMs;
    off = (uint32_t)(((uint64_t)span * (uint64_t)k) / (uint64_t)steps);
    off -= off % LV_JUMP_ANIM_TICK_MS;
    if (off > span - LV_JUMP_ANIM_TICK_MS) off = span - LV_JUMP_ANIM_TICK_MS;
    return (toMs > fromMs) ? (fromMs + off) : (fromMs - off);
}

/* The step that is due elapsedMs into an animation durMs long: 0 (nothing
   yet) to steps (the target). The steps come fast at first and slow near the
   target (1 - (1 - u)^2). The target is due only at the end of the time, so
   the animation is as long for a short jump as for a long one. */
static inline int lvJumpAnimStepAt(uint32_t elapsedMs, uint32_t durMs, int steps) {
    float u, e;
    int k;
    if (steps < 1) steps = 1;
    if (durMs == 0 || elapsedMs >= durMs) return steps;
    u = (float)elapsedMs / (float)durMs;
    e = 1.0f - (1.0f - u) * (1.0f - u);
    k = (int)(e * (float)steps);
    if (k < 0) k = 0;
    if (k > steps - 1) k = steps - 1;
    return k;
}

#ifdef __cplusplus
}
#endif

#endif /* LV_JUMP_ANIM_H */
