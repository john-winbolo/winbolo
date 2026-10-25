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
 *                made-up times. Also the search for how much
 *                of a newswire line fits, given a measure.
 *********************************************************/

#ifndef NEWSWIRE_SLIDE_H
#define NEWSWIRE_SLIDE_H

#include <stdbool.h>
#include <stddef.h>
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

/* Returns the byte offset of the start of the longest suffix of text (len
   bytes) that fits, where fits(text + off, len - off, user) says whether a
   suffix fits. The offset is always on a UTF-8 character start. Returns len
   when nothing fits and 0 when all of it fits.

   A binary search over character starts: a byte offset is moved forward past
   continuation bytes to the next character start, and since a longer suffix
   is never narrower, fits is true for every offset past the answer and false
   for every offset before it. It calls fits about log2(len) times and never
   on the empty suffix.

   Defined here rather than in newswire_slide.c because sdl3draw_status.c
   calls it and the log viewer links that file but not newswire_slide.c. */
static inline size_t newswireSuffixStart(const char *text, size_t len,
                                         bool (*fits)(const char *s, size_t n,
                                                      void *user),
                                         void *user) {
    size_t lo = 0;
    size_t hi = len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t start = mid;
        while (start < len && ((unsigned char)text[start] & 0xC0) == 0x80) {
            start++;
        }
        if (start == len || fits(text + start, len - start, user)) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    while (lo < len && ((unsigned char)text[lo] & 0xC0) == 0x80) lo++;
    return lo;
}

#ifdef __cplusplus
}
#endif

#endif /* NEWSWIRE_SLIDE_H */
