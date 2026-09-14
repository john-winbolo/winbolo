/*
 * Load Session rename (test_loadbrowser_rename.c).
 *
 * A recorder dir name is "<YYYYMMDD_HHMMSS>_<B>[_<label>]" with an optional
 * "_part<X>of<N>" from brainrec_split, and the LABEL is whatever
 * WINBOLO_BRAINDBG_LABEL happened to say when the game STARTED. Renaming it
 * afterwards has to move every dir of that game at once — all 15-minute
 * blocks and all their split parts — or the browser's segment walk (which
 * matches blocks by timestamp + label) would no longer join them up.
 *
 * These tests pin:
 *   - loadBrowserSplitName over every name shape on disk (bare timestamp,
 *     <ts>_<B>, labelled, labels holding '_' and labels that are all digits,
 *     with and without a part suffix, and the junk it must reject);
 *   - which dirs a rename claims as one FAMILY (same timestamp AND label;
 *     another label at the same timestamp, or the same label at another
 *     timestamp, stay put);
 *   - the plan itself: label added, label swapped, label dropped, block
 *     number and part suffix always preserved, parts listed before blocks;
 *   - every refusal (illegal characters, over-long label, no change, target
 *     name already taken, a member being the session BrainTest is replaying,
 *     a legacy dir with no block number, a plan bigger than the buffer);
 *   - and, against real directories, that applying a plan moves what it says
 *     it will and refuses up front when a target already exists on disk.
 *
 * Everything but the last test is pure computation over an in-memory listing.
 */

#include <SDL3/SDL.h>
#include <string.h>

#include "braintest_loadbrowser.h"
#include "test_harness.h"

/* The synthetic listing. Deliberately out of on-disk order and deliberately
 * crowded with near misses, so a family lookup that matched loosely (by
 * prefix, or by timestamp alone) would pull in dirs it must not touch.
 *
 *   20260903_011013 "john"        blocks 1 (split 3 ways, whole dir listed
 *                                 too, as the splitter leaves it), 2 (never
 *                                 split) and 3 (split 2 ways)  <- the family
 *   20260903_011013 "other"       same game start, another label
 *   20260903_011013 "oilrig_6bots" block 1 only — the collision target
 *   20260812_224500 "john"        same label, another game
 *   20260904_090000 ""            an unlabelled game (blocks 1+parts, 2)
 *   20260902_040000 "2026"        an all-digit label
 *   20260901_101010 "16v17_60min" a label carrying '_' and digits
 *   20260701_120000               a legacy bare-timestamp recording
 *   handmade_session              neither shape
 */
static const char *kNames[] = {
    "20260903_011013_1_john_part2of3",
    "20260903_011013_2_john",
    "handmade_session",
    "20260903_011013_1_john_part1of3",
    "20260903_011013_1_john",
    "20260903_011013_1_john_part3of3",
    "20260903_011013_1_other",
    "20260812_224500_1_john",
    "20260903_011013_1_oilrig_6bots",
    "20260701_120000",
    "20260903_011013_3_john_part1of2",
    "20260903_011013_3_john_part2of2",
    "20260904_090000_1",
    "20260904_090000_1_part1of2",
    "20260904_090000_1_part2of2",
    "20260904_090000_2",
    "20260902_040000_1_2026",
    "20260902_040000_2_2026",
    "20260901_101010_1_16v17_60min",
    "20260901_101010_1_16v17_60min_part1of2",
    "20260901_101010_1_16v17_60min_part2of2",
};
#define NNAMES ((int)(sizeof kNames / sizeof kNames[0]))

static LoadSessionEntry g_list[NNAMES];
static char g_plan[LOADBROWSER_PLAN_MAX][2][300];
static char g_err[400];

static void buildList(void) {
    for (int i = 0; i < NNAMES; i++) {
        memset(&g_list[i], 0, sizeof g_list[i]);
        snprintf(g_list[i].dir, sizeof g_list[i].dir, "debug_sessions/%s", kNames[i]);
        snprintf(g_list[i].name, sizeof g_list[i].name, "%s", kNames[i]);
        g_list[i].loadable = true;
    }
}

/* Plan the rename of `member`'s family to `label`, with nothing loaded. */
static int planFor(const char *member, const char *label) {
    g_err[0] = '\0';
    return loadBrowserRenameFamily(g_list, NNAMES, member, label, NULL,
                                   g_plan, LOADBROWSER_PLAN_MAX,
                                   g_err, sizeof g_err);
}

/* Is (old,new) in the plan, as basenames? */
static bool planHas(int n, const char *oldName, const char *newName) {
    for (int i = 0; i < n; i++) {
        char ob[LOADBROWSER_MAX_NAME], nb[LOADBROWSER_MAX_NAME];
        loadBrowserBaseName(g_plan[i][0], ob, sizeof ob);
        loadBrowserBaseName(g_plan[i][1], nb, sizeof nb);
        if (strcmp(ob, oldName) == 0 && strcmp(nb, newName) == 0) return true;
    }
    return false;
}

#define EXPECT_PAIR(n, o, w)                                                \
    UT_ASSERT_MSG(planHas((n), (o), (w)), "plan is missing %s -> %s", (o), (w))

int run_loadbrowser_rename_splits_names(void) {
    char ts[16], label[LOADBROWSER_MAX_NAME];
    int block, x, n;

#define SPLIT(name) \
    loadBrowserSplitName((name), ts, sizeof ts, &block, label, sizeof label, &x, &n)

    /* The everyday shape. */
    UT_ASSERT(SPLIT("20260903_011013_2_john"));
    UT_ASSERT_MSG(strcmp(ts, "20260903_011013") == 0, "ts '%s'", ts);
    UT_ASSERT_MSG(block == 2, "block %d", block);
    UT_ASSERT_MSG(strcmp(label, "john") == 0, "label '%s'", label);
    UT_ASSERT_MSG(x == 0 && n == 0, "part %d/%d", x, n);

    /* Same, split. */
    UT_ASSERT(SPLIT("20260903_011013_2_john_part1of3"));
    UT_ASSERT(block == 2 && strcmp(label, "john") == 0);
    UT_ASSERT_MSG(x == 1 && n == 3, "part %d/%d", x, n);

    /* No label. */
    UT_ASSERT(SPLIT("20260904_090000_1"));
    UT_ASSERT(block == 1 && label[0] == '\0' && n == 0);
    UT_ASSERT(SPLIT("20260904_090000_1_part2of2"));
    UT_ASSERT_MSG(block == 1 && label[0] == '\0' && x == 2 && n == 2,
                  "block %d label '%s' part %d/%d", block, label, x, n);

    /* A label that is all digits — the one case a looser parser would read as
     * the block number — and one carrying '_' and digits of its own. */
    UT_ASSERT(SPLIT("20260902_040000_2_2026"));
    UT_ASSERT_MSG(block == 2 && strcmp(label, "2026") == 0,
                  "block %d label '%s'", block, label);
    UT_ASSERT(SPLIT("20260901_101010_1_16v17_60min_part2of2"));
    UT_ASSERT_MSG(block == 1 && strcmp(label, "16v17_60min") == 0 && x == 2 && n == 2,
                  "block %d label '%s' part %d/%d", block, label, x, n);
    /* A label that itself starts with '_' (the sanitizer allows it). */
    UT_ASSERT(SPLIT("20260902_023310_3__seek_trees"));
    UT_ASSERT_MSG(block == 3 && strcmp(label, "_seek_trees") == 0,
                  "block %d label '%s'", block, label);
    /* A label containing "part" that is not a part suffix. */
    UT_ASSERT(SPLIT("20260902_023310_1_partial"));
    UT_ASSERT_MSG(block == 1 && strcmp(label, "partial") == 0 && n == 0,
                  "label '%s' part %d/%d", label, x, n);
    /* Two-digit blocks and part counts. */
    UT_ASSERT(SPLIT("20260902_023310_12_x_part10of12"));
    UT_ASSERT_MSG(block == 12 && x == 10 && n == 12, "block %d part %d/%d",
                  block, x, n);

    /* A legacy bare timestamp: parses, but with no block. */
    UT_ASSERT(SPLIT("20260701_120000"));
    UT_ASSERT_MSG(block == 0 && label[0] == '\0' && n == 0,
                  "bare ts gave block %d label '%s'", block, label);

    /* And the rejects, each with every output cleared. */
    UT_ASSERT(!SPLIT("handmade_session"));
    UT_ASSERT(block == 0 && ts[0] == '\0' && label[0] == '\0' && x == 0 && n == 0);
    UT_ASSERT(!SPLIT("20260701_12000"));            /* short timestamp */
    UT_ASSERT(!SPLIT("2026070a_120000_1"));         /* non-digit in the date */
    UT_ASSERT(!SPLIT("20260701-120000_1"));         /* wrong separator */
    UT_ASSERT(!SPLIT("20260701_120000_0_x"));       /* block 0 is not a block */
    UT_ASSERT(!SPLIT("20260701_120000_oilrig"));    /* label with no block */
    UT_ASSERT(!SPLIT("20260701_120000x"));          /* junk after the stamp */
    UT_ASSERT(!SPLIT(""));
    UT_ASSERT(!SPLIT(NULL));
    /* Part suffixes that are not part suffixes. */
    UT_ASSERT(SPLIT("20260701_120000_1_part1of"));  /* parses as a LABEL */
    UT_ASSERT_MSG(n == 0 && strcmp(label, "part1of") == 0, "label '%s' n %d", label, n);
    UT_ASSERT(SPLIT("20260701_120000_1_part0of3"));
    UT_ASSERT_MSG(n == 0, "part0of3 must not be a part");
    UT_ASSERT(SPLIT("20260701_120000_1_part4of3"));
    UT_ASSERT_MSG(n == 0, "part4of3 must not be a part");

    /* NULL outputs are allowed (the segment walk only wants some of them). */
    UT_ASSERT(loadBrowserSplitName("20260903_011013_2_john", NULL, 0, &block,
                                   NULL, 0, NULL, NULL));
    UT_ASSERT(block == 2);
#undef SPLIT
    return 0;
}

int run_loadbrowser_rename_family_plan(void) {
    buildList();

    /* Swap one label for another: all three blocks of 20260903_011013 "john"
     * plus every part, and nothing else. */
    int n = planFor("debug_sessions/20260903_011013_1_john_part2of3", "oilrig");
    UT_ASSERT_MSG(n == 7, "family size %d (%s)", n, g_err);
    EXPECT_PAIR(n, "20260903_011013_1_john_part1of3", "20260903_011013_1_oilrig_part1of3");
    EXPECT_PAIR(n, "20260903_011013_1_john_part2of3", "20260903_011013_1_oilrig_part2of3");
    EXPECT_PAIR(n, "20260903_011013_1_john_part3of3", "20260903_011013_1_oilrig_part3of3");
    EXPECT_PAIR(n, "20260903_011013_1_john",          "20260903_011013_1_oilrig");
    EXPECT_PAIR(n, "20260903_011013_2_john",          "20260903_011013_2_oilrig");
    EXPECT_PAIR(n, "20260903_011013_3_john_part1of2", "20260903_011013_3_oilrig_part1of2");
    EXPECT_PAIR(n, "20260903_011013_3_john_part2of2", "20260903_011013_3_oilrig_part2of2");

    /* The near misses stay out: another label at the same game start, the
     * same label at another game start, the collision target, the junk. */
    for (int i = 0; i < n; i++) {
        char ob[LOADBROWSER_MAX_NAME];
        loadBrowserBaseName(g_plan[i][0], ob, sizeof ob);
        UT_ASSERT_MSG(strcmp(ob, "20260903_011013_1_other") != 0, "took another label");
        UT_ASSERT_MSG(strcmp(ob, "20260812_224500_1_john") != 0, "took another game");
        UT_ASSERT_MSG(strcmp(ob, "20260903_011013_1_oilrig_6bots") != 0, "took a stranger");
        UT_ASSERT_MSG(strcmp(ob, "handmade_session") != 0, "took a hand-named dir");
    }

    /* PARTS are planned before the BLOCK dirs they were cut from. */
    {
        bool seenBlock = false;
        for (int i = 0; i < n; i++) {
            char ob[LOADBROWSER_MAX_NAME];
            int block = 0, x = 0, pn = 0;
            loadBrowserBaseName(g_plan[i][0], ob, sizeof ob);
            UT_ASSERT(loadBrowserSplitName(ob, NULL, 0, &block, NULL, 0, &x, &pn));
            if (pn == 0) seenBlock = true;
            else UT_ASSERT_MSG(!seenBlock, "part %s planned after a block dir", ob);
        }
    }

    /* Any member of the family names the same family. */
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_2_john", "oilrig") == 7,
                  "block dir gave a different family (%s)", g_err);
    UT_ASSERT_MSG(planFor("20260903_011013_3_john_part2of2", "oilrig") == 7,
                  "bare basename gave a different family (%s)", g_err);
    UT_ASSERT_MSG(planFor("D:/x/build/debug_sessions\\20260903_011013_1_john\\",
                          "oilrig") == 7,
                  "path form gave a different family (%s)", g_err);

    /* DROP the label: the block number and the part suffix survive, the
     * label and its separator go. */
    n = planFor("debug_sessions/20260903_011013_1_john", "");
    UT_ASSERT_MSG(n == 7, "drop-label family size %d (%s)", n, g_err);
    EXPECT_PAIR(n, "20260903_011013_1_john_part1of3", "20260903_011013_1_part1of3");
    EXPECT_PAIR(n, "20260903_011013_2_john",          "20260903_011013_2");
    /* NULL means the same thing as "". */
    UT_ASSERT(planFor("debug_sessions/20260903_011013_1_john", NULL) == 7);

    /* ADD a label to a game that never had one. */
    n = planFor("debug_sessions/20260904_090000_1_part1of2", "seek_trees");
    UT_ASSERT_MSG(n == 4, "add-label family size %d (%s)", n, g_err);
    EXPECT_PAIR(n, "20260904_090000_1_part1of2", "20260904_090000_1_seek_trees_part1of2");
    EXPECT_PAIR(n, "20260904_090000_1_part2of2", "20260904_090000_1_seek_trees_part2of2");
    EXPECT_PAIR(n, "20260904_090000_1",          "20260904_090000_1_seek_trees");
    EXPECT_PAIR(n, "20260904_090000_2",          "20260904_090000_2_seek_trees");

    /* An all-digit label is a label, not a block number. */
    n = planFor("debug_sessions/20260902_040000_1_2026", "2027");
    UT_ASSERT_MSG(n == 2, "all-digit family size %d (%s)", n, g_err);
    EXPECT_PAIR(n, "20260902_040000_1_2026", "20260902_040000_1_2027");
    EXPECT_PAIR(n, "20260902_040000_2_2026", "20260902_040000_2_2027");

    /* A label carrying '_' is matched whole, not up to the first '_'. */
    n = planFor("debug_sessions/20260901_101010_1_16v17_60min", "16v17_90min");
    UT_ASSERT_MSG(n == 3, "underscore-label family size %d (%s)", n, g_err);
    EXPECT_PAIR(n, "20260901_101010_1_16v17_60min",
                   "20260901_101010_1_16v17_90min");
    EXPECT_PAIR(n, "20260901_101010_1_16v17_60min_part2of2",
                   "20260901_101010_1_16v17_90min_part2of2");

    /* The new dir keeps the old dir's path prefix. */
    n = planFor("debug_sessions/20260902_040000_1_2026", "2027");
    UT_ASSERT_MSG(strcmp(g_plan[0][1], "debug_sessions/20260902_040000_1_2027") == 0 ||
                  strcmp(g_plan[1][1], "debug_sessions/20260902_040000_1_2027") == 0,
                  "plan lost the prefix: '%s'", g_plan[0][1]);
    return 0;
}

int run_loadbrowser_rename_rejections(void) {
    char big[LOADBROWSER_LABEL_MAX + 8];
    buildList();

    /* Characters that have no business in a dir name. */
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "oil rig") == -1,
                  "space accepted");
    UT_ASSERT(g_err[0]);
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "oil/rig") == -1,
                  "separator accepted");
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "oil.rig") == -1,
                  "dot accepted");
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "..") == -1,
                  "parent dir accepted");
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "oil:rig") == -1,
                  "drive separator accepted");
    /* ...and the ones that are fine. */
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "oil-rig_2") == 7,
                  "'-' or '_' rejected (%s)", g_err);

    /* Too long to leave room for the block and part suffixes. */
    for (int i = 0; i < (int)sizeof big - 1; i++) big[i] = 'x';
    big[sizeof big - 1] = '\0';
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", big) == -1,
                  "over-long label accepted");

    /* Nothing to do. */
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "john") == -1,
                  "no-op rename accepted");
    UT_ASSERT_MSG(planFor("debug_sessions/20260904_090000_1", "") == -1,
                  "dropping an absent label accepted");

    /* The target name is already another session's. */
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "oilrig_6bots") == -1,
                  "collision accepted");
    UT_ASSERT_MSG(planFor("debug_sessions/20260903_011013_1_john", "other") == -1,
                  "collision with the sibling label accepted");
    /* A name that only collides at ANOTHER timestamp is not a collision. */
    UT_ASSERT_MSG(planFor("debug_sessions/20260812_224500_1_john", "other") == 1,
                  "false collision across timestamps (%s)", g_err);

    /* A member is the recording this BrainTest is replaying. */
    UT_ASSERT_MSG(loadBrowserRenameFamily(g_list, NNAMES,
                      "debug_sessions/20260903_011013_1_john", "oilrig",
                      "debug_sessions/20260903_011013_2_john",
                      g_plan, LOADBROWSER_PLAN_MAX, g_err, sizeof g_err) == -1,
                  "renamed the loaded session's family");
    UT_ASSERT(g_err[0]);
    /* Even when the loaded dir IS the clicked row, and whatever path form it
     * arrives in. */
    UT_ASSERT(loadBrowserRenameFamily(g_list, NNAMES,
                  "debug_sessions/20260903_011013_1_john", "oilrig",
                  "D:/x/build/debug_sessions\\20260903_011013_1_john",
                  g_plan, LOADBROWSER_PLAN_MAX, g_err, sizeof g_err) == -1);
    /* A session loaded from OUTSIDE the family is no obstacle. */
    UT_ASSERT_MSG(loadBrowserRenameFamily(g_list, NNAMES,
                      "debug_sessions/20260903_011013_1_john", "oilrig",
                      "debug_sessions/20260812_224500_1_john",
                      g_plan, LOADBROWSER_PLAN_MAX, g_err, sizeof g_err) == 7,
                  "an unrelated loaded session blocked the rename (%s)", g_err);

    /* Dirs with no label slot at all. */
    UT_ASSERT_MSG(planFor("debug_sessions/handmade_session", "oilrig") == -1,
                  "hand-named dir accepted");
    UT_ASSERT_MSG(planFor("debug_sessions/20260701_120000", "oilrig") == -1,
                  "legacy bare-timestamp dir accepted");

    /* Not in the list at all. */
    UT_ASSERT(planFor("debug_sessions/20261231_235959_1_ghost", "oilrig") == -1);
    UT_ASSERT(planFor("", "oilrig") == -1);
    UT_ASSERT(planFor(NULL, "oilrig") == -1);

    /* More dirs than the caller's buffer holds. */
    g_err[0] = '\0';
    UT_ASSERT_MSG(loadBrowserRenameFamily(g_list, NNAMES,
                      "debug_sessions/20260903_011013_1_john", "oilrig",
                      NULL, g_plan, 3, g_err, sizeof g_err) == -1,
                  "overran a 3-pair plan buffer");
    UT_ASSERT(g_err[0]);
    /* Exactly enough is enough. */
    UT_ASSERT(loadBrowserRenameFamily(g_list, NNAMES,
                  "debug_sessions/20260903_011013_1_john", "oilrig",
                  NULL, g_plan, 7, g_err, sizeof g_err) == 7);

    /* An empty listing. */
    UT_ASSERT(loadBrowserRenameFamily(g_list, 0,
                  "debug_sessions/20260903_011013_1_john", "oilrig", NULL,
                  g_plan, LOADBROWSER_PLAN_MAX, g_err, sizeof g_err) == -1);
    return 0;
}

/* ---- The only test that touches disk ----------------------------------- */

#define TMPROOT "ut_lbrename_tmp"

static const char *kDiskNames[] = {
    "20260903_011013_1_john_part1of2",
    "20260903_011013_1_john_part2of2",
    "20260903_011013_1_john",
    "20260903_011013_2_john",
};
#define NDISK ((int)(sizeof kDiskNames / sizeof kDiskNames[0]))

static void diskCleanup(void) {
    char p[300];
    static const char *leftovers[] = {
        "20260903_011013_1_john_part1of2", "20260903_011013_1_john_part2of2",
        "20260903_011013_1_john",          "20260903_011013_2_john",
        "20260903_011013_1_oilrig_part1of2", "20260903_011013_1_oilrig_part2of2",
        "20260903_011013_1_oilrig",          "20260903_011013_2_oilrig",
    };
    for (int i = 0; i < (int)(sizeof leftovers / sizeof leftovers[0]); i++) {
        SDL_snprintf(p, sizeof p, TMPROOT "/%s", leftovers[i]);
        SDL_RemovePath(p);
    }
    SDL_RemovePath(TMPROOT);
}

static bool isDir(const char *path) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}

int run_loadbrowser_rename_applies(void) {
    LoadSessionEntry list[NDISK];
    char path[300];
    int n, applied;

    diskCleanup();                       /* any wreckage from a failed run */
    UT_ASSERT_MSG(SDL_CreateDirectory(TMPROOT), "mkdir %s: %s", TMPROOT, SDL_GetError());
    for (int i = 0; i < NDISK; i++) {
        SDL_snprintf(path, sizeof path, TMPROOT "/%s", kDiskNames[i]);
        UT_ASSERT_MSG(SDL_CreateDirectory(path), "mkdir %s: %s", path, SDL_GetError());
        memset(&list[i], 0, sizeof list[i]);
        SDL_strlcpy(list[i].dir, path, sizeof list[i].dir);
        SDL_strlcpy(list[i].name, kDiskNames[i], sizeof list[i].name);
        list[i].loadable = true;
    }

    /* Plan and apply the whole family. */
    g_err[0] = '\0';
    n = loadBrowserRenameFamily(list, NDISK, TMPROOT "/20260903_011013_1_john",
                                "oilrig", NULL, g_plan, LOADBROWSER_PLAN_MAX,
                                g_err, sizeof g_err);
    UT_ASSERT_MSG(n == NDISK, "planned %d of %d (%s)", n, NDISK, g_err);

    applied = loadBrowserApplyRenames((const char (*)[2][300])g_plan, n,
                                      g_err, sizeof g_err);
    UT_ASSERT_MSG(applied == n, "applied %d of %d: %s", applied, n, g_err);
    UT_ASSERT_MSG(g_err[0] == '\0', "success left an error: %s", g_err);

    /* Every old dir is gone and every new one is there — block number and
     * part suffix intact. */
    for (int i = 0; i < n; i++) {
        UT_ASSERT_MSG(!isDir(g_plan[i][0]), "%s survived the rename", g_plan[i][0]);
        UT_ASSERT_MSG(isDir(g_plan[i][1]), "%s was not created", g_plan[i][1]);
    }
    UT_ASSERT(isDir(TMPROOT "/20260903_011013_1_oilrig_part1of2"));
    UT_ASSERT(isDir(TMPROOT "/20260903_011013_1_oilrig_part2of2"));
    UT_ASSERT(isDir(TMPROOT "/20260903_011013_1_oilrig"));
    UT_ASSERT(isDir(TMPROOT "/20260903_011013_2_oilrig"));

    /* A target that exists on disk but never reached the listing (no
     * brainrec.btr, so the scan skipped it) is refused BEFORE anything moves.
     * Here: put "john" back in the way of a rename to "john". */
    {
        LoadSessionEntry back[NDISK];
        for (int i = 0; i < NDISK; i++) {
            char newName[LOADBROWSER_MAX_NAME];
            loadBrowserBaseName(g_plan[i][1], newName, sizeof newName);
            memset(&back[i], 0, sizeof back[i]);
            SDL_strlcpy(back[i].dir, g_plan[i][1], sizeof back[i].dir);
            SDL_strlcpy(back[i].name, newName, sizeof back[i].name);
            back[i].loadable = true;
        }
        /* An unlisted squatter on one of the targets. */
        UT_ASSERT(SDL_CreateDirectory(TMPROOT "/20260903_011013_2_john"));

        g_err[0] = '\0';
        n = loadBrowserRenameFamily(back, NDISK, TMPROOT "/20260903_011013_1_oilrig",
                                    "john", NULL, g_plan, LOADBROWSER_PLAN_MAX,
                                    g_err, sizeof g_err);
        UT_ASSERT_MSG(n == NDISK, "planned %d (%s)", n, g_err);   /* listing is clean */

        applied = loadBrowserApplyRenames((const char (*)[2][300])g_plan, n,
                                          g_err, sizeof g_err);
        UT_ASSERT_MSG(applied == -1, "applied over an existing dir");
        UT_ASSERT_MSG(g_err[0], "refusal gave no reason");
        /* And nothing moved. */
        UT_ASSERT(isDir(TMPROOT "/20260903_011013_1_oilrig_part1of2"));
        UT_ASSERT(isDir(TMPROOT "/20260903_011013_1_oilrig"));
        UT_ASSERT(isDir(TMPROOT "/20260903_011013_2_oilrig"));
    }

    /* An empty plan is a no-op with a reason, not a crash. */
    g_err[0] = '\0';
    UT_ASSERT(loadBrowserApplyRenames((const char (*)[2][300])g_plan, 0,
                                      g_err, sizeof g_err) == -1);
    UT_ASSERT(g_err[0]);
    UT_ASSERT(loadBrowserApplyRenames(NULL, 3, g_err, sizeof g_err) == -1);

    diskCleanup();
    UT_ASSERT_MSG(!isDir(TMPROOT), "test left %s behind", TMPROOT);
    return 0;
}
