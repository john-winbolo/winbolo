/*
 * Copyright (c) 1998-2026 John Morrison.
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
*Name:          LogViewer
*Filename:      logviewer.h
*Author:        John Morrison
*Purpose:
*  Central state struct for the log viewer. Holds all state
*  previously spread across module-level globals, enabling
*  the logviewer to be embedded in the main app (like the
*  map editor pattern).
*********************************************************/

#ifndef _LOGVIEWER_H
#define _LOGVIEWER_H

#include "lv_global.h"
#include "backend.h"
#include "snapshot.h"
#include "lv_players.h"
#include "lv_shells.h"
#include "lv_bolo_map.h"
#include "lv_bases.h"
#include "lv_starts.h"

/* Forward declarations for SDL types */
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
typedef uint32_t SDL_TimerID;

/* Log reader states (mirrors enum in screen.c) */
typedef enum {
  lv_lr_start,
  lv_lr_longwait,
  lv_lr_shortwait
} lvLrStates;

/* Per-player HUD state for game-view armour bar tracking. Logs do not
 * carry tank armour, so the bar is binary (alive=full / dead=destroyed)
 * and the timestamps drive any death/respawn animations. */
typedef struct {
  bool     alive;
  uint32_t deathTimeMs;
  uint32_t respawnTimeMs;
} GameViewPlayerHud;

/* Per-tank inventory inferred from the event stream for legacy logs that
 * do not carry tank shells/mines/armour/trees. Reset at spawn/respawn
 * from gameTypeGetItems; mutated on log_SoundShoot/MineLay/HitTank/
 * MineExplode by tile correlation with the shooter/victim's last
 * log_PlayerLocation; refilled on log_BaseSetStock deltas when a
 * friendly tank is standing on the base tile. Approximate — sound
 * events lack player IDs, so adjacent tanks may steal each other's
 * deltas. New (post-snapshot-tank-stats) logs should override these
 * from authoritative snapshots. */
typedef struct {
  BYTE shells;
  BYTE mines;
  BYTE armour;
  BYTE trees;
} TankInventory;

/*********************************************************
 * LogViewerState — all viewer state in one struct.
 *
 * Grouped by the file that originally owned the globals.
 *********************************************************/
typedef struct LogViewerState {

  /* --- FROM screen.c globals --- */
  screen       view;
  screenMines  mineView;
  map          mp;
  bases        bs;
  pillboxes    pb;
  starts       ss;
  shells       shs;
  char         mapName[64];
  BYTE         gt;
  BYTE         allowHiddenMines;
  BYTE         ai;
  BYTE         maxPlayers;
  bool         usePassword;
  BYTE         wbnKey[32];
  char         logFileName[FILENAME_MAX];
  snapshot     snap;
  BYTE         xOffset;
  BYTE         yOffset;
  int          subPxX;     /* sub-tile pan offset within (xOffset,yOffset) tile,
                            * in zoom-1 native pixels. Range: [0, TILE_SIZE_X). */
  int          subPxY;
  BYTE         screenSizeX;
  BYTE         screenSizeY;
  bool         isPlaying;
  bool         logLoaded;
  uint32_t     timeRunning;
  uint32_t     totalTimeMs;     /* Total log duration in ms, computed once on load */
  bool         centredTank;
  bool         fastForwarding;
  int32_t      gmeStartDelay;
  int32_t      gmeLength;
  int32_t      gmeCreateTime;
  char         serverIP[256];
  unsigned short serverPort;
  BYTE         versionMajor;
  BYTE         versionMinor;
  BYTE         versionRevision;
  BYTE         loadedLogVersion;
  BYTE         selectedItem;
  BYTE         selectedItemType;
  int          state;       /* lvLrStates enum value */
  unsigned short waitLen;

  /* --- FROM main.c globals --- */
  BYTE         tc[17];
  bool         playIsPlaying;
  bool         isLoaded;
  bool         isSoundsPlaying;
  int          soundVolume;     /* 0-100; passed to lv_soundSetVolume() */
  bool         useTeamColours;
  bool         wantScreenUpdate;
  bool         doubleSpeed;
  BYTE         speed;
  int          timerSleep;
  SDL_TimerID  timerGameID;
  SDL_TimerID  timerFrameID;

  /* --- Game-view (trailer-capture) skin state --- */
  bool         gameView;             /* gates the rendering branch */
  BYTE         cameraSlot;           /* 0..MAX_TANKS-1, the spectated player */
  bool         savedUseTeamColours;  /* restored when leaving game view mode */
  uint16_t     kills[MAX_TANKS];     /* per-player kill tally */
  uint16_t     deaths[MAX_TANKS];    /* per-player death tally */
  GameViewPlayerHud gameViewHud[MAX_TANKS];
  TankInventory tankInv[MAX_TANKS];  /* inferred per-tank inventory (legacy logs) */
  /* Per-base previous-stock snapshot for log_BaseSetStock delta
   * computation. prevBaseStockValid is set FALSE until the first
   * log_BaseSetStock for a base, so the initial value isn't credited
   * to whichever tank happens to be standing on that tile. Indexed by
   * base number minus 1 (1..MAX_BASES → 0..MAX_BASES-1). */
  BYTE         prevBaseShells[MAX_BASES];
  BYTE         prevBaseMines[MAX_BASES];
  BYTE         prevBaseArmour[MAX_BASES];
  bool         prevBaseStockValid[MAX_BASES];

  /* --- FROM draw.c (SDL handles) --- */
  struct SDL_Window   *window;
  struct SDL_Renderer *renderer;
  bool         ownsWindow;     /* TRUE in standalone, FALSE when embedded */
  bool         fromMainMenu;   /* TRUE when launched from main app menu */
  bool         quit;           /* Set TRUE to exit the event loop */

  /* --- FROM players.c globals (will be moved here in a later step) --- */
  /* players plrs; */
  /* BYTE myPlayerNum; */

  /* --- FROM blocks.c globals (will be moved here in a later step) --- */
  /* log buffer state: logData, logSize, logCapacity, logPosition, logEOF, blockKey, logFile */

  /* --- FROM clientmutex.c (will be moved here in a later step) --- */
  /* SDL_Mutex *mutex; */

} LogViewerState;

/* Entry point for embedded logviewer */
void logViewerRun(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu);

/* Entry point for loading from an in-memory zip buffer (mobile).
 * Takes ownership of zipData. */
void logViewerRunFromMemory(struct SDL_Window *window, struct SDL_Renderer *renderer,
                            uint8_t *zipData, size_t zipLen, bool fromMainMenu);

/* Host-callable decoder lifecycle: create allocates the state, sets its
 * field defaults, and registers it as the active state; destroy closes any
 * loaded log, frees the state, and clears the active state. */
LogViewerState *lv_decoderCreate(bool fromMainMenu);
void lv_decoderDestroy(LogViewerState *lv);

/* State accessors used by screen.c and other modules */
void lv_screenSetState(LogViewerState *lv);
LogViewerState *lv_screenGetState(void);

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE lv_screenGetXOffset(void);
BYTE lv_screenGetYOffset(void);
bool lv_screenGetFastForwarding(void);

#endif /* _LOGVIEWER_H */
