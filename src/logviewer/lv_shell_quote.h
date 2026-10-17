/*
 * lv_shell_quote.h - put text safely into a shell command or an AppleScript
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * On macOS and Linux the viewer's file dialogs run zenity, kdialog or
 * osascript through popen, and so through /bin/sh. The name the Save Log
 * dialog offers comes from player names in the log, and a log can come from
 * anyone. Each piece of text that goes into such a command goes through
 * these functions first, so that the shell and AppleScript read it as text
 * only. Header only, so the unit tests can check them on every system
 * (tests/unit/test_lv_shell_quote.c).
 *
 *   lvShellQuote - the text as one shell word: in single quotes, with each
 *       single quote in it written as '\''. Inside single quotes the shell
 *       gives no other character a special meaning ($, `, \, ").
 *   lvAppleScriptQuote - the text as one AppleScript string: in double
 *       quotes, with each backslash and double quote after a backslash.
 *
 * Both write into the caller's buffer and give true, or give false and an
 * empty buffer when the result does not fit. A caller that gets false must
 * not run the command.
 */

#ifndef LV_SHELL_QUOTE_H
#define LV_SHELL_QUOTE_H

#include <stdbool.h>
#include <stddef.h>

static inline bool lvShellQuote(char *out, size_t size, const char *text) {
    size_t n = 0;
    if (!out || size == 0) return false;
    if (!text) text = "";
#define LV_SQ_PUT(ch)                                                          \
    do {                                                                       \
        if (n + 1 >= size) { out[0] = '\0'; return false; }                    \
        out[n++] = (ch);                                                       \
    } while (0)
    LV_SQ_PUT('\'');
    for (const char *p = text; *p; p++) {
        if (*p == '\'') {
            /* End the quoted part, put a quote after a backslash, start a
               new quoted part. */
            LV_SQ_PUT('\'');
            LV_SQ_PUT('\\');
            LV_SQ_PUT('\'');
            LV_SQ_PUT('\'');
        } else {
            LV_SQ_PUT(*p);
        }
    }
    LV_SQ_PUT('\'');
#undef LV_SQ_PUT
    out[n] = '\0';
    return true;
}

static inline bool lvAppleScriptQuote(char *out, size_t size, const char *text) {
    size_t n = 0;
    if (!out || size == 0) return false;
    if (!text) text = "";
#define LV_AQ_PUT(ch)                                                          \
    do {                                                                       \
        if (n + 1 >= size) { out[0] = '\0'; return false; }                    \
        out[n++] = (ch);                                                       \
    } while (0)
    LV_AQ_PUT('"');
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') LV_AQ_PUT('\\');
        LV_AQ_PUT(*p);
    }
    LV_AQ_PUT('"');
#undef LV_AQ_PUT
    out[n] = '\0';
    return true;
}

#endif /* LV_SHELL_QUOTE_H */
