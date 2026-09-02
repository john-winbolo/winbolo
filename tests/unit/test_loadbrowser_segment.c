/*
 * Load Session segment walking (test_loadbrowser_segment.c).
 *
 * loadBrowserFindSegment turns "which dir do I load next?" into an index, so
 * the browser's [ / ] buttons can step through a long recording. The two
 * shapes on disk are the recorder's 15-minute BLOCKS
 * ("<YYYYMMDD_HHMMSS>_<B>[_<label>]", server_lifecycle.c) and
 * brainrec_split's PARTS ("<block>_part<X>of<N>"). These tests pin:
 *
 *   - inside a split block, next/prev is the neighbouring part;
 *   - off the last part, the NEXT block's part1of<M> (any M);
 *   - when the next block was never split, its whole dir instead;
 *   - back off part1, the PREVIOUS block's LAST part;
 *   - a missing neighbour is -1, never a wrap-around or a nearby session;
 *   - trailing separators, path prefixes and (on Windows) case don't matter;
 *   - a dir that is neither a part nor a block yields -1.
 */

#include <string.h>

#include "braintest_loadbrowser.h"
#include "test_harness.h"

/* The synthetic listing every test works against. Deliberately not in
 * on-disk order, and deliberately mixing two unrelated sessions, so a
 * lookup that matched loosely (prefix, or "the next row") would fail.
 *
 *   block 1 of 20260902_030233 "16v17": split into 4 parts (whole dir listed
 *                                       too, exactly as the splitter leaves it)
 *   block 2 of 20260902_030233 "16v17": split into 3 parts
 *   block 3 of 20260902_030233 "16v17": never split — whole dir only
 *   20260902_000405_1_other        : an unrelated (unsplit, unblocked-past-1)
 *                                    session
 *   handmade_session               : neither shape
 */
static const char *kNames[] = {
    "20260902_030233_1_16v17_part2of4",
    "20260902_030233_2_16v17_part1of3",
    "handmade_session",
    "20260902_030233_1_16v17_part1of4",
    "20260902_030233_1_16v17",
    "20260902_030233_2_16v17_part3of3",
    "20260902_030233_1_16v17_part4of4",
    "20260902_000405_1_other",
    "20260902_030233_2_16v17_part2of3",
    "20260902_030233_1_16v17_part3of4",
    "20260902_030233_3_16v17",
    "20260902_030233_2_16v17",
};
#define NNAMES ((int)(sizeof kNames / sizeof kNames[0]))

static LoadSessionEntry g_list[NNAMES];

static void buildList(void) {
    for (int i = 0; i < NNAMES; i++) {
        memset(&g_list[i], 0, sizeof g_list[i]);
        snprintf(g_list[i].dir, sizeof g_list[i].dir, "debug_sessions/%s", kNames[i]);
        snprintf(g_list[i].name, sizeof g_list[i].name, "%s", kNames[i]);
        g_list[i].loadable = true;
    }
}

/* Name of the entry loadBrowserFindSegment picked, or "-1"/"?" so a failure
 * message says which dir it chose rather than an index. */
static const char *pick(const char *loaded, int dir) {
    int idx = loadBrowserFindSegment(g_list, NNAMES, loaded, dir);
    if (idx == -1) return "-1";
    if (idx < 0 || idx >= NNAMES) return "?";
    return g_list[idx].name;
}

#define EXPECT_PICK(loaded, dir, want)                                      \
    UT_ASSERT_MSG(strcmp(pick((loaded), (dir)), (want)) == 0,               \
                  "%s dir=%+d: got %s want %s",                             \
                  (loaded), (dir), pick((loaded), (dir)), (want))

int run_loadbrowser_segment_walks_parts(void) {
    buildList();
    /* Forward and back inside one block's parts. */
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17_part1of4", +1,
                "20260902_030233_1_16v17_part2of4");
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17_part2of4", +1,
                "20260902_030233_1_16v17_part3of4");
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17_part3of4", -1,
                "20260902_030233_1_16v17_part2of4");
    EXPECT_PICK("debug_sessions/20260902_030233_2_16v17_part2of3", -1,
                "20260902_030233_2_16v17_part1of3");
    return 0;
}

int run_loadbrowser_segment_crosses_blocks(void) {
    buildList();
    /* Last part of block 1 -> block 2's FIRST part (not block 2's whole dir,
     * which the split superseded, and not block 1's own whole dir). */
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17_part4of4", +1,
                "20260902_030233_2_16v17_part1of3");
    /* First part of block 2 -> block 1's LAST part. */
    EXPECT_PICK("debug_sessions/20260902_030233_2_16v17_part1of3", -1,
                "20260902_030233_1_16v17_part4of4");
    /* Block 3 was never split, so the last part of block 2 lands on the
     * whole dir. */
    EXPECT_PICK("debug_sessions/20260902_030233_2_16v17_part3of3", +1,
                "20260902_030233_3_16v17");
    /* And from an unsplit block dir the walk still works both ways. */
    EXPECT_PICK("debug_sessions/20260902_030233_3_16v17", -1,
                "20260902_030233_2_16v17_part3of3");
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17", +1,
                "20260902_030233_2_16v17_part1of3");
    return 0;
}

int run_loadbrowser_segment_no_neighbour(void) {
    buildList();
    /* Past the last block, and before block 1 (block 0 doesn't exist). */
    EXPECT_PICK("debug_sessions/20260902_030233_3_16v17", +1, "-1");
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17_part1of4", -1, "-1");
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17", -1, "-1");
    /* A session whose neighbouring blocks were never recorded — and which
     * must NOT borrow the other session's rows. */
    EXPECT_PICK("debug_sessions/20260902_000405_1_other", +1, "-1");
    EXPECT_PICK("debug_sessions/20260902_000405_1_other", -1, "-1");
    /* Neither a part nor a block. */
    EXPECT_PICK("debug_sessions/handmade_session", +1, "-1");
    EXPECT_PICK("debug_sessions/handmade_session", -1, "-1");
    /* Nothing loaded, and a nonsense direction. */
    UT_ASSERT(loadBrowserFindSegment(g_list, NNAMES, "", +1) == -1);
    UT_ASSERT(loadBrowserFindSegment(g_list, NNAMES, NULL, +1) == -1);
    UT_ASSERT(loadBrowserFindSegment(g_list, NNAMES,
                  "debug_sessions/20260902_030233_1_16v17_part1of4", 0) == -1);
    UT_ASSERT(loadBrowserFindSegment(g_list, 0,
                  "debug_sessions/20260902_030233_1_16v17_part1of4", +1) == -1);
    return 0;
}

int run_loadbrowser_segment_path_forms(void) {
    buildList();
    /* -loadsession may arrive with any prefix, either separator, a trailing
     * separator, or bare. All name the same session. */
    EXPECT_PICK("20260902_030233_1_16v17_part1of4", +1,
                "20260902_030233_1_16v17_part2of4");
    EXPECT_PICK("debug_sessions/20260902_030233_1_16v17_part1of4/", +1,
                "20260902_030233_1_16v17_part2of4");
    EXPECT_PICK("debug_sessions\\20260902_030233_1_16v17_part1of4", +1,
                "20260902_030233_1_16v17_part2of4");
    EXPECT_PICK("D:/x/build/debug_sessions\\20260902_030233_1_16v17_part1of4\\\\", +1,
                "20260902_030233_1_16v17_part2of4");
#ifdef _WIN32
    /* Filenames are case-insensitive here, so a differently-cased
     * -loadsession still finds its neighbour. */
    EXPECT_PICK("debug_sessions/20260902_030233_1_16V17_PART1OF4", +1,
                "20260902_030233_1_16v17_part2of4");
#endif

    /* Basename helper on its own: the browser prints it above the buttons. */
    {
        char b[LOADBROWSER_MAX_NAME];
        loadBrowserBaseName("debug_sessions/foo/", b, sizeof b);
        UT_ASSERT_MSG(strcmp(b, "foo") == 0, "basename got '%s'", b);
        loadBrowserBaseName("bare", b, sizeof b);
        UT_ASSERT_MSG(strcmp(b, "bare") == 0, "basename got '%s'", b);
        loadBrowserBaseName("///", b, sizeof b);
        UT_ASSERT_MSG(b[0] == '\0', "basename of separators got '%s'", b);
        loadBrowserBaseName(NULL, b, sizeof b);
        UT_ASSERT(b[0] == '\0');
    }

    /* And the same-session compare the row highlight uses. */
    UT_ASSERT(loadBrowserSameSession("debug_sessions/a_1", "x/y/a_1"));
    UT_ASSERT(!loadBrowserSameSession("debug_sessions/a_1", "debug_sessions/a_2"));
    UT_ASSERT(!loadBrowserSameSession("debug_sessions/a_1", ""));
    return 0;
}

int run_loadbrowser_segment_labels(void) {
    /* Label handling: an empty label ("<ts>_<B>"), a label that itself starts
     * with '_' (WINBOLO_BRAINDBG_LABEL is sanitized to [A-Za-z0-9_-], so
     * "<ts>_<B>__seek_trees" happens), and a label that is all digits — the
     * one case a looser block parser would read as the block number. */
    static const char *names[] = {
        "20260902_001658_1",
        "20260902_001658_2",
        "20260902_023310_1__seek_trees",
        "20260902_023310_2__seek_trees",
        "20260902_040000_1_2026",
        "20260902_040000_2_2026",
    };
    const int n = (int)(sizeof names / sizeof names[0]);
    LoadSessionEntry list[6];
    for (int i = 0; i < n; i++) {
        memset(&list[i], 0, sizeof list[i]);
        snprintf(list[i].dir, sizeof list[i].dir, "debug_sessions/%s", names[i]);
        snprintf(list[i].name, sizeof list[i].name, "%s", names[i]);
        list[i].loadable = true;
    }
    UT_ASSERT_MSG(loadBrowserFindSegment(list, n, "debug_sessions/20260902_001658_1", +1) == 1,
                  "no-label block step");
    UT_ASSERT_MSG(loadBrowserFindSegment(list, n, "debug_sessions/20260902_001658_2", -1) == 0,
                  "no-label block step back");
    UT_ASSERT_MSG(loadBrowserFindSegment(list, n,
                      "debug_sessions/20260902_023310_1__seek_trees", +1) == 3,
                  "leading-underscore label");
    UT_ASSERT_MSG(loadBrowserFindSegment(list, n,
                      "debug_sessions/20260902_040000_1_2026", +1) == 5,
                  "all-digit label");
    UT_ASSERT_MSG(loadBrowserFindSegment(list, n, "debug_sessions/20260902_001658_2", +1) == -1,
                  "invented a block 3");
    return 0;
}
