/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The quoting the viewer's macOS and Linux file dialogs put around text
 * before it goes to the shell or to AppleScript (lv_shell_quote.h).
 *
 *   lv_shell_quote — a single quote (O'Brien), a backquote, $(...), a
 *       backslash and a double quote each come out as plain text in one
 *       shell word and in one AppleScript string; a result too long for the
 *       buffer gives false and an empty buffer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lv_shell_quote.h"
#include "test_harness.h"

#define SQ_EXPECT(fn, in, want)                                                \
    do {                                                                       \
        char got_[256];                                                        \
        UT_ASSERT_MSG(fn(got_, sizeof(got_), in), "%s failed on \"%s\"", #fn,  \
                      in);                                                     \
        UT_ASSERT_MSG(strcmp(got_, want) == 0, "%s: got [%s], want [%s]", #fn, \
                      got_, want);                                             \
    } while (0)

int run_lv_shell_quote(void) {
    /* Shell: in single quotes; a single quote ends the quoted part, comes
       after a backslash and a new quoted part starts. Nothing else changes,
       because inside single quotes no character has a special meaning. */
    SQ_EXPECT(lvShellQuote, "plain", "'plain'");
    SQ_EXPECT(lvShellQuote, "", "''");
    SQ_EXPECT(lvShellQuote, "O'Brien", "'O'\\''Brien'");
    SQ_EXPECT(lvShellQuote, "a`id`b", "'a`id`b'");
    SQ_EXPECT(lvShellQuote, "$(rm -rf ~)", "'$(rm -rf ~)'");
    SQ_EXPECT(lvShellQuote, "back\\slash", "'back\\slash'");
    SQ_EXPECT(lvShellQuote, "say \"hi\"", "'say \"hi\"'");
    SQ_EXPECT(lvShellQuote, "''", "''\\'''\\'''");
    SQ_EXPECT(lvShellQuote, "-x.wbv", "'-x.wbv'");

    /* A file name that is an argument of its own: "./" before a leading
       '-', and the rest quoted as before. */
    SQ_EXPECT(lvShellQuoteFileArg, "-x.wbv", "'./-x.wbv'");
    SQ_EXPECT(lvShellQuoteFileArg, "--help O'Brien.wbv", "'./--help O'\\''Brien.wbv'");
    SQ_EXPECT(lvShellQuoteFileArg, "a-b.wbv", "'a-b.wbv'");
    SQ_EXPECT(lvShellQuoteFileArg, "", "''");

    /* AppleScript: in double quotes; a backslash and a double quote come
       after a backslash. A single quote, a backquote and $ are plain text
       in an AppleScript string. */
    SQ_EXPECT(lvAppleScriptQuote, "plain", "\"plain\"");
    SQ_EXPECT(lvAppleScriptQuote, "O'Brien", "\"O'Brien\"");
    SQ_EXPECT(lvAppleScriptQuote, "a`id`b", "\"a`id`b\"");
    SQ_EXPECT(lvAppleScriptQuote, "$(rm -rf ~)", "\"$(rm -rf ~)\"");
    SQ_EXPECT(lvAppleScriptQuote, "back\\slash", "\"back\\\\slash\"");
    SQ_EXPECT(lvAppleScriptQuote, "say \"hi\"", "\"say \\\"hi\\\"\"");

    /* The two together, as the osascript command uses them: the script
       holds the AppleScript string and is then one shell word. */
    {
        char as[128], sh[256];
        UT_ASSERT(lvAppleScriptQuote(as, sizeof(as), "O'Brien \"x\" $(id)"));
        UT_ASSERT(lvShellQuote(sh, sizeof(sh), as));
        UT_ASSERT_MSG(strcmp(sh, "'\"O'\\''Brien \\\"x\\\" $(id)\"'") == 0,
                      "got [%s]", sh);
    }

    /* Too long for the buffer: false and an empty buffer, never a cut
       string that the shell could read in another way. */
    {
        char small[6];
        memset(small, 'x', sizeof(small));
        UT_ASSERT(!lvShellQuote(small, sizeof(small), "O'Brien"));
        UT_ASSERT(small[0] == '\0');
        memset(small, 'x', sizeof(small));
        UT_ASSERT(!lvAppleScriptQuote(small, sizeof(small), "abcdef"));
        UT_ASSERT(small[0] == '\0');
        /* An exact fit: 'abc' and the end byte. */
        UT_ASSERT(lvShellQuote(small, sizeof(small), "abc"));
        UT_ASSERT(strcmp(small, "'abc'") == 0);
    }
    return 0;
}
