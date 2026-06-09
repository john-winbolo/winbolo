/*
 * Coverage for -log path composition (serverDedicatedLogComposePath).
 *
 * Pins the behaviour of the -log argument:
 *   - bare -log (empty arg)        -> auto-name in the cwd
 *   - -log <file>                  -> that file verbatim
 *   - -log <dir>                   -> auto-name placed *inside* the dir
 *   - .wbv extension is appended exactly once in every case
 *
 * The directory case is the feature these tests were added for: passing a
 * directory (e.g. -log /tmp) auto-names the replay inside it rather than
 * treating the directory path as a filename. A real directory is created
 * with SDL so SDL_GetPathInfo sees it as one, then removed at the end.
 */

#include <string.h>

#include <SDL3/SDL.h>

#include "server_dedicated_log.h"
#include "test_harness.h"

#define AUTO_BASE "20260609t120000_Everard_Island"

/* True if `s` ends with `suffix`. */
static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

int run_log_path_empty_uses_autobase(void) {
    char out[512];
    serverDedicatedLogComposePath("", AUTO_BASE, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, AUTO_BASE ".wbv") == 0,
                  "empty -log should auto-name in cwd, got '%s'", out);
    return 0;
}

int run_log_path_explicit_file_verbatim(void) {
    char out[512];
    /* A name that does not exist on disk is treated as an explicit file. */
    serverDedicatedLogComposePath("mygame", AUTO_BASE, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "mygame.wbv") == 0,
                  "explicit -log <file> should be used verbatim, got '%s'", out);
    return 0;
}

int run_log_path_explicit_file_keeps_single_wbv(void) {
    char out[512];
    serverDedicatedLogComposePath("mygame.wbv", AUTO_BASE, out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "mygame.wbv") == 0,
                  ".wbv must not be doubled, got '%s'", out);
    return 0;
}

int run_log_path_directory_autonames_inside(void) {
    char dir[512];
    char out[512];
    char want[600];

    /* Create a real directory so SDL_GetPathInfo classifies it as one. */
    SDL_snprintf(dir, sizeof(dir), "ut_logdir_compose");
    SDL_RemovePath(dir);  /* clean any stale leftover */
    UT_ASSERT_MSG(SDL_CreateDirectory(dir), "could not create test dir '%s': %s",
                  dir, SDL_GetError());

    serverDedicatedLogComposePath(dir, AUTO_BASE, out, sizeof(out));

    SDL_snprintf(want, sizeof(want), "%s/%s.wbv", dir, AUTO_BASE);
    int ok = (strcmp(out, want) == 0);
    SDL_RemovePath(dir);
    UT_ASSERT_MSG(ok, "-log <dir> should auto-name inside the dir: want '%s' got '%s'",
                  want, out);
    return 0;
}

int run_log_path_directory_trailing_slash_no_double(void) {
    char dir[512];
    char dirSlash[514];
    char out[512];
    char want[600];

    SDL_snprintf(dir, sizeof(dir), "ut_logdir_trailing");
    SDL_RemovePath(dir);
    UT_ASSERT_MSG(SDL_CreateDirectory(dir), "could not create test dir '%s': %s",
                  dir, SDL_GetError());

    SDL_snprintf(dirSlash, sizeof(dirSlash), "%s/", dir);
    serverDedicatedLogComposePath(dirSlash, AUTO_BASE, out, sizeof(out));

    SDL_snprintf(want, sizeof(want), "%s/%s.wbv", dir, AUTO_BASE);
    int ok = (strcmp(out, want) == 0) && (strstr(out, "//") == NULL);
    SDL_RemovePath(dir);
    UT_ASSERT_MSG(ok, "trailing-slash dir must not double the separator: want '%s' got '%s'",
                  want, out);
    (void)ends_with;
    return 0;
}

int run_log_path_directory_arg_appends_wbv(void) {
    char dir[512];
    char out[512];

    SDL_snprintf(dir, sizeof(dir), "ut_logdir_wbv");
    SDL_RemovePath(dir);
    UT_ASSERT_MSG(SDL_CreateDirectory(dir), "could not create test dir '%s': %s",
                  dir, SDL_GetError());

    serverDedicatedLogComposePath(dir, AUTO_BASE, out, sizeof(out));
    int ok = ends_with(out, ".wbv");
    SDL_RemovePath(dir);
    UT_ASSERT_MSG(ok, "directory-composed path must end in .wbv, got '%s'", out);
    return 0;
}
