/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Panel Slot
 *Filename:      scenario_panel_slot.h
 *Purpose:
 *  Where the tablet UI puts the scenario panel: a fixed
 *  square in the game view's top-right corner, in place of
 *  the window the desktop lets the player move and resize.
 *
 *  Plain C and arithmetic only, so a test can hold the
 *  square to hand-worked numbers with no window, no
 *  renderer and no ImGui behind it.
 *********************************************************/
#ifndef SCENARIO_PANEL_SLOT_H
#define SCENARIO_PANEL_SLOT_H

#include <stdbool.h>

#include "scenario_panel.h" /* SCN_PANEL_UNITS */

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
 *NAME:          scnPanelSlotRect
 *PURPOSE:
 *  The tablet panel's square, in screen pixels.
 *
 *  The side is a quarter of the screen's shorter side,
 *  rounded down. The square sits in the game view's
 *  top-right corner, inset by pad on the top and the right,
 *  so the right gutter's controls are never under it.
 *
 *  A side that would not fit inside the view with the inset
 *  on both sides is shrunk to fit, but not below
 *  SCN_PANEL_UNITS pixels (the panel at zoom 1) unless the
 *  view itself is smaller than that. A square too big for
 *  its inset loses the inset before it leaves the view, so
 *  it is always inside the view.
 *
 *  False, with the outputs zeroed, for a screen or a view
 *  with no area.
 *
 *ARGUMENTS:
 *  screenW/screenH - the screen, in pixels
 *  viewX/viewY     - the game view's top-left
 *  viewW/viewH     - the game view's size
 *  pad             - the inset from the view's edges
 *  outX/outY       - receive the square's top-left
 *  outSide         - receives the square's side
 *********************************************************/
static inline bool scnPanelSlotRect(int screenW, int screenH, float viewX,
                                    float viewY, float viewW, float viewH,
                                    float pad, float *outX, float *outY,
                                    float *outSide) {
    int   shortSide;
    float want, viewShort, fit, least, side, x, y;

    *outX    = 0.0f;
    *outY    = 0.0f;
    *outSide = 0.0f;
    if (screenW <= 0 || screenH <= 0 || viewW <= 0.0f || viewH <= 0.0f) {
        return false;
    }
    if (pad < 0.0f) pad = 0.0f;

    shortSide = (screenW < screenH) ? screenW : screenH;
    want      = (float)(shortSide / 4);

    viewShort = (viewW < viewH) ? viewW : viewH;
    fit       = viewShort - 2.0f * pad;
    side      = want;
    if (side > fit) {
        least = (float)SCN_PANEL_UNITS;
        if (least > viewShort) least = viewShort;
        side = (fit > least) ? fit : least;
        if (side > want) side = want;
    }
    if (side <= 0.0f) return false;

    x = viewX + viewW - pad - side;
    if (x < viewX) x = viewX;
    y = viewY + pad;
    if (y + side > viewY + viewH) y = viewY + viewH - side;

    *outX    = x;
    *outY    = y;
    *outSide = side;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* SCENARIO_PANEL_SLOT_H */
