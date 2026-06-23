/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
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
 * Returns the 1-based index of the currently focused start, or 0 when none
 * is focused — so the caller can highlight that start on the map preview. */
int lobbyStartListRender(struct ClientSim *cs, int myPlayerNum, int startCount);

#ifdef __cplusplus
}
#endif

#endif /* LOBBY_START_LIST_H */
