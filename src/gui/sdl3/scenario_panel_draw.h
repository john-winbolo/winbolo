/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Scenario Panel Drawer
 *Filename:      scenario_panel_draw.h
 *Purpose:
 *  Turns one decoded display list into pixels with ImGui's
 *  2D calls. A scenario sends the list as data and every
 *  frontend interprets it with its own drawing: the list is
 *  the contract, and this is the ImGui frontends' half of
 *  it.
 *
 *  What it depends on is what the desktop client, the log
 *  viewer and the wasm target all already compile: ImGui,
 *  public/scenario_panel.h and the tile-sheet lookup in
 *  sprite_positions.c. No ClientSim, no sim headers and no
 *  lang table — a text primitive carries the script's own
 *  bytes and is not localised, and a name primitive is
 *  resolved through the callback below rather than by
 *  reading a roster this file cannot see.
 *
 *  Plain C, and free of ImGui, so the timer's arithmetic
 *  below can be held to a table of strings by a test that
 *  links no renderer. The drawing body is C++ because
 *  ImGui is, but nothing in this header is.
 *********************************************************/
#ifndef SCENARIO_PANEL_DRAW_H
#define SCENARIO_PANEL_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wire_limits.h"    /* PACKET_MAX_CHAT_MESSAGE, the status line's cap */
#include "scenario_panel.h" /* the decoded list, and GAME_NUMTOTALTICKS_SEC
                             * by way of global.h */

#ifdef __cplusplus
extern "C" {
#endif

/* Text height, in panel units, for the three sizes a text, name or timer
 * primitive asks for. Large is double small. Units rather than pixels because the square scales
 * with the game's zoom and the writing in it has to scale with the square:
 * a script that fits a line across the panel at one zoom fits it at every
 * other. */
#define SCN_PANEL_TEXT_NORMAL_UNITS 11.0f
#define SCN_PANEL_TEXT_LARGE_UNITS  16.0f
#define SCN_PANEL_TEXT_SMALL_UNITS   8.0f

/* Longest string a timer renders, "M:SS" with the minutes unbounded, plus
 * its terminator. A round long enough to overrun this is longer than the
 * tick counter itself. */
#define SCN_PANEL_TIMER_TEXT_MAX 24

/*********************************************************
 *NAME:          scnPanelTimerText
 *PURPOSE:
 *  The minutes and seconds between two game ticks, as the
 *  timer primitive shows them.
 *
 *  A countdown reads the time from now until its tick, a
 *  count-up the time from its tick until now, and either
 *  reads 0:00 rather than going negative once it is on the
 *  wrong side of its target — a wave whose time has run out
 *  sits at zero until the script says something else.
 *
 *  Ticks run at GAME_NUMTOTALTICKS_SEC, a hundred a second,
 *  which is the clock the script counted its tick on.
 *  Minutes are not wrapped at sixty: an hour and a half
 *  reads 90:00, because a panel that showed 30:00 for it
 *  would be lying about the hour.
 *
 *  Arithmetic over its arguments and nothing else, which is
 *  what lets a unit test hold it to exact strings with no
 *  window, no renderer and no ImGui behind it.
 *
 *ARGUMENTS:
 *  now     - the tick the viewer is on
 *  target  - the tick the primitive carries
 *  mode    - a ScnPanelTimerMode
 *  out     - receives the text, always terminated
 *  outSize - bytes of out, including the terminator
 *********************************************************/
static inline void scnPanelTimerText(uint32_t now, uint32_t target,
                                     uint8_t mode, char *out, size_t outSize) {
    uint32_t     ticks;
    uint32_t     seconds;
    unsigned int mins, secs;

    if (out == NULL || outSize == 0) return;

    if (mode == (uint8_t)SCN_PANEL_TIMER_UP) {
        ticks = (now > target) ? (now - target) : 0u;
    } else {
        ticks = (target > now) ? (target - now) : 0u;
    }

    seconds = ticks / (uint32_t)GAME_NUMTOTALTICKS_SEC;
    mins    = (unsigned int)(seconds / 60u);
    secs    = (unsigned int)(seconds % 60u);
    snprintf(out, outSize, "%u:%02u", mins, secs);
}

/* The most bytes the status line's full text can hold: the script's line,
 * the gap before the countdown, and the countdown. */
#define SCN_STATUS_LINE_MAX (PACKET_MAX_CHAT_MESSAGE + 1 + 4 + \
                             SCN_PANEL_TIMER_TEXT_MAX)

/*********************************************************
 *NAME:          scnStatusLineText
 *PURPOSE:
 *  The status line as it is drawn: the script's text, and
 *  when it asked for a countdown, four spaces and the time
 *  left until that tick as a panel countdown shows it
 *  ("Wave 3/10    2:44"). The countdown stops at 0:00.
 *
 *  False, with out empty, when there is no line to draw.
 *
 *ARGUMENTS:
 *  text    - the script's line, or NULL
 *  endsAt  - the tick counted to, or SCN_STATUS_NO_COUNTDOWN
 *  now     - the tick the viewer is on
 *  out     - receives the text, always terminated
 *  outSize - bytes of out, including the terminator
 *********************************************************/
static inline bool scnStatusLineText(const char *text, uint32_t endsAt,
                                     uint32_t now, char *out,
                                     size_t outSize) {
    char timer[SCN_PANEL_TIMER_TEXT_MAX];

    if (out == NULL || outSize == 0) return false;
    out[0] = '\0';
    if (text == NULL || text[0] == '\0') return false;
    if (endsAt == SCN_STATUS_NO_COUNTDOWN) {
        snprintf(out, outSize, "%s", text);
        return true;
    }
    scnPanelTimerText(now, endsAt, (uint8_t)SCN_PANEL_TIMER_DOWN, timer,
                      sizeof(timer));
    snprintf(out, outSize, "%s    %s", text, timer);
    return true;
}

/*********************************************************
 *NAME:          scnAnnounceRemaining
 *PURPOSE:
 *  Whether a scenario's announcement is still on screen,
 *  and how many ticks it has left.
 *
 *  An announcement carries the tick it landed at and how
 *  long it was asked to stay up, both on the tick the
 *  server counts in, and every viewer works the rest out
 *  against its own clock — the same bargain the timer
 *  primitive makes. Nothing to show is an empty text, a
 *  duration of nothing, or a clock that has reached the end
 *  of one; all three answer false and leave the remainder
 *  zero.
 *
 *  It sits beside the timer's text above because the two
 *  are the same kind of thing: the arithmetic under a
 *  scenario's presentation, with no renderer in it, which
 *  is what lets a test hold either to a table of answers.
 *
 *ARGUMENTS:
 *  text        - the announcement, or NULL
 *  arrivedTick - the tick it landed at
 *  ticks       - how long it was asked to stay up
 *  nowTick     - the tick the viewer is on
 *  outLeft     - receives the ticks left; may be NULL
 *********************************************************/
static inline bool scnAnnounceRemaining(const char *text, uint32_t arrivedTick,
                                        uint16_t ticks, uint32_t nowTick,
                                        uint32_t *outLeft) {
    uint32_t elapsed;

    if (outLeft != NULL) *outLeft = 0;
    if (text == NULL || text[0] == '\0' || ticks == 0) return false;

    /* A clock behind the arrival has not reached it yet, which is the whole
     * duration still to run rather than a negative age. */
    elapsed = (nowTick > arrivedTick) ? (nowTick - arrivedTick) : 0u;
    if (elapsed >= (uint32_t)ticks) return false;

    if (outLeft != NULL) *outLeft = (uint32_t)ticks - elapsed;
    return true;
}

/* -------------------------------------------------------
 * The Scenario Text window
 *
 * The status line and the announcement are drawn in one
 * window of their own, the Scenario Text window, and not at
 * a place on the game view. The lines are laid out in a
 * source rectangle measured in panel units, and that
 * rectangle is scaled into the window with one factor for
 * both axes, as large as the window allows up to a cap on
 * the letter height (SCN_TEXT_MAX_LETTER_UI_SHARE), and
 * centred in it. The text is never stretched.
 *
 * The source rectangle is as wide as the widest row, with
 * the outline and the pad on each side, so the text is as
 * large as the window's width and height both allow. It has
 * a row for each row of text the round has used so far: the
 * status line's on top, then the announcements', oldest
 * first, as many as have been up at once. The script's
 * rows are counted over the whole round and not frame by
 * frame, but the width is this frame's widest row, and the
 * rows that breaking a long line adds are this frame's
 * only. So the text size follows the widest row on screen,
 * and stays the same when a line comes and goes only while
 * the letter height cap holds it, which is the usual case.
 * A row is the script's own: a line is split where the
 * script put a "\n", and nowhere else, while it fits across
 * the window at the smallest letter height
 * (SCN_TEXT_MIN_LETTER_UI_SHARE). A line that does not fit
 * even then is broken at its spaces into the fewest rows
 * that fit, of widths as even as they go
 * (scnTextWrapBalanced). A word is never broken inside: a
 * single word wider than the window even at the smallest
 * letter height is drawn smaller than that, down to
 * SCN_TEXT_FLOOR_LETTER_UI_SHARE, so it fits across. A row
 * in a language written without spaces (Chinese, Japanese,
 * Korean) is broken between its characters instead
 * (scnTextBreakUnspaced). An announcement's share across
 * is taken over the window's width (scnTextAcrossSpan).
 * ------------------------------------------------------- */

/* The source rectangle's width for the window's own default size, in panel
 * units: the width of the classic game view at zoom 1 (15 squares of 16
 * units). The text inside is laid out by its own width (see above). */
#define SCN_TEXT_SRC_W_UNITS 240.0f

/* The largest letter height in the Scenario Text window, as a share of the
 * height of the game's normal UI text (the font size the panel's bar and
 * the dialogs use, ImGui's font size: 18 pixels at UI scale 1). Short text
 * in a large window stops growing at this size. */
#define SCN_TEXT_MAX_LETTER_UI_SHARE 1.5f

/* The smallest letter height the Scenario Text window lays a script's line
 * out at on one row, as a share of the game's normal UI text height. A line
 * too wide for the window even at this size is broken into rows. */
#define SCN_TEXT_MIN_LETTER_UI_SHARE 0.75f

/* The smallest letter height a single word too wide for the window is drawn
 * at, as a share of the game's normal UI text height (9 pixels at UI scale
 * 1). A word is not broken inside, so a word wider than the window at
 * SCN_TEXT_MIN_LETTER_UI_SHARE is made smaller until it fits, but never
 * smaller than this; a word still too wide here is centred and cut off at
 * the window's sides. */
#define SCN_TEXT_FLOOR_LETTER_UI_SHARE 0.5f

/* The most words a row is balanced over when it is broken into rows. A row
 * of more words is broken at the last space that fits, row by row. */
#define SCN_TEXT_WRAP_MAX_WORDS 96

/* A line's letters, in panel units: the height the announcement was always
 * drawn at. */
#define SCN_TEXT_LINE_UNITS 20.0f

/* How far the outline reaches past the letters on each side, in panel units. */
#define SCN_TEXT_OUTLINE_UNITS 1.0f

/* The space kept between the lines and the edges of the source rectangle,
 * and between the two rows, in panel units. */
#define SCN_TEXT_PAD_UNITS 2.0f

/* The height of one row: the letters and the outline above and below them. */
#define SCN_TEXT_ROW_UNITS (SCN_TEXT_LINE_UNITS + 2.0f * SCN_TEXT_OUTLINE_UNITS)

/* The most rows there are: the status line and up to four announcements at
 * once (SCN_ANNOUNCE_STACK_MAX in client_sim.h), each of a row or two once
 * its text is broken to fit across. A row past this many is not drawn. */
#define SCN_TEXT_MAX_ROWS 10

/* The source rectangle's height for a number of rows. Fewer than one row
 * counts as one, and more than SCN_TEXT_MAX_ROWS as that many. */
static inline float scnTextSourceHeight(int rows) {
    if (rows < 1) rows = 1;
    if (rows > SCN_TEXT_MAX_ROWS) rows = SCN_TEXT_MAX_ROWS;
    return 2.0f * SCN_TEXT_PAD_UNITS + (float)rows * SCN_TEXT_ROW_UNITS +
           (float)(rows - 1) * SCN_TEXT_PAD_UNITS;
}

/* The top of a row in the source rectangle, row 0 at the top. */
static inline float scnTextRowTop(int row) {
    if (row < 0) row = 0;
    return SCN_TEXT_PAD_UNITS +
           (float)row * (SCN_TEXT_ROW_UNITS + SCN_TEXT_PAD_UNITS);
}

/* How the source rectangle lands in the window: screen pixels for one
 * source unit, and the screen point the source rectangle's top-left corner
 * goes to. */
typedef struct ScnTextFit {
    float scale;
    float originX, originY;
} ScnTextFit;

/*********************************************************
 *NAME:          scnTextFitRect
 *PURPOSE:
 *  Scales a source rectangle into a window rectangle as
 *  large as it goes with one factor for both axes, so the
 *  shape of the text is kept, and centres it in the window.
 *  The spare space is on the left and right when the window
 *  is wider than the source, and above and below when it is
 *  taller. A source or window with no area gives a scale of
 *  0 at the window's top-left corner.
 *
 *ARGUMENTS:
 *  srcW/srcH - the source rectangle's size, in units
 *  winX/winY - the window's top-left corner, in pixels
 *  winW/winH - the window's size, in pixels
 *********************************************************/
static inline ScnTextFit scnTextFitRectHeld(float srcW, float srcH,
                                            float winX, float winY,
                                            float winW, float winH,
                                            float minWScale, float maxScale);
static inline ScnTextFit scnTextFitRect(float srcW, float srcH, float winX,
                                        float winY, float winW, float winH) {
    return scnTextFitRectHeld(srcW, srcH, winX, winY, winW, winH, 0.0f, 0.0f);
}

/*********************************************************
 *NAME:          scnTextFitRectCapped
 *PURPOSE:
 *  scnTextFitRect, with the scale held at or under
 *  maxScale, so short text in a large window does not get
 *  huge. The source rectangle is then smaller than the
 *  window on both axes, and centred both ways. A maxScale
 *  of 0 or less is no cap.
 *
 *ARGUMENTS:
 *  srcW/srcH - the source rectangle's size, in units
 *  winX/winY - the window's top-left corner, in pixels
 *  winW/winH - the window's size, in pixels
 *  maxScale  - the most pixels for one unit, or 0
 *********************************************************/
static inline ScnTextFit scnTextFitRectCapped(float srcW, float srcH,
                                              float winX, float winY,
                                              float winW, float winH,
                                              float maxScale) {
    return scnTextFitRectHeld(srcW, srcH, winX, winY, winW, winH, 0.0f,
                              maxScale);
}

/*********************************************************
 *NAME:          scnTextFitRectHeld
 *PURPOSE:
 *  scnTextFitRectCapped, with the scale the width allows
 *  held at or over minWScale, so a source wider than the
 *  window is not made smaller than that. The source is
 *  then centred and cut off at the window's sides, as
 *  much on the left as on the right. The height still
 *  makes the scale smaller, and the cap still holds it. A
 *  minWScale of 0 or less is no floor.
 *
 *ARGUMENTS:
 *  srcW/srcH - the source rectangle's size, in units
 *  winX/winY - the window's top-left corner, in pixels
 *  winW/winH - the window's size, in pixels
 *  minWScale - the fewest pixels for one unit the width
 *              may give, or 0
 *  maxScale  - the most pixels for one unit, or 0
 *********************************************************/
static inline ScnTextFit scnTextFitRectHeld(float srcW, float srcH,
                                            float winX, float winY,
                                            float winW, float winH,
                                            float minWScale, float maxScale) {
    ScnTextFit fit;
    fit.scale   = 0.0f;
    fit.originX = winX;
    fit.originY = winY;
    if (srcW <= 0.0f || srcH <= 0.0f || winW <= 0.0f || winH <= 0.0f) {
        return fit;
    }
    fit.scale = winW / srcW;
    if (minWScale > 0.0f && fit.scale < minWScale) fit.scale = minWScale;
    if (winH / srcH < fit.scale) fit.scale = winH / srcH;
    if (maxScale > 0.0f && fit.scale > maxScale) fit.scale = maxScale;
    fit.originX = winX + (winW - srcW * fit.scale) * 0.5f;
    fit.originY = winY + (winH - srcH * fit.scale) * 0.5f;
    return fit;
}

/* Measures text from s up to e (not including e) at the line's letter
 * height, SCN_TEXT_LINE_UNITS, in panel units. ctx is the caller's (the
 * font, in the game). */
typedef float (*ScnTextMeasureFn)(const char *s, const char *e, void *ctx);

/* The source rectangle's width for rows of text: the widest row, with the
 * outline and the pad on each side. Text with no row width gives 0. */
static inline float scnTextContentWidth(const char *text,
                                        ScnTextMeasureFn measure, void *ctx) {
    float widest = 0.0f;
    const char *r = text;
    if (text == NULL || measure == NULL) return 0.0f;
    for (;;) {
        const char *e = r;
        float w;
        while (*e != '\0' && *e != '\n') e++;
        w = measure(r, e, ctx);
        if (w > widest) widest = w;
        if (*e == '\0') break;
        r = e + 1;
    }
    if (widest <= 0.0f) return 0.0f;
    return widest + 2.0f * SCN_TEXT_OUTLINE_UNITS + 2.0f * SCN_TEXT_PAD_UNITS;
}

/* One way to break a row's words into rows of at most maxW: from the first
 * word down (fromBack false), each row taking as many words as fit, or from
 * the last word up (fromBack true). A word wider than maxW is a row by
 * itself. Fills brk[i] with true where a row ends after word i, and gives
 * the number of rows and, in *minW and *maxOut, the narrowest and widest
 * row. ws/we are the words' first and past-the-end characters. */
static inline int scnTextGreedyRows(const char *const *ws,
                                    const char *const *we, int n, float maxW,
                                    bool fromBack, ScnTextMeasureFn measure,
                                    void *ctx, bool *brk, float *minW,
                                    float *maxOut) {
    int rows = 0;
    int i;
    float lo = 0.0f, hi = 0.0f;
    for (i = 0; i < n; i++) brk[i] = false;
    if (!fromBack) {
        int start = 0;
        while (start < n) {
            int end = start;   /* the row's last word */
            float w;
            while (end + 1 < n && measure(ws[start], we[end + 1], ctx) <= maxW) {
                end++;
            }
            w = measure(ws[start], we[end], ctx);
            if (rows == 0 || w < lo) lo = w;
            if (rows == 0 || w > hi) hi = w;
            rows++;
            brk[end] = true;
            start = end + 1;
        }
    } else {
        int last = n - 1;
        while (last >= 0) {
            int first = last;   /* the row's first word */
            float w;
            while (first - 1 >= 0 && measure(ws[first - 1], we[last], ctx) <= maxW) {
                first--;
            }
            w = measure(ws[first], we[last], ctx);
            if (rows == 0 || w < lo) lo = w;
            if (rows == 0 || w > hi) hi = w;
            rows++;
            if (first > 0) brk[first - 1] = true;
            last = first - 1;
        }
        brk[n - 1] = true;
    }
    if (minW != NULL) *minW = lo;
    if (maxOut != NULL) *maxOut = hi;
    return rows;
}

/*********************************************************
 *NAME:          scnTextWrapBalanced
 *PURPOSE:
 *  Breaks each row of text wider than maxW into rows, in
 *  place, at its spaces only, never inside a word: the
 *  space at a break becomes "\n". A row is broken into the
 *  fewest rows that fit in maxW, and those rows are made as
 *  even in width as they go, so no short word sits alone
 *  on a row of its own: the narrowest row width that still
 *  gives that few rows is found, and of the two ways to
 *  fill rows at that width (from the first word, or from
 *  the last), the one with the smaller gap between its
 *  widest and narrowest row is used. A row that fits is
 *  not changed, nor is a row with no space (one word, or
 *  a language written without spaces). A word wider than
 *  maxW is a row by itself. Gives the number of rows the
 *  text has after.
 *
 *ARGUMENTS:
 *  text    - the rows, split by "\n"; changed in place
 *  measure - measures text at the line's letter height
 *  ctx     - passed to measure
 *  maxW    - the widest a row may be, in units
 *********************************************************/
static inline int scnTextWrapBalanced(char *text, ScnTextMeasureFn measure,
                                      void *ctx, float maxW) {
    char *row = text;
    int   rows = 0;
    if (text == NULL || measure == NULL) return 0;
    for (;;) {
        char *e = row;
        while (*e != '\0' && *e != '\n') e++;
        if (maxW > 0.0f && measure(row, e, ctx) > maxW) {
            const char *ws[SCN_TEXT_WRAP_MAX_WORDS];
            const char *we[SCN_TEXT_WRAP_MAX_WORDS];
            bool        brk[SCN_TEXT_WRAP_MAX_WORDS];
            bool        brk2[SCN_TEXT_WRAP_MAX_WORDS];
            int         n = 0;
            bool        tooMany = false;
            char       *p = row;
            /* The words: runs of characters between spaces. */
            while (p < e) {
                while (p < e && *p == ' ') p++;
                if (p >= e) break;
                if (n == SCN_TEXT_WRAP_MAX_WORDS) { tooMany = true; break; }
                ws[n] = p;
                while (p < e && *p != ' ') p++;
                we[n] = p;
                n++;
            }
            if (n > 1 && !tooMany) {
                float lo = 0.0f, hi = maxW, wordW = 0.0f;
                int   want, i, iter;
                float aMin, aMax, bMin, bMax;
                const bool *use;
                for (i = 0; i < n; i++) {
                    const float w = measure(ws[i], we[i], ctx);
                    if (w > wordW) wordW = w;
                }
                want = scnTextGreedyRows(ws, we, n, maxW, false, measure, ctx,
                                         brk, NULL, NULL);
                /* The narrowest width that still gives that few rows. */
                lo = wordW;
                if (lo > hi) lo = hi;
                for (iter = 0; iter < 24; iter++) {
                    const float mid = (lo + hi) * 0.5f;
                    if (scnTextGreedyRows(ws, we, n, mid, false, measure, ctx,
                                          brk, NULL, NULL) <= want) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                scnTextGreedyRows(ws, we, n, hi, false, measure, ctx, brk,
                                  &aMin, &aMax);
                scnTextGreedyRows(ws, we, n, hi, true, measure, ctx, brk2,
                                  &bMin, &bMax);
                use = (bMax - bMin < aMax - aMin) ? brk2 : brk;
                /* A break after word i: the first space after it. */
                for (i = 0; i + 1 < n; i++) {
                    if (use[i]) *(char *)we[i] = '\n';
                }
                /* Count the rows made, and go on after this row. */
                for (i = 0; i + 1 < n; i++) {
                    if (use[i]) rows++;
                }
            } else if (tooMany) {
                /* A row of very many words: the last space that fits. */
                char *start = row;
                char *lastSpace = NULL;
                for (p = row; p < e; p++) {
                    if (*p == ' ') {
                        if (measure(start, p, ctx) > maxW && lastSpace != NULL) {
                            *lastSpace = '\n';
                            rows++;
                            start = lastSpace + 1;
                        }
                        lastSpace = p;
                    }
                }
                if (measure(start, e, ctx) > maxW && lastSpace != NULL &&
                    lastSpace >= start) {
                    *lastSpace = '\n';
                    rows++;
                }
            }
        }
        rows++;
        if (*e == '\0') break;
        row = e + 1;
    }
    return rows;
}

/* True for a character of a language written without spaces between its
 * words: the CJK marks and letters (U+2E80 to U+9FFF), Hangul syllables
 * (U+AC00 to U+D7AF), CJK compatibility letters (U+F900 to U+FAFF), the
 * full-width forms (U+FF00 to U+FFEF) and the CJK letters from U+20000. */
static inline bool scnTextIsUnspacedChar(uint32_t cp) {
    return (cp >= 0x2E80u && cp <= 0x9FFFu) ||
           (cp >= 0xAC00u && cp <= 0xD7AFu) ||
           (cp >= 0xF900u && cp <= 0xFAFFu) ||
           (cp >= 0xFF00u && cp <= 0xFFEFu) || (cp >= 0x20000u);
}

/* The length in bytes of the UTF-8 character at p, 1 to 4, never past the
 * terminating NUL, and its code point in *cp. A stray byte is one
 * character. */
static inline size_t scnTextUtf8At(const char *p, uint32_t *cp) {
    const unsigned char c = (unsigned char)*p;
    size_t   n = 1, k;
    uint32_t v = c;
    if (c >= 0xF0) { n = 4; v = c & 0x07u; }
    else if (c >= 0xE0) { n = 3; v = c & 0x0Fu; }
    else if (c >= 0xC0) { n = 2; v = c & 0x1Fu; }
    for (k = 1; k < n; k++) {
        const unsigned char d = (unsigned char)p[k];
        if (d == 0 || (d & 0xC0u) != 0x80u) { n = k; break; }
        v = (v << 6) | (uint32_t)(d & 0x3Fu);
    }
    if (n == 1) v = c;
    if (cp != NULL) *cp = v;
    return n;
}

/*********************************************************
 *NAME:          scnTextBreakUnspaced
 *PURPOSE:
 *  Breaks each row of text wider than maxW that has no
 *  space in it and holds a character of a language written
 *  without spaces (scnTextIsUnspacedChar: Chinese,
 *  Japanese, Korean) into rows that fit, in place, before
 *  the first character that does not fit. A run of other
 *  characters in such a row, such as a Latin name or a
 *  number, is not split: the row is broken before or after
 *  the run instead. A "\n" is put in
 *  there while text has room for it (cap). Run after
 *  scnTextWrapBalanced. A row with a space is not changed
 *  (scnTextWrapBalanced breaks it at its spaces), nor is a
 *  row of one word in a language written with spaces: a
 *  word of Latin or other such letters is never broken
 *  inside. A row always keeps at least one character.
 *  Gives the number of rows the text has after.
 *
 *ARGUMENTS:
 *  text    - the rows, split by "\n"; changed in place
 *  cap     - the size of text's buffer, in bytes
 *  measure - measures text at the line's letter height
 *  ctx     - passed to measure
 *  maxW    - the widest a row may be, in units
 *********************************************************/
static inline int scnTextBreakUnspaced(char *text, size_t cap,
                                       ScnTextMeasureFn measure, void *ctx,
                                       float maxW) {
    size_t len;
    char  *row;
    int    rows = 1;
    if (text == NULL) return 0;
    for (row = text; *row != '\0'; row++) {
        if (*row == '\n') rows++;
    }
    if (measure == NULL || cap == 0 || maxW <= 0.0f) return rows;
    len  = strlen(text);
    rows = 0;
    row  = text;
    for (;;) {
        char *e = row;
        char *p;
        bool  hasSpace = false, unspaced = false;
        char *breakAt  = NULL;
        float w        = 0.0f;
        while (*e != '\0' && *e != '\n') {
            uint32_t cp;
            const size_t n = scnTextUtf8At(e, &cp);
            if (*e == ' ') hasSpace = true;
            if (scnTextIsUnspacedChar(cp)) unspaced = true;
            e += n;
        }
        if (!hasSpace && unspaced) {
            /* A break is taken only before a character where it or the
               one before it is unspaced, so a run of Latin letters or
               digits next to such text is never split. When the first
               character that does not fit is inside such a run, the row
               is broken at the last place before it that may break, or
               at the first one after it when there is none before. */
            char    *lastOk = NULL;
            uint32_t prevCp = 0;
            for (p = row; p < e;) {
                uint32_t     cp;
                const size_t n  = scnTextUtf8At(p, &cp);
                const float  cw = measure(p, p + n, ctx);
                const bool   ok = p > row && (scnTextIsUnspacedChar(cp) ||
                                              scnTextIsUnspacedChar(prevCp));
                if (w + cw > maxW && p > row) {
                    if (ok) {
                        breakAt = p;
                        break;
                    }
                    if (lastOk != NULL) {
                        breakAt = lastOk;
                        break;
                    }
                }
                if (ok) lastOk = p;
                w += cw;
                prevCp = cp;
                p += n;
            }
        }
        rows++;
        if (breakAt != NULL && len + 1 < cap) {
            memmove(breakAt + 1, breakAt, len - (size_t)(breakAt - text) + 1);
            *breakAt = '\n';
            len++;
            row = breakAt + 1;
            continue;
        }
        if (*e == '\0') break;
        row = e + 1;
    }
    return rows;
}

/*********************************************************
 *NAME:          scnTextAcrossSpan
 *PURPOSE:
 *  The span a line's share across is taken over: the text
 *  area's whole width, in source units, and the screen
 *  point its left edge is at, so a line at share 0.1 sits
 *  near the area's left side and not near the left of the
 *  centred text block. When the text block is wider than
 *  the area (a word held at the letter floor), the span is
 *  the block, so no line is made smaller to fit the area.
 *  A fit with no scale gives the block.
 *
 *ARGUMENTS:
 *  areaX/areaW - the text area's left edge and width, in
 *                pixels
 *  fit         - the source rectangle's fit in the area
 *  srcW        - the source rectangle's width, in units
 *  outLeft     - receives the span's left edge, in pixels
 *  outW        - receives the span's width, in units
 *********************************************************/
static inline void scnTextAcrossSpan(float areaX, float areaW, ScnTextFit fit,
                                     float srcW, float *outLeft,
                                     float *outW) {
    float left = fit.originX;
    float w    = srcW;
    if (fit.scale > 0.0f && areaW / fit.scale > srcW) {
        left = areaX;
        w    = areaW / fit.scale;
    }
    if (outLeft != NULL) *outLeft = left;
    if (outW != NULL) *outW = w;
}

/* A rectangle in screen pixels. */
typedef struct ScnTextBand {
    float x, y, w, h;
} ScnTextBand;

/*********************************************************
 *NAME:          scnTextTabletBand
 *PURPOSE:
 *  Where tablet mode draws the scenario text: a band at
 *  the top middle of its game view, inset from the top
 *  edge. The band is the source rectangle at the zoom's
 *  scale. A band wider than the room it has is made
 *  smaller, keeping its shape, until it fits.
 *
 *  The room is the view's width. When the scenario panel's
 *  square is shown in the view's top-right corner, the room
 *  ends one inset left of the square, and the band is
 *  centred in what is left, so it never covers the square.
 *  A square outside the view's width does not change the
 *  room.
 *
 *  A band with no room, or a view or source with no area,
 *  has a width and height of 0.
 *
 *ARGUMENTS:
 *  viewX/viewY - the game view's top-left, in pixels
 *  viewW       - the game view's width, in pixels
 *  srcW/srcH   - the source rectangle, in units
 *  scale       - pixels per unit at the zoom
 *  inset       - the space from the top edge, and kept
 *                from the square, in pixels
 *  haveSlot    - the panel's square is shown
 *  slotX       - the square's left edge, in pixels
 *********************************************************/
static inline ScnTextBand scnTextTabletBand(float viewX, float viewY,
                                            float viewW, float srcW,
                                            float srcH, float scale,
                                            float inset, bool haveSlot,
                                            float slotX) {
    ScnTextBand band;
    float       right = viewX + viewW;
    float       room;

    if (inset < 0.0f) inset = 0.0f;
    band.x = viewX;
    band.y = viewY + inset;
    band.w = 0.0f;
    band.h = 0.0f;
    if (viewW <= 0.0f || srcW <= 0.0f || srcH <= 0.0f || scale <= 0.0f) {
        return band;
    }
    if (haveSlot && slotX > viewX && slotX < right) {
        right = slotX - inset;
    }
    room = right - viewX;
    if (room <= 0.0f) return band;

    band.w = srcW * scale;
    band.h = srcH * scale;
    if (band.w > room) {
        band.h *= room / band.w;
        band.w  = room;
    }
    band.x = viewX + (room - band.w) * 0.5f;
    return band;
}

/*********************************************************
 *NAME:          scnTextPrintedRecently
 *PURPOSE:
 *  Whether a line was on screen in the Scenario Text
 *  window within the last holdMs milliseconds. The window
 *  stays up for that time after its last line goes, and
 *  not after it, so an empty window does not stay on the
 *  game view.
 *
 *ARGUMENTS:
 *  everPrinted - a line has been on screen this round
 *  lastMs      - the SDL tick, in ms, a line was last on
 *                screen
 *  nowMs       - the SDL tick now, in ms
 *  holdMs      - how long the window stays up after its
 *                last line, in ms
 *********************************************************/
static inline bool scnTextPrintedRecently(bool everPrinted, uint64_t lastMs,
                                          uint64_t nowMs, uint64_t holdMs) {
    if (!everPrinted) return false;
    if (nowMs < lastMs) return true;
    return nowMs - lastMs < holdMs;
}

/*********************************************************
 *NAME:          scnTextWindowShown
 *PURPOSE:
 *  Whether the desktop Scenario Text window is up this
 *  frame. It is up while the player has it shown and there
 *  is a line to show, or a line was on screen a short time
 *  ago (scnTextPrintedRecently), so the player can still
 *  grab it just after a line goes. It is also up while the
 *  player is busy with it (a drag, its settings, its close
 *  confirm), so that is not cut off. With no recent line
 *  and nothing busy it is not up, so no empty window stays
 *  on the game view.
 *
 *ARGUMENTS:
 *  shown    - the player has the window shown
 *  recent   - a line was on screen a short time ago
 *  haveText - there is a status line or an announcement
 *  busy     - the player is busy with the window
 *********************************************************/
static inline bool scnTextWindowShown(bool shown, bool recent,
                                      bool haveText, bool busy) {
    return shown && (recent || haveText || busy);
}

/*********************************************************
 *NAME:          scnFloatPointerOnHandle
 *PURPOSE:
 *  Whether a point is on a window frame's move bar or on
 *  its resize grip: the two places where a window with
 *  the scenario window frame (a scenario panel, the
 *  Scenario Text window) takes a click. The
 *  bar is the strip directly above the content, as wide as
 *  the content and barH high, so its bottom edge is the
 *  content's top edge and it covers none of the content.
 *  It holds the gear and the X. The grip is the gripSide
 *  square in the content's bottom-right corner. Each
 *  rectangle includes its top and left edges and not its
 *  bottom and right edges, as ImGui's own rectangle test
 *  does. Content with no area has no bar and no grip. A
 *  barH of 0 means no bar.
 *
 *ARGUMENTS:
 *  x/y      - the content's top-left corner, in pixels
 *  w/h      - the content's size, in pixels
 *  barH     - the bar's height, in pixels
 *  gripSide - the grip's side, in pixels
 *  px/py    - the point, in pixels
 *********************************************************/
static inline bool scnFloatPointerOnHandle(float x, float y, float w, float h,
                                           float barH, float gripSide,
                                           float px, float py) {
    if (w <= 0.0f || h <= 0.0f) return false;
    if (px < x || px >= x + w) return false;
    if (barH > 0.0f && py >= y - barH && py < y) return true;
    if (gripSide > 0.0f && px >= x + w - gripSide &&
        py >= y + h - gripSide && py < y + h) {
        return true;
    }
    return false;
}

/*********************************************************
 *NAME:          scnFloatTakesInput
 *PURPOSE:
 *  Whether the ImGui window of a scenario window frame (a
 *  scenario panel, the Scenario Text window) takes the
 *  mouse this frame: only while the pointer is on its bar
 *  (which holds the gear and the X) or on its grip, or
 *  while a move or resize drag that began there is on. The
 *  rest of the time a click or the wheel on its content
 *  goes through to the game view. Its settings window and
 *  its close confirm are windows of their own, and take
 *  the mouse while they are open.
 *
 *ARGUMENTS:
 *  onHandle - the pointer is on the bar or the grip
 *  dragging - a move or resize drag is on
 *********************************************************/
static inline bool scnFloatTakesInput(bool onHandle, bool dragging) {
    return onHandle || dragging;
}

/*********************************************************
 *NAME:          scnFloatPointerToGame
 *PURPOSE:
 *  Whether a mouse motion or wheel event goes on to the
 *  game while ImGui wants the mouse, so the game's screen
 *  cursor keeps following the pointer over a scenario
 *  window frame's bar and grip, as it does over the rest
 *  of the window. It goes on when the item ImGui is
 *  dragging is in a frame window (the bar or the grip), or
 *  when the pointer is over a frame window and ImGui is
 *  dragging nothing. A press and a release never go on:
 *  the bar and the grip keep their clicks. Any other
 *  ImGui window keeps the mouse as before.
 *
 *ARGUMENTS:
 *  hoverIsFrame  - the window under the pointer is a
 *                  scenario window frame's
 *  anyActive     - ImGui has an active item
 *  activeIsFrame - that item is in a frame window
 *********************************************************/
static inline bool scnFloatPointerToGame(bool hoverIsFrame, bool anyActive,
                                         bool activeIsFrame) {
    if (anyActive) return activeIsFrame;
    return hoverIsFrame;
}

/*********************************************************
 *NAME:          scnFloatFrameShown
 *PURPOSE:
 *  Whether the window frame a scenario panel and the
 *  Scenario Text window share draws its faint edge round
 *  the content and, while the bar shows, its resize grip.
 *  The panel (hideAtZero false) always draws them, at any
 *  background opacity. The Scenario Text window
 *  (hideAtZero true) draws them at 0% background opacity
 *  only while the pointer is on the window (the content,
 *  or the bar while the bar is up) or while a move or a
 *  resize drag is on, also when the drag takes the pointer
 *  off the window: at 0% the text stands on the map with
 *  no frame round it. Above 0% it draws them as the panel
 *  does. The grip takes the mouse whether it is drawn or
 *  not.
 *
 *ARGUMENTS:
 *  hideAtZero - the window's hideFrameAtZeroOpacity
 *  bgPct      - the background opacity, 0 to 100
 *  hovered    - the pointer is on the window
 *  moving     - the move drag is on
 *  resizing   - the resize drag is on
 *********************************************************/
static inline bool scnFloatFrameShown(bool hideAtZero, int bgPct,
                                      bool hovered, bool moving,
                                      bool resizing) {
    if (!hideAtZero || bgPct > 0) return true;
    return hovered || moving || resizing;
}

/*********************************************************
 *NAME:          scnTextSavePlace
 *PURPOSE:
 *  Whether the Scenario Text window writes its place to
 *  the preferences this frame, when the place differs from
 *  the saved one. A move or a resize drag saves it. A
 *  window the player has moved before also saves the place
 *  it is pushed to when it is kept inside the main window,
 *  but only while it has a line to show: a window with no
 *  line never changes the saved place by itself.
 *
 *ARGUMENTS:
 *  dragging  - the bar or the grip is being dragged
 *  everMoved - a place has been saved before
 *  haveText  - there is a status line or an announcement
 *********************************************************/
static inline bool scnTextSavePlace(bool dragging, bool everMoved,
                                    bool haveText) {
    return dragging || (everMoved && haveText);
}

/* A position byte as a share of the view, 0 to 1. A byte past
 * SCN_ANNOUNCE_POS_MAX reads as the far edge. */
static inline float scnAnnouncePosShare(uint8_t b) {
    if (b > (uint8_t)SCN_ANNOUNCE_POS_MAX) b = (uint8_t)SCN_ANNOUNCE_POS_MAX;
    return (float)b / (float)SCN_ANNOUNCE_POS_MAX;
}

/*********************************************************
 *NAME:          scnTextLineAcross
 *PURPOSE:
 *  Where one line goes across the source rectangle, and
 *  how much smaller it is drawn. The line's centre goes at
 *  the share across that the script named, then the line
 *  is moved the least it takes to keep it one pad inside
 *  the edges. A line too wide for the room between the pads
 *  is made smaller by itself, keeping its shape, until it
 *  fits that room exactly.
 *
 *ARGUMENTS:
 *  srcW      - the source rectangle's width, in units
 *  lineW     - the line's width at full size, outline
 *              included, in units
 *  share     - the centre's share across, 0 to 1
 *  outX      - receives the line's left edge, in units
 *  outShrink - receives the factor the line is drawn at,
 *              1 for full size
 *********************************************************/
static inline void scnTextLineAcross(float srcW, float lineW, float share,
                                     float *outX, float *outShrink) {
    const float room   = srcW - 2.0f * SCN_TEXT_PAD_UNITS;
    float       shrink = 1.0f;
    float       w      = lineW;
    float       x;

    if (share < 0.0f) share = 0.0f;
    if (share > 1.0f) share = 1.0f;
    if (lineW > room && lineW > 0.0f && room > 0.0f) {
        shrink = room / lineW;
        w      = room;
    }
    x = share * srcW - w * 0.5f;
    if (x > srcW - SCN_TEXT_PAD_UNITS - w) x = srcW - SCN_TEXT_PAD_UNITS - w;
    if (x < SCN_TEXT_PAD_UNITS) x = SCN_TEXT_PAD_UNITS;
    if (outX != NULL) *outX = x;
    if (outShrink != NULL) *outShrink = shrink;
}

/* The sizes the desktop panel's resize drag snaps to, as percents of the
 * size the game's own zoom gives the panel, smallest first. A hundred is
 * that size exactly, which is the size every scenario was laid out against.
 * When the default size below does not fit, the default is one of these,
 * so a player who resizes the panel from it starts on a size the drag
 * knows. */
#define SCN_PANEL_CARDINALS {25, 50, 75, 100, 125, 150, 200, 300}

/* The most of the game view's shorter side the default panel may cover.
 * At zoom 1 the view is 240 pixels, so this lets the default reach 192
 * pixels (150 percent) and keeps a strip of the map clear beside and
 * below it. The view must be at least 288 pixels (zoom 1.2) for the full
 * SCN_PANEL_DEFAULT_PX to fit. */
#define SCN_PANEL_DEFAULT_VIEW_SHARE 0.8f

/* The side in screen pixels of the default panel, at every zoom: 180
 * percent of the 128 units a list is written in at zoom 1, so that its
 * writing is easy to read. It is 0.8 of the 288 it was before, which
 * covered too much of the view. The percent this gives is often not a
 * cardinal (90 at zoom 2, 45 at zoom 4); the drag starts from it all the
 * same, and snaps only when it comes near a cardinal. At zoom 1 the view
 * is too small for this side, and the default is the largest cardinal
 * that fits (150 percent, 192 pixels). */
#define SCN_PANEL_DEFAULT_PX 230.4f

/*********************************************************
 *NAME:          scnPanelDefaultPlace
 *PURPOSE:
 *  Where a desktop scenario panel opens, and how big, when
 *  the player has saved neither: its top-left corner at the
 *  game view's top-left corner, in by inset on both sides.
 *  The corner is never above minTop: the panel's move bar
 *  sits directly above the panel, so minTop is the lowest
 *  the menu bar reaches plus the bar's height, and the bar
 *  is then never under the menu bar or off the top of the
 *  main window.
 *
 *  The side is SCN_PANEL_DEFAULT_PX when that fits, and the
 *  percent is that side over baseSide: 144 at zoom 1.25,
 *  120 at zoom 1.5, 90 at zoom 2, 60 at zoom 3, 45 at zoom
 *  4, 30 at zoom 6, 22.5 at zoom 8. It is not rounded, so
 *  the side is exactly that many pixels. Over zoom 7.2 the
 *  percent is under the 25 of SCN_PANEL_SCALE_MIN, and the
 *  caller's size limits then hold the side at 25 percent.
 *
 *  When SCN_PANEL_DEFAULT_PX does not fit (a small display,
 *  or a zoom under 1.2), the percent is the largest cardinal
 *  from 100 up that fits: 150 (192 pixels) at zoom 1.
 *
 *  A size fits when its side is no more than
 *  SCN_PANEL_DEFAULT_VIEW_SHARE of the view's shorter side
 *  and stays inside the display from the corner it is
 *  placed at.
 *
 *  False when no size fits (not SCN_PANEL_DEFAULT_PX, and
 *  not even 100 percent), or there is no view or no zoom
 *  size: the caller then uses its older placement. The
 *  outputs are left alone on false.
 *
 *ARGUMENTS:
 *  viewX0/viewY0 - the game view's top-left, in render
 *                  pixels
 *  viewX1/viewY1 - its bottom-right
 *  displayW/H    - the display, in render pixels
 *  baseSide      - the panel's side at 100 percent
 *  inset         - the gap from the view's corner
 *  minTop        - the least the top-left's y may be
 *  cardinals     - the cardinal percents, smallest first
 *  count         - how many cardinals there are
 *  outX/outY     - receive the panel's top-left
 *  outPercent    - receives the percent
 *********************************************************/
static inline bool scnPanelDefaultPlace(float viewX0, float viewY0,
                                        float viewX1, float viewY1,
                                        float displayW, float displayH,
                                        float baseSide, float inset,
                                        float minTop,
                                        const int *cardinals, size_t count,
                                        float *outX, float *outY,
                                        float *outPercent) {
    const float viewW = viewX1 - viewX0;
    const float viewH = viewY1 - viewY0;
    const float x     = viewX0 + inset;
    const float y     = (viewY0 + inset < minTop) ? minTop : viewY0 + inset;
    float       room;
    float       best = -1.0f;
    size_t      i;

    if (viewW <= 0.0f || viewH <= 0.0f || baseSide <= 0.0f) return false;
    room = SCN_PANEL_DEFAULT_VIEW_SHARE * ((viewW < viewH) ? viewW : viewH);
    if (displayW - x < room) room = displayW - x;
    if (displayH - y < room) room = displayH - y;

    /* Half a pixel of slack in both tests, so a side that matches the
       room exactly is not lost to rounding. */
    if (SCN_PANEL_DEFAULT_PX <= room + 0.5f) {
        best = SCN_PANEL_DEFAULT_PX * 100.0f / baseSide;
    } else if (cardinals != NULL) {
        for (i = 0; i < count; i++) {
            const int   c    = cardinals[i];
            const float side = baseSide * (float)c / 100.0f;
            if (c < 100) continue;
            if (side > room + 0.5f) break;
            best = (float)c;
        }
    }
    if (best < 0.0f) return false;
    if (outX != NULL) *outX = x;
    if (outY != NULL) *outY = y;
    if (outPercent != NULL) *outPercent = best;
    return true;
}

/* The least width, in screen pixels, the Scenario Text window takes beside
 * a default scenario panel (scnTextBesidePanelPlace). At this width a line
 * of the 240-unit source is about 13 pixels high. With less room than this
 * the window keeps its older default, the top middle of the game view. */
#define SCN_TEXT_BESIDE_MIN_W_PX 160.0f

/*********************************************************
 *NAME:          scnTextBesidePanelPlace
 *PURPOSE:
 *  Where the Scenario Text window's text area goes, and how
 *  big it is, when the player has saved no place for it
 *  and a scenario panel is at its own default place at the
 *  left of the game view: in the room from the panel's
 *  right edge to the game view's right edge, as high as
 *  the panel.
 *
 *  The left edge is one inset right of the panel, and the
 *  right edge is one inset left of the view's right edge
 *  (or of the display's, when the display ends first). The
 *  top is the panel's top and the bottom is the panel's
 *  bottom, so the text area lines up with the panel and
 *  the two move bars line up above them.
 *
 *  False when that room is narrower than minW, or the
 *  numbers give no room at all, or the panel has no
 *  height: the caller then keeps its older default. The
 *  outputs are left alone on false.
 *
 *ARGUMENTS:
 *  panelRight  - the panel's right edge, in render pixels
 *  panelTop    - the panel's top edge (below its bar)
 *  panelBottom - the panel's bottom edge
 *  viewRight   - the game view's right edge
 *  displayW    - the display's width
 *  inset       - the gap kept on both sides
 *  minW        - the least width the text area may have
 *  outX/outY   - receive the text area's top-left
 *  outW/outH   - receive the text area's width and height
 *********************************************************/
static inline bool scnTextBesidePanelPlace(float panelRight, float panelTop,
                                           float panelBottom,
                                           float viewRight, float displayW,
                                           float inset, float minW,
                                           float *outX, float *outY,
                                           float *outW, float *outH) {
    const float right = ((viewRight < displayW) ? viewRight : displayW) - inset;
    const float x     = panelRight + inset;
    const float w     = right - x;
    const float h     = panelBottom - panelTop;
    if (w <= 0.0f || w < minW || h <= 0.0f) return false;
    if (outX != NULL) *outX = x;
    if (outY != NULL) *outY = panelTop;
    if (outW != NULL) *outW = w;
    if (outH != NULL) *outH = h;
    return true;
}


/*********************************************************
 *NAME:          scnPanelColourRGBA
 *PURPOSE:
 *  The palette entry an index names, as RGBA bytes.
 *
 *  False for an index that draws nothing — 0, one of the
 *  four reserved entries, or one past the palette — and the
 *  outputs are left alone. Any output pointer may be NULL.
 *
 *  The table is in scenario_panel_draw.cpp and this is the
 *  way out of it, so the panel drawn through ImGui and the
 *  map marker drawn through the SDL renderer take their
 *  sixteen colours from one place rather than from two
 *  copies that would drift the first time a skin changed
 *  one.
 *
 *ARGUMENTS:
 *  index   - a ScnPanelColour
 *  r/g/b/a - receive the colour; each may be NULL
 *********************************************************/
bool scnPanelColourRGBA(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b,
                        uint8_t *a);

/* What the drawer needs from the frontend around it.
 *
 * playerName answers the name a slot is playing under, or NULL for a slot
 * nobody holds; the whole callback may be NULL where a frontend has no
 * roster to ask. Either way a name primitive draws nothing, which is what
 * keeps a script from sending names itself and what makes a rename show
 * through without an update.
 *
 * tick is the game tick the viewer is on, for the timer primitive. On the
 * client that is the last tick heard from the server, which is the clock the
 * scenario counted in.
 *
 * scale is screen pixels per panel unit: one at the game's zoom 1, and the
 * zoom from there.
 *
 * alpha is how solid the frontend wants the scenario's drawing, from 0 for
 * gone to 1 for as the script asked. It multiplies the alpha channel of
 * every colour this file resolves, so it fades what was drawn and never
 * touches a pixel nothing was drawn on: a square the script left empty is
 * as transparent at 0.2 as it is at 1. It is the scenario's drawing only.
 * A frontend's own chrome around the square — a border, a grip, a settings
 * window — is not on this list and does not fade with it, or a panel taken
 * to nothing would leave the player nothing to take it back with.
 *
 * There is no "unset": a zeroed env is a panel that draws nothing at all,
 * so a frontend that builds one of these must fill this in. 1.0f is the
 * value that means "as it was before this existed".
 *
 * tiles is the skin's tile sheet as the frontend's ImGui backend takes it —
 * the SDL_Texture the desktop client and the log viewer each keep their own
 * accessor for, which is why it arrives here rather than being fetched. NULL
 * draws no sprite. The sheet's own pixel size is not needed: the atlas
 * lookup is in the classic 1x layout and the sheet is that layout scaled
 * whole, so the scale cancels out of the texture coordinates.
 */
typedef struct {
    const char *(*playerName)(void *ctx, uint8_t slot); /* may be NULL */
    void       *ctx;
    uint32_t    tick;
    float       scale;
    float       alpha;
    void       *tiles;
} ScnPanelDrawEnv;

/*********************************************************
 *NAME:          scnPanelDraw
 *PURPOSE:
 *  Draws one decoded list into the current ImGui window's
 *  draw list, with the square's top-left at (originX,
 *  originY) in screen pixels.
 *
 *  Every primitive is clipped to the square. The parser
 *  lets a primitive start inside the square and run past
 *  its edge on purpose, so the clip is what keeps a rect
 *  the script placed badly from painting over the game.
 *
 *  A NULL list, an empty one, or a NULL env draws nothing.
 *
 *ARGUMENTS:
 *  list            - the decoded list to draw
 *  originX/originY - the square's top-left, in screen pixels
 *  env             - what the frontend around it supplies
 *********************************************************/
void scnPanelDraw(const ScnPanelList *list, float originX, float originY,
                  const ScnPanelDrawEnv *env);

#ifdef __cplusplus
}
#endif

#endif /* SCENARIO_PANEL_DRAW_H */
