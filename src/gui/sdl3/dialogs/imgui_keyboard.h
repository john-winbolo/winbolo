/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_keyboard.h
 * Purpose:       Controller-navigable on-screen keyboard.
 *                The universal text-entry fallback when
 *                Steam Input is not driving the pad: a
 *                QWERTY grid (base / shift / symbols layers)
 *                plus a numeric layout, driven by a manual
 *                gamepad cursor and injected into the live
 *                ImGui IO queue so the focused field keeps
 *                its active state.
 *********************************************************/

#ifndef IMGUI_KEYBOARD_H
#define IMGUI_KEYBOARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { OSK_MODE_TEXT = 0, OSK_MODE_NUMERIC, OSK_MODE_PASSWORD };

void keyboardOpen(int mode);   /* mode is one of OSK_MODE_* */
void keyboardRender(void);     /* call inside the frame; no-op unless open */
void keyboardClose(void);
bool keyboardIsOpen(void);
void keyboardUpdate(void);     /* drives the keyboard backend for one frame
                                  (call inside the frame, before ImGui::Render()) */

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_KEYBOARD_H */
