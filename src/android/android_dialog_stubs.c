/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * android_dialog_stubs.c - Stub implementations for dialog functions not
 * yet carried on Android.
 *
 * Mirrors the platform-stub set in src/gui/ios/ios_stubs.c — Android also
 * skips the map editor, the modal log viewer and desktop window-position
 * persistence.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <SDL3/SDL.h>

#include "global.h"

/* ---- map editor stubs ---- */

void mapEditorRun(void *window, void *renderer, const char *mapPath, bool fromMainMenu) {
  (void)window; (void)renderer; (void)mapPath; (void)fromMainMenu;
}

/* The editor never runs here, so it cannot have asked to quit the app and
   has no Settings menu item to hand a handler to. */
bool mapEditorAppQuitRequested(void) { return false; }
void mapEditorSetSettingsHandler(void (*fn)(void)) { (void)fn; }

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
