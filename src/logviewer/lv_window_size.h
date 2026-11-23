/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer fixed window size
 *Filename:      lv_window_size.h
 *Purpose:
 *  "-size 1920x1080" on the standalone Log Viewer's command
 *  line opens its window with a drawing area of exactly that
 *  many pixels, and keeps it there, for recording the screen
 *  at a size a video wants. This reads the text, and turns
 *  pixels into the units SDL sizes a window in.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same code.
 *********************************************************/

#ifndef LV_WINDOW_SIZE_H
#define LV_WINDOW_SIZE_H

#include <stdbool.h>
#include <stdlib.h>

/* The smallest and largest sizes taken. */
#define LV_WINDOW_SIZE_MIN_W 320
#define LV_WINDOW_SIZE_MIN_H 240
#define LV_WINDOW_SIZE_MAX   16384

/* Reads "WxH" (an x or an X between two whole numbers, nothing else) into
 * *w and *h. A size outside the limits above is refused. */
static inline bool lvWindowSizeParse(const char *text, int *w, int *h) {
    char *end = NULL;
    long  pw, ph;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    pw = strtol(text, &end, 10);
    if (end == NULL || (*end != 'x' && *end != 'X')) {
        return false;
    }
    text = end + 1;
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    ph = strtol(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return false;
    }
    if (pw < LV_WINDOW_SIZE_MIN_W || pw > LV_WINDOW_SIZE_MAX ||
        ph < LV_WINDOW_SIZE_MIN_H || ph > LV_WINDOW_SIZE_MAX) {
        return false;
    }
    *w = (int)pw;
    *h = (int)ph;
    return true;
}

/* SDL sizes a window in its own units, which are pixels divided by the
 * window's pixel density (2 on a Retina Mac, the display scale where the
 * system does the same). The units for px pixels, rounded to the nearest. */
static inline int lvWindowSizeUnits(int px, float density) {
    if (density <= 0.0f) {
        density = 1.0f;
    }
    return (int)((float)px / density + 0.5f);
}

#endif /* LV_WINDOW_SIZE_H */
