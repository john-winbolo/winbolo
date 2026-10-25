/*
 * Tests for the newswire's slide on and off the bottom edge
 * (newswire_slide.c), which the desktop's full screen map and the touch
 * layout both draw from. Times are made up; the module has no clock.
 */

#include "test_harness.h"

#include "newswire_slide.h"

#include <string.h>

/* A stand-in for the font measure: 1 cell per ASCII byte, 2 per character
   whose lead byte is 0xE0 or more (3- and 4-byte sequences, standing in for
   double-width CJK), 1 per 2-byte character. Fits when the cells are at most
   the limit in user. */
static bool cellsFit(const char *s, size_t n, void *user) {
    size_t limit = *(const size_t *)user;
    size_t cells = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) == 0x80) continue;
        cells += (b >= 0xE0) ? 2 : 1;
    }
    return cells <= limit;
}

static bool neverFits(const char *s, size_t n, void *user) {
    (void)s; (void)n; (void)user;
    return false;
}

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

    /* The rightmost part of a line that fits, measured as a suffix. */
    size_t limit;
    size_t r;

    /* All of it fits: start at 0. */
    const char *ascii = "abcdef";
    limit = 6;
    r = newswireSuffixStart(ascii, strlen(ascii), cellsFit, &limit);
    UT_ASSERT_MSG(r == 0, "all fits, got %zu", r);

    /* Nothing fits: start at len, with a measure that never fits and with
       one too narrow for any character. */
    r = newswireSuffixStart(ascii, strlen(ascii), neverFits, NULL);
    UT_ASSERT_MSG(r == 6, "never fits, got %zu", r);
    const char *cjk = "\xE4\xB8\xAD\xE6\x96\x87";             /* 2 CJK */
    limit = 1;
    r = newswireSuffixStart(cjk, strlen(cjk), cellsFit, &limit);
    UT_ASSERT_MSG(r == 6, "no CJK character fits in 1 cell, got %zu", r);

    /* Three CJK characters then "abcd": 13 bytes, 10 cells. In 7 cells the
       longest suffix is the third CJK character and "abcd" (6 cells), from
       byte 6. Counting 7 cells from the start gives 10 bytes, and 13 - 10 is
       byte 3, a suffix of 8 cells that overflows. */
    const char *cjkFirst = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97" "abcd";
    limit = 7;
    r = newswireSuffixStart(cjkFirst, strlen(cjkFirst), cellsFit, &limit);
    UT_ASSERT_MSG(r == 6, "CJK then ASCII, got %zu", r);
    UT_ASSERT(!cellsFit(cjkFirst + 3, strlen(cjkFirst) - 3, &limit));

    /* The opposite mix, "abcd" then three CJK characters: in 7 cells the
       suffix is "d" and the CJK, from byte 3. Counting from the start gives
       7 bytes, byte 6, moved to byte 7: two CJK, leaving 3 cells unused. */
    const char *asciiFirst = "abcd" "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97";
    r = newswireSuffixStart(asciiFirst, strlen(asciiFirst), cellsFit, &limit);
    UT_ASSERT_MSG(r == 3, "ASCII then CJK, got %zu", r);

    /* A cut that would land inside a 3-byte character moves to its start:
       "ab" then two CJK in 3 cells is the last CJK only, from byte 5, and the
       search's midpoints at bytes 3 and 4 fall inside the first CJK. */
    const char *mid = "ab" "\xE4\xB8\xAD\xE6\x96\x87";
    limit = 3;
    r = newswireSuffixStart(mid, strlen(mid), cellsFit, &limit);
    UT_ASSERT_MSG(r == 5, "cut on a character start, got %zu", r);
    UT_ASSERT(((unsigned char)mid[r] & 0xC0) != 0x80);

    /* A 4-byte character: "x", an emoji, "yz". 3 cells is "yz" from byte 5;
       4 cells takes the emoji too, from byte 1. */
    const char *emoji = "x" "\xF0\x9F\x98\x80" "yz";
    limit = 3;
    r = newswireSuffixStart(emoji, strlen(emoji), cellsFit, &limit);
    UT_ASSERT_MSG(r == 5, "4-byte character left out, got %zu", r);
    limit = 4;
    r = newswireSuffixStart(emoji, strlen(emoji), cellsFit, &limit);
    UT_ASSERT_MSG(r == 1, "4-byte character kept, got %zu", r);

    /* An empty line: 0. */
    r = newswireSuffixStart("", 0, neverFits, NULL);
    UT_ASSERT_MSG(r == 0, "empty line, got %zu", r);

    return 0;
}
