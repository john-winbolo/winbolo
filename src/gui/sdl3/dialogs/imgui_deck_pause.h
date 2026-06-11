/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_deck_pause.h
 * Purpose:       Steam Deck / gamepad pause overlay.
 *                Modal popup triggered by the Start button
 *                in Deck mode, exposing the same actions a
 *                desktop player reaches via the menu bar.
 *********************************************************/

#ifndef IMGUI_DECK_PAUSE_H
#define IMGUI_DECK_PAUSE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

void deckPauseOpen(void);                    /* call when Start pressed */
void deckPauseRender(struct ClientSim *cs);  /* call from render path */
bool deckPauseIsOpen(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_DECK_PAUSE_H */
