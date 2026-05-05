/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Gamepad Input
*Filename:      input_gamepad.c
*Purpose:
*  Gamepad input handling. Polls SDL3 gamepad state each
*  frame, converting axis/button state into tank
*  movement, fire, mine, gunsight, and scroll commands.
*
*  Single-controller policy: first-connected wins. On
*  disconnect, walks SDL_GetGamepads() to promote the
*  next available gamepad. Caches the active handle
*  so haptic / queries don't pay open/close overhead.
*********************************************************/

#include <math.h>
#include <SDL3/SDL.h>
#include "input_gamepad.h"
#include "input_joystick.h"

/* Axis normalisation: int16 range to -1..1 */
#define AXIS_NORM (1.0f / 32767.0f)

/* Movement and scroll deadzones (radial, normalised) */
#define MOVE_DEADZONE  0.20f
#define MOVE_MAX_REACH 0.95f
#define SCROLL_DEADZONE  0.20f
#define SCROLL_MAX_REACH 0.95f

/* Trigger threshold (normalised 0..1) */
#define TRIGGER_THRESHOLD 0.5f

/* --- State --- */

static SDL_Gamepad     *s_activeGamepad = NULL;
static SDL_JoystickID   s_activeId      = 0;
static SDL_GamepadType  s_activeType    = SDL_GAMEPAD_TYPE_UNKNOWN;

/* Latch so joystickResetState() fires once per stick-centred transition. */
static bool s_moveWasActive = false;

/* Edge-triggered gunsight delta: -1/+1 set by button-down, consumed on read. */
static int s_gunsightPending = 0;

/* --- Helpers --- */

static void openGamepadById(SDL_JoystickID id) {
  SDL_Gamepad *gp = SDL_OpenGamepad(id);
  if (!gp) return;
  s_activeGamepad = gp;
  s_activeId      = id;
  s_activeType    = SDL_GetGamepadType(gp);
}

static void clearActive(void) {
  s_activeGamepad = NULL;
  s_activeId      = 0;
  s_activeType    = SDL_GAMEPAD_TYPE_UNKNOWN;
}

static void promoteNextGamepad(void) {
  int count = 0;
  SDL_JoystickID *list = SDL_GetGamepads(&count);
  if (list) {
    for (int i = 0; i < count; i++) {
      if (list[i] != 0) {
        openGamepadById(list[i]);
        if (s_activeGamepad) break;
      }
    }
    SDL_free(list);
  }
}

/* --- Init / Shutdown --- */

void inputGamepadInit(void) {
  clearActive();
  s_moveWasActive   = false;
  s_gunsightPending = 0;

  /* Steam Deck built-in controller HIDAPI access. With a real Steam
     App ID, Steam Input handles this automatically; this hint covers
     development and non-Steam builds. */
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");

  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    return;
  }

  promoteNextGamepad();
}

void inputGamepadShutdown(void) {
  if (s_activeGamepad) {
    SDL_CloseGamepad(s_activeGamepad);
  }
  clearActive();
}

/* --- Event processing --- */

void inputGamepadProcessEvent(const SDL_Event *e) {
  if (!e) return;

  switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      if (s_activeGamepad == NULL) {
        openGamepadById(e->gdevice.which);
      }
      break;

    case SDL_EVENT_GAMEPAD_REMOVED:
      if (s_activeGamepad && e->gdevice.which == s_activeId) {
        SDL_CloseGamepad(s_activeGamepad);
        clearActive();
        promoteNextGamepad();
      }
      break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      if (s_activeGamepad && e->gbutton.which == s_activeId) {
        switch (e->gbutton.button) {
          case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            s_gunsightPending = -1;
            break;
          case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            s_gunsightPending = 1;
            break;
          default:
            break;
        }
      }
      break;

    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      /* No-op: held-state queries read live via SDL_GetGamepadButton. */
      break;

    default:
      break;
  }
}

/* --- Queries --- */

bool inputGamepadIsConnected(void) {
  return s_activeGamepad != NULL;
}

SDL_Gamepad *inputGamepadGetActiveHandle(void) {
  return s_activeGamepad;
}

SDL_GamepadType inputGamepadGetActiveType(void) {
  return s_activeType;
}

tankButton inputGamepadGetMovement(BYTE tankAngle) {
  if (!s_activeGamepad) {
    if (s_moveWasActive) {
      joystickResetState();
      s_moveWasActive = false;
    }
    return TNONE;
  }

  float x = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_LEFTX) * AXIS_NORM;
  float y = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_LEFTY) * AXIS_NORM;
  float dist = sqrtf(x * x + y * y);

  if (dist < MOVE_DEADZONE) {
    if (s_moveWasActive) {
      joystickResetState();
      s_moveWasActive = false;
    }
    return TNONE;
  }

  s_moveWasActive = true;

  if (joystickGetAbsoluteSteering()) {
    return joystickGetMovementAbsolute(x, y, dist, MOVE_DEADZONE, MOVE_MAX_REACH, tankAngle);
  } else {
    return joystickGetMovementRelative(x, y, dist, MOVE_DEADZONE, MOVE_MAX_REACH);
  }
}

bool inputGamepadIsFireHeld(void) {
  if (!s_activeGamepad) return false;

  if (SDL_GetGamepadButton(s_activeGamepad, SDL_GAMEPAD_BUTTON_SOUTH)) return true;

  float rt = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * AXIS_NORM;
  return rt > TRIGGER_THRESHOLD;
}

bool inputGamepadIsMineHeld(void) {
  if (!s_activeGamepad) return false;

  if (SDL_GetGamepadButton(s_activeGamepad, SDL_GAMEPAD_BUTTON_EAST)) return true;

  float lt = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * AXIS_NORM;
  return lt > TRIGGER_THRESHOLD;
}

int inputGamepadGetGunsightChange(void) {
  int v = s_gunsightPending;
  s_gunsightPending = 0;
  return v;
}

bool inputGamepadGetScrollDirection(float *dx, float *dy) {
  if (dx) *dx = 0.0f;
  if (dy) *dy = 0.0f;
  if (!s_activeGamepad) return false;

  float x = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_RIGHTX) * AXIS_NORM;
  float y = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_RIGHTY) * AXIS_NORM;
  float dist = sqrtf(x * x + y * y);
  if (dist < SCROLL_DEADZONE) return false;

  /* Radial deadzone with outer-reach clamp to (-1..1). */
  float clampDist = dist > SCROLL_MAX_REACH ? SCROLL_MAX_REACH : dist;
  float reach = (clampDist - SCROLL_DEADZONE) / (SCROLL_MAX_REACH - SCROLL_DEADZONE);
  float scale = reach / dist;  /* re-scale unit vector by reach */

  if (dx) *dx = x * scale;
  if (dy) *dy = y * scale;
  return true;
}

void inputGamepadRumble(float strength, Uint32 durationMs) {
  if (!s_activeGamepad) return;
  if (strength < 0.0f) strength = 0.0f;
  if (strength > 1.0f) strength = 1.0f;
  Uint16 mag = (Uint16)(strength * 65535.0f);
  SDL_RumbleGamepad(s_activeGamepad, mag, mag, durationMs);
}
