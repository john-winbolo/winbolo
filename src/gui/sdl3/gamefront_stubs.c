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
