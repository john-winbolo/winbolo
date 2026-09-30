/*
 * A brain's mode manifest: brains/<brain>/modes.txt, parsed by
 * brainListLoadModes (src/bolo/brain_list.c).
 *
 * This file is the API by which a brain tells the lobby which MODES it can
 * be run in and which difficulty LEVELS each of those modes offers. It is
 * read locally on every client (brains ship with the client) before a game
 * starts, so the parse has to be forgiving in exactly the right places:
 *
 *   - a brain with no manifest gets the synthesized single "default" mode
 *     with easy / medium / hard and hard as the default, which is what the
 *     game did before manifests existed. Nothing may ever hand a caller an
 *     empty list, because the lobby indexes straight into it;
 *   - a malformed line is SKIPPED, not fatal — a typo in one level entry
 *     must not cost the player the other modes;
 *   - the counts are clamped to BRAIN_MODES_MAX / BRAIN_LEVELS_MAX, since
 *     the list is a fixed-size struct the wire indexes into.
 *
 * The manifest is written into a temp brain directory under the working
 * directory's brains/, which is the first place brainListLoadModes looks.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "brain_list.h"
#include "test_harness.h"

#if defined(_WIN32)
#  define SEP '\\'
#else
#  define SEP '/'
#endif

/* The brain directory this test writes its manifest into, named after the
 * running test.
 *
 * All four tests in this file used to share one compile-time name. CTest
 * runs each of them in its own process out of the same working directory,
 * so under `ctest -j` they created, read and deleted one another's
 * manifest: brain_modes_manifest_parses would read the eight-mode file
 * brain_modes_counts_clamped had just written and report "got 4 modes,
 * expected 2". Each passed alone, which is what made it look like a parse
 * bug rather than a fixture one. A name per test removes the sharing —
 * see utCurrentTestName in test_harness.h for why it is the test name and
 * not the process id. */
static const char *modes_brain_name(void) {
    static char name[160];
    SDL_snprintf(name, sizeof(name), "wbtest_modesbrain_%s",
                 utCurrentTestName());
    return name;
}

static void modes_path(char *out, size_t outSz) {
    SDL_snprintf(out, outSz, "brains%c%s%cmodes.txt", SEP,
                 modes_brain_name(), SEP);
}

/* Remove this test's fixture. Called on the way IN as well as out, so a
 * previous run that died mid-test cannot poison this one — which is the
 * whole reason the directory name is stable across runs. */
static void modes_cleanup(void) {
    char p[512];
    modes_path(p, sizeof(p));
    SDL_RemovePath(p);
    SDL_snprintf(p, sizeof(p), "brains%c%s", SEP, modes_brain_name());
    SDL_RemovePath(p);
    /* brains/ itself is deliberately left behind. It is shared with every
     * other test running right now, and removing it between a sibling's
     * SDL_CreateDirectory("brains") and its CreateDirectory of the child
     * would fail that sibling for no reason. An empty brains/ in a build
     * tree costs nothing; in a real checkout it was already there. */
}

/* Write `contents` as the temp brain's modes.txt. Returns 0 on success. */
static int modes_write(const char *contents) {
    char p[512];
    /* Both creates tolerate "it already exists" — brains/ normally does,
     * and a sibling test may be making it at the same moment. */
    if (!SDL_CreateDirectory("brains")) return 1;
    SDL_snprintf(p, sizeof(p), "brains%c%s", SEP, modes_brain_name());
    if (!SDL_CreateDirectory(p)) return 1;
    modes_path(p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return 1;
    fputs(contents, f);
    fclose(f);
    return 0;
}

/* The three things every caller relies on about the fallback. */
static int assert_is_synthesized_default(const BrainModes *m) {
    UT_ASSERT_MSG(m->modeCount == 1, "fallback has %d modes", m->modeCount);
    UT_ASSERT(strcmp(m->modes[0].key, "default") == 0);
    UT_ASSERT(strcmp(m->modes[0].label, "Default") == 0);
    UT_ASSERT_MSG(m->modes[0].levelCount == 3, "fallback has %d levels",
                  m->modes[0].levelCount);
    UT_ASSERT(strcmp(m->modes[0].levels[0].key, "easy") == 0);
    UT_ASSERT(strcmp(m->modes[0].levels[1].key, "medium") == 0);
    UT_ASSERT(strcmp(m->modes[0].levels[2].key, "hard") == 0);
    UT_ASSERT(strcmp(m->modes[0].levels[2].label, "Hard") == 0);
    UT_ASSERT_MSG(m->modes[0].defaultLevel == 2,
                  "fallback default level is %d, expected hard (2)",
                  m->modes[0].defaultLevel);
    return 0;
}

int run_brain_modes_manifest_parses(void) {
    BrainModes m;

    /* The shape brains/GoalHunter_1.7/modes.txt ships in, with a second mode
     * added: comments, blank lines, CRLF endings and spaces around every
     * '='. */
    modes_cleanup();
    UT_ASSERT(modes_write(
        "# GoalHunter modes.\r\n"
        "\r\n"
        "[default]\r\n"
        "label   = Default\r\n"
        "levels  = easy:Easy, medium:Medium, hard:Hard\r\n"
        "default = hard\r\n"
        "\r\n"
        "# Placeholder levels for now.\r\n"
        "[turtle]\r\n"
        "label   = Turtle\r\n"
        "levels  = easy:Easy, medium:Medium, hard:Hard\r\n"
        "default = medium\r\n") == 0);

    UT_ASSERT_MSG(brainListLoadModes(modes_brain_name(), &m),
                  "a readable manifest must report true");
    UT_ASSERT_MSG(m.modeCount == 2, "got %d modes, expected 2", m.modeCount);

    /* First section is mode 0 — the mode every ordinary game uses. */
    UT_ASSERT(strcmp(m.modes[0].key, "default") == 0);
    UT_ASSERT(strcmp(m.modes[0].label, "Default") == 0);
    UT_ASSERT(m.modes[0].levelCount == 3);
    UT_ASSERT(strcmp(m.modes[0].levels[1].key, "medium") == 0);
    UT_ASSERT(strcmp(m.modes[0].levels[1].label, "Medium") == 0);
    UT_ASSERT_MSG(m.modes[0].defaultLevel == 2, "default mode starts at %d",
                  m.modes[0].defaultLevel);

    /* A label may contain spaces; the key may not. */
    UT_ASSERT(strcmp(m.modes[1].key, "turtle") == 0);
    UT_ASSERT(strcmp(m.modes[1].label, "Turtle") == 0);
    UT_ASSERT_MSG(m.modes[1].defaultLevel == 1, "turtle starts at %d",
                  m.modes[1].defaultLevel);

    /* Key lookups, which is how every non-lobby caller (the -mode flag, the
     * chosen-mode preference) turns a word back into an index. */
    UT_ASSERT(brainModesFindMode(&m, "turtle") == 1);
    UT_ASSERT(brainModesFindMode(&m, "TURTLE") == 1);
    UT_ASSERT(brainModesFindMode(&m, "nosuchmode") == -1);
    UT_ASSERT(brainModeFindLevel(&m.modes[1], "hard") == 2);
    UT_ASSERT(brainModeFindLevel(&m.modes[1], "brutal") == -1);
    UT_ASSERT(brainModesFindMode(NULL, "default") == -1);
    UT_ASSERT(brainModeFindLevel(&m.modes[0], NULL) == -1);

    modes_cleanup();
    return 0;
}

int run_brain_modes_missing_falls_back(void) {
    BrainModes m;

    /* No manifest anywhere: false, and the synthesized default mode. */
    modes_cleanup();
    memset(&m, 0xAB, sizeof(m));
    UT_ASSERT_MSG(!brainListLoadModes(modes_brain_name(), &m),
                  "a brain with no modes.txt must report false");
    if (assert_is_synthesized_default(&m) != 0) return 1;

    /* A brain name that is empty or absent is the same story, never a
     * half-filled struct — the lobby indexes into this without checking. */
    memset(&m, 0xAB, sizeof(m));
    UT_ASSERT(!brainListLoadModes("", &m));
    if (assert_is_synthesized_default(&m) != 0) return 1;
    memset(&m, 0xAB, sizeof(m));
    UT_ASSERT(!brainListLoadModes(NULL, &m));
    if (assert_is_synthesized_default(&m) != 0) return 1;

    /* An unreadable NULL out pointer is refused rather than crashed on. */
    UT_ASSERT(!brainListLoadModes(modes_brain_name(), NULL));

    /* A file that exists but says nothing usable also falls back, because
     * an empty mode list would leave the lobby with nothing to render. */
    UT_ASSERT(modes_write("# only a comment\n\n   \n") == 0);
    memset(&m, 0xAB, sizeof(m));
    UT_ASSERT_MSG(!brainListLoadModes(modes_brain_name(), &m),
                  "a manifest with no usable mode must report false");
    if (assert_is_synthesized_default(&m) != 0) return 1;

    modes_cleanup();
    return 0;
}

int run_brain_modes_malformed_lines_skipped(void) {
    BrainModes m;

    modes_cleanup();
    UT_ASSERT(modes_write(
        "this line has no section and no meaning\n"
        "[Default]\n"                          /* upper case folds down    */
        "  label = Default   # trailing comment\n"
        "levels = easy:Easy, oops-no-colon, :NoKey, bad key:Bad, "
        "medium:Medium, hard:Hard, toolongkeyisrejected:X\n"
        "unknown_field = ignored entirely\n"
        "default = hard\n"
        "[empty]\n"                            /* declares no levels       */
        "label = Has No Levels\n"
        "[bad key]\n"                          /* illegal section key      */
        "levels = a:A\n"
        "[terse]\n"                            /* no label -> key is label */
        "levels = one:One, two:Two\n"
        "default = nosuchlevel\n") == 0);      /* unknown -> last level    */

    UT_ASSERT(brainListLoadModes(modes_brain_name(), &m));
    /* "bad key" is not a legal section key, so it is gone and the survivors
     * are contiguous. "empty" STAYS, which is a change: a mode may declare no
     * levels at all, and that says the brain has one way of playing and no
     * difficulty to pick (see brain_list.h). Dropping it, as this used to,
     * would make "no difficulty" impossible to express. */
    UT_ASSERT_MSG(m.modeCount == 3, "got %d modes, expected 3", m.modeCount);
    UT_ASSERT(strcmp(m.modes[0].key, "default") == 0);
    UT_ASSERT(strcmp(m.modes[1].key, "empty") == 0);
    UT_ASSERT(strcmp(m.modes[2].key, "terse") == 0);

    /* The no-levels mode really carries none, and its default index is a
     * harmless 0 that nothing reads. */
    UT_ASSERT_MSG(m.modes[1].levelCount == 0,
                  "the no-levels mode kept %d levels", m.modes[1].levelCount);
    UT_ASSERT(m.modes[1].defaultLevel == 0);

    /* Only the three well-formed level entries survived, in file order.
     * "toolongkeyisrejected" is 20 characters, past the 15 a key may be. */
    UT_ASSERT_MSG(m.modes[0].levelCount == 3, "kept %d levels, expected 3",
                  m.modes[0].levelCount);
    UT_ASSERT(strcmp(m.modes[0].levels[0].key, "easy") == 0);
    UT_ASSERT(strcmp(m.modes[0].levels[1].key, "medium") == 0);
    UT_ASSERT(strcmp(m.modes[0].levels[2].key, "hard") == 0);
    /* The trailing "# comment" is not part of the label. */
    UT_ASSERT_MSG(strcmp(m.modes[0].label, "Default") == 0,
                  "label kept the comment: '%s'", m.modes[0].label);

    /* A section with no label line renders as its key rather than blank. */
    UT_ASSERT(strcmp(m.modes[2].label, "terse") == 0);
    /* An unknown default key falls back to the last (hardest) level. */
    UT_ASSERT_MSG(m.modes[2].defaultLevel == 1,
                  "unknown default resolved to %d, expected the last level",
                  m.modes[2].defaultLevel);

    modes_cleanup();
    return 0;
}

int run_brain_modes_counts_clamped(void) {
    BrainModes m;
    char buf[4096];
    size_t pos = 0;
    int i;

    /* Ten levels in one mode and ten modes in the file — two more than
     * either fixed-size array holds. */
    modes_cleanup();
    pos += (size_t)SDL_snprintf(buf + pos, sizeof(buf) - pos, "[wide]\nlevels =");
    for (i = 0; i < 10; i++) {
        pos += (size_t)SDL_snprintf(buf + pos, sizeof(buf) - pos,
                                    "%s lv%d:Level %d", i ? "," : "", i, i);
    }
    pos += (size_t)SDL_snprintf(buf + pos, sizeof(buf) - pos, "\n");
    for (i = 0; i < 10; i++) {
        pos += (size_t)SDL_snprintf(buf + pos, sizeof(buf) - pos,
                                    "[m%d]\nlevels = only:Only\n", i);
    }
    UT_ASSERT(pos < sizeof(buf));
    UT_ASSERT(modes_write(buf) == 0);

    UT_ASSERT(brainListLoadModes(modes_brain_name(), &m));
    UT_ASSERT_MSG(m.modeCount == BRAIN_MODES_MAX,
                  "kept %d modes, expected the %d cap",
                  m.modeCount, BRAIN_MODES_MAX);
    UT_ASSERT(strcmp(m.modes[0].key, "wide") == 0);
    UT_ASSERT_MSG(m.modes[0].levelCount == BRAIN_LEVELS_MAX,
                  "kept %d levels, expected the %d cap",
                  m.modes[0].levelCount, BRAIN_LEVELS_MAX);
    /* The kept levels are the FIRST ones, in file order. */
    UT_ASSERT(strcmp(m.modes[0].levels[0].key, "lv0") == 0);
    UT_ASSERT(strcmp(m.modes[0].levels[BRAIN_LEVELS_MAX - 1].key, "lv7") == 0);
    /* No default line, so it is the last kept level. */
    UT_ASSERT(m.modes[0].defaultLevel == BRAIN_LEVELS_MAX - 1);
    /* The 8th mode is m6 (wide + m0..m6); m7..m9 fell off the end. */
    UT_ASSERT(strcmp(m.modes[BRAIN_MODES_MAX - 1].key, "m6") == 0);

    modes_cleanup();
    return 0;
}
