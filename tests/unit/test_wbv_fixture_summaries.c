/*
 * Recorded-run replay fixtures. Each <name>.wbv listed below was written by
 * a WinBoloHeadless --record run (see the run_changes scenarios in
 * tests/baseline/run.sh) and lives in tests/fixtures/wbv beside a committed
 * <name>.summary. wbv_fixture_summaries decodes every .wbv through the
 * production log-viewer reader, writes its summary with
 * replayHarnessWriteSummary and requires it to match the committed summary
 * byte for byte, printing the two as a unified-style diff when it does not.
 *
 * wbv_summary_capture (re)writes the .summary files from the .wbv files and
 * is dispatch-only, never run under CTest:
 *
 *   WB_WBV_FIXTURE_DIR=tests/fixtures/wbv \
 *       ./WinBoloUnitTests --test wbv_summary_capture
 *
 * To refresh a .wbv itself, run its scenario through tests/baseline/run.sh
 * and copy tests/baseline/actual/<name>.wbv over the fixture, then capture.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay_harness.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

static const char *const s_fixtures[] = {
    "road_spit_shell_open",
};
#define NUM_FIXTURES (sizeof(s_fixtures) / sizeof(s_fixtures[0]))

static const char *fixtureDir(void) {
    const char *dir = getenv("WB_WBV_FIXTURE_DIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = WB_WBV_FIXTURE_DIR;
    }
    return dir;
}

/* Whole file as a NUL-terminated buffer, or NULL when it cannot be read.
 * *len receives the byte count without the terminator. */
static char *readAll(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *) malloc((size_t) sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t) sz, f) != (size_t) sz) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *len = (size_t) sz;
    return buf;
}

/* Decode <dir>/<name>.wbv and return its summary text, or NULL when the
 * decode fails. The summary goes through a scratch file in the working
 * directory so the writer only ever sees a FILE. */
static char *summarize(const char *dir, const char *name, size_t *len) {
    char wbvPath[512];
    char tmpPath[512];
    ReplayWorld *w;
    ReplayFileInfo info;
    FILE *f;
    char *text;

    snprintf(wbvPath, sizeof(wbvPath), "%s/%s.wbv", dir, name);
    snprintf(tmpPath, sizeof(tmpPath), "wbv_summary_%s.tmp", name);

    w = (ReplayWorld *) malloc(sizeof(ReplayWorld));
    if (w == NULL) {
        return NULL;
    }
    if (!replayHarnessDecodeFile(wbvPath, w, &info)) {
        free(w);
        return NULL;
    }
    f = fopen(tmpPath, "wb");
    if (f == NULL) {
        free(w);
        return NULL;
    }
    replayHarnessWriteSummary(w, &info, f);
    fclose(f);
    free(w);

    text = readAll(tmpPath, len);
    remove(tmpPath);
    return text;
}

/* Print expected and actual as a unified-style diff: matching lines with a
 * leading space, differing lines as - and + pairs. */
static void printDiff(const char *expected, const char *actual) {
    const char *e = expected;
    const char *a = actual;
    fprintf(stderr, "--- expected\n+++ actual\n");
    while (*e != '\0' || *a != '\0') {
        const char *eEnd = strchr(e, '\n');
        const char *aEnd = strchr(a, '\n');
        size_t eLen = eEnd != NULL ? (size_t) (eEnd - e) : strlen(e);
        size_t aLen = aEnd != NULL ? (size_t) (aEnd - a) : strlen(a);
        if (*e != '\0' && *a != '\0' && eLen == aLen && memcmp(e, a, eLen) == 0) {
            fprintf(stderr, " %.*s\n", (int) eLen, e);
        } else {
            if (*e != '\0') fprintf(stderr, "-%.*s\n", (int) eLen, e);
            if (*a != '\0') fprintf(stderr, "+%.*s\n", (int) aLen, a);
        }
        e = *e != '\0' ? (eEnd != NULL ? eEnd + 1 : e + eLen) : e;
        a = *a != '\0' ? (aEnd != NULL ? aEnd + 1 : a + aLen) : a;
    }
}

int run_wbv_fixture_summaries(void) {
    const char *dir = fixtureDir();
    size_t i;

    for (i = 0; i < NUM_FIXTURES; i++) {
        char summaryPath[512];
        size_t actualLen = 0;
        size_t expectedLen = 0;
        char *actual;
        char *expected;
        bool same;

        snprintf(summaryPath, sizeof(summaryPath), "%s/%s.summary", dir,
                 s_fixtures[i]);
        actual = summarize(dir, s_fixtures[i], &actualLen);
        UT_ASSERT_MSG(actual != NULL, "cannot decode %s/%s.wbv", dir,
                      s_fixtures[i]);
        expected = readAll(summaryPath, &expectedLen);
        if (expected == NULL) {
            free(actual);
            UT_FAIL("summary missing: %s — generate it with: "
                    "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test "
                    "wbv_summary_capture", summaryPath, dir);
        }
        same = actualLen == expectedLen &&
               memcmp(actual, expected, actualLen) == 0;
        if (!same) {
            printDiff(expected, actual);
        }
        free(actual);
        free(expected);
        UT_ASSERT_MSG(same, "summary of %s.wbv differs from %s", s_fixtures[i],
                      summaryPath);
    }
    return 0;
}

int run_wbv_summary_capture(void) {
    const char *dir = fixtureDir();
    size_t i;

    for (i = 0; i < NUM_FIXTURES; i++) {
        char summaryPath[512];
        size_t len = 0;
        char *text;
        FILE *f;

        snprintf(summaryPath, sizeof(summaryPath), "%s/%s.summary", dir,
                 s_fixtures[i]);
        text = summarize(dir, s_fixtures[i], &len);
        UT_ASSERT_MSG(text != NULL, "cannot decode %s/%s.wbv", dir,
                      s_fixtures[i]);
        f = fopen(summaryPath, "wb");
        if (f == NULL) {
            free(text);
            UT_FAIL("cannot write %s", summaryPath);
        }
        fwrite(text, 1, len, f);
        fclose(f);
        free(text);
        fprintf(stderr, "wbv_summary_capture: wrote %zu bytes -> %s\n", len,
                summaryPath);
    }
    return 0;
}
