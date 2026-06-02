/*
 * $Id$
 *
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
*Name:          Screen
*Filename:      screen.c
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified:  4/11/98
*Purpose:
*  Provides Interfaces with the front end
*********************************************************/

/* Includes */
#include <math.h>
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

/* File-scope pointer to the central LogViewerState */
static LogViewerState *g_lv = NULL;

void lv_screenSetState(LogViewerState *lv) { g_lv = lv; }
LogViewerState *lv_screenGetState(void) { return g_lv; }

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE lv_screenGetXOffset(void) { return g_lv->xOffset; }
BYTE lv_screenGetYOffset(void) { return g_lv->yOffset; }
bool lv_screenGetFastForwarding(void) { return g_lv->fastForwarding; }
uint32_t lv_screenGetTimeRunning(void) { return g_lv->timeRunning; }

/* --- Inferred tank-inventory helpers (legacy logs only) -------------
 * Logs prior to the snapshot-tank-stats version don't carry per-tank
 * shells/mines/armour/trees. We approximate them by walking the event
 * stream:
 *   - log_PlayerJoined / log_PlayerRejoin / first log_PlayerLocation
 *     after death  -> spawn defaults from gameType
 *   - log_KillPlayer / log_PlayerDied / log_PlayerLeaving / Quit -> 0
 *   - log_SoundShoot at (mx,my)        -> tank at tile: shells--
 *   - log_SoundMineLay at (mx,my)      -> tank at tile: mines--
 *   - log_SoundHitTank at (mx,my)      -> tank at tile: armour -= DAMAGE
 *   - log_SoundMineExplode at (mx,my)  -> tank at tile: armour -= MINE_DAMAGE
 *   - log_BaseSetStock                 -> tank on base: receives delta
 *
 * Sound events lack player IDs, so adjacent tanks may steal each
 * other's deltas. Drift accumulates over the game; new logs will
 * carry authoritative values in snapshots and override these. */

static void inv_setSpawn(BYTE slot) {
  if (slot >= MAX_TANKS) return;
  /* Mirrors gameTypeGetItems() in src/bolo/gametype.c. The
   * gameTournament shell amount depends on the runtime ratio of
   * neutral bases to total bases at spawn time — use TANK_FULL_SHELLS
   * as the upper bound rather than try to reproduce that calculation
   * over a different base ownership tracker. */
  g_lv->tankInv[slot].armour = TANK_FULL_ARMOUR;
  switch (g_lv->gt) {
    case gameOpen:
      g_lv->tankInv[slot].shells = TANK_FULL_SHELLS;
      g_lv->tankInv[slot].mines  = TANK_FULL_MINES;
      g_lv->tankInv[slot].trees  = TANK_FULL_TREES;
      break;
    case gameTournament:
      g_lv->tankInv[slot].shells = TANK_FULL_SHELLS;
      g_lv->tankInv[slot].mines  = 0;
      g_lv->tankInv[slot].trees  = 0;
      break;
    case gameStrictTournament:
    default:
      g_lv->tankInv[slot].shells = 0;
      g_lv->tankInv[slot].mines  = 0;
      g_lv->tankInv[slot].trees  = 0;
      break;
  }
}

static void inv_zero(BYTE slot) {
  if (slot >= MAX_TANKS) return;
  g_lv->tankInv[slot].armour = 0;
  g_lv->tankInv[slot].shells = 0;
  g_lv->tankInv[slot].mines  = 0;
  g_lv->tankInv[slot].trees  = 0;
}

/* Subtract `amount` from `*field`, clamped at 0. */
static void inv_subClamp(BYTE *field, BYTE amount) {
  if (*field <= amount) *field = 0;
  else *field = (BYTE)(*field - amount);
}

/* Add `amount` to `*field`, clamped at `cap`. */
static void inv_addClamp(BYTE *field, BYTE amount, BYTE cap) {
  unsigned int sum = (unsigned int)(*field) + amount;
  if (sum > cap) sum = cap;
  *field = (BYTE)sum;
}

/* Find the first in-use tank whose mapX/mapY matches (mx,my). Returns
 * 0xFF if none. Tanks can't normally share a tile (collision), so the
 * first match is the only match in practice. */
static BYTE inv_findTankAtTile(BYTE mx, BYTE my) {
  BYTE i;
  for (i = 0; i < MAX_TANKS; i++) {
    if (!lv_playersIsInUse(i)) continue;
    if (!g_lv->gameViewHud[i].alive) continue;
    BYTE tmx, tmy, tpx, tpy, tframe;
    bool tOnBoat;
    lv_playersGetTankDetails(i, &tmx, &tmy, &tpx, &tpy, &tframe, &tOnBoat);
    if (tmx == mx && tmy == my) return i;
  }
  return 0xFF;
}

/* Find the in-use tank whose LGM is at (mx,my). Used for events that
 * fire at the LGM's tile (mine lay, farm, build) rather than the
 * tank's. Returns 0xFF if no LGM matches. */
static BYTE inv_findTankByLgmAtTile(BYTE mx, BYTE my) {
  BYTE i;
  for (i = 0; i < MAX_TANKS; i++) {
    if (!lv_playersIsInUse(i)) continue;
    BYTE lmx, lmy, lpx, lpy, lframe;
    lv_playersGetLgmDetails(i, &lmx, &lmy, &lpx, &lpy, &lframe);
    if (lmx == mx && lmy == my) return i;
  }
  return 0xFF;
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
  BYTE count;   /* Looping Variables */
  BYTE count2;
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
    logReadBytes(&code, 1);

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
        } else if (g_lv->loadedLogVersion == LOG_VERSION_V1) {
          /* Version 1: opt2-opt3 are 2-char country code,
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
        inv_setSpawn(opt1);
      }
      break;
    case log_PlayerQuit:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str);
      lv_playersLeaveGame(opt1, TRUE);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_QUIT_GAME, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        inv_zero(opt1);
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
      lv_playersGetPlayerName(opt1, str);
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
      lv_playersGetPlayerName(opt1, str);
      lv_playersGetPlayerName(opt2, mem);
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
      lv_playersGetPlayerName(opt1, str);
      lv_playersGetPlayerName(opt2, mem);
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
      lv_playersGetPlayerName(opt1, str);
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

      /* Inferred-inventory side effects. opt1=mx, opt2=my of the
       * sound source. log_SoundShoot fires at the shooter's tile
       * (tank or pill — pills don't share tiles with tanks so the
       * lookup safely returns nobody). log_SoundHitTank fires at the
       * victim's tile. log_SoundMineLay fires at the LGM's tile (the
       * server emits manLayingMineNear in lgm.c, not the tank's
       * position) — find the owning tank via the LGM tracker.
       * log_SoundMineExplode fires at the mine tile; if a tank is on
       * it, that tank took the damage. log_SoundFarm/Build aren't
       * wired (would inflow trees, but trees ride home with the LGM
       * before crediting the tank — too noisy to model here). */
      if (code == log_SoundShoot) {
        BYTE slot = inv_findTankAtTile(opt1, opt2);
        if (slot != 0xFF) inv_subClamp(&g_lv->tankInv[slot].shells, 1);
      } else if (code == log_SoundMineLay) {
        BYTE slot = inv_findTankByLgmAtTile(opt1, opt2);
        if (slot != 0xFF) inv_subClamp(&g_lv->tankInv[slot].mines, 1);
      } else if (code == log_SoundHitTank) {
        BYTE slot = inv_findTankAtTile(opt1, opt2);
        if (slot != 0xFF) inv_subClamp(&g_lv->tankInv[slot].armour, DAMAGE);
      } else if (code == log_SoundMineExplode) {
        BYTE slot = inv_findTankAtTile(opt1, opt2);
        if (slot != 0xFF) inv_subClamp(&g_lv->tankInv[slot].armour, MINE_DAMAGE);
      }
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
          inv_setSpawn(opt1);
        }
      }
      break;
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
      lv_playersGetPlayerName(opt1, name);
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
      lv_playersGetPlayerName(opt1, name);
      lv_playersGetPlayerName(opt2, name2);
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
      /* Refill inference: when a base's shells/mines/armour drops and a
       * tank is standing on the base tile, credit the delta to that
       * tank. Stock increases (server-side production) are ignored.
       * Suppressed on the first event per base so the initial value
       * isn't credited to whoever happens to be there. opt1 is the
       * 0-based base index in the log (server emits it post-decrement
       * — see basesSetBase / basesUpdateStock in src/bolo/bases.c).
       * lv_basesGetBase wants the 1-based number, hence opt1+1. */
      if (opt1 < MAX_BASES) {
        BYTE bsIdx = opt1;
        if (g_lv->prevBaseStockValid[bsIdx]) {
          base bi;
          lv_basesGetBase(&g_lv->bs, &bi, (BYTE)(opt1 + 1));
          BYTE slot = inv_findTankAtTile(bi.x, bi.y);
          if (slot != 0xFF) {
            if (opt2 < g_lv->prevBaseShells[bsIdx]) {
              inv_addClamp(&g_lv->tankInv[slot].shells,
                           (BYTE)(g_lv->prevBaseShells[bsIdx] - opt2),
                           TANK_FULL_SHELLS);
            }
            if (opt3 < g_lv->prevBaseMines[bsIdx]) {
              inv_addClamp(&g_lv->tankInv[slot].mines,
                           (BYTE)(g_lv->prevBaseMines[bsIdx] - opt3),
                           TANK_FULL_MINES);
            }
            if (opt4 < g_lv->prevBaseArmour[bsIdx]) {
              inv_addClamp(&g_lv->tankInv[slot].armour,
                           (BYTE)(g_lv->prevBaseArmour[bsIdx] - opt4),
                           TANK_FULL_ARMOUR);
            }
          }
        }
        g_lv->prevBaseShells[bsIdx] = opt2;
        g_lv->prevBaseMines[bsIdx]  = opt3;
        g_lv->prevBaseArmour[bsIdx] = opt4;
        g_lv->prevBaseStockValid[bsIdx] = true;
      }
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
    case log_KillPlayer:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, mem);
      if (opt1 == opt2 || opt2 == NEUTRAL) {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, mem);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_DIED, &args);
      } else {
        MessageArgs args = {0};
        lv_playersGetPlayerName(opt2, str);
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
        inv_zero(opt1);
      }
      break;
    case log_PlayerRejoin:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_REJOINED, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = true;
        g_lv->gameViewHud[opt1].respawnTimeMs = g_lv->timeRunning;
        inv_setSpawn(opt1);
      }
      break;
    case log_PlayerLeaving:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(newsWireMessage, MESSAGE_NEWSWIRE, STR_LV_PLAYER_LEAVING, &args);
      }
      if (opt1 < MAX_TANKS) {
        g_lv->gameViewHud[opt1].alive = false;
        inv_zero(opt1);
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
        inv_zero(opt1);
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
      lv_playersGetPlayerName(opt1, str);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_READY, &args);
      }
      break;
    case log_PlayerUnready:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str);
      {
        MessageArgs args = {0};
        snprintf(args.playerName, sizeof(args.playerName), "%.*s", (int)sizeof(args.playerName) - 1, str);
        lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_PLAYER_UNREADY, &args);
      }
      break;
    case log_TeamSet:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersGetPlayerName(opt1, str);
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
      lv_playersGetPlayerName(opt1, str);
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
    case log_BalanceApplied:
      lv_messageAdd(networkStatus, MESSAGE_NETSERVER, STR_LV_TEAM_BALANCE, NULL);
      break;
    case log_GameVoteStart:
      logReadBytes(&opt1, 1);  /* kind */
      logReadBytes(&opt2, 1);  /* initiator */
      logReadBytes(&opt3, 1);  /* team (0 = global) */
      lv_playersGetPlayerName(opt2, str);
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
      lv_playersGetPlayerName(opt2, str);
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
    default:
      lv_windowStop(TRUE);
      count = numEvents;
      break;

    }
    lv_blocksSetKey(code);
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
        BYTE x = lv_playersGetCentredX();
        BYTE y = lv_playersGetCentredY();
        if (x != 0 && y != 0 && x != 255 && y != 255) {
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
            g_lv->wantScreenUpdate = TRUE;
          }
        }
      }
    }
  }
  return returnValue;
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
      lv_screenUpdate(redraw);
    }
  }
}

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
    returnValue = lv_mapReadRuns(&g_lv->mp);
    if (returnValue == FALSE) {
      returnValue = FALSE;
    }
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
              lv_playersUpdateLgm(count, lgmmx, lgmmy, lgmpx, lgmpy,lgmframe);
            }
          }
        }
      }
    }
    count++;
  }

  /* Legacy snapshots don't carry tank inventory or per-base prev-stock
   * tracking. Best effort: assume any in-use tank in the snapshot is
   * at spawn defaults and invalidate the base-stock cache so the next
   * log_BaseSetStock re-anchors without crediting a bogus delta. New
   * log format will replace this with authoritative snapshot data. */
  for (BYTE slot = 0; slot < MAX_TANKS; slot++) {
    if (lv_playersIsInUse(slot)) {
      inv_setSpawn(slot);
    } else {
      inv_zero(slot);
    }
  }
  memset(g_lv->prevBaseStockValid, 0, sizeof(g_lv->prevBaseStockValid));

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
      { BYTE b[4]; if (logReadBytes(b, 4) != 4) return -1; }
      return 4;
    case log_PlayerLocation:
      { BYTE b[5]; if (logReadBytes(b, 5) != 5) return -1; }
      return 5;
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
      /* 1 opt byte + pascal string */
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
      /* 2 opt bytes + pascal string */
      { BYTE b[2]; if (logReadBytes(b, 2) != 2) return -1; }
      if (logReadBytes(&lenByte, 1) != 1) return -1;
      { BYTE buf[256]; rc = lenByte ? logReadBytes(buf, lenByte) : 0;
        if (rc != lenByte) return -1; }
      return 3 + lenByte;
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
  for (i = 0; i < numEvents; i++) {
    if (logReadBytes(&code, 1) != 1) return FALSE;
    if (walkSkipEventBody(code) < 0) return FALSE;
    lv_blocksSetKey(code);
  }
  return TRUE;
}

/* Skip a snapshot block. Mirrors lv_processSnapshot's read order without
 * applying any state. */
static bool walkSkipSnapshot(void) {
  BYTE buf[512];
  BYTE dlen;
  int32_t hdr;
  BYTE runHead[SIZEOFBMAP_RUN_HEADER];
  int i;

  /* gmeStartDelay + gmeLength */
  if (logReadBytes((BYTE *)&hdr, (int)sizeof(int32_t)) != (int)sizeof(int32_t)) return FALSE;
  if (logReadBytes((BYTE *)&hdr, (int)sizeof(int32_t)) != (int)sizeof(int32_t)) return FALSE;

  /* pills, bases, starts: 1-byte len + len bytes (BYTE max 255 fits in buf) */
  for (i = 0; i < 3; i++) {
    if (logReadBytes(&dlen, 1) != 1) return FALSE;
    if (dlen > 0 && logReadBytes(buf, dlen) != dlen) return FALSE;
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
  }

  /* MAX_TANKS player records: 1-byte len + len bytes (BYTE max 255 fits in buf) */
  for (i = 0; i < MAX_TANKS; i++) {
    if (logReadBytes(&dlen, 1) != 1) return FALSE;
    if (dlen > 0 && logReadBytes(buf, dlen) != dlen) return FALSE;
  }
  return TRUE;
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
  memset(g_lv->prevBaseStockValid, 0, sizeof(g_lv->prevBaseStockValid));

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
    } else if (logVersion == LOG_VERSION_V0 || logVersion == LOG_VERSION_V1) {
      g_lv->loadedLogVersion = logVersion;
    } else {
      returnValue = FALSE;
    }
  }

  /* Read map name */
  if (returnValue == TRUE) {
    logReadBytes(&dataLen, 1);
    len = logReadBytes((BYTE *)g_lv->mapName, dataLen);
    g_lv->mapName[dataLen] = '\0';
    if (len != dataLen) {
      returnValue = FALSE;
    }
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

  lv_blocksSetKey((BYTE) (g_lv->gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = lv_processSnapshot();
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
  returnValue = lv_logLoad(fileName, memoryBufferSize);
  if (returnValue == TRUE) {
    /* Decompress entire log so we know total size for the seek slider */
    lv_logDecompressAll();
    /* Pre-scan to compute total game time. Byte position vs. time is
     * non-linear due to compression and variable event density, so the
     * one-shot walk is the only way to get accurate total/remaining
     * before a player joins. */
    g_lv->totalTimeMs = lv_walkComputeTotalTimeMs();
    /* Set the game information up */
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, 1, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
  }
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
  memset(g_lv->prevBaseStockValid, 0, sizeof(g_lv->prevBaseStockValid));

  returnValue = lv_blocksCreateFromMemory(zipData, zipLen);
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
    } else if (logVersion == LOG_VERSION_V0 || logVersion == LOG_VERSION_V1) {
      g_lv->loadedLogVersion = logVersion;
    } else {
      returnValue = FALSE;
    }
  }

  if (returnValue == TRUE) {
    logReadBytes(&dataLen, 1);
    len = logReadBytes((BYTE *)g_lv->mapName, dataLen);
    g_lv->mapName[dataLen] = '\0';
    if (len != dataLen) {
      returnValue = FALSE;
    }
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

  lv_blocksSetKey((BYTE) (g_lv->gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = lv_processSnapshot();
  }

  g_lv->logLoaded = returnValue;
  return returnValue;
}

bool lv_screenLoadMapFromMemory(uint8_t *zipData, size_t zipLen) {
  bool returnValue;

  returnValue = FALSE;
  lv_blocksDestroy();
  lv_screenDestroy();
  lv_screenSetup();
  returnValue = lv_logLoadFromMemory(zipData, zipLen);
  if (returnValue == TRUE) {
    lv_logDecompressAll();
    g_lv->totalTimeMs = lv_walkComputeTotalTimeMs();
    lv_frontEndSetGameInformation(FALSE, g_lv->versionMajor, 1, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    lv_screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
  }
  return returnValue;
}

bool lv_screenIsPlaying() {
  return g_lv->isPlaying;
}

bool lv_screenCloseLog() {
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;

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

  g_lv->subPxX = newSubX;
  g_lv->subPxY = newSubY;

  if (wholeChanged) {
    g_lv->xOffset = (BYTE)newOffX;
    g_lv->yOffset = (BYTE)newOffY;
    lv_screenUpdate(redraw);
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

void lv_screenGetTime(char *dest) {
  double mins;
  double secs;

  secs = g_lv->timeRunning / 1000.00;
  mins = secs / 60.0;
  mins = floor(mins);
  secs = secs - (mins * 60.0);
  secs = floor(secs);

  snprintf(dest, 6, "%02d:%02d", (int) mins, (int) secs);
  if (strcmp(dest, "02:09") == FALSE) {
    mins = 0;
  }
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
  lv_screenUpdate(redraw);
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
    g_lv->timeRunning = currentTime;
    lv_logSetPosition(wantedPos);
    lv_blocksSetKey(key);
    lv_playersSetTeams(pTeams);
    lv_processSnapshot();
    lv_windowRemoveEventsAfter(g_lv->timeRunning);
    g_lv->isPlaying = TRUE;
    g_lv->state = lv_lr_start;
    if (wantedPos == 0) {
      lv_startOfLog();
    }
  }
}

void lv_screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime, uint32_t *totalTime) {
  *currentPos = lv_logGetCurrentPosition();
  *totalSize = lv_logGetTotalSize();
  *currentTime = g_lv->timeRunning;
  *totalTime = g_lv->totalTimeMs;
}

void lv_screenSeekToPosition(float ratio) {
  uint32_t totalTime = g_lv->totalTimeMs;
  uint32_t targetTime;
  size_t snapPos;
  uint32_t snapTime;
  BYTE key;
  BYTE *pTeams = NULL;

  if (totalTime == 0) return;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;

  targetTime = (uint32_t)(ratio * (float)totalTime);

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

int32_t lv_screenGetGameTimeLeft() {
  return g_lv->gmeLength;
}

int32_t lv_screenGetGameStartDelay() {
  return g_lv->gmeStartDelay;
}


BYTE lv_screenGetNumPlayers() {
  return lv_playersGetNumPlayers();
}

void lv_screenGetPlayerName(char *name, BYTE playerNum) {
  lv_playersGetPlayerName(playerNum, name);
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
