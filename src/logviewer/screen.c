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
#include "global.h"
#include "backend.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "screencalc.h"
#include "screentank.h"
#include "screenlgm.h"
#include "screenbullet.h"
#include "starts.h"
#include "util.h"
#include "shells.h"
#include "sounddist.h"
#include "players.h"
#include "snapshot.h"
#include "blocks.h"
#include "dns.h"
#include "logviewer.h"

/* File-scope pointer to the central LogViewerState */
static LogViewerState *g_lv = NULL;

void lv_screenSetState(LogViewerState *lv) { g_lv = lv; }
LogViewerState *lv_screenGetState(void) { return g_lv; }

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE lv_screenGetXOffset(void) { return g_lv->xOffset; }
BYTE lv_screenGetYOffset(void) { return g_lv->yOffset; }
bool lv_screenGetFastForwarding(void) { return g_lv->fastForwarding; }

// Some prototypes to cleanup and document

bool logIsEOF();

int logReadBytes(BYTE *buff, int len);

void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank);

bool lv_processSnapshot();
bool lv_logLoad(char *fileName, int memoryBufferSize);
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor, BYTE versionRevision, char *mapName, BYTE gameType, bool hiddenMines, BYTE aiType, int32_t startDelay, int32_t timeLimit, BYTE *wbnKey, int32_t startTime);
void lv_startOfLog();
void lv_windowRemoveEvents();

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

int a = (scrY*lv_screenGetSizeX())+scrX ;

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

  for (count=0;count < ssx; count++) {
    for (count2=0;count2 < ssy; count2++) {
      *((*g_lv->view).screenItem+(ssx*count2)+count) = lv_screenCalcSquare((BYTE) (count+g_lv->xOffset),(BYTE) (count2+g_lv->yOffset), count, count2);
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
  lv_playersMakeScreenLgm(&sl, g_lv->xOffset, (BYTE) (g_lv->xOffset + lv_screenGetSizeX()), g_lv->yOffset, (BYTE) (g_lv->yOffset + lv_screenGetSizeY()));
  lv_shellsCalcScreenBullets(&g_lv->shs, &sb, g_lv->xOffset, (BYTE) (g_lv->xOffset + lv_screenGetSizeX()), g_lv->yOffset, (BYTE) (g_lv->yOffset + lv_screenGetSizeY()));
  lv_playersMakeScreenTanks(&st, g_lv->xOffset, (BYTE) (g_lv->xOffset + lv_screenGetSizeX()), g_lv->yOffset, (BYTE) (g_lv->yOffset + lv_screenGetSizeY()));
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

  if (xValue < lv_screenGetSizeX() && yValue < lv_screenGetSizeY()) {
      returnValue = *((*g_lv->view).screenItem+(yValue*lv_screenGetSizeX()+xValue));
  }
  return returnValue;
}

#include "log.h"
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
      if (g_lv->loadedLogVersion == LOG_VERSION_V0) {
        /* Version 0: opt2-opt5 are IP address octets */
        snprintf(mem, sizeof(mem), "%d.%d.%d.%d", opt2, opt3, opt4, opt5);
        lv_dnsLookup(mem, str, sizeof(str));
        strncpy(mem, str, sizeof(mem) - 1);
        mem[sizeof(mem) - 1] = '\0';
      } else if (g_lv->loadedLogVersion == LOG_VERSION_V1) {
        /* Version 1: opt2-opt3 are 2-char country code, opt4-opt5 unused */
        snprintf(mem, sizeof(mem), "[%c%c]", opt2, opt3);
      }
      lv_playersSetPlayer(opt1, name, mem, 0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE, FALSE);
      break;
    case log_PlayerQuit:
      logReadBytes(&opt1, 1);
      lv_playersLeaveGame(opt1, TRUE);
      break;
    case log_LostMan:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str);
      strncat(str, " just lost his builder.", sizeof(str) - strlen(str) - 1);
      lv_windowAddEvent(0, str);
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
      strncat(str, " just requested alliance with ", sizeof(str) - strlen(str) - 1);
      strncat(str, mem, sizeof(str) - strlen(str) - 1);
      lv_windowAddEvent(0, str);
      break;
    case log_AllyAccept:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      lv_playersAcceptAlliance(opt1, opt2);
      lv_playersGetPlayerName(opt1, str);
      lv_playersGetPlayerName(opt2, mem);
      strncat(str, " just accepted alliance with ", sizeof(str) - strlen(str) - 1);
      strncat(str, mem, sizeof(str) - strlen(str) - 1);
      lv_windowAddEvent(0, str);
      break;
    case log_AllyLeave:
      logReadBytes(&opt1, 1);
      lv_playersLeaveAlliance(opt1);
      lv_playersGetPlayerName(opt1, str);
      strncat(str, " just left alliance", sizeof(str) - strlen(str) - 1);
      lv_windowAddEvent(0, str);
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
      snprintf(mem, sizeof(mem), "Message to all from %s: %s", name, str);
      lv_windowAddEvent(0, mem);
      break;
    case log_MessagePlayers:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      lv_playersGetPlayerName(opt1, name);
      lv_playersGetPlayerName(opt2, name2);
      snprintf(mem, sizeof(mem), "Message from %s to %s: %s", name, name2, str);
      lv_windowAddEvent(0, mem);
      break;
    case log_MessageServer:
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      lv_utilPtoCString(mem, str);
      snprintf(mem, sizeof(mem), "Server Message: %s", str);
      lv_windowAddEvent(0, mem);
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
      lv_playersGetPlayerName(opt2, str);
      strncat(str, " just killed player ", sizeof(str) - strlen(str) - 1);
      strncat(str, mem, sizeof(str) - strlen(str) - 1);
      lv_windowAddEvent(0, str);
      lv_playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      break;
    case log_PlayerRejoin:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str);
      snprintf(mem, sizeof(mem), "%s just rejoined game.", str);
      lv_windowAddEvent(0, mem);
      break;
    case log_PlayerLeaving:
      logReadBytes(&opt1, 1);
      lv_playersGetPlayerName(opt1, str);
      snprintf(mem, sizeof(mem), "%s is leaving game.", str);
      lv_windowAddEvent(0, mem);
      break;
    case log_PlayerDied:
      logReadBytes(&opt1, 1);
      lv_playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      break;
    case log_SaveMap:
      /* No-op — marker event with no visual effect on replay */
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
        lv_windowAddEvent(0, "End of Log File Reached");
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
        BYTE newXOffset;
        BYTE newYOffset;
        if (x != 0 && y != 0 && x != 255 && y != 255) {
          newXOffset = x - (g_lv->screenSizeX / 2);
          newYOffset = y - (g_lv->screenSizeY / 2);
          if (newXOffset != g_lv->xOffset || g_lv->yOffset != newYOffset) {
            g_lv->xOffset = newXOffset;
            g_lv->yOffset = newYOffset;
            lv_screenUpdate(redraw);
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
  BYTE newXOffset;
  BYTE newYOffset;
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
    newXOffset = x - (g_lv->screenSizeX / 2);
    newYOffset = y - (g_lv->screenSizeY / 2);
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
              lv_playersSetPlayer(count, name, location, mx ,my, px, py, frame, onBoat, numAllies, allies, FALSE, TRUE);
              lv_playersUpdateLgm(count, lgmmx, lgmmy, lgmpx, lgmpy,lgmframe);
            }
          }
        }
      }
    }
    count++;
  }

  return returnValue;
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

  if (xValue <= lv_screenGetSizeX() && yValue <= lv_screenGetSizeX()) {
    returnValue = *((*value)->mineItem+(yValue*lv_screenGetSizeX()+xValue));
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

void lv_screenPanToOffsets(BYTE newXOffset, BYTE newYOffset) {
  if (g_lv->logLoaded == FALSE) {
    return;
  }
  if (newXOffset == g_lv->xOffset && newYOffset == g_lv->yOffset) {
    return;
  }
  g_lv->xOffset = newXOffset;
  g_lv->yOffset = newYOffset;
  lv_screenUpdate(redraw);
}

void lv_messageAdd(char *messageStr) {
  lv_windowAddEvent(0, messageStr);
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
    lv_processSnapshot();
    lv_playersSetTeams(pTeams);
    lv_windowRemoveEvents();
    g_lv->isPlaying = TRUE;
    if (wantedPos == 0) {
      lv_startOfLog();
    }
  }
}

void lv_screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime) {
  *currentPos = lv_logGetCurrentPosition();
  *totalSize = lv_logGetTotalSize();
  *currentTime = g_lv->timeRunning;
}

void lv_screenSeekToPosition(float ratio) {
  size_t totalSize = lv_logGetTotalSize();
  size_t targetPos;
  size_t snapPos;
  uint32_t snapTime;
  BYTE key;
  BYTE *pTeams = NULL;

  if (totalSize == 0) return;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;

  targetPos = (size_t)(ratio * (float)totalSize);

  if (lv_snapshotFindByPosition(&g_lv->snap, targetPos, &snapPos, &snapTime, &key, &pTeams)) {
    g_lv->timeRunning = snapTime;
    lv_logSetPosition(snapPos);
    lv_blocksSetKey(key);
    lv_processSnapshot();
    lv_playersSetTeams(pTeams);
    lv_windowRemoveEvents();
    g_lv->isPlaying = TRUE;
    g_lv->state = lv_lr_start;

    /* Fast-forward from snapshot to target position */
    g_lv->fastForwarding = TRUE;
    while (lv_logGetCurrentPosition() < targetPos && g_lv->isPlaying == TRUE) {
      lv_screenLogTick();
    }
    g_lv->fastForwarding = FALSE;
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
