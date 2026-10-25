/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * ios_stubs.c — Stub implementations for modules not included
 * in the iOS target. The server runtime (server_static: the UDP server,
 * the lifecycle, geolookup without maxminddb) and the server side of
 * WinBolo.net (winbolonet_server) are linked as on the desktop.
 * The cursor is the shared cursor.c: the iPad's classic screen maps a tap to
 * its square through it.
 *
 * Most GUI modules (sdl3imgui, flags, input, dialogs, dns_lookups)
 * are now compiled directly from the shared sdl3 sources.
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"

/* Portable RECT */
#ifndef _WIN32
#ifndef _RECT_DEFINED
#define _RECT_DEFINED
typedef struct { long left, top, right, bottom; } RECT;
#endif
#endif

/* The map editor (mapEditorRun, mapEditorAppQuitRequested,
   mapEditorSetSettingsHandler) is built on iOS; see the map editor block of
   the iOS target in CMakeLists.txt. */

/* ---- log viewer stubs (the modal viewer is not built on iOS) ----
 *
 * logViewerRun is in src/gui/mobile/mobile_recordings.cpp: the recordings on
 * the phone, played in the reel. */

/* gamefront.c reads this after a log viewer run returns, to learn whether
 * the player quit the application from inside it. The modal viewer does not
 * run on iOS, so it cannot have asked. */
bool logViewerAppQuitRequested(void) { return false; }

/* logViewerRunFromMemory, which the WinBolo.net log browser's downloads come
 * to, is in src/gui/mobile/mobile_log_player.cpp: it plays the game in the
 * lobby's reel. */

/* ---- window state stubs (provided by sdl3/winbolo.c, not in iOS build) ---- */

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
/* View > Smooth Scrolling, which the iPad's desktop layout shows, is
   windowSmoothScrolling_toggle in client_frontend_common.c, the desktop's
   body, which also clears the sub-tile drag offset when turned off. */

/* ---- macOS menu-bar stubs ----
 *
 * The native NSMenu menu bar (mac_menubar.mm) is AppKit and desktop-only;
 * iOS has no menu bar. sdl3imgui.cpp and gamefront.c call these behind a
 * plain __APPLE__ check, which is true on iOS too, so they need bodies. */

#include "../sdl3/platform/mac_menubar.h"

void mac_menubar_install(struct SDL_Window *win, void *clientSim) {
    (void)win; (void)clientSim;
}
void mac_menubar_install_dock_menu(void) {}
void mac_menubar_refresh(const struct MacMenuState *s) { (void)s; }

/* spectatorRun, the live spectator feed, is in
 * src/gui/mobile/mobile_spectator.cpp: it shows the feed in the lobby's
 * reel. */
