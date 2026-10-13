/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_dock_layout.h - the parts of the log viewer's sidebar layout that are
 * kept in the viewer's own text, not ImGui's.
 *
 * Two things. The sidebar options: which of the four are on and how big each
 * is. And a sidebar's arrangement while it is off: how it was split, which
 * panels sat in which part and which tab was in front, so that it comes back
 * as it was left. (A sidebar that is on is in ImGui's own settings text with
 * the floating windows.)
 *
 * Header only and free of SDL and ImGui, so reading and writing can be tested
 * on their own (tests/unit/test_lv_dock_fit.c). Text read here comes from a
 * preferences file a user or an older or newer viewer may have written: a
 * fault in it gives the defaults, never a failure.
 */

#ifndef LV_DOCK_LAYOUT_H
#define LV_DOCK_LAYOUT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lv_dock_fit.h"

/* ---- The sidebar options -------------------------------------------------
 *
 * One line: "v1 R=1,420 L=0,420 T=0,220 B=1,180 I=1". A letter a side, then
 * on or off and the size asked for in window points. I says the right
 * sidebar has had its first layout (every panel stacked in it), which
 * happens once. */

/* No sidebar is asked to be bigger than this; the fit to the window makes it
 * smaller. It only keeps a damaged number from being believed. */
#define LV_DOCK_SIZE_MAX 8192

typedef struct {
    int enabled[LV_DOCK_SIDES];
    int size[LV_DOCK_SIDES];
    int rightLaidOut;
} LvDockSides;

static inline char lvDockSideLetter(int side) {
    switch (side) {
        case LV_DOCK_RIGHT:  return 'R';
        case LV_DOCK_LEFT:   return 'L';
        case LV_DOCK_TOP:    return 'T';
        case LV_DOCK_BOTTOM: return 'B';
        default:             return '?';
    }
}

static inline void lvDockSidesDefault(LvDockSides *s) {
    int i;
    for (i = 0; i < LV_DOCK_SIDES; i++) {
        s->enabled[i] = 0;
        s->size[i] = lvDockSideDefault(i);
    }
    s->rightLaidOut = 0;
}

/* A size as it may be kept: at least the side's smallest, at most the cap. */
static inline int lvDockSidesClampSize(int side, int size) {
    if (size < lvDockSideMin(side)) size = lvDockSideMin(side);
    if (size > LV_DOCK_SIZE_MAX) size = LV_DOCK_SIZE_MAX;
    return size;
}

/* A whole number of at most five digits at *p; moves *p past it. Returns 0
 * and leaves *p alone when there is none. */
static inline int lvDockReadInt(const char **p, int *out) {
    const char *q = *p;
    int v = 0, digits = 0;
    while (*q >= '0' && *q <= '9' && digits < 5) {
        v = v * 10 + (*q - '0');
        q++;
        digits++;
    }
    if (digits == 0 || (*q >= '0' && *q <= '9')) return 0;
    *out = v;
    *p = q;
    return 1;
}

/* Read the options line. Returns 1 when the text is this format, 0 when it
 * is not (empty, cut off before the version, or something else), and then
 * out holds the defaults. A side that is missing or damaged keeps its
 * default; the sides that read correctly are kept, and what is not known is
 * passed over, so a line a newer viewer wrote still reads. */
static inline int lvDockSidesParse(const char *text, LvDockSides *out) {
    const char *p;

    lvDockSidesDefault(out);
    if (text == NULL || strncmp(text, "v1", 2) != 0) return 0;
    p = text + 2;
    if (*p != '\0' && *p != ' ') return 0;

    while (*p != '\0') {
        const char *q;
        int side = -1, i, a = 0, b = 0;

        while (*p == ' ') p++;
        if (*p == '\0') break;
        for (i = 0; i < LV_DOCK_SIDES; i++) {
            if (*p == lvDockSideLetter(i)) side = i;
        }
        q = p + 1;
        if (side >= 0 && *q == '=') {
            q++;
            if (lvDockReadInt(&q, &a) && a <= 1 && *q == ',') {
                q++;
                if (lvDockReadInt(&q, &b) && (*q == ' ' || *q == '\0')) {
                    out->enabled[side] = a;
                    out->size[side] = lvDockSidesClampSize(side, b);
                }
            }
        } else if (*p == 'I' && *q == '=') {
            q++;
            if (lvDockReadInt(&q, &a) && a <= 1 && (*q == ' ' || *q == '\0')) {
                out->rightLaidOut = a;
            }
        }
        /* On to the next word, whatever this one was. */
        while (*p != '\0' && *p != ' ') p++;
    }
    return 1;
}

/* Write the options line. Returns its length, or -1 when it does not fit
 * (out then holds an empty string). */
static inline int lvDockSidesFormat(const LvDockSides *s, char *out, size_t size) {
    int n;
    if (out == NULL || size == 0) return -1;
    n = snprintf(out, size, "v1 R=%d,%d L=%d,%d T=%d,%d B=%d,%d I=%d",
                 s->enabled[LV_DOCK_RIGHT] ? 1 : 0,
                 lvDockSidesClampSize(LV_DOCK_RIGHT, s->size[LV_DOCK_RIGHT]),
                 s->enabled[LV_DOCK_LEFT] ? 1 : 0,
                 lvDockSidesClampSize(LV_DOCK_LEFT, s->size[LV_DOCK_LEFT]),
                 s->enabled[LV_DOCK_TOP] ? 1 : 0,
                 lvDockSidesClampSize(LV_DOCK_TOP, s->size[LV_DOCK_TOP]),
                 s->enabled[LV_DOCK_BOTTOM] ? 1 : 0,
                 lvDockSidesClampSize(LV_DOCK_BOTTOM, s->size[LV_DOCK_BOTTOM]),
                 s->rightLaidOut ? 1 : 0);
    if (n < 0 || (size_t)n >= size) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

/* ---- A sidebar's arrangement ----------------------------------------------
 *
 * A tree. A part that is split is "X350(first)(second)" or "Y350(...)(...)":
 * X side by side, Y one above the other, and the number the first part's
 * share in thousandths. A part that holds panels is "L[###a,###b|1]": the
 * panels' window ids in tab order, then the tab in front counted from 0.
 * "L*[...]" is the part that takes up the slack when the sidebar changes
 * size (ImGui's central node); it may be empty, "L*[]". */

#define LV_DOCK_TREE_NODES 31
#define LV_DOCK_TREE_WINS  12
#define LV_DOCK_TREE_NAME  32
#define LV_DOCK_TREE_DEPTH 12
#define LV_DOCK_TREE_TEXT_MAX 2048

typedef struct {
    char axis;          /* 'X' or 'Y' for a split, 0 for a part with panels */
    int  permille;      /* a split: the first part's share, 1..999 */
    int  child[2];      /* a split: the two parts, as indexes into node[] */
    int  central;
    int  selected;      /* the tab in front, or -1 */
    int  winCount;
    char win[LV_DOCK_TREE_WINS][LV_DOCK_TREE_NAME];
} LvDockTreeNode;

/* node[0] is the whole sidebar. count 0 is "nothing remembered". */
typedef struct {
    int            count;
    LvDockTreeNode node[LV_DOCK_TREE_NODES];
} LvDockTree;

/* The characters a panel id may have here: the ones that cannot be mistaken
 * for the tree's own punctuation. */
static inline int lvDockTreeNameChar(char c) {
    if ((unsigned char)c <= ' ' || (unsigned char)c >= 127) return 0;
    return c != ',' && c != '|' && c != '[' && c != ']' && c != '(' && c != ')' &&
           c != '*';
}

/* Take the next free node, cleared. -1 when the tree is full. */
static inline int lvDockTreeAdd(LvDockTree *t) {
    LvDockTreeNode *n;
    if (t->count >= LV_DOCK_TREE_NODES) return -1;
    n = &t->node[t->count];
    memset(n, 0, sizeof(*n));
    n->selected = -1;
    n->child[0] = n->child[1] = -1;
    return t->count++;
}

static inline int lvDockTreeParseNode(const char **pp, LvDockTree *t, int depth) {
    const char *p = *pp;
    int idx, k;

    if (depth > LV_DOCK_TREE_DEPTH) return -1;
    idx = lvDockTreeAdd(t);
    if (idx < 0) return -1;

    if (*p == 'X' || *p == 'Y') {
        int share = 0;
        t->node[idx].axis = *p++;
        if (!lvDockReadInt(&p, &share)) return -1;
        if (share < 1) share = 1;
        if (share > 999) share = 999;
        t->node[idx].permille = share;
        for (k = 0; k < 2; k++) {
            int child;
            if (*p != '(') return -1;
            p++;
            child = lvDockTreeParseNode(&p, t, depth + 1);
            if (child < 0) return -1;
            t->node[idx].child[k] = child;
            if (*p != ')') return -1;
            p++;
        }
    } else if (*p == 'L') {
        p++;
        if (*p == '*') {
            t->node[idx].central = 1;
            p++;
        }
        if (*p != '[') return -1;
        p++;
        while (*p != ']' && *p != '|') {
            LvDockTreeNode *n = &t->node[idx];
            int len = 0;
            if (n->winCount >= LV_DOCK_TREE_WINS) return -1;
            while (lvDockTreeNameChar(*p)) {
                if (len >= LV_DOCK_TREE_NAME - 1) return -1;
                n->win[n->winCount][len++] = *p++;
            }
            if (len == 0) return -1;
            n->win[n->winCount][len] = '\0';
            n->winCount++;
            if (*p == ',') {
                p++;
                if (*p == ']' || *p == '|') return -1;
            } else if (*p != ']' && *p != '|') {
                return -1;
            }
        }
        if (*p == '|') {
            int sel = 0;
            p++;
            if (!lvDockReadInt(&p, &sel)) return -1;
            /* A tab that is not there is no tab: ImGui then picks one. */
            t->node[idx].selected = (sel < t->node[idx].winCount) ? sel : -1;
        }
        if (*p != ']') return -1;
        p++;
    } else {
        return -1;
    }
    *pp = p;
    return idx;
}

/* Read an arrangement. Returns 1 and fills out when the whole text is one
 * tree; anything else (empty, cut short, too deep, too big, text after the
 * end) returns 0 with out->count 0. */
static inline int lvDockTreeParse(const char *text, LvDockTree *out) {
    const char *p = text;
    out->count = 0;
    if (text == NULL || text[0] == '\0') return 0;
    if (strlen(text) >= LV_DOCK_TREE_TEXT_MAX) return 0;
    if (lvDockTreeParseNode(&p, out, 0) != 0 || *p != '\0') {
        out->count = 0;
        return 0;
    }
    return 1;
}

static inline int lvDockTreeFormatNode(const LvDockTree *t, int idx, int depth,
                                       char *out, size_t size, size_t *at) {
    const LvDockTreeNode *n;
    int w, k;

    if (idx < 0 || idx >= t->count || depth > LV_DOCK_TREE_DEPTH) return 0;
    n = &t->node[idx];
#define LV_DOCK_TREE_PUT(...)                                              \
    do {                                                                   \
        w = snprintf(out + *at, size - *at, __VA_ARGS__);                  \
        if (w < 0 || (size_t)w >= size - *at) return 0;                    \
        *at += (size_t)w;                                                  \
    } while (0)
    if (n->axis == 'X' || n->axis == 'Y') {
        int share = n->permille;
        if (share < 1) share = 1;
        if (share > 999) share = 999;
        LV_DOCK_TREE_PUT("%c%d", n->axis, share);
        for (k = 0; k < 2; k++) {
            LV_DOCK_TREE_PUT("(");
            if (!lvDockTreeFormatNode(t, n->child[k], depth + 1, out, size, at)) return 0;
            LV_DOCK_TREE_PUT(")");
        }
    } else {
        LV_DOCK_TREE_PUT("%s", n->central ? "L*[" : "L[");
        if (n->winCount < 0 || n->winCount > LV_DOCK_TREE_WINS) return 0;
        for (k = 0; k < n->winCount; k++) {
            const char *c;
            if (n->win[k][0] == '\0') return 0;
            for (c = n->win[k]; *c != '\0'; c++) {
                if (!lvDockTreeNameChar(*c)) return 0;
            }
            LV_DOCK_TREE_PUT("%s%s", k > 0 ? "," : "", n->win[k]);
        }
        if (n->selected >= 0 && n->selected < n->winCount) {
            LV_DOCK_TREE_PUT("|%d", n->selected);
        }
        LV_DOCK_TREE_PUT("]");
    }
#undef LV_DOCK_TREE_PUT
    return 1;
}

/* Write an arrangement. Returns the text's length, or -1 when the tree is
 * empty, broken or does not fit (out then holds an empty string). */
static inline int lvDockTreeFormat(const LvDockTree *t, char *out, size_t size) {
    size_t at = 0;
    if (out == NULL || size == 0) return -1;
    out[0] = '\0';
    if (t->count < 1 || t->count > LV_DOCK_TREE_NODES) return -1;
    if (!lvDockTreeFormatNode(t, 0, 0, out, size, &at)) {
        out[0] = '\0';
        return -1;
    }
    return (int)at;
}

/* How many panels the arrangement names. */
static inline int lvDockTreeWindowCount(const LvDockTree *t) {
    int i, sum = 0;
    for (i = 0; i < t->count; i++) {
        if (t->node[i].axis == 0) sum += t->node[i].winCount;
    }
    return sum;
}

/* True when the part idx, or a part inside it, is the central one. */
static inline int lvDockTreeHasCentral(const LvDockTree *t, int idx, int depth) {
    const LvDockTreeNode *n;
    if (idx < 0 || idx >= t->count || depth > LV_DOCK_TREE_DEPTH) return 0;
    n = &t->node[idx];
    if (n->axis == 0) return n->central;
    return lvDockTreeHasCentral(t, n->child[0], depth + 1) ||
           lvDockTreeHasCentral(t, n->child[1], depth + 1);
}

#endif /* LV_DOCK_LAYOUT_H */
