/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Input Source
 *Filename:      input_source.h
 *Purpose:
 *  Tracks which input device the player most recently
 *  used (keyboard vs gamepad). Drives glyph-set selection
 *  for tutorial dialogs and any other UI that wants to
 *  show button hints in the appropriate iconography.
 *
 *  Mouse motion is ignored (too easy to nudge); a mouse
 *  button click counts as keyboard. Gamepad axis motion
 *  must exceed the stick deadzone before it counts as a
 *  gamepad event so a resting stick at idle bias doesn't
 *  flip the source.
 *********************************************************/

#ifndef INPUT_SOURCE_H
#define INPUT_SOURCE_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  INPUT_SOURCE_KEYBOARD = 0,
  INPUT_SOURCE_GAMEPAD  = 1
} InputSource;

/* Initialise the default source from the current UI mode.
 * Call once after uiModeDetect()/uiModeSet() has settled. */
void inputSourceInit(void);

/* Inspect an SDL event and update the last-used source if
 * relevant. Safe to call for every event in the main pump. */
void inputSourceUpdate(const SDL_Event *ev);

/* Returns the most recently used input source. */
InputSource inputSourceCurrent(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_SOURCE_H */
