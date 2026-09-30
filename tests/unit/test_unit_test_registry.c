/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * THE TEST ROSTER IS WRITTEN DOWN TWICE, SO IT IS CHECKED.
 *
 * tests/unit/test_main.c holds the dispatch table: a name and the function
 * that runs it. CMakeLists.txt holds _unit_test_names: what CTest actually
 * runs. A case in the table but not in the CMake list compiles, links, passes
 * review, and is never run by anybody — which is what happened to
 * brain_list_texts_read and lobby_brain_docs_chunk_codec_roundtrip.
 *
 * So this case reads both files off disk (WB_REPO_ROOT_DIR names the tree the
 * binary was built from) and compares the two lists. Text, not symbols: the
 * dispatch table is file-static inside test_main.c, and CMakeLists.txt is not
 * C at all.
 *
 * TWO GROUPS ARE LEFT OUT OF THE CMAKE LIST ON PURPOSE and are not failures:
 *
 *   the fixture writers — wire_corpus_capture, wbv_v2_capture,
 *       wbv_summary_capture, spectator_seed_capture. Running one REWRITES a
 *       committed fixture, which is how a fixture is meant to be regenerated,
 *       so they stay dispatchable by name and out of the run-everything path.
 *       test_main.c's own s_fixtureWriters[] is the same list; this file
 *       parses it out of the source rather than keeping a third copy.
 *
 *   the netdebug cases — they sit behind #ifdef WB_NETDEBUG in the table and
 *       behind an if(WB_NETDEBUG) in CMake, so they are absent from the plain
 *       list by design. The parser skips anything inside that guard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"

#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

#define UTR_NAME_MAX  128
#define UTR_MAX_NAMES 4096

typedef struct {
    char  name[UTR_NAME_MAX];
} UtrName;

typedef struct {
    UtrName item[UTR_MAX_NAMES];
    int     count;
} UtrList;

static bool utrAdd(UtrList *l, const char *s, size_t len) {
    if (l->count >= UTR_MAX_NAMES || len == 0 || len >= UTR_NAME_MAX) return false;
    memcpy(l->item[l->count].name, s, len);
    l->item[l->count].name[len] = '\0';
    l->count++;
    return true;
}

static bool utrHas(const UtrList *l, const char *name) {
    int i;
    for (i = 0; i < l->count; i++) {
        if (strcmp(l->item[i].name, name) == 0) return true;
    }
    return false;
}

/* Read a whole text file into a malloc'd NUL-terminated buffer. */
static char *utrSlurp(const char *relPath) {
    char   path[1024];
    FILE  *f;
    long   n;
    char  *buf;
    size_t got;

    snprintf(path, sizeof(path), "%s/%s", WB_REPO_ROOT_DIR, relPath);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* A C identifier character, for the name bodies both files carry. */
static bool utrIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Every `{ "name",` row of test_main.c's dispatch table, minus anything
 * inside an #ifdef WB_NETDEBUG block. Also fills `writers` from the
 * s_fixtureWriters[] array, which is the deliberate-omission list. */
static bool utrParseTestMain(const char *src, UtrList *names, UtrList *writers) {
    const char *p = src;
    const char *writersStart = strstr(src, "s_fixtureWriters[] = {");
    int         netdebugDepth = 0;   /* >0 while inside #ifdef WB_NETDEBUG */
    int         ifDepth = 0;         /* nesting inside that block */

    /* The fixture-writer list first: a run of "name" string literals up to
     * the closing brace. */
    if (writersStart != NULL) {
        const char *q = writersStart;
        const char *end = strchr(q, '}');
        while (end != NULL && q < end) {
            const char *s = strchr(q, '"');
            if (s == NULL || s > end) break;
            {
                const char *e = strchr(s + 1, '"');
                if (e == NULL || e > end) break;
                utrAdd(writers, s + 1, (size_t)(e - s - 1));
                q = e + 1;
            }
        }
    }

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t      lineLen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        /* Directive tracking, so the netdebug rows can be skipped. */
        {
            const char *t = p;
            while (t < p + lineLen && (*t == ' ' || *t == '\t')) t++;
            if (t < p + lineLen && *t == '#') {
                if (strncmp(t, "#ifdef WB_NETDEBUG", 18) == 0) {
                    netdebugDepth = 1;
                    ifDepth = 0;
                } else if (netdebugDepth > 0 &&
                           (strncmp(t, "#if", 3) == 0)) {
                    ifDepth++;
                } else if (netdebugDepth > 0 && strncmp(t, "#endif", 6) == 0) {
                    if (ifDepth > 0) ifDepth--;
                    else             netdebugDepth = 0;
                }
            }
        }

        if (netdebugDepth == 0) {
            /* A table row reads:  { "name", run_name }  — possibly with the
             * function on the next line. The opening brace then a quote,
             * with only spaces between, is what names a row. */
            const char *t = p;
            while (t < p + lineLen && (*t == ' ' || *t == '\t')) t++;
            if (t < p + lineLen && *t == '{') {
                const char *q = t + 1;
                while (q < p + lineLen && (*q == ' ' || *q == '\t')) q++;
                if (q < p + lineLen && *q == '"') {
                    const char *e = strchr(q + 1, '"');
                    if (e != NULL && e < p + lineLen) {
                        if (!utrAdd(names, q + 1, (size_t)(e - q - 1))) {
                            fprintf(stderr, "test_main roster overflowed\n");
                            return false;
                        }
                    }
                }
            }
        }

        if (eol == NULL) break;
        p = eol + 1;
    }
    return true;
}

/* Every bare name inside set(_unit_test_names ... ) in CMakeLists.txt. */
static bool utrParseCMake(const char *src, UtrList *names) {
    const char *p = strstr(src, "set(_unit_test_names");
    if (p == NULL) {
        fprintf(stderr, "set(_unit_test_names not found in CMakeLists.txt\n");
        return false;
    }
    p = strchr(p, '\n');
    if (p == NULL) return false;
    p++;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t      lineLen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        const char *t = p;
        const char *e;

        while (t < p + lineLen && (*t == ' ' || *t == '\t' || *t == '\r')) t++;
        if (t < p + lineLen && *t == ')') return true;        /* list closed */
        if (t < p + lineLen && *t != '#' && t != p + lineLen) {
            e = t;
            while (e < p + lineLen && utrIdentChar(*e)) e++;
            if (e > t) {
                if (!utrAdd(names, t, (size_t)(e - t))) {
                    fprintf(stderr, "cmake roster overflowed\n");
                    return false;
                }
            }
        }
        if (eol == NULL) break;
        p = eol + 1;
    }
    fprintf(stderr, "set(_unit_test_names was never closed\n");
    return false;
}

int run_unit_test_names_match_cmake(void) {
    char    *mainSrc  = utrSlurp("tests/unit/test_main.c");
    char    *cmakeSrc = utrSlurp("CMakeLists.txt");
    UtrList *inMain   = NULL;
    UtrList *inCMake  = NULL;
    UtrList *writers  = NULL;
    int      i;
    int      missing = 0, stray = 0, dupes = 0;
    int      rc = 0;

    if (mainSrc == NULL || cmakeSrc == NULL) {
        fprintf(stderr,
                "could not read the two rosters out of %s — is "
                "WB_REPO_ROOT_DIR right?\n", WB_REPO_ROOT_DIR);
        rc = 1;
        goto done;
    }

    inMain  = (UtrList *)calloc(1, sizeof(*inMain));
    inCMake = (UtrList *)calloc(1, sizeof(*inCMake));
    writers = (UtrList *)calloc(1, sizeof(*writers));
    if (inMain == NULL || inCMake == NULL || writers == NULL) { rc = 1; goto done; }

    if (!utrParseTestMain(mainSrc, inMain, writers)) { rc = 1; goto done; }
    if (!utrParseCMake(cmakeSrc, inCMake))           { rc = 1; goto done; }

    UT_ASSERT_MSG(inMain->count > 500,
                  "only %d rows parsed out of test_main.c's table — the "
                  "parser has lost the shape of the file", inMain->count);
    UT_ASSERT_MSG(inCMake->count > 500,
                  "only %d names parsed out of _unit_test_names — the parser "
                  "has lost the shape of the list", inCMake->count);
    UT_ASSERT_MSG(writers->count >= 4,
                  "only %d fixture writers parsed out of s_fixtureWriters[] — "
                  "the deliberate-omission list is what keeps this case from "
                  "crying wolf", writers->count);

    /* In the table, not in the list: compiled, linked, never run. */
    for (i = 0; i < inMain->count; i++) {
        const char *n = inMain->item[i].name;
        if (utrHas(writers, n)) continue;          /* rewrites a fixture */
        if (!utrHas(inCMake, n)) {
            fprintf(stderr,
                    "test_main.c has \"%s\" but _unit_test_names does not: "
                    "CTest never runs it\n", n);
            missing++;
        }
    }

    /* In the list, not in the table: CTest would ask for a name the binary
     * cannot dispatch. */
    for (i = 0; i < inCMake->count; i++) {
        const char *n = inCMake->item[i].name;
        if (!utrHas(inMain, n)) {
            fprintf(stderr,
                    "_unit_test_names has \"%s\" but test_main.c's table does "
                    "not: that CTest row can only fail\n", n);
            stray++;
        }
    }

    /* One name, two rows — a copy/paste that quietly runs a case twice or
     * shadows a different one. */
    for (i = 0; i < inMain->count; i++) {
        int j;
        for (j = i + 1; j < inMain->count; j++) {
            if (strcmp(inMain->item[i].name, inMain->item[j].name) == 0) {
                fprintf(stderr,
                        "test_main.c's table lists \"%s\" twice\n",
                        inMain->item[i].name);
                dupes++;
                break;
            }
        }
    }

    UT_ASSERT_MSG(missing == 0,
                  "%d test case(s) are in test_main.c and not in "
                  "CMakeLists.txt's _unit_test_names, so nothing runs them "
                  "(named above)", missing);
    UT_ASSERT_MSG(stray == 0,
                  "%d name(s) in _unit_test_names have no row in "
                  "test_main.c's table (named above)", stray);
    UT_ASSERT_MSG(dupes == 0,
                  "%d duplicate name(s) in test_main.c's table (named above)",
                  dupes);

done:
    free(mainSrc);
    free(cmakeSrc);
    free(inMain);
    free(inCMake);
    free(writers);
    return rc;
}
