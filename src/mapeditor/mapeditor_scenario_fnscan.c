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
 *   Every spelling a definition takes is found, and every one
 *   of them answers the bare name — the name the catalogue
 *   holds and the editor matches on:
 *
 *       function on_tick(tick) end
 *       function scenario.on_tick(tick) end
 *       function scenario:on_tick(tick) end
 *       on_tick = function(tick) end
 *       scenario.on_tick = function(tick) end
 *       local function on_tick(tick) end
 *       function M.on_tick(tick) end
 *
 *   Each row also says which of them it was, because the
 *   host resolves a hook as a global or as a field of the
 *   scenario table and reaches none of the rest. Which
 *   spelling answers which form, and what the host does with
 *   each, is in the header beside MEScnFnForm.
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
 *   level. A local is recognised only where the word stands
 *   on the same line as the definition and in front of it: a
 *   name declared local on one line and given its body on a
 *   later one is read as a global, because the line the body
 *   is on says nothing about where the name came from.
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
#define ME_SCN_LOCAL          "local"
#define ME_SCN_LOCAL_LEN      5

/* The one table the host reads a hook off, so the one table name that makes
 * a qualified definition reachable. */
#define ME_SCN_HOST_TABLE     "scenario"
#define ME_SCN_HOST_TABLE_LEN 8

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
                         MEScnFoundFn *row, int lineNo, MEScnFnForm form) {
    size_t len = to - from;

    if (len == 0 || len >= ME_SCN_FN_NAME_MAX) {
        return 0;
    }
    memcpy(row->name, line + from, len);
    row->name[len] = '\0';
    row->line      = lineNo;
    row->form      = form;
    return 1;
}

/* Whether the word local stands in front of that position with nothing but
 * spaces in between. Both spellings of a local definition need this — the
 * word sits in front of the keyword in local function on_tick, and in front
 * of the name in local on_tick = function — so both ask it, each about the
 * place its own word would be. */
static int meScnLocalBefore(const char *line, size_t at) {
    size_t i = at;

    while (i > 0 && meScnIsSpace(line[i - 1])) {
        i--;
    }
    if (i < ME_SCN_LOCAL_LEN ||
        memcmp(line + i - ME_SCN_LOCAL_LEN, ME_SCN_LOCAL,
               ME_SCN_LOCAL_LEN) != 0) {
        return 0;
    }
    i -= ME_SCN_LOCAL_LEN;
    /* Standing alone, so nonlocal and locals are not the word. */
    return (i == 0 || !meScnIsNameChar(line[i - 1]));
}

/* Which spelling the definition was written in: the table it was qualified
 * with if it was qualified at all, and otherwise whether the word local is
 * in front of it at localAt — the keyword for one spelling, the name for
 * the other.
 *
 * The table is the whole of what stands in front of the last separator, so
 * a.scenario.on_tick is a field of a table this cannot reach rather than of
 * the scenario table. sep is 0 for a name with no table in front of it. */
static MEScnFnForm meScnFormOf(const char *line, char sep, size_t tabFrom,
                               size_t tabTo, size_t localAt) {
    if (sep != '\0') {
        if (tabTo - tabFrom == ME_SCN_HOST_TABLE_LEN &&
            memcmp(line + tabFrom, ME_SCN_HOST_TABLE,
                   ME_SCN_HOST_TABLE_LEN) == 0) {
            return (sep == ':') ? ME_SCN_FORM_COLON : ME_SCN_FORM_SCENARIO;
        }
        return ME_SCN_FORM_TABLE;
    }
    return meScnLocalBefore(line, localAt) ? ME_SCN_FORM_LOCAL
                                           : ME_SCN_FORM_GLOBAL;
}

/* The name after the keyword: a plain one, or the last part of a qualified
 * one, since scenario.on_tick and scenario:on_tick both define on_tick.
 *
 * A definition is a name the line then opens a parameter list on. Without
 * that test a `function` with a name after it and no call ahead of it — the
 * start of an expression split over two lines — would read as a definition.
 *
 * Answers where the name sits, or 0 for a run of text that is not one. What
 * stood in front of the name comes back in sep and in tabFrom to tabTo,
 * which is the table the definition is a field of; sep is 0 and the table
 * is empty for a name written on its own. */
static int meScnNameAfterKeyword(const char *line, size_t len, size_t at,
                                 size_t *nameFrom, size_t *nameTo, char *sep,
                                 size_t *tabFrom, size_t *tabTo) {
    size_t i = at;

    *sep     = '\0';
    *tabFrom = 0;
    *tabTo   = 0;
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
           and the name is still to come. The table runs from the first
           part to the one in hand, so a.b.on_tick is a field of a.b. */
        if (i < len && (line[i] == '.' || line[i] == ':') &&
            i + 1 < len && meScnIsNameStart(line[i + 1])) {
            if (*sep == '\0') {
                *tabFrom = *nameFrom;
            }
            *tabTo = *nameTo;
            *sep   = line[i];
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
 * this is a definition sits — the assignment. Reading back from the = stops
 * at the last part of a qualified name, which is the part wanted, and the
 * read carries on over the separator to pick up the table in front of it.
 *
 * Answers where the name sits, or 0 when what is in front of the keyword is
 * not an assignment to a name — a return function() end, or a call taking a
 * function as its argument. sep and the table are as they are above. */
static int meScnNameBeforeKeyword(const char *line, size_t at,
                                  size_t *nameFrom, size_t *nameTo, char *sep,
                                  size_t *tabFrom, size_t *tabTo) {
    size_t i;

    *sep     = '\0';
    *tabFrom = 0;
    *tabTo   = 0;
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
    if (!meScnIsNameStart(line[i])) {
        return 0;
    }
    /* What the name is a field of, where it is a field at all. A table this
       cannot read back — the t of t[1].on_tick — leaves the separator set
       and the table empty, which is the right answer for it: some table,
       and not the one the host reads. */
    if (i > 0 && (line[i - 1] == '.' || line[i - 1] == ':')) {
        *sep   = line[i - 1];
        *tabTo = i - 1;
        i--;
        for (;;) {
            while (i > 0 && meScnIsNameChar(line[i - 1])) {
                i--;
            }
            *tabFrom = i;
            if (i > 1 && (line[i - 1] == '.' || line[i - 1] == ':') &&
                meScnIsNameChar(line[i - 2])) {
                i--;
                continue;
            }
            break;
        }
    }
    return 1;
}

/* One line's definitions, however many it holds: a line may carry more than
 * one, and each is found where its own keyword is. */
static size_t meScnScanLine(const char *line, size_t len, int lineNo,
                            MEScnFoundFn *out, size_t outMax) {
    size_t count = 0;
    size_t i;

    for (i = 0; i + ME_SCN_FN_KEYWORD_LEN <= len && count < outMax; i++) {
        size_t after   = i + ME_SCN_FN_KEYWORD_LEN;
        size_t from    = 0;
        size_t to      = 0;
        size_t tabFrom = 0;
        size_t tabTo   = 0;
        size_t localAt;
        char   sep     = '\0';

        if (!meScnKeywordAt(line, len, i)) {
            continue;
        }
        /* Where the word local would stand, which is in front of the
           keyword for one spelling and in front of the name for the
           other. */
        if (meScnNameAfterKeyword(line, len, after, &from, &to, &sep,
                                  &tabFrom, &tabTo)) {
            localAt = i;
        } else if (meScnNameBeforeKeyword(line, i, &from, &to, &sep, &tabFrom,
                                          &tabTo)) {
            localAt = from;
        } else {
            i = after - 1;
            continue;
        }
        if (meScnTakeName(line, from, to, &out[count], lineNo,
                          meScnFormOf(line, sep, tabFrom, tabTo, localAt))) {
            count++;
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
