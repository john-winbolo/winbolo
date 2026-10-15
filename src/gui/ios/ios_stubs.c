/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * ios_stubs.c — Stub implementations for modules not included
 * in the iOS target. Covers: the server side of winbolonet, geolookup,
 * and cursor.
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
#include "../../winbolonet/winbolonet_server.h"

/* Portable RECT */
#ifndef _WIN32
#ifndef _RECT_DEFINED
#define _RECT_DEFINED
typedef struct { long left, top, right, bottom; } RECT;
#endif
#endif

/* ---- cursor stubs ---- */

bool cursorSetup(void) { return true; }
void cursorCleanup(void) {}
void cursorSetCursor(bool normalCurs) { (void)normalCurs; }
void cursorMove(int mouseX, int mouseY) { (void)mouseX; (void)mouseY; }
bool cursorPos(RECT *rcWindow, BYTE *xValue, BYTE *yValue,
               int subPosX, int subPosY) {
    (void)rcWindow; (void)xValue; (void)yValue;
    (void)subPosX; (void)subPosY;
    return false;
}
void cursorAcquireCursor(void) {}
void cursorLeaveWindow(void) {}
bool cursorIsWarpEcho(float winX, float winY) { (void)winX; (void)winY; return false; }
void cursorAnchorToView(int xOffset, int yOffset, int subPosX, int subPosY) {
    (void)xOffset; (void)yOffset; (void)subPosX; (void)subPosY;
}
void cursorDropAnchor(void) {}
void moveMousePointer(int value) { (void)value; }

/* ---- winbolonet server stubs ----
 *
 * winbolonet_core and winbolonet_client are linked (with http_nsurl.m for
 * the transport). winbolonet_server is not, because a phone does not host
 * games, so the server-side calls bolo, the sim, gamefront.c and
 * winbolonet_core.c still reference get bodies here that do nothing. */

bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
    (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
    (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
    return false;
}

void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
    (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}
void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams) {
    (void)array; (void)length; (void)numTeams;
}
void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases, BYTE numFreePills, bool sendNow) {
    (void)numPlayers; (void)numFreeBases; (void)numFreePills; (void)sendNow;
}
bool winboloNetIsPlayerParticipant(BYTE playerNum) { (void)playerNum; return false; }
void winboloNetSendLock(bool isLocked) { (void)isLocked; }
bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName, BYTE playerNum, char *errorMsg, bool *hasSteam, bool *isSupporter) {
    (void)playerKey; (void)playerName; (void)playerNum; (void)errorMsg;
    if (hasSteam)    *hasSteam    = false;
    if (isSupporter) *isSupporter = false;
    return false;
}
bool winboloNetVerifySpectatorKey(const char *spectatorKey, const char *playerName, char *errorMsg, bool *isLoggedIn) {
    (void)spectatorKey; (void)playerName; (void)errorMsg;
    if (isLoggedIn) *isLoggedIn = false;
    return false;
}
void winboloNetSpectatorLeaveGame(const char *spectatorKey) {
    (void)spectatorKey;
}

/* ---- geolookup stubs (requires libmaxminddb) ---- */

bool geoLookupCreate(const char *mmdbPath) { (void)mmdbPath; return false; }
void geoLookupDestroy(void) {}
bool geoLookupCountry(const char *ipStr, char countryCode[3]) {
    (void)ipStr;
    countryCode[0] = '\0';
    return false;
}
bool geoLookupIsLoaded(void) { return false; }

/* ---- map editor stubs (not available on iOS) ---- */

void mapEditorRun(void *window, void *renderer, const char *mapPath, bool fromMainMenu) {
    (void)window; (void)renderer; (void)mapPath; (void)fromMainMenu;
}

/* ---- log viewer stubs (not available on iOS) ---- */

void logViewerRun(void *window, void *renderer, const char *logPath, bool fromMainMenu) {
    (void)window; (void)renderer; (void)logPath; (void)fromMainMenu;
}

/* gamefront.c reads these after a map editor or log viewer run returns, to
 * learn whether the player quit the application from inside one. Neither
 * tool runs on iOS, so neither can have asked. */
bool logViewerAppQuitRequested(void) { return false; }
bool mapEditorAppQuitRequested(void) { return false; }

/* gamefront.c hands the embedded map editor the Settings dialog opener
 * before running it. No editor here to hand it to. The parameter is
 * mapeditor.h's MapEditorSettingsFn spelled out, because that header also
 * declares mapEditorRun with SDL types and the stub above takes void
 * pointers. */
void mapEditorSetSettingsHandler(void (*fn)(void)) { (void)fn; }

#include <stdint.h>
#include <stddef.h>

void logViewerRunFromMemory(void *window, void *renderer, uint8_t *zipData, size_t zipLen, bool fromMainMenu) {
    (void)window; (void)renderer; (void)zipData; (void)zipLen; (void)fromMainMenu;
}

/* ---- WBN browser stub (imgui_wbn_browser.cpp is not built on iOS) ---- */

#include "../sdl3/dialogs/imgui_wbn_browser.h"

WbnBrowserResult imguiWbnBrowserShow(struct SDL_Window *window,
                                     struct SDL_Renderer *renderer) {
    (void)window; (void)renderer;
    WbnBrowserResult r = {0};
    r.action = WBN_BROWSER_CLOSE;
    return r;
}

/* ---- server lifecycle (local-only) ----
 *
 * iOS links server_sim_static but not server_static, so the real
 * serverInstanceStartup / Tick bodies in server_lifecycle.c are not on the
 * link line. A phone does not host games, so everything those bodies do
 * beyond the local case — UDP transport, WinBolo.net, tracker, NAT portmap,
 * brain recording, free-disk checks — has nothing to drive here. What is
 * left is the local-only subset, and single-player needs it:
 * gameFrontStartServerSim refuses to arm the host timer unless startup
 * returns true, and that timer is the only thing that ticks the sim.
 *
 * serverSimApplyInstanceConfig is what applies cfg's lobby / skipLobby
 * state transition along with the bot brain path, AI type, password and
 * view player; it is declared in bolo's internal server_sim_lifecycle.h,
 * hence this file's per-file grant in CMakeLists.txt.
 *
 * src/wasm/server_stubs_wasm.c is the same shape and is the peer to keep
 * this in step with. */

#include "../../server/server_lifecycle.h"
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig,
                                    * serverSimLocalTick */

bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg) {
    if (sim == NULL || cfg == NULL) return false;
    serverSimApplyInstanceConfig(sim, cfg);
    return true;
}

/* Ticks run, counted at the top of each call the way the UDP server counts
 * its receive passes. The lobby-slot heartbeat reads it. */
static uint32_t s_localTickCount = 0;

/* One slice of the local server, driven by gamefront.c's host timer. That
 * callback holds the threading mutex across the call, so nothing here takes
 * it. The lobby and round sequence (bot tick, sim tick, the game-over,
 * game-start, countdown and return-to-lobby edges, the slot heartbeat) has
 * one body, serverSimLocalTick, shared with the dedicated server and the web
 * client so the three cannot drift. No hooks: there is no network server
 * around this one. */
void serverInstanceTick(ServerSim *sim) {
    if (sim == NULL) return;
    s_localTickCount++;
    serverSimLocalTick(sim, s_localTickCount, NULL, NULL);
}

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
/* NAT hole punching belongs to the hosting path, which iOS does not have. */
bool serverInstanceIsNatPunchActive(void) { return false; }

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
void windowSmoothScrolling_toggle(void) {}

/* ---- winbolonet server stubs: lobby and map status ---- */

void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
    (void)mapName; (void)numBases; (void)numPills; (void)freeBases; (void)freePills;
}

void winbolonetSetLobbyInfo(const WbnLobbyInfo *info) { (void)info; }

void winbolonetSendLobbyUpdate(void) { }

/* ---- log stubs (log.c requires minizip/zlib) ---- */

#include "log.h"

void logCreate(void) {}
void logWriteEmpty(void) {}
void logWriteEvents(BYTE key) { (void)key; }
void logWriteTick(void) {}
void logStop(void) {}
bool logIsRecording(void) { return false; }
void logAddEvent(logitem itemNum, BYTE opt1, BYTE opt2, BYTE opt3, BYTE opt4, unsigned short short1, char *words) {
    (void)itemNum; (void)opt1; (void)opt2; (void)opt3; (void)opt4; (void)short1; (void)words;
}
void logDestroy(void) {}
bool logStart(char *fn, ServerSim *ssim, BYTE ai, BYTE maxPlayers, bool usePassword) {
    (void)fn; (void)ssim; (void)ai; (void)maxPlayers; (void)usePassword;
    return false;
}
bool logWriteSnapshot(ServerSim *ssim, bool check) {
    (void)ssim; (void)check;
    return false;
}
bool logCheckTankSame(BYTE playerNum, BYTE mx, BYTE my, BYTE pxy, BYTE opt) {
    (void)playerNum; (void)mx; (void)my; (void)pxy; (void)opt;
    return true;
}
void logSetLobbyMode(bool enabled) { (void)enabled; }
void logAddMapName(ServerSim *ssim) { (void)ssim; }

/* ---- winbolonet server stub: session end ----
 *
 * gamefront.c ends a hosted session before it leaves; there is never one
 * here. */

void winbolonetEndSession(uint32_t drainMaxMs) { (void)drainMaxMs; }

/* ---- dedicated-server round log stubs ----
 *
 * The .wbv round log the host records and uploads lives in
 * server_dedicated_log.c (server_static), which iOS does not link because a
 * phone does not host games. gamefront.c drives these on every game start
 * and leave, so they need bodies; there is never a round to stash or send. */

#include "../../server/server_dedicated_log.h"

void serverDedicatedLogInstall(struct ServerSim *sim, bool dontSendLogFlag) {
    (void)sim; (void)dontSendLogFlag;
}
void serverDedicatedLogUninstall(void) {}
void serverDedicatedLogSetServeMode(int mode) { (void)mode; }
void serverDedicatedLogSetCompletedPath(const char *path) { (void)path; }
void serverDedicatedLogStashCurrentRound(void) {}
void serverDedicatedLogFlushPendingUpload(void) {}
bool serverDedicatedLogHasPendingUpload(void) { return false; }

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

/* ---- spectator stub ----
 *
 * The live delayed spectator feed is hosted by logviewer.c, and the iOS
 * target links neither src/logviewer nor spectator_input.c. Returning false
 * is "the user left or the feed never loaded", which sends gamefront.c back
 * to the menu instead of into a live lobby. */

bool spectatorRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  void *cs, const char *serverHost, uint16_t serverPort) {
    (void)window; (void)renderer; (void)cs; (void)serverHost; (void)serverPort;
    return false;
}
