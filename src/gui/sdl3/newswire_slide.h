/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          newswire_slide.h
 * Purpose:       When the newswire is on screen and when it
 *                has slid off the bottom edge. One body for
 *                every newswire that hides itself: the
 *                desktop's full screen map and the touch
 *                layout. Arithmetic only, with no SDL and no
 *                clock of its own, so a test can drive it with
 *                made-up times.
 *********************************************************/

#ifndef NEWSWIRE_SLIDE_H
#define NEWSWIRE_SLIDE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long the newswire stays up after the last new message, and how long it
   takes to slide all the way on or off. */
#define NEWSWIRE_HOLD_MS  30000
#define NEWSWIRE_SLIDE_MS 300

/* Where the slide is: off is 0 fully on screen and 1 fully off the bottom
   edge; tick is the time of the last step, 0 before the first. A newswire
   starts off screen and comes up with the first message. */
typedef struct NewswireSlide {
    float    off;
    uint64_t tick;
} NewswireSlide;

#define NEWSWIRE_SLIDE_INIT { 1.0f, 0 }

/* Move the slide on to now and return where it is, 0 to 1. The newswire is
   wanted on screen while the last message (lastMessageTick, 0 for none yet)
   is younger than the hold time, or always with autoHide off; it moves
   towards that at the slide rate. The first step lands where it is wanted
   rather than sliding there, and so does a step after a long gap with no
   steps. The step is by the time since the last one, so calling it twice in
   one frame moves it once. Times are in milliseconds on any one clock. */
float newswireSlideStep(NewswireSlide *slide, uint64_t now,
                        uint64_t lastMessageTick, bool autoHide);

#ifdef __cplusplus
}
#endif

#endif /* NEWSWIRE_SLIDE_H */
