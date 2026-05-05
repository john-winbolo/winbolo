/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          UI Mode
*Filename:      ui_mode.h
*Purpose:
*  Detects and manages UI mode (desktop vs tablet).
*  Tablet mode enables fullscreen game view, touch
*  controls, and ImGui overlay status panels.
*********************************************************/

#ifndef UI_MODE_H
#define UI_MODE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  UI_MODE_DESKTOP    = 0,
  UI_MODE_TABLET     = 1,
  UI_MODE_STEAM_DECK = 2
} UIMode;

/*********************************************************
*NAME:          uiModeDetect
*PURPOSE:
*  Auto-detect UI mode from platform, touch devices,
*  and screen size. Returns the detected mode.
*********************************************************/
UIMode uiModeDetect(void);

/*********************************************************
*NAME:          uiModeGet
*PURPOSE:
*  Returns the current UI mode.
*********************************************************/
UIMode uiModeGet(void);

/*********************************************************
*NAME:          uiModeSet
*PURPOSE:
*  Manually override the UI mode. Use for testing
*  tablet layout on desktop.
*********************************************************/
void uiModeSet(UIMode m);

/*********************************************************
*NAME:          uiModeIsTablet
*PURPOSE:
*  Returns true ONLY for UI_MODE_TABLET (iOS, Android,
*  small touchscreens). Steam Deck uses the desktop UI
*  as its baseline and does NOT inherit tablet behaviour
*  (touch widgets, fullscreen overlay, no menu bar).
*  Deck-specific divergences gate on uiModeIsSteamDeck().
*********************************************************/
bool uiModeIsTablet(void);

/*********************************************************
*NAME:          uiModeIsSteamDeck
*PURPOSE:
*  Convenience: returns true if current mode is Steam Deck.
*********************************************************/
bool uiModeIsSteamDeck(void);

/*********************************************************
*NAME:          uiModeIsSteamDeckHardware
*PURPOSE:
*  Hardware-only check: true if running on Steam Deck
*  hardware (SteamDeck=1 hint/env, or /etc/os-release ID
*  starts with "steamos"). Independent of the current UI
*  mode, so callers can branch on real hardware before
*  uiModeDetect() finalises s_currentMode.
*********************************************************/
bool uiModeIsSteamDeckHardware(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MODE_H */
