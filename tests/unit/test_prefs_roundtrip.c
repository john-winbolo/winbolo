/*
 * Format-agnostic regression tests for the Profile API contract
 * (Get/WritePrivateProfileString) used to persist WinBolo preferences.
 *
 * These tests pin behaviour, not storage layout: they assert only what
 * comes back through a write->read round-trip and how missing
 * keys/sections/files resolve to the supplied default. They deliberately
 * make NO claim about the on-disk representation (no line counts, no byte
 * inspection, no [SECTION] checks, no file mode) so they stay green when
 * the backing store is swapped from INI to another format. The
 * INI-specific guarantees live in test_ini_reader_writer.c and stay there.
 *
 *   - prefs_document_roundtrip: a full prefs document round-trips key for
 *                               key, plus default-value semantics.
 *   - prefs_keys_roundtrip:     every [KEYS] binding gameFrontPutPrefs
 *                               persists survives a write->read cycle, and
 *                               an earlier write is not clobbered by a
 *                               later one.
 *
 * Stubbed out entirely on Windows (the Profile API there is the real
 * Win32 implementation; the hand-rolled stub only ships on POSIX).
 */

#include "test_harness.h"

#ifdef _WIN32

int run_prefs_document_roundtrip(void) { return 0; }
int run_prefs_keys_roundtrip(void)     { return 0; }

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posix_stubs.h"

/* Each test uses its own path so parallel test runs can't collide. */
static const char *PATH_DOCUMENT = "/tmp/winbolo_ut_prefs_document.ini";
static const char *PATH_KEYS     = "/tmp/winbolo_ut_prefs_keys.ini";

/* Sentinel returned when a key/section/file is absent. Distinct from any
 * value written so a missing-vs-empty mix-up can't pass silently. */
#define UNSET "<unset>"

static void read_back(const char *section, const char *key,
                      char *buf, size_t bufSize, const char *path) {
    GetPrivateProfileString(section, key, UNSET, buf, (DWORD)bufSize, path);
}

/* -------------------------------------------------------------------- */

int run_prefs_document_roundtrip(void) {
    unlink(PATH_DOCUMENT);

    /* The canonical prefs document: section/key/value triples written
     * through the Profile API, then read back and compared. */
    struct { const char *section; const char *key; const char *value; } doc[] = {
        { "SETTINGS",     "Player Name",          "Spectre"             },
        { "SETTINGS",     "Language",             "en"                  },
        { "SETTINGS",     "Remember Player Name", "Yes"                 },
        { "SETTINGS",     "Use UPnP",             "Yes"                 },
        { "KEYS",         "Forward",              "273"                 },
        { "KEYS",         "Backwards",            "274"                 },
        { "KEYS",         "Left",                 "276"                 },
        { "KEYS",         "Right",                "275"                 },
        { "KEYS",         "Shoot",                "57"                  },
        { "GAME OPTIONS", "Hidden Mines",         "No"                  },
        { "GAME OPTIONS", "Game Type",            "1"                   },
        { "TRACKER",      "Address",              "tracker.winbolo.net" },
        { "TRACKER",      "Port",                 "50000"               },
        { "TRACKER",      "Enabled",              "No"                  },
        { "MENU",         "Frame Rate",           "30"                  },
        { "MENU",         "Sound Effects",        "Yes"                 },
    };
    int n = (int)(sizeof(doc) / sizeof(doc[0]));

    int i;
    for (i = 0; i < n; i++) {
        UT_ASSERT_MSG(WritePrivateProfileString(doc[i].section, doc[i].key,
                                                 doc[i].value, PATH_DOCUMENT),
                      "write failed for [%s] %s", doc[i].section, doc[i].key);
    }

    /* Read every key back and assert it equals what was written. */
    char buf[128];
    for (i = 0; i < n; i++) {
        read_back(doc[i].section, doc[i].key, buf, sizeof(buf), PATH_DOCUMENT);
        UT_ASSERT_MSG(strcmp(buf, doc[i].value) == 0,
                      "round-trip mismatch for [%s] %s: wrote '%s', read '%s'",
                      doc[i].section, doc[i].key, doc[i].value, buf);
    }

    /* Default semantics: a missing key in an existing section returns the
     * supplied default. */
    read_back("SETTINGS", "No Such Key", buf, sizeof(buf), PATH_DOCUMENT);
    UT_ASSERT_MSG(strcmp(buf, UNSET) == 0,
                  "missing key should return default, got '%s'", buf);

    /* A missing section returns the supplied default. */
    read_back("NO SUCH SECTION", "Player Name", buf, sizeof(buf), PATH_DOCUMENT);
    UT_ASSERT_MSG(strcmp(buf, UNSET) == 0,
                  "missing section should return default, got '%s'", buf);

    /* Reading from a non-existent file returns the supplied default. */
    read_back("SETTINGS", "Player Name", buf, sizeof(buf),
              "/tmp/winbolo_ut_prefs_does_not_exist.ini");
    UT_ASSERT_MSG(strcmp(buf, UNSET) == 0,
                  "missing file should return default, got '%s'", buf);

    unlink(PATH_DOCUMENT);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_keys_roundtrip(void) {
    unlink(PATH_KEYS);

    /* Every [KEYS] binding gameFrontPutPrefs persists. Each gets a
     * distinct numeric value so a write that clobbers an earlier key
     * shows up as a wrong value on read-back, not a silent pass. */
    const char *keys[] = {
        "Forward", "Backwards", "Left", "Right", "Shoot",
        "Lay Mine", "Increase Range", "Decrease Range",
        "Tank View", "Pill View", "Ally View", "LGM View", "Base View",
        "Scroll Up", "Scroll Down", "Scroll Left", "Scroll Right",
        "Quick Tree", "Quick Road", "Quick Wall", "Quick Pillbox",
        "Quick Mine",
    };
    int n = (int)(sizeof(keys) / sizeof(keys[0]));

    /* Write all of them first... */
    int i;
    for (i = 0; i < n; i++) {
        char value[32];
        snprintf(value, sizeof(value), "%d", 100 + i);
        UT_ASSERT_MSG(WritePrivateProfileString("KEYS", keys[i], value, PATH_KEYS),
                      "write failed for [KEYS] %s", keys[i]);
    }

    /* ...then re-read each one (not just the last) to prove an earlier
     * write wasn't clobbered by a later one. */
    for (i = 0; i < n; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "%d", 100 + i);
        char buf[64];
        read_back("KEYS", keys[i], buf, sizeof(buf), PATH_KEYS);
        UT_ASSERT_MSG(strcmp(buf, expected) == 0,
                      "binding lost or clobbered for [KEYS] %s: "
                      "wrote '%s', read '%s'", keys[i], expected, buf);
    }

    unlink(PATH_KEYS);
    return 0;
}

#endif /* !_WIN32 */
