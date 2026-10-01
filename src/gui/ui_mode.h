/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* UI-scale override.  Auto keeps the display-derived scale; the presets
   pin the in-game / dialog ImGui scale to a fixed factor.  Desktop-only —
   Steam Deck, tablet, and mobile keep their own scaling paths and ignore
   this. */
typedef enum {
  UI_SCALE_AUTO   = 0,
  UI_SCALE_SMALL  = 1,
  UI_SCALE_MEDIUM = 2,
  UI_SCALE_LARGE  = 3
} UiScalePref;

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

/*********************************************************
*NAME:          uiShouldUseControllerMode
*PURPOSE:
*  Phase 8.1: returns true when the UI should run in
*  controller-first mode (menu bar hidden, Start opens the
*  pause overlay, dialog auto-size disabled).  True when
*  any of:
*    - current UI mode is Steam Deck (always controller),
*    - controllerMode pref is ON,
*    - controllerMode is AUTO and a gamepad was connected
*      at startup (snapshotted on first call).
*  Does NOT flip mid-session for AUTO — the controller-
*  detected prompt handles hot-plug.
*********************************************************/
bool uiShouldUseControllerMode(void);

/*********************************************************
*NAME:          uiUiScaleSet / Get
*PURPOSE:
*  In-process getter/setter for the UI-scale override.
*  As with the controller-mode pref, the on-disk value is
*  owned by gamefront.c; these mirror it so the scale
*  helpers and settings UI can read it without dragging in
*  gamefront.h.  Set from prefs load and the settings UI.
*********************************************************/
void        uiUiScaleSet(UiScalePref s);
UiScalePref uiUiScaleGet(void);

/*********************************************************
*NAME:          uiUiScalePresetFactor
*PURPOSE:
*  Fixed scale factor for a preset: Small 1.0, Medium 1.5,
*  Large 2.0.  Returns 0 for UI_SCALE_AUTO so callers can
*  tell "no override" apart from a real factor.
*********************************************************/
float uiUiScalePresetFactor(UiScalePref s);

#ifdef __cplusplus
}
#endif

#endif /* UI_MODE_H */
