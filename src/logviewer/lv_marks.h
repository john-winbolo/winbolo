/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer map marks
 *Filename:      lv_marks.h
 *Purpose:
 *  Shift and a drag of the left mouse button draws a line on
 *  the Log Viewer's map, for pointing something out while the
 *  screen is recorded. A line stays a moment after the button
 *  comes up, then fades out and is gone.
 *
 *  The points are kept in map pixels at zoom 1, the same
 *  units the pan is counted in, so a line stays on the part
 *  of the map it was drawn on when the map is zoomed or
 *  moved. imgui_game_viewport.cpp turns them into points on
 *  the screen and draws them.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same code.
 *********************************************************/

#ifndef LV_MARKS_H
#define LV_MARKS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* How many lines are kept at once; one more drops the oldest. */
#define LV_MARKS_MAX_STROKES 32
/* The points in one line. A longer drag carries on as a new line that
 * starts where the full one ends, so it looks like one line. */
#define LV_MARKS_MAX_POINTS  512
/* How long a line stays whole after the button comes up, and how long it
 * then takes to fade out. */
#define LV_MARKS_HOLD_MS     2000u
#define LV_MARKS_FADE_MS     1000u

typedef struct {
    float x, y;
} LvMarkPoint;

typedef struct {
    LvMarkPoint pts[LV_MARKS_MAX_POINTS];
    int         count;
    /* Still being drawn: the button is down. */
    bool        open;
    /* When the button came up. */
    uint64_t    endedMs;
} LvMarkStroke;

/* The lines, oldest first, in a ring of LV_MARKS_MAX_STROKES. */
typedef struct {
    LvMarkStroke strokes[LV_MARKS_MAX_STROKES];
    int          first;
    int          count;
} LvMarks;

static inline void lvMarksClear(LvMarks *m) {
    m->first = 0;
    m->count = 0;
}

/* The nth line, 0 the oldest. */
static inline LvMarkStroke *lvMarksAt(LvMarks *m, int n) {
    return &m->strokes[(m->first + n) % LV_MARKS_MAX_STROKES];
}

/* How solid a line is drawn at nowMs: 1 while it is drawn and for the hold
 * after, falling to 0 over the fade. */
static inline float lvMarksAlpha(const LvMarkStroke *s, uint64_t nowMs) {
    uint64_t gone;

    if (s->open) {
        return 1.0f;
    }
    gone = (nowMs > s->endedMs) ? nowMs - s->endedMs : 0;
    if (gone < LV_MARKS_HOLD_MS) {
        return 1.0f;
    }
    if (gone >= LV_MARKS_HOLD_MS + LV_MARKS_FADE_MS) {
        return 0.0f;
    }
    return 1.0f - (float)(gone - LV_MARKS_HOLD_MS) / (float)LV_MARKS_FADE_MS;
}

/* Drops the lines that have faded out. Lines end in the order they were
 * started, so the faded ones are always the oldest. */
static inline void lvMarksPrune(LvMarks *m, uint64_t nowMs) {
    while (m->count > 0 && lvMarksAlpha(lvMarksAt(m, 0), nowMs) <= 0.0f) {
        m->first = (m->first + 1) % LV_MARKS_MAX_STROKES;
        m->count--;
    }
}

/* Starts a line at (x, y). */
static inline void lvMarksBegin(LvMarks *m, float x, float y) {
    LvMarkStroke *s;

    if (m->count == LV_MARKS_MAX_STROKES) {
        m->first = (m->first + 1) % LV_MARKS_MAX_STROKES;
        m->count--;
    }
    s = lvMarksAt(m, m->count);
    m->count++;
    s->pts[0].x = x;
    s->pts[0].y = y;
    s->count = 1;
    s->open = true;
    s->endedMs = 0;
}

/* The line being drawn, NULL when the button is up. */
static inline LvMarkStroke *lvMarksOpen(LvMarks *m) {
    LvMarkStroke *s;

    if (m->count == 0) {
        return NULL;
    }
    s = lvMarksAt(m, m->count - 1);
    return s->open ? s : NULL;
}

/* Adds (x, y) to the line being drawn when it is at least minStep from the
 * last point kept, so a mouse held still adds nothing. */
static inline void lvMarksAdd(LvMarks *m, float x, float y, float minStep) {
    LvMarkStroke *s = lvMarksOpen(m);
    LvMarkPoint   last;
    float         dx, dy;

    if (s == NULL) {
        return;
    }
    last = s->pts[s->count - 1];
    dx = x - last.x;
    dy = y - last.y;
    if (dx * dx + dy * dy < minStep * minStep) {
        return;
    }
    if (s->count == LV_MARKS_MAX_POINTS) {
        /* Full: the drag goes on in a new line from the same point. The
         * full one stays open, so both end when the button comes up. */
        lvMarksBegin(m, last.x, last.y);
        s = lvMarksOpen(m);
    }
    s->pts[s->count].x = x;
    s->pts[s->count].y = y;
    s->count++;
}

/* The button came up: every line still open starts its hold now. */
static inline void lvMarksEnd(LvMarks *m, uint64_t nowMs) {
    int i;

    for (i = 0; i < m->count; i++) {
        LvMarkStroke *s = lvMarksAt(m, i);
        if (s->open) {
            s->open = false;
            s->endedMs = nowMs;
        }
    }
}

#endif /* LV_MARKS_H */
