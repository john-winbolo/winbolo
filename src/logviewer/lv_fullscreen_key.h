/*
 * lv_fullscreen_key.h - is a key press the Log Viewer's full screen key
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Alt+Enter turns full screen on and off, as it does in the game
 * (dialogIsFullScreenToggleEvent in src/gui/sdl3/dialogs/imgui_dialog_utils.h
 * and the same test in sdl3imgui.cpp). This header has the decision with no
 * SDL and no ImGui in it, so the unit tests can run it. The numbers are
 * SDL's; logviewer.c checks them against SDL's own at compile time.
 */

#ifndef LV_FULLSCREEN_KEY_H
#define LV_FULLSCREEN_KEY_H

#include <stdbool.h>

/* SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER. */
#define LV_FSKEY_SCANCODE_RETURN   40u
#define LV_FSKEY_SCANCODE_KP_ENTER 88u
/* SDL_KMOD_LALT, SDL_KMOD_RALT. */
#define LV_FSKEY_MOD_LALT 0x0100u
#define LV_FSKEY_MOD_RALT 0x0200u

typedef enum {
  LV_FSKEY_NOT_OURS = 0, /* some other key: it goes on to the viewer as ever */
  LV_FSKEY_TOGGLE,       /* turn full screen on or off */
  LV_FSKEY_SWALLOW       /* part of the same keystroke: nothing happens, and
                            the viewer does not see it either */
} LvFullScreenKey;

/* What a key-down event is to the full screen command.
 *   repeat    the key is held and this is the keyboard repeating it
 *   mod       the modifier keys held (SDL's key.mod)
 *   scancode  the key (SDL's key.scancode)
 *
 * Enter or the Enter of the number pad, with either Alt key held, is the
 * command. Held down, it is one command: the repeats do nothing. They are
 * kept from the rest of the viewer all the same, since a repeat of Enter
 * that got through would be an Enter in a text box or on a button. The
 * other modifier keys make no difference, as in the game. */
static inline LvFullScreenKey lvFullScreenKey(bool repeat, unsigned int mod,
                                              unsigned int scancode) {
  if ((mod & (LV_FSKEY_MOD_LALT | LV_FSKEY_MOD_RALT)) == 0) return LV_FSKEY_NOT_OURS;
  if (scancode != LV_FSKEY_SCANCODE_RETURN && scancode != LV_FSKEY_SCANCODE_KP_ENTER) {
    return LV_FSKEY_NOT_OURS;
  }
  return repeat ? LV_FSKEY_SWALLOW : LV_FSKEY_TOGGLE;
}

#endif /* LV_FULLSCREEN_KEY_H */
