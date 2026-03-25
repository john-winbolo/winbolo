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

/* Module Level Variables */
screen view = NULL;
screenMines mineView = NULL;
map mp = NULL;
bases bs = NULL;
pillboxes pb = NULL;
starts ss = NULL;
shells shs = NULL;

/* From log file */
char mapName[64];
BYTE gt;
BYTE allowHiddenMines;
BYTE ai;
bool usePassword;
BYTE maxPlayers;
BYTE wbnKey[32];
char logFileName[FILENAME_MAX];

snapshot snap = NULL;

/* The offset from the top and left of the map */
BYTE xOffset;
BYTE yOffset;

BYTE screenSizeX = MAIN_SCREEN_SIZE_X+15;
BYTE screenSizeY = MAIN_SCREEN_SIZE_Y+15;

bool isPlaying = FALSE;
bool logLoaded = FALSE;
uint32_t timeRunning;
bool centredTank = FALSE;

bool fastForwarding = FALSE;

/* Game length and start delay */
int32_t gmeStartDelay;
int32_t gmeLength;
int32_t gmeCreateTime;

char serverIP[256];
unsigned short serverPort;
BYTE versionMajor;
BYTE versionMinor;
BYTE versionRevision;
BYTE loadedLogVersion;

BYTE selectedItem = 0; 
BYTE selectedItemType = 0;

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
  *((*mineView).mineItem+a) = FALSE;
  /* Set up Items */
  if ((pillsExistPos(&pb,xValue,yValue)) == TRUE) {
    returnValue = pillsGetScreenHealth(&pb, xValue, yValue);
  } else if ((basesExistPos(&bs,xValue,yValue)) == TRUE) {
     ba = basesGetAlliancePos(&bs, xValue, yValue);
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
      if (basesAmOwner(&bs, playersGetSelf(), xValue, yValue) == TRUE) {
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
    currentPos = mapGetPos(&mp,xValue,yValue);
    if (mapIsMine(&mp, xValue, yValue) == TRUE) {
      *((*mineView).mineItem+a) = TRUE;
      if (currentPos != DEEP_SEA) {
        currentPos = currentPos - MINE_SUBTRACT;
      }
    } else {
      *((*mineView).mineItem+a) = FALSE;
    }

    if (basesExistPos(&bs, (BYTE) (xValue-1), (BYTE) (yValue-1)) == TRUE) {
      aboveLeft = ROAD;
    } else {
      aboveLeft = mapGetPos(&mp,(BYTE) (xValue-1),(BYTE) (yValue-1));
      if (aboveLeft >= MINE_START && aboveLeft <= MINE_END) {
        aboveLeft = aboveLeft - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&bs, xValue, (BYTE) (yValue-1)) == TRUE) {
      above = ROAD;
    } else {
      above = mapGetPos(&mp,xValue,(BYTE) (yValue-1));
      if (above >= MINE_START && above <= MINE_END) {
        above = above - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&bs, (BYTE) (xValue+1), (BYTE) (yValue-1)) == TRUE) {
      aboveRight = ROAD;
    } else {
      aboveRight = mapGetPos(&mp,(BYTE) (xValue+1),(BYTE) (yValue-1));
      if (aboveRight >= MINE_START && aboveRight <= MINE_END) {
        aboveRight = aboveRight - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&bs, (BYTE) (xValue-1), yValue) == TRUE) {
      leftPos = ROAD;
    } else {
      leftPos = mapGetPos(&mp,(BYTE) (xValue-1),yValue);
      if (leftPos >= MINE_START && leftPos <= MINE_END) {
        leftPos = leftPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&bs, (BYTE) (xValue+1), yValue) == TRUE) {
      rightPos = ROAD;
    } else {
      rightPos = mapGetPos(&mp,(BYTE) (xValue+1),yValue);
      if (rightPos >= MINE_START && rightPos <= MINE_END) {
        rightPos = rightPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&bs, (BYTE) (xValue-1), (BYTE) (yValue+1)) == TRUE) {
      belowLeft = ROAD;
    } else {
      belowLeft = mapGetPos(&mp,(BYTE) (xValue-1),(BYTE) (yValue+1));
      if (belowLeft >= MINE_START && belowLeft <= MINE_END) {
        belowLeft = belowLeft - MINE_SUBTRACT;
      }
    }


    if (basesExistPos(&bs, xValue, (BYTE) (yValue+1)) == TRUE) {
      below = ROAD;
    } else {
      below = mapGetPos(&mp,xValue,(BYTE) (yValue+1));
      if (below >= MINE_START && below <= MINE_END) {
        below = below - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&bs, (BYTE) (xValue+1), (BYTE) (yValue+1)) == TRUE) {
      belowRight = ROAD;
    } else {
      belowRight = mapGetPos(&mp,(BYTE) (xValue+1),(BYTE) (yValue+1));
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

  if (logLoaded == FALSE) {
    return;
  }

  if (centredTank == FALSE || value == redraw) {
    if (value == left) {
      xOffset--;
    } else if (value == right) {
      xOffset++;
    } else if (value == up) {
      yOffset--;
    } else if (value == down) {
      yOffset++;
    }
  }

  for (count=0;count < ssx; count++) {
    for (count2=0;count2 < ssy; count2++) {
      *((*view).screenItem+(ssx*count2)+count) = screenCalcSquare((BYTE) (count+xOffset),(BYTE) (count2+yOffset), count, count2);
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
  gmeStartDelay = 0;
  gmeLength = UNLIMITED_GAME_TIME;
  isPlaying = FALSE;
  logLoaded = FALSE;
  xOffset = 127;
  yOffset = 127;
  mapCreate(&mp);
  pillsCreate(&pb);
  startsCreate(&ss);
  basesCreate(&bs);
  playersCreate();  
  playersSetSelf(0);
  shs = shellsCreate();
  if (view != NULL) {
    free((*view).screenItem);
    Dispose(view);
  }
  New(view);
  if (view != NULL) {
    (*view).screenItem = malloc((screenGetSizeX()+2) * (screenGetSizeY()+2));
  }
  if (mineView != NULL) {
    free((*mineView).mineItem);
    free(mineView);
  }
  a = (screenGetSizeX()+1) * (screenGetSizeY()+1);
  New(mineView);
  if (mineView != NULL) {
    (*mineView).mineItem = malloc(a * sizeof(bool));
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
  if (mp != NULL) {
    mapDestroy(&mp);
    mp = NULL;
  }
  if (pb != NULL) {
    pillsDestroy(&pb);
    pb = NULL;
  }
  if (ss != NULL) {
    startsDestroy(&ss);
    ss = NULL;
  }
  if (bs != NULL) {
    basesDestroy(&bs);
    bs = NULL;
  }
  playersDestroy();
  if (shs != NULL) {
    shellsDestroy(&shs);
    shs = NULL;
  }
  if (view != NULL) {
    free((*view).screenItem);
    Dispose(view);
    view = NULL;
  }
  if (mineView != NULL) {
    free((*mineView).mineItem);
    Dispose(mineView);
    mineView = NULL;
  }
  isPlaying = FALSE;
  logLoaded = FALSE;
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
  
  if (logLoaded == FALSE) {
    return;
  }
  
  screenUpdateView(value);
  playersMakeScreenLgm(&sl, xOffset, (BYTE) (xOffset + screenGetSizeX()), yOffset, (BYTE) (yOffset + screenGetSizeY()));
  shellsCalcScreenBullets(&shs, &sb, xOffset, (BYTE) (xOffset + screenGetSizeX()), yOffset, (BYTE) (yOffset + screenGetSizeY()));
  playersMakeScreenTanks(&st, xOffset, (BYTE) (xOffset + screenGetSizeX()), yOffset, (BYTE) (yOffset + screenGetSizeY()));
  frontEndDrawMainScreen(&view, &mineView, &st, NULL, &sb, &sl, 0, FALSE, 0, 0);
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
  mapSetPos(&mp, xValue, yValue, terrain);
  basesDeleteBase(&bs, xValue, yValue);
  startsDeleteStart(&ss, xValue, yValue); 
  pillsDeletePill(&pb, xValue, yValue);
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
      returnValue = *((*view).screenItem+(yValue*screenGetSizeX()+xValue));
  }
  return returnValue;
}

#include "log.h"
void windowAddEvent(int eventType, char *msg);
typedef enum {
  lr_start,
  lr_longwait,
  lr_shortwait
} lrStates;


lrStates state;
unsigned short waitLen;


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
      if (loadedLogVersion == LOG_VERSION_V0) {
        /* Version 0: opt2-opt5 are IP address octets */
        snprintf(mem, sizeof(mem), "%d.%d.%d.%d", opt2, opt3, opt4, opt5);
        dnsLookup(mem, str, sizeof(str));
        strncpy(mem, str, sizeof(mem) - 1);
        mem[sizeof(mem) - 1] = '\0';
      } else if (loadedLogVersion == LOG_VERSION_V1) {
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
      mapSetPos(&mp, opt1, opt2, opt3);
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
      shellsAddItem(&shs, opt1, opt2, px, py, opt4);
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
      basesSetOwner(&bs, opt1, opt2, opt3);
      break;
    case log_BaseSetStock:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      logReadBytes(&opt4, 1);
      basesSetStock(&bs, opt1, opt2, opt3, opt4);
      break;
    case log_PillSetOwner:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      pillsSetPillOwner(&pb, opt1, opt2, opt3);
      break;
    case log_PillSetPlace:
      logReadBytes(&opt1, 1);
      logReadBytes(&opt2, 1);
      logReadBytes(&opt3, 1);
      pillsSetPos(&pb, opt1, opt2, opt3);
      break;
    case log_PillSetHealth:
      logReadBytes(&opt1, 1);
      utilGetNibbles(opt1, &opt2, &opt3);
      pillsSetHealth(&pb, opt2, opt3);
      break;
    case log_PillSetInTank:
      logReadBytes(&opt1, 1);
      utilGetNibbles(opt1, &opt2, &opt3);
      pillsSetInTank(&pb, opt2, opt3);
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
  //void updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank)
  if (isPlaying == TRUE && fastForwarding == FALSE) {
    if (selectedItemType == 0) {
      updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (selectedItemType == 1) {
      updateItem(1, selectedItem, bs->item[selectedItem].owner, bs->item[selectedItem].x, bs->item[selectedItem].y, bs->item[selectedItem].armour, bs->item[selectedItem].shells, bs->item[selectedItem].mines, FALSE);
    } else {
      updateItem(2, selectedItem, pb->item[selectedItem].owner, pb->item[selectedItem].x, pb->item[selectedItem].y, pb->item[selectedItem].armour, 0, 0, pb->item[selectedItem].inTank);
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
  timeRunning += 20; /* Add 20 ms */  
  if (gmeStartDelay > 0) {
    gmeStartDelay--;
  }
  if (gmeLength > 0 && gmeStartDelay == 0) {
    gmeLength--;
  }
  shellsDestroy(&shs);
  shs = shellsCreate();
  if (isPlaying == TRUE) {
    switch (state) {
    case lr_start:
      /* Read bytes */
      logReadBytes(&code, 1);
      switch (code) {
      case LOG_QUIT:
        isPlaying = FALSE;
        windowAddEvent(0, "End of Log File Reached");
        finished();
        returnValue = TRUE;
        break;
      case LOG_SNAPSHOT:
        processSnapshot();
        returnValue = TRUE;
        break;
      case LOG_NOEVENTS:
        logReadBytes((BYTE *) &waitLen, 1);
        state = lr_shortwait;
        if (waitLen == 0) {
          waitLen = 1;
        }
        break;
      case LOG_NOEVENTS_LONG:
        logReadBytes(&top, 1);
        logReadBytes(&bottom, 1);
        us = top << 8;
        us += bottom;
        waitLen = ntohs(us);

        state = lr_longwait;
        if (waitLen == 0) {
          waitLen = 1;
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
    case lr_longwait:
    case lr_shortwait:
      waitLen--;
      if (waitLen == 0) {
        state = lr_start;
      }
      break;
    }
    if (process == TRUE) {
      playersLgmZero();
      screenProcessLog(len);
      if (centredTank == TRUE) {
        BYTE x = playersGetCentredX();
        BYTE y = playersGetCentredY();
        BYTE newXOffset;
        BYTE newYOffset;
        if (x != 0 && y != 0 && x != 255 && y != 255) {
          newXOffset = x - (screenSizeX / 2); 
          newYOffset = y - (screenSizeY / 2);
          if (newXOffset != xOffset || yOffset != newYOffset) {
            xOffset = newXOffset;
            yOffset = newYOffset;
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
  if (isPlaying == TRUE && selectedItemType != 0) {
    if (selectedItemType == 2) {
      pillsGetPill(&pb, &p, (BYTE) (selectedItem+1));
      x = p.x;
      y = p.y;
    } else {
      basesGetBase(&bs, &b, (BYTE) (selectedItem+1));
      x = b.x;
      y = b.y;
    }
    newXOffset = x - (screenSizeX / 2); 
    newYOffset = y - (screenSizeY / 2);
    if (newXOffset != xOffset || yOffset != newYOffset) {
      xOffset = newXOffset;
      yOffset = newYOffset;
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
  snapshotAdd(&snap, logGetCurrentPosition(), timeRunning, blocksGetKey(), data);


  /* Read in start delay and time limit */
  logReadBytes((BYTE *) &gmeStartDelay, sizeof(int32_t));
  gmeStartDelay = ntohl(gmeStartDelay);
  logReadBytes((BYTE *) &gmeLength , sizeof(int32_t));
  gmeLength = ntohl(gmeLength);

  /* Read pillboxes, bases and starts */
  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      pillsSetPillNetData(&pb, data, dataLen);
    }
  }

  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      basesSetBaseNetData(&bs, data, dataLen);
    }
  }
  if (returnValue == TRUE) {
    len = logReadBytes(&dataLenRaw, 1);
    dataLen = dataLenRaw;
    if (dataLen > sizeof(data)) {
      returnValue = FALSE;
    } else {
      logReadBytes(data, dataLen);
      startsSetStartNetData(&ss, data, dataLen);
    }
  }  
  if (returnValue == TRUE) {
    returnValue = mapReadRuns(&mp);
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

  snapshotDestroy(&snap);
  snap = snapshotCreate();
  timeRunning = 0;

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
      loadedLogVersion = logVersion;
    } else {
      returnValue = FALSE;
    }
  }

  /* Read map name */
  if (returnValue == TRUE) {
    logReadBytes(&dataLen, 1);
    len = logReadBytes((BYTE *)mapName, dataLen);
    mapName[dataLen] = '\0';
    if (len != dataLen) {
      returnValue = FALSE;
    }
  }

  /* Read game type, mines, ai, password, max players */
  if (returnValue == TRUE) {
    logReadBytes(&gt, 1);
    logReadBytes(&allowHiddenMines, 1);
    logReadBytes(&ai, 1);
    { BYTE tmp; logReadBytes(&tmp, 1); usePassword = tmp; }
    logReadBytes(&maxPlayers, 1);
    logReadBytes(&versionMajor, 1);
    logReadBytes(&versionMinor, 1);
    logReadBytes(&versionRevision, 1);
    logReadBytes(ip, 4);
    snprintf(serverIP, sizeof(serverIP), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    logReadBytes((BYTE *) &serverPort, sizeof(unsigned short));
    serverPort = ntohs(serverPort);
    logReadBytes((BYTE *) &gmeCreateTime, sizeof(int32_t));
    gmeCreateTime = ntohl(gmeCreateTime);
    len = logReadBytes((BYTE *) &wbnKey, 32);
    if (len != 32) {
      returnValue = FALSE;
    }
  }

  blocksSetKey((BYTE) (gmeCreateTime & 0xFF));
  len = logReadBytes(&dataLen, 1);
  if (len != 1 || dataLen != LOG_SNAPSHOT) {
    returnValue = FALSE;
  } else {
    returnValue = processSnapshot();
  }

  logLoaded = returnValue;
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
    frontEndSetGameInformation(FALSE, versionMajor, 1, versionRevision, mapName, gt, allowHiddenMines, ai, gmeStartDelay, gmeLength, wbnKey, gmeCreateTime);
    isPlaying = TRUE;
    screenUpdateView(redraw);
    state = lr_start;
  }
  return returnValue;
}


bool screenIsPlaying() {
  return isPlaying;
}

bool screenCloseLog() {
  isPlaying = FALSE;
  logLoaded = FALSE; 

  blocksDestroy();
  screenDestroy();
  return TRUE;
}

void screenSetOffset(BYTE x, BYTE y) {
  xOffset = x;
  yOffset = y;
}
BYTE screenGetOffsetX() {
  return xOffset;
}

BYTE screenGetOffsetY() {
  return yOffset;
}

BYTE screenGetNumPills() {
  return pillsGetNumPills(&pb);
}

BYTE screenGetNumBases() {
  return basesGetNumBases(&bs);
}

BYTE screenGetNumStarts() {
  return startsGetNumStarts(&ss);
}


bool screenSetStart(BYTE x, BYTE y) {
  BYTE num;
  start s;
  num = startsGetNumStarts(&ss);
  if (num >= MAX_STARTS) {
    return FALSE;
  }
  basesDeleteBase(&bs, x, y);
  startsDeleteStart(&ss, x, y);
  pillsDeletePill(&pb, x, y);

  mapSetPos(&mp, x, y, DEEP_SEA);
  s.x = x;
  s.y = y;
  s.dir = 0;
  num = startsGetNumStarts(&ss);
  startsSetNumStarts(&ss, (BYTE) (num+1));
  startsSetStart(&ss, &s, (BYTE) (num+1));
  return TRUE;
}

bool screenSetPill(BYTE x, BYTE y) {
  BYTE num;
  pillbox s;
  num = pillsGetNumPills(&pb);
  if (num >= MAX_PILLS) {
    return FALSE;
  }
  basesDeleteBase(&bs, x, y);
  startsDeleteStart(&ss, x, y);
  pillsDeletePill(&pb, x, y);

  mapSetPos(&mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 15;
  s.speed = 0;
  s.inTank = FALSE;
  num = pillsGetNumPills(&pb);
  pillsSetNumPills(&pb, (BYTE) (num+1));
  pillsSetPill(&pb, &s, (BYTE) (num+1));
  return TRUE;

}

bool screenSetBase(BYTE x, BYTE y) {
  BYTE num;
  base s;

  num = basesGetNumBases(&bs);
  if (num >= MAX_BASES) {
    return FALSE;
  }
  basesDeleteBase(&bs, x, y);
  startsDeleteStart(&ss, x, y);
  pillsDeletePill(&pb, x, y);

  mapSetPos(&mp, x, y, ROAD);
  s.x = x;
  s.y = y;
  s.owner = 0xFF;
  s.armour = 90;
  s.mines = 90;
  s.shells = 90;

  
  num = basesGetNumBases(&bs);
  basesSetNumBases(&bs, (BYTE) (num+1));
  basesSetBase(&bs, &s, (BYTE) (num+1));
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
  return pillsGetNumPills(&pb);
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
  return basesGetNumBases(&bs);
}


BYTE screenGetSizeX() {
  return screenSizeX;
}

BYTE screenGetSizeY() {
  return screenSizeY;
}

/* Forward declaration for draw.c function */
extern void drawResizeRenderTarget(void);

void screenSetSizeX(BYTE x) {
  screenSizeX = x;
  if (view != NULL) {
    BYTE *newItems = malloc((screenGetSizeX()+2) * (screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*view).screenItem);
      (*view).screenItem = newItems;
    }
  }
  if (mineView != NULL) {
    bool *newItems = malloc((screenGetSizeX()+1) * (screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*mineView).mineItem);
      (*mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  drawResizeRenderTarget();
}

void screenSetSizeY(BYTE y) {
  screenSizeY = y;
  if (view != NULL) {
    BYTE *newItems = malloc((screenGetSizeX()+2) * (screenGetSizeY()+2));
    if (newItems != NULL) {
      free((*view).screenItem);
      (*view).screenItem = newItems;
    }
  }
  if (mineView != NULL) {
    bool *newItems = malloc((screenGetSizeX()+1) * (screenGetSizeY()+1) * sizeof(bool));
    if (newItems != NULL) {
      free((*mineView).mineItem);
      (*mineView).mineItem = newItems;
    }
  }
  /* Resize the render target to match the new screen size.
   * This is critical for correct mouse coordinate mapping. */
  drawResizeRenderTarget();
}

void screenGetOffsets(BYTE *x, BYTE *y) {
  if (x != NULL) {
    *x = xOffset;
  }
  if (y != NULL) {
    *y = yOffset;
  }
}

void screenPanToOffsets(BYTE newXOffset, BYTE newYOffset) {
  if (logLoaded == FALSE) {
    return;
  }
  if (newXOffset == xOffset && newYOffset == yOffset) {
    return;
  }
  xOffset = newXOffset;
  yOffset = newYOffset;
  screenUpdate(redraw);
}

void messageAdd(char *messageStr) {
  windowAddEvent(0, messageStr);
}

void screenGetTime(char *dest) {
  double mins;
  double secs;

  secs = timeRunning / 1000.00;
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

  if (logLoaded == FALSE) {
    return;
  }
  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);
  xOffset = (xOffset + xClick) - (screenSizeX / 2);
  yOffset = (yOffset + yClick) - (screenSizeY / 2);
  screenUpdate(redraw);
}

void screenMouseInformationClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;
  BYTE mapX, mapY;  /* Map coordinates */

  if (logLoaded == FALSE) {
    return;
  }
  
  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);
  
  /* Calculate map coordinates */
  mapX = (BYTE) (xOffset + xClick);
  mapY = (BYTE) (yOffset + yClick);
  
  // Item info
  if (pillsExistPos(&pb, mapX, mapY) == TRUE) {
    selectedItem = pillsItemNumAt(&pb, mapX, mapY);
    selectedItemType = 2;
  } else if (basesExistPos(&bs, mapX, mapY) == TRUE) {
    selectedItem = basesItemNumAt(&bs, mapX, mapY);
    selectedItemType = 1;
  }


  if (fastForwarding == FALSE) {
    if (selectedItemType == 0) {
      updateItem(0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else if (selectedItemType == 1) {
      updateItem(1, selectedItem, bs->item[selectedItem].owner, bs->item[selectedItem].x, bs->item[selectedItem].y, bs->item[selectedItem].armour, bs->item[selectedItem].shells, bs->item[selectedItem].mines, FALSE);
    } else {
      updateItem(2, selectedItem, pb->item[selectedItem].owner, pb->item[selectedItem].x, pb->item[selectedItem].y, pb->item[selectedItem].armour, 0, 0, pb->item[selectedItem].inTank);
    }
  }  

}

void screenMouseClick(int xPos, int yPos) {
  div_t dt;        /* Used for integer division */
  int xClick;
  int yClick;

  if (logLoaded == FALSE) {
    return;
  }
  dt = div(xPos, (16)); //screenSizeX
  xClick = (int) (dt.quot);
  dt = div(yPos, (16)); //screenSizeY
  yClick = (int) (dt.quot);

  // Who am I viewing?
  if (playersChooseView(xOffset + xClick, yOffset + yClick) == TRUE) {
  } else if (pillsChooseView(&pb, xOffset + xClick, yOffset + yClick) == TRUE) {
  } else if (basesChooseView(&bs, xOffset + xClick, yOffset + yClick) == TRUE) {
  } else {
  }
}

void screenFastForward() {
  bool available;

  if (isPlaying == TRUE && fastForwarding == FALSE) {
    fastForwarding = TRUE;
    available = screenLogTick();
    while (available == FALSE && isPlaying == TRUE) {
      available = screenLogTick();
    }
    fastForwarding = FALSE;
  }
}

void screenTankCentred(int enabled) {
  centredTank = enabled;
}

void screenRewind() {
  uint32_t currentTime = timeRunning;
  size_t wantedPos;
  uint32_t firstTime;
  bool available;
  BYTE key;
  BYTE *pTeams = NULL;

  available = snapshotBackwards(&snap, &wantedPos, &currentTime, &key, &pTeams);
  if (available == TRUE) {
    if (currentTime > 1000 && (timeRunning - currentTime) < 1000) {
      firstTime = currentTime;
      currentTime -= 1000;
      if (snapshotBackwards(&snap, &wantedPos, &currentTime, &key, &pTeams) == FALSE) {
        currentTime = firstTime;
      }
    }
    timeRunning = currentTime;
    logSetPosition(wantedPos);
    blocksSetKey(key);
    processSnapshot();
    playersSetTeams(pTeams);
    windowRemoveEvents();
    isPlaying = TRUE;
    if (wantedPos == 0) {
      startOfLog();
    }
  }
}

void screenGetLogProgress(size_t *currentPos, size_t *totalSize, uint32_t *currentTime) {
  *currentPos = logGetCurrentPosition();
  *totalSize = logGetTotalSize();
  *currentTime = timeRunning;
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

  if (snapshotFindByPosition(&snap, targetPos, &snapPos, &snapTime, &key, &pTeams)) {
    timeRunning = snapTime;
    logSetPosition(snapPos);
    blocksSetKey(key);
    processSnapshot();
    playersSetTeams(pTeams);
    windowRemoveEvents();
    isPlaying = TRUE;
    state = lr_start;

    /* Fast-forward from snapshot to target position */
    fastForwarding = TRUE;
    while (logGetCurrentPosition() < targetPos && isPlaying == TRUE) {
      screenLogTick();
    }
    fastForwarding = FALSE;
  }
}

int32_t screenGetGameTimeLeft() {
  return gmeLength;
}

int32_t screenGetGameStartDelay() {
  return gmeStartDelay;
}


BYTE screenGetNumPlayers() {
  return playersGetNumPlayers();
}

void screenGetPlayerName(char *name, BYTE playerNum) {
  playersGetPlayerName(playerNum, name);
}

void screenGetMapName(char *dest) {
  strncpy(dest, mapName, sizeof(mapName) - 1);
  dest[sizeof(mapName) - 1] = '\0';
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
  return mapWrite(fileName, &mp, &pb, &bs, &ss, saveOwnerships);
}


BYTE screenGetPillTeam(BYTE x, BYTE y, BYTE *pillHealth) {
  pillbox p;
  BYTE itemNum = pillsItemNumAt(&pb,(BYTE) (x+xOffset), (BYTE) (y+yOffset));

  p.owner = NEUTRAL;
  p.armour = 15;
  pillsGetPill(&pb, &p, (BYTE) (itemNum+1));
 *pillHealth = p.armour;
  if (p.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return playersGetTeamForOwner(p.owner);
}

BYTE screenGetBaseTeam(BYTE x, BYTE y) {
  base b;
  BYTE itemNum = basesItemNumAt(&bs, (BYTE) (x+xOffset), (BYTE) (y+yOffset));

  b.owner = NEUTRAL;
  basesGetBase(&bs, &b, (BYTE) (itemNum+1));
  if (b.owner == NEUTRAL) {
    return NEUTRAL_TEAM;
  }

  return playersGetTeamForOwner(b.owner);
}
	
