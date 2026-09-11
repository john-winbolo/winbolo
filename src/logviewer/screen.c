/*
 * $Id$
 *
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
*Name:          Screen
*Filename:      screen.c
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified:  4/11/98
*Purpose:
*  Provides Interfaces with the front end
*********************************************************/

/* Includes */
#include <stdio.h>
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#endif
#include "lv_global.h"
#include "backend.h"
#include "lv_bolo_map.h"
#include "tiles.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_screencalc.h"
#include "lv_screentank.h"
#include "lv_screenlgm.h"
#include "lv_screenbullet.h"
#include "lv_starts.h"
#include "lv_util.h"
#include "lv_shells.h"
#include "lv_sounddist.h"
#include "lv_players.h"
#include "snapshot.h"
#include "blocks.h"
#include "dns.h"
#include "logviewer.h"
#include "lv_messages.h"
#include "../gui/lang.h"
#include "../gui/ping_kinds.h"   /* PING_DISPLAY_MS, pingKindMessageId */

/* File-scope pointer to the central LogViewerState */
static LogViewerState *g_lv = NULL;

void lv_screenSetState(LogViewerState *lv) { g_lv = lv; }
LogViewerState *lv_screenGetState(void) { return g_lv; }

/* The lobby settings the recording carries, as the raw log_GameSettings
 * payload (layout in docs/replay-format.md). Length 0 means no settings,
 * which is every log written before the event existed. Held as raw bytes
 * because the payload is append-only: the reader that draws it takes the
 * fields it knows and ignores anything past them. */
static BYTE s_gameSettings[LV_GAME_SETTINGS_MAX];
static int  s_gameSettingsLen = 0;
/* Set once the load-time walk has read the whole file. What it leaves in the
 * store is the last settings event the file holds, which is the settings the
 * round was played under — no such event is written after the round starts.
 * A live feed gets no walk, so this stays FALSE there. */
static bool s_gameSettingsWalked = FALSE;

/* Keep payload as the current settings. A zero length clears the store. */
static void lv_screenStoreGameSettings(const BYTE *payload, int len) {
  if (payload == NULL || len <= 0) {
    s_gameSettingsLen = 0;
    return;
  }
  if (len > LV_GAME_SETTINGS_MAX) {
    len = LV_GAME_SETTINGS_MAX;
  }
  memcpy(s_gameSettings, payload, (size_t)len);
  s_gameSettingsLen = len;
}

/* A settings event the decoder passed. On a live feed this is the only way
 * the settings arrive. On a loaded file the walk has already read the whole
 * stream, so an event the playhead crosses is an older one it has seen and
 * discarded — replaying it would put the lobby's opening settings back in
 * place of the round's. */
static void lv_screenPlaybackGameSettings(const BYTE *payload, int len) {
  if (s_gameSettingsWalked) {
    return;
  }
  lv_screenStoreGameSettings(payload, len);
}

int lv_screenGetGameSettings(BYTE *out, int maxLen) {
  int len = s_gameSettingsLen;

  if (out == NULL || len <= 0) {
    return 0;
  }
  if (len > maxLen) {
    len = maxLen;
  }
  memcpy(out, s_gameSettings, (size_t)len);
  return len;
}

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE lv_screenGetXOffset(void) { return g_lv->xOffset; }
BYTE lv_screenGetYOffset(void) { return g_lv->yOffset; }
bool lv_screenGetFastForwarding(void) { return g_lv->fastForwarding; }
uint32_t lv_screenGetTimeRunning(void) { return g_lv->timeRunning; }

/* --- Smart pings ----------------------------------------------------
 * Every log_Ping the playback has walked past that is still inside its
 * PING_DISPLAY_MS. Aged on g_lv->timeRunning, the playback clock, so a ping
 * lasts the same five seconds of game time however fast the round is being
 * played back — and a rewind, which winds that clock backwards, leaves every
 * stored ping in the future and so drops them all with no explicit reset.
 *
 * The viewer shows every team's pings: a replay is watched from outside, so
 * there is no team to filter to. */
#define LV_MAX_PINGS 16

typedef struct {
  BYTE     sender;
  BYTE     kind;
  uint16_t worldX;
  uint16_t worldY;
  uint32_t timeMs;   /* playback clock at the ping, +1 so 0 means "empty" */
} lvPing;

static lvPing s_pings[LV_MAX_PINGS];
static int    s_pingWrite = 0;

/* Drop every stored ping. A new log restarts the playback clock at zero, so
   the previous log's pings all read as "in the future" and are hidden — until
   playback runs past the time they were stored at, when they would come back
   over a replay they were never part of. Called from lv_screenSetup, which
   every load path runs through. */
static void lv_pingReset(void) {
  memset(s_pings, 0, sizeof(s_pings));
  s_pingWrite = 0;
}

static void lv_pingAdd(BYTE sender, BYTE kind, uint16_t wx, uint16_t wy) {
  s_pings[s_pingWrite].sender = sender;
  s_pings[s_pingWrite].kind   = kind;
  s_pings[s_pingWrite].worldX = wx;
  s_pings[s_pingWrite].worldY = wy;
  s_pings[s_pingWrite].timeMs = g_lv->timeRunning + 1;
  s_pingWrite = (s_pingWrite + 1) % LV_MAX_PINGS;
}

/* Read out one live ping. `index` walks 0..LV_MAX_PINGS-1 from the oldest
 * slot, so drawing in order puts the newest on top; a slot that is empty,
 * expired or (after a rewind) still in the future returns false and the
 * caller moves on. ageMs is measured on the playback clock. */
bool lv_screenGetPing(int index, BYTE *kind, uint16_t *worldX,
                      uint16_t *worldY, uint32_t *ageMs) {
  const lvPing *p;
  uint32_t at;
  if (index < 0 || index >= LV_MAX_PINGS) return FALSE;
  p = &s_pings[(s_pingWrite + index) % LV_MAX_PINGS];
  if (p->timeMs == 0) return FALSE;
  at = p->timeMs - 1;
  if (g_lv->timeRunning < at) return FALSE;
  if (g_lv->timeRunning - at >= (uint32_t)PING_DISPLAY_MS) return FALSE;
  if (kind)   *kind   = p->kind;
  if (worldX) *worldX = p->worldX;
  if (worldY) *worldY = p->worldY;
  if (ageMs)  *ageMs  = g_lv->timeRunning - at;
  return TRUE;
}

int lv_screenGetPingCapacity(void) { return LV_MAX_PINGS; }

/* Store a slot's tank stocks. The recording is the only source: the four
 * values arrive either on the end of a snapshot's player block or in a
 * log_TankSetStock record. All zeros is what a slot gets when the recording
 * carries neither — a file written before the stocks were recorded, or a slot
 * not in use — and the status panel then draws empty bars rather than a
 * guessed number. */
static void lv_screenSetTankStock(BYTE slot, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
  if (slot >= MAX_TANKS) return;
  g_lv->tankInv[slot].shells = shells;
  g_lv->tankInv[slot].mines  = mines;
  g_lv->tankInv[slot].armour = armour;
  g_lv->tankInv[slot].trees  = trees;
}

/* Store a slot's modifier set. A log_TankSetModifiers record replaces the whole
 * set, as the op that wrote it did. Nothing draws these yet. */
static void lv_screenSetTankModifiers(BYTE slot, const BYTE *mods) {
  if (slot >= MAX_TANKS) return;
  g_lv->tankMods[slot].speed  = mods[0];
  g_lv->tankMods[slot].accel  = mods[1];
  g_lv->tankMods[slot].turn   = mods[2];
  g_lv->tankMods[slot].reload = mods[3];
  g_lv->tankMods[slot].dealt  = mods[4];
  g_lv->tankMods[slot].taken  = mods[5];
}

// Some prototypes to cleanup and document

bool logIsEOF();

int logReadBytes(BYTE *buff, int len);

void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank);

bool lv_processSnapshot();
bool lv_logLoad(char *fileName, int memoryBufferSize);
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor, BYTE versionRevision, char *mapName, BYTE gameType, bool hiddenMines, BYTE aiType, int32_t startDelay, int32_t timeLimit, BYTE *wbnKey, int32_t startTime);
void lv_startOfLog();
void lv_windowRemoveEvents();
void lv_windowRemoveEventsAfter(uint32_t timeMs);

/*********************************************************
*NAME:          lv_screenCalcSquare
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED:  1/11/99
*PURPOSE:
*  Calculates the terrain type for a given location
*
*ARGUMENTS:
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate
*********************************************************/
BYTE lv_screenCalcSquare(BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY) {
  baseAlliance ba;  /* The allience of a base */
  BYTE returnValue; /* Value to return */
  BYTE currentPos;
  BYTE aboveLeft;
  BYTE above;
  BYTE aboveRight;
  BYTE leftPos;
  BYTE rightPos;
  BYTE belowLeft;
  BYTE below;
  BYTE belowRight;

/* Stride is sizeX+1 (not sizeX) because the screen buffer carries a
 * one-tile margin column/row beyond the visible viewport, used by the
 * sub-tile-scrolling blit to avoid the trailing-edge bleed. */
int a = (scrY*(lv_screenGetSizeX()+1))+scrX ;

  if (a > 1989) {
    above = 1;
  }
  *((*g_lv->mineView).mineItem+a) = FALSE;
  /* Set up Items */
  if ((lv_pillsExistPos(&g_lv->pb,xValue,yValue)) == TRUE) {
    returnValue = lv_pillsGetScreenHealth(&g_lv->pb, xValue, yValue);
  } else if ((lv_basesExistPos(&g_lv->bs,xValue,yValue)) == TRUE) {
     ba = lv_basesGetAlliancePos(&g_lv->bs, xValue, yValue);
    switch (ba) {
    case baseOwnGood:
      returnValue = BASE_GOOD;
      break;
    case baseAllieGood:
      returnValue = BASE_GOOD;
      break;
    case baseNeutral:
      returnValue = BASE_NEUTRAL;
      break;
    case baseDead:
      if (lv_basesAmOwner(&g_lv->bs, lv_playersGetSelf(), xValue, yValue) == TRUE) {
        returnValue = BASE_GOOD;
      } else {
        returnValue = BASE_EVIL;
      }
      break;
    case baseEvil:
    default:
      /* Base Evil */
      returnValue = BASE_EVIL;
    }
  }  else {
    currentPos = lv_mapGetPos(&g_lv->mp,xValue,yValue);
    if (lv_mapIsMine(&g_lv->mp, xValue, yValue) == TRUE) {
      *((*g_lv->mineView).mineItem+a) = TRUE;
      if (currentPos != DEEP_SEA) {
        currentPos = currentPos - MINE_SUBTRACT;
      }
    } else {
      *((*g_lv->mineView).mineItem+a) = FALSE;
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue-1), (BYTE) (yValue-1)) == TRUE) {
      aboveLeft = ROAD;
    } else {
      aboveLeft = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue-1),(BYTE) (yValue-1));
      if (aboveLeft >= MINE_START && aboveLeft <= MINE_END) {
        aboveLeft = aboveLeft - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, xValue, (BYTE) (yValue-1)) == TRUE) {
      above = ROAD;
    } else {
      above = lv_mapGetPos(&g_lv->mp,xValue,(BYTE) (yValue-1));
      if (above >= MINE_START && above <= MINE_END) {
        above = above - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue+1), (BYTE) (yValue-1)) == TRUE) {
      aboveRight = ROAD;
    } else {
      aboveRight = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue+1),(BYTE) (yValue-1));
      if (aboveRight >= MINE_START && aboveRight <= MINE_END) {
        aboveRight = aboveRight - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue-1), yValue) == TRUE) {
      leftPos = ROAD;
    } else {
      leftPos = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue-1),yValue);
      if (leftPos >= MINE_START && leftPos <= MINE_END) {
        leftPos = leftPos - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue+1), yValue) == TRUE) {
      rightPos = ROAD;
    } else {
      rightPos = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue+1),yValue);
      if (rightPos >= MINE_START && rightPos <= MINE_END) {
        rightPos = rightPos - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue-1), (BYTE) (yValue+1)) == TRUE) {
      belowLeft = ROAD;
    } else {
      belowLeft = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue-1),(BYTE) (yValue+1));
      if (belowLeft >= MINE_START && belowLeft <= MINE_END) {
        belowLeft = belowLeft - MINE_SUBTRACT;
      }
    }


    if (lv_basesExistPos(&g_lv->bs, xValue, (BYTE) (yValue+1)) == TRUE) {
      below = ROAD;
    } else {
      below = lv_mapGetPos(&g_lv->mp,xValue,(BYTE) (yValue+1));
      if (below >= MINE_START && below <= MINE_END) {
        below = below - MINE_SUBTRACT;
      }
    }

    if (lv_basesExistPos(&g_lv->bs, (BYTE) (xValue+1), (BYTE) (yValue+1)) == TRUE) {
      belowRight = ROAD;
    } else {
      belowRight = lv_mapGetPos(&g_lv->mp,(BYTE) (xValue+1),(BYTE) (yValue+1));
      if (belowRight >= MINE_START && belowRight <= MINE_END) {
        belowRight = belowRight - MINE_SUBTRACT;
      }
    }

    switch (currentPos) {
    case ROAD:
      returnValue = lv_screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BUILDING:
      returnValue = lv_screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case FOREST:
      returnValue = lv_screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case RIVER:
      returnValue = lv_screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case DEEP_SEA:
      returnValue = lv_screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BOAT:
      returnValue = lv_screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    default:
      returnValue = currentPos;
      break;
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenUpdateView
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Updates the values in the view area
*
*ARGUMENTS:
* value - The update type (Helps in optimisations)
*********************************************************/
void lv_screenUpdateView(updateType value) {
  /* int (not BYTE): the loops run to ssx/ssy inclusive, which clamp to 255 at
   * a fullscreen, zoomed-out viewport. A BYTE counter would wrap 255->0 at
   * count++ and keep satisfying count <= 255, looping forever (hard lockup). */
  int count;    /* Looping Variables */
  int count2;
  int ssx = lv_screenGetSizeX();
  int ssy = lv_screenGetSizeY();

  if (g_lv->logLoaded == FALSE) {
    return;
  }

  if (g_lv->centredTank == FALSE || value == redraw) {
    if (value == left) {
      g_lv->xOffset--;
    } else if (value == right) {
      g_lv->xOffset++;
    } else if (value == up) {
      g_lv->yOffset--;
    } else if (value == down) {
      g_lv->yOffset++;
    }
  }

  /* Iterate sizeX+1 by sizeY+1 to populate one extra column and row
   * beyond the visible viewport. The margin tile is shown when sub-tile
   * scrolling shifts the final blit, eliminating the trailing-edge
   * bleed. The margin coords reach 255 at the map boundary, which is
   * still in-range for lv_mapGetPos; adjacency reads inside
   * lv_screenCalcSquare wrap (BYTE +1 of 255 -> 0) but only when
   * subPx == 0, in which case the blit's srcRect clips the margin
   * tile from view, so the wrong adjacency is never user-visible. */
  for (count=0;count <= ssx; count++) {
    for (count2=0;count2 <= ssy; count2++) {
      *((*g_lv->view).screenItem+((ssx+1)*count2)+count) = lv_screenCalcSquare((BYTE) (count+g_lv->xOffset),(BYTE) (count2+g_lv->yOffset), count, count2);
    }
  }
}



/*********************************************************
*NAME:          lv_screenSetup
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets up all the variables - Should be run when the
*  program starts.
*
*ARGUMENTS:
*
*********************************************************/
void lv_screenSetup() {
  int a = 0;
  /* Every load path runs through here, so drop the previous log's recorded
     settings before the new one's walk can collect its own. */
  lv_screenStoreGameSettings(NULL, 0);
  s_gameSettingsWalked = FALSE;
  lv_pingReset();
  g_lv->gmeStartDelay = 0;
  g_lv->gmeLength = UNLIMITED_GAME_TIME;
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;
  g_lv->xOffset = 127;
  g_lv->yOffset = 127;
  g_lv->subPxX  = 0;
  g_lv->subPxY  = 0;
  lv_mapCreate(&g_lv->mp);
  lv_pillsCreate(&g_lv->pb);
  lv_startsCreate(&g_lv->ss);
  lv_basesCreate(&g_lv->bs);
  lv_playersCreate();
  lv_playersSetSelf(0);
  g_lv->shs = lv_shellsCreate();
  if (g_lv->view != NULL) {
    free((*g_lv->view).screenItem);
    Dispose(g_lv->view);
  }
  New(g_lv->view);
  if (g_lv->view != NULL) {
    (*g_lv->view).screenItem = malloc((lv_screenGetSizeX()+2) * (lv_screenGetSizeY()+2));
  }
  if (g_lv->mineView != NULL) {
    free((*g_lv->mineView).mineItem);
    free(g_lv->mineView);
  }
  a = (lv_screenGetSizeX()+1) * (lv_screenGetSizeY()+1);
  New(g_lv->mineView);
  if (g_lv->mineView != NULL) {
    (*g_lv->mineView).mineItem = malloc(a * sizeof(bool));
  }

  lv_screenUpdateView(redraw);
}

/*********************************************************
*NAME:          lv_screenDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the structures Should be called on
*  program exit
*
*ARGUMENTS:
*
*********************************************************/
void lv_screenDestroy() {
  if (g_lv->mp != NULL) {
    lv_mapDestroy(&g_lv->mp);
    g_lv->mp = NULL;
  }
  if (g_lv->pb != NULL) {
    lv_pillsDestroy(&g_lv->pb);
    g_lv->pb = NULL;
  }
  if (g_lv->ss != NULL) {
    lv_startsDestroy(&g_lv->ss);
    g_lv->ss = NULL;
  }
  if (g_lv->bs != NULL) {
    lv_basesDestroy(&g_lv->bs);
    g_lv->bs = NULL;
  }
  lv_playersDestroy();
  if (g_lv->shs != NULL) {
    lv_shellsDestroy(&g_lv->shs);
    g_lv->shs = NULL;
  }
  if (g_lv->view != NULL) {
    free((*g_lv->view).screenItem);
    Dispose(g_lv->view);
    g_lv->view = NULL;
  }
  if (g_lv->mineView != NULL) {
    free((*g_lv->mineView).mineItem);
    Dispose(g_lv->mineView);
    g_lv->mineView = NULL;
  }
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;
}

void lv_frontEndDrawMainScreen(screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms, int32_t srtDelay, bool isPillView, int edgeX, int edgeY);


/*********************************************************
*NAME:          lv_screenUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Updates the screen. Takes numerous directions
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void lv_screenUpdate(updateType value) {
  screenTanks st;
  screenBullets sb;
  screenLgm sl;


  lv_screenLgmCreate(&sl);
  sb = lv_screenBulletsCreate();
  lv_screenTanksCreate(&st);

  if (g_lv->logLoaded == FALSE) {
    return;
  }

  lv_screenUpdateView(value);
  {
    int rightEdge = (int)g_lv->xOffset + lv_screenGetSizeX();
    int bottomEdge = (int)g_lv->yOffset + lv_screenGetSizeY();
    if (rightEdge > 255) rightEdge = 255;
    if (bottomEdge > 255) bottomEdge = 255;
    lv_playersMakeScreenLgm(&sl, g_lv->xOffset, (BYTE) rightEdge, g_lv->yOffset, (BYTE) bottomEdge);
    lv_shellsCalcScreenBullets(&g_lv->shs, &sb, g_lv->xOffset, (BYTE) rightEdge, g_lv->yOffset, (BYTE) bottomEdge);
    lv_playersMakeScreenTanks(&st, g_lv->xOffset, (BYTE) rightEdge, g_lv->yOffset, (BYTE) bottomEdge);
  }
  lv_frontEndDrawMainScreen(&g_lv->view, &g_lv->mineView, &st, NULL, &sb, &sl, 0, FALSE, 0, 0);
  lv_screenTanksDestroy(&st);
  lv_screenBulletsDestroy(&sb);
  lv_screenLgmDestroy(&sl);
}

/*********************************************************
*NAME:          lv_screenSetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the value of a square in the structure
*
*ARGUMENTS:
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*  terrain - Terraint to set to
*********************************************************/
void lv_screenSetPos(BYTE xValue, BYTE yValue, BYTE terrain) {
  lv_mapSetPos(&g_lv->mp, xValue, yValue, terrain);
  lv_basesDeleteBase(&g_lv->bs, xValue, yValue);
  lv_startsDeleteStart(&g_lv->ss, xValue, yValue);
  lv_pillsDeletePill(&g_lv->pb, xValue, yValue);
}

/*********************************************************
*NAME:          lv_screenGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the value of a square in the structure
*  Return DEEP_SEA if out of range
*
*ARGUMENTS:
*  value  - Pointer to the screen structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
BYTE lv_screenGetPos(screen *value,BYTE xValue, BYTE yValue) {
  BYTE returnValue = DEEP_SEA; /* Value to return */

  /* Buffer holds sizeX+1 by sizeY+1 tiles (visible + 1-tile margin for
   * sub-tile scrolling). Stride is sizeX+1. */
  if (xValue <= lv_screenGetSizeX() && yValue <= lv_screenGetSizeY()) {
      returnValue = *((*g_lv->view).screenItem+(yValue*(lv_screenGetSizeX()+1)+xValue));
  }
  return returnValue;
}

#include "lv_log.h"
void lv_windowAddEvent(int eventType, char *msg);


void lv_screenProcessLog(unsigned short numEvents) {
  unsigned short count = 0;
  BYTE code;
  BYTE opt1, opt2, opt3, opt4, opt5, px, py, frame, onBoat;
  char mem[4096 + 1024]; /* Extra space for snprintf format overhead with names */
  char str[4096];
  char name[256];
  char name2[256];

  while (count < numEvents) {
    bool isV2 = (g_lv->loadedLogVersion == LOG_VERSION_V2);
    unsigned short evLen = 0; /* v2 only: framed payload length after code */

    logReadBytes(&code, 1);

    if (isV2) {
      /* v2 frames every event as [type][u16 BE payload-length][payload].
         Read the length unconditionally; known-type cases below consume
         exactly that many payload bytes, unknown types skip it. */
      BYTE lenBytes[2];
      logReadBytes(lenBytes, 2);
      evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
    }

    switch (code) {
    case log_PlayerJoined:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      logReadBytes(&opt5, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, name);
      {
        BYTE accountFlags = 0;
        if (g_lv->loadedLogVersion == LOG_VERSION_V0) {
          /* Version 0: opt2-opt5 are IP address octets */
          snprintf(mem, sizeof(mem), "%d.%d.%d.%d", opt2, opt3, opt4, opt5);
          lv_dnsLookup(mem, str, sizeof(str));
          snprintf(mem, sizeof(mem), "%s", str);
        } else if (g_lv->loadedLogVersion == LOG_VERSION_V1 ||
                   g_lv->loadedLogVersion == LOG_VERSION_V2) {
          /* Version 1/2: opt2-opt3 are 2-char country code,
           * opt4 is accountFlags (bit 0=WBN, bit 1=Steam, bit 5=bot),
           * opt5 reserved (zero in current writers). */
          snprintf(mem, sizeof(mem), "[%c%c]", opt2, opt3);
          accountFlags = opt4;
        }
        lv_playersSetPlayer(opt1, name, mem, 0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE, FALSE, accountFlags);
      }
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, name);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_JOINED, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = true;
        g_lv->gameViewHud[opt1].respawnTimeMs = g_lv->timeRunning;
      }
      break;
    case log_PlayerQuit:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      lv_playersLeaveGame(opt1, TRUE);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_QUIT_GAME, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        if (g_lv->gameView && opt1 == g_lv->cameraSlot) {
          /* Advance camera to next in-use slot. Mirrors Tab cycle in
           * logviewer.c:651-660. The `next != opt1` guard skips the
           * leaving player explicitly: lv_playersIsInUse(opt1) may still
           * return TRUE here, and we don't want the camera to bounce
           * back. */
          BYTE start = g_lv->cameraSlot;
          BYTE found = start;
          BYTE i;
          for (i = 1; i <= MAX_TANKS; i++) {
            BYTE next = (BYTE)((start + i) % MAX_TANKS);
            if (next != opt1 && lv_playersIsInUse(next)) {
              found = next;
              break;
            }
          }
          g_lv->cameraSlot = found;
          g_lv->wantScreenUpdate = TRUE;
        }
      }
      break;
    case log_LostMan:
      logReadBytes(&opt1, 1);
      lv_playersSetLgmDead(opt1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_LGM_DEAD, &args);
      }
      break;
    case log_MapChange:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_mapSetPos(&g_lv->mp, opt1, opt2, opt3);
      break;
    case log_ChangeName:
      logReadBytes(&opt1, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      lv_playersSetPlayerName(opt1, str);
      break;
    case log_AllyRequest:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      lv_playersGetPlayerName(opt2, mem, sizeof(mem));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_ALLY_REQUEST, &args);
      }
      break;
    case log_AllyAccept:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersAcceptAlliance(opt1, opt2);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      lv_playersGetPlayerName(opt2, mem, sizeof(mem));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_ALLY_ACCEPT, &args);
      }
      break;
    case log_AllyLeave:
      logReadBytes(&opt1, 1);
      lv_playersLeaveAlliance(opt1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_ALLY_LEAVE, &args);
      }
      break;
    case log_SoundBuild:
    case log_SoundFarm:
    case log_SoundShoot:
    case log_SoundHitTank:
    case log_SoundHitTree:
    case log_SoundHitWall:
    case log_SoundMineLay:
    case log_SoundMineExplode:
    case log_SoundExplosion:
    case log_SoundBigExplosion:
    case log_SoundManDie:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);

      if (code == log_SoundBuild) {
        opt3 = manBuildingNear;
      } else if (code == log_SoundFarm) {
        opt3 = farmingTreeNear;
      } else if (code == log_SoundShoot) {
        opt3 = shootSelf;
      } else if (code == log_SoundHitTank) {
        opt3 = hitTankNear;
      } else if (code == log_SoundHitWall) {
        opt3 = shotBuildingNear;
      } else if (code == log_SoundMineLay) {
        opt3 = manLayingMineNear;
      } else if (code ==log_SoundMineExplode) {
        opt3 = mineExplosionNear;
      } else if (code == log_SoundExplosion) {
        opt3 = mineExplosionNear;
      } else if (code == log_SoundBigExplosion) {
        opt3 = bigExplosionNear;
      } else if (code == log_SoundHitTree) {
        opt3 = shotTreeNear;
      } else {
         opt3 = manDyingNear;
      }
      lv_soundDist(opt3, opt1, opt2);
      break;
    case log_PlayerLocation:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      logReadBytes(&opt5, 1);
      lv_utilGetNibbles(opt4, &px, &py);
      lv_utilGetNibbles(opt5, &frame, &onBoat);
      if (opt2 != 0) {
        lv_playersUpdateTank(opt1, opt2, opt3, px, py, frame, onBoat);
        if (opt1 < MAX_TANKS && !g_lv->gameViewHud[opt1].alive) {
          g_lv->gameViewHud[opt1].alive = true;
          g_lv->gameViewHud[opt1].respawnTimeMs = g_lv->timeRunning;
        }
      }
      break;
    case log_TankSetStock:
      logReadBytes(&opt1, 1);  /* player */
      logReadBytes(&opt2, 1);  /* shells */
      logReadBytes(&opt3, 1);  /* mines */
      logReadBytes(&opt4, 1);  /* armour */
      logReadBytes(&opt5, 1);  /* trees */
      lv_screenSetTankStock(opt1, opt2, opt3, opt4, opt5);
      break;
    case log_TankSetModifiers: {
      /* player, then a length-prefixed blob of the six modifier bytes. */
      BYTE modLen;
      BYTE mods[6];
      logReadBytes(&opt1, 1);
      logReadBytes(&modLen, 1);
      if (modLen == sizeof(mods) && logReadBytes(mods, sizeof(mods)) == (int)sizeof(mods)) {
        lv_screenSetTankModifiers(opt1, mods);
      }
      break;
    }
    case log_Shell:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      lv_utilGetNibbles(opt3, &px, &py);
      lv_shellsAddItem(&g_lv->shs, opt1, opt2, px, py, opt4);
      break;
    case log_LgmLocation:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      lv_utilGetNibbles(opt1, &onBoat, &frame);
      lv_utilGetNibbles(opt4, &px, &py);
      lv_playersUpdateLgm(onBoat, opt2, opt3, px, py, frame);
      break;
    case log_MessageAll:
      logReadBytes(&opt1, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      lv_playersGetPlayerName(opt1, name, sizeof(name));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, name);
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_MSG_ALL, &args);
      }
      break;
    case log_MessagePlayers:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      lv_playersGetPlayerName(opt1, name, sizeof(name));
      lv_playersGetPlayerName(opt2, name2, sizeof(name2));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, name);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, name2);
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_MSG_PLAYERS, &args);
      }
      break;
    case log_MessageServer:
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      {
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(networkMessage, MESSAGE_NETSERVER, STR_LV_MSG_SERVER, &args);
      }
      break;
    case log_ServerText:
      /* A server line with the destination it was published to: opt1 the team
         it was held to, opt2 the slot. A line the whole game saw carries 0 and
         0xFF and reads like any other server line; one that reached a single
         team or a single player says so, because the recording is the only
         place that difference is visible. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      {
        MessageArgs args = {0};
        /* The destination and the line share one 64-byte argument, so each
           part carries its own precision — the same defence the message cases
           above use against a name or a line longer than the field. */
        if (opt2 != 0xFF) {
          lv_playersGetPlayerName(opt2, name, sizeof(name));
          snprintf(args.string1, sizeof(args.string1), "[to %.*s] %.*s",
                   16, name, 36, str);
        } else if (opt1 != 0) {
          snprintf(args.string1, sizeof(args.string1), "[to team %u] %.*s",
                   (unsigned)opt1, 36, str);
        } else {
          snprintf(args.string1, sizeof(args.string1), "%.*s",
                   (int)sizeof(args.string1) - 1, str);
        }
        lv_messageAdd(networkMessage, MESSAGE_NETSERVER, STR_LV_MSG_SERVER, &args);
      }
      break;
    case log_GameTimeSet:
      /* The round's game time, as a big-endian int32 of ticks. The viewer
         counts gmeLength down a tick at a time the way the server does, so
         adopting the recorded value keeps a replay's clock on the round's own
         remaining time instead of the length the round opened with. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      g_lv->gmeLength = (int32_t)(((uint32_t)opt1 << 24) |
                                  ((uint32_t)opt2 << 16) |
                                  ((uint32_t)opt3 << 8)  |
                                  (uint32_t)opt4);
      break;
    case log_BaseSetOwner:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_basesSetOwner(&g_lv->bs, opt1, opt2, opt3);
      break;
    case log_BaseSetStock:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      lv_basesSetStock(&g_lv->bs, opt1, opt2, opt3, opt4);
      /* opt1 is the 0-based base index in the log (the server emits it
       * post-decrement — see basesSetBase / basesUpdateStock in
       * src/bolo/bases.c). */
      break;
    case log_PillSetOwner:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_pillsSetPillOwner(&g_lv->pb, opt1, opt2, opt3);
      break;
    case log_PillSetPlace:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      lv_pillsSetPos(&g_lv->pb, opt1, opt2, opt3);
      break;
    case log_PillSetHealth:
      logReadBytes(&opt1, 1);
      lv_utilGetNibbles(opt1, &opt2, &opt3);
      lv_pillsSetHealth(&g_lv->pb, opt2, opt3);
      break;
    case log_PillSetInTank:
      logReadBytes(&opt1, 1);
      lv_utilGetNibbles(opt1, &opt2, &opt3);
      lv_pillsSetInTank(&g_lv->pb, opt2, opt3);
      break;
    case log_EntityChange:
      /* One pillbox, base or start has joined the map or left it. opt1 is
         which list, opt2 the item's number counting from zero — the three
         modules count from one — and opt3 whether it is now on the map. The
         pascal blob after them is the item's map record, six bytes for a
         pillbox or a base and three for a start, the same bytes a live client
         gets on the wire. A removal keeps the slot and the count, so every
         number above it goes on meaning the same item. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      {
        BYTE num = (BYTE)(opt2 + 1);
        const BYTE *rec = (const BYTE *)(mem + 1);
        BYTE recLen = (BYTE)mem[0];
        switch (opt1) {
        case LV_ENTITY_KIND_PILL:
          if (opt3 != 0) {
            if (recLen >= 6) {
              pillbox item;
              memset(&item, 0, sizeof(item));
              item.x = rec[0];
              item.y = rec[1];
              item.owner = rec[2];
              item.armour = rec[3];
              item.speed = rec[4];
              item.inTank = rec[5] ? TRUE : FALSE;
              lv_pillsInstallItem(&g_lv->pb, &item, num);
            }
          } else {
            lv_pillsRemoveItem(&g_lv->pb, num);
          }
          break;
        case LV_ENTITY_KIND_BASE:
          if (opt3 != 0) {
            if (recLen >= 6) {
              base item;
              memset(&item, 0, sizeof(item));
              item.x = rec[0];
              item.y = rec[1];
              item.owner = rec[2];
              item.armour = rec[3];
              item.shells = rec[4];
              item.mines = rec[5];
              lv_basesInstallItem(&g_lv->bs, &item, num);
            }
          } else {
            lv_basesRemoveItem(&g_lv->bs, num);
          }
          break;
        case LV_ENTITY_KIND_START:
          if (opt3 != 0) {
            if (recLen >= 3) {
              start item;
              memset(&item, 0, sizeof(item));
              item.x = rec[0];
              item.y = rec[1];
              item.dir = rec[2];
              lv_startsInstallItem(&g_lv->ss, &item, num);
            }
          } else {
            lv_startsRemoveItem(&g_lv->ss, num);
          }
          break;
        default:
          /* A kind with no list behind it. The framed length has already
             been consumed, so there is nothing to resynchronise. */
          break;
        }
      }
      g_lv->wantScreenUpdate = TRUE;
      break;
    case log_EntityMasks:
      /* Which indices are on the map, as three big-endian 16-bit masks — the
         part the snapshot before this one had nowhere to put. Bit i stands
         for index i counting from zero; a bit at or above a list's own count
         names no item and is skipped. Only the flags move: the counts and the
         records are the ones the snapshot installed, and taking an item off
         the map keeps its record, so an index put back holds the item it
         always held. That makes a snapshot and this record together enough to
         state the world, which is what a seek lands on. */
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      logReadBytes(&opt5, 1);
      logReadBytes(&px, 1);
      {
        unsigned short pillMask  = (unsigned short)((opt1 << 8) | opt2);
        unsigned short baseMask  = (unsigned short)((opt3 << 8) | opt4);
        unsigned short startMask = (unsigned short)((opt5 << 8) | px);
        BYTE num;
        BYTE total;

        /* Each count is clamped to its list's size before it is walked: a
           16-bit mask cannot name anything past index 15 anyway, and a count
           a malformed blob left above the array is not a number this loop
           should reach for. */
        total = lv_pillsGetNumPills(&g_lv->pb);
        if (total > MAX_PILLS) total = MAX_PILLS;
        for (num = 1; num <= total; num++) {
          lv_pillsSetActive(&g_lv->pb, num,
                            (pillMask & (1u << (num - 1))) != 0);
        }
        total = lv_basesGetNumBases(&g_lv->bs);
        if (total > MAX_BASES) total = MAX_BASES;
        for (num = 1; num <= total; num++) {
          lv_basesSetActive(&g_lv->bs, num,
                            (baseMask & (1u << (num - 1))) != 0);
        }
        total = lv_startsGetNumStarts(&g_lv->ss);
        if (total > MAX_STARTS) total = MAX_STARTS;
        for (num = 1; num <= total; num++) {
          lv_startsSetActive(&g_lv->ss, num,
                             (startMask & (1u << (num - 1))) != 0);
        }
      }
      g_lv->wantScreenUpdate = TRUE;
      break;
    case log_KillPlayer:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, mem, sizeof(mem));
      if (opt1 == opt2 || opt2 == NEUTRAL) {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_DIED, &args);
      } else {
        MessageArgs args = {0};
        lv_playersGetPlayerName(opt2, str, sizeof(str));
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        snprintf(args.otherName, sizeof(args.otherName), "%.*s", (int)sizeof(args.otherName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_KILLED, &args);
      }
      if (opt1 < MAX_TANKS) g_lv->deaths[opt1]++;
      if (opt2 != opt1 && opt2 != NEUTRAL && opt2 < MAX_TANKS) {
        g_lv->kills[opt2]++;
      }
      lv_playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        g_lv->gameViewHud[opt1].deathTimeMs = g_lv->timeRunning;
      }
      break;
    case log_PlayerRejoin:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_REJOINED, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = true;
        g_lv->gameViewHud[opt1].respawnTimeMs = g_lv->timeRunning;
      }
      break;
    case log_PlayerLeaving:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_LEAVING, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        if (g_lv->gameView && opt1 == g_lv->cameraSlot) {
          /* Advance camera to next in-use slot. Mirrors Tab cycle in
           * logviewer.c:651-660. The `next != opt1` guard skips the
           * leaving player explicitly: lv_playersIsInUse(opt1) may still
           * return TRUE here, and we don't want the camera to bounce
           * back. */
          BYTE start = g_lv->cameraSlot;
          BYTE found = start;
          BYTE i;
          for (i = 1; i <= MAX_TANKS; i++) {
            BYTE next = (BYTE)((start + i) % MAX_TANKS);
            if (next != opt1 && lv_playersIsInUse(next)) {
              found = next;
              break;
            }
          }
          g_lv->cameraSlot = found;
          g_lv->wantScreenUpdate = TRUE;
        }
      }
      break;
    case log_PlayerDied:
      logReadBytes(&opt1, 1);
      lv_playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        g_lv->gameViewHud[opt1].deathTimeMs = g_lv->timeRunning;
      }
      break;
    case log_SaveMap:
      /* No-op — marker event with no visual effect on replay */
      break;
    case log_LobbyEnter:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_LOBBY_OPENED, NULL);
      break;
    case log_LobbyExit:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_GAME_STARTED, NULL);
      break;
    case log_PlayerReady:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_READY, &args);
      }
      break;
    case log_PlayerUnready:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_UNREADY, &args);
      }
      break;
    case log_TeamSet:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        if (opt2 == 0) {
          lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_LEFT_TEAM, &args);
        } else {
          args.number = opt2;
          lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_JOINED_TEAM, &args);
        }
      }
      break;
    case log_CountdownStart:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_COUNTDOWN_START, NULL);
      break;
    case log_CountdownCancel:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_COUNTDOWN_CANCEL, NULL);
      break;
    case log_MapSkipVote:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_MAP_SKIP_VOTE, &args);
      }
      break;
    case log_MapSkipApplied:
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      {
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%.*s", (int)sizeof(args.string1) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_MAP_SKIPPED, &args);
      }
      break;
    case log_GameSettings:
      /* Panel data, not a chat line: the Game Information window reads the
         settings back out of the store, so nothing goes to the newswire. */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_screenPlaybackGameSettings((BYTE *)(mem+1), (unsigned char)mem[0]);
      break;
    case log_BalanceApplied:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_TEAM_BALANCE, NULL);
      break;
    case log_GameVoteStart:
      logReadBytes(&opt1, 1);  /* kind */
      logReadBytes(&opt2, 1);  /* initiator */
      logReadBytes(&opt3, 1);  /* team (0 = global) */
      lv_playersGetPlayerName(opt2, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE,
                      opt1 == GAME_VOTE_KIND_SURRENDER
                          ? STR_LV_VOTE_START_SURRENDER
                          : STR_LV_VOTE_START_LOBBY,
                      &args);
      }
      break;
    case log_GameVoteCast:
      logReadBytes(&opt1, 1);  /* kind */
      logReadBytes(&opt2, 1);  /* player */
      logReadBytes(&opt3, 1);  /* voteYes (0/1) */
      lv_playersGetPlayerName(opt2, str, sizeof(str));
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE,
                      opt3 ? STR_LV_VOTE_CAST_YES : STR_LV_VOTE_CAST_NO,
                      &args);
      }
      break;
    case log_GameVoteEnd:
      logReadBytes(&opt1, 1);  /* kind */
      logReadBytes(&opt2, 1);  /* result */
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER,
                    opt2 ? STR_LV_VOTE_PASSED : STR_LV_VOTE_FAILED, NULL);
      break;
    case log_Ping:
      /* sender + kind + worldX (BE u16) + worldY (BE u16). */
      logReadBytes(&opt1, 1);  /* sender */
      logReadBytes(&opt2, 1);  /* kind */
      logReadBytes(&opt3, 1);  /* worldX high */
      logReadBytes(&opt4, 1);  /* worldX low */
      logReadBytes(&opt5, 1);  /* worldY high */
      {
        BYTE yLo = 0;
        uint16_t wx, wy;
        logReadBytes(&yLo, 1);
        wx = (uint16_t)((opt3 << 8) | opt4);
        wy = (uint16_t)((opt5 << 8) | yLo);
        lv_pingAdd(opt1, opt2, wx, wy);
        lv_playersGetPlayerName(opt1, str, sizeof(str));
        {
          MessageArgs args = {0};
          snprintf(args.playerName, sizeof(args.playerName), "%.*s",
                   (int)sizeof(args.playerName) - 1, str);
          lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE,
                        pingKindMessageId(opt2), &args);
        }
      }
      break;
    case log_SpectatorJoined:
      logReadBytes(&opt1, 1);  /* spectator slot */
      logReadBytes(&opt2, 1);  /* country[0] */
      logReadBytes(&opt3, 1);  /* country[1] */
      logReadBytes(&opt4, 1);  /* wbnFlags (not displayed) */
      logReadBytes(&opt5, 1);  /* reserved */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, name);
      {
        /* Prefix the 2-char country into {player} only when present and not
           the "XX" unknown sentinel; the [%c%c] tag is a raw country code and
           is not localized. */
        MessageArgs args = {0};
        if (opt2 != 0 && opt3 != 0 && !(opt2 == 'X' && opt3 == 'X')) {
          snprintf(args.playerName, sizeof(args.playerName), "[%c%c] %s", opt2, opt3, name);
        } else {
          snprintf(args.playerName, sizeof(args.playerName), "%s", name);
        }
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_SPEC_JOINED, &args);
      }
      break;
    case log_SpectatorLeft:
      logReadBytes(&opt1, 1);  /* spectator slot */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      lv_utilPtoCString(mem, name);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%s", name);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_SPEC_LEFT, &args);
      }
      break;
    case log_SpectatorChat:
      /* No emitter yet (format-reserved for Phase 6); decode so the cursor
         stays aligned and the line is ready when chat ships. */
      logReadBytes(&opt1, 1);  /* sender spectator slot */
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);  /* copies the message out of mem */
      {
        MessageArgs args = {0};
        args.number = opt1;
        snprintf(args.string1, sizeof(args.string1), "%s", str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_SPEC_CHAT, &args);
      }
      break;
    default:
      if (isV2) {
        /* Unknown future event type: skip its framed payload and keep
           going. evLen is a full u16 so it can exceed the v1 per-type
           maximum; advance the read cursor directly. */
        lv_logSetPosition(lv_logGetCurrentPosition() + evLen);
      } else {
        lv_windowStop(TRUE);
        count = numEvents;
      }
      break;

    }
    /* v2 is plaintext: blockKey stays 0 for the whole stream, so the
       per-event key roll is suppressed. v1 rolls the key to the event
       code (matches the writer's logKey rotation). */
    if (!isV2) {
      lv_blocksSetKey(code);
    }
    count++;
  }
}

void lv_screenRequestUpdate() {
  /* Finally lets update our item in the frontend */
  if (g_lv->isPlaying == TRUE && g_lv->fastForwarding == FALSE) {
    if (g_lv->selectedItemType == 0) {
      lv_updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (g_lv->selectedItemType == 1) {
      lv_updateItem(1, g_lv->selectedItem, g_lv->bs->item[g_lv->selectedItem].owner, g_lv->bs->item[g_lv->selectedItem].x, g_lv->bs->item[g_lv->selectedItem].y, g_lv->bs->item[g_lv->selectedItem].armour, g_lv->bs->item[g_lv->selectedItem].shells, g_lv->bs->item[g_lv->selectedItem].mines, FALSE);
    } else {
      lv_updateItem(2, g_lv->selectedItem, g_lv->pb->item[g_lv->selectedItem].owner, g_lv->pb->item[g_lv->selectedItem].x, g_lv->pb->item[g_lv->selectedItem].y, g_lv->pb->item[g_lv->selectedItem].armour, 0, 0, g_lv->pb->item[g_lv->selectedItem].inTank);
    }
  }
}

/* Returns TRUE on log end or snapshot */
bool lv_screenLogTick() {
  bool returnValue = FALSE;
  BYTE code;
  BYTE top;
  BYTE bottom;
  unsigned short us;
  unsigned short len = 0;

  bool process = FALSE;
  g_lv->timeRunning += 20; /* Add 20 ms */
  if (g_lv->gmeStartDelay > 0) {
    g_lv->gmeStartDelay--;
  }
  if (g_lv->gmeLength > 0 && g_lv->gmeStartDelay == 0) {
    g_lv->gmeLength--;
  }
  lv_shellsDestroy(&g_lv->shs);
  g_lv->shs = lv_shellsCreate();
  if (g_lv->isPlaying == TRUE) {
    switch (g_lv->state) {
    case lv_lr_start:
      /* Read bytes */
      logReadBytes(&code, 1);
      switch (code) {
      case LOG_QUIT:
        g_lv->isPlaying = FALSE;
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_END_OF_LOG, NULL);
        lv_finished();
        returnValue = TRUE;
        break;
      case LOG_SNAPSHOT:
        lv_processSnapshot();
        returnValue = TRUE;
        break;
      case LOG_NOEVENTS:
        logReadBytes((BYTE *) &g_lv->waitLen, 1);
        g_lv->state = lv_lr_shortwait;
        if (g_lv->waitLen == 0) {
          g_lv->waitLen = 1;
        }
        break;
      case LOG_NOEVENTS_LONG:
        logReadBytes(&top, 1);
        logReadBytes(&bottom, 1);
        us = top << 8;
        us += bottom;
        g_lv->waitLen = ntohs(us);

        g_lv->state = lv_lr_longwait;
        if (g_lv->waitLen == 0) {
          g_lv->waitLen = 1;
        }
        break;
      case LOG_EVENT:
        logReadBytes(&top, 1);
        len = top;
        process = TRUE;
        break;
      case LOG_EVENT_LONG:
        logReadBytes(&top, 1);
        logReadBytes(&bottom, 1);
        us = top << 8;
        us += bottom;
        len = ntohs(us);
        process = TRUE;
        break;
      }
      break;
    case lv_lr_longwait:
    case lv_lr_shortwait:
      g_lv->waitLen--;
      if (g_lv->waitLen == 0) {
        g_lv->state = lv_lr_start;
      }
      break;
    }
    if (process == TRUE) {
      lv_playersLgmZero();
      lv_screenProcessLog(len);
      if (g_lv->centredTank == TRUE) {
        lv_screenFollowCentredTank();
      }
    }
  }
  return returnValue;
}

/* Put the camera on the followed tank, in native pixels rather than whole
 * map squares.
 *
 * This used to assign xOffset/yOffset straight from lv_playersGetCentredX/Y —
 * which are BYTE map squares — and never touch subPxX/subPxY, so the view
 * could only move in 16-pixel steps: the tank drifted a whole tile off centre
 * and the whole world snapped back the instant it crossed a boundary. One
 * jump per tile of travel, forever, which is exactly the cyclic one-tile jump
 * the reel showed while a round played.
 *
 * Everything needed for the smooth version was already here: the render target
 * carries one spare tile (draw.c sizes it (screenSizeX + 1) * TILE_SIZE_X) and
 * the host already uses subPxX/subPxY as the blit's source origin
 * (lv_screenGetSubOffset -> lvEmbedFrameTexture). Only the sub-tile part was
 * missing. Same defect and same fix as viewCamPixelsF in the map preview
 * widget: derive every value from one un-truncated position.
 *
 * Clamps mirror lv_screenPanToTotalPixels — the whole-tile range is
 * [0, 255 - screenSize] and sub-pixel is forced to zero at the far edge so the
 * trailing edge has no bleed past the rendered tiles.
 *
 * A sub-tile-only move needs no tile re-render: the host re-reads the sub
 * offset every frame and shifts the source rect. wantScreenUpdate is still set
 * because the tank sprites inside the target moved. */
void lv_screenFollowCentredTank(void) {
  int px, py;
  int sizeX, sizeY, maxOffX, maxOffY, maxPxX, maxPxY;
  int camPxX, camPxY, newOffX, newOffY, newSubX, newSubY;

  if (g_lv == NULL || g_lv->logLoaded == FALSE) {
    return;
  }
  px = lv_playersGetCentredPixelX();
  py = lv_playersGetCentredPixelY();
  if (px < 0 || py < 0) {
    return;
  }

  sizeX = lv_screenGetSizeX();
  sizeY = lv_screenGetSizeY();
  /* Centre the viewport on the tank, in pixels. */
  camPxX = px - (sizeX * TILE_SIZE_X) / 2;
  camPxY = py - (sizeY * TILE_SIZE_Y) / 2;

  maxOffX = 255 - sizeX; if (maxOffX < 0) maxOffX = 0;
  maxOffY = 255 - sizeY; if (maxOffY < 0) maxOffY = 0;
  maxPxX = maxOffX * TILE_SIZE_X;
  maxPxY = maxOffY * TILE_SIZE_Y;
  if (camPxX < 0) camPxX = 0;
  if (camPxY < 0) camPxY = 0;
  if (camPxX > maxPxX) camPxX = maxPxX;
  if (camPxY > maxPxY) camPxY = maxPxY;

  newOffX = camPxX / TILE_SIZE_X;
  newOffY = camPxY / TILE_SIZE_Y;
  newSubX = camPxX - newOffX * TILE_SIZE_X;
  newSubY = camPxY - newOffY * TILE_SIZE_Y;

  /* Compare BEFORE storing, then flag on ANY move, sub-tile included: the
   * host's blit origin comes from a snapshot taken at render time, so a camera
   * move that triggers no re-render would never reach the screen. Matters most
   * for the paused reel, where a focus jump is the only thing moving; during
   * playback the sprites dirty the screen every tick anyway, so this costs
   * nothing extra. Skipping the flag when nothing moved keeps a paused,
   * unfocused reel fully idle. */
  if ((BYTE)newOffX != g_lv->xOffset || (BYTE)newOffY != g_lv->yOffset ||
      newSubX != g_lv->subPxX || newSubY != g_lv->subPxY) {
    g_lv->xOffset = (BYTE)newOffX;
    g_lv->yOffset = (BYTE)newOffY;
    g_lv->subPxX  = newSubX;
    g_lv->subPxY  = newSubY;
    g_lv->wantScreenUpdate = TRUE;
  }
}

void lv_screenCentreOnSelectedItem() {
  BYTE x;
  BYTE y;
  base b;
  pillbox p;
  if (g_lv->isPlaying == TRUE && g_lv->selectedItemType != 0) {
    if (g_lv->selectedItemType == 2) {
      lv_pillsGetPill(&g_lv->pb, &p, (BYTE) (g_lv->selectedItem+1));
      x = p.x;
      y = p.y;
    } else {
      lv_basesGetBase(&g_lv->bs, &b, (BYTE) (g_lv->selectedItem+1));
      x = b.x;
      y = b.y;
    }
    int cx = (int)x - (int)(g_lv->screenSizeX / 2);
    int cy = (int)y - (int)(g_lv->screenSizeY / 2);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + g_lv->screenSizeX > 255) cx = 255 - g_lv->screenSizeX;
    if (cy + g_lv->screenSizeY > 255) cy = 255 - g_lv->screenSizeY;
    BYTE newXOffset = (BYTE)cx;
    BYTE newYOffset = (BYTE)cy;
    if (newXOffset != g_lv->xOffset || g_lv->yOffset != newYOffset) {
      g_lv->xOffset = newXOffset;
      g_lv->yOffset = newYOffset;
      /* Defer to the flag — see lv_screenPanToTotalPixels: an inline render
       * during input pairs new content with the frame's already-recorded
       * blit offsets (one displaced frame). */
      g_lv->wantScreenUpdate = TRUE;
    }
  }
}

/* Whether the snapshot lv_processSnapshot last decoded carried a world. The
 * recorder's lobby-mode body writes no map runs at all (logSerializeSnapshotBody
 * in log.c), a running-round body always writes at least one, so the run count
 * is what separates the two. */
static bool s_lastSnapshotHadWorld = FALSE;

/* The same answer for the opening snapshot, which the loader consumes before
 * the event stream starts. Set, it means the log opened on a running round and
 * has no lobby in front of it. Both loaders write it as they decode that
 * snapshot, so it needs no separate reset. */
static bool s_openingSnapshotHadWorld = FALSE;

bool lv_processSnapshot() {
  bool returnValue = TRUE;
  BYTE data[512];
  BYTE dataLenRaw;
  unsigned int dataLen;
  int len;
  BYTE count = 0;
  BYTE mx, my, px, py, lgmmx, lgmmy, lgmpx, lgmpy, lgmframe, frame;
  bool onBoat;
  char name[64];
  char location[64];
  BYTE numAllies;
  BYTE *allies;
  BYTE pos;

  // We should add this snapshot timestamp and file location to the store so we can goto later
  lv_playersCopyPTeams(data);
  lv_snapshotAdd(&g_lv->snap, lv_logGetCurrentPosition(), g_lv->timeRunning, lv_blocksGetKey(), data);


  /* Read in start delay and time limit */
  logReadBytes((BYTE *) &g_lv->gmeStartDelay, sizeof(int32_t));
  g_lv->gmeStartDelay = ntohl(g_lv->gmeStartDelay);
  logReadBytes((BYTE *) &g_lv->gmeLength , sizeof(int32_t));
  g_lv->gmeLength = ntohl(g_lv->gmeLength);

  /* Read pillboxes, bases and starts */
  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      lv_pillsSetPillNetData(&g_lv->pb, data, dataLen);
    }
  }

  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      lv_basesSetBaseNetData(&g_lv->bs, data, dataLen);
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      lv_startsSetStartNetData(&g_lv->ss, data, dataLen);
    }
  }
  if (returnValue == TRUE) {
    int numRuns = 0;
    returnValue = lv_mapReadRuns(&g_lv->mp, &numRuns);
    s_lastSnapshotHadWorld = (numRuns > 0);
  }


  /* Process each player */
  while (count < MAX_TANKS && returnValue == TRUE) {
    logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen == 2) {
      /* Player is not in use */
      lv_playersLeaveGame(count, FALSE);
      // Process the dummy data
      len = logReadBytes(data, 2);
      lv_screenSetTankStock(count, 0, 0, 0, 0);

    } else if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      len = logReadBytes(data, dataLen);
      if ((unsigned int)len != dataLen) {
        returnValue = FALSE;
      } else if (dataLen < 12) {
        /* Minimum player record: 2 header + 9 fixed fields + 1 name len byte */
        returnValue = FALSE;
      } else {
        pos = 2;
        mx = data[pos++];
        my = data[pos++];
        lv_utilGetNibbles(data[pos++], &px, &py);
        frame = data[pos++];
        onBoat = data[pos++];
        lgmmx = data[pos++];
        lgmmy = data[pos++];
        lv_utilGetNibbles(data[pos++], &lgmpx, &lgmpy);
        lgmframe = data[pos++];
        /* pos is now 11 — skip the redundant dataLen byte */
        pos++;
        /* Parse player name (pascal string: length byte + chars) */
        if (pos > dataLen || *(data+pos-1) >= sizeof(name)) {
          returnValue = FALSE;
        } else {
          lv_utilPtoCString((char *)(data+pos-1), name);
          pos += *(data+pos-1);
          pos++;
        }
        /* Parse location (pascal string) */
        if (returnValue == TRUE) {
          if (pos > dataLen || *(data+pos-1) >= sizeof(location)) {
            returnValue = FALSE;
          } else {
            lv_utilPtoCString((char *)(data+pos-1), location);
            pos += *(data+pos-1);
          }
        }
        /* Parse allies */
        if (returnValue == TRUE) {
          if (pos >= dataLen) {
            returnValue = FALSE;
          } else {
            numAllies = data[pos];
            pos++;
            if ((unsigned int)(pos + numAllies) > dataLen) {
              returnValue = FALSE;
            } else {
              allies = data+pos;
              /* Snapshot replay does not extract the clientFlags byte in
               * TankSnapshot; default the v1-log accountFlags storage to 0.
               * The flags will be re-set by any subsequent log_PlayerJoined
               * event for this slot. */
              lv_playersSetPlayer(count, name, location, mx ,my, px, py, frame, onBoat, numAllies, allies, FALSE, TRUE, 0);
              /* Tank stocks: shells, mines, armour, trees, on the end of the
                 block after the alliance list. A block that ends with the
                 alliances was written before the recorder carried them, so the
                 slot reads as no stocks rather than a guessed value. */
              pos = (BYTE)(pos + numAllies);
              if ((unsigned int)(pos + 4) <= dataLen) {
                lv_screenSetTankStock(count, data[pos], data[pos+1],
                                      data[pos+2], data[pos+3]);
              } else {
                lv_screenSetTankStock(count, 0, 0, 0, 0);
              }
              /* mx != 0 means the tank is on the map (alive) — the same sentinel
                 the forward log_PlayerLocation path uses. Mark the slot alive so
                 a mid-game seed clears the death-static overlay for living tanks;
                 a dead/off-map tank (mx == 0) stays not-alive. */
              if (mx != 0) {
                g_lv->gameViewHud[count].alive = true;
                g_lv->gameViewHud[count].respawnTimeMs = g_lv->timeRunning;
              }
              /* A snapshot carries (0,0) lgm coords for a man who is aboard/idle
                 (the server's idle sentinel). Only mark him out for a real
                 out-of-tank position; lv_playersSetPlayer above already left
                 lgmIsOut FALSE for the aboard case, matching the forward
                 stream where a boarded man emits no log_LgmLocation. */
              if (lgmmx != 0 || lgmmy != 0) {
                lv_playersUpdateLgm(count, lgmmx, lgmmy, lgmpx, lgmpy,lgmframe);
              }
            }
          }
        }
      }
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
* Walker for total-time computation
*
* Walks the decompressed log byte stream from the current
* position to LOG_QUIT/EOF, counting ticks (20ms each) so
* that the seek bar can show real time/remaining instead of
* a byte-ratio estimate. Saves and restores logPosition and
* the XOR key so it leaves no observable side effects.
*
* Compression makes byte position non-linear in time, and
* event density varies wildly between phases (lobby vs.
* play), so a one-shot scan is the only way to get a stable
* total. The buffer is fully decompressed before this runs,
* so it's just a byte walk.
*********************************************************/

/* Returns bytes-after-code consumed by event 'code' (excluding code byte
 * itself). For variable-length events with a pascal-string payload this
 * peeks the length byte by reading and decrypting it via lv_blocksReadBytes.
 * Returns -1 on read error / unknown event. */
static int walkSkipEventBody(BYTE code) {
  BYTE lenByte;
  int rc;
  switch (code) {
    /* Fixed-size payloads */
    case log_PlayerQuit:
    case log_LostMan:
    case log_AllyLeave:
    case log_PillSetHealth:
    case log_PillSetInTank:
    case log_PlayerRejoin:
    case log_PlayerLeaving:
    case log_PlayerDied:
    case log_PlayerReady:
    case log_PlayerUnready:
    case log_MapSkipVote:
      /* 1 byte */
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      return 1;
    case log_AllyRequest:
    case log_AllyAccept:
    case log_KillPlayer:
    case log_TeamSet:
    case log_SoundBuild:
    case log_SoundFarm:
    case log_SoundShoot:
    case log_SoundHitTank:
    case log_SoundHitTree:
    case log_SoundHitWall:
    case log_SoundMineLay:
    case log_SoundMineExplode:
    case log_SoundExplosion:
    case log_SoundBigExplosion:
    case log_SoundManDie:
    case log_GameVoteEnd:
      { BYTE b[2]; if (logReadBytes(b, 2) != 2) return -1; }
      return 2;
    case log_MapChange:
    case log_BaseSetOwner:
    case log_PillSetOwner:
    case log_PillSetPlace:
    case log_GameVoteStart:
    case log_GameVoteCast:
      { BYTE b[3]; if (logReadBytes(b, 3) != 3) return -1; }
      return 3;
    case log_BaseSetStock:
    case log_LgmLocation:
    case log_Shell:
    case log_GameTimeSet:
      { BYTE b[4]; if (logReadBytes(b, 4) != 4) return -1; }
      return 4;
    case log_PlayerLocation:
    case log_TankSetStock:
      { BYTE b[5]; if (logReadBytes(b, 5) != 5) return -1; }
      return 5;
    case log_Ping:
    case log_EntityMasks:
      /* A ping is sender + kind + two big-endian u16 coordinates; the masks
         record is three big-endian u16. Six bytes either way. Only v2 logs
         can carry either, but the v1 walker keeps a full table so a future
         re-encoder cannot silently desynchronise the cursor. */
      { BYTE b[6]; if (logReadBytes(b, 6) != 6) return -1; }
      return 6;
    case log_SaveMap:
    case log_LobbyEnter:
    case log_LobbyExit:
    case log_CountdownStart:
    case log_CountdownCancel:
    case log_BalanceApplied:
      return 0;
    /* Variable-length: pascal string trailing the fixed prefix */
    case log_PlayerJoined:
      /* 5 opt bytes + pascal string */
      { BYTE b[5]; if (logReadBytes(b, 5) != 5) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 6 + lenByte;
    case log_ChangeName:
    case log_TankSetModifiers:
      /* 1 opt byte + pascal string (the modifier record's blob is always six
         bytes, but it is walked as a pascal string like any other) */
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 2 + lenByte;
    case log_MessageAll:
      /* 1 opt byte + pascal string */
      { BYTE b; if (logReadBytes(&b, 1) != 1) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 2 + lenByte;
    case log_MessagePlayers:
    case log_ServerText:
      /* 2 opt bytes + pascal string */
      { BYTE b[2]; if (logReadBytes(b, 2) != 2) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 3 + lenByte;
    case log_EntityChange:
      /* kind + index + on-the-map flag + the item's record as a pascal blob.
         Only v2 logs can carry one, but the v1 walker keeps a full table for
         the reason it keeps one for log_Ping. */
      { BYTE b[3]; if (logReadBytes(b, 3) != 3) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 4 + lenByte;
    case log_MessageServer:
    case log_MapSkipApplied:
      /* pascal string only */
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 1 + lenByte;
    default:
      return -1;
  }
}

/* Skip an event packet of numEvents events. Reader updates the XOR key
 * to the event's code byte after each event (matches the writer's
 * logKey rotation). Returns FALSE on read error / unknown event. */
static bool walkSkipEvents(unsigned short numEvents) {
  unsigned short i;
  BYTE code;
  bool isV2 = (g_lv->loadedLogVersion == LOG_VERSION_V2);
  for (i = 0; i < numEvents; i++) {
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (isV2) {
      /* v2: [type][u16 BE payload-length][payload]. Skip via the framed
         length; blockKey stays 0 so no key roll. */
      BYTE lenBytes[2];
      unsigned short evLen;
      if (logReadBytes(lenBytes, 2) != 2) return FALSE;
      evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
      lv_logSetPosition(lv_logGetCurrentPosition() + evLen);
    } else {
      if (walkSkipEventBody(code) < 0) return FALSE;
      lv_blocksSetKey(code);
    }
  }
  return TRUE;
}

/* Skip a snapshot block. Mirrors lv_processSnapshot's read order without
 * applying any state. When baseX/baseY/numBases are non-NULL and the snapshot
 * carries a base table, the bases' map cells are copied out in blob order —
 * the same 0-based index a log_BaseSetOwner event carries. A snapshot with no
 * bases (any lobby-phase snapshot: the game world doesn't exist yet) leaves
 * the outputs untouched, so a previously extracted table survives. When
 * numRuns is non-NULL it reports how many non-terminator map runs the body
 * carried — none for a lobby snapshot, at least one for a running round. */
static bool walkSkipSnapshotBases(uint8_t *baseX, uint8_t *baseY,
                                  int *numBases, int *numRuns) {
  BYTE buf[512];
  BYTE dlen;
  int32_t hdr;
  BYTE runHead[SIZEOFBMAP_RUN_HEADER];
  int i;
  int runs = 0;

  /* gmeStartDelay + gmeLength */
  if (logReadBytes((BYTE *)&hdr, (int)sizeof(int32_t)) != (int)sizeof(int32_t)) return FALSE;
  if (logReadBytes((BYTE *)&hdr, (int)sizeof(int32_t)) != (int)sizeof(int32_t)) return FALSE;

  /* pills, bases, starts: 1-byte len + len bytes (BYTE max 255 fits in buf) */
  for (i = 0; i < 3; i++) {
    if (logReadBytes(&dlen, 1) != 1) return FALSE;
    if (dlen > 0 && logReadBytes(buf, dlen) != dlen) return FALSE;
    if (i == 1 && numBases != NULL && dlen > 0 && buf[0] > 0) {
      /* Bases blob: [numBases] then 10 bytes per base — x, y, owner, armour,
       * shells, mines, refuelTime, baseTime(2), justStopped (the layout
       * lv_basesSetBaseNetData consumes). Only the cells are kept. */
      int n = buf[0] > MAX_BASES ? MAX_BASES : buf[0];
      int k;
      for (k = 0; k < n; k++) {
        int off = 1 + k * 10;
        if (off + 1 >= (int)dlen) { n = k; break; }
        baseX[k] = buf[off];
        baseY[k] = buf[off + 1];
      }
      *numBases = n;
    }
  }

  /* Map runs: 4-byte header repeating until terminator
   * (datalen==4, y==255, startx==255, endx==255). Non-terminator runs
   * are followed by (datalen - 4) data bytes. */
  for (;;) {
    if (logReadBytes(runHead, SIZEOFBMAP_RUN_HEADER) != SIZEOFBMAP_RUN_HEADER) return FALSE;
    /* Layout: datalen, y, startx, endx (all BYTE per bmapRunHeader) */
    if (runHead[0] == SIZEOFBMAP_RUN_HEADER && runHead[1] == MAP_ARRAY_LAST
        && runHead[2] == MAP_ARRAY_LAST && runHead[3] == MAP_ARRAY_LAST) {
      break;
    }
    {
      int dataBytes = (int)runHead[0] - SIZEOFBMAP_RUN_HEADER;
      if (dataBytes < 0) return FALSE;
      while (dataBytes > 0) {
        int chunk = dataBytes > (int)sizeof(buf) ? (int)sizeof(buf) : dataBytes;
        if (logReadBytes(buf, chunk) != chunk) return FALSE;
        dataBytes -= chunk;
      }
    }
    runs++;
  }
  if (numRuns != NULL) *numRuns = runs;

  /* MAX_TANKS player records: 1-byte len + len bytes (BYTE max 255 fits in buf) */
  for (i = 0; i < MAX_TANKS; i++) {
    if (logReadBytes(&dlen, 1) != 1) return FALSE;
    if (dlen > 0 && logReadBytes(buf, dlen) != dlen) return FALSE;
  }
  return TRUE;
}

static bool walkSkipSnapshot(void) {
  return walkSkipSnapshotBases(NULL, NULL, NULL, NULL);
}

/* Walk the log buffer from the current position to LOG_QUIT/EOF, counting
 * 20ms ticks. Saves and restores logPosition + XOR key. */
static uint32_t lv_walkComputeTotalTimeMs(void) {
  size_t   savedPos = lv_logGetCurrentPosition();
  BYTE     savedKey = lv_blocksGetKey();
  uint64_t ticks    = 0;
  bool     done     = FALSE;
  BYTE     code;
  BYTE     b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        ticks++;
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshot()) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  return (uint32_t)(ticks * 20);
}

/* Playback time (ms) of the world rewrite that ends the lobby — the round's
 * first real frame — computed once at load. 0 when the log has no lobby, so
 * the round runs from tick 0. */
static uint32_t s_gameStartMs = 0;

/* Every name each slot carried this round, collected once at load from the
 * log's own join events. Unlike the live roster this never forgets a player
 * who joined mid-round or quit before the end. */
static char s_slotNames[MAX_TANKS][PLAYER_NAME_LEN];

/* Presentation window: while set, the viewer reports times and seeks against
 * [gameStart, totalTime) instead of the whole file, so a log that sat hours in
 * the lobby reads on the round's own clock. Absolute ms is still what the
 * decoder, the snapshot index and the highlight clips use. */
static bool s_hideLobby = TRUE;

/* Event-stream start position (just past the opening snapshot), recorded at
 * load so later walks (the calibration anchor scan) can re-enter the stream
 * from a known-good position instead of trusting the caller's current one. */
static size_t s_walkStartPos = 0;

/* Walk the log from the current position to LOG_QUIT/EOF counting 20ms ticks,
 * and return the playback time in ms of the first snapshot whose body carries
 * a world — the rewrite the recorder emits when the lobby ends, which is the
 * round's first real frame. Returns 0 when the opening snapshot the loader
 * consumed already carried a world (no lobby ran, so the round starts at tick
 * 0 and every snapshot left in the stream is a periodic in-game resync), and
 * 0 if no such snapshot is reached. The tick accounting mirrors
 * lv_walkComputeTotalTimeMs, so the result is comparable to the decoder's
 * timeRunning; must be entered at the event-stream start (as at load). Saves
 * and restores logPosition + XOR key. */
static uint32_t lv_walkComputeGameStartMs(void) {
  size_t   savedPos;
  BYTE     savedKey;
  uint64_t ticks    = 0;
  bool     done     = FALSE;
  BYTE     code, b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;
  uint32_t result = 0;
  int      numRuns;

  if (s_openingSnapshotHadWorld == TRUE) return 0;

  savedPos = lv_logGetCurrentPosition();
  savedKey = lv_blocksGetKey();

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        numRuns = 0;
        if (!walkSkipSnapshotBases(NULL, NULL, NULL, &numRuns)) {
          done = TRUE;
          break;
        }
        ticks++;
        if (numRuns > 0) {
          result = (uint32_t)(ticks * 20);
          done = TRUE;
        }
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkSkipEvents(numEvents)) { done = TRUE; break; }
        ticks++;
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  return result;
}

/* Read the slot and name out of a log_PlayerJoined (5 opt bytes) or
 * log_ChangeName (1 opt byte) payload and record it in s_slotNames. Entered
 * with the reader just past the event's code byte — and, on v2, past the
 * framed length — and consumes exactly the bytes the matching
 * walkSkipEventBody case would. Last name wins, so a rename replaces the
 * earlier one and a reused slot ends up holding its most recent occupant.
 * Returns FALSE on a short read. */
static bool walkReadSlotName(BYTE code) {
  BYTE opt[5];
  int  optCount = (code == log_PlayerJoined) ? 5 : 1;
  char pstr[256];   /* [len][chars]; `pascal` is an MSVC keyword */
  char name[256];
  int  len;

  if (logReadBytes(opt, optCount) != optCount) return FALSE;
  if (logReadBytes((BYTE *)pstr, 1) != 1) return FALSE;
  len = (unsigned char)pstr[0];
  if (len > 0 && logReadBytes((BYTE *)pstr + 1, len) != len) return FALSE;
  lv_utilPtoCString(pstr, name);
  if (opt[0] < MAX_TANKS && name[0] != '\0') {
    snprintf(s_slotNames[opt[0]], sizeof(s_slotNames[opt[0]]), "%s", name);
  }
  return TRUE;
}

/* Read a log_GameSettings payload — a length byte then that many bytes — into
 * the settings store. Entered with the reader just past the framed length.
 * The last one in the file wins, and the walk keeps going, because a lobby
 * edit writes another event and no settings event follows the round start.
 * Returns FALSE on a short read. */
static bool walkReadGameSettings(void) {
  BYTE blob[LV_GAME_SETTINGS_MAX];
  int  len;

  if (logReadBytes(blob, 1) != 1) return FALSE;
  len = blob[0];
  if (len > 0 && logReadBytes(blob, len) != len) return FALSE;
  lv_screenStoreGameSettings(blob, len);
  return TRUE;
}

/* Like walkSkipEvents, but decodes the two events that carry a slot name and
 * the lobby settings; every other event is skipped by the shared helper. */
static bool walkScanNames(unsigned short numEvents) {
  unsigned short i;
  BYTE code;
  bool isV2 = (g_lv->loadedLogVersion == LOG_VERSION_V2);
  for (i = 0; i < numEvents; i++) {
    bool named;
    if (logReadBytes(&code, 1) != 1) return FALSE;
    named = (code == log_PlayerJoined || code == log_ChangeName);
    if (isV2) {
      /* v2: [type][u16 BE payload-length][payload]. Read what we need, then
         land on the framed end regardless; blockKey stays 0 so no key roll. */
      BYTE lenBytes[2];
      unsigned short evLen;
      size_t payloadPos;
      if (logReadBytes(lenBytes, 2) != 2) return FALSE;
      evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
      payloadPos = lv_logGetCurrentPosition();
      if (named && !walkReadSlotName(code)) return FALSE;
      /* Only the v2 arm looks for settings: the event postdates v0 and v1,
         so no file the arm below reads can contain one. */
      if (code == log_GameSettings && !walkReadGameSettings()) return FALSE;
      lv_logSetPosition(payloadPos + evLen);
    } else {
      if (named) {
        if (!walkReadSlotName(code)) return FALSE;
      } else if (walkSkipEventBody(code) < 0) {
        return FALSE;
      }
      lv_blocksSetKey(code);
    }
  }
  return TRUE;
}

/* Walk the log from the event-stream start to LOG_QUIT/EOF, recording the name
 * each join or rename event gives a slot. Mirrors lv_walkComputeGameStartMs;
 * must be entered at load, while the reader's XOR key still matches the stream
 * start. Saves and restores logPosition + XOR key. */
static void lv_walkCollectSlotNames(void) {
  size_t savedPos = lv_logGetCurrentPosition();
  BYTE   savedKey = lv_blocksGetKey();
  bool   done     = FALSE;
  BYTE   code, b1, b2;
  unsigned short numEvents;
  uint16_t us;

  lv_logSetPosition(s_walkStartPos);

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshot()) done = TRUE;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) done = TRUE;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1 || logReadBytes(&b2, 1) != 1) done = TRUE;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        if (!walkScanNames(numEvents)) done = TRUE;
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        if (!walkScanNames(numEvents)) done = TRUE;
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  /* The whole stream has been read: whatever settings the store holds now are
     the last the file carries, and playback must not put an earlier set back
     in their place. */
  s_gameSettingsWalked = TRUE;
}

/* Log time (ms) at which the round started (the lobby's world rewrite); 0 if
 * no lobby. */
uint32_t lv_screenGameStartMs(void) { return s_gameStartMs; }

static void lv_screenSeekToAbsoluteMs(uint32_t targetTime);

/* Start of the presented window in absolute log ms; 0 when the window is the
 * whole file (feature off, no lobby marker, degenerate log, or a live feed). */
static uint32_t lv_windowStartMs(void) {
  if (s_hideLobby == FALSE) return 0;
  if (lv_screenSpecIsLiveMode()) return 0;
  if (g_lv == NULL) return 0;
  if (s_gameStartMs == 0 || s_gameStartMs >= g_lv->totalTimeMs) return 0;
  return s_gameStartMs;
}

/* Length of the presented window in ms; 0 when nothing is loaded. */
static uint32_t lv_windowLenMs(void) {
  uint32_t start = lv_windowStartMs();
  if (g_lv == NULL || g_lv->totalTimeMs <= start) return 0;
  return g_lv->totalTimeMs - start;
}

/* With the lobby hidden, a freshly loaded log opens at game start so 00:00, the
 * first rendered frame and Play all agree. No-op when the window is the whole
 * file. */
static void lv_screenParkAtWindowStart(void) {
  uint32_t start = lv_windowStartMs();
  if (start > 0) {
    lv_screenSeekToAbsoluteMs(start);
  }
}

/* Scan one v2 LOG_EVENT frame for base-ownership gains, counting matches of
 * (cell, owner) for the two anchors and latching each anchor's time when its
 * ordinal is reached. Base positions are static per round, so the cell is
 * resolved from the base index via the caller-maintained table (parsed from
 * the log's own snapshots — the loaded lobby snapshot has no bases, so
 * g_lv->bs cannot be used here). */
static bool walkScanBaseOwners(unsigned short numEvents, uint32_t frameMs,
                               const uint8_t *baseX, const uint8_t *baseY,
                               int numBases,
                               uint8_t xE, uint8_t yE, uint8_t ownerE, int ordE,
                               uint8_t xL, uint8_t yL, uint8_t ownerL, int ordL,
                               int *cntE, uint32_t *msE, bool *haveE,
                               int *cntL, uint32_t *msL, bool *haveL) {
  unsigned short i;
  for (i = 0; i < numEvents; i++) {
    BYTE code, lenBytes[2];
    unsigned short evLen;
    size_t payloadStart;
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (logReadBytes(lenBytes, 2) != 2) return FALSE;
    evLen = (unsigned short)((lenBytes[0] << 8) | lenBytes[1]);
    payloadStart = lv_logGetCurrentPosition();
    if (code == log_BaseSetOwner && evLen >= 2) {
      BYTE idx, owner;
      if (logReadBytes(&idx, 1) == 1 && logReadBytes(&owner, 1) == 1 &&
          owner < MAX_TANKS && idx < numBases) {
        uint8_t bx = baseX[idx], by = baseY[idx];
        if (!*haveE && bx == xE && by == yE && owner == ownerE &&
            ++(*cntE) == ordE) { *msE = frameMs; *haveE = true; }
        if (!*haveL && bx == xL && by == yL && owner == ownerL &&
            ++(*cntL) == ordL) { *msL = frameMs; *haveL = true; }
      }
    }
    lv_logSetPosition(payloadStart + evLen);
  }
  return TRUE;
}

/* Calibration anchors: return the playback ms of two base-ownership gains,
 * each identified as the ordinal-th gain at cell (x,y) by `owner`. Pairs the
 * attribution track's first/last base captures to their real scrubber times so
 * the tick->ms line can be fitted. Ordinal + owner matching pins the exact
 * event: allied captures are recorded in the track too (ATTR_CAP_ALLY), so
 * before game over every owner<MAX_TANKS gain has a matching capture record —
 * but the game-over handover re-assigns every base to the winner with no
 * record, so "last gain at this cell" can be a later event than the track's
 * last capture (observed inflating the fitted slope ~10%). v2 logs only
 * (framed events); false otherwise or if either anchor is missing. Walks from
 * the recorded event-stream start; saves and restores position + key.
 *
 * Base index -> cell resolution comes from the log's own snapshots: the walk
 * is seeded from g_lv->bs (covers a no-lobby log whose only base table is the
 * opening snapshot, consumed before the stream start) and updated from every
 * snapshot it passes. A lobby-started log's opening snapshot has NO bases —
 * the game world doesn't exist yet — so the table only appears in the first
 * in-game snapshot, which the walk reaches before any capture event can. */
bool lv_walkFindBaseOwnerTimes(uint8_t xE, uint8_t yE, uint8_t ownerE, int ordE,
                               uint8_t xL, uint8_t yL, uint8_t ownerL, int ordL,
                               uint32_t *outMsE, uint32_t *outMsL) {
  size_t   savedPos;
  BYTE     savedKey;
  uint64_t ticks = 0;
  bool     done = FALSE, haveE = FALSE, haveL = FALSE;
  uint32_t msE = 0, msL = 0;
  int      cntE = 0, cntL = 0;
  BYTE     code, b1, b2;
  unsigned short waitLen, numEvents;
  uint16_t us;
  uint8_t  baseX[MAX_BASES], baseY[MAX_BASES];
  int      numBases = 0;
  int      i;

  if (ordE <= 0 || ordL <= 0) return FALSE;

  if (g_lv == NULL || g_lv->loadedLogVersion != LOG_VERSION_V2) return FALSE;
  savedPos = lv_logGetCurrentPosition();
  savedKey = lv_blocksGetKey();
  lv_logSetPosition(s_walkStartPos);
  lv_blocksSetKey(0);   /* v2 is plaintext (identity de-XOR) */

  /* Seed from the loaded base table (empty on a lobby-started log). */
  for (i = 0; i < (int)lv_basesGetNumBases(&g_lv->bs) && i < MAX_BASES; i++) {
    base bi;
    lv_basesGetBase(&g_lv->bs, &bi, (BYTE)(i + 1));
    baseX[i] = bi.x;
    baseY[i] = bi.y;
    numBases = i + 1;
  }

  while (!done && !lv_blocksIsEOF()) {
    if (logReadBytes(&code, 1) != 1) break;
    switch (code) {
      case LOG_QUIT:
        done = TRUE;
        break;
      case LOG_SNAPSHOT:
        if (!walkSkipSnapshotBases(baseX, baseY, &numBases, NULL)) { done = TRUE; break; }
        ticks++;
        break;
      case LOG_NOEVENTS:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        waitLen = b1 == 0 ? 1 : b1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_NOEVENTS_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        waitLen = ntohs(us);
        if (waitLen == 0) waitLen = 1;
        ticks += 1 + (uint64_t)waitLen;
        break;
      case LOG_EVENT:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        numEvents = b1;
        ticks++;
        if (!walkScanBaseOwners(numEvents, (uint32_t)(ticks * 20),
                                baseX, baseY, numBases,
                                xE, yE, ownerE, ordE, xL, yL, ownerL, ordL,
                                &cntE, &msE, &haveE,
                                &cntL, &msL, &haveL)) { done = TRUE; break; }
        break;
      case LOG_EVENT_LONG:
        if (logReadBytes(&b1, 1) != 1) { done = TRUE; break; }
        if (logReadBytes(&b2, 1) != 1) { done = TRUE; break; }
        us = (uint16_t)((b1 << 8) | b2);
        numEvents = ntohs(us);
        ticks++;
        if (!walkScanBaseOwners(numEvents, (uint32_t)(ticks * 20),
                                baseX, baseY, numBases,
                                xE, yE, ownerE, ordE, xL, yL, ownerL, ordL,
                                &cntE, &msE, &haveE,
                                &cntL, &msL, &haveL)) { done = TRUE; break; }
        break;
      default:
        done = TRUE;
        break;
    }
  }

  lv_logSetPosition(savedPos);
  lv_blocksSetKey(savedKey);
  if (haveE && haveL) { *outMsE = msE; *outMsL = msL; return TRUE; }
  return FALSE;
}

// Memory size in MB
bool lv_logLoad(char *fileName, int memoryBufferSize) {
  char id[LENGTH_ID+1]; /* The map ID Should read "BMAPBOLO" */
  BYTE dataLen;
  BYTE logVersion;    /* Version of the map file */
  bool returnValue = TRUE;
  int len;
  BYTE ip[4];

  lv_snapshotDestroy(&g_lv->snap);
  g_lv->snap = lv_snapshotCreate();
  g_lv->timeRunning = 0;
  memset(g_lv->kills,        0, sizeof(g_lv->kills));
  memset(g_lv->deaths,       0, sizeof(g_lv->deaths));
  memset(g_lv->gameViewHud,  0, sizeof(g_lv->gameViewHud));
  memset(g_lv->tankInv,      0, sizeof(g_lv->tankInv));

  returnValue = lv_blocksCreate(fileName, memoryBufferSize);
  if (returnValue == TRUE) {
    len = logReadBytes((BYTE *)id, LENGTH_ID);
    if (len != LENGTH_ID || strncmp(id,"WBOLOMOV", LENGTH_ID) != 0) {
      returnValue = FALSE;
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&logVersion, 1);
    if (len <= 0) {
      returnValue = FALSE;
    } else if (logVersion == LOG_VERSION_V0 || logVersion == LOG_VERSION_V1 ||
               logVersion == LOG_VERSION_V2) {
      g_lv->loadedLogVersion = logVersion;
    } else {
      returnValue = FALSE;
    }
  }

  /* Read map name */
  if (returnValue == TRUE) {
    logReadBytes(&dataLen, 1);
    /* An empty map name is valid (display-only field; the map data lives in the
       snapshot body). Only read+check when there are name bytes — a zero-length
       read returns -1, which would otherwise fail the load. */
    if (dataLen > 0) {
      len = logReadBytes((BYTE *)g_lv->mapName, dataLen);
      if (len != dataLen) {
        returnValue = FALSE;
      }
    }
    g_lv->mapName[dataLen] = '\0';
  }

  /* Read game type, mines, ai, password, max players */
  if (returnValue == TRUE) {
    logReadBytes(&g_lv->gt, 1);
    logReadBytes(&g_lv->allowHiddenMines, 1);
    logReadBytes(&g_lv->ai, 1);
    { BYTE tmp; logReadBytes(&tmp, 1); g_lv->usePassword = tmp; }
    logReadBytes(&g_lv->maxPlayers, 1);
    logReadBytes(&g_lv->versionMajor, 1);
    logReadBytes(&g_lv->versionMinor, 1);
    logReadBytes(&g_lv->versionRevision, 1);
    logReadBytes(ip, 4);
    snprintf(g_lv->serverIP, sizeof(g_lv->serverIP), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    logReadBytes((BYTE *) &g_lv->serverPort, sizeof(unsigned short));
    g_lv->serverPort = ntohs(g_lv->serverPort);
    logReadBytes((BYTE *) &g_lv->gmeCreateTime, sizeof(int32_t));
    g_lv->gmeCreateTime = ntohl(g_lv->gmeCreateTime);
    len = logReadBytes((BYTE *) &g_lv->wbnKey, 32);
    if (len != 32) {
      returnValue = FALSE;
    }
  }

  /* v2 is plaintext: blockKey stays 0 (identity de-XOR). v0/v1 seed the
     rolling key from the low byte of the game create time. */
  lv_blocksSetKey(g_lv->loadedLogVersion == LOG_VERSION_V2
                      ? 0
                      : (BYTE) (g_lv->gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = lv_processSnapshot();
    /* Latch whether the opening snapshot carried a world, before anything else
     * can decode another one. That is what tells lv_walkComputeGameStartMs
     * whether a lobby ran in front of the event stream. */
    s_openingSnapshotHadWorld = s_lastSnapshotHadWorld;
  }

  g_lv->logLoaded = returnValue;
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenLoadMap
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Loads a map. Returns if it was sucessful reading the
*  map or not.
*
*ARGUMENTS:
*********************************************************/
bool lv_screenLoadMap(char *fileName, int memoryBufferSize) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  lv_blocksDestroy();
  lv_screenDestroy();
  lv_screenSetup();
  /* Drop the previous log's names before anything can read them back. */
  memset(s_slotNames, 0, sizeof(s_slotNames));
  returnValue = lv_logLoad(fileName, memoryBufferSize);
  if (returnValue == TRUE) {
    /* Decompress entire log so we know total size for the seek slider */
    lv_logDecompressAll();
    /* Pre-scan to compute total game time. Byte position vs. time is
     * non-linear due to compression and variable event density, so the
     * one-shot walk is the only way to get accurate total/remaining
     * before a player joins. */
    s_walkStartPos = lv_logGetCurrentPosition();
    g_lv->totalTimeMs = lv_walkComputeTotalTimeMs();
    s_gameStartMs = lv_walkComputeGameStartMs();
    lv_walkCollectSlotNames();
    /* Set the game information up */
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, g_lv->versionMinor, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
    lv_screenParkAtWindowStart();
  }
  return returnValue;
}


/*********************************************************
*NAME:          lv_logLoadCommon
*PURPOSE:
*  Decodes the log header and opening snapshot from the
*  already-set-up blocks source. Resets the per-log g_lv
*  fields, reads the WBOLOMOV header, seeds the block key,
*  processes the opening snapshot, and records the result in
*  g_lv->logLoaded. The caller must have set up the blocks
*  source first (lv_blocksCreateFromMemory for a .wbv zip, or
*  lv_blocksBeginStream + lv_blocksAppendBytes for a stream).
*********************************************************/
static bool lv_logLoadCommon(void) {
  char id[LENGTH_ID+1];
  BYTE dataLen;
  BYTE logVersion;
  bool returnValue = TRUE;
  int len;
  BYTE ip[4];

  lv_snapshotDestroy(&g_lv->snap);
  g_lv->snap = lv_snapshotCreate();
  g_lv->timeRunning = 0;
  memset(g_lv->kills,        0, sizeof(g_lv->kills));
  memset(g_lv->deaths,       0, sizeof(g_lv->deaths));
  memset(g_lv->gameViewHud,  0, sizeof(g_lv->gameViewHud));
  memset(g_lv->tankInv,      0, sizeof(g_lv->tankInv));

  if (returnValue == TRUE) {
    len = logReadBytes((BYTE *)id, LENGTH_ID);
    if (len != LENGTH_ID || strncmp(id,"WBOLOMOV", LENGTH_ID) != 0) {
      returnValue = FALSE;
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&logVersion, 1);
    if (len <= 0) {
      returnValue = FALSE;
    } else if (logVersion == LOG_VERSION_V0 || logVersion == LOG_VERSION_V1 ||
               logVersion == LOG_VERSION_V2) {
      g_lv->loadedLogVersion = logVersion;
    } else {
      returnValue = FALSE;
    }
  }

  if (returnValue == TRUE) {
    logReadBytes(&dataLen, 1);
    /* An empty map name is valid (display-only field; the map data lives in the
       snapshot body). Only read+check when there are name bytes — a zero-length
       read returns -1, which would otherwise fail the load. */
    if (dataLen > 0) {
      len = logReadBytes((BYTE *)g_lv->mapName, dataLen);
      if (len != dataLen) {
        returnValue = FALSE;
      }
    }
    g_lv->mapName[dataLen] = '\0';
  }

  if (returnValue == TRUE) {
    logReadBytes(&g_lv->gt, 1);
    logReadBytes(&g_lv->allowHiddenMines, 1);
    logReadBytes(&g_lv->ai, 1);
    { BYTE tmp; logReadBytes(&tmp, 1); g_lv->usePassword = tmp; }
    logReadBytes(&g_lv->maxPlayers, 1);
    logReadBytes(&g_lv->versionMajor, 1);
    logReadBytes(&g_lv->versionMinor, 1);
    logReadBytes(&g_lv->versionRevision, 1);
    logReadBytes(ip, 4);
    snprintf(g_lv->serverIP, sizeof(g_lv->serverIP), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    logReadBytes((BYTE *) &g_lv->serverPort, sizeof(unsigned short));
    g_lv->serverPort = ntohs(g_lv->serverPort);
    logReadBytes((BYTE *) &g_lv->gmeCreateTime, sizeof(int32_t));
    g_lv->gmeCreateTime = ntohl(g_lv->gmeCreateTime);
    len = logReadBytes((BYTE *) &g_lv->wbnKey, 32);
    if (len != 32) {
      returnValue = FALSE;
    }
  }

  /* v2 is plaintext: blockKey stays 0 (identity de-XOR). v0/v1 seed the
     rolling key from the low byte of the game create time. */
  lv_blocksSetKey(g_lv->loadedLogVersion == LOG_VERSION_V2
                      ? 0
                      : (BYTE) (g_lv->gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = lv_processSnapshot();
    /* Latch whether the opening snapshot carried a world, before anything else
     * can decode another one. That is what tells lv_walkComputeGameStartMs
     * whether a lobby ran in front of the event stream. */
    s_openingSnapshotHadWorld = s_lastSnapshotHadWorld;
  }

  g_lv->logLoaded = returnValue;
  return returnValue;
}

/*********************************************************
*NAME:          lv_logLoadFromMemory
*PURPOSE:
*  Loads log data from an in-memory zip buffer.
*  Same as lv_logLoad but uses lv_blocksCreateFromMemory.
*  Takes ownership of zipData.
*********************************************************/
static bool lv_logLoadFromMemory(uint8_t *zipData, size_t zipLen) {
  if (lv_blocksCreateFromMemory(zipData, zipLen) != TRUE) {
    g_lv->logLoaded = FALSE;
    return FALSE;
  }
  return lv_logLoadCommon();
}

bool lv_screenLoadMapFromMemory(uint8_t *zipData, size_t zipLen) {
  bool returnValue;

  returnValue = FALSE;
  lv_blocksDestroy();
  lv_screenDestroy();
  lv_screenSetup();
  /* Drop the previous log's names before anything can read them back. */
  memset(s_slotNames, 0, sizeof(s_slotNames));
  returnValue = lv_logLoadFromMemory(zipData, zipLen);
  if (returnValue == TRUE) {
    lv_logDecompressAll();
    s_walkStartPos = lv_logGetCurrentPosition();
    g_lv->totalTimeMs = lv_walkComputeTotalTimeMs();
    s_gameStartMs = lv_walkComputeGameStartMs();
    lv_walkCollectSlotNames();
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, g_lv->versionMinor, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
    lv_screenParkAtWindowStart();
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenLoadFromStream
*PURPOSE:
*  Loads the decoder from a plaintext (v2) byte stream. The
*  caller supplies the initial header + opening snapshot bytes
*  here, then appends further records via lv_blocksAppendBytes
*  and steps lv_screenLogTick. Unlike the .wbv zip path there is
*  no fixed total size, so the decompress / total-time /
*  game-info steps are intentionally skipped.
*********************************************************/
bool lv_screenLoadFromStream(const uint8_t *bytes, size_t len) {
  bool ok;

  lv_blocksDestroy();
  lv_screenDestroy();
  lv_screenSetup();
  lv_blocksBeginStream();
  if (lv_blocksAppendBytes(bytes, len) != TRUE) {
    g_lv->logLoaded = FALSE;
    return FALSE;
  }
  ok = lv_logLoadCommon();
  if (ok == TRUE) {
    /* Publish the header's game information (map name / type / settings) to the
     * front end, as the file and in-memory loaders do — lv_logLoadCommon parses
     * it into g_lv but doesn't push it. This is the spectator seed-load path
     * (lv_specSeedLoad); standalone .wbv loads run through lv_screenLoadMap /
     * lv_screenLoadMapFromMemory and are unaffected. */
    /* The spectator seed's synthesized header carries no real version, so
     * lv_logLoadCommon parsed zeros into g_lv->version*. The spectator runs a
     * protocol compatible with the server, so the client's own build version is
     * the right thing to show — overwrite with it before publishing. */
    g_lv->versionMajor    = BOLO_VERSION_MAJOR;
    g_lv->versionMinor    = BOLO_VERSION_MINOR;
    g_lv->versionRevision = BOLO_VERSION_REVISION;
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, g_lv->versionMinor, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
  }
  return ok;
}

bool lv_screenIsPlaying() {
  return g_lv->isPlaying;
}

/*********************************************************
*NAME:          lv_screenStreamPump
*PURPOSE:
*  Feeds a live, append-only byte stream into the decoder.
*  Appends the caller-supplied newly-arrived bytes, then
*  advances playback over the whole records that are now
*  fully buffered, and parks cleanly when it catches up.
*
*  Appends are record-aligned (the ring and the record
*  translator emit complete records), so logPosition <
*  logSize means at least one complete record is present and
*  one reading tick lands exactly on the next record
*  boundary. When the cursor reaches logSize the decoder is
*  left untouched (state intact, still playing) rather than
*  ticked — a reading tick at the boundary would read a short
*  count and misalign the cursor, and a live stream carries no
*  LOG_QUIT, so "caught up" must never be treated as
*  end-of-log. The isPlaying guard stops the loop if playback
*  ever does finish so a non-advancing tick cannot spin.
*
*  Single-threaded and source-agnostic: the caller supplies
*  the bytes. Returns the decoder's isPlaying state.
*PARAMS:        bytes - newly-arrived stream bytes (may be NULL)
*               len   - number of bytes (may be 0 when caught up)
*RETURNS:       TRUE while playback is live, FALSE once finished.
*********************************************************/
/* --- Spectator live-DVR state (see backend.h / lv_screenSpec* below) --- */
static bool     s_specLiveMode    = false; /* spectatorRun active: append-only pump, host-driven advance */
static bool     s_specFollowLive  = true;  /* TRUE: slam to head; FALSE: parked, paced real-time playback */
static bool     s_specSeekPark    = false; /* a user seek/rewind happened -> park on the next frame update */
static bool     s_specHaveAnchor  = false;
static uint32_t s_specHeadTick    = 0;     /* latest drained forward-record game tick */
static uint32_t s_specAnchorTick  = 0;     /* head tick captured at the last follow-live re-anchor */
static uint32_t s_specAnchorMs    = 0;     /* timeRunning captured at the last follow-live re-anchor */
static uint32_t s_specPaceLastMs  = 0;     /* wall-clock anchor for the parked 20ms pacing accumulator */
static int32_t  s_specPaceAccumMs = 0;

bool lv_screenStreamPump(const uint8_t *bytes, size_t len) {
  if (g_lv == NULL) {
    return FALSE;
  }
  if (bytes != NULL && len > 0) {
    if (lv_blocksAppendBytes(bytes, len) != TRUE) {
      return g_lv->isPlaying;
    }
  }
  /* Live spectator mode appends only — the host advances the decoder itself via
     lv_screenSpecFrameUpdate so a parked view doesn't slam to the head. */
  while (g_lv->isPlaying == TRUE && !s_specLiveMode &&
         lv_logGetCurrentPosition() < lv_logGetTotalSize()) {
    lv_screenLogTick();
  }
  return g_lv->isPlaying;
}

/* Allocate a decoder state, set its field defaults, and register it as the
 * active state. Host-callable: lets a caller drive lv_screenLoadMapFromMemory
 * / lv_screenLogTick / lv_screenCloseLog without the standalone GUI/platform
 * scaffolding. Returns NULL on allocation failure. */
LogViewerState *lv_decoderCreate(bool fromMainMenu) {
  LogViewerState *lv = (LogViewerState *)calloc(1, sizeof(LogViewerState));
  if (lv == NULL) {
    return NULL;
  }
  lv->fromMainMenu = fromMainMenu;
  lv->screenSizeX = MAIN_SCREEN_SIZE_X + 15; /* default 30 */
  lv->screenSizeY = MAIN_SCREEN_SIZE_Y + 15; /* default 30 */
  lv->isLoaded = FALSE;
  lv->isSoundsPlaying = TRUE;
  lv->soundVolume = 50;

  /* Game-view skin state — calloc above already zeroed these, but be
   * explicit so the defaults are visible alongside the other init. */
  lv->gameView = FALSE;
  lv->cameraSlot = 0;
  lv->savedUseTeamColours = FALSE;
  memset(lv->kills, 0, sizeof(lv->kills));
  memset(lv->deaths, 0, sizeof(lv->deaths));
  memset(lv->gameViewHud, 0, sizeof(lv->gameViewHud));
  memset(lv->tankInv, 0, sizeof(lv->tankInv));

  lv_screenSetState(lv);
  return lv;
}

/* Close any loaded log (frees the zip buffer + screen structures), free the
 * decoder state, and clear the active state. NULL-safe. lv_screenCloseLog is
 * safe on a never-loaded state (lv_blocksDestroy and lv_screenDestroy both
 * no-op on the zeroed pointers), so it is called unconditionally. */
void lv_decoderDestroy(LogViewerState *lv) {
  if (lv == NULL) {
    return;
  }
  lv_screenCloseLog();
  free(lv);
  lv_screenSetState(NULL);
}

bool lv_screenCloseLog() {
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;
  lv_screenStoreGameSettings(NULL, 0);
  s_gameSettingsWalked = FALSE;

  lv_blocksDestroy();
  lv_screenDestroy();
  return TRUE;
}

void lv_screenSetOffset(BYTE x, BYTE y) {
  g_lv->xOffset = x;
  g_lv->yOffset = y;
}
BYTE lv_screenGetOffsetX() {
  return g_lv->xOffset;
}

BYTE lv_screenGetOffsetY() {
  return g_lv->yOffset;
}

BYTE lv_screenGetNumPills() {
  return lv_pillsGetNumPills(&g_lv->pb);
}

BYTE lv_screenGetNumBases() {
  return lv_basesGetNumBases(&g_lv->bs);
}

BYTE lv_screenGetNumStarts() {
  return lv_startsGetNumStarts(&g_lv->ss);
}


bool lv_screenSetStart(BYTE x, BYTE y) {
  BYTE num;
  start s;
  num = lv_startsGetNumStarts(&g_lv->ss);
  if (num >= MAX_STARTS) {
    return FALSE;
  }
  lv_basesDeleteBase(&g_lv->bs, x, y);
  lv_startsDeleteStart(&g_lv->ss, x, y);
  lv_pillsDeletePill(&g_lv->pb, x, y);

  lv_mapSetPos(&g_lv->mp, x, y, DEEP_SEA);
  s.x = x;
  s.y = y;
  s.dir = 0;
  num = lv_startsGetNumStarts(&g_lv->ss);
  lv_startsSetNumStarts(&g_lv->ss, (BYTE) (num+1));
  lv_startsSetStart(&g_lv->ss, &s, (BYTE) (num+1));
  return TRUE;
}

bool lv_screenSetPill(BYTE x, BYTE y) {
  BYTE num;
  pillbox s;
  num = lv_pillsGetNumPills(&g_lv->pb);
  if (num >= MAX_PILLS) {
    return FALSE;
  }
  lv_basesDeleteBase(&g_lv->bs, x, y);
  lv_startsDeleteStart(&g_lv->ss, x, y);
  lv_pillsDeletePill(&g_lv->pb, x, y);

  lv_mapSetPos(&g_lv->mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 15;
  s.speed = 0;
  s.inTank = FALSE;
  num = lv_pillsGetNumPills(&g_lv->pb);
  lv_pillsSetNumPills(&g_lv->pb, (BYTE) (num+1));
  lv_pillsSetPill(&g_lv->pb, &s, (BYTE) (num+1));
  return TRUE;

}

bool lv_screenSetBase(BYTE x, BYTE y) {
  BYTE num;
  base s;

  num = lv_basesGetNumBases(&g_lv->bs);
  if (num >= MAX_BASES) {
    return FALSE;
  }
  lv_basesDeleteBase(&g_lv->bs, x, y);
  lv_startsDeleteStart(&g_lv->ss, x, y);
  lv_pillsDeletePill(&g_lv->pb, x, y);

  lv_mapSetPos(&g_lv->mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 90;
  s.mines = 90;
  s.shells = 90;


  num = lv_basesGetNumBases(&g_lv->bs);
  lv_basesSetNumBases(&g_lv->bs, (BYTE) (num+1));
  lv_basesSetBase(&g_lv->bs, &s, (BYTE) (num+1));
  return TRUE;
}

/*********************************************************
*NAME:          lv_screenIsMine
*AUTHOR:        John Morrison
*CREATION DATE: 6/11/98
*LAST MODIFIED: 6/11/98
*PURPOSE:
*  Returns if a square on the screen should have a mine
*  drawn on it.
*  If value is out of range returns FALSE
*
*ARGUMENTS:
*  value  - Pointer to the screenMines structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
bool lv_screenIsMine(screenMines *value,BYTE xValue, BYTE yValue) {
  bool returnValue = FALSE; /* Value to return */

  /* Same +1 margin geometry as the screen tile buffer; see lv_screenGetPos. */
  if (xValue <= lv_screenGetSizeX() && yValue <= lv_screenGetSizeY()) {
    returnValue = *((*value)->mineItem+(yValue*(lv_screenGetSizeX()+1)+xValue));
  }
  return returnValue;
}

/*********************************************************
*NAME:          lv_screenNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of pillboxes
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_screenNumPills(void) {
  return lv_pillsGetNumPills(&g_lv->pb);
}

/*********************************************************
*NAME:          lv_screenNumBases
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of bases
*
*ARGUMENTS:
*
*********************************************************/
BYTE lv_screenNumBases(void) {
  return lv_basesGetNumBases(&g_lv->bs);
}


BYTE lv_screenGetSizeX() {
  return g_lv->screenSizeX;
}

BYTE lv_screenGetSizeY() {
  return g_lv->screenSizeY;
}

/* Forward declaration for draw.c function */
extern void lv_drawResizeRenderTarget(void);

void lv_screenSetSizeX(BYTE x) {
  g_lv->screenSizeX = x;
  if (g_lv->view != NULL) {
    BYTE *newItems = malloc((lv_screenGetSizeX()+2) * (lv_screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*g_lv->view).screenItem);
      (*g_lv->view).screenItem = newItems;
    }
  }
  if (g_lv->mineView != NULL) {
    bool *newItems = malloc((lv_screenGetSizeX()+1) * (lv_screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*g_lv->mineView).mineItem);
      (*g_lv->mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  lv_drawResizeRenderTarget();
}

void lv_screenSetSizeY(BYTE y) {
  g_lv->screenSizeY = y;
  if (g_lv->view != NULL) {
    BYTE *newItems = malloc((lv_screenGetSizeX()+2) * (lv_screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*g_lv->view).screenItem);
      (*g_lv->view).screenItem = newItems;
    }
  }
  if (g_lv->mineView != NULL) {
    bool *newItems = malloc((lv_screenGetSizeX()+1) * (lv_screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*g_lv->mineView).mineItem);
      (*g_lv->mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  lv_drawResizeRenderTarget();
}

void lv_screenGetOffsets(BYTE *x, BYTE *y) {
  if (x != NULL) {
    *x = g_lv->xOffset;
  }
  if (y != NULL) {
    *y = g_lv->yOffset;
  }
}

void lv_screenGetSubOffset(int *x, int *y) {
  if (x != NULL) *x = g_lv->subPxX;
  if (y != NULL) *y = g_lv->subPxY;
}

void lv_screenSetSubOffset(int x, int y) {
  g_lv->subPxX = x;
  g_lv->subPxY = y;
}

/* Decompose a total pan position (in zoom-1 native pixels) into
 * (xOffset,yOffset) tile + (subPxX,subPxY) pixel components, clamping
 * to the map. The whole-tile range matches the existing resize-time
 * clamp ([0, 255 - screenSize]); when the viewport reaches that edge
 * we force sub-pixel to zero so the trailing edge has no bleed past
 * the rendered tiles. */
void lv_screenPanToTotalPixels(int totalPxX, int totalPxY) {
  if (g_lv->logLoaded == FALSE) {
    return;
  }

  int sizeX = lv_screenGetSizeX();
  int sizeY = lv_screenGetSizeY();
  int maxOffX = 255 - sizeX; if (maxOffX < 0) maxOffX = 0;
  int maxOffY = 255 - sizeY; if (maxOffY < 0) maxOffY = 0;
  int maxPxX = maxOffX * TILE_SIZE_X;
  int maxPxY = maxOffY * TILE_SIZE_Y;

  if (totalPxX < 0) totalPxX = 0;
  if (totalPxY < 0) totalPxY = 0;
  if (totalPxX > maxPxX) totalPxX = maxPxX;
  if (totalPxY > maxPxY) totalPxY = maxPxY;

  int newOffX = totalPxX / TILE_SIZE_X;
  int newOffY = totalPxY / TILE_SIZE_Y;
  int newSubX = totalPxX - newOffX * TILE_SIZE_X;
  int newSubY = totalPxY - newOffY * TILE_SIZE_Y;

  bool wholeChanged = ((BYTE)newOffX != g_lv->xOffset) ||
                      ((BYTE)newOffY != g_lv->yOffset);
  bool subChanged   = (newSubX != g_lv->subPxX) || (newSubY != g_lv->subPxY);

  g_lv->subPxX = newSubX;
  g_lv->subPxY = newSubY;

  if (wholeChanged) {
    g_lv->xOffset = (BYTE)newOffX;
    g_lv->yOffset = (BYTE)newOffY;
  }

  /* Mutate the camera ONLY — never render here. Both hosts consume
   * wantScreenUpdate at the top of their next frame (lvEmbedFrameTexture for
   * the embedded reel, the logviewer.c main loop for the standalone app), and
   * that is the only place a render pairs coherently with the blit-offset
   * snapshot. Pan input runs AFTER the frame's image was already recorded, so
   * an inline lv_screenUpdate(redraw) on a whole-tile crossing repainted the
   * texture for the NEW camera while the frame presented it with the OLD
   * offsets — one displaced frame at every tile boundary of a drag, the
   * reel's "flickers every ~16 pixels of pan". Deferring to the flag renders
   * once, next frame, with matched offsets. Cheap: the redraw is
   * differential, so an unchanged grid blits nothing. */
  if (wholeChanged || subChanged) {
    g_lv->wantScreenUpdate = TRUE;
  }
}

void lv_messageAdd(messageType msgType, langid topId, langid bodyId,
                   const MessageArgs *args) {
  /* The events panel renders a single line per message — the channel
   * header (topId) is not displayed. Render the body via the lang
   * runtime so each viewer sees its own language; switching languages
   * mid-replay does not retranslate prior entries (acceptable per the
   * Phase 1 caveat — the events panel stores rendered strings). */
  char body[FILENAME_MAX];
  const char *rendered = langGetTextFmt(bodyId, args);
  body[0] = '\0';
  if (rendered) {
    snprintf(body, sizeof(body), "%s", rendered);
  }
  lv_windowAddEvent(0, body);

  /* Feed the scrolling-marquee queue used by the trailer game view.
   * Mirror the live game's clientMessageAdd: the channel header
   * (topId — e.g. "Newswire:") goes on the top line on the first
   * message of a run from a given source, and is blanked on
   * consecutive messages of the same source so successive entries
   * read as one continuing transcript. The queue is dormant when
   * the game view isn't running (no consumer); the cost is one
   * list-append per event. */
  {
    static messageType s_lastMessage = (messageType)-1;
    const char *topRendered =
        (s_lastMessage != msgType) ? langGetText(topId) : MESSAGE_EMPTY;
    s_lastMessage = msgType;
    char top[FILENAME_MAX];
    top[0] = '\0';
    if (topRendered) {
      snprintf(top, sizeof(top), "%s", topRendered);
    }
    lv_messageAddItem(top, body);
  }
}

/* Format an absolute log time as the displayed (window-relative) mm:ss. */
void lv_screenFormatTime(uint32_t absMs, char *dest, size_t destSize) {
  uint32_t start = lv_windowStartMs();
  uint32_t secs  = ((absMs > start) ? (absMs - start) : 0) / 1000u;
  snprintf(dest, destSize, "%02u:%02u", secs / 60u, secs % 60u);
}

void lv_screenGetTime(char *dest) {
  lv_screenFormatTime(g_lv->timeRunning, dest, 6);
}


void lv_screenMouseCentreClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;

  if (g_lv->logLoaded == FALSE) {
    return;
  }
  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);
  g_lv->xOffset = (g_lv->xOffset + xClick) - (g_lv->screenSizeX / 2);
  g_lv->yOffset = (g_lv->yOffset + yClick) - (g_lv->screenSizeY / 2);
  /* Defer to the flag — see lv_screenPanToTotalPixels. */
  g_lv->wantScreenUpdate = TRUE;
}

/* Centre the game view on a map cell, clamped so the offset stays in range
 * (xOffset/yOffset are unsigned tile indices). */
void lv_screenCentreOnCell(int mapX, int mapY) {
  int cx, cy;
  if (g_lv->logLoaded == FALSE) {
    return;
  }
  cx = mapX - (g_lv->screenSizeX / 2);
  cy = mapY - (g_lv->screenSizeY / 2);
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx > 255) cx = 255;
  if (cy > 255) cy = 255;
  g_lv->xOffset = (BYTE)cx;
  g_lv->yOffset = (BYTE)cy;
  /* Defer to the flag — see lv_screenPanToTotalPixels. */
  g_lv->wantScreenUpdate = TRUE;
}

/* Takes an absolute log time (highlight clip times are absolute) and clamps it
 * into the presented window. */
void lv_screenSeekToTimeMs(uint32_t ms) {
  uint32_t start = lv_windowStartMs();
  uint32_t len   = lv_windowLenMs();
  if (len == 0) return;
  if (ms < start) ms = start;
  if (ms > start + len) ms = start + len;
  lv_screenSeekToAbsoluteMs(ms);
}

void lv_screenMouseInformationClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;
  BYTE mapX, mapY;  /* Map coordinates */

  if (g_lv->logLoaded == FALSE) {
    return;
  }

  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);

  /* Calculate map coordinates */
  mapX = (BYTE) (g_lv->xOffset + xClick);
  mapY = (BYTE) (g_lv->yOffset + yClick);

  // Item info
  if (lv_pillsExistPos(&g_lv->pb, mapX, mapY) == TRUE) {
    g_lv->selectedItem = lv_pillsItemNumAt(&g_lv->pb, mapX, mapY);
    g_lv->selectedItemType = 2;
  } else if (lv_basesExistPos(&g_lv->bs, mapX, mapY) == TRUE) {
    g_lv->selectedItem = lv_basesItemNumAt(&g_lv->bs, mapX, mapY);
    g_lv->selectedItemType = 1;
  }


  if (g_lv->fastForwarding == FALSE) {
    if (g_lv->selectedItemType == 0) {
      lv_updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (g_lv->selectedItemType == 1) {
      lv_updateItem(1, g_lv->selectedItem, g_lv->bs->item[g_lv->selectedItem].owner, g_lv->bs->item[g_lv->selectedItem].x, g_lv->bs->item[g_lv->selectedItem].y, g_lv->bs->item[g_lv->selectedItem].armour, g_lv->bs->item[g_lv->selectedItem].shells, g_lv->bs->item[g_lv->selectedItem].mines, FALSE);
    } else {
      lv_updateItem(2, g_lv->selectedItem, g_lv->pb->item[g_lv->selectedItem].owner, g_lv->pb->item[g_lv->selectedItem].x, g_lv->pb->item[g_lv->selectedItem].y, g_lv->pb->item[g_lv->selectedItem].armour, 0, 0, g_lv->pb->item[g_lv->selectedItem].inTank);
    }
  }

}

void lv_screenMouseClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;

  if (g_lv->logLoaded == FALSE) {
    return;
  }
  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);

  // Who am I viewing?
  if (lv_playersChooseView(g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else if (lv_pillsChooseView(&g_lv->pb, g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else if (lv_basesChooseView(&g_lv->bs, g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else {
  }
}

void lv_screenFastForward() {
  bool available;

  if (g_lv->isPlaying == TRUE && g_lv->fastForwarding == FALSE) {
    g_lv->fastForwarding = TRUE;
    available = lv_screenLogTick();
    while (available == FALSE && g_lv->isPlaying == TRUE) {
      available = lv_screenLogTick();
    }
    g_lv->fastForwarding = FALSE;
  }
}

void lv_screenTankCentred(int enabled) {
  g_lv->centredTank = enabled;
}

void lv_screenSetHideLobby(int enabled) {
  bool want = enabled ? TRUE : FALSE;
  uint32_t start;
  if (want == s_hideLobby) return;
  s_hideLobby = want;
  /* Ticking it while the playhead sits in the lobby jumps to game start;
   * anywhere else, and on un-tick, nothing moves — only the scale relabels. */
  if (want == FALSE) return;
  if (g_lv == NULL || g_lv->logLoaded == FALSE) return;
  start = lv_windowStartMs();
  if (start > 0 && g_lv->timeRunning < start) {
    lv_screenSeekToAbsoluteMs(start);
  }
}

int lv_screenGetHideLobby(void) { return s_hideLobby ? 1 : 0; }

uint32_t lv_screenWindowStartMs(void) { return lv_windowStartMs(); }

void lv_screenRewind() {
  uint32_t currentTime = g_lv->timeRunning;
  size_t wantedPos;
  uint32_t firstTime;
  bool available;
  BYTE key;
  BYTE *pTeams = NULL;

  available = lv_snapshotBackwards(&g_lv->snap, &wantedPos, &currentTime, &key, &pTeams);
  if (available == TRUE) {
    if (currentTime > 1000 && (g_lv->timeRunning - currentTime) < 1000) {
      firstTime = currentTime;
      currentTime -= 1000;
      if (lv_snapshotBackwards(&g_lv->snap, &wantedPos, &currentTime, &key, &pTeams) == FALSE) {
        currentTime = firstTime;
      }
    }
    /* A rewind never steps back into the hidden lobby; it parks at game start. */
    if (currentTime < lv_windowStartMs()) {
      lv_screenSeekToAbsoluteMs(lv_windowStartMs());
      return;
    }
    g_lv->timeRunning = currentTime;
    lv_logSetPosition(wantedPos);
    lv_blocksSetKey(key);
    lv_playersSetTeams(pTeams);
    lv_processSnapshot();
    lv_windowRemoveEventsAfter(g_lv->timeRunning);
    g_lv->isPlaying = TRUE;
    g_lv->state = lv_lr_start;
    /* Rewinding parks the live-DVR view in the past (consumed next frame). */
    s_specSeekPark = true;
    if (wantedPos == 0) {
      lv_startOfLog();
    }
  }
}

void lv_screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime, uint32_t *totalTime) {
  uint32_t start = lv_windowStartMs();
  uint32_t len   = lv_windowLenMs();
  uint32_t cur   = g_lv->timeRunning;
  *currentPos = lv_logGetCurrentPosition();
  *totalSize  = lv_logGetTotalSize();
  cur = (cur > start) ? (cur - start) : 0;
  if (cur > len) cur = len;
  *currentTime = cur;
  *totalTime   = len;
}

void lv_screenSeekToPosition(float ratio) {
  uint32_t start = lv_windowStartMs();
  uint32_t len   = lv_windowLenMs();
  if (len == 0) return;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  lv_screenSeekToAbsoluteMs(start + (uint32_t)(ratio * (float)len));
}

/* Seek playback to an absolute log time: restore the newest snapshot at or
 * before it, then fast-forward the decoder to the target.
 *
 * A forward seek skips the restore entirely. The decoder is a sequential state
 * machine and its current state is already the replay of everything up to
 * timeRunning, so ticking on to a later target lands in exactly the state a
 * restore-and-replay would produce, for only the ticks in between. That
 * matters because a round carries essentially one snapshot, at its start: the
 * restore path re-decodes the whole round every time, at 20 ms of log per
 * tick, which is why scrubbing used to be affordable only once on release.
 * With this, dragging the recap's seek slider forward costs just the ticks the
 * handle crossed since the last frame. Backward seeks still rewind through the
 * snapshot — there is no way to un-tick — so callers throttle those. */
static void lv_screenSeekToAbsoluteMs(uint32_t targetTime) {
  size_t snapPos;
  uint32_t snapTime;
  BYTE key;
  BYTE *pTeams = NULL;

  if (targetTime >= g_lv->timeRunning && g_lv->logLoaded == TRUE) {
    /* Already there: nothing to decode, and no state to disturb. */
    if (targetTime == g_lv->timeRunning) {
      return;
    }
    g_lv->isPlaying = TRUE;
    /* A user scrub parks the live-DVR view in the past (consumed next frame),
       exactly as the restore path below does. */
    s_specSeekPark = true;
    g_lv->fastForwarding = TRUE;
    while (g_lv->timeRunning < targetTime && g_lv->isPlaying == TRUE) {
      lv_screenLogTick();
    }
    g_lv->fastForwarding = FALSE;
    /* Same reason as the restore path: drain what the fast-forward queued so
       the newswire is not still scrolling out pre-seek text afterwards. */
    lv_messageDrainQueue();
    return;
  }

  if (lv_snapshotFindByTime(&g_lv->snap, targetTime, &snapPos, &snapTime, &key, &pTeams)) {
    g_lv->timeRunning = snapTime;
    lv_logSetPosition(snapPos);
    lv_blocksSetKey(key);
    /* Restore team colours before processing snapshot so that new players
       appearing in the snapshot get a valid team assigned instead of being
       overwritten with NO_TEAM_SET from the pre-snapshot pTeams. */
    lv_playersSetTeams(pTeams);
    lv_processSnapshot();
    lv_windowRemoveEventsAfter(snapTime);
    g_lv->isPlaying = TRUE;
    g_lv->state = lv_lr_start;

    /* Reset the scrolling-newswire marquee. Both the pending queue and
     * the visible cells carry forward across a seek; without this the
     * marquee keeps scrolling out characters from messages emitted
     * before the seek long after we've jumped past their time. The
     * fast-forward below re-queues every event between the snapshot
     * and the target. */
    lv_messageDestroy();
    lv_messageCreate();

    /* A user scrub parks the live-DVR view in the past (consumed next frame). */
    s_specSeekPark = true;

    /* Fast-forward from snapshot to target time */
    g_lv->fastForwarding = TRUE;
    while (g_lv->timeRunning < targetTime && g_lv->isPlaying == TRUE) {
      lv_screenLogTick();
    }
    g_lv->fastForwarding = FALSE;

    /* Drain whatever the fast-forward queued straight into the visible
     * cells. Without this, the user would have to wait for tens of
     * seconds of accumulated text to scroll past at wall-clock pace
     * before fresh events show up. After the drain the visible row
     * holds the tail of the [snapshot..target] message stream — i.e.
     * the most recent message(s) at the seek point — and the queue is
     * empty so the next live message starts scrolling in normally. */
    lv_messageDrainQueue();
  }
}

/* --- Spectator live-DVR (declared in backend.h) ----------------------------
 * Head-time tracking (decision B, incremental, O(1) per record): following live
 * re-anchors the tracked head time to the decoder's true timeRunning after the
 * slam-to-head; while parked it extrapolates from the latest drained record's
 * game tick (20ms/tick) off that anchor. Any parked-time drift is wiped on the
 * next re-anchor (jump-to-live or reaching the head), so it only needs to be
 * monotonic and close. totalTimeMs is repointed at it so the existing
 * scrubber/seek math tracks the growing live head with no other change. */

static void lv_specHeadTimeFromDelta(void) {
  if (s_specHaveAnchor) {
    uint32_t d = (s_specHeadTick >= s_specAnchorTick)
                     ? (s_specHeadTick - s_specAnchorTick) : 0;
    g_lv->totalTimeMs = s_specAnchorMs + d * 20;
  }
}

static void lv_specReanchorAtHead(void) {
  s_specAnchorTick  = s_specHeadTick;
  s_specAnchorMs    = g_lv->timeRunning;
  s_specHaveAnchor  = true;
  g_lv->totalTimeMs = g_lv->timeRunning;
}

static void lv_specAdvanceToHead(void) {
  while (g_lv->isPlaying == TRUE &&
         lv_logGetCurrentPosition() < lv_logGetTotalSize()) {
    lv_screenLogTick();
  }
}

void lv_screenSpecSetLiveMode(bool on) {
  s_specLiveMode    = on;
  s_specFollowLive  = true;
  s_specSeekPark    = false;
  s_specHeadTick    = 0;
  s_specAnchorTick  = 0;
  s_specAnchorMs    = 0;
  s_specHaveAnchor  = false;
  s_specPaceAccumMs = 0;
  s_specPaceLastMs  = 0;
  /* A stream is not a round with a lobby in front of it; drop any game-start
     offset left behind by a file loaded earlier in this session, and the
     slot names that came with it — a feed has no file to scan for its own. */
  s_gameStartMs     = 0;
  memset(s_slotNames, 0, sizeof(s_slotNames));
  if (on) {
    /* The seed left the decoder at the head; play by default. The first
       follow-live frame re-anchors head time once a record tick is known. */
    g_lv->playIsPlaying = TRUE;
  }
}

bool lv_screenSpecIsLiveMode(void) {
  return s_specLiveMode;
}

void lv_screenSpecNoteHeadTick(uint32_t gameTick) {
  /* Monotonic within a segment. A world reset (new lobby/map) regresses the tick
     below the head; the host detects that and calls lv_screenSpecResetSegment,
     which zeroes the head so the new segment's first tick is captured here. */
  if (gameTick >= s_specHeadTick) {
    s_specHeadTick = gameTick;
  }
}

uint32_t lv_screenSpecHeadTick(void) {
  return s_specHeadTick;
}

/* Reset the DVR at a segment boundary (a world reset: new lobby/map). The live
   buffer and decoder are rebuilt by the re-seed the host runs straight after
   this; here we drop the previous segment's seek index so scroll-back cannot
   cross into the old map, and restart the head/anchor/follow state so head-time
   tracking resumes from the new segment's first tick and the view follows the
   new head. Live-mode only — standalone .wbv playback never calls this. */
void lv_screenSpecResetSegment(void) {
  if (!s_specLiveMode) {
    return;
  }
  lv_snapshotDestroy(&g_lv->snap);
  g_lv->snap        = lv_snapshotCreate();
  s_specFollowLive  = true;
  s_specSeekPark    = false;
  s_specHeadTick    = 0;
  s_specAnchorTick  = 0;
  s_specAnchorMs    = 0;
  s_specHaveAnchor  = false;
  s_specPaceAccumMs = 0;
  s_specPaceLastMs  = 0;
}

void lv_screenSpecJumpToLive(void) {
  if (!s_specLiveMode) {
    return;
  }
  lv_specAdvanceToHead();
  lv_specReanchorAtHead();
  s_specFollowLive = true;
  s_specSeekPark   = false;
}

void lv_screenSpecFrameUpdate(uint32_t nowMs) {
  bool playing;
  if (!s_specLiveMode) {
    return;
  }

  /* A scrubber drag or rewind parks the view in the past. */
  if (s_specSeekPark) {
    s_specSeekPark    = false;
    s_specFollowLive  = false;
    s_specPaceLastMs  = nowMs;
    s_specPaceAccumMs = 0;
  }

  playing = (g_lv->playIsPlaying == TRUE);

  if (s_specFollowLive) {
    if (playing) {
      lv_specAdvanceToHead();   /* slam to the live head */
      lv_specReanchorAtHead();  /* head time == true decoder time here */
    } else {
      lv_specHeadTimeFromDelta(); /* frozen; the head keeps growing */
    }
    s_specPaceLastMs = nowMs;
  } else {
    /* Parked: keep the slider's max tracking the growing head either way. */
    lv_specHeadTimeFromDelta();
    if (playing) {
      if (nowMs > s_specPaceLastMs) {
        s_specPaceAccumMs += (int32_t)(nowMs - s_specPaceLastMs);
      }
      s_specPaceLastMs = nowMs;
      if (s_specPaceAccumMs > 200) {
        s_specPaceAccumMs = 200; /* cap catch-up after a stall/hitch */
      }
      while (s_specPaceAccumMs >= 20 && g_lv->isPlaying == TRUE &&
             lv_logGetCurrentPosition() < lv_logGetTotalSize()) {
        lv_screenLogTick();
        s_specPaceAccumMs -= 20;
      }
      /* Paced playback reached the head -> resume following it. */
      if (lv_logGetCurrentPosition() >= lv_logGetTotalSize()) {
        lv_specReanchorAtHead();
        s_specFollowLive = true;
      }
    } else {
      s_specPaceLastMs = nowMs;
    }
  }
}

int32_t lv_screenGetGameTimeLeft() {
  return g_lv->gmeLength;
}

int32_t lv_screenGetGameStartDelay() {
  return g_lv->gmeStartDelay;
}


BYTE lv_screenGetNumPlayers() {
  return lv_playersGetNumPlayers();
}

void lv_screenGetPlayerName(char *name, BYTE playerNum, size_t destSize) {
  lv_playersGetPlayerName(playerNum, name, destSize);
}

bool lv_screenGetLoggedPlayerName(BYTE slot, char *dest, size_t destSize) {
  if (dest == NULL || destSize == 0) return FALSE;
  dest[0] = '\0';
  if (slot >= MAX_TANKS || s_slotNames[slot][0] == '\0') return FALSE;
  snprintf(dest, destSize, "%s", s_slotNames[slot]);
  return TRUE;
}

void lv_screenGetMapName(char *dest) {
  strncpy(dest, g_lv->mapName, sizeof(g_lv->mapName) - 1);
  dest[sizeof(g_lv->mapName) - 1] = '\0';
}

/*********************************************************
*NAME:          lv_screenSaveMap
*AUTHOR:        John Morrison
*CREATION DATE:  5/2/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
* Saves the map. Returns whether the operation was
* sucessful or not.
*
*ARGUMENTS:
*  fileName - path and filename to save
*  saveOwnerships - Do we save ownerships or not
*********************************************************/
bool lv_screenSaveMap(char *fileName, bool saveOwnerships) {
  return lv_mapWrite(fileName, &g_lv->mp, &g_lv->pb, &g_lv->bs, &g_lv->ss, saveOwnerships);
}


BYTE lv_screenGetPillTeam(BYTE x, BYTE y, BYTE *pillHealth) {
  pillbox p;
  BYTE itemNum = lv_pillsItemNumAt(&g_lv->pb,(BYTE) (x+g_lv->xOffset), (BYTE) (y+g_lv->yOffset));

  p.owner = NEUTRAL;
  p.armour = 15;
  lv_pillsGetPill(&g_lv->pb, &p, (BYTE) (itemNum+1));
 *pillHealth = p.armour;
  if (p.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return lv_playersGetTeamForOwner(p.owner);
}

BYTE lv_screenGetBaseTeam(BYTE x, BYTE y) {
  base b;
  BYTE itemNum = lv_basesItemNumAt(&g_lv->bs, (BYTE) (x+g_lv->xOffset), (BYTE) (y+g_lv->yOffset));

  b.owner = NEUTRAL;
  lv_basesGetBase(&g_lv->bs, &b, (BYTE) (itemNum+1));
  if (b.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return lv_playersGetTeamForOwner(b.owner);
}
