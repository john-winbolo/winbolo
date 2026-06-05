/*
 * Tests for the process-global preferences API in src/common/prefs.c.
 *
 *   - prefs_api_roundtrip:      missing file is materialized on init,
 *                               sets flush (not dirty), values persist
 *                               across shutdown/re-init
 *   - prefs_api_defaults:       missing section/key returns the default,
 *                               NULL default reads back empty
 *   - prefs_api_corrupt_backup: an unparseable file is renamed to
 *                               "<path>.corrupt" and replaced by a fresh
 *                               empty document; the corrupt content is
 *                               discarded, not used
 *
 * Stubbed out on Windows: the API itself is cross-platform, but these
 * tests lean on POSIX unlink/stat for path hygiene.
 */

#include "test_harness.h"

#ifdef _WIN32

int run_prefs_api_roundtrip(void)      { return 0; }
int run_prefs_api_defaults(void)       { return 0; }
int run_prefs_api_corrupt_backup(void) { return 0; }

#else

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/prefs.h"

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void cleanup(const char *jsonPath) {
    char corruptPath[1024];
    snprintf(corruptPath, sizeof(corruptPath), "%s.corrupt", jsonPath);
    unlink(jsonPath);
    unlink(corruptPath);
}

/* -------------------------------------------------------------------- */

int run_prefs_api_roundtrip(void) {
    const char *jsonPath = "/tmp/winbolo_ut_prefsapi_roundtrip.json";
    cleanup(jsonPath);

    prefsInit(jsonPath);
    UT_ASSERT_MSG(file_exists(jsonPath),
                  "init did not materialize a missing file at %s", jsonPath);

    UT_ASSERT(prefsSetString("SETTINGS", "Player Name", "Me"));
    UT_ASSERT(prefsSetString("SETTINGS", "Target Address", "127.0.0.1"));
    UT_ASSERT(prefsSetString("KEYS", "Forward", "8"));

    /* Flush-on-set leaves nothing pending. */
    UT_ASSERT_MSG(!prefsIsDirty(), "document dirty after a flushing set");

    char buf[128];
    prefsGetString("SETTINGS", "Player Name", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Me") == 0, "round-trip mismatch: '%s'", buf);
    prefsGetString("KEYS", "Forward", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "8") == 0, "round-trip mismatch: '%s'", buf);

    prefsShutdown();

    /* Values survive a fresh load of the same file. */
    prefsInit(jsonPath);
    prefsGetString("SETTINGS", "Target Address", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "127.0.0.1") == 0,
                  "value did not persist across re-init: '%s'", buf);
    prefsGetString("KEYS", "Forward", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "8") == 0,
                  "value did not persist across re-init: '%s'", buf);
    prefsShutdown();

    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_defaults(void) {
    const char *jsonPath = "/tmp/winbolo_ut_prefsapi_defaults.json";
    cleanup(jsonPath);

    prefsInit(jsonPath);

    char buf[64];
    prefsGetString("NOPE", "Missing", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<default>") == 0,
                  "missing section/key should return default, got '%s'", buf);

    /* NULL default reads back as an empty string. */
    prefsGetString("NOPE", "Missing", NULL, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "") == 0,
                  "NULL default should read back empty, got '%s'", buf);

    prefsShutdown();
    cleanup(jsonPath);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_api_corrupt_backup(void) {
    const char *jsonPath = "/tmp/winbolo_ut_prefsapi_corrupt.json";
    char corruptPath[1024];
    snprintf(corruptPath, sizeof(corruptPath), "%s.corrupt", jsonPath);
    cleanup(jsonPath);

    /* Plant an unparseable file. */
    FILE *fp = fopen(jsonPath, "wb");
    UT_ASSERT(fp != NULL);
    UT_ASSERT(fputs("{ not valid json", fp) >= 0);
    UT_ASSERT(fclose(fp) == 0);

    prefsInit(jsonPath);

    /* Corrupt content preserved under the .corrupt sibling. */
    UT_ASSERT_MSG(file_exists(corruptPath),
                  "corrupt file not backed up to %s", corruptPath);
    /* json path rewritten with a valid, freshly materialized document. */
    UT_ASSERT_MSG(file_exists(jsonPath),
                  "json path not rewritten after corrupt recovery");

    char buf[64];
    prefsGetString("SETTINGS", "Player Name", "<default>", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<default>") == 0,
                  "corrupt content was not discarded: got '%s'", buf);

    prefsShutdown();
    cleanup(jsonPath);
    return 0;
}

#endif /* !_WIN32 */
