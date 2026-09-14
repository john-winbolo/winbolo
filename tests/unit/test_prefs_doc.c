/*
 * Tests for the cJSON-backed preferences document in
 * src/common/prefs_doc.c.
 *
 * The in-memory tests (round-trip, defaults, special characters,
 * unknown-section preservation) run on every platform. The
 * atomic-save/file test exercises POSIX-only behaviour (mode 0600,
 * fsync, no leaked .tmp) and is stubbed out on Windows.
 */

#include "test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/prefs_doc.h"

/* -------------------------------------------------------------------- */

int run_prefs_doc_roundtrip(void) {
    PrefsDoc *doc = prefsDocNew();
    UT_ASSERT(doc != NULL);
    UT_ASSERT_MSG(prefsDocVersion(doc) == 1,
                  "fresh doc version %d, expected 1", prefsDocVersion(doc));
    UT_ASSERT_MSG(!prefsDocIsDirty(doc), "fresh doc should not be dirty");

    UT_ASSERT(prefsDocSetString(doc, "SETTINGS", "Player Name", "Spectre"));
    UT_ASSERT(prefsDocSetString(doc, "SETTINGS", "Language", "en"));
    UT_ASSERT(prefsDocSetString(doc, "KEYS", "Forward", "273"));
    UT_ASSERT(prefsDocSetString(doc, "KEYS", "Shoot", "57"));
    UT_ASSERT(prefsDocSetString(doc, "MENU", "Frame Rate", "30"));
    UT_ASSERT_MSG(prefsDocIsDirty(doc), "doc should be dirty after SetString");

    char *json = prefsDocSerialize(doc);
    UT_ASSERT(json != NULL);

    PrefsDoc *re = prefsDocParseJson(json);
    UT_ASSERT(re != NULL);
    UT_ASSERT_MSG(!prefsDocIsDirty(re), "reparsed doc should not be dirty");
    UT_ASSERT_MSG(prefsDocVersion(re) == 1,
                  "reparsed version %d, expected 1", prefsDocVersion(re));

    char buf[128];
    prefsDocGetString(re, "SETTINGS", "Player Name", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Spectre") == 0, "Player Name: '%s'", buf);
    prefsDocGetString(re, "SETTINGS", "Language", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "en") == 0, "Language: '%s'", buf);
    prefsDocGetString(re, "KEYS", "Forward", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "273") == 0, "Forward: '%s'", buf);
    prefsDocGetString(re, "KEYS", "Shoot", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "57") == 0, "Shoot: '%s'", buf);
    prefsDocGetString(re, "MENU", "Frame Rate", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "30") == 0, "Frame Rate: '%s'", buf);

    free(json);
    prefsDocFree(doc);
    prefsDocFree(re);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_doc_defaults(void) {
    PrefsDoc *doc = prefsDocNew();
    UT_ASSERT(doc != NULL);

    char buf[64];
    unsigned long n;

    /* Missing section -> default. */
    n = prefsDocGetString(doc, "NOPE", "Key", "def", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "def") == 0, "missing section: '%s'", buf);
    UT_ASSERT_MSG(n == 3, "length %lu, expected 3", n);

    /* Present section, missing key -> default. */
    UT_ASSERT(prefsDocSetString(doc, "SETTINGS", "Present", "1"));
    prefsDocGetString(doc, "SETTINGS", "Missing", "def2", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "def2") == 0, "missing key: '%s'", buf);

    /* NULL default -> empty string. */
    n = prefsDocGetString(doc, "SETTINGS", "Missing", NULL, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "") == 0, "NULL default not empty: '%s'", buf);
    UT_ASSERT_MSG(n == 0, "NULL default length %lu, expected 0", n);

    n = prefsDocGetString(doc, "NOPE", "Key", NULL, buf, sizeof(buf));
    UT_ASSERT_MSG(buf[0] == '\0' && n == 0,
                  "NULL default on missing section: '%s' (%lu)", buf, n);

    prefsDocFree(doc);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_doc_special_chars(void) {
    /* Quote, backslash, newline, tab, and an embedded INI-injection
     * payload. None of this may corrupt the JSON or materialise a
     * spurious section on read-back. */
    const char *evil = "a\"b\\c\nd\te[OWNED]\nx=y";

    PrefsDoc *doc = prefsDocNew();
    UT_ASSERT(doc != NULL);
    UT_ASSERT(prefsDocSetString(doc, "SETTINGS", "Weird", evil));

    char *json = prefsDocSerialize(doc);
    UT_ASSERT(json != NULL);

    PrefsDoc *re = prefsDocParseJson(json);
    UT_ASSERT(re != NULL);

    char buf[128];
    prefsDocGetString(re, "SETTINGS", "Weird", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, evil) == 0,
                  "special value not byte-for-byte: '%s'", buf);

    /* The "[OWNED]\nx=y" inside the value must not become a section. */
    char buf2[64];
    prefsDocGetString(re, "OWNED", "x", "<none>", buf2, sizeof(buf2));
    UT_ASSERT_MSG(strcmp(buf2, "<none>") == 0,
                  "injection leaked an OWNED section: '%s'", buf2);

    free(json);
    prefsDocFree(doc);
    prefsDocFree(re);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_doc_unknown_preserved(void) {
    /* A _version newer than the module understands, plus a section/key
     * the module never writes. Both must survive a round-trip. */
    const char *src =
        "{\"_version\":99,"
        "\"SETTINGS\":{\"Player Name\":\"Me\"},"
        "\"FUTURE\":{\"newkey\":\"newval\"}}";

    PrefsDoc *doc = prefsDocParseJson(src);
    UT_ASSERT(doc != NULL);
    UT_ASSERT_MSG(prefsDocVersion(doc) == 99,
                  "version %d, expected 99", prefsDocVersion(doc));

    char buf[64];
    prefsDocGetString(doc, "SETTINGS", "Player Name", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Me") == 0, "known value: '%s'", buf);

    char *json = prefsDocSerialize(doc);
    UT_ASSERT(json != NULL);
    PrefsDoc *re = prefsDocParseJson(json);
    UT_ASSERT(re != NULL);

    prefsDocGetString(re, "FUTURE", "newkey", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "newval") == 0,
                  "unknown section/key dropped: '%s'", buf);
    UT_ASSERT_MSG(prefsDocVersion(re) == 99,
                  "version not preserved: %d", prefsDocVersion(re));

    free(json);
    prefsDocFree(doc);
    prefsDocFree(re);
    return 0;
}

/* -------------------------------------------------------------------- */

#ifdef _WIN32

int run_prefs_doc_save_atomic(void) { return 0; }

#else

#include <sys/stat.h>
#include <unistd.h>

int run_prefs_doc_save_atomic(void) {
    char jsonPath[1024];
    char tmpSibling[1100];

    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath), "prefs.json"));
    snprintf(tmpSibling, sizeof(tmpSibling), "%s.tmp", jsonPath);

    unlink(jsonPath);
    unlink(tmpSibling);

    /* Save a dirty doc atomically. */
    PrefsDoc *doc = prefsDocNew();
    UT_ASSERT(doc != NULL);
    UT_ASSERT(prefsDocSetString(doc, "SETTINGS", "Player Name", "Spectre"));
    UT_ASSERT(prefsDocSetString(doc, "KEYS", "Forward", "273"));
    UT_ASSERT(prefsDocIsDirty(doc));
    UT_ASSERT(prefsDocSave(doc, jsonPath));
    UT_ASSERT_MSG(!prefsDocIsDirty(doc), "dirty not cleared after save");

    struct stat st;
    UT_ASSERT_MSG(stat(jsonPath, &st) == 0, "saved file missing");
    UT_ASSERT_MSG((st.st_mode & 0777) == 0600,
                  "file mode %o, expected 0600", st.st_mode & 0777);
    UT_ASSERT_MSG(stat(tmpSibling, &st) != 0,
                  "atomic-write temp sibling leaked at %s", tmpSibling);

    /* Load it back. */
    PrefsDoc *loaded = prefsDocLoad(jsonPath);
    UT_ASSERT(loaded != NULL);
    UT_ASSERT_MSG(!prefsDocIsDirty(loaded), "loaded doc should not be dirty");
    char buf[64];
    prefsDocGetString(loaded, "SETTINGS", "Player Name", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Spectre") == 0, "loaded Player Name: '%s'", buf);
    prefsDocGetString(loaded, "KEYS", "Forward", "X", buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "273") == 0, "loaded Forward: '%s'", buf);

    prefsDocFree(doc);
    prefsDocFree(loaded);

    unlink(jsonPath);
    unlink(tmpSibling);
    return 0;
}

#endif /* !_WIN32 */
