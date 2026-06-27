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
 *Name:          Spectator Controller Input Stubs
 *Filename:      spectator_input_stub.c
 *Purpose:
 *  No-op stubs for the spectator_input.h seam, linked only into
 *  the standalone Log Viewer.
 *
 *  logviewer.c carries spectatorRun, which reads the game
 *  controller through this seam. The real bodies live gui-side in
 *  src/gui/sdl3/spectator_input.c (reading the singleton gamepad),
 *  which the WinBolo client links alongside input_gamepad.c. The
 *  standalone Log Viewer compiles logviewer.c but links no gui-tier
 *  gamepad module and never spectates, so these stubs resolve the
 *  seam symbols. They report "no controller, no step".
 *********************************************************/

#include <stdbool.h>

#include "spectator_input.h"

bool specInputPollPan(float *dx, float *dy) {
  if (dx) *dx = 0.0f;
  if (dy) *dy = 0.0f;
  return false;
}

bool specInputPollZoom(int *dir) {
  if (dir) *dir = 0;
  return false;
}
