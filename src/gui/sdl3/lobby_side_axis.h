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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          lobby_side_axis.h
 * Purpose:       The two-team start-side axis rules behind
 *                the lobby map preview's compass.
 *
 *                A compass axis is a pair of opposite
 *                sides — N/S or E/W. When exactly two teams
 *                have members the host can set both of them
 *                at once by clicking an axis, and clicking
 *                the axis they are already on puts both back
 *                to START_SIDE_ANY.
 *
 *                Pure integer helpers only: which teams are
 *                populated, which axis a pair of sides
 *                forms, and what a click on an axis has to
 *                send. No ImGui, no ClientSim — so the unit
 *                tests compile the same code the lobby draws
 *                from. Header-only (static inline) to avoid
 *                a separate translation unit.
 *********************************************************/

#ifndef LOBBY_SIDE_AXIS_H
#define LOBBY_SIDE_AXIS_H

#include <stdbool.h>

#include "start_sides.h"  /* START_SIDE_* — the wire side values */

/* The two compass axes, plus "no axis". */
#define LOBBY_SIDE_AXIS_NONE 0
#define LOBBY_SIDE_AXIS_NS   1
#define LOBBY_SIDE_AXIS_EW   2

/* Exactly two populated teams? counts[t] is the number of connected lobby
 * slots (bots included) on team t, for t in 1..nTeams-1; team 0 is "no
 * team" and never counts. On true, *outA is the lower team id and *outB
 * the higher. Both outputs are set to 0 when the answer is false, so a
 * caller that ignores the return value still reads no team. */
static inline bool lobbySideTwoTeamPair(const int *counts, int nTeams,
                                        int *outA, int *outB) {
    int a = 0, b = 0, found = 0, t;
    if (outA) *outA = 0;
    if (outB) *outB = 0;
    if (!counts) return false;
    for (t = 1; t < nTeams; t++) {
        if (counts[t] <= 0) continue;
        found++;
        if (found > 2) return false;   /* three or more: not a pair */
        if (found == 1) a = t;
        else            b = t;
    }
    if (found != 2) return false;
    if (outA) *outA = a;
    if (outB) *outB = b;
    return true;
}

/* The axis one side sits on. START_SIDE_ANY, and anything outside the
 * START_SIDE_* range, is on no axis. */
static inline int lobbySideAxisOfSide(BYTE side) {
    if (side == START_SIDE_N || side == START_SIDE_S) return LOBBY_SIDE_AXIS_NS;
    if (side == START_SIDE_E || side == START_SIDE_W) return LOBBY_SIDE_AXIS_EW;
    return LOBBY_SIDE_AXIS_NONE;
}

/* The two sides of an axis, in the order the pair is assigned: the first
 * goes to the lower team id. NONE gives ANY twice. */
static inline void lobbySideAxisSides(int axis, BYTE *outA, BYTE *outB) {
    BYTE a = START_SIDE_ANY, b = START_SIDE_ANY;
    if (axis == LOBBY_SIDE_AXIS_NS)      { a = START_SIDE_N; b = START_SIDE_S; }
    else if (axis == LOBBY_SIDE_AXIS_EW) { a = START_SIDE_E; b = START_SIDE_W; }
    if (outA) *outA = a;
    if (outB) *outB = b;
}

/* The axis two teams' sides form. Only a complementary pair counts —
 * N with S, or E with W, in either order. One team without a side, two
 * teams on the same side, or one team north and the other east, all read
 * as no axis, which is what leaves the compass drawn unset. */
static inline int lobbySideAxisOfPair(BYTE sideA, BYTE sideB) {
    if ((sideA == START_SIDE_N && sideB == START_SIDE_S) ||
        (sideA == START_SIDE_S && sideB == START_SIDE_N)) {
        return LOBBY_SIDE_AXIS_NS;
    }
    if ((sideA == START_SIDE_E && sideB == START_SIDE_W) ||
        (sideA == START_SIDE_W && sideB == START_SIDE_E)) {
        return LOBBY_SIDE_AXIS_EW;
    }
    return LOBBY_SIDE_AXIS_NONE;
}

/* What clicking `axis` has to send for the two teams, given the axis
 * they are on now (setAxis, from lobbySideAxisOfPair). Clicking the axis
 * already set clears both teams back to START_SIDE_ANY — "go back to
 * custom starts"; any other click assigns the axis, first side to the
 * lower team id. Returns false for LOBBY_SIDE_AXIS_NONE, where there is
 * nothing to send. */
static inline bool lobbySideAxisClick(int axis, int setAxis,
                                      BYTE *outSideA, BYTE *outSideB) {
    if (axis != LOBBY_SIDE_AXIS_NS && axis != LOBBY_SIDE_AXIS_EW) {
        if (outSideA) *outSideA = START_SIDE_ANY;
        if (outSideB) *outSideB = START_SIDE_ANY;
        return false;
    }
    if (axis == setAxis) {
        if (outSideA) *outSideA = START_SIDE_ANY;
        if (outSideB) *outSideB = START_SIDE_ANY;
        return true;
    }
    lobbySideAxisSides(axis, outSideA, outSideB);
    return true;
}

#endif /* LOBBY_SIDE_AXIS_H */
