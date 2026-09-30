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
 * Name:          lobby_start_list.h
 * Purpose:       Focusable, D-pad-navigable list of map
 *                start slots for the lobby start picker.
 *                A non-spatial alternative to the popup's
 *                zoom/pan marker picking: one focusable row
 *                per start, coloured by ownership, that
 *                claims a free start on activate (Space /
 *                gamepad A via the keyboard-nav bridge).
 *********************************************************/

#ifndef LOBBY_START_LIST_H
#define LOBBY_START_LIST_H

#include "platform_types.h"   /* BYTE */

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/* Render a vertical list of focusable Selectable rows, one per start
 * (1..startCount, 1-based). Each row shows the start number plus the
 * reserving player's name (or an "open" indication when free) and is
 * coloured by ownership relative to myPlayerNum. Activating a free start
 * claims it for myPlayerNum (the same clientSimNetSendLobbyClaimStart path
 * the mouse picker uses); re-activating a free start while already holding
 * one moves the claim. Rows for occupied starts are non-actionable here.
 *
 * sideMasks, when given, is indexed by 1-based start (MAX_STARTS + 1
 * entries) and holds each start's START_SIDE_BIT_* mask. A free start the
 * side rules keep the local player's team off is then drawn dimmed with the
 * off-side suffix, is not actionable — the server would refuse the claim
 * and the mouse picker blocks it the same way — and says so when focused.
 * NULL applies no side rule.
 *
 * onMap, when given, is indexed the same way; a start whose entry is false
 * is not on the map and gets no row. NULL lists every start.
 *
 * Returns the 1-based index of the currently focused start, or 0 when none
 * is focused — so the caller can highlight that start on the map preview. */
int lobbyStartListRender(struct ClientSim *cs, int myPlayerNum, int startCount,
                         const BYTE *sideMasks, const bool *onMap);

#ifdef __cplusplus
}
#endif

#endif /* LOBBY_START_LIST_H */
