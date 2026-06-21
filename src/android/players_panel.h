/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * players_panel.h - Floating players panel for mobile (Android/iOS)
 *
 * Shows the list of players with alliance checkboxes, replacing the
 * desktop Players menu which isn't accessible on touch devices.
 * Rendered via ImGui into the existing SDL3 window.
 */

#ifndef PLAYERS_PANEL_H
#define PLAYERS_PANEL_H

#include <stdbool.h>
#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Toggle the players panel open/closed. */
void playersPanelToggle(void);

/* Returns true if the panel is currently visible. */
bool playersPanelIsOpen(void);

struct ClientSim;

/* Render the players panel (call during ImGui frame, after NewFrame). */
void playersPanelRender(struct ClientSim *cs);

/* Update player state — called from frontend callbacks. */
void playersPanelSetPlayer(unsigned char playerNum, const char *name);
void playersPanelClearPlayer(unsigned char playerNum);
void playersPanelSetCheckState(unsigned char playerNum, bool isChecked);

#ifdef __cplusplus
}
#endif

#endif /* PLAYERS_PANEL_H */
