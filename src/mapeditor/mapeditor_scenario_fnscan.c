/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_fnscan.c
 * Purpose:
 *   Finds the function definitions in a scenario script by
 *   reading its text a line at a time.
 *
 *   Four spellings reach the host, and an author may use any
 *   of them, so all four are found and all four answer the
 *   bare name — the name the catalogue holds and the editor
 *   matches on:
 *
 *       function on_tick(tick) end
 *       function scenario.on_tick(tick) end
 *       on_tick = function(tick) end
 *       scenario.on_tick = function(tick) end
 *
 *   What it does not know, and what that costs.
 *
 *   A line comment ends the line: everything from the first
 *   -- onwards is not code. That is the whole of what this
 *   understands about Lua's own syntax.
 *
 *   It does not know block comments. A definition inside
 *   --[[ ... ]] is reported as though it were live, and the
 *   line the block opens on is cut at its own --, which is
 *   right for that line and for no other.
 *
 *   It does not know strings. A definition written inside
 *   one is reported, and a -- inside one ends the line early.
 *
 *   It does not know scope. A function defined inside
 *   another function's body reads the same as one at the top
 *   level, and a local reads the same as a global.
 *
 *   All three are deliberate. Half a Lua parser would get
 *   more of them right and would be wrong in ways a reader
 *   could not predict; a line scan is wrong in ways that can
 *   be written down, which is what this comment is. The view
 *   above this decides what to do with an answer, and an
 *   author who writes a hook inside a block comment has a
 *   problem the editor is not obliged to hide.
 *********************************************************/

#include <stddef.h>
#include <string.h>

#include "mapeditor_scenario_fnscan.h"

#define ME_SCN_FN_KEYWORD     "function"
#define ME_SCN_FN_KEYWORD_LEN 8

static int meScnIsNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int meScnIsNameStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int meScnIsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* Where the line's code stops: the first -- in it, which this treats as the
 * start of a comment wherever it appears. */
static size_t meScnCodeLen(const char *line, size_t len) {
    size_t i;

    for (i = 0; i + 1 < len; i++) {
        if (line[i] == '-' && line[i + 1] == '-') {
            return i;
        }
    }
    return len;
}

/* The keyword stands alone here rather than sitting inside a longer name,
 * so neither myfunction nor functional is read as a definition. */
static int meScnKeywordAt(const char *line, size_t len, size_t at) {
    if (len - at < ME_SCN_FN_KEYWORD_LEN ||
        memcmp(line + at, ME_SCN_FN_KEYWORD, ME_SCN_FN_KEYWORD_LEN) != 0) {
        return 0;
    }
    if (at > 0 && meScnIsNameChar(line[at - 1])) {
        return 0;
    }
    if (at + ME_SCN_FN_KEYWORD_LEN < len &&
        meScnIsNameChar(line[at + ME_SCN_FN_KEYWORD_LEN])) {
        return 0;
    }
    return 1;
}

/* Copies a name out of the line, answering false for one that does not fit.
 * A name too long to hold is left out of the answer entirely rather than
 * cut: a cut name would match a catalogue row this file does not define. */
static int meScnTakeName(const char *line, size_t from, size_t to,
                         MEScnFoundFn *row, int lineNo) {
    size_t len = to - from;

    if (len == 0 || len >= ME_SCN_FN_NAME_MAX) {
        return 0;
    }
    memcpy(row->name, line + from, len);
    row->name[len] = '\0';
    row->line      = lineNo;
    return 1;
}

/* The name after the keyword: a plain one, or the last part of a qualified
 * one, since scenario.on_tick and scenario:on_tick both define on_tick.
 *
 * A definition is a name the line then opens a parameter list on. Without
 * that test a `function` with a name after it and no call ahead of it — the
 * start of an expression split over two lines — would read as a definition.
 *
 * Answers where the name sits, or 0 for a run of text that is not one. */
static int meScnNameAfterKeyword(const char *line, size_t len, size_t at,
                                 size_t *nameFrom, size_t *nameTo) {
    size_t i = at;

    while (i < len && meScnIsSpace(line[i])) {
        i++;
    }
    if (i >= len || !meScnIsNameStart(line[i])) {
        return 0;
    }
    for (;;) {
        *nameFrom = i;
        while (i < len && meScnIsNameChar(line[i])) {
            i++;
        }
        *nameTo = i;
        /* A dot or a colon means what has been read so far was the table
           and the name is still to come. */
        if (i < len && (line[i] == '.' || line[i] == ':') &&
            i + 1 < len && meScnIsNameStart(line[i + 1])) {
            i++;
            continue;
        }
        break;
    }
    while (i < len && meScnIsSpace(line[i])) {
        i++;
    }
    return (i < len && line[i] == '(');
}

/* The name before an anonymous function: the on_tick of on_tick = function.
 * Read backwards from the keyword, which is where the one thing that says
 * this is a definition sits — the assignment. A qualified name needs no
 * special handling here, because reading back from the = stops at the last
 * part of it, which is the part wanted.
 *
 * Answers where the name sits, or 0 when what is in front of the keyword is
 * not an assignment to a name — a return function() end, or a call taking a
 * function as its argument. */
static int meScnNameBeforeKeyword(const char *line, size_t at,
                                  size_t *nameFrom, size_t *nameTo) {
    size_t i;

    if (at == 0) {
        return 0;
    }
    i = at;
    while (i > 0 && meScnIsSpace(line[i - 1])) {
        i--;
    }
    if (i == 0 || line[i - 1] != '=') {
        return 0;
    }
    i--;
    /* ==, ~=, <= and >= are questions, not assignments. */
    if (i > 0 && (line[i - 1] == '=' || line[i - 1] == '~' ||
                  line[i - 1] == '<' || line[i - 1] == '>')) {
        return 0;
    }
    while (i > 0 && meScnIsSpace(line[i - 1])) {
        i--;
    }
    if (i == 0 || !meScnIsNameChar(line[i - 1])) {
        return 0;
    }
    *nameTo = i;
    while (i > 0 && meScnIsNameChar(line[i - 1])) {
        i--;
    }
    *nameFrom = i;
    /* t[1] = function() end names nothing this can report, and neither does
       a number. */
    return meScnIsNameStart(line[i]);
}

/* One line's definitions, however many it holds: a line may carry more than
 * one, and each is found where its own keyword is. */
static size_t meScnScanLine(const char *line, size_t len, int lineNo,
                            MEScnFoundFn *out, size_t outMax) {
    size_t count = 0;
    size_t i;

    for (i = 0; i + ME_SCN_FN_KEYWORD_LEN <= len && count < outMax; i++) {
        size_t after = i + ME_SCN_FN_KEYWORD_LEN;
        size_t from  = 0;
        size_t to    = 0;

        if (!meScnKeywordAt(line, len, i)) {
            continue;
        }
        if (meScnNameAfterKeyword(line, len, after, &from, &to) ||
            meScnNameBeforeKeyword(line, i, &from, &to)) {
            if (meScnTakeName(line, from, to, &out[count], lineNo)) {
                count++;
            }
        }
        /* Past the keyword either way. The name cannot hold another of
           these, so nothing is skipped by moving on from here. */
        i = after - 1;
    }
    return count;
}

size_t meScnScanFunctions(const char *text, MEScnFoundFn *out, size_t outMax) {
    const char *p     = text;
    size_t      count = 0;
    int         lineNo = 1;

    if (text == NULL || out == NULL || outMax == 0) {
        return 0;
    }
    while (*p != '\0' && count < outMax) {
        const char *eol = strchr(p, '\n');
        size_t      len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        count += meScnScanLine(p, meScnCodeLen(p, len), lineNo, out + count,
                               outMax - count);
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
        lineNo++;
    }
    return count;
}
