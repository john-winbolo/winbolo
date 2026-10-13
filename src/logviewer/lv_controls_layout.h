/*
 * lv_controls_layout.h - where the items of the Controls panel go
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * The Controls panel has a top row (the playback buttons, the speed and the
 * "Overlay events" list) and a bottom row (the time, the time left and the
 * tick). Each is put in the middle of the panel's width. A row that does not
 * go on one line takes more lines, and each line is put in the middle by
 * itself. The sums are here, with no ImGui in them, so the unit tests can
 * run them for every width (tests/unit/test_lv_controls_layout.c).
 *
 * All lengths are in the units the caller draws in. x is from the left edge
 * of the panel's content.
 */

#ifndef LV_CONTROLS_LAYOUT_H
#define LV_CONTROLS_LAYOUT_H

#include <stdbool.h>

/* A playback button is this many times as wide as it is high, when the
 * panel has the room. */
#define LV_CONTROLS_BTN_WIDE 2.0f

/* The most playback buttons: four, and "to the live head" on a live feed. */
#define LV_CONTROLS_MAX_BUTTONS 5

/* The slider of the speed is never drawn narrower than this. */
#define LV_CONTROLS_SPEED_FLOOR 16.0f

/* The start of a line that is lineW wide, so it is in the middle of a
 * content that is contentW wide. A line wider than the content starts at
 * the left edge. Whole units, so nothing is drawn between two pixels. */
static inline float lvControlsCentreX(float lineW, float contentW) {
    float x = (contentW - lineW) * 0.5f;
    if (x < 0.0f) return 0.0f;
    return (float)(int)x;
}

/* What the top row is made of. */
typedef struct {
    float contentW;     /* the width of the panel's content */
    int buttons;        /* 4, or 5 on a live feed */
    float btnSide;      /* the height of a button: its width when narrow */
    float btnGap;       /* between two buttons */
    float labelW;       /* the speed's label with the space after it */
    float speedMin;     /* the slider gives up width down to this */
    float speedMax;     /* and is never wider than this */
    float comboFullW;   /* "Overlay events" with its name on it */
    float comboArrowW;  /* only its arrow */
    float gap;          /* before the speed and before the list */
} LvControlsTopIn;

/* Where each item of the top row goes. */
typedef struct {
    float btnW;                             /* the width of every button */
    int btnLine[LV_CONTROLS_MAX_BUTTONS];
    float btnX[LV_CONTROLS_MAX_BUTTONS];
    bool label;                             /* the speed's label is drawn */
    int speedLine;
    float labelX;                           /* when label */
    float speedX, speedW;                   /* the slider */
    int comboLine;
    float comboX;
    bool comboFull;                         /* with its name on it */
    int lines;                              /* how many lines the row takes */
    float lineW[LV_CONTROLS_MAX_BUTTONS + 2];
} LvControlsTop;

/* The top row.
 *
 * The buttons: twice as wide as high when all of them go on one line like
 * that. In less room they all get narrower by the same amount, down to a
 * square. In less room than that they stay square and go on more lines.
 *
 * The speed: its label and its slider, after the buttons where there is
 * room for the slider at its least width. The slider gives up width first.
 * Under that the two go on a line of their own, and in a panel too narrow
 * for both the label goes and the slider keeps the line.
 *
 * The list: after the slider with its name on it where there is room, with
 * only its arrow in less, and on a line of its own when not even that goes.
 * The slider leaves room for the arrow when it can spare it.
 *
 * Then each line is put in the middle. */
static inline LvControlsTop lvControlsTopLayout(const LvControlsTopIn *in) {
    LvControlsTop o;
    float start[LV_CONTROLS_MAX_BUTTONS + 2];
    float W = in->contentW;
    int n = in->buttons;
    int perLine, i, line;
    float used, room, spare, oneLine;

    if (n < 1) n = 1;
    if (n > LV_CONTROLS_MAX_BUTTONS) n = LV_CONTROLS_MAX_BUTTONS;
    if (W < 0.0f) W = 0.0f;
    for (i = 0; i < LV_CONTROLS_MAX_BUTTONS + 2; i++) {
        o.lineW[i] = 0.0f;
        start[i] = 0.0f;
    }
    for (i = 0; i < LV_CONTROLS_MAX_BUTTONS; i++) {
        o.btnLine[i] = 0;
        o.btnX[i] = 0.0f;
    }

    /* The buttons. */
    oneLine = (float)n * in->btnSide + (float)(n - 1) * in->btnGap;
    if (oneLine <= W) {
        float w = (float)(int)((W - (float)(n - 1) * in->btnGap) / (float)n);
        float wide = (float)(int)(in->btnSide * LV_CONTROLS_BTN_WIDE);
        if (w > wide) w = wide;
        if (w < in->btnSide) w = in->btnSide;
        o.btnW = w;
        perLine = n;
    } else {
        o.btnW = in->btnSide;
        perLine = (int)((W + in->btnGap) / (in->btnSide + in->btnGap));
        if (perLine < 1) perLine = 1;
    }
    line = 0;
    used = 0.0f;
    for (i = 0; i < n; i++) {
        if (i > 0 && (i % perLine) == 0) {
            o.lineW[line] = used;
            line++;
            used = 0.0f;
        }
        if (used > 0.0f) used += in->btnGap;
        o.btnLine[i] = line;
        o.btnX[i] = used;
        used += o.btnW;
    }

    /* The speed. */
    o.label = true;
    room = W - used - in->gap - in->labelW;
    if (room >= in->speedMin) {
        used += in->gap;
    } else {
        o.lineW[line] = used;
        line++;
        used = 0.0f;
        room = W - in->labelW;
        if (room < in->speedMin) {
            o.label = false;
            room = W;
        }
    }
    spare = in->gap + in->comboArrowW;
    if (room - spare >= in->speedMin) {
        room -= spare;
    }
    o.speedW = (room < in->speedMax) ? room : in->speedMax;
    if (o.speedW < LV_CONTROLS_SPEED_FLOOR) o.speedW = LV_CONTROLS_SPEED_FLOOR;
    o.speedLine = line;
    o.labelX = used;
    if (o.label) used += in->labelW;
    o.speedX = used;
    used += o.speedW;

    /* The list. */
    room = W - used - in->gap;
    if (room >= in->comboFullW) {
        o.comboFull = true;
        used += in->gap;
    } else if (room >= in->comboArrowW) {
        o.comboFull = false;
        used += in->gap;
    } else {
        o.lineW[line] = used;
        line++;
        used = 0.0f;
        o.comboFull = (W >= in->comboFullW);
    }
    o.comboLine = line;
    o.comboX = used;
    used += o.comboFull ? in->comboFullW : in->comboArrowW;
    o.lineW[line] = used;
    o.lines = line + 1;

    /* Each line in the middle. */
    for (i = 0; i < o.lines; i++) {
        start[i] = lvControlsCentreX(o.lineW[i], W);
    }
    for (i = 0; i < n; i++) {
        o.btnX[i] += start[o.btnLine[i]];
    }
    o.labelX += start[o.speedLine];
    o.speedX += start[o.speedLine];
    o.comboX += start[o.comboLine];
    return o;
}

/* A row of n pieces of text, each w[i] wide, with gap between two on a
 * line. A piece goes on the line while it has room there and starts a new
 * line when it has not; a piece wider than the content has a line to itself
 * and starts at the left edge (it is then drawn on more lines). Each line is
 * put in the middle. Gives each piece its line and its x; returns how many
 * lines the row takes. */
static inline int lvControlsFlow(const float *w, int n, float gap, float contentW,
                                 int *lineOut, float *xOut) {
    int i, j, line = 0, first = 0;
    float used = 0.0f;

    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        if (i > first && used + gap + w[i] > contentW) {
            float x0 = lvControlsCentreX(used, contentW);
            for (j = first; j < i; j++) xOut[j] += x0;
            line++;
            first = i;
            used = 0.0f;
        }
        if (i > first) used += gap;
        lineOut[i] = line;
        xOut[i] = used;
        used += w[i];
    }
    {
        float x0 = lvControlsCentreX(used, contentW);
        for (j = first; j < n; j++) xOut[j] += x0;
    }
    return line + 1;
}

/* The zoom of the map is the last piece of the bottom row. It gives way
 * first: it has its whole text ("Zoom 150%") while that adds no line to the
 * row, then only the number ("150%"), then it is left out. So it never
 * makes the panel higher and never pushes another piece off its line. */
typedef enum {
    LV_ZOOM_FORM_FULL = 0,
    LV_ZOOM_FORM_SHORT,
    LV_ZOOM_FORM_NONE
} LvZoomForm;

/* The most pieces the row has before the zoom. */
#define LV_CONTROLS_ROW_MAX 7

/* w are the n pieces in front of it (n at most LV_CONTROLS_ROW_MAX); fullW
 * and shortW the widths of its two forms. */
static inline LvZoomForm lvControlsZoomForm(const float *w, int n, float fullW, float shortW,
                                            float gap, float contentW) {
    float all[LV_CONTROLS_ROW_MAX + 1], x[LV_CONTROLS_ROW_MAX + 1];
    int line[LV_CONTROLS_ROW_MAX + 1];
    int i, lines;
    if (n < 0) n = 0;
    if (n > LV_CONTROLS_ROW_MAX) n = LV_CONTROLS_ROW_MAX;
    for (i = 0; i < n; i++) all[i] = w[i];
    lines = (n > 0) ? lvControlsFlow(all, n, gap, contentW, line, x) : 1;
    all[n] = fullW;
    if (fullW <= contentW && lvControlsFlow(all, n + 1, gap, contentW, line, x) == lines) {
        return LV_ZOOM_FORM_FULL;
    }
    all[n] = shortW;
    if (shortW <= contentW && lvControlsFlow(all, n + 1, gap, contentW, line, x) == lines) {
        return LV_ZOOM_FORM_SHORT;
    }
    return LV_ZOOM_FORM_NONE;
}

#endif /* LV_CONTROLS_LAYOUT_H */
