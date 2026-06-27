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
 *Name:          Spectator Controller Input
 *Filename:      spectator_input.c
 *Purpose:
 *  Real bodies for the spectator_input.h seam, linked into the
 *  WinBolo client (alongside input_gamepad.c). They read the
 *  already-open singleton gamepad the game owns and surface a
 *  per-frame pan direction and a debounced zoom step for the
 *  live-spectator overview in logviewer.c.
 *
 *  Pan reuses inputGamepadGetScrollDirection — the existing
 *  Steam-Input-aware stick reader (Path A map-scroll action, or
 *  Path B right stick), already radially deadzoned. Zoom reads the
 *  triggers via SDL on the active SDL_Gamepad handle (Path B only;
 *  inputGamepadGetActiveHandle returns NULL under Steam Input, so
 *  trigger-zoom degrades to no-op there while pan still works).
 *
 *  The gamepad is owned by the game (gamefront.c calls
 *  inputGamepadInit). These functions never open/init/close it.
 *********************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "spectator_input.h"
#include "input_gamepad.h"   /* inputGamepadGetActiveHandle / GetScrollDirection */

/* Trigger pull (normalised 0..1) past which a trigger counts as held. Matches
 * input_gamepad.c's TRIGGER_THRESHOLD, redeclared here as that one is private. */
#define SPEC_TRIGGER_THRESHOLD 0.5f

/* SDL reports a trigger axis in 0..32767. */
#define SPEC_TRIGGER_AXIS_MAX 32767.0f

/* Comfortable auto-repeat for a held trigger. */
#define SPEC_ZOOM_REPEAT_MS 150u

bool specInputPollPan(float *dx, float *dy) {
  if (dx) *dx = 0.0f;
  if (dy) *dy = 0.0f;

  /* inputGamepadGetScrollDirection already covers both input paths and returns
   * false when no controller is active or the stick is neutral, so no separate
   * active-handle gate is needed here (Path A has no SDL handle but still
   * scrolls via the Steam map-scroll action). */
  float x = 0.0f, y = 0.0f;
  if (!inputGamepadGetScrollDirection(&x, &y)) {
    return false;
  }
  if (dx) *dx = x;
  if (dy) *dy = y;
  return true;
}

bool specInputPollZoom(int *dir) {
  if (dir) *dir = 0;

  SDL_Gamepad *gp = inputGamepadGetActiveHandle();
  if (gp == NULL) {
    return false;
  }

  float lt = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) /
             SPEC_TRIGGER_AXIS_MAX;
  float rt = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) /
             SPEC_TRIGGER_AXIS_MAX;

  /* Right trigger zooms in, left zooms out; the deeper pull wins if both are
   * held so the result is never ambiguous. */
  int want = 0;
  if (rt > SPEC_TRIGGER_THRESHOLD && rt >= lt) {
    want = 1;
  } else if (lt > SPEC_TRIGGER_THRESHOLD) {
    want = -1;
  }

  static uint32_t lastStepMs = 0;
  static int      lastDir    = 0;

  if (want == 0) {
    lastDir = 0;
    return false;
  }

  uint32_t now = SDL_GetTicks();
  /* Fire immediately on a fresh pull (or a direction flip), then repeat at the
   * auto-repeat interval while the trigger stays held. */
  if (want != lastDir || (now - lastStepMs) >= SPEC_ZOOM_REPEAT_MS) {
    lastStepMs = now;
    lastDir    = want;
    if (dir) *dir = want;
    return true;
  }
  return false;
}
