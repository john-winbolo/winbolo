/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* Fold this frame's gamepad activity into the last-used source.
 * Steam Input (Path A) controller input is polled, not delivered as
 * SDL events, so call once per frame for it to register. */
void inputSourceTick(void);

/* Mark the last-used device explicitly. Used by the front-end menu loops,
 * which detect controller / mouse use without going through the SDL event
 * pump that inputSourceUpdate reads. */
void inputSourceNoteGamepad(void);
void inputSourceNoteKeyboard(void);

/* The game warps the OS cursor (scroll-tracking, build cursor) which emits a
 * mouse-motion event indistinguishable from real movement. Call this right
 * after a SDL_WarpMouseInWindow so the resulting echo motion isn't mistaken
 * for the player reaching for the mouse (which would wrongly flip to keyboard
 * mode and pop the menu bar). */
void inputSourceNoteCursorWarp(void);

/* Returns the active input source: keyboard when no real controller is
 * present; otherwise the most recently used device (controller at launch
 * before any input). */
InputSource inputSourceCurrent(void);

/* Effective tank setting. Controller use temporarily enables slowdown;
 * mere attachment (the UI's launch default) must not override the saved
 * choice. Keyboard/mouse use or disconnection restores that choice. */
bool inputSourceAutoSlowdown(bool savedPreference);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_SOURCE_H */
