/*
 * Regression coverage for brainListScanParent's stored disk path.
 *
 * The lobby bot dropdown is built from brainListScan, which records, for
 * each brain it finds, the disk path the lobby later hands to
 * luaBrainInstanceCreate to actually load that brain. The path MUST point
 * at the directory the brain was scanned from: brains under the prefs dir
 * (or SDL_GetBasePath) live outside the cwd, and the host chdir's to
 * SDL_GetBasePath at startup, so a hardcoded relative "Brains/<name>"
 * would resolve against the wrong directory and the bot would fail to
 * load even though it showed up in the list.
 *
 * This test scans a temp directory whose name is deliberately NOT
 * "Brains", then asserts the stored path opens — which only holds when
 * the path tracks the scanned directory rather than a fixed prefix.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "brain_list.h"
#include "brain_list_internal.h"
#include "test_harness.h"

#if defined(_WIN32)
#  define SEP '\\'
#else
#  define SEP '/'
#endif

#define SCAN_ROOT "wbtest_brainscan"
#define BRAIN_DIR_NAME "MyTestBrain"
#define EMPTY_DIR_NAME "NoInitHere"

static void rm(const char *p) { SDL_RemovePath(p); }

/* Best-effort teardown of the temp tree (children before parents). */
static void cleanup(void) {
    char p[512];
    SDL_snprintf(p, sizeof(p), "%s%c%s%cinit.lua", SCAN_ROOT, SEP,
                 BRAIN_DIR_NAME, SEP);
    rm(p);
    SDL_snprintf(p, sizeof(p), "%s%c%s", SCAN_ROOT, SEP, BRAIN_DIR_NAME);
    rm(p);
    SDL_snprintf(p, sizeof(p), "%s%c%s", SCAN_ROOT, SEP, EMPTY_DIR_NAME);
    rm(p);
    rm(SCAN_ROOT);
}

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs(contents, f);
    fclose(f);
    return 0;
}

int run_brain_list_scan_path_resolves(void) {
    char p[512];

    /* Fresh tree: SCAN_ROOT/MyTestBrain/init.lua (a real brain) and
     * SCAN_ROOT/NoInitHere (a dir with no init.lua — must be skipped). */
    cleanup();
    UT_ASSERT(SDL_CreateDirectory(SCAN_ROOT));
    SDL_snprintf(p, sizeof(p), "%s%c%s", SCAN_ROOT, SEP, BRAIN_DIR_NAME);
    UT_ASSERT(SDL_CreateDirectory(p));
    SDL_snprintf(p, sizeof(p), "%s%c%s", SCAN_ROOT, SEP, EMPTY_DIR_NAME);
    UT_ASSERT(SDL_CreateDirectory(p));
    SDL_snprintf(p, sizeof(p), "%s%c%s%cinit.lua", SCAN_ROOT, SEP,
                 BRAIN_DIR_NAME, SEP);
    UT_ASSERT_MSG(write_file(p, "return {}\n") == 0, "could not write %s", p);

    BrainList list;
    char paths[BRAIN_LIST_MAX][BRAIN_LIST_PATH_LEN];
    memset(&list, 0, sizeof(list));
    memset(paths, 0, sizeof(paths));

    brainListScanParent(&list, paths, SCAN_ROOT);

    /* Only the dir that actually has an init.lua is catalogued. */
    UT_ASSERT_MSG(list.count == 1, "expected 1 brain, got %d", list.count);
    UT_ASSERT(strcmp(list.entries[0].name, BRAIN_DIR_NAME) == 0);

    /* The stored path must track the scanned directory, not a fixed
     * "Brains/<name>" — i.e. it must point at the file we just wrote. */
    char expect[512];
    SDL_snprintf(expect, sizeof(expect), "%s%c%s%cinit.lua", SCAN_ROOT, SEP,
                 BRAIN_DIR_NAME, SEP);
    UT_ASSERT_MSG(strcmp(paths[0], expect) == 0,
                  "stored path '%s' != scanned path '%s'", paths[0], expect);

    FILE *f = fopen(paths[0], "rb");
    UT_ASSERT_MSG(f != NULL,
                  "stored brain path '%s' does not open — lobby would fail "
                  "to load a brain it listed", paths[0]);
    fclose(f);

    cleanup();
    return 0;
}
