/*
 * Tests for the newswire's slide on and off the bottom edge
 * (newswire_slide.c), which the desktop's full screen map and the touch
 * layout both draw from. Times are made up; the module has no clock.
 */

#include "test_harness.h"

#include "newswire_slide.h"

int run_newswire_slide(void) {
    NewswireSlide s = NEWSWIRE_SLIDE_INIT;

    /* Off screen to start with, and it stays off with no message yet. */
    UT_ASSERT(s.off == 1.0f);
    UT_ASSERT(newswireSlideStep(&s, 1000, 0, true) == 1.0f);

    /* The first message: it slides up at the slide rate, half way in half the
       slide time, all the way in the whole of it. */
    float off = newswireSlideStep(&s, 1000 + NEWSWIRE_SLIDE_MS / 2, 1000, true);
    UT_ASSERT_MSG(off > 0.45f && off < 0.55f, "half way up, got %f", off);
    off = newswireSlideStep(&s, 1000 + NEWSWIRE_SLIDE_MS, 1000, true);
    UT_ASSERT_MSG(off == 0.0f, "fully up, got %f", off);

    /* A second call at the same time moves it nowhere. */
    UT_ASSERT(newswireSlideStep(&s, 1000 + NEWSWIRE_SLIDE_MS, 1000, true) == 0.0f);

    /* It stays up for the hold time after the message. */
    uint64_t t = 1000 + NEWSWIRE_HOLD_MS - 1;
    UT_ASSERT(newswireSlideStep(&s, t, 1000, true) == 0.0f);

    /* Then slides off: it has only just started at the hold time, is half way
       down half a slide time later and all the way a whole one later. */
    t = 1000 + NEWSWIRE_HOLD_MS;
    off = newswireSlideStep(&s, t, 1000, true);
    UT_ASSERT_MSG(off > 0.0f && off < 0.01f, "just leaving, got %f", off);
    off = newswireSlideStep(&s, t + NEWSWIRE_SLIDE_MS / 2, 1000, true);
    UT_ASSERT_MSG(off > 0.45f && off < 0.55f, "half way down, got %f", off);
    off = newswireSlideStep(&s, t + NEWSWIRE_SLIDE_MS, 1000, true);
    UT_ASSERT_MSG(off == 1.0f, "fully down, got %f", off);

    /* Auto-hide off keeps it up with no message for a long time, and with no
       message at all. */
    t += 60000;
    newswireSlideStep(&s, t, 1000, false);
    off = newswireSlideStep(&s, t + NEWSWIRE_SLIDE_MS, 1000, false);
    UT_ASSERT_MSG(off == 0.0f, "auto-hide off keeps it up, got %f", off);
    NewswireSlide none = NEWSWIRE_SLIDE_INIT;
    UT_ASSERT(newswireSlideStep(&none, 5000, 0, false) == 0.0f);

    /* After a long gap with no steps it lands where it is wanted at once,
       rather than sliding from where it was left. */
    NewswireSlide gap = NEWSWIRE_SLIDE_INIT;
    UT_ASSERT(newswireSlideStep(&gap, 100, 50, true) == 0.0f);
    UT_ASSERT(newswireSlideStep(&gap, 100 + 120000, 50, true) == 1.0f);

    /* A clock that went backwards, or a message stamped ahead of now, never
       moves it past either end. */
    NewswireSlide back = { 0.5f, 10000 };
    off = newswireSlideStep(&back, 9000, 20000, true);
    UT_ASSERT_MSG(off >= 0.0f && off <= 1.0f, "stays in range, got %f", off);

    /* No slide to step: reported off screen. */
    UT_ASSERT(newswireSlideStep(0, 1000, 1000, true) == 1.0f);

    return 0;
}
