/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * ios_stubs.c — Stub implementations for modules not included
 * in the iOS target. Covers: winbolonet, geolookup, and cursor.
 *
 * Most GUI modules (sdl3imgui, flags, input, dialogs, dns_lookups)
 * are now compiled directly from the shared sdl3 sources.
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "../../winbolonet/winbolonet_server.h"
#include "../../winbolonet/winbolonet_client.h"

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
void cursorMove(int mouseX, int mouseY) { (void)mouseX; (void)mouseY; }
bool cursorPos(RECT *rcWindow, BYTE *xValue, BYTE *yValue,
               int subPosX, int subPosY) {
    (void)rcWindow; (void)xValue; (void)yValue;
    (void)subPosX; (void)subPosY;
    return false;
}
void cursorAcquireCursor(void) {}
void cursorLeaveWindow(void) {}
void moveMousePointer(int value) { (void)value; }

/* ---- winbolonet stubs (requires libcurl) ---- */

bool winbolonetCreateServer(char *mapName, unsigned short port, BYTE gameType, BYTE ai, bool mines, bool password, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills, BYTE numPlayers) {
    (void)mapName; (void)port; (void)gameType; (void)ai; (void)mines; (void)password;
    (void)numBases; (void)numPills; (void)freeBases; (void)freePills; (void)numPlayers;
    return false;
}

void winbolonetDestroy(bool isServer) { (void)isServer; }

void winboloNetGetServerKey(char *keyBuff) { if (keyBuff) keyBuff[0] = '\0'; }
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
    (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}
void winbolonetGoodbye(void) {}
void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams) {
    (void)array; (void)length; (void)numTeams;
}
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB, bool aIsBot, bool bIsBot) {
    (void)eventType; (void)isServer; (void)playerA; (void)playerB; (void)aIsBot; (void)bIsBot;
}
void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases, BYTE numFreePills, bool sendNow) {
    (void)numPlayers; (void)numFreeBases; (void)numFreePills; (void)sendNow;
}
bool winbolonetIsRunning(void) { return false; }
bool winboloNetIsPlayerParticipant(BYTE playerNum) { (void)playerNum; return false; }
void winboloNetSendLock(bool isLocked) { (void)isLocked; }
bool winboloNetVerifyClientKey(const char *playerKey, const char *playerName, BYTE playerNum, char *errorMsg, bool *hasSteam, bool *isSupporter) {
    (void)playerKey; (void)playerName; (void)playerNum; (void)errorMsg;
    if (hasSteam)    *hasSteam    = false;
    if (isSupporter) *isSupporter = false;
    return false;
}
bool winbolonetClientJoinSession(const char *apiToken, const char *serverKey, char *playerKeyOut, char *errorMsg) {
    (void)apiToken; (void)serverKey; (void)errorMsg;
    if (playerKeyOut) playerKeyOut[0] = '\0';
    return false;
}
bool winbolonetClientJoinSpectatorSession(const char *apiToken, const char *serverKey, const char *playerName, char *spectatorKeyOut, char *errorMsg) {
    (void)apiToken; (void)serverKey; (void)playerName; (void)errorMsg;
    if (spectatorKeyOut) spectatorKeyOut[0] = '\0';
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
bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
    (void)username; (void)password; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg;
    return false;
}
bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
    (void)steamTicketHex; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg;
    return false;
}
bool winbolonetAuthSteamRegister(const char *steamTicketHex, const char *username, const char *email, char *tokenOut, char *expiryOut, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg, char *errorCodeOut) {
    (void)steamTicketHex; (void)username; (void)email; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg; (void)errorCodeOut;
    return false;
}
bool winbolonetAuthValidate(const char *token, char *playerNameOut, int *rankOut, int *rankTotalOut, WbnStats *statsOut, char *errorMsg) {
    (void)token; (void)playerNameOut; (void)rankOut; (void)rankTotalOut; (void)statsOut; (void)errorMsg;
    return false;
}

/* ---- winbolonet thread stubs ---- */

bool winbolonetThreadCreate(void) { return true; }
void winbolonetThreadDestroy(void) {}
void winbolonetThreadAddRequest(const char *ep, const char *jb) { (void)ep; (void)jb; }

/* ---- winbolonet events stubs ---- */

void winbolonetEventsCreate(void) {}
void winbolonetEventsDestroy(void) {}
void winbolonetEventsAddItem(BYTE itemType, const char *keyA, const char *keyB, bool aIsBot, bool bIsBot) {
    (void)itemType; (void)keyA; (void)keyB; (void)aIsBot; (void)bIsBot;
}
int winbolonetEventsGetSize(void) { return 0; }
BYTE winbolonetEventsRemove(char *keyA, char *keyB, bool *aIsBot, bool *bIsBot) {
    (void)keyA; (void)keyB; (void)aIsBot; (void)bIsBot;
    return 0;
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

#include <stdint.h>
#include <stddef.h>

void logViewerRunFromMemory(void *window, void *renderer, uint8_t *zipData, size_t zipLen, bool fromMainMenu) {
    (void)window; (void)renderer; (void)zipData; (void)zipLen; (void)fromMainMenu;
}

/* ---- WBN browser stub (requires libcurl/cJSON, not built on iOS) ---- */

#include "../sdl3/dialogs/imgui_wbn_browser.h"

WbnBrowserResult imguiWbnBrowserShow(struct SDL_Window *window,
                                     struct SDL_Renderer *renderer) {
    (void)window; (void)renderer;
    WbnBrowserResult r = {0};
    r.action = WBN_BROWSER_CLOSE;
    return r;
}

/* ---- server lifecycle stubs (dedicated-server-only on iOS) ---- */

#include "../../server/server_lifecycle.h"

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

/* ---- winbolonet map change stub ---- */

void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
    (void)mapName; (void)numBases; (void)numPills; (void)freeBases; (void)freePills;
}

void winbolonetSetLobbyInfo(const WbnLobbyInfo *info) { (void)info; }

void winbolonetSendLobbyUpdate(void) { }

/* ---- skins stubs (requires minizip/zlib) ---- */

bool skinsLoadSkin(char *fileName) { (void)fileName; return false; }
bool skinsIsLoaded(void) { return false; }
void skinsGetSkinDirectory(char *value) { value[0] = '\0'; }
void skinsGetFileName(char *value) { value[0] = '\0'; }

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
