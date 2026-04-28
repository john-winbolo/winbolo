/*
 * Copyright (c) 1998-2008 John Morrison.
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

#include "global.h"
#include "backend.h"
#include "snapshot.h"
#include "players.h"
#include "shells.h"
#include "bolo_map.h"
#include "bases.h"
#include "starts.h"

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
  bool         useTeamColours;
  bool         wantScreenUpdate;
  bool         doubleSpeed;
  BYTE         speed;
  int          timerSleep;
  SDL_TimerID  timerGameID;
  SDL_TimerID  timerFrameID;

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

/* State accessors used by screen.c and other modules */
void lv_screenSetState(LogViewerState *lv);
LogViewerState *lv_screenGetState(void);

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE lv_screenGetXOffset(void);
BYTE lv_screenGetYOffset(void);
bool lv_screenGetFastForwarding(void);

#endif /* _LOGVIEWER_H */
