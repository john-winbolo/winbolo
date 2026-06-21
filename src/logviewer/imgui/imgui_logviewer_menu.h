/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_logviewer_menu.h
 * Purpose:       Log viewer Start-button popup menu for
 *                tablet / Steam Deck. Modal popup that
 *                replaces the desktop top menu bar in
 *                compact mode. Modelled on imgui_deck_pause.
 *********************************************************/

#ifndef IMGUI_LOGVIEWER_MENU_H
#define IMGUI_LOGVIEWER_MENU_H

#include <stdbool.h>

struct LogViewerState;

#ifdef __cplusplus
extern "C" {
#endif

/* Which panel is visible in compact mode.  Controls (toolbar) is always
   shown alongside whichever of these is selected. */
typedef enum {
    LV_PANEL_EVENTS = 0,
    LV_PANEL_GAME_INFO,
    LV_PANEL_ITEM_INFO,
    LV_PANEL_COMMENTS
} LvCompactPanel;

void lvMenuOpen(void);                              /* call when Start pressed */
void lvMenuRender(struct LogViewerState *lv);       /* call from render path */
bool lvMenuIsOpen(void);

/* C-callable helper: poll ImGui for a Start-button press this frame and
   open the popup if so.  Must be invoked between ImGui::NewFrame and the
   first BeginPopup of the frame. */
void lvMenuPollOpenInput(void);

/* Compact-mode active panel — read from logviewer.c to decide which
   single panel window to render. */
LvCompactPanel lvCompactGetActivePanel(void);
void           lvCompactSetActivePanel(LvCompactPanel p);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_LOGVIEWER_MENU_H */
