/*
 * ios_stubs.c — Stub implementations for modules not included
 * in the iOS target. Covers: winbolonet, geolookup, cursor,
 * and posix_stubs (preferences I/O).
 *
 * Most GUI modules (sdl3imgui, flags, input, dialogs, dns_lookups)
 * are now compiled directly from the shared sdl3 sources.
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#include "../../bolo/global.h"
#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"

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
bool cursorPos(RECT *rcWindow, BYTE *xValue, BYTE *yValue) {
    (void)rcWindow; (void)xValue; (void)yValue;
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

bool winbolonetCreateClient(const char *token, const char *serverKey, char *errorMsg) {
    (void)token; (void)serverKey; (void)errorMsg;
    return false;
}

void winbolonetDestroy(bool isServer) { (void)isServer; }

void winboloNetGetServerKey(char *keyBuff) { if (keyBuff) keyBuff[0] = '\0'; }
void winboloNetGetMyClientKey(char *keyBuff) { if (keyBuff) keyBuff[0] = '\0'; }
bool winboloNetVerifyClientKey(const char *playerKey, char *userName, BYTE playerNum) {
    (void)playerKey; (void)userName; (void)playerNum;
    return false;
}
void winboloNetClientLeaveGame(BYTE playerNum, BYTE numPlayers, BYTE freeBases, BYTE freePills) {
    (void)playerNum; (void)numPlayers; (void)freeBases; (void)freePills;
}
void winbolonetGoodbye(void) {}
void winbolonetServerSendTeams(BYTE *array, BYTE length, BYTE numTeams) {
    (void)array; (void)length; (void)numTeams;
}
void winbolonetAddEvent(BYTE eventType, bool isServer, BYTE playerA, BYTE playerB) {
    (void)eventType; (void)isServer; (void)playerA; (void)playerB;
}
void winbolonetServerUpdate(BYTE numPlayers, BYTE numFreeBases, BYTE numFreePills, bool sendNow) {
    (void)numPlayers; (void)numFreeBases; (void)numFreePills; (void)sendNow;
}
bool winbolonetIsRunning(void) { return false; }
bool winboloNetIsPlayerParticipant(BYTE playerNum) { (void)playerNum; return false; }
void winboloNetSendLock(bool isLocked) { (void)isLocked; }
bool winbolonetServerVerifyToken(const char *token, BYTE playerNum, char *errorMsg, bool *hasSteam) {
    (void)token; (void)playerNum; (void)errorMsg;
    if (hasSteam) *hasSteam = false;
    return false;
}
bool winbolonetAuthLogin(const char *username, const char *password, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg) {
    (void)username; (void)password; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)errorMsg;
    return false;
}
bool winbolonetAuthSteam(const char *steamTicketHex, char *tokenOut, char *expiryOut, char *playerNameOut, char *errorMsg) {
    (void)steamTicketHex; (void)tokenOut; (void)expiryOut; (void)playerNameOut; (void)errorMsg;
    return false;
}
bool winbolonetAuthValidate(const char *token, char *playerNameOut, char *errorMsg) {
    (void)token; (void)playerNameOut; (void)errorMsg;
    return false;
}

/* ---- winbolonet thread stubs ---- */

bool winbolonetThreadCreate(void) { return true; }
void winbolonetThreadDestroy(void) {}
void winbolonetThreadAddRequest(const char *ep, const char *jb) { (void)ep; (void)jb; }

/* ---- winbolonet events stubs ---- */

void winbolonetEventsCreate(void) {}
void winbolonetEventsDestroy(void) {}
void winbolonetEventsAddItem(BYTE itemType, const char *keyA, const char *keyB) {
    (void)itemType; (void)keyA; (void)keyB;
}
int winbolonetEventsGetSize(void) { return 0; }
BYTE winbolonetEventsRemove(char *keyA, char *keyB) {
    (void)keyA; (void)keyB;
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

/* ---- posix_stubs (INI file preferences — not used on iOS) ---- */

void preferencesGetPreferenceFile(char *dest) {
    strcpy(dest, "WinBolo.ini");
}

unsigned long GetPrivateProfileString(const char *section, const char *key,
                                       const char *def, char *dest,
                                       unsigned long size, const char *file) {
    (void)section; (void)key; (void)file;
    if (def && dest && size > 0) {
        strncpy(dest, def, size - 1);
        dest[size - 1] = '\0';
        return (unsigned long)strlen(dest);
    }
    if (dest && size > 0) dest[0] = '\0';
    return 0;
}

int WritePrivateProfileString(const char *section, const char *key,
                               const char *value, const char *file) {
    (void)section; (void)key; (void)value; (void)file;
    return 1;
}

/* ---- map editor stubs (not available on iOS) ---- */

void mapEditorRun(void *window, void *renderer, const char *mapPath, bool fromMainMenu) {
    (void)window; (void)renderer; (void)mapPath; (void)fromMainMenu;
}

/* ---- log viewer stubs (not available on iOS) ---- */

void logViewerRun(void *window, void *renderer, const char *logPath, bool fromMainMenu) {
    (void)window; (void)renderer; (void)logPath; (void)fromMainMenu;
}

/* ---- winbolonet map change stub ---- */

void winbolonetSendMapChange(char *mapName, BYTE numBases, BYTE numPills, BYTE freeBases, BYTE freePills) {
    (void)mapName; (void)numBases; (void)numPills; (void)freeBases; (void)freePills;
}

/* ---- skins stubs (requires minizip/zlib) ---- */

bool skinsLoadSkin(char *fileName) { (void)fileName; return false; }
bool skinsIsLoaded(void) { return false; }
void skinsGetSkinDirectory(char *value) { value[0] = '\0'; }
void skinsGetFileName(char *value) { value[0] = '\0'; }

/* ---- log stubs (log.c requires minizip/zlib) ---- */

#include "../../bolo/log.h"
#include "../../bolo/bolo_map.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/bases.h"
#include "../../bolo/starts.h"
#include "../../bolo/players.h"

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
bool logStart(char *fn, ServerSim *ssim, map *mp, bases *bs, pillboxes *pb, starts *ss, players *plrs, BYTE ai, BYTE maxPlayers, bool usePassword) {
    (void)fn; (void)ssim; (void)mp; (void)bs; (void)pb; (void)ss; (void)plrs; (void)ai; (void)maxPlayers; (void)usePassword;
    return false;
}
bool logWriteSnapshot(ServerSim *ssim, map *mp, pillboxes *pb, bases *bs, starts *ss, players *plrs, bool check) {
    (void)ssim; (void)mp; (void)pb; (void)bs; (void)ss; (void)plrs; (void)check;
    return false;
}
bool logCheckTankSame(BYTE playerNum, BYTE mx, BYTE my, BYTE pxy, BYTE opt) {
    (void)playerNum; (void)mx; (void)my; (void)pxy; (void)opt;
    return true;
}
