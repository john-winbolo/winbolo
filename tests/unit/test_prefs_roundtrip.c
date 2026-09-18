/*
 * Round-trip tests for the process-global preferences API
 * (src/common/prefs.c): a write->read cycle through prefsSetString /
 * prefsGetString, plus default-value semantics for missing
 * keys/sections.
 *
 *   - prefs_document_roundtrip: a full prefs document round-trips key for
 *                               key, plus default-value semantics.
 *   - prefs_keys_roundtrip:     every [KEYS] binding gameFrontPutPrefs
 *                               persists survives a write->read cycle, and
 *                               an earlier write is not clobbered by a
 *                               later one.
 *
 * Stubbed out on Windows: the prefs document is file-backed and these
 * tests use /tmp paths.
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

#include "common/prefs.h"

/* Each test gets its own document from utScratchPath — private to this
 * process, so a second ctest run cannot land on the same file. The two
 * absolute /tmp paths that used to live here were shared by every checkout
 * on the machine, and two worktrees running this test at once failed it
 * about half the time. */
#define PREFS_LEAF "prefs.json"

/* Sentinel returned when a key/section is absent. Distinct from any
 * value written so a missing-vs-empty mix-up can't pass silently. */
#define UNSET "<unset>"

static void cleanup(const char *path) {
    /* Comfortably over the 1024 the callers give utScratchPath, plus the
     * longest suffix below. A scratch path carries the whole build
     * directory now, so the old 512 could have truncated and left the
     * sibling behind without saying so. */
    char sibling[1100];
    unlink(path);
    snprintf(sibling, sizeof(sibling), "%s.tmp", path);
    unlink(sibling);
    snprintf(sibling, sizeof(sibling), "%s.corrupt", path);
    unlink(sibling);
}

/* -------------------------------------------------------------------- */

int run_prefs_document_roundtrip(void) {
    char path[1024];

    UT_ASSERT(utScratchPath(path, sizeof(path), PREFS_LEAF));
    cleanup(path);
    prefsInit(path);

    /* The canonical prefs document: section/key/value triples written
     * through the prefs API, then read back and compared. */
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
        UT_ASSERT_MSG(prefsSetString(doc[i].section, doc[i].key, doc[i].value),
                      "write failed for [%s] %s", doc[i].section, doc[i].key);
    }

    /* Read every key back and assert it equals what was written. */
    char buf[128];
    for (i = 0; i < n; i++) {
        prefsGetString(doc[i].section, doc[i].key, UNSET, buf, sizeof(buf));
        UT_ASSERT_MSG(strcmp(buf, doc[i].value) == 0,
                      "round-trip mismatch for [%s] %s: wrote '%s', read '%s'",
                      doc[i].section, doc[i].key, doc[i].value, buf);
    }

    /* Default semantics: a missing key in an existing section returns the
     * supplied default. */
    prefsGetString("SETTINGS", "No Such Key", UNSET, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, UNSET) == 0,
                  "missing key should return default, got '%s'", buf);

    /* A missing section returns the supplied default. */
    prefsGetString("NO SUCH SECTION", "Player Name", UNSET, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, UNSET) == 0,
                  "missing section should return default, got '%s'", buf);

    prefsShutdown();
    cleanup(path);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_prefs_keys_roundtrip(void) {
    char path[1024];

    UT_ASSERT(utScratchPath(path, sizeof(path), PREFS_LEAF));
    cleanup(path);
    prefsInit(path);

    /* Every [KEYS] binding gameFrontPutPrefs persists. Each gets a
     * distinct numeric value so a write that clobbers an earlier key
     * shows up as a wrong value on read-back, not a silent pass. */
    const char *keys[] = {
        "Forward", "Backwards", "Left", "Right", "Shoot",
        "Lay Mine", "Increase Range", "Decrease Range",
        "Tank View", "Pill View", "Ally View", "Base View",
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
        UT_ASSERT_MSG(prefsSetString("KEYS", keys[i], value),
                      "write failed for [KEYS] %s", keys[i]);
    }

    /* ...then re-read each one (not just the last) to prove an earlier
     * write wasn't clobbered by a later one. */
    for (i = 0; i < n; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "%d", 100 + i);
        char buf[64];
        prefsGetString("KEYS", keys[i], UNSET, buf, sizeof(buf));
        UT_ASSERT_MSG(strcmp(buf, expected) == 0,
                      "binding lost or clobbered for [KEYS] %s: "
                      "wrote '%s', read '%s'", keys[i], expected, buf);
    }

    prefsShutdown();
    cleanup(path);
    return 0;
}

#endif /* !_WIN32 */
