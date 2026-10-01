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
 *Name:          GameFront Stubs
 *Filename:      gamefront_stubs.c
 *Purpose:
 *  No-op stubs for gamefront.c symbols pulled in by shared
 *  GUI headers (e.g. imgui_fonts.h) on targets that don't
 *  link the full SDL3 game frontend — currently LogViewer
 *  and MapEditor. The real implementations live in
 *  src/gui/sdl3/gamefront.c for WinBolo / WinBoloIOS.
 *********************************************************/

#include <stdbool.h>
#include "../gamefront.h"
#include "input_source.h"
#include "dialogs/imgui_mapchooser.h"  /* mapChooserMapHasScript */

void gameFrontGetLanguageCode(char *out, int outSize) {
  if (!out || outSize <= 0) return;
  out[0] = '\0';
}

/* Pulled in by shared imgui dialogs (e.g. the reused WBN browser) that
 * call gameFrontPumpDirty to flush cloud-prefs changes. No frontend to
 * pump on LogViewer / MapEditor, so this is a no-op; the real pump lives
 * in gamefront.c. */
void gameFrontPumpDirty(void) {
}

/* Pulled in transitively by imgui_dialog_utils.h's dialogSaveCurrentPosition /
 * dialogRestorePosition. The real definitions live in gamefront.c (main game
 * only). For LogViewer / MapEditor the values are unused at runtime — those
 * binaries don't read or persist these elsewhere — but the linker needs the
 * symbols to resolve. -1 means "no saved position", matching gamefront.c. */
int gameFrontDialogX = -1;
int gameFrontDialogY = -1;

/* Gamepad / Steam Input stubs — LogViewer and MapEditor don't link the
 * full SDL3 input stack (input_gamepad.c, input_source.c, imgui_steam_nav.cpp).
 * ui_mode.c and the shared WBN browser reach for these symbols; on stub-only
 * targets they behave as "no gamepad / keyboard source / menu set active". */
bool inputGamepadIsConnected(void) { return false; }
void imguiSteamNavActivateMenuSet(void) {}
void imguiSteamNavFeedCurrentContext(void) {}
int imguiSteamNavConsumeMenuTabShift(void) { return 0; }
bool inputGamepadGetScrollDirection(float *dx, float *dy) {
  if (dx) *dx = 0.0f;
  if (dy) *dy = 0.0f;
  return false;
}

/* uiShouldUseControllerMode() consults inputSourceCurrent(). LogViewer /
 * MapEditor have no controller input, so report keyboard — keeps the desktop
 * menu bar and keyboard-style dialogs. */
InputSource inputSourceCurrent(void) { return INPUT_SOURCE_KEYBOARD; }

/* The map chooser tags a map that has a script beside it. MapEditor builds
 * the chooser without the scenario library, and has nothing to run a script
 * with anyway, so every map reads plain there. */
bool mapChooserMapHasScript(const char *mapPath) {
  (void)mapPath;
  return false;
}
