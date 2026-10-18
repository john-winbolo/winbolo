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
 */

/*********************************************************
 *Name:          Scenario Panel Preferences
 *Filename:      scn_panel_prefs.c
 *Purpose:
 *  See scn_panel_prefs.h.
 *********************************************************/

#include "scn_panel_prefs.h"

#include <stdio.h>

static int scnClampSide(int v) {
    if (v < SCN_PANEL_POPOUT_MIN_PX) return SCN_PANEL_POPOUT_MIN_PX;
    if (v > SCN_PANEL_POPOUT_MAX_PX) return SCN_PANEL_POPOUT_MAX_PX;
    return v;
}

bool scnPanelPopoutParse(const char *row, ScnPanelPopout *out) {
    int  open, x, y, w, h;
    char tail;
    if (row == NULL || out == NULL) return false;
    /* The trailing %c catches a row with more after the fifth number, which
       is not a row this code wrote. */
    if (sscanf(row, "%d,%d,%d,%d,%d%c", &open, &x, &y, &w, &h, &tail) != 5) {
        return false;
    }
    if (open != 0 && open != 1) return false;
    out->open = (open == 1);
    out->x    = x;
    out->y    = y;
    out->w    = scnClampSide(w);
    out->h    = scnClampSide(h);
    return true;
}

void scnPanelPopoutFormat(const ScnPanelPopout *in, char *buf, size_t bufSz) {
    if (buf == NULL || bufSz == 0) return;
    if (in == NULL) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, bufSz, "%d,%d,%d,%d,%d", in->open ? 1 : 0, in->x, in->y,
             in->w, in->h);
}

bool scnPanelYesNoParse(const char *word, bool fallback) {
    if (word == NULL) return fallback;
    switch (word[0]) {
    case 'Y': case 'y': case '1': return true;
    case 'N': case 'n': case '0': return false;
    default:                      return fallback;
    }
}

const char *scnPanelYesNoWord(bool yes) { return yes ? "Yes" : "No"; }

/* Copied a character at a time rather than with snprintf because a control
   character in a script's name would reach the preferences file as an
   escape and make the row impossible to match up by eye. The read and the
   write both come through here, so both still name the same row. A byte
   above 0x7F is left as it is: those are the middle of a UTF-8 character
   in a name somebody chose, not a control code. */
void scnPanelPrefsKey(const char *script, char *key, size_t keySz) {
    size_t i = 0;
    if (key == NULL || keySz == 0) return;
    if (script != NULL) {
        while (script[i] != '\0' && i + 1 < keySz) {
            key[i] = ((unsigned char)script[i] >= 0x20) ? script[i] : '_';
            i++;
        }
    }
    key[i] = '\0';
}

void scnPanelPrefsPopoutKey(const char *script, int panel, char *key,
                            size_t keySz) {
    size_t len;
    if (key == NULL || keySz == 0) return;
    /* The name is cut where the other rows' key cuts it, 8 short of this
       buffer, so the three rows of one long-named script still match. */
    scnPanelPrefsKey(script, key, keySz > 8 ? keySz - 8 : keySz);
    len = 0;
    while (key[len] != '\0') len++;
    snprintf(key + len, keySz - len, "#%d", panel);
}

/* How far two spans [a0, a0+aw) and [b0, b0+bw) overlap, 0 for not at all. */
static int scnOverlap(int a0, int aw, int b0, int bw) {
    long lo = (a0 > b0) ? a0 : b0;
    long hi = ((long)a0 + aw < (long)b0 + bw) ? (long)a0 + aw : (long)b0 + bw;
    return (hi > lo) ? (int)(hi - lo) : 0;
}

bool scnPanelRescueRect(const ScnPanelRect *win, const ScnPanelRect *displays,
                        int n, const ScnPanelRect *primaryUsable,
                        ScnPanelRect *out) {
    int i;
    int needW, needH;
    ScnPanelRect r;

    if (win == NULL || displays == NULL || n <= 0 || primaryUsable == NULL ||
        out == NULL) {
        return false;
    }

    /* A window smaller than the strip only has to show what it has. */
    needW = (win->w < SCN_PANEL_RESCUE_MIN_PX) ? win->w : SCN_PANEL_RESCUE_MIN_PX;
    needH = (win->h < SCN_PANEL_RESCUE_MIN_PX) ? win->h : SCN_PANEL_RESCUE_MIN_PX;
    if (needW < 1) needW = 1;
    if (needH < 1) needH = 1;

    for (i = 0; i < n; i++) {
        const ScnPanelRect *d = &displays[i];
        if (scnOverlap(win->x, win->w, d->x, d->w) >= needW &&
            scnOverlap(win->y, needH, d->y, d->h) >= (needH + 1) / 2) {
            return false;
        }
    }

    /* Lost: onto the primary display, whole and centred. */
    r.w = win->w;
    r.h = win->h;
    if (r.w > primaryUsable->w) r.w = primaryUsable->w;
    if (r.h > primaryUsable->h) r.h = primaryUsable->h;
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = primaryUsable->x + (primaryUsable->w - r.w) / 2;
    r.y = primaryUsable->y + (primaryUsable->h - r.h) / 2;
    *out = r;
    return true;
}

int scnOpacityClamp(int pct) {
    if (pct == -1) return -1;
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

void scnOpacityMigrate(int oldPct, int *bgPct, int *textPct) {
    int v = scnOpacityClamp(oldPct);
    if (v == -1) {
        if (bgPct != NULL) *bgPct = -1;
        if (textPct != NULL) *textPct = -1;
        return;
    }
    if (bgPct != NULL) *bgPct = (v * SCN_BG_OPACITY_DEFAULT + 50) / 100;
    if (textPct != NULL) *textPct = v;
}

/* One saved percent from its string: "" or a string that does not start
   with a whole number is -1, never set. */
static int scnOpacityRead(const char *s) {
    int v;
    if (s == NULL || s[0] == '\0') return -1;
    if (sscanf(s, "%d", &v) != 1) return -1;
    return scnOpacityClamp(v);
}

void scnOpacityFromPrefs(const char *bgStr, const char *textStr,
                         const char *oldStr, int *bgPct, int *textPct) {
    const bool haveBg   = (bgStr != NULL && bgStr[0] != '\0');
    const bool haveText = (textStr != NULL && textStr[0] != '\0');
    if (!haveBg && !haveText) {
        scnOpacityMigrate(scnOpacityRead(oldStr), bgPct, textPct);
        return;
    }
    if (bgPct != NULL) *bgPct = scnOpacityRead(bgStr);
    if (textPct != NULL) *textPct = scnOpacityRead(textStr);
}

int scnBackingAlpha(int bgPct) {
    int v = scnOpacityClamp(bgPct);
    if (v == -1) v = SCN_BG_OPACITY_DEFAULT;
    return (v * 255 + 50) / 100;
}

/* One whole number and nothing after it. */
static bool scnReadWhole(const char *s, int *out) {
    int  v;
    char tail;
    if (s == NULL || s[0] == '\0') return false;
    if (sscanf(s, "%d%c", &v, &tail) != 1) return false;
    *out = v;
    return true;
}

bool scnPanelLayoutParse(const char *row, const char *bgRow, int *x, int *y,
                         int *scale, int *bgPct, int *textPct) {
    int  rx, ry, rscale, a, b, bgv;
    char tail;
    int  n;
    if (row == NULL || x == NULL || y == NULL || scale == NULL ||
        bgPct == NULL || textPct == NULL) {
        return false;
    }
    /* The trailing %c catches a row with more after the last number, which
       is not a row this code or the code before it wrote. */
    n = sscanf(row, "%d,%d,%d,%d,%d%c", &rx, &ry, &rscale, &a, &b, &tail);
    if (n == 5) {
        /* An earlier build of this change: both opacities on the row. */
        *bgPct   = scnOpacityClamp(a);
        *textPct = scnOpacityClamp(b);
    } else if (n == 4 &&
               sscanf(row, "%d,%d,%d,%d%c", &rx, &ry, &rscale, &a, &tail) ==
                   4) {
        if (scnReadWhole(bgRow, &bgv)) {
            /* This code's row: the text opacity, and the background kept
               in its own section. */
            *bgPct   = scnOpacityClamp(bgv);
            *textPct = scnOpacityClamp(a);
        } else {
            /* The row main and 2.1 write: one opacity for both. */
            scnOpacityMigrate(a, bgPct, textPct);
        }
    } else {
        return false;
    }
    *x     = rx;
    *y     = ry;
    *scale = rscale;
    return true;
}

void scnPanelLayoutFormat(int x, int y, int scale, int textPct, char *buf,
                          size_t bufSz) {
    if (buf == NULL || bufSz == 0) return;
    snprintf(buf, bufSz, "%d,%d,%d,%d", x, y, scale, textPct);
}

void scnPanelLayoutBgFormat(int bgPct, char *buf, size_t bufSz) {
    if (buf == NULL || bufSz == 0) return;
    snprintf(buf, bufSz, "%d", bgPct);
}
