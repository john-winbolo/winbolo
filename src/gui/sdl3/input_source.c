/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Input Source
 *Filename:      input_source.c
 *Purpose:       See input_source.h.
 *********************************************************/

#include "input_source.h"

#include "../ui_mode.h"

/* Mirrors MOVE_DEADZONE in input_gamepad.c (0.20 normalised).
 * Axis events below this magnitude don't count as a gamepad use —
 * a resting stick that hovers near the centre would otherwise flip
 * the source on every frame the centring drifts. */
#define AXIS_TRIGGER ((Sint16)(32767 * 0.20f))

static InputSource s_current = INPUT_SOURCE_KEYBOARD;

void inputSourceInit(void) {
  switch (uiModeGet()) {
    case UI_MODE_STEAM_DECK: s_current = INPUT_SOURCE_GAMEPAD;  break;
    default:                 s_current = INPUT_SOURCE_KEYBOARD; break;
  }
}

void inputSourceUpdate(const SDL_Event *ev) {
  if (!ev) return;
  switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      s_current = INPUT_SOURCE_KEYBOARD;
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      s_current = INPUT_SOURCE_GAMEPAD;
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      Sint16 v = ev->gaxis.value;
      if (v >= AXIS_TRIGGER || v <= -AXIS_TRIGGER) {
        s_current = INPUT_SOURCE_GAMEPAD;
      }
      break;
    }
    default:
      break;
  }
}

InputSource inputSourceCurrent(void) {
  return s_current;
}
