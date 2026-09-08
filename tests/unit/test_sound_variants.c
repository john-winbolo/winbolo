/*
 * Sound variant pools (src/gui/sdl3/sound_variants.c).
 *
 * A skin may ship <name>.wav plus <name>_0.wav through <name>_9.wav for one
 * effect, and the pool is whichever of those eleven names the source holds.
 * This test drives soundVariantPool with a fake source that answers from a
 * list of names and records every name it was asked for, which pins three
 * things the callers depend on: the order (plain name first, then _0 upward),
 * that _10 and beyond are never even asked for, and that the names handed to
 * the callback are bare — no sounds/ in front of them, because looking in two
 * places inside a skin belongs to the adapter in sound.c and not here.
 *
 * It then drives soundVariantLoad with a fake load that refuses named
 * members. That is the part worth pinning: a member that will not decode must
 * not use up its position, or the pool has a hole in it and the caller can
 * pick a member that was never filled in. The return value is the count of
 * members that decoded, and they sit at positions 0 upward with nothing
 * missing in between.
 *
 * Nothing here opens a file, decodes audio or needs a skin on disk.
 */
#include <stdio.h>
#include <string.h>

#include "sound_variants.h"
#include "test_harness.h"

#define FAKE_NAME_MAX  16   /* entries in the held / refused lists */
#define FAKE_ASKED_MAX 32   /* names one case may ask the source about */
#define FAKE_CALL_MAX  16   /* members one case may offer to load */

/* One recorded call to the fake load. */
typedef struct FakeLoadCall {
    char name[SOUND_VARIANT_NAME];
    int  slot;
    bool accepted;
} FakeLoadCall;

/* A source that holds `held` and whose load turns down `refused`, keeping
   what it was asked so the test can check the questions as well as the
   answers. `written` counts the accepted loads that landed in each pool
   position. */
typedef struct FakeSource {
    const char   *held[FAKE_NAME_MAX];
    const char   *refused[FAKE_NAME_MAX];
    char          asked[FAKE_ASKED_MAX][SOUND_VARIANT_NAME];
    int           askedCount;
    FakeLoadCall  calls[FAKE_CALL_MAX];
    int           callCount;
    int           written[SOUND_VARIANT_MAX];
} FakeSource;

static void fakeReset(FakeSource *f, const char *const *held,
                      const char *const *refused) {
    int i;

    memset(f, 0, sizeof(*f));
    for (i = 0; i < FAKE_NAME_MAX - 1 && held != NULL && held[i] != NULL; i++) {
        f->held[i] = held[i];
    }
    for (i = 0;
         i < FAKE_NAME_MAX - 1 && refused != NULL && refused[i] != NULL; i++) {
        f->refused[i] = refused[i];
    }
}

static bool fakeExists(void *ctx, const char *relName) {
    FakeSource *f = (FakeSource *)ctx;
    int i;

    if (f->askedCount < FAKE_ASKED_MAX) {
        snprintf(f->asked[f->askedCount], SOUND_VARIANT_NAME, "%s", relName);
        f->askedCount++;
    }
    for (i = 0; f->held[i] != NULL; i++) {
        if (strcmp(f->held[i], relName) == 0) return true;
    }
    return false;
}

static bool fakeLoad(void *ctx, const char *relName, int slot) {
    FakeSource *f = (FakeSource *)ctx;
    bool accepted = true;
    int i;

    for (i = 0; f->refused[i] != NULL; i++) {
        if (strcmp(f->refused[i], relName) == 0) {
            accepted = false;
            break;
        }
    }
    if (f->callCount < FAKE_CALL_MAX) {
        snprintf(f->calls[f->callCount].name, SOUND_VARIANT_NAME, "%s",
                 relName);
        f->calls[f->callCount].slot = slot;
        f->calls[f->callCount].accepted = accepted;
        f->callCount++;
    }
    if (accepted && slot >= 0 && slot < SOUND_VARIANT_MAX) f->written[slot]++;
    return accepted;
}

static bool fakeWasAsked(const FakeSource *f, const char *name) {
    int i;

    for (i = 0; i < f->askedCount; i++) {
        if (strcmp(f->asked[i], name) == 0) return true;
    }
    return false;
}

/* Builds the pool for baseName and checks it against a NULL-terminated list
   of the names expected, in the order expected. */
static int checkPool(FakeSource *f, const char *baseName,
                     const char *const *expected, const char *label) {
    char out[SOUND_VARIANT_MAX][SOUND_VARIANT_NAME];
    int want = 0;
    int got;
    int i;

    while (expected[want] != NULL) want++;

    got = soundVariantPool(baseName, fakeExists, f, out);
    UT_ASSERT_MSG(got == want, "%s: pool holds %d names, expected %d",
                  label, got, want);
    for (i = 0; i < got; i++) {
        UT_ASSERT_MSG(strcmp(out[i], expected[i]) == 0,
                      "%s: name %d is '%s', expected '%s'",
                      label, i, out[i], expected[i]);
    }
    return 0;
}

int run_sound_variant_pool_names(void) {
    FakeSource f;
    int i;

    /* The plain name on its own is a pool of one. */
    {
        static const char *const held[] = { "chop.wav", NULL };
        static const char *const want[] = { "chop.wav", NULL };

        fakeReset(&f, held, NULL);
        if (checkPool(&f, "chop.wav", want, "plain name only") != 0) return 1;
    }

    /* _0 through _9 with no plain name: ten members, ascending. */
    {
        static const char *const held[] = {
            "chop_0.wav", "chop_1.wav", "chop_2.wav", "chop_3.wav",
            "chop_4.wav", "chop_5.wav", "chop_6.wav", "chop_7.wav",
            "chop_8.wav", "chop_9.wav", NULL
        };
        static const char *const want[] = {
            "chop_0.wav", "chop_1.wav", "chop_2.wav", "chop_3.wav",
            "chop_4.wav", "chop_5.wav", "chop_6.wav", "chop_7.wav",
            "chop_8.wav", "chop_9.wav", NULL
        };

        fakeReset(&f, held, NULL);
        if (checkPool(&f, "chop.wav", want, "digits only") != 0) return 1;
    }

    /* The plain name and all ten digits: the full eleven, plain name first. */
    {
        static const char *const held[] = {
            "chop.wav",
            "chop_0.wav", "chop_1.wav", "chop_2.wav", "chop_3.wav",
            "chop_4.wav", "chop_5.wav", "chop_6.wav", "chop_7.wav",
            "chop_8.wav", "chop_9.wav", NULL
        };
        static const char *const want[] = {
            "chop.wav",
            "chop_0.wav", "chop_1.wav", "chop_2.wav", "chop_3.wav",
            "chop_4.wav", "chop_5.wav", "chop_6.wav", "chop_7.wav",
            "chop_8.wav", "chop_9.wav", NULL
        };

        fakeReset(&f, held, NULL);
        if (checkPool(&f, "chop.wav", want, "plain name and ten digits") != 0) {
            return 1;
        }
        UT_ASSERT_MSG(f.askedCount == SOUND_VARIANT_MAX,
                      "full pool asked about %d names, expected %d",
                      f.askedCount, SOUND_VARIANT_MAX);
    }

    /* Gaps are allowed and the members that exist stay next to each other. */
    {
        static const char *const held[] = {
            "chop_0.wav", "chop_1.wav", "chop_5.wav", NULL
        };
        static const char *const want[] = {
            "chop_0.wav", "chop_1.wav", "chop_5.wav", NULL
        };

        fakeReset(&f, held, NULL);
        if (checkPool(&f, "chop.wav", want, "gaps") != 0) return 1;
    }

    /* _10 is not part of the scheme: it is not a member, and the name is
       never even constructed, so a source is never asked about it. */
    {
        static const char *const held[] = {
            "chop_0.wav", "chop_1.wav", "chop_5.wav", "chop_10.wav", NULL
        };
        static const char *const want[] = {
            "chop_0.wav", "chop_1.wav", "chop_5.wav", NULL
        };

        fakeReset(&f, held, NULL);
        if (checkPool(&f, "chop.wav", want, "_10 present") != 0) return 1;
        UT_ASSERT_MSG(!fakeWasAsked(&f, "chop_10.wav"),
                      "the source was asked about chop_10.wav");
    }

    /* A base name that already ends in _<digit> is an ordinary base name:
       its own pool is that name plus that name with _0 upward. */
    {
        static const char *const held[] = {
            "chop_3.wav", "chop_3_0.wav", "chop_3_7.wav", NULL
        };
        static const char *const want[] = {
            "chop_3.wav", "chop_3_0.wav", "chop_3_7.wav", NULL
        };

        fakeReset(&f, held, NULL);
        if (checkPool(&f, "chop_3.wav", want, "base name ending in a digit")
            != 0) {
            return 1;
        }
        UT_ASSERT_MSG(fakeWasAsked(&f, "chop_3_9.wav"),
                      "chop_3.wav's pool did not reach chop_3_9.wav");
    }

    /* Every name the source is asked about is bare. Looking under sounds/
       and then at a skin's top level is the adapter's job in sound.c, and a
       prefix applied here would ask for names no source has. */
    {
        static const char *const held[] = {
            "bubbles.wav", "bubbles_0.wav", NULL
        };
        static const char *const want[] = {
            "bubbles.wav", "bubbles_0.wav", NULL
        };

        fakeReset(&f, held, NULL);
        if (checkPool(&f, "bubbles.wav", want, "bare names only") != 0) {
            return 1;
        }
        UT_ASSERT_MSG(f.askedCount == SOUND_VARIANT_MAX,
                      "bare-name pool asked about %d names, expected %d",
                      f.askedCount, SOUND_VARIANT_MAX);
        for (i = 0; i < f.askedCount; i++) {
            UT_ASSERT_MSG(strchr(f.asked[i], '/') == NULL,
                          "the source was asked about '%s', which carries a "
                          "path", f.asked[i]);
        }
    }

    return 0;
}

int run_sound_variant_load_compaction(void) {
    FakeSource f;
    int got;
    int i;

    /* Nothing refuses: every member decodes into the position it would take
       in name order. */
    {
        static const char *const held[] = {
            "chop.wav", "chop_0.wav", "chop_1.wav", NULL
        };

        fakeReset(&f, held, NULL);
        got = soundVariantLoad("chop.wav", fakeExists, fakeLoad, &f);
        UT_ASSERT_MSG(got == 3, "all decoded: returned %d, expected 3", got);
        UT_ASSERT_MSG(f.callCount == 3,
                      "all decoded: load saw %d members, expected 3",
                      f.callCount);
        UT_ASSERT(strcmp(f.calls[0].name, "chop.wav") == 0);
        UT_ASSERT(strcmp(f.calls[1].name, "chop_0.wav") == 0);
        UT_ASSERT(strcmp(f.calls[2].name, "chop_1.wav") == 0);
        for (i = 0; i < f.callCount; i++) {
            UT_ASSERT_MSG(f.calls[i].slot == i,
                          "all decoded: '%s' was offered position %d, "
                          "expected %d", f.calls[i].name, f.calls[i].slot, i);
        }
    }

    /* _0 will not decode, so it does not use up position 0: _1 takes it and
       _5 follows, leaving position 2 upward untouched. */
    {
        static const char *const held[] = {
            "chop_0.wav", "chop_1.wav", "chop_5.wav", NULL
        };
        static const char *const refused[] = { "chop_0.wav", NULL };

        fakeReset(&f, held, refused);
        got = soundVariantLoad("chop.wav", fakeExists, fakeLoad, &f);
        UT_ASSERT_MSG(got == 2, "one refused: returned %d, expected 2", got);
        UT_ASSERT_MSG(f.callCount == 3,
                      "one refused: load saw %d members, expected 3",
                      f.callCount);
        UT_ASSERT_MSG(strcmp(f.calls[0].name, "chop_0.wav") == 0 &&
                          f.calls[0].slot == 0 && !f.calls[0].accepted,
                      "one refused: '%s' was offered position %d and %s",
                      f.calls[0].name, f.calls[0].slot,
                      f.calls[0].accepted ? "took it" : "turned it down");
        UT_ASSERT_MSG(strcmp(f.calls[1].name, "chop_1.wav") == 0 &&
                          f.calls[1].slot == 0,
                      "one refused: '%s' took position %d, expected "
                      "chop_1.wav at 0", f.calls[1].name, f.calls[1].slot);
        UT_ASSERT_MSG(strcmp(f.calls[2].name, "chop_5.wav") == 0 &&
                          f.calls[2].slot == 1,
                      "one refused: '%s' took position %d, expected "
                      "chop_5.wav at 1", f.calls[2].name, f.calls[2].slot);
        UT_ASSERT_MSG(f.written[0] == 1 && f.written[1] == 1,
                      "one refused: positions 0 and 1 took %d and %d members",
                      f.written[0], f.written[1]);
        for (i = 2; i < SOUND_VARIANT_MAX; i++) {
            UT_ASSERT_MSG(f.written[i] == 0,
                          "one refused: position %d was written to", i);
        }
    }

    /* Nothing decodes: an empty pool, so the caller's guard sees a count of
       zero rather than a position that was never filled in. */
    {
        static const char *const held[] = {
            "chop.wav", "chop_0.wav", "chop_3.wav", NULL
        };
        static const char *const refused[] = {
            "chop.wav", "chop_0.wav", "chop_3.wav", NULL
        };

        fakeReset(&f, held, refused);
        got = soundVariantLoad("chop.wav", fakeExists, fakeLoad, &f);
        UT_ASSERT_MSG(got == 0, "all refused: returned %d, expected 0", got);
        UT_ASSERT_MSG(f.callCount == 3,
                      "all refused: load saw %d members, expected 3",
                      f.callCount);
        for (i = 0; i < SOUND_VARIANT_MAX; i++) {
            UT_ASSERT_MSG(f.written[i] == 0,
                          "all refused: position %d was written to", i);
        }
    }

    /* One member, and it will not decode. */
    {
        static const char *const held[] = { "chop.wav", NULL };
        static const char *const refused[] = { "chop.wav", NULL };

        fakeReset(&f, held, refused);
        got = soundVariantLoad("chop.wav", fakeExists, fakeLoad, &f);
        UT_ASSERT_MSG(got == 0,
                      "lone plain name refused: returned %d, expected 0", got);
        UT_ASSERT_MSG(f.callCount == 1,
                      "lone plain name refused: load saw %d members, "
                      "expected 1", f.callCount);
        UT_ASSERT_MSG(f.written[0] == 0,
                      "lone plain name refused: position 0 was written to");
    }

    /* No load to hand members to is an empty pool, not a crash. */
    {
        static const char *const held[] = { "chop.wav", NULL };

        fakeReset(&f, held, NULL);
        got = soundVariantLoad("chop.wav", fakeExists, NULL, &f);
        UT_ASSERT_MSG(got == 0, "no load callback: returned %d, expected 0",
                      got);
    }

    return 0;
}
