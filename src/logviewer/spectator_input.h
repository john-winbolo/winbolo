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

/*
 * spectator_input — a dependency-free seam for reading the open singleton
 * game controller from the live-spectator overview.
 *
 * logviewer.c carries spectatorRun, which compiles into BOTH the WinBolo
 * client and the standalone Log Viewer. The client links the gui-tier
 * input_gamepad.c (src/gui/sdl3/); the standalone Log Viewer does not. So
 * spectatorRun cannot call inputGamepad* directly without breaking the
 * standalone link. This header is the neutral crossing point — it names only
 * plain types (bool / int / float) and no SDL or gui type. The real bodies
 * live gui-side (src/gui/sdl3/spectator_input.c, reading the singleton
 * gamepad); the standalone links spectator_input_stub.c (no-ops). Mirrors the
 * spectator_drain.h pattern.
 *
 * The gamepad is owned by the game (gamefront.c calls inputGamepadInit); these
 * functions only read it. They never open, init, or close it.
 */

#ifndef SPECTATOR_INPUT_H
#define SPECTATOR_INPUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-frame pan direction from the controller, normalized and deadzoned: each
 * of *dx / *dy is in -1..1 (+x = right, +y = down). Returns false (outputs
 * zeroed) when no controller is active or the stick is in its neutral zone. */
bool specInputPollPan(float *dx, float *dy);

/* Per-frame zoom intent from the triggers, already debounced / repeat-timed in
 * the impl so a held trigger steps at a comfortable rate. Sets *dir to +1 (in,
 * right trigger) or -1 (out, left trigger). Returns false (outputs zeroed) when
 * no controller is active or no zoom step is due this frame. */
bool specInputPollZoom(int *dir);

#ifdef __cplusplus
}
#endif

#endif /* SPECTATOR_INPUT_H */
