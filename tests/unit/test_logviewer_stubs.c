/*
 * Link-only stubs for the headless log-viewer reader test
 * (test_wbv_reader.c). lv_screenLoadMapFromMemory and the decode TUs the
 * unit-test binary links (screen.c, blocks.c, players.c, ...) reference a
 * handful of host / render / window / message entry points whose real
 * bodies live in the GUI TUs (logviewer.c, draw.c, messages.c) that the
 * binary deliberately does not link. These no-op definitions satisfy the
 * linker so the decode path runs without a window, renderer or audio
 * device. None carries behaviour the reader gate depends on.
 */
#include <stdint.h>

/* Only the logviewer-side headers, which resolve unambiguously via the
 * src/logviewer include dir. We deliberately do NOT include draw.h or
 * lv_messages.h: this TU lives in tests/unit, so quoted includes fall
 * through -I order, where src/gui/sdl3/draw.h would shadow the logviewer
 * draw.h. The draw/message stubs below carry their own prototypes. */
#include "lv_global.h"
#include "backend.h"

/* Defined in logviewer.c / draw.c / messages.c but with no shared header
 * reachable here — declare them so the no-op definitions match the
 * production signatures. */
void lv_frontEndDrawMainScreen(screen *value, screenMines *mineView,
                               screenTanks *tks, screenGunsight *gs,
                               screenBullets *sBullet, screenLgm *lgms,
                               int32_t srtDelay, bool isPillView,
                               int edgeX, int edgeY);
void lv_frontEndPlaySound(sndEffects value);
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor,
                                   BYTE versionMinor, BYTE versionRevision,
                                   char *mapName, BYTE gameType,
                                   bool hiddenMines, BYTE aiType,
                                   int32_t startDelay, int32_t timeLimit,
                                   BYTE *wbnKey, int32_t startTime);
void lv_startOfLog(void);
void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x,
                   BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank);
void lv_windowRemoveEvents(void);
void lv_windowRemoveEventsAfter(uint32_t timeMs);
void lv_drawResizeRenderTarget(void);
void lv_messageCreate(void);
void lv_messageDestroy(void);
void lv_messageAddItem(char *top, char *bottom);
void lv_messageDrainQueue(void);

void lv_frontEndDrawMainScreen(screen *value, screenMines *mineView,
                               screenTanks *tks, screenGunsight *gs,
                               screenBullets *sBullet, screenLgm *lgms,
                               int32_t srtDelay, bool isPillView,
                               int edgeX, int edgeY) {
  (void)value; (void)mineView; (void)tks; (void)gs; (void)sBullet;
  (void)lgms; (void)srtDelay; (void)isPillView; (void)edgeX; (void)edgeY;
}

void lv_frontEndPlaySound(sndEffects value) { (void)value; }

void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor,
                                   BYTE versionMinor, BYTE versionRevision,
                                   char *mapName, BYTE gameType,
                                   bool hiddenMines, BYTE aiType,
                                   int32_t startDelay, int32_t timeLimit,
                                   BYTE *wbnKey, int32_t startTime) {
  (void)clear; (void)versionMajor; (void)versionMinor; (void)versionRevision;
  (void)mapName; (void)gameType; (void)hiddenMines; (void)aiType;
  (void)startDelay; (void)timeLimit; (void)wbnKey; (void)startTime;
}

void lv_startOfLog(void) {}

void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x,
                   BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank) {
  (void)itemType; (void)itemNumber; (void)owner; (void)x; (void)y;
  (void)armour; (void)shells; (void)mines; (void)inTank;
}

void lv_windowRemoveEvents(void) {}

void lv_windowRemoveEventsAfter(uint32_t timeMs) { (void)timeMs; }

/* Declared in backend.h (already included). */
void lv_finished(void) {}
void lv_windowAddEvent(int eventType, char *msg) { (void)eventType; (void)msg; }
void lv_windowAddHighlight(char *msg, uint32_t seekMs, int mapX, int mapY) {
  (void)msg; (void)seekMs; (void)mapX; (void)mapY;
}
void lv_windowStop(int corruptLog) { (void)corruptLog; }

/* Defined in draw.c. */
void lv_drawResizeRenderTarget(void) {}

/* Defined in messages.c. */
void lv_messageCreate(void) {}
void lv_messageDestroy(void) {}
void lv_messageAddItem(char *top, char *bottom) { (void)top; (void)bottom; }
void lv_messageDrainQueue(void) {}
