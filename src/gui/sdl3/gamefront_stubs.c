/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *Name:          GameFront Stubs
 *Filename:      gamefront_stubs.c
 *Purpose:
 *  No-op stubs for gamefront.c symbols pulled in by shared
 *  GUI headers (e.g. imgui_fonts.h) on targets that don't
 *  link the full SDL3 game frontend — currently LogViewer
 *  and MapEditor. The real implementations live in
 *  src/gui/sdl3/gamefront.c for WinBolo / WinBoloIOS.
 *********************************************************/

#include "../gamefront.h"

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
