/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          GUI stubs (wasm)
 *Filename:      gui_stubs_wasm.c
 *Purpose:
 *  Link-time stubs for GUI source files the wasm client
 *  does not build: the input-source tracker
 *  (input_source.c), the tutorial overlay
 *  (imgui_tutorial_overlay.cpp) and the controller-lost
 *  prompt (imgui_controller_disconnect.cpp).  sdl3imgui.cpp
 *  and ui_mode.c reference these symbols unconditionally;
 *  the browser build keeps them as no-ops.
 *
 *  Definitions match the real headers (included below) so
 *  the signatures can't drift from the desktop build.
 *********************************************************/

#include "input_source.h"
#include "dialogs/imgui_tutorial_overlay.h"
#include "dialogs/imgui_controller_disconnect.h"

/* ----- Input source ---------------------------------------------------
 * The browser build always reports keyboard input — there is no
 * glyph-set switching for tutorial hints in wasm, so the device-tracking
 * machinery collapses to a fixed keyboard source. */
void inputSourceInit(void) {}
void inputSourceUpdate(const SDL_Event *ev) { (void)ev; }
void inputSourceTick(void) {}
void inputSourceNoteGamepad(void) {}
void inputSourceNoteKeyboard(void) {}
void inputSourceNoteCursorWarp(void) {}
bool inputSourceCursorWarpActive(void) { return false; }
InputSource inputSourceCurrent(void) { return INPUT_SOURCE_KEYBOARD; }

/* The tutorial overlay is now real in the wasm build — see
 * imgui_tutorial_overlay.cpp (compiled in) and the driver in main_wasm.c. */

/* ----- Controller-lost prompt -----------------------------------------
 * Controller hot-plug handling is desktop/Deck only; the browser build
 * never raises the "controller disconnected" prompt. */
void controllerDisconnectOpen(void) {}
void controllerDisconnectClose(void) {}
bool controllerDisconnectIsOpen(void) { return false; }
bool controllerDisconnectRender(void) { return false; }
