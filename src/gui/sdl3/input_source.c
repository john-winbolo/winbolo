/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Input Source
 *Filename:      input_source.c
 *Purpose:       See input_source.h.
 *********************************************************/

#include "input_source.h"

#include "input_gamepad.h"

/* Mirrors MOVE_DEADZONE in input_gamepad.c (0.20 normalised).
 * Axis events below this magnitude don't count as a gamepad use —
 * a resting stick that hovers near the centre would otherwise flip
 * the source on every frame the centring drifts. */
#define AXIS_TRIGGER ((Sint16)(32767 * 0.20f))

/* Last device the player actually used.  Only consulted once s_haveInput
 * is set; until then the source is derived from real-controller presence
 * (see inputSourceCurrent) so a pad attached at launch starts in
 * controller mode. */
static InputSource s_current   = INPUT_SOURCE_KEYBOARD;
static bool        s_haveInput = false;

/* Frames to ignore mouse-motion as keyboard use after a game-initiated
 * cursor warp — the warp emits an echo motion event a frame later. */
static int         s_warp_ignore_frames = 0;

static void note(InputSource src) {
  s_current   = src;
  s_haveInput = true;
}

void inputSourceInit(void) {
  s_haveInput = false;
  s_current   = INPUT_SOURCE_KEYBOARD;
  s_warp_ignore_frames = 0;
}

void inputSourceNoteGamepad(void)  { note(INPUT_SOURCE_GAMEPAD);  }
void inputSourceNoteKeyboard(void) { note(INPUT_SOURCE_KEYBOARD); }

void inputSourceNoteCursorWarp(void) { s_warp_ignore_frames = 2; }

void inputSourceUpdate(const SDL_Event *ev) {
  if (!ev) return;
  switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
      note(INPUT_SOURCE_KEYBOARD);
      break;
    case SDL_EVENT_MOUSE_MOTION: {
      /* Count only plausible hand movement.  Skip: the echo from a
         game-initiated cursor warp (s_warp_ignore_frames), zero-delta
         events, and teleport-sized jumps — a cursor warp or a window/view
         coordinate change at a screen transition produces a single huge
         delta (hundreds of px in one frame) that no human hand makes; a
         real sweep is a stream of small deltas, so it still registers. */
      const float TELEPORT_PX = 200.0f;
      float ax = ev->motion.xrel < 0 ? -ev->motion.xrel : ev->motion.xrel;
      float ay = ev->motion.yrel < 0 ? -ev->motion.yrel : ev->motion.yrel;
      if (s_warp_ignore_frames == 0 &&
          (ev->motion.xrel != 0.0f || ev->motion.yrel != 0.0f) &&
          ax < TELEPORT_PX && ay < TELEPORT_PX) {
        note(INPUT_SOURCE_KEYBOARD);
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      note(INPUT_SOURCE_GAMEPAD);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      Sint16 v = ev->gaxis.value;
      if (v >= AXIS_TRIGGER || v <= -AXIS_TRIGGER) {
        note(INPUT_SOURCE_GAMEPAD);
      }
      break;
    }
    default:
      break;
  }
}

void inputSourceTick(void) {
  if (s_warp_ignore_frames > 0) s_warp_ignore_frames--;
  /* Steam Input (Path A) controller input is polled, not evented, so the
     SDL_EVENT_GAMEPAD_* cases in inputSourceUpdate never fire for it.
     Catch live controller use here so it counts as gamepad. */
  if (inputGamepadActivityDetected()) {
    note(INPUT_SOURCE_GAMEPAD);
  }
}

InputSource inputSourceCurrent(void) {
  /* No real controller: keyboard/mouse is the only device, so a hot-unplug
     reverts immediately regardless of what was used last. */
  if (!inputGamepadRealControllerConnected()) return INPUT_SOURCE_KEYBOARD;
  /* A real controller is present but the player hasn't touched anything
     yet: start in controller mode (the launch default). */
  if (!s_haveInput) return INPUT_SOURCE_GAMEPAD;
  /* Otherwise follow the most recently used device. */
  return s_current;
}

bool inputSourceAutoSlowdown(bool savedPreference) {
  return savedPreference ||
         (s_haveInput && inputSourceCurrent() == INPUT_SOURCE_GAMEPAD);
}
