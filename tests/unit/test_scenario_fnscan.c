/*
 * Finding the function definitions in a scenario script.
 *
 * The scanner reads text and answers names and line numbers. It links no
 * Lua and parses nothing, so every case here is a fixture written by the
 * case itself and an answer read straight back — no sim, no host, no VM.
 *
 * What it does not understand is as much the subject as what it does: a
 * definition inside a line comment is not one, and the block comments and
 * strings it cannot see are written down at the top of
 * mapeditor_scenario_fnscan.c rather than asserted here.
 *
 * run_scenario_fnscan_forms
 *      — the four spellings a definition takes, the comment that hides one,
 *        repeats, the bounds of the answer, and the names that are nobody's
 *        hook
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mapeditor_scenario_fnscan.h"
#include "test_harness.h"

/* The row at that index says this name on this line. Written as one helper
 * because every section below asks the same question of a different
 * fixture, and a failure has to say which row was wrong and what was in
 * it. */
static int sfsRowIs(const MEScnFoundFn *rows, size_t count, size_t at,
                    const char *name, int line) {
    if (at >= count) {
        fprintf(stderr, "        row %d was never written; %d rows came "
                        "back\n", (int)at, (int)count);
        return 0;
    }
    if (strcmp(rows[at].name, name) != 0 || rows[at].line != line) {
        fprintf(stderr, "        row %d is '%s' on line %d, expected '%s' on "
                        "line %d\n", (int)at, rows[at].name, rows[at].line,
                name, line);
        return 0;
    }
    return 1;
}

int run_scenario_fnscan_forms(void) {
    /* The four spellings that reach the host, one per line, each with a
       name of its own so a row taken from the wrong line cannot read as the
       right answer by coincidence. The qualified two are recorded under the
       bare name, which is the name the catalogue holds. */
    static const char *const kForms =
        "function on_setup()\n"              /* 1 */
        "end\n"                              /* 2 */
        "function scenario.on_start()\n"     /* 3 */
        "end\n"                              /* 4 */
        "on_tick = function(tick)\n"         /* 5 */
        "end\n"                              /* 6 */
        "scenario.on_end = function()\n"     /* 7 */
        "end\n";                             /* 8 */

    /* A definition behind a -- is not one, whatever else is on the line.
       The live definition two lines down is what says the scan carried on
       rather than stopping at the comment. */
    static const char *const kCommented =
        "-- function on_setup() end\n"       /* 1 */
        "   -- on_tick = function() end\n"   /* 2 */
        "function on_chat(p, text) end\n"    /* 3 */
        "function on_end() end -- and a note about it\n";  /* 4 */

    /* Indented, which is how a definition inside a block of any kind
       arrives, and how most authors write the second one. */
    static const char *const kIndented =
        "\tfunction on_lobby(p) end\n"       /* 1 */
        "        on_ping = function() end\n";/* 2 */

    /* The same name twice. Lua keeps the later one and says nothing, so
       both are reported and the author is the one who decides. */
    static const char *const kRepeated =
        "function on_tick(tick) end\n"       /* 1 */
        "function helper() end\n"            /* 2 */
        "function on_tick(tick) end\n";      /* 3 */

    /* Names no catalogue row holds. The scan answers what the file defines;
       what to do about a name that is nobody's hook is the view's
       business. */
    static const char *const kOwnNames =
        "local function helper() end\n"      /* 1 */
        "function my_own_thing() end\n"      /* 2 */
        "local later = function() end\n";    /* 3 */

    /* Neither of these is a definition: one hands a function over and the
       other gives one back. Both put the keyword on the line with no name
       assigned to it. */
    static const char *const kNotDefinitions =
        "game.timer(1, function() end)\n"    /* 1 */
        "local f = (function() return 1 end)\n";  /* 2 */

    MEScnFoundFn rows[16];
    char         longName[ME_SCN_FN_NAME_MAX + 8];
    char         longFixture[256];
    size_t       count;

    /* ── The four forms ──────────────────────────────────────────── */

    memset(rows, 0, sizeof(rows));
    count = meScnScanFunctions(kForms, rows, 16);
    UT_ASSERT_MSG(count == 4, "the four forms answered %d definitions",
                  (int)count);
    UT_ASSERT(sfsRowIs(rows, count, 0, "on_setup", 1));
    UT_ASSERT(sfsRowIs(rows, count, 1, "on_start", 3));
    UT_ASSERT(sfsRowIs(rows, count, 2, "on_tick",  5));
    UT_ASSERT(sfsRowIs(rows, count, 3, "on_end",   7));

    /* ── A comment is not a definition ───────────────────────────── */

    memset(rows, 0, sizeof(rows));
    count = meScnScanFunctions(kCommented, rows, 16);
    UT_ASSERT_MSG(count == 2,
                  "the commented fixture answered %d definitions, expected "
                  "the two that are not behind a --", (int)count);
    UT_ASSERT(sfsRowIs(rows, count, 0, "on_chat", 3));
    UT_ASSERT(sfsRowIs(rows, count, 1, "on_end",  4));

    /* ── Whitespace in front of it hides nothing ─────────────────── */

    memset(rows, 0, sizeof(rows));
    count = meScnScanFunctions(kIndented, rows, 16);
    UT_ASSERT_MSG(count == 2, "the indented fixture answered %d definitions",
                  (int)count);
    UT_ASSERT(sfsRowIs(rows, count, 0, "on_lobby", 1));
    UT_ASSERT(sfsRowIs(rows, count, 1, "on_ping",  2));

    /* ── Both of a repeated name, in file order ──────────────────── */

    memset(rows, 0, sizeof(rows));
    count = meScnScanFunctions(kRepeated, rows, 16);
    UT_ASSERT_MSG(count == 3, "the repeated fixture answered %d definitions",
                  (int)count);
    UT_ASSERT(sfsRowIs(rows, count, 0, "on_tick", 1));
    UT_ASSERT(sfsRowIs(rows, count, 1, "helper",  2));
    UT_ASSERT(sfsRowIs(rows, count, 2, "on_tick", 3));

    /* ── A name that is nobody's hook is still a definition ──────── */

    memset(rows, 0, sizeof(rows));
    count = meScnScanFunctions(kOwnNames, rows, 16);
    UT_ASSERT_MSG(count == 3, "the author's own names answered %d "
                              "definitions", (int)count);
    UT_ASSERT(sfsRowIs(rows, count, 0, "helper",        1));
    UT_ASSERT(sfsRowIs(rows, count, 1, "my_own_thing",  2));
    UT_ASSERT(sfsRowIs(rows, count, 2, "later",         3));

    /* ── A keyword with no name on it is not a definition ────────── */

    memset(rows, 0, sizeof(rows));
    count = meScnScanFunctions(kNotDefinitions, rows, 16);
    UT_ASSERT_MSG(count == 0,
                  "a function passed as an argument or returned answered %d "
                  "definitions, expected none", (int)count);

    /* ── A name too long to hold is left out, and only it ────────── */

    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    snprintf(longFixture, sizeof(longFixture),
             "function %s() end\nfunction after_it() end\n", longName);

    memset(rows, 0, sizeof(rows));
    count = meScnScanFunctions(longFixture, rows, 16);
    UT_ASSERT_MSG(count == 1,
                  "a name of %d bytes answered %d definitions, expected only "
                  "the one after it", (int)(sizeof(longName) - 1),
                  (int)count);
    UT_ASSERT(sfsRowIs(rows, count, 0, "after_it", 2));

    /* ── outMax is the end of it ─────────────────────────────────── */

    memset(rows, 0, sizeof(rows));
    rows[2].line = -1;              /* a mark the scan must not write over */
    count = meScnScanFunctions(kForms, rows, 2);
    UT_ASSERT_MSG(count == 2, "a fixture of four definitions answered %d "
                              "into room for two", (int)count);
    UT_ASSERT(sfsRowIs(rows, count, 0, "on_setup", 1));
    UT_ASSERT(sfsRowIs(rows, count, 1, "on_start", 3));
    UT_ASSERT_MSG(rows[2].line == -1 && rows[2].name[0] == '\0',
                  "the scan wrote past the room it was given");

    /* ── Nothing to read, or nowhere to put it ───────────────────── */

    memset(rows, 0, sizeof(rows));
    UT_ASSERT_MSG(meScnScanFunctions(NULL, rows, 16) == 0,
                  "a NULL text answered definitions");
    UT_ASSERT_MSG(meScnScanFunctions(kForms, NULL, 16) == 0,
                  "a NULL row array answered definitions");
    UT_ASSERT_MSG(meScnScanFunctions(kForms, rows, 0) == 0,
                  "no room answered definitions");
    UT_ASSERT_MSG(meScnScanFunctions("", rows, 16) == 0,
                  "an empty script answered definitions");

    return 0;
}
