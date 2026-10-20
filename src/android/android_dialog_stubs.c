/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * android_dialog_stubs.c - Stub implementations for dialog functions not
 * yet carried on Android.
 *
 * Mirrors the platform-stub set in src/gui/ios/ios_stubs.c — Android also
 * skips the map editor, log viewer and desktop window-position
 * persistence.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "../common/wb_log.h"

/* ---- map editor stubs ---- */

void mapEditorRun(void *window, void *renderer, const char *mapPath, bool fromMainMenu) {
  (void)window; (void)renderer; (void)mapPath; (void)fromMainMenu;
}

/* The editor never runs here, so it cannot have asked to quit the app and
   has no Settings menu item to hand a handler to. */
bool mapEditorAppQuitRequested(void) { return false; }
void mapEditorSetSettingsHandler(void (*fn)(void)) { (void)fn; }

/* ---- log viewer stubs ---- */

/* A recording handed over by another app reaches this through the welcome
   screen's kept-drop path (android_drops.c); the line says it got here. */
void logViewerRun(void *window, void *renderer, const char *logPath, bool fromMainMenu) {
  (void)window; (void)renderer; (void)fromMainMenu;
  WB_LOG_INFO(WB_LOG_CAT_GUI, "[Android] Log viewer not carried yet; asked to play %s",
              logPath ? logPath : "(no file)");
}

void logViewerRunFromMemory(void *window, void *renderer, uint8_t *zipData, size_t zipLen, bool fromMainMenu) {
  (void)window; (void)renderer; (void)zipData; (void)zipLen; (void)fromMainMenu;
}

/* Same as the editor: the modal viewer does not run here, so it cannot
   have asked to quit the app. */
bool logViewerAppQuitRequested(void) { return false; }

/* The live spectator feed. Returns false, "the user left", so the caller
   goes back to the welcome screen. */
bool spectatorRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  void *cs, const char *serverHost, uint16_t serverPort) {
  (void)window; (void)renderer; (void)cs; (void)serverHost; (void)serverPort;
  return false;
}

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
