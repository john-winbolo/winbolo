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

void screenSetState(LogViewerState *lv) { g_lv = lv; }
LogViewerState *screenGetState(void) { return g_lv; }

/* Accessor functions for sounddist.c (replaces extern globals) */
BYTE screenGetXOffset(void) { return g_lv->xOffset; }
BYTE screenGetYOffset(void) { return g_lv->yOffset; }
bool screenGetFastForwarding(void) { return g_lv->fastForwarding; }

// Some prototypes to cleanup and document

bool logIsEOF();

int logReadBytes(BYTE *buff, int len);

void updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank);

bool processSnapshot();
bool logLoad(char *fileName, int memoryBufferSize);
void frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor, BYTE versionRevision, char *mapName, BYTE gameType, bool hiddenMines, BYTE aiType, int32_t startDelay, int32_t timeLimit, BYTE *wbnKey, int32_t startTime);
void startOfLog();
void windowRemoveEvents();

/*********************************************************
*NAME:          screenCalcSquare
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
BYTE screenCalcSquare(BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY) {
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

int a = (scrY*screenGetSizeX())+scrX ;

  if (a > 1989) {
    above = 1;
  }
  *((*g_lv->mineView).mineItem+a) = FALSE;
  /* Set up Items */
  if ((pillsExistPos(&g_lv->pb,xValue,yValue)) == TRUE) {
    returnValue = pillsGetScreenHealth(&g_lv->pb, xValue, yValue);
  } else if ((basesExistPos(&g_lv->bs,xValue,yValue)) == TRUE) {
     ba = basesGetAlliancePos(&g_lv->bs, xValue, yValue);
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
      if (basesAmOwner(&g_lv->bs, playersGetSelf(), xValue, yValue) == TRUE) {
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
    currentPos = mapGetPos(&g_lv->mp,xValue,yValue);
    if (mapIsMine(&g_lv->mp, xValue, yValue) == TRUE) {
      *((*g_lv->mineView).mineItem+a) = TRUE;
      if (currentPos != DEEP_SEA) {
        currentPos = currentPos - MINE_SUBTRACT;
      }
    } else {
      *((*g_lv->mineView).mineItem+a) = FALSE;
    }

    if (basesExistPos(&g_lv->bs, (BYTE) (xValue-1), (BYTE) (yValue-1)) == TRUE) {
      aboveLeft = ROAD;
    } else {
      aboveLeft = mapGetPos(&g_lv->mp,(BYTE) (xValue-1),(BYTE) (yValue-1));
      if (aboveLeft >= MINE_START && aboveLeft <= MINE_END) {
        aboveLeft = aboveLeft - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&g_lv->bs, xValue, (BYTE) (yValue-1)) == TRUE) {
      above = ROAD;
    } else {
      above = mapGetPos(&g_lv->mp,xValue,(BYTE) (yValue-1));
      if (above >= MINE_START && above <= MINE_END) {
        above = above - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&g_lv->bs, (BYTE) (xValue+1), (BYTE) (yValue-1)) == TRUE) {
      aboveRight = ROAD;
    } else {
      aboveRight = mapGetPos(&g_lv->mp,(BYTE) (xValue+1),(BYTE) (yValue-1));
      if (aboveRight >= MINE_START && aboveRight <= MINE_END) {
        aboveRight = aboveRight - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&g_lv->bs, (BYTE) (xValue-1), yValue) == TRUE) {
      leftPos = ROAD;
    } else {
      leftPos = mapGetPos(&g_lv->mp,(BYTE) (xValue-1),yValue);
      if (leftPos >= MINE_START && leftPos <= MINE_END) {
        leftPos = leftPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&g_lv->bs, (BYTE) (xValue+1), yValue) == TRUE) {
      rightPos = ROAD;
    } else {
      rightPos = mapGetPos(&g_lv->mp,(BYTE) (xValue+1),yValue);
      if (rightPos >= MINE_START && rightPos <= MINE_END) {
        rightPos = rightPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&g_lv->bs, (BYTE) (xValue-1), (BYTE) (yValue+1)) == TRUE) {
      belowLeft = ROAD;
    } else {
      belowLeft = mapGetPos(&g_lv->mp,(BYTE) (xValue-1),(BYTE) (yValue+1));
      if (belowLeft >= MINE_START && belowLeft <= MINE_END) {
        belowLeft = belowLeft - MINE_SUBTRACT;
      }
    }


    if (basesExistPos(&g_lv->bs, xValue, (BYTE) (yValue+1)) == TRUE) {
      below = ROAD;
    } else {
      below = mapGetPos(&g_lv->mp,xValue,(BYTE) (yValue+1));
      if (below >= MINE_START && below <= MINE_END) {
        below = below - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&g_lv->bs, (BYTE) (xValue+1), (BYTE) (yValue+1)) == TRUE) {
      belowRight = ROAD;
    } else {
      belowRight = mapGetPos(&g_lv->mp,(BYTE) (xValue+1),(BYTE) (yValue+1));
      if (belowRight >= MINE_START && belowRight <= MINE_END) {
        belowRight = belowRight - MINE_SUBTRACT;
      }
    }

    switch (currentPos) {
    case ROAD:
      returnValue = screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BUILDING:
      returnValue = screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case FOREST:
      returnValue = screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case RIVER:
      returnValue = screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case DEEP_SEA:
      returnValue = screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    case BOAT:
      returnValue = screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    default:
      returnValue = currentPos;
      break;
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          screenUpdateView
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Updates the values in the view area
*
*ARGUMENTS:
* value - The update type (Helps in optimisations)
*********************************************************/
void screenUpdateView(updateType value) {
  BYTE count;   /* Looping Variables */
  BYTE count2;
  int ssx = screenGetSizeX();
  int ssy = screenGetSizeY();

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
      *((*g_lv->view).screenItem+(ssx*count2)+count) = screenCalcSquare((BYTE) (count+g_lv->xOffset),(BYTE) (count2+g_lv->yOffset), count, count2);
    }
  }
}



/*********************************************************
*NAME:          screenSetup
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
void screenSetup() {
  int a = 0;
  g_lv->gmeStartDelay = 0;
  g_lv->gmeLength = UNLIMITED_GAME_TIME;
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;
  g_lv->xOffset = 127;
  g_lv->yOffset = 127;
  mapCreate(&g_lv->mp);
  pillsCreate(&g_lv->pb);
  startsCreate(&g_lv->ss);
  basesCreate(&g_lv->bs);
  playersCreate();
  playersSetSelf(0);
  g_lv->shs = shellsCreate();
  if (g_lv->view != NULL) {
    free((*g_lv->view).screenItem);
    Dispose(g_lv->view);
  }
  New(g_lv->view);
  if (g_lv->view != NULL) {
    (*g_lv->view).screenItem = malloc((screenGetSizeX()+2) * (screenGetSizeY()+2));
  }
  if (g_lv->mineView != NULL) {
    free((*g_lv->mineView).mineItem);
    free(g_lv->mineView);
  }
  a = (screenGetSizeX()+1) * (screenGetSizeY()+1);
  New(g_lv->mineView);
  if (g_lv->mineView != NULL) {
    (*g_lv->mineView).mineItem = malloc(a * sizeof(bool));
  }

  screenUpdateView(redraw);
}

/*********************************************************
*NAME:          screenDestroy
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
void screenDestroy() {
  if (g_lv->mp != NULL) {
    mapDestroy(&g_lv->mp);
    g_lv->mp = NULL;
  }
  if (g_lv->pb != NULL) {
    pillsDestroy(&g_lv->pb);
    g_lv->pb = NULL;
  }
  if (g_lv->ss != NULL) {
    startsDestroy(&g_lv->ss);
    g_lv->ss = NULL;
  }
  if (g_lv->bs != NULL) {
    basesDestroy(&g_lv->bs);
    g_lv->bs = NULL;
  }
  playersDestroy();
  if (g_lv->shs != NULL) {
    shellsDestroy(&g_lv->shs);
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

void frontEndDrawMainScreen(screen *value, screenMines *mineView, screenTanks *tks, screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms, int32_t srtDelay, bool isPillView, int edgeX, int edgeY);


/*********************************************************
*NAME:          screenUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Updates the screen. Takes numerous directions
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void screenUpdate(updateType value) {
  screenTanks st;
  screenBullets sb;
  screenLgm sl;


  screenLgmCreate(&sl);
  sb = screenBulletsCreate();
  screenTanksCreate(&st);

  if (g_lv->logLoaded == FALSE) {
    return;
  }

  screenUpdateView(value);
  playersMakeScreenLgm(&sl, g_lv->xOffset, (BYTE) (g_lv->xOffset + screenGetSizeX()), g_lv->yOffset, (BYTE) (g_lv->yOffset + screenGetSizeY()));
  shellsCalcScreenBullets(&g_lv->shs, &sb, g_lv->xOffset, (BYTE) (g_lv->xOffset + screenGetSizeX()), g_lv->yOffset, (BYTE) (g_lv->yOffset + screenGetSizeY()));
  playersMakeScreenTanks(&st, g_lv->xOffset, (BYTE) (g_lv->xOffset + screenGetSizeX()), g_lv->yOffset, (BYTE) (g_lv->yOffset + screenGetSizeY()));
  frontEndDrawMainScreen(&g_lv->view, &g_lv->mineView, &st, NULL, &sb, &sl, 0, FALSE, 0, 0);
  screenTanksDestroy(&st);
  screenBulletsDestroy(&sb);
  screenLgmDestroy(&sl);
}

/*********************************************************
*NAME:          screenSetPos
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
void screenSetPos(BYTE xValue, BYTE yValue, BYTE terrain) {
  mapSetPos(&g_lv->mp, xValue, yValue, terrain);
  basesDeleteBase(&g_lv->bs, xValue, yValue);
  startsDeleteStart(&g_lv->ss, xValue, yValue);
  pillsDeletePill(&g_lv->pb, xValue, yValue);
}

/*********************************************************
*NAME:          screenGetPos
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
BYTE screenGetPos(screen *value,BYTE xValue, BYTE yValue) {
  BYTE returnValue = DEEP_SEA; /* Value to return */

  if (xValue < screenGetSizeX() && yValue < screenGetSizeY()) {
      returnValue = *((*g_lv->view).screenItem+(yValue*screenGetSizeX()+xValue));
  }
  return returnValue;
}

#include "log.h"
void windowAddEvent(int eventType, char *msg);


void screenProcessLog(unsigned short numEvents) {
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
      utilPtoCString(mem, name);
      if (g_lv->loadedLogVersion == LOG_VERSION_V0) {
        /* Version 0: opt2-opt5 are IP address octets */
        snprintf(mem, sizeof(mem), "%d.%d.%d.%d", opt2, opt3, opt4, opt5);
        dnsLookup(mem, str, sizeof(str));
        strncpy(mem, str, sizeof(mem) - 1);
        mem[sizeof(mem) - 1] = '\0';
      } else if (g_lv->loadedLogVersion == LOG_VERSION_V1) {
        /* Version 1: opt2-opt3 are 2-char country code, opt4-opt5 unused */
        snprintf(mem, sizeof(mem), "[%c%c]", opt2, opt3);
      }
      playersSetPlayer(opt1, name, mem, 0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE, FALSE);
      break;
    case log_PlayerQuit:
      logReadBytes(&opt1, 1);
      playersLeaveGame(opt1, TRUE);
      break;
    case log_LostMan:
      logReadBytes(&opt1, 1);
      playersGetPlayerName(opt1, str);
      strncat(str, " just lost his builder.", sizeof(str) - strlen(str) - 1);
      windowAddEvent(0, str);
      break;
    case log_MapChange:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      mapSetPos(&g_lv->mp, opt1, opt2, opt3);
      break;
    case log_ChangeName:
      logReadBytes(&opt1, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)mem+1, (unsigned char)mem[0]);
      utilPtoCString(mem, str);
      playersSetPlayerName(opt1, str);
      break;
    case log_AllyRequest:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      playersGetPlayerName(opt1, str);
      playersGetPlayerName(opt2, mem);
      strncat(str, " just requested alliance with ", sizeof(str) - strlen(str) - 1);
      strncat(str, mem, sizeof(str) - strlen(str) - 1);
      windowAddEvent(0, str);
      break;
    case log_AllyAccept:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      playersAcceptAlliance(opt1, opt2);
      playersGetPlayerName(opt1, str);
      playersGetPlayerName(opt2, mem);
      strncat(str, " just accepted alliance with ", sizeof(str) - strlen(str) - 1);
      strncat(str, mem, sizeof(str) - strlen(str) - 1);
      windowAddEvent(0, str);
      break;
    case log_AllyLeave:
      logReadBytes(&opt1, 1);
      playersLeaveAlliance(opt1);
      playersGetPlayerName(opt1, str);
      strncat(str, " just left alliance", sizeof(str) - strlen(str) - 1);
      windowAddEvent(0, str);
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
      soundDist(opt3, opt1, opt2);
      break;
    case log_PlayerLocation:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      logReadBytes(&opt5, 1);
      utilGetNibbles(opt4, &px, &py);
      utilGetNibbles(opt5, &frame, &onBoat);
      if (opt2 != 0) {
        playersUpdateTank(opt1, opt2, opt3, px, py, frame, onBoat);
      }
      break;
    case log_Shell:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      utilGetNibbles(opt3, &px, &py);
      shellsAddItem(&g_lv->shs, opt1, opt2, px, py, opt4);
      break;
    case log_LgmLocation:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      utilGetNibbles(opt1, &onBoat, &frame);
      utilGetNibbles(opt4, &px, &py);
      playersUpdateLgm(onBoat, opt2, opt3, px, py, frame);
      break;
    case log_MessageAll:
      logReadBytes(&opt1, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      utilPtoCString(mem, str);
      playersGetPlayerName(opt1, name);
      snprintf(mem, sizeof(mem), "Message to all from %s: %s", name, str);
      windowAddEvent(0, mem);
      break;
    case log_MessagePlayers:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      utilPtoCString(mem, str);
      playersGetPlayerName(opt1, name);
      playersGetPlayerName(opt2, name2);
      snprintf(mem, sizeof(mem), "Message from %s to %s: %s", name, name2, str);
      windowAddEvent(0, mem);
      break;
    case log_MessageServer:
      logReadBytes((BYTE *)mem, 1);
      logReadBytes((BYTE *)(mem+1), (unsigned char)mem[0]);
      utilPtoCString(mem, str);
      snprintf(mem, sizeof(mem), "Server Message: %s", str);
      windowAddEvent(0, mem);
      break;
    case log_BaseSetOwner:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      basesSetOwner(&g_lv->bs, opt1, opt2, opt3);
      break;
    case log_BaseSetStock:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      basesSetStock(&g_lv->bs, opt1, opt2, opt3, opt4);
      break;
    case log_PillSetOwner:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      pillsSetPillOwner(&g_lv->pb, opt1, opt2, opt3);
      break;
    case log_PillSetPlace:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      pillsSetPos(&g_lv->pb, opt1, opt2, opt3);
      break;
    case log_PillSetHealth:
      logReadBytes(&opt1, 1);
      utilGetNibbles(opt1, &opt2, &opt3);
      pillsSetHealth(&g_lv->pb, opt2, opt3);
      break;
    case log_PillSetInTank:
      logReadBytes(&opt1, 1);
      utilGetNibbles(opt1, &opt2, &opt3);
      pillsSetInTank(&g_lv->pb, opt2, opt3);
      break;
    case log_KillPlayer:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      playersGetPlayerName(opt1, mem);
      playersGetPlayerName(opt2, str);
      strncat(str, " just killed player ", sizeof(str) - strlen(str) - 1);
      strncat(str, mem, sizeof(str) - strlen(str) - 1);
      windowAddEvent(0, str);
      playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      break;
    case log_PlayerRejoin:
      logReadBytes(&opt1, 1);
      playersGetPlayerName(opt1, str);
      snprintf(mem, sizeof(mem), "%s just rejoined game.", str);
      windowAddEvent(0, mem);
      break;
    case log_PlayerLeaving:
      logReadBytes(&opt1, 1);
      playersGetPlayerName(opt1, str);
      snprintf(mem, sizeof(mem), "%s is leaving game.", str);
      windowAddEvent(0, mem);
      break;
    case log_PlayerDied:
      logReadBytes(&opt1, 1);
      playersUpdateTank(opt1, 0, 0, 0, 0, 0, TRUE);
      break;
    default:
      windowStop(TRUE);
      count = numEvents;
      break;

    }
    blocksSetKey(code);
    count++;
  }
}

void screenRequestUpdate() {
  /* Finally lets update our item in the frontend */
  if (g_lv->isPlaying == TRUE && g_lv->fastForwarding == FALSE) {
    if (g_lv->selectedItemType == 0) {
      updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (g_lv->selectedItemType == 1) {
      updateItem(1, g_lv->selectedItem, g_lv->bs->item[g_lv->selectedItem].owner, g_lv->bs->item[g_lv->selectedItem].x, g_lv->bs->item[g_lv->selectedItem].y, g_lv->bs->item[g_lv->selectedItem].armour, g_lv->bs->item[g_lv->selectedItem].shells, g_lv->bs->item[g_lv->selectedItem].mines, FALSE);
    } else {
      updateItem(2, g_lv->selectedItem, g_lv->pb->item[g_lv->selectedItem].owner, g_lv->pb->item[g_lv->selectedItem].x, g_lv->pb->item[g_lv->selectedItem].y, g_lv->pb->item[g_lv->selectedItem].armour, 0, 0, g_lv->pb->item[g_lv->selectedItem].inTank);
    }
  }
}

/* Returns TRUE on log end or snapshot */
bool screenLogTick() {
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
  shellsDestroy(&g_lv->shs);
  g_lv->shs = shellsCreate();
  if (g_lv->isPlaying == TRUE) {
    switch (g_lv->state) {
    case lv_lr_start:
      /* Read bytes */
      logReadBytes(&code, 1);
      switch (code) {
      case LOG_QUIT:
        g_lv->isPlaying = FALSE;
        windowAddEvent(0, "End of Log File Reached");
        finished();
        returnValue = TRUE;
        break;
      case LOG_SNAPSHOT:
        processSnapshot();
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
      playersLgmZero();
      screenProcessLog(len);
      if (g_lv->centredTank == TRUE) {
        BYTE x = playersGetCentredX();
        BYTE y = playersGetCentredY();
        BYTE newXOffset;
        BYTE newYOffset;
        if (x != 0 && y != 0 && x != 255 && y != 255) {
          newXOffset = x - (g_lv->screenSizeX / 2);
          newYOffset = y - (g_lv->screenSizeY / 2);
          if (newXOffset != g_lv->xOffset || g_lv->yOffset != newYOffset) {
            g_lv->xOffset = newXOffset;
            g_lv->yOffset = newYOffset;
            screenUpdate(redraw);
          }
        }
      }
    }
  }
  return returnValue;
}

void screenCentreOnSelectedItem() {
  BYTE x;
  BYTE y;
  BYTE newXOffset;
  BYTE newYOffset;
  base b;
  pillbox p;
  if (g_lv->isPlaying == TRUE && g_lv->selectedItemType != 0) {
    if (g_lv->selectedItemType == 2) {
      pillsGetPill(&g_lv->pb, &p, (BYTE) (g_lv->selectedItem+1));
      x = p.x;
      y = p.y;
    } else {
      basesGetBase(&g_lv->bs, &b, (BYTE) (g_lv->selectedItem+1));
      x = b.x;
      y = b.y;
    }
    newXOffset = x - (g_lv->screenSizeX / 2);
    newYOffset = y - (g_lv->screenSizeY / 2);
    if (newXOffset != g_lv->xOffset || g_lv->yOffset != newYOffset) {
      g_lv->xOffset = newXOffset;
      g_lv->yOffset = newYOffset;
      screenUpdate(redraw);
    }
  }
}

bool processSnapshot() {
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
  playersCopyPTeams(data);
  snapshotAdd(&g_lv->snap, logGetCurrentPosition(), g_lv->timeRunning, blocksGetKey(), data);


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
      pillsSetPillNetData(&g_lv->pb, data, dataLen);
    }
  }

  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      basesSetBaseNetData(&g_lv->bs, data, dataLen);
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      startsSetStartNetData(&g_lv->ss, data, dataLen);
    }
  }
  if (returnValue == TRUE) {
    returnValue = mapReadRuns(&g_lv->mp);
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
      playersLeaveGame(count, FALSE);
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
        utilGetNibbles(data[pos++], &px, &py);
        frame = data[pos++];
        onBoat = data[pos++];
        lgmmx = data[pos++];
        lgmmy = data[pos++];
        utilGetNibbles(data[pos++], &lgmpx, &lgmpy);
        lgmframe = data[pos++];
        /* pos is now 11 — skip the redundant dataLen byte */
        pos++;
        /* Parse player name (pascal string: length byte + chars) */
        if (pos > dataLen || *(data+pos-1) >= sizeof(name)) {
          returnValue = FALSE;
        } else {
          utilPtoCString((char *)(data+pos-1), name);
          pos += *(data+pos-1);
          pos++;
        }
        /* Parse location (pascal string) */
        if (returnValue == TRUE) {
          if (pos > dataLen || *(data+pos-1) >= sizeof(location)) {
            returnValue = FALSE;
          } else {
            utilPtoCString((char *)(data+pos-1), location);
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
              playersSetPlayer(count, name, location, mx ,my, px, py, frame, onBoat, numAllies, allies, FALSE, TRUE);
              playersUpdateLgm(count, lgmmx, lgmmy, lgmpx, lgmpy,lgmframe);
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
bool logLoad(char *fileName, int memoryBufferSize) {
  char id[LENGTH_ID+1]; /* The map ID Should read "BMAPBOLO" */
  BYTE dataLen;
  BYTE logVersion;    /* Version of the map file */
  bool returnValue = TRUE;
  int len;
  BYTE ip[4];

  snapshotDestroy(&g_lv->snap);
  g_lv->snap = snapshotCreate();
  g_lv->timeRunning = 0;

  returnValue = blocksCreate(fileName, memoryBufferSize);
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

  blocksSetKey((BYTE) (g_lv->gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = processSnapshot();
  }

  g_lv->logLoaded = returnValue;
  return returnValue;
}

/*********************************************************
*NAME:          screenLoadMap
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Loads a map. Returns if it was sucessful reading the
*  map or not.
*
*ARGUMENTS:
*********************************************************/
bool screenLoadMap(char *fileName, int memoryBufferSize) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  blocksDestroy();
  screenDestroy();
  screenSetup();
  returnValue = logLoad(fileName, memoryBufferSize);
  if (returnValue == TRUE) {
    /* Decompress entire log so we know total size for the seek slider */
    logDecompressAll();
    /* Set the game information up */
    frontEndSetGameInformation(FALSE, g_lv->versionMajor, 1, g_lv->versionRevision, g_lv->mapName, g_lv->gt, g_lv->allowHiddenMines, g_lv->ai, g_lv->gmeStartDelay, g_lv->gmeLength, g_lv->wbnKey, g_lv->gmeCreateTime);
    g_lv->isPlaying = TRUE;
    screenUpdateView(redraw);
    g_lv->state = lv_lr_start;
  }
  return returnValue;
}


bool screenIsPlaying() {
  return g_lv->isPlaying;
}

bool screenCloseLog() {
  g_lv->isPlaying = FALSE;
  g_lv->logLoaded = FALSE;

  blocksDestroy();
  screenDestroy();
  return TRUE;
}

void screenSetOffset(BYTE x, BYTE y) {
  g_lv->xOffset = x;
  g_lv->yOffset = y;
}
BYTE screenGetOffsetX() {
  return g_lv->xOffset;
}

BYTE screenGetOffsetY() {
  return g_lv->yOffset;
}

BYTE screenGetNumPills() {
  return pillsGetNumPills(&g_lv->pb);
}

BYTE screenGetNumBases() {
  return basesGetNumBases(&g_lv->bs);
}

BYTE screenGetNumStarts() {
  return startsGetNumStarts(&g_lv->ss);
}


bool screenSetStart(BYTE x, BYTE y) {
  BYTE num;
  start s;
  num = startsGetNumStarts(&g_lv->ss);
  if (num >= MAX_STARTS) {
    return FALSE;
  }
  basesDeleteBase(&g_lv->bs, x, y);
  startsDeleteStart(&g_lv->ss, x, y);
  pillsDeletePill(&g_lv->pb, x, y);

  mapSetPos(&g_lv->mp, x, y, DEEP_SEA);
  s.x = x;
  s.y = y;
  s.dir = 0;
  num = startsGetNumStarts(&g_lv->ss);
  startsSetNumStarts(&g_lv->ss, (BYTE) (num+1));
  startsSetStart(&g_lv->ss, &s, (BYTE) (num+1));
  return TRUE;
}

bool screenSetPill(BYTE x, BYTE y) {
  BYTE num;
  pillbox s;
  num = pillsGetNumPills(&g_lv->pb);
  if (num >= MAX_PILLS) {
    return FALSE;
  }
  basesDeleteBase(&g_lv->bs, x, y);
  startsDeleteStart(&g_lv->ss, x, y);
  pillsDeletePill(&g_lv->pb, x, y);

  mapSetPos(&g_lv->mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 15;
  s.speed = 0;
  s.inTank = FALSE;
  num = pillsGetNumPills(&g_lv->pb);
  pillsSetNumPills(&g_lv->pb, (BYTE) (num+1));
  pillsSetPill(&g_lv->pb, &s, (BYTE) (num+1));
  return TRUE;

}

bool screenSetBase(BYTE x, BYTE y) {
  BYTE num;
  base s;

  num = basesGetNumBases(&g_lv->bs);
  if (num >= MAX_BASES) {
    return FALSE;
  }
  basesDeleteBase(&g_lv->bs, x, y);
  startsDeleteStart(&g_lv->ss, x, y);
  pillsDeletePill(&g_lv->pb, x, y);

  mapSetPos(&g_lv->mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 90;
  s.mines = 90;
  s.shells = 90;


  num = basesGetNumBases(&g_lv->bs);
  basesSetNumBases(&g_lv->bs, (BYTE) (num+1));
  basesSetBase(&g_lv->bs, &s, (BYTE) (num+1));
  return TRUE;
}

/*********************************************************
*NAME:          screenIsMine
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
bool screenIsMine(screenMines *value,BYTE xValue, BYTE yValue) {
  bool returnValue = FALSE; /* Value to return */

  if (xValue <= screenGetSizeX() && yValue <= screenGetSizeX()) {
    returnValue = *((*value)->mineItem+(yValue*screenGetSizeX()+xValue));
  }
  return returnValue;
}

/*********************************************************
*NAME:          screenNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of pillboxes
*
*ARGUMENTS:
*
*********************************************************/
BYTE screenNumPills(void) {
  return pillsGetNumPills(&g_lv->pb);
}

/*********************************************************
*NAME:          screenNumBases
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the number of bases
*
*ARGUMENTS:
*
*********************************************************/
BYTE screenNumBases(void) {
  return basesGetNumBases(&g_lv->bs);
}


BYTE screenGetSizeX() {
  return g_lv->screenSizeX;
}

BYTE screenGetSizeY() {
  return g_lv->screenSizeY;
}

/* Forward declaration for draw.c function */
extern void drawResizeRenderTarget(void);

void screenSetSizeX(BYTE x) {
  g_lv->screenSizeX = x;
  if (g_lv->view != NULL) {
    BYTE *newItems = malloc((screenGetSizeX()+2) * (screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*g_lv->view).screenItem);
      (*g_lv->view).screenItem = newItems;
    }
  }
  if (g_lv->mineView != NULL) {
    bool *newItems = malloc((screenGetSizeX()+1) * (screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*g_lv->mineView).mineItem);
      (*g_lv->mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  drawResizeRenderTarget();
}

void screenSetSizeY(BYTE y) {
  g_lv->screenSizeY = y;
  if (g_lv->view != NULL) {
    BYTE *newItems = malloc((screenGetSizeX()+2) * (screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*g_lv->view).screenItem);
      (*g_lv->view).screenItem = newItems;
    }
  }
  if (g_lv->mineView != NULL) {
    bool *newItems = malloc((screenGetSizeX()+1) * (screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*g_lv->mineView).mineItem);
      (*g_lv->mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  drawResizeRenderTarget();
}

void screenGetOffsets(BYTE *x, BYTE *y) {
  if (x != NULL) {
    *x = g_lv->xOffset;
  }
  if (y != NULL) {
    *y = g_lv->yOffset;
  }
}

void screenPanToOffsets(BYTE newXOffset, BYTE newYOffset) {
  if (g_lv->logLoaded == FALSE) {
    return;
  }
  if (newXOffset == g_lv->xOffset && newYOffset == g_lv->yOffset) {
    return;
  }
  g_lv->xOffset = newXOffset;
  g_lv->yOffset = newYOffset;
  screenUpdate(redraw);
}

void messageAdd(char *messageStr) {
  windowAddEvent(0, messageStr);
}

void screenGetTime(char *dest) {
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


void screenMouseCentreClick(int xPos, int yPos) {
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
  screenUpdate(redraw);
}

void screenMouseInformationClick(int xPos, int yPos) {
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
  if (pillsExistPos(&g_lv->pb, mapX, mapY) == TRUE) {
    g_lv->selectedItem = pillsItemNumAt(&g_lv->pb, mapX, mapY);
    g_lv->selectedItemType = 2;
  } else if (basesExistPos(&g_lv->bs, mapX, mapY) == TRUE) {
    g_lv->selectedItem = basesItemNumAt(&g_lv->bs, mapX, mapY);
    g_lv->selectedItemType = 1;
  }


  if (g_lv->fastForwarding == FALSE) {
    if (g_lv->selectedItemType == 0) {
      updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (g_lv->selectedItemType == 1) {
      updateItem(1, g_lv->selectedItem, g_lv->bs->item[g_lv->selectedItem].owner, g_lv->bs->item[g_lv->selectedItem].x, g_lv->bs->item[g_lv->selectedItem].y, g_lv->bs->item[g_lv->selectedItem].armour, g_lv->bs->item[g_lv->selectedItem].shells, g_lv->bs->item[g_lv->selectedItem].mines, FALSE);
    } else {
      updateItem(2, g_lv->selectedItem, g_lv->pb->item[g_lv->selectedItem].owner, g_lv->pb->item[g_lv->selectedItem].x, g_lv->pb->item[g_lv->selectedItem].y, g_lv->pb->item[g_lv->selectedItem].armour, 0, 0, g_lv->pb->item[g_lv->selectedItem].inTank);
    }
  }

}

void screenMouseClick(int xPos, int yPos) {
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
  if (playersChooseView(g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else if (pillsChooseView(&g_lv->pb, g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else if (basesChooseView(&g_lv->bs, g_lv->xOffset + xClick, g_lv->yOffset + yClick) == TRUE) {
  } else {
  }
}

void screenFastForward() {
  bool available;

  if (g_lv->isPlaying == TRUE && g_lv->fastForwarding == FALSE) {
    g_lv->fastForwarding = TRUE;
    available = screenLogTick();
    while (available == FALSE && g_lv->isPlaying == TRUE) {
      available = screenLogTick();
    }
    g_lv->fastForwarding = FALSE;
  }
}

void screenTankCentred(int enabled) {
  g_lv->centredTank = enabled;
}

void screenRewind() {
  uint32_t currentTime = g_lv->timeRunning;
  size_t wantedPos;
  uint32_t firstTime;
  bool available;
  BYTE key;
  BYTE *pTeams = NULL;

  available = snapshotBackwards(&g_lv->snap, &wantedPos, &currentTime, &key, &pTeams);
  if (available == TRUE) {
    if (currentTime > 1000 && (g_lv->timeRunning - currentTime) < 1000) {
      firstTime = currentTime;
      currentTime -= 1000;
      if (snapshotBackwards(&g_lv->snap, &wantedPos, &currentTime, &key, &pTeams) == FALSE) {
        currentTime = firstTime;
      }
    }
    g_lv->timeRunning = currentTime;
    logSetPosition(wantedPos);
    blocksSetKey(key);
    processSnapshot();
    playersSetTeams(pTeams);
    windowRemoveEvents();
    g_lv->isPlaying = TRUE;
    if (wantedPos == 0) {
      startOfLog();
    }
  }
}

void screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime) {
  *currentPos = logGetCurrentPosition();
  *totalSize = logGetTotalSize();
  *currentTime = g_lv->timeRunning;
}

void screenSeekToPosition(float ratio) {
  size_t totalSize = logGetTotalSize();
  size_t targetPos;
  size_t snapPos;
  uint32_t snapTime;
  BYTE key;
  BYTE *pTeams = NULL;

  if (totalSize == 0) return;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;

  targetPos = (size_t)(ratio * (float)totalSize);

  if (snapshotFindByPosition(&g_lv->snap, targetPos, &snapPos, &snapTime, &key, &pTeams)) {
    g_lv->timeRunning = snapTime;
    logSetPosition(snapPos);
    blocksSetKey(key);
    processSnapshot();
    playersSetTeams(pTeams);
    windowRemoveEvents();
    g_lv->isPlaying = TRUE;
    g_lv->state = lv_lr_start;

    /* Fast-forward from snapshot to target position */
    g_lv->fastForwarding = TRUE;
    while (logGetCurrentPosition() < targetPos && g_lv->isPlaying == TRUE) {
      screenLogTick();
    }
    g_lv->fastForwarding = FALSE;
  }
}

int32_t screenGetGameTimeLeft() {
  return g_lv->gmeLength;
}

int32_t screenGetGameStartDelay() {
  return g_lv->gmeStartDelay;
}


BYTE screenGetNumPlayers() {
  return playersGetNumPlayers();
}

void screenGetPlayerName(char *name, BYTE playerNum) {
  playersGetPlayerName(playerNum, name);
}

void screenGetMapName(char *dest) {
  strncpy(dest, g_lv->mapName, sizeof(g_lv->mapName) - 1);
  dest[sizeof(g_lv->mapName) - 1] = '\0';
}

/*********************************************************
*NAME:          screenSaveMap
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
bool screenSaveMap(char *fileName, bool saveOwnerships) {
  return mapWrite(fileName, &g_lv->mp, &g_lv->pb, &g_lv->bs, &g_lv->ss, saveOwnerships);
}


BYTE screenGetPillTeam(BYTE x, BYTE y, BYTE *pillHealth) {
  pillbox p;
  BYTE itemNum = pillsItemNumAt(&g_lv->pb,(BYTE) (x+g_lv->xOffset), (BYTE) (y+g_lv->yOffset));

  p.owner = NEUTRAL;
  p.armour = 15;
  pillsGetPill(&g_lv->pb, &p, (BYTE) (itemNum+1));
 *pillHealth = p.armour;
  if (p.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return playersGetTeamForOwner(p.owner);
}

BYTE screenGetBaseTeam(BYTE x, BYTE y) {
  base b;
  BYTE itemNum = basesItemNumAt(&g_lv->bs, (BYTE) (x+g_lv->xOffset), (BYTE) (y+g_lv->yOffset));

  b.owner = NEUTRAL;
  basesGetBase(&g_lv->bs, &b, (BYTE) (itemNum+1));
  if (b.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return playersGetTeamForOwner(b.owner);
}
