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
