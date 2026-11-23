/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_dock_clear.h - a sidebar of the log viewer whose unused part is
 * transparent.
 *
 * A sidebar has the option "Make unused sidebar portions transparent". With
 * it off the sidebar is a solid strip and its panels share all of its
 * length. With it on:
 *
 *  - the panels are packed from the start of the sidebar, with no room
 *    between them. In the left and right sidebars that is from the top
 *    down; in the top and bottom ones from the left across;
 *  - each panel has a length of its own (a height in the left and right
 *    sidebars, a width in the top and bottom ones) and a grip on its far
 *    edge (its bottom edge, or its right edge) that the user drags to
 *    change that length and no other;
 *  - a panel is solid inside its rectangle;
 *  - past the grip of the last panel the sidebar is clear: the map shows
 *    there and a click there is a click on the map.
 *
 * The map is drawn under such a sidebar, but what counts as the visible map
 * (for the middle of the view) is still the part of the window that no
 * sidebar is on, so the option does not move the map.
 *
 * Header only and free of SDL and ImGui, so the sums can be tested on their
 * own (tests/unit/test_lv_dock_clear.c). Every length is in window points
 * unless it says otherwise. "Along" is the way the panels are packed.
 */

#ifndef LV_DOCK_CLEAR_H
#define LV_DOCK_CLEAR_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lv_dock_fit.h"
#include "lv_dock_layout.h"

/* The most panels packed in one sidebar. The viewer has nine. */
#define LV_DOCK_CLEAR_SLOTS 16

/* How thick the grip on a panel's far edge is. */
#define LV_DOCK_CLEAR_GRIP 4

/* The least length of a panel: a tab and a row or two down, and the least
 * width a sidebar has across. */
#define LV_DOCK_CLEAR_MIN_DOWN   48
#define LV_DOCK_CLEAR_MIN_ACROSS LV_DOCK_WIDTH_MIN

/* No saved length is believed past this. */
#define LV_DOCK_CLEAR_LEN_MAX 8192

/* True when the panels of the sidebar are packed downward (the left and the
 * right one), false when across (the top and the bottom one). */
static inline int lvDockClearIsDown(int side) {
    return lvDockSideIsWide(side);
}

static inline int lvDockClearMin(int side) {
    return lvDockClearIsDown(side) ? LV_DOCK_CLEAR_MIN_DOWN : LV_DOCK_CLEAR_MIN_ACROSS;
}

/* ---- The panels, by name -------------------------------------------------
 *
 * The part of a panel's window title that makes its ID, and the panel's part
 * of its preference names (the part the "Window.<Panel>.Visible" names
 * have). The order is the one the right sidebar stacks them in. */
typedef struct {
    const char *id;
    const char *key;
} LvDockClearPanel;

#define LV_DOCK_CLEAR_PANELS 10

static const LvDockClearPanel lvDockClearPanels[LV_DOCK_CLEAR_PANELS] = {
    { "###controls",   "Controls" },
    { "###hud",        "HUD" },
    { "###overview",   "Overview" },
    { "###tank",       "Tank" },
    { "###gameinfo",   "GameInformation" },
    { "###lvscnpanel", "ScenarioPanel" },
    { "###iteminfo",   "ItemInformation" },
    { "###events",     "Events" },
    { "###comments",   "Comments" },
    { "###chat",       "Chat" },
};

/* The index of a panel by its ID, -1 for a window that is not a panel. */
static inline int lvDockClearPanelIndex(const char *id) {
    int i;
    if (id == NULL) return -1;
    for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) {
        if (strcmp(lvDockClearPanels[i].id, id) == 0) return i;
    }
    return -1;
}

/* ---- A panel's length ------------------------------------------------------ */

/* The length a panel gets the first time it is in a sidebar with the option
 * on. current is the length it has now and content the length of what it
 * shows (0 when that is not known yet). exact says the panel shows a fixed
 * amount (the Controls, HUD, Overview and Tank panels): it gets the length
 * of its content, shorter or longer than it was. A panel that is a list
 * gets the smaller of the two, so a short one stops where its content
 * stops and a long one keeps the length it had and does not take the whole
 * sidebar. Never less than the least length. */
static inline int lvDockClearFirstLen(int current, int content, int min, int exact) {
    int len = current;
    if (content > 0 && (exact || len <= 0 || content < len)) len = content;
    if (len < min) len = min;
    if (len > LV_DOCK_CLEAR_LEN_MAX) len = LV_DOCK_CLEAR_LEN_MAX;
    return len;
}

/* The length a double click on a panel's grip gives it: the length of what
 * it shows, or the length it has when that is not known. */
static inline int lvDockClearSnapLen(int current, int content, int min) {
    int len = (content > 0) ? content : current;
    if (len < min) len = min;
    if (len > LV_DOCK_CLEAR_LEN_MAX) len = LV_DOCK_CLEAR_LEN_MAX;
    return len;
}

/* ---- Where the panels go --------------------------------------------------- */

/* The panels along a sidebar: where each starts and how long it is, measured
 * from the start of the sidebar. After each panel is its grip. used is the
 * length up to the end of the last grip; the clear part starts there. */
typedef struct {
    int count;
    int pos[LV_DOCK_CLEAR_SLOTS];
    int len[LV_DOCK_CLEAR_SLOTS];
    int used;
    int clearLen;
} LvDockClearPlan;

/* Pack count panels that ask for wanted[i] each into a sidebar extent long,
 * with a grip of `grip` after each.
 *
 * When they fit, each gets what it asks for (never less than its least
 * length) and the rest of the sidebar is clear. When they do not fit, the
 * last panel gives up length first, down to its least length, then the one
 * before it, and so on: the panels the user reads first keep their size.
 * When even the least lengths do not fit, every panel gets its share of the
 * room in proportion, and nothing is clear. The lengths asked for are not
 * changed by this: in a taller window each panel has its own length again. */
static inline void lvDockClearLayout(const int *wanted, const int *min, int count,
                                     int extent, int grip, LvDockClearPlan *out) {
    int i, avail, sum = 0, over, at = 0;

    if (count < 0) count = 0;
    if (count > LV_DOCK_CLEAR_SLOTS) count = LV_DOCK_CLEAR_SLOTS;
    if (extent < 0) extent = 0;
    if (grip < 0) grip = 0;
    out->count = count;
    avail = extent - grip * count;
    if (avail < 0) avail = 0;

    for (i = 0; i < count; i++) {
        int lo = (min[i] > 1) ? min[i] : 1;
        out->len[i] = (wanted[i] > lo) ? wanted[i] : lo;
        sum += out->len[i];
    }
    over = sum - avail;
    for (i = count - 1; i >= 0 && over > 0; i--) {
        int lo = (min[i] > 1) ? min[i] : 1;
        int give = out->len[i] - lo;
        if (give > over) give = over;
        out->len[i] -= give;
        over -= give;
    }
    if (over > 0) {
        /* Not even the least lengths fit. */
        int total = 0, left = avail;
        for (i = 0; i < count; i++) total += out->len[i];
        for (i = 0; i < count; i++) {
            int share = (total > 0) ? (int)(((long long)out->len[i] * avail) / total) : 0;
            if (i == count - 1) share = left;
            if (share < 1) share = 1;
            out->len[i] = share;
            left -= share;
        }
    }
    for (i = 0; i < count; i++) {
        out->pos[i] = at;
        at += out->len[i] + grip;
    }
    out->used = at;
    out->clearLen = (extent > at) ? extent - at : 0;
}

/* The length panel i gets when the user drags its grip to ask for wanted.
 * Never less than its least length, and never so much that the panels after
 * it, each at its least length, would not fit: a drag makes the later panels
 * shorter down to that, and stops there. */
static inline int lvDockClearDragLen(const LvDockClearPlan *plan, const int *min, int i,
                                     int wanted, int extent, int grip) {
    int k, hi, lo;
    if (i < 0 || i >= plan->count) return wanted;
    lo = (min[i] > 1) ? min[i] : 1;
    hi = extent - grip * plan->count;
    for (k = 0; k < i; k++) hi -= plan->len[k];
    for (k = i + 1; k < plan->count; k++) hi -= (min[k] > 1) ? min[k] : 1;
    if (wanted > hi) wanted = hi;
    if (wanted < lo) wanted = lo;
    return wanted;
}

typedef struct {
    int x, y, w, h;
} LvDockRect;

/* A length along the sidebar, from `from` for `len`, as a rectangle of the
 * strip: the full size across. */
static inline LvDockRect lvDockClearSpan(int side, LvDockRect strip, int from, int len) {
    LvDockRect r = strip;
    if (len < 0) len = 0;
    if (lvDockClearIsDown(side)) {
        r.y = strip.y + from;
        r.h = len;
    } else {
        r.x = strip.x + from;
        r.w = len;
    }
    return r;
}

/* The rectangle of panel i. strip is the sidebar's part of the window and
 * edge the thickness of the grip on the sidebar's inner edge (the one the
 * user drags to make the sidebar bigger), which the panels leave alone. */
static inline LvDockRect lvDockClearPanelRect(int side, LvDockRect strip, int edge,
                                              const LvDockClearPlan *plan, int i) {
    LvDockRect r = lvDockClearSpan(side, strip, plan->pos[i], plan->len[i]);
    if (edge < 0) edge = 0;
    switch (side) {
        case LV_DOCK_RIGHT:  r.x += edge; r.w -= edge; break;
        case LV_DOCK_LEFT:   r.w -= edge; break;
        case LV_DOCK_TOP:    r.h -= edge; break;
        default:             r.y += edge; r.h -= edge; break;
    }
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

/* The grip on the far edge of panel i. */
static inline LvDockRect lvDockClearGripRect(int side, LvDockRect strip, int grip,
                                             const LvDockClearPlan *plan, int i) {
    return lvDockClearSpan(side, strip, plan->pos[i] + plan->len[i], grip);
}

/* The part of the strip the panels and their grips take. */
static inline LvDockRect lvDockClearUsedRect(int side, LvDockRect strip,
                                             const LvDockClearPlan *plan) {
    int ext = lvDockClearIsDown(side) ? strip.h : strip.w;
    int used = (plan->used < ext) ? plan->used : ext;
    return lvDockClearSpan(side, strip, 0, used);
}

/* The clear part of the strip: from the end of the last grip to the end of
 * the sidebar. Nothing (a length of 0) when the panels fill the sidebar. */
static inline LvDockRect lvDockClearClearRect(int side, LvDockRect strip,
                                              const LvDockClearPlan *plan) {
    int ext = lvDockClearIsDown(side) ? strip.h : strip.w;
    int used = (plan->used < ext) ? plan->used : ext;
    return lvDockClearSpan(side, strip, used, ext - used);
}

static inline int lvDockRectHas(const LvDockRect *r, float px, float py) {
    return px >= (float)r->x && px < (float)(r->x + r->w) &&
           py >= (float)r->y && py < (float)(r->y + r->h);
}

/* ---- The map under a sidebar ----------------------------------------------- */

/* What bounds the map as it is drawn: the margins, with nothing for a
 * sidebar whose unused part is clear, because the map goes under that one.
 * clear is indexed by LV_DOCK_*.
 *
 * The left and right sidebars run the full height and the top and bottom
 * ones sit between them. So when a left or right sidebar is clear the map
 * has to reach the top and the bottom of the window in that sidebar's
 * column, and it is one rectangle: it then goes under the top and bottom
 * sidebars too, solid or not. A solid one hides the part under it. */
static inline LvDockMargins lvDockClearMapMargins(const LvDockMargins *m, const int *clear) {
    LvDockMargins out = *m;
    if (clear[LV_DOCK_RIGHT])  out.right = 0;
    if (clear[LV_DOCK_LEFT])   out.left = 0;
    if (clear[LV_DOCK_TOP])    out.top = 0;
    if (clear[LV_DOCK_BOTTOM]) out.bottom = 0;
    if ((clear[LV_DOCK_LEFT] && m->left > 0) || (clear[LV_DOCK_RIGHT] && m->right > 0)) {
        out.top = 0;
        out.bottom = 0;
    }
    return out;
}

/* What the solid sidebars take of the window: the margins, with nothing for
 * a sidebar whose unused part is clear. Unlike lvDockClearMapMargins a solid
 * top or bottom sidebar keeps its margin beside a clear left or right one:
 * the map is drawn under it but does not show there. The window less these
 * is the largest rectangle of map no solid sidebar hides; the panels of a
 * clear sidebar can still be over part of it. */
static inline LvDockMargins lvDockClearOpenMargins(const LvDockMargins *m, const int *clear) {
    LvDockMargins out = *m;
    if (clear[LV_DOCK_RIGHT])  out.right = 0;
    if (clear[LV_DOCK_LEFT])   out.left = 0;
    if (clear[LV_DOCK_TOP])    out.top = 0;
    if (clear[LV_DOCK_BOTTOM]) out.bottom = 0;
    return out;
}

/* True when the window point is on the map: not on the menu bar, not on a
 * solid sidebar, and not on the panels of a sidebar whose unused part is
 * clear. used[side] is the part of that sidebar its panels take (all of the
 * strip for one with no panel in it, which is where a panel is dropped). */
static inline int lvDockClearPointOnMap(const LvDockMargins *m, const int *clear,
                                        const LvDockRect *used, int windowW, int windowH,
                                        int menuH, float px, float py) {
    int side = -1;
    if (px < 0.0f || py < (float)menuH || px >= (float)windowW || py >= (float)windowH) return 0;
    /* Which sidebar's strip the point is in: the left and right ones run
     * the full height, the top and bottom ones are between them. */
    if (px < (float)m->left) side = LV_DOCK_LEFT;
    else if (px >= (float)(windowW - m->right)) side = LV_DOCK_RIGHT;
    else if (py < (float)(menuH + m->top)) side = LV_DOCK_TOP;
    else if (py >= (float)(windowH - m->bottom)) side = LV_DOCK_BOTTOM;
    if (side < 0) return 1;
    if (!clear[side]) return 0;
    return lvDockRectHas(&used[side], px, py) ? 0 : 1;
}

/* The middle of the visible map along one way, in map pixels from the start
 * of the view. viewPx is the length of the view, which now reaches under
 * the clear sidebars; insetNear and insetFar are how much of it is under one
 * at each end. With nothing under a sidebar this is half the view, as it
 * always was. When the sidebars leave less than one square the whole view
 * counts. */
static inline int lvDockClearViewCentre(int viewPx, int insetNear, int insetFar) {
    int visible;
    if (insetNear < 0) insetNear = 0;
    if (insetFar < 0) insetFar = 0;
    visible = viewPx - insetNear - insetFar;
    if (visible < 16) return viewPx / 2;
    return insetNear + visible / 2;
}

/* How far the map's scroll position moves, in map pixels, when the place
 * the map starts at on the screen moves from oldStart to newStart (window
 * points) and the map is to stay where it is: the option was turned on or
 * off. zoom is the map's zoom. */
static inline int lvDockClearScrollShift(int oldStart, int newStart, float zoom) {
    float d;
    if (zoom <= 0.0f) zoom = 1.0f;
    d = (float)(newStart - oldStart) / zoom;
    return (int)(d < 0.0f ? d - 0.5f : d + 0.5f);
}

/* ---- The preferences -------------------------------------------------------
 *
 * Which sidebars have the option on, one line: "v1 R=0 L=1 T=0 B=0".
 * And for each sidebar the length of each panel that has one, one line a
 * sidebar: "v1 Controls=118 HUD=96". A fault in either gives the defaults
 * (off, no length), never a failure; what is not known is passed over. */

static inline int lvDockClearFlagsParse(const char *text, int *clear) {
    const char *p;
    int i;
    for (i = 0; i < LV_DOCK_SIDES; i++) clear[i] = 0;
    if (text == NULL || strncmp(text, "v1", 2) != 0) return 0;
    p = text + 2;
    if (*p != '\0' && *p != ' ') return 0;
    while (*p != '\0') {
        int side = -1, a = 0;
        const char *q;
        while (*p == ' ') p++;
        if (*p == '\0') break;
        for (i = 0; i < LV_DOCK_SIDES; i++) {
            if (*p == lvDockSideLetter(i)) side = i;
        }
        q = p + 1;
        if (side >= 0 && *q == '=') {
            q++;
            if (lvDockReadInt(&q, &a) && a <= 1 && (*q == ' ' || *q == '\0')) {
                clear[side] = a;
            }
        }
        while (*p != '\0' && *p != ' ') p++;
    }
    return 1;
}

static inline int lvDockClearFlagsFormat(const int *clear, char *out, size_t size) {
    int n;
    if (out == NULL || size == 0) return -1;
    n = snprintf(out, size, "v1 R=%d L=%d T=%d B=%d",
                 clear[LV_DOCK_RIGHT] ? 1 : 0, clear[LV_DOCK_LEFT] ? 1 : 0,
                 clear[LV_DOCK_TOP] ? 1 : 0, clear[LV_DOCK_BOTTOM] ? 1 : 0);
    if (n < 0 || (size_t)n >= size) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

/* The lengths of one sidebar's panels. len is indexed as lvDockClearPanels;
 * 0 is "none yet". A length that is not between 1 and the cap is left out
 * on the way out and not believed on the way in. */
static inline int lvDockClearLensParse(const char *text, int *len) {
    const char *p;
    int i;
    for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) len[i] = 0;
    if (text == NULL || strncmp(text, "v1", 2) != 0) return 0;
    p = text + 2;
    if (*p != '\0' && *p != ' ') return 0;
    while (*p != '\0') {
        const char *eq;
        while (*p == ' ') p++;
        if (*p == '\0') break;
        eq = p;
        while (*eq != '\0' && *eq != ' ' && *eq != '=') eq++;
        if (*eq == '=') {
            for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) {
                size_t n = strlen(lvDockClearPanels[i].key);
                const char *q = eq + 1;
                int v = 0;
                if ((size_t)(eq - p) != n || strncmp(p, lvDockClearPanels[i].key, n) != 0) continue;
                if (lvDockReadInt(&q, &v) && (*q == ' ' || *q == '\0') && v >= 1 &&
                    v <= LV_DOCK_CLEAR_LEN_MAX) {
                    len[i] = v;
                }
            }
        }
        while (*p != '\0' && *p != ' ') p++;
    }
    return 1;
}

static inline int lvDockClearLensFormat(const int *len, char *out, size_t size) {
    size_t at;
    int i, n;
    if (out == NULL || size < 3) return -1;
    n = snprintf(out, size, "v1");
    at = (size_t)n;
    for (i = 0; i < LV_DOCK_CLEAR_PANELS; i++) {
        if (len[i] < 1 || len[i] > LV_DOCK_CLEAR_LEN_MAX) continue;
        n = snprintf(out + at, size - at, " %s=%d", lvDockClearPanels[i].key, len[i]);
        if (n < 0 || (size_t)n >= size - at) {
            out[0] = '\0';
            return -1;
        }
        at += (size_t)n;
    }
    return (int)at;
}

#endif /* LV_DOCK_CLEAR_H */
