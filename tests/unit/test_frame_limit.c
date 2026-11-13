/*
 * The Frame Rate choice (#312, src/gui/frame_limit.h).
 *
 * Four things are pinned here:
 *
 *   1. Each old FRAME_RATE_* value that older builds saved moves to the
 *      new choice nearest to what it really did: the old "60", "50" and
 *      "30" (the old default) become Match display, and the old "20" and
 *      lower become the 30 limit. A number that was never a FRAME_RATE_*
 *      value, an empty value and text become the default.
 *
 *   2. A saved new value wins over an old one, and an unknown new word
 *      falls back to the old value.
 *
 *   3. Every choice survives a trip through its saved word.
 *
 *   4. Only Unlimited turns vsync off, the limits are what their names
 *      say, and the menu game's refreshes-per-frame is the nearest whole
 *      number, never less than 1.
 */

#include <string.h>

#include "frame_limit.h"
#include "test_harness.h"

int run_frame_limit_migrate_old(void) {
    /* The old values, as older builds wrote them. */
    UT_ASSERT(frameLimitLoad("", "111") == FRAME_LIMIT_VSYNC);  /* "60" */
    UT_ASSERT(frameLimitLoad("", "82")  == FRAME_LIMIT_VSYNC);  /* "50" */
    UT_ASSERT(frameLimitLoad("", "55")  == FRAME_LIMIT_VSYNC);  /* "30", default */
    UT_ASSERT(frameLimitLoad("", "37")  == FRAME_LIMIT_30);     /* "20" */
    UT_ASSERT(frameLimitLoad("", "28")  == FRAME_LIMIT_30);     /* "15" */
    UT_ASSERT(frameLimitLoad("", "23")  == FRAME_LIMIT_30);     /* "12" */
    UT_ASSERT(frameLimitLoad("", "19")  == FRAME_LIMIT_30);     /* "10" */

    /* Nothing saved, or something that was never an old value. */
    UT_ASSERT(frameLimitLoad("", "")       == FRAME_LIMIT_DEFAULT);
    UT_ASSERT(frameLimitLoad(NULL, NULL)   == FRAME_LIMIT_DEFAULT);
    UT_ASSERT(frameLimitLoad("", "0")      == FRAME_LIMIT_DEFAULT);
    UT_ASSERT(frameLimitLoad("", "60")     == FRAME_LIMIT_DEFAULT);
    UT_ASSERT(frameLimitLoad("", "-5")     == FRAME_LIMIT_DEFAULT);
    UT_ASSERT(frameLimitLoad("", "fast")   == FRAME_LIMIT_DEFAULT);
    UT_ASSERT(frameLimitLoad("", "999999999999") == FRAME_LIMIT_DEFAULT);
    return 0;
}

int run_frame_limit_new_value_wins(void) {
    UT_ASSERT(frameLimitLoad("Unlimited", "19") == FRAME_LIMIT_UNLIMITED);
    UT_ASSERT(frameLimitLoad("144", "55")       == FRAME_LIMIT_144);
    UT_ASSERT(frameLimitLoad("Match display", "37") == FRAME_LIMIT_VSYNC);
    /* An unknown word in the new key is as good as no new key. */
    UT_ASSERT(frameLimitLoad("75", "37")        == FRAME_LIMIT_30);
    UT_ASSERT(frameLimitLoad("unlimited", "")   == FRAME_LIMIT_DEFAULT);
    return 0;
}

int run_frame_limit_pref_round_trip(void) {
    for (int i = 0; i < FRAME_LIMIT_COUNT; i++) {
        const char *word = frameLimitPrefString(i);
        UT_ASSERT_MSG(word != NULL && word[0] != '\0', "limit %d has no word", i);
        UT_ASSERT_MSG(frameLimitFromPrefString(word) == i,
                      "limit %d word '%s' reads back as %d", i, word,
                      frameLimitFromPrefString(word));
        UT_ASSERT(frameLimitLoad(word, "19") == i);
    }
    UT_ASSERT(frameLimitFromPrefString(NULL) == -1);
    UT_ASSERT(frameLimitFromPrefString("") == -1);
    UT_ASSERT(!frameLimitValid(-1));
    UT_ASSERT(!frameLimitValid(FRAME_LIMIT_COUNT));
    return 0;
}

int run_frame_limit_pacing_values(void) {
    const long long hz60 = 1000000000LL / 60;
    const long long hz144 = 1000000000LL / 144;

    for (int i = 0; i < FRAME_LIMIT_COUNT; i++) {
        UT_ASSERT(frameLimitUsesVsync(i) == (i != FRAME_LIMIT_UNLIMITED));
    }
    UT_ASSERT(frameLimitCapHz(FRAME_LIMIT_VSYNC) == 0);
    UT_ASSERT(frameLimitCapHz(FRAME_LIMIT_UNLIMITED) == 0);
    UT_ASSERT(frameLimitCapHz(FRAME_LIMIT_30) == 30);
    UT_ASSERT(frameLimitCapHz(FRAME_LIMIT_60) == 60);
    UT_ASSERT(frameLimitCapHz(FRAME_LIMIT_120) == 120);
    UT_ASSERT(frameLimitCapHz(FRAME_LIMIT_144) == 144);
    UT_ASSERT(frameLimitCapHz(FRAME_LIMIT_240) == 240);

    /* The menu game: refreshes between redraws. */
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_VSYNC, hz60) == 1);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_UNLIMITED, hz60) == 1);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_30, hz60) == 2);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_60, hz60) == 1);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_240, hz60) == 1);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_30, hz144) == 5);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_60, hz144) == 2);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_120, hz144) == 1);
    UT_ASSERT(frameLimitRefreshesPerFrame(FRAME_LIMIT_30, 0) == 1);
    return 0;
}
