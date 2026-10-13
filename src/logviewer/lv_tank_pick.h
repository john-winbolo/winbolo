/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer tank pick sums
 *Filename:      lv_tank_pick.h
 *Purpose:
 *  A click on a tank on the Log Viewer's map selects that
 *  tank, and the selected tank has a mark round it (draw.c
 *  does both). This is the arithmetic: which of the tanks on
 *  the screen a point is on, and where the mark goes.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_TANK_PICK_H
#define LV_TANK_PICK_H

#include <math.h>
#include <stdbool.h>

/* A tank's picture is one map square. Zoomed out that is a few points
 * across, too small to hit, so the box a click is tested against is never
 * under this many points a side. It grows evenly round the picture's
 * middle. */
#define LV_TANK_PICK_MIN_BOX 12.0f

/* The mark round the selected tank: four corners, each a third of the side
 * long, one gap outside the picture. Sizes in points. Drawn 75% solid, as
 * the bars over the tanks are. */
#define LV_TANK_MARK_GAP   2.0f
#define LV_TANK_MARK_THICK 1.0f
#define LV_TANK_MARK_ALPHA 0.75f

/* One tank on the screen: the top left of its picture in framebuffer
 * pixels, its slot, and whether a click can pick it (a dead tank's picture
 * can stay on the screen until the next repaint; it is not a tank). */
typedef struct LvTankPickBox {
    float x, y;
    int   slot;
    bool  pickable;
} LvTankPickBox;

/* The side of the box a click is tested against, in framebuffer pixels:
 * the picture's own side (tilePx), or the least size when that is larger.
 * pxScale is the display's pixel density. */
static inline float lvTankPickBoxSide(float tilePx, float pxScale) {
    float least;
    if (pxScale <= 0.0f) pxScale = 1.0f;
    least = LV_TANK_PICK_MIN_BOX * pxScale;
    return (tilePx > least) ? tilePx : least;
}

/* The tank the point (px, py) is on, as an index into tanks, or -1 for
 * none. A point is on a tank when it is inside the tank's box; the left and
 * top edges are in, the right and bottom ones out. With the point on more
 * than one, the tank whose middle is nearest wins, and of two at the same
 * distance the earlier one in the list. */
static inline int lvTankPick(const LvTankPickBox *tanks, int count, float px, float py,
                             float tilePx, float pxScale) {
    float side = lvTankPickBoxSide(tilePx, pxScale);
    float half = side * 0.5f;
    float best = 0.0f;
    int found = -1;
    int i;

    if (tanks == NULL || tilePx <= 0.0f) return -1;
    for (i = 0; i < count; i++) {
        float cx = tanks[i].x + tilePx * 0.5f;
        float cy = tanks[i].y + tilePx * 0.5f;
        float dx = px - cx;
        float dy = py - cy;
        float d;
        if (!tanks[i].pickable) continue;
        if (dx < -half || dx >= half || dy < -half || dy >= half) continue;
        d = dx * dx + dy * dy;
        if (found < 0 || d < best) {
            best = d;
            found = i;
        }
    }
    return found;
}

/* The mark round a tank whose picture has its top left at (tankLeft,
 * tankTop): the outer square (x, y, side), how long a corner's arm is and
 * how thick, all in whole framebuffer pixels. */
typedef struct LvTankMark {
    float x, y, side;
    float arm;
    float thick;
} LvTankMark;

static inline LvTankMark lvTankMarkLayout(float tankLeft, float tankTop, float tilePx,
                                          float pxScale) {
    LvTankMark m;
    float gap;
    if (pxScale <= 0.0f) pxScale = 1.0f;
    gap = floorf(LV_TANK_MARK_GAP * pxScale + 0.5f);
    m.thick = floorf(LV_TANK_MARK_THICK * pxScale + 0.5f);
    if (m.thick < 1.0f) m.thick = 1.0f;
    m.x = floorf(tankLeft) - gap - m.thick;
    m.y = floorf(tankTop) - gap - m.thick;
    m.side = floorf(tilePx + 0.5f) + 2.0f * (gap + m.thick);
    m.arm = floorf(m.side / 3.0f);
    if (m.arm < m.thick + 1.0f) m.arm = m.thick + 1.0f;
    return m;
}

/* ---- A double click on a tank -----------------------------------------
 * One click on a tank on the map selects it. A double click also changes
 * whose view of the game is shown, the view the eye of the HUD panel stands
 * for: it turns the view of that tank's player on, or, when that view was
 * on already, off again (back to all teams). Each click of a double click
 * still does all a single click does, so the first one selects the tank and
 * the second one changes the view.
 *
 * clickCount: which click in a row this one is (1 for a single click, 2 for
 *             the second click of a double click, 3 for a third...).
 * tank:       the slot of the tank the click is on, -1 for a click on
 *             anything else (a pillbox, a base, the map).
 * prevTank:   what the click before this one in the row was on.
 * viewBefore: the slot of the player whose view was shown before the click
 *             before this one (the first click of the pair) did anything,
 *             -1 for the view of all teams. It is not the view as it is
 *             now: in Select Team mode the first click moves a view that is
 *             on to the tank it picks, and the pair must not be read as a
 *             double click on a tank whose view was on.
 *
 * The answer is what the view is to be after this click. Only the second
 * click of a pair changes it (the 2nd, the 4th... of a row), so a double
 * click changes the view exactly once and a triple click no more than a
 * double click. Both clicks must be on the same tank: a click on the map
 * and then one on a tank that drove under the mouse is not a double click
 * on that tank. */
typedef enum {
    LV_MAP_VIEW_KEEP = 0, /* this click does not change the view */
    LV_MAP_VIEW_ON,       /* show the view of the clicked tank's player */
    LV_MAP_VIEW_OFF       /* back to all teams */
} LvMapClickView;

static inline LvMapClickView lvMapClickViewsTank(int clickCount, int tank, int prevTank,
                                                 int viewBefore) {
    if (clickCount < 2 || (clickCount % 2) != 0) return LV_MAP_VIEW_KEEP;
    if (tank < 0 || tank != prevTank) return LV_MAP_VIEW_KEEP;
    return (viewBefore == tank) ? LV_MAP_VIEW_OFF : LV_MAP_VIEW_ON;
}

#endif /* LV_TANK_PICK_H */
