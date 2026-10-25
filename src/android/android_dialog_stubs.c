/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * android_dialog_stubs.c - The no-op bodies for what the desktop client has
 * and Android does not: the modal log viewer's quit question and the
 * desktop's window-position persistence.
 *
 * The same set src/gui/ios/ios_stubs.c carries for iOS, which leaves out
 * the same two things.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <SDL3/SDL.h>

#include "global.h"

/* ---- log viewer stubs (the modal viewer is not built on Android) ----
 *
 * logViewerRun, logViewerRunFromMemory and spectatorRun are the mobile
 * screens in src/gui/mobile (the recordings list, the reel player and the
 * live spectator feed), as on iOS. */

/* gamefront.c reads this after a log viewer run returns, to learn whether
   the player quit the application from inside it. The modal viewer does not
   run on Android, so it cannot have asked (the same body as ios_stubs.c). */
bool logViewerAppQuitRequested(void) { return false; }

/* ---- window state stubs (desktop-only persistence in sdl3/winbolo.c) ---- */

void windowGetSavedPosition(int *x, int *y) {
  if (x) *x = 0;
  if (y) *y = 0;
}
void windowSetSavedPosition(int x, int y) { (void)x; (void)y; }
void windowGetCustomSize(int *w, int *h) {
  if (w) *w = 0;
  if (h) *h = 0;
}
void windowSetCustomSize(int w, int h) { (void)w; (void)h; }
void windowSaveCurrentPosition(void) {}
void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                     int *outW, int *outH, int *outX, int *outY) {
  if (outW) *outW = actualW;
  if (outH) *outH = actualH;
  if (outX) *outX = actualX;
  if (outY) *outY = actualY;
}
