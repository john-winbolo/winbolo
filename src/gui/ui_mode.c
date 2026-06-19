/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          UI Mode
*Filename:      ui_mode.c
*Purpose:
*  Detects and manages UI mode (desktop vs tablet).
*  Uses SDL3 APIs to detect touch devices, screen size,
*  and platform hints.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "ui_mode.h"
#include "sdl3/input_gamepad.h"

static UIMode s_currentMode = UI_MODE_DESKTOP;
static bool   s_initialized = false;

/* Phase 8.1 — controller-mode pref state.  Mirrored here so callers
   can ask uiShouldUseControllerMode() without a gamefront.h include.
   gamefront.c writes via uiControllerModeSet on prefs load and
   on settings-dialog change. */
static ControllerModePref s_controllerMode = CONTROLLER_MODE_OFF;
static bool               s_controllerPromptAsk = true;

/* UI-scale override.  Default Auto preserves the display-derived scale.
   gamefront.c writes via uiUiScaleSet on prefs load and on settings-dialog
   change. */
static UiScalePref s_uiScalePref = UI_SCALE_AUTO;

/* Shared device preset index for Ctrl+T cycling (-1 = desktop, no preset) */
int g_currentDevicePreset = -1;

/*********************************************************
 * ui_isSteamOS — read /etc/os-release and return true if
 * ID starts with "steamos" (case-insensitive, with or
 * without surrounding quotes). Linux-only file; absent on
 * macOS/Windows so this just returns false there.
 *********************************************************/
static bool ui_isSteamOS(void) {
  FILE *f = fopen("/etc/os-release", "r");
  if (!f) return false;
  char line[256];
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] != 'I' || line[1] != 'D' || line[2] != '=') continue;
    const char *val = line + 3;
    if (*val == '"') val++;
    if ((val[0] == 's' || val[0] == 'S') &&
        (val[1] == 't' || val[1] == 'T') &&
        (val[2] == 'e' || val[2] == 'E') &&
        (val[3] == 'a' || val[3] == 'A') &&
        (val[4] == 'm' || val[4] == 'M') &&
        (val[5] == 'o' || val[5] == 'O') &&
        (val[6] == 's' || val[6] == 'S')) {
      found = true;
    }
    break;
  }
  fclose(f);
  return found;
}

bool uiModeIsSteamDeckHardware(void) {
  const char *hint = SDL_GetHint("SteamDeck");
  if (hint && SDL_strcmp(hint, "1") == 0) return true;
  const char *env = SDL_getenv("SteamDeck");
  if (env && SDL_strcmp(env, "1") == 0) return true;
  return ui_isSteamOS();
}

UIMode uiModeDetect(void) {
  UIMode mode = UI_MODE_DESKTOP;

#if defined(__ANDROID__)
  mode = UI_MODE_TABLET;
#elif defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
  mode = UI_MODE_TABLET;
#endif

  if (mode == UI_MODE_DESKTOP) {
    /* Check for Steam Deck */
    const char *hint = SDL_GetHint("SteamDeck");
    if (hint && SDL_strcmp(hint, "1") == 0) {
      mode = UI_MODE_STEAM_DECK;
    }
    const char *env = SDL_getenv("SteamDeck");
    if (env && SDL_strcmp(env, "1") == 0) {
      mode = UI_MODE_STEAM_DECK;
    }
    /* Fallback: SteamOS hardware, regardless of launch path. */
    if (mode == UI_MODE_DESKTOP && ui_isSteamOS()) {
      mode = UI_MODE_STEAM_DECK;
    }
  }

  if (mode == UI_MODE_DESKTOP) {
    /* Check for touch devices on a small screen */
    int touchCount = 0;
    SDL_TouchID *touchDevices = SDL_GetTouchDevices(&touchCount);
    if (touchDevices && touchCount > 0) {
      /* Has touch — check if screen is small (< 1200px wide suggests tablet/phone) */
      SDL_DisplayID *displays = SDL_GetDisplays(NULL);
      if (displays) {
        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(displays[0]);
        if (dm && dm->w < 1200) {
          mode = UI_MODE_TABLET;
        }
        SDL_free(displays);
      }
    }
    SDL_free(touchDevices);
  }

  s_currentMode = mode;
  s_initialized = true;
  return mode;
}

UIMode uiModeGet(void) {
  if (!s_initialized) {
    uiModeDetect();
  }
  return s_currentMode;
}

void uiModeSet(UIMode m) {
  s_currentMode = m;
  s_initialized = true;
}

bool uiModeIsTablet(void) {
  return uiModeGet() == UI_MODE_TABLET;
}

bool uiModeIsSteamDeck(void) {
  return uiModeGet() == UI_MODE_STEAM_DECK;
}

void uiControllerModeSet(ControllerModePref m) {
  s_controllerMode = m;
}

ControllerModePref uiControllerModeGet(void) {
  return s_controllerMode;
}

void uiControllerPromptAskOnConnectSet(bool ask) {
  s_controllerPromptAsk = ask;
}

bool uiControllerPromptAskOnConnectGet(void) {
  return s_controllerPromptAsk;
}

void uiUiScaleSet(UiScalePref s) {
  s_uiScalePref = s;
}

UiScalePref uiUiScaleGet(void) {
  return s_uiScalePref;
}

float uiUiScalePresetFactor(UiScalePref s) {
  switch (s) {
    case UI_SCALE_SMALL:  return 1.0f;
    case UI_SCALE_MEDIUM: return 1.5f;
    case UI_SCALE_LARGE:  return 2.0f;
    case UI_SCALE_AUTO:
    default:              return 0.0f;
  }
}

bool uiShouldUseControllerMode(void) {
  /* Steam Deck: always controller-mode regardless of pref or connection
     state — the built-in pad is part of the device. */
  if (uiModeIsSteamDeck()) return true;
  /* Desktop: Off is unconditional; On and Auto require a currently-
     connected gamepad.  Tracking the live state means hot-unplug
     reverts to keyboard UX (menu bar comes back) without bookkeeping
     a snapshot, and the previous "Auto snapshots before SDL_INIT_GAMEPAD"
     race goes away because there's nothing to snapshot.  Difference
     between On and Auto now lives entirely in the connect-edge prompt
     trigger: Off asks, On/Auto enter controller mode silently. */
  if (s_controllerMode == CONTROLLER_MODE_OFF) return false;
  return inputGamepadIsConnected();
}
