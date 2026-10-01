/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
 *  Path B right stick), already radially deadzoned. Zoom has two
 *  paths to match: under Steam Input (Path A) it reuses the InGame
 *  gunsight_inc/gunsight_dec digital actions (idle while spectating),
 *  since the SDL trigger axes are inert there and there is no SDL
 *  handle; under native SDL (Path B) it reads the triggers on the
 *  active SDL_Gamepad handle.
 *
 *  The gamepad is owned by the game (gamefront.c calls
 *  inputGamepadInit). These functions never open/init/close it.
 *********************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "spectator_input.h"
#include "input_gamepad.h"   /* inputGamepadGetActiveHandle / GetScrollDirection / IsSteamInput */
#include "../../steam/steam_wrapper.h"        /* steam_input_is_action_pressed */
#include "../../steam/steam_input_actions.h"  /* SI_ACTION_GUNSIGHT_INC / _DEC */

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

  int want = 0;

  if (inputGamepadIsSteamInput()) {
    /* Path A: SDL trigger axes are inert under Steam Input and there is no SDL
     * handle, so reuse the InGame gunsight range actions (idle while
     * spectating). inc zooms in, dec zooms out; inc wins if both report. */
    if (steam_input_is_action_pressed(SI_ACTION_GUNSIGHT_INC)) {
      want = 1;
    } else if (steam_input_is_action_pressed(SI_ACTION_GUNSIGHT_DEC)) {
      want = -1;
    }
  } else {
    /* Path B: native SDL gamepad — read the analog triggers directly. */
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
    if (rt > SPEC_TRIGGER_THRESHOLD && rt >= lt) {
      want = 1;
    } else if (lt > SPEC_TRIGGER_THRESHOLD) {
      want = -1;
    }
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
