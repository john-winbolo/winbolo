/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer name header sums
 *Filename:      lv_name_header.h
 *Purpose:
 *  The line at the top of the Log Viewer's Tank panel and of
 *  its HUD panel that names a player: a stripe in the colour
 *  of the player's team, the name, and at the right a small
 *  "X" button (imgui_lv_icons.cpp draws it). This is the
 *  arithmetic: how much room the name gets and where the
 *  button goes.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_NAME_HEADER_H
#define LV_NAME_HEADER_H

#include <stdbool.h>

typedef struct LvNameHeader {
    float nameX;   /* where the name starts, from the left of the line */
    float nameW;   /* the room the name has; a longer name is cut to it */
    bool  nameCut; /* the name is longer than its room */
    float closeX;  /* where the button starts */
} LvNameHeader;

/* availW is the width of the line, lead what comes before the name (the
 * stripe, and an icon when there is one, with their gaps), nameW the width
 * of the whole name, closeW the side of the button and gap the space kept
 * between the name and the button.
 *
 * The button is at the right end of the line. The name has what is left
 * between the lead and the button, less the gap, and is cut when it is
 * longer: the name gives way, never the button, and the two never overlap.
 * In a line too narrow for the lead and the button, the button goes straight
 * after the lead and the name has no room. */
static inline LvNameHeader lvNameHeaderLayout(float availW, float lead, float nameW,
                                              float closeW, float gap) {
    LvNameHeader out;
    float room;

    if (availW < 0.0f) availW = 0.0f;
    if (lead < 0.0f) lead = 0.0f;
    if (nameW < 0.0f) nameW = 0.0f;
    out.nameX = lead;
    out.closeX = availW - closeW;
    if (out.closeX < lead) out.closeX = lead;
    room = out.closeX - gap - lead;
    if (room < 0.0f) room = 0.0f;
    out.nameCut = nameW > room;
    out.nameW = out.nameCut ? room : nameW;
    return out;
}

#endif /* LV_NAME_HEADER_H */
