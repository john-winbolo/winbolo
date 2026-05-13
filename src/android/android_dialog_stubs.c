/*
 * android_dialog_stubs.c - Stub implementations for dialog/host functions
 * not applicable on Android.
 *
 * The ImGui dialog backend (dialog_backend.c) is used with Android-specific
 * implementations of imguiWelcomeShow and imguiGameSetupShow provided by
 * android_welcome.cpp and android_gamesetup.cpp respectively.
 *
 * Mirrors the platform-stub set in src/gui/ios/ios_stubs.c — Android also
 * skips the map editor, log viewer, WBN browser, hosted-server lifecycle,
 * and desktop window-position persistence.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <SDL3/SDL.h>

#include "global.h"

/* Tracker setup stub — not applicable on Android */
int imguiTrackerSetupShow(void) {
  return 0;
}

/* ---- map editor stub ---- */

void mapEditorRun(void *window, void *renderer, const char *mapPath, bool fromMainMenu) {
  (void)window; (void)renderer; (void)mapPath; (void)fromMainMenu;
}

/* ---- log viewer stubs ---- */

void logViewerRun(void *window, void *renderer, const char *logPath, bool fromMainMenu) {
  (void)window; (void)renderer; (void)logPath; (void)fromMainMenu;
}

void logViewerRunFromMemory(void *window, void *renderer, uint8_t *zipData, size_t zipLen, bool fromMainMenu) {
  (void)window; (void)renderer; (void)zipData; (void)zipLen; (void)fromMainMenu;
}

/* ---- WBN browser stub (requires libcurl/cJSON) ---- */

#include "../gui/sdl3/dialogs/imgui_wbn_browser.h"

WbnBrowserResult imguiWbnBrowserShow(struct SDL_Window *window,
                                     struct SDL_Renderer *renderer) {
  (void)window; (void)renderer;
  WbnBrowserResult r = {0};
  r.action = WBN_BROWSER_CLOSE;
  return r;
}

/* ---- server lifecycle stubs (no hosted server on Android) ---- */

#include "../server/server_lifecycle.h"

bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg) {
  (void)sim; (void)cfg;
  return false;
}
void serverInstanceTick(ServerSim *sim) { (void)sim; }
void serverInstanceShutdown(ServerSim *sim) { (void)sim; }
void serverInstanceGetPortmapInfo(ServerPortmapInfo *out) {
  if (out) {
    out->status = SERVER_PORTMAP_DISABLED;
    out->externalIp[0] = '\0';
    out->externalPort = 0;
    out->internalPort = 0;
  }
}
void serverInstanceTriggerManualProbe(void) {}
ManualProbeState serverInstanceGetManualProbeState(void) { return MANUAL_PROBE_IDLE; }

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
void windowSmoothScrolling_toggle(void) {}
void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                     int *outW, int *outH, int *outX, int *outY) {
  if (outW) *outW = actualW;
  if (outH) *outH = actualH;
  if (outX) *outX = actualX;
  if (outY) *outY = actualY;
}
