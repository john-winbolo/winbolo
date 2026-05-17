/*
 * Tests for the POSIX INI reader/writer in src/server/posix_stubs.c.
 *
 * Three regression tests, one per bug fixed during the recent prefs
 * persistence work:
 *
 *   - ini_writer_persistence:     no-doubling-cascade + no fixed line cap
 *                                 (commit "Fix POSIX INI writer ...")
 *   - ini_writer_insertion_point: new key lands under its own section,
 *                                 not at end-of-file
 *   - ini_writer_security:        \n/\r/= rejection, NULL handling,
 *                                 mode 0600, atomic temp file
 *                                 (commit "Harden POSIX INI writer ...")
 *
 * Stubbed out entirely on Windows (the INI writer there is the real
 * Win32 API; this hand-rolled stub only ships on POSIX).
 */

#include "test_harness.h"

#ifdef _WIN32

int run_ini_writer_persistence(void)     { return 0; }
int run_ini_writer_insertion_point(void) { return 0; }
int run_ini_writer_security(void)        { return 0; }

#else

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix_stubs.h"

/* Each test uses its own path so parallel test runs can't collide. */
static const char *PATH_PERSIST    = "/tmp/winbolo_ut_ini_persist.ini";
static const char *PATH_INSERTION  = "/tmp/winbolo_ut_ini_insertion.ini";
static const char *PATH_SECURITY   = "/tmp/winbolo_ut_ini_security.ini";

static int count_lines(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int n = 0;
    int c, last = '\n';
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') n++;
        last = c;
    }
    if (last != '\n' && last != EOF) n++;  /* trailing line without newline */
    fclose(fp);
    return n;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static int read_back(const char *section, const char *key,
                     char *buf, size_t bufSize, const char *path) {
    return (int)GetPrivateProfileString(section, key, "<default>",
                                        buf, (DWORD)bufSize, path);
}

/* -------------------------------------------------------------------- */

int run_ini_writer_persistence(void) {
    unlink(PATH_PERSIST);

    /* Round-trip: write a key, read it back. */
    UT_ASSERT(WritePrivateProfileString("AUTH", "Token", "deadbeef", PATH_PERSIST));
    char buf[128];
    read_back("AUTH", "Token", buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "deadbeef") == 0,
                  "round-trip mismatch: got '%s'", buf);

    /* Missing key returns default. */
    read_back("AUTH", "MissingKey", buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "<default>") == 0,
                  "missing key should return default, got '%s'", buf);

    /* Missing section returns default. */
    read_back("NOPE", "Token", buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "<default>") == 0,
                  "missing section should return default, got '%s'", buf);

    /* Build a small but multi-section file like the real WinBolo INI. */
    UT_ASSERT(WritePrivateProfileString("SETTINGS", "Player Name",   "Me",        PATH_PERSIST));
    UT_ASSERT(WritePrivateProfileString("SETTINGS", "Target Address", "127.0.0.1", PATH_PERSIST));
    UT_ASSERT(WritePrivateProfileString("KEYS",     "Forward",        "8",         PATH_PERSIST));
    UT_ASSERT(WritePrivateProfileString("KEYS",     "Backward",       "7",         PATH_PERSIST));
    UT_ASSERT(WritePrivateProfileString("MENU",     "Window X",       "100",       PATH_PERSIST));
    UT_ASSERT(WritePrivateProfileString("MENU",     "Window Y",       "200",       PATH_PERSIST));

    int baselineLines = count_lines(PATH_PERSIST);
    UT_ASSERT(baselineLines > 0);

    /* Regression for the doubling-cascade bug: repeatedly updating
     * existing keys must NOT grow the file. The old writer added a
     * blank line per file-line after each matched key on every call,
     * so the file grew geometrically and eventually overflowed the
     * 512-line stack array. */
    int i;
    for (i = 0; i < 100; i++) {
        UT_ASSERT(WritePrivateProfileString("SETTINGS", "Player Name",   "Me",        PATH_PERSIST));
        UT_ASSERT(WritePrivateProfileString("MENU",     "Window X",       "100",       PATH_PERSIST));
        UT_ASSERT(WritePrivateProfileString("KEYS",     "Forward",        "8",         PATH_PERSIST));
    }
    int afterLines = count_lines(PATH_PERSIST);
    UT_ASSERT_MSG(afterLines == baselineLines,
                  "file grew under repeated updates: %d -> %d lines",
                  baselineLines, afterLines);

    /* Values still readable after the update storm. */
    read_back("MENU", "Window X", buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "100") == 0, "Window X lost after updates: '%s'", buf);
    read_back("KEYS", "Forward", buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "8") == 0, "Forward lost after updates: '%s'", buf);

    /* Regression for the 512-line cap: write more keys than the old
     * stack array could hold, and verify both the first and last keys
     * survive a re-read. */
    for (i = 0; i < 1000; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "Big%d", i);
        snprintf(v, sizeof(v), "val%d", i);
        UT_ASSERT(WritePrivateProfileString("BULK", k, v, PATH_PERSIST));
    }
    UT_ASSERT(count_lines(PATH_PERSIST) > 1000);

    read_back("BULK", "Big0",   buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "val0") == 0,   "first bulk key lost: '%s'", buf);
    read_back("BULK", "Big999", buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "val999") == 0, "last bulk key lost: '%s'", buf);

    /* Earlier sections still intact after the bulk write. */
    read_back("SETTINGS", "Player Name", buf, sizeof(buf), PATH_PERSIST);
    UT_ASSERT_MSG(strcmp(buf, "Me") == 0, "SETTINGS lost after bulk: '%s'", buf);

    unlink(PATH_PERSIST);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_ini_writer_insertion_point(void) {
    unlink(PATH_INSERTION);

    /* Build a file with three sections, in order [A] [B] [C]. */
    UT_ASSERT(WritePrivateProfileString("A", "a1", "1", PATH_INSERTION));
    UT_ASSERT(WritePrivateProfileString("B", "b1", "1", PATH_INSERTION));
    UT_ASSERT(WritePrivateProfileString("C", "c1", "1", PATH_INSERTION));

    /* Add a NEW key under [A]. Old code would have appended at file end
     * (under [C]); the next read couldn't find it under [A], so on the
     * next launch the same write happened again and duplicates piled up. */
    UT_ASSERT(WritePrivateProfileString("A", "a2", "2", PATH_INSERTION));

    char buf[64];
    read_back("A", "a2", buf, sizeof(buf), PATH_INSERTION);
    UT_ASSERT_MSG(strcmp(buf, "2") == 0,
                  "new key not findable under its section: got '%s'", buf);

    /* The repeat-on-every-launch failure mode: write the same new key
     * again, then read it. If it landed under [C], the reader would
     * not find it, so we'd write a duplicate, growing the file. */
    long sizeBefore = file_size(PATH_INSERTION);
    UT_ASSERT(WritePrivateProfileString("A", "a2", "2", PATH_INSERTION));
    UT_ASSERT(WritePrivateProfileString("A", "a2", "2", PATH_INSERTION));
    long sizeAfter = file_size(PATH_INSERTION);
    UT_ASSERT_MSG(sizeAfter == sizeBefore,
                  "duplicate insertions grew file: %ld -> %ld",
                  sizeBefore, sizeAfter);

    /* Read every original key — none got reordered out of its section. */
    read_back("A", "a1", buf, sizeof(buf), PATH_INSERTION);
    UT_ASSERT(strcmp(buf, "1") == 0);
    read_back("B", "b1", buf, sizeof(buf), PATH_INSERTION);
    UT_ASSERT(strcmp(buf, "1") == 0);
    read_back("C", "c1", buf, sizeof(buf), PATH_INSERTION);
    UT_ASSERT(strcmp(buf, "1") == 0);

    /* Inserting into a section the reader should still find above
     * any later section: spot-check by writing a new key under [B]
     * and confirming [C]'s key is still in [C], not under [B]. */
    UT_ASSERT(WritePrivateProfileString("B", "b2", "2", PATH_INSERTION));
    read_back("B", "b2", buf, sizeof(buf), PATH_INSERTION);
    UT_ASSERT(strcmp(buf, "2") == 0);
    read_back("C", "c1", buf, sizeof(buf), PATH_INSERTION);
    UT_ASSERT_MSG(strcmp(buf, "1") == 0, "C's key wandered: '%s'", buf);

    unlink(PATH_INSERTION);
    return 0;
}

/* -------------------------------------------------------------------- */

int run_ini_writer_security(void) {
    unlink(PATH_SECURITY);

    /* Establish a baseline. */
    UT_ASSERT(WritePrivateProfileString("AUTH", "Token", "good", PATH_SECURITY));
    long sizeClean = file_size(PATH_SECURITY);

    /* Newline in value rejected — file content unchanged. */
    UT_ASSERT(WritePrivateProfileString("AUTH", "Token",
                                        "evil\n[OWNED]\nx=y", PATH_SECURITY) == 0);
    UT_ASSERT_MSG(file_size(PATH_SECURITY) == sizeClean,
                  "file changed after rejected newline write");
    char buf[128];
    read_back("AUTH", "Token", buf, sizeof(buf), PATH_SECURITY);
    UT_ASSERT_MSG(strcmp(buf, "good") == 0,
                  "Token clobbered by rejected write: '%s'", buf);
    read_back("OWNED", "x", buf, sizeof(buf), PATH_SECURITY);
    UT_ASSERT_MSG(strcmp(buf, "<default>") == 0,
                  "injected [OWNED] section materialised: '%s'", buf);

    /* CR in value rejected. */
    UT_ASSERT(WritePrivateProfileString("AUTH", "Token", "ev\ril", PATH_SECURITY) == 0);

    /* = in key rejected (would split key=value on read-back). */
    UT_ASSERT(WritePrivateProfileString("AUTH", "k=ey", "x", PATH_SECURITY) == 0);

    /* [ or ] in section rejected (would let value pretend to be a header). */
    UT_ASSERT(WritePrivateProfileString("AU]TH", "k", "v", PATH_SECURITY) == 0);
    UT_ASSERT(WritePrivateProfileString("[AUTH", "k", "v", PATH_SECURITY) == 0);

    /* NULL section/key/filePath rejected. */
    UT_ASSERT(WritePrivateProfileString(NULL,   "k", "v", PATH_SECURITY) == 0);
    UT_ASSERT(WritePrivateProfileString("AUTH", NULL, "v", PATH_SECURITY) == 0);
    UT_ASSERT(WritePrivateProfileString("AUTH", "k",  "v", NULL)         == 0);

    /* NULL value treated as empty string, not a crash. Round-trip empty. */
    UT_ASSERT(WritePrivateProfileString("AUTH", "Empty", NULL, PATH_SECURITY));
    read_back("AUTH", "Empty", buf, sizeof(buf), PATH_SECURITY);
    UT_ASSERT_MSG(strcmp(buf, "") == 0, "NULL value didn't read back empty: '%s'", buf);

    /* Mode must be 0600 — auth token shouldn't be world-readable. */
    struct stat st;
    UT_ASSERT(stat(PATH_SECURITY, &st) == 0);
    UT_ASSERT_MSG((st.st_mode & 0777) == 0600,
                  "INI file mode is %o, expected 0600", st.st_mode & 0777);

    /* Even if a previous run left a permissive mode, a fresh write
     * should bring it back to 0600 (defensive chmod path). */
    chmod(PATH_SECURITY, 0644);
    UT_ASSERT(WritePrivateProfileString("AUTH", "Token", "good2", PATH_SECURITY));
    UT_ASSERT(stat(PATH_SECURITY, &st) == 0);
    UT_ASSERT_MSG((st.st_mode & 0777) == 0600,
                  "mode not re-asserted after rewrite: got %o", st.st_mode & 0777);

    /* Atomic-write contract: the sibling .tmp must not leak on success. */
    char tmpPath[1024];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", PATH_SECURITY);
    UT_ASSERT_MSG(!file_exists(tmpPath),
                  "atomic-write temp file leaked at %s", tmpPath);

    unlink(PATH_SECURITY);
    return 0;
}

#endif /* !_WIN32 */
