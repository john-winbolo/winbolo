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
*Last Modified: 17/12/03
*Purpose:
*  Provides Interfaces with the front end
*********************************************************/


/* NOTE: This really just here for historical reasons. It was present in each of the WIP releases
 * and used to scroll across the newswire as the game started.
 * Stuarts email address no longer works */
/*#define BOLO_VERSION_STRING "  WinBolo - WIP R11 (13/6/99) - DO NOT DISTRIBUTE\0"
#define OLD_BOLO_COPYRIGHT_STRING "Bolo � 1987-1995 Stuart Cheshire <>\0"
#define NEW_BOLO_COPYRIGHT_STRING "- WinBolo � 1998-1999 John Morrison <john@winbolo.com>          \0" */

/* Includes */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "../common/wb_log.h"
#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "rubble.h"
#include "explosions.h"
#include "screenbullet.h"
#include "frontend.h"
#include "sounddist.h"
#include "messages.h"
#include "grass.h"
#include "swamp.h"
#include "building.h"
#include "tilenum.h"
#include "screencalc.h"
#include "tankexp.h"
#include "scroll.h"
#include "lgm.h"
#include "log.h"
#include "floodfill.h"
#include "minesexp.h"
#include "treegrow.h"
#include "mines.h"
#include "labels.h"
#include "players.h"
#include "screentank.h"
#include "screenlgm.h"
#include "screenbrainmap.h"
#include "screen.h"
#include "client_state.h"
#include "interpolation.h"
#include "util.h"
#include "client_sim.h"
#include "../server/server_sim.h"
#include <SDL3/SDL.h>
#include "../steam/steam_wrapper.h"
/* Forward declaration — implemented in gui/sdl3/cursor.c */
extern void moveMousePointer(updateType value);

/* clientCenterTankCS is declared in screen.h (extern) so client_snapshot.c
 * can call it; the bot path never invokes it (guarded by isHuman/keypress
 * checks) but the linker still needs to resolve the symbol. */

/* Module Level Variables */

/* Display statics removed — now fields of ClientSim (see client_sim.h) */

/*********************************************************
*NAME:          screenRenderSetup
*PURPOSE:
*  Sets up rendering-only state (view buffers, offsets).
*  Called by screenSetup after clientSimCreate.
*********************************************************/
static void screenRenderSetup(ClientSim *csPtr) {
  New(*clientSimGetView(csPtr));
  New(*clientSimGetMineView(csPtr));
  clientSimSetXOffset(csPtr, 0);
  clientSimSetYOffset(csPtr, 0);
  clientSimSetCursorPosX(csPtr, -1);
  clientSimSetCursorPosY(csPtr, -1);
  clientSimSetNeedScreenReCalc(csPtr, FALSE);
  clientSimSetInPillView(csPtr, FALSE);
  clientSimSetPillViewX(csPtr, 0);
  clientSimSetPillViewY(csPtr, 0);
}

/*********************************************************
*NAME:          screenRenderDestroy
*PURPOSE:
*  Cleans up rendering-only state (view buffers).
*  Called by screenDestroy before clientSimDestroy.
*********************************************************/
static void screenRenderDestroy(ClientSim *csPtr) {
  if (*clientSimGetView(csPtr) != NULL) {
    Dispose(*clientSimGetView(csPtr));
    clientSimSetView(csPtr, NULL);
  }
  if (*clientSimGetMineView(csPtr) != NULL) {
    Dispose(*clientSimGetMineView(csPtr));
    clientSimSetMineView(csPtr, NULL);
  }
}

/*********************************************************
*NAME:          screenSetup
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 05/05/01
*PURPOSE:
*  Sets up all the variables - Should be run when the
*  program starts.
*
*ARGUMENTS:
*  game - The game type-Open/tournament/strict tournament
*  hiddenMines - Are hidden mines allowed
*  srtDelay    - Game start delay (50th second increments)
*  gmeLen      - Length of the game (in 50ths)
*                (-1 =unlimited)
*********************************************************/
static void screenSetupCS(ClientSim *csPtr, gameType game, bool hiddenMines, int srtDelay, int32_t gmeLen) {
  /* Initialize simulation state in ClientSim */
  clientSimCreate(csPtr, game, hiddenMines, srtDelay, gmeLen);

  /* Initialize rendering state */
  screenRenderSetup(csPtr);

  /* Initialize display variables */
  clientSimGetMapNameMutable(csPtr)[0] = '\0';
  clientSimSetGmeStartDelay(csPtr, srtDelay);
  clientSimSetGmeLength(csPtr, gmeLen);
  clientSimSetTimeStart(csPtr, 0);
}

/*********************************************************
*NAME:          screenDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 05/05/01
*PURPOSE:
*  Destroys the structures Should be called on
*  program exit
*
*ARGUMENTS:
*
*********************************************************/
void screenDestroyCS(ClientSim *csPtr) {
  /* Clean up rendering state first */
  screenRenderDestroy(csPtr);

  /* Clean up simulation state */
  clientSimDestroy(csPtr);
}

/*********************************************************
*NAME:          screenUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 21/01/01
*PURPOSE:
*  Updates the screen. Takes numerous directions
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void screenUpdateCS(ClientSim *csPtr, updateType value) {
  static bool b = FALSE;
  BYTE x; /* X and Y co-ords of the tank */
  BYTE y;
  BYTE px; /* Pixel X and Y co-ords of the tank */
  BYTE py;
  screenGunsight gs;
  screenBullets sBullets;
  screenLgm lgms;
  screenTanks scnTnk;
  BYTE gsX;
  netStatus ns;
  BYTE oldXOffset = clientSimGetXOffset(csPtr);
  BYTE oldYOffset = clientSimGetYOffset(csPtr);

  if (b == TRUE) {
    return;
  }
  ns = clientSimGetNetStatus(csPtr);
  if (ns != netRunning && ns != netFailed) {
    frontEndDrawDownload(csPtr, FALSE);
    return;
  }
  if (clientSimGetGameSim(csPtr)->inStartFind == TRUE) {
    frontEndDrawDownload(csPtr, TRUE);
    return;
  }
  if (clientSimIsNeedScreenReCalc(csPtr) == TRUE) {
    clientSimSetNeedScreenReCalc(csPtr, FALSE);
    screenUpdateViewCS(csPtr, (updateType) 0);
  }

  b = TRUE;

  if (MY_TANK(csPtr) == NULL) {
    b = FALSE;
    return;
  }

  x = tankGetScreenMX(&MY_TANK(csPtr));
  y = tankGetScreenMY(&MY_TANK(csPtr));
  px = tankGetScreenPX(&MY_TANK(csPtr));
  py = tankGetScreenPY(&MY_TANK(csPtr));

  if (px >3 || (x - clientSimGetXOffset(csPtr)) == 0) {
    x++;
  }
  if (py >2) {
    y++;
  }

  screenLgmCreate(&lgms);
  sBullets = screenBulletsCreate();
  screenTanksCreate(&scnTnk);


  switch (value) {
  case redraw:
    break;
  case left:
    if (clientSimIsInPillView(csPtr) == FALSE) {
      if (scrollCheck((BYTE) (clientSimGetXOffset(csPtr)-1), clientSimGetYOffset(csPtr), x, y) == TRUE) {
        clientSimSetXOffset(csPtr, clientSimGetXOffset(csPtr) - 1);
		moveMousePointer(left);
        if (clientSimGetXOffset(csPtr) != oldXOffset) {
          clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) + 1);
        }
      }
    } else {
      screenPillViewCS(csPtr, -1, 0);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  case right:
    if (clientSimIsInPillView(csPtr) == FALSE) {
      if (scrollCheck((BYTE) (clientSimGetXOffset(csPtr)+1), clientSimGetYOffset(csPtr), x, y) == TRUE) {
        clientSimSetXOffset(csPtr, clientSimGetXOffset(csPtr) + 1);
		moveMousePointer(right);
        if (clientSimGetXOffset(csPtr) != oldXOffset) {
          clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) - 1);
        }
      }
    } else {
      screenPillViewCS(csPtr, 1, 0);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  case up:
    if (clientSimIsInPillView(csPtr) == FALSE) {
      if (scrollCheck(clientSimGetXOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr)-1), x, y) == TRUE) {
        clientSimSetYOffset(csPtr, clientSimGetYOffset(csPtr) - 1);
		moveMousePointer(up);
        if (clientSimGetYOffset(csPtr) != oldYOffset) {
          clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) + 1);
        }
      }
    } else {
      screenPillViewCS(csPtr, 0, -1);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  case down:
  default:
    if (clientSimIsInPillView(csPtr) == FALSE) {
      if (scrollCheck(clientSimGetXOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr)+1), x, y) == TRUE) {
        clientSimSetYOffset(csPtr, clientSimGetYOffset(csPtr) + 1);
		moveMousePointer(down);
        if (clientSimGetYOffset(csPtr) != oldYOffset) {
          clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) - 1);
        }
      }
    } else {
      screenPillViewCS(csPtr, 0, 1);
    }
    clientSimSetNeedScreenReCalc(csPtr, TRUE);
    break;
  }


  if (value == redraw) {
    screenTanksPrepare(csPtr, &scnTnk, &MY_TANK(csPtr), clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y));
    screenLgmPrepare(csPtr, &lgms, clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1 ), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y-1));
    if (tankIsGunsightShow(&MY_TANK(csPtr)) == TRUE) {
      tankGetGunsight(&MY_TANK(csPtr), &gsX, &(gs.mapY), &(gs.pixelX), &(gs.pixelY));
      gs.mapX = gsX;

      if (gs.mapX >= clientSimGetXOffset(csPtr) && gs.mapX < (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1) && gs.mapY >= clientSimGetYOffset(csPtr) && gs.mapY < (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y -1 )) {
        gs.mapX -= clientSimGetXOffset(csPtr);
        gs.mapY -= clientSimGetYOffset(csPtr);
      } else {
        gs.mapX = NO_GUNSIGHT;
      }
    } else {
      gs.mapX = NO_GUNSIGHT;
    }

    /* Server shell snapshots (other players' shells only — own shells filtered
     * during snapshot sync in screenSyncFromSnapshotCS) */
    {
      int si;
      BYTE myPlayer = clientSimGetInterpCtx(csPtr)->localPlayer;
      for (si = 0; si < clientSimGetServerShellCount(csPtr); si++) {
        BYTE smx = (BYTE)(clientSimGetServerShellSnaps(csPtr)[si].worldX >> TANK_SHIFT_MAPSIZE);
        BYTE smy = (BYTE)(clientSimGetServerShellSnaps(csPtr)[si].worldY >> TANK_SHIFT_MAPSIZE);
        if (clientSimGetServerShellSnaps(csPtr)[si].owner == myPlayer) {
          continue;
        }
        if (smx >= clientSimGetXOffset(csPtr) && smx < (BYTE)(clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X - 1) &&
            smy >= clientSimGetYOffset(csPtr) && smy < (BYTE)(clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y - 1)) {
          WORLD conv;
          BYTE spx, spy, sframe;
          conv = clientSimGetServerShellSnaps(csPtr)[si].worldX;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          spx = (BYTE)conv;
          conv = clientSimGetServerShellSnaps(csPtr)[si].worldY;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          spy = (BYTE)conv;
          sframe = (BYTE)(utilGetDir((TURNTYPE)clientSimGetServerShellSnaps(csPtr)[si].angle) + SHELL_START_EXPLODE + 1);
          screenBulletsAddItem(&sBullets, (BYTE)(smx - clientSimGetXOffset(csPtr)), (BYTE)(smy - clientSimGetYOffset(csPtr)), spx, spy, sframe);
        }
      }
    }
    /* Client-predicted shells (local player only) */
    {
      int pi;
      for (pi = 0; pi < clientSimGetPredictedShellCount(csPtr); pi++) {
        const PredictedShell *ps = &clientSimGetPredictedShells(csPtr)[pi];
        /* Don't render shells that have expired — prevents ghost frame alongside explosion */
        if (ps->length <= SHELL_DEATH) continue;
        BYTE pmx = (BYTE)(ps->x >> TANK_SHIFT_MAPSIZE);
        BYTE pmy = (BYTE)(ps->y >> TANK_SHIFT_MAPSIZE);
        if (pmx >= clientSimGetXOffset(csPtr) && pmx < (BYTE)(clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X - 1) &&
            pmy >= clientSimGetYOffset(csPtr) && pmy < (BYTE)(clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y - 1)) {
          WORLD conv;
          BYTE ppx, ppy, pframe;
          conv = ps->x;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          ppx = (BYTE)conv;
          conv = ps->y;
          conv <<= TANK_SHIFT_MAPSIZE;
          conv >>= TANK_SHIFT_PIXELSIZE;
          ppy = (BYTE)conv;
          pframe = (BYTE)(utilGetDir(ps->angle) + SHELL_START_EXPLODE + 1);
          screenBulletsAddItem(&sBullets, (BYTE)(pmx - clientSimGetXOffset(csPtr)), (BYTE)(pmy - clientSimGetYOffset(csPtr)), ppx, ppy, pframe);
        }
      }
    }
    explosionsCalcScreenBullets(&clientSimGetGameSim(csPtr)->expl, &sBullets, clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y-1));
    tkExplosionCalcScreenBullets(&clientSimGetGameSim(csPtr)->tankExplosions, &sBullets, clientSimGetXOffset(csPtr), (BYTE) (clientSimGetXOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_X-1), clientSimGetYOffset(csPtr), (BYTE) (clientSimGetYOffset(csPtr) + MAIN_BACK_BUFFER_SIZE_Y-1));
    frontEndDrawMainScreen(csPtr, clientSimGetView(csPtr), clientSimGetMineView(csPtr), &scnTnk, &gs, &sBullets, &lgms, clientSimGetGmeStartDelay(csPtr), clientSimIsInPillView(csPtr), &MY_TANK(csPtr), 0, 0);
  }
  screenBulletsDestroy(&sBullets);
  screenLgmDestroy(&lgms);
  screenTanksDestroy(&scnTnk);
  b = FALSE;
}


/*********************************************************
*NAME:          screenGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 30/12/2008
*PURPOSE:
*  Gets the value of a square in the structure
*  Return RIVER if out of range
*
*ARGUMENTS:
*  value  - Pointer to the screen structure
*  xValue - The X co-ordinate
*  yValue - The Y co-ordinate
*********************************************************/
BYTE screenGetPos(screen *value,BYTE xValue, BYTE yValue) {
  BYTE returnValue = RIVER; /* Value to return */

  if (xValue < MAIN_BACK_BUFFER_SIZE_X && yValue < MAIN_BACK_BUFFER_SIZE_Y) {
    returnValue = (*value)->screenItem[xValue][yValue];
  }
  return returnValue;
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

  if (xValue <= MAIN_SCREEN_SIZE_X && yValue <= MAIN_SCREEN_SIZE_Y) {
    returnValue = ((*value)->mineItem[xValue][yValue]);
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
void screenUpdateViewCS(ClientSim *csPtr, updateType value) {
/* NOTE: value UNUSED */
  BYTE count;   /* Looping Variables */
  BYTE count2;

  for (count=0;count<MAIN_BACK_BUFFER_SIZE_X;count++) {
    for (count2=0;count2<MAIN_BACK_BUFFER_SIZE_Y;count2++) {
      (*clientSimGetView(csPtr))->screenItem[count][count2] = screenCalcSquareCS(csPtr, (BYTE) (count+clientSimGetXOffset(csPtr)),(BYTE) (count2+clientSimGetYOffset(csPtr)), count, count2);
      screenBrainMapSetPos(clientSimGetBrainMap(csPtr), (BYTE) (count+clientSimGetXOffset(csPtr)), (BYTE) (count2+clientSimGetYOffset(csPtr)), mapGetPos(&clientSimGetGameSim(csPtr)->mp, (BYTE) (count+clientSimGetXOffset(csPtr)), (BYTE) (count2+clientSimGetYOffset(csPtr))), minesExistPos(&clientSimGetGameSim(csPtr)->mns, &clientSimGetGameSim(csPtr)->mp, (BYTE) (count+clientSimGetXOffset(csPtr)), (BYTE) (count2+clientSimGetYOffset(csPtr))));
    }
  }
}

/*********************************************************
*NAME:          screenCalcSquare
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 30/01/02
*PURPOSE:
*  Calculates the terrain type for a given location
*
*ARGUMENTS:
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate
*********************************************************/
BYTE screenCalcSquareCS(ClientSim *csPtr, BYTE xValue, BYTE yValue, BYTE scrX, BYTE scrY) {
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

  (*clientSimGetMineView(csPtr))->mineItem[scrX][scrY] = FALSE;
  /* Set up Items */
  if ((pillsExistPos(&clientSimGetGameSim(csPtr)->pb,xValue,yValue)) == TRUE) {
    returnValue = pillsGetScreenHealth(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, xValue, yValue);
  } else if ((basesExistPos(&clientSimGetGameSim(csPtr)->bs,xValue,yValue)) == TRUE) {
     ba = basesGetAlliancePos(clientSimGetGameSim(csPtr), xValue, yValue);
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
      if (basesAmOwner(clientSimGetGameSim(csPtr), clientSimGetMyPlayerNum(csPtr), xValue, yValue) == TRUE) {
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
    currentPos = mapGetPos(&clientSimGetGameSim(csPtr)->mp,xValue,yValue);
	if (currentPos>=HALFBUILDING+MINE_SUBTRACT&&currentPos!=DEEP_SEA){
	  minesRemoveItem(&clientSimGetGameSim(csPtr)->mns, xValue, yValue);
	  (*clientSimGetMineView(csPtr))->mineItem[scrX][scrY] = FALSE;
	  currentPos = currentPos - MINE_SUBTRACT;
	  mapSetPos(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->mp, xValue, yValue, currentPos,TRUE,TRUE);
	}
    if (mapIsMine(&clientSimGetGameSim(csPtr)->mp, xValue, yValue) == TRUE) {
      if (minesExistPos(&clientSimGetGameSim(csPtr)->mns, &clientSimGetGameSim(csPtr)->mp, xValue, yValue) == TRUE) {
        (*clientSimGetMineView(csPtr))->mineItem[scrX][scrY] = TRUE;
      }
	  if (currentPos != DEEP_SEA) {
        currentPos = currentPos - MINE_SUBTRACT;
      }
    } else {
      (*clientSimGetMineView(csPtr))->mineItem[scrX][scrY] = FALSE;
    }

    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, (BYTE) (xValue-1), (BYTE) (yValue-1)) == TRUE) {
      aboveLeft = ROAD;
    } else {
      aboveLeft = mapGetPos(&clientSimGetGameSim(csPtr)->mp,(BYTE) (xValue-1),(BYTE) (yValue-1));
      if (aboveLeft >= MINE_START && aboveLeft <= MINE_END) {
        aboveLeft = aboveLeft - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, xValue, (BYTE) (yValue-1)) == TRUE) {
      above = ROAD;
    } else {
      above = mapGetPos(&clientSimGetGameSim(csPtr)->mp,xValue,(BYTE) (yValue-1));
      if (above >= MINE_START && above <= MINE_END) {
        above = above - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, (BYTE) (xValue+1), (BYTE) (yValue-1)) == TRUE) {
      aboveRight = ROAD;
    } else {
      aboveRight = mapGetPos(&clientSimGetGameSim(csPtr)->mp,(BYTE) (xValue+1),(BYTE) (yValue-1));
      if (aboveRight >= MINE_START && aboveRight <= MINE_END) {
        aboveRight = aboveRight - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, (BYTE) (xValue-1), yValue) == TRUE) {
      leftPos = ROAD;
    } else {
      leftPos = mapGetPos(&clientSimGetGameSim(csPtr)->mp,(BYTE) (xValue-1),yValue);
      if (leftPos >= MINE_START && leftPos <= MINE_END) {
        leftPos = leftPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, (BYTE) (xValue+1), yValue) == TRUE) {
      rightPos = ROAD;
    } else {
      rightPos = mapGetPos(&clientSimGetGameSim(csPtr)->mp,(BYTE) (xValue+1),yValue);
      if (rightPos >= MINE_START && rightPos <= MINE_END) {
        rightPos = rightPos - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, (BYTE) (xValue-1), (BYTE) (yValue+1)) == TRUE) {
      belowLeft = ROAD;
    } else {
      belowLeft = mapGetPos(&clientSimGetGameSim(csPtr)->mp,(BYTE) (xValue-1),(BYTE) (yValue+1));
      if (belowLeft >= MINE_START && belowLeft <= MINE_END) {
        belowLeft = belowLeft - MINE_SUBTRACT;
      }
    }


    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, xValue, (BYTE) (yValue+1)) == TRUE) {
      below = ROAD;
    } else {
      below = mapGetPos(&clientSimGetGameSim(csPtr)->mp,xValue,(BYTE) (yValue+1));
      if (below >= MINE_START && below <= MINE_END) {
        below = below - MINE_SUBTRACT;
      }
    }

    if (basesExistPos(&clientSimGetGameSim(csPtr)->bs, (BYTE) (xValue+1), (BYTE) (yValue+1)) == TRUE) {
      belowRight = ROAD;
    } else {
      belowRight = mapGetPos(&clientSimGetGameSim(csPtr)->mp,(BYTE) (xValue+1),(BYTE) (yValue+1));
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
    case CRATER:
      returnValue = screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
      break;
    default:
      returnValue = currentPos;
      break;
    }
  }
  return returnValue;
}


/*********************************************************
*NAME:          screenLoadMap
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 30/01/02
*PURPOSE:
*  Loads a map. Returns if it was sucessful reading the
*  map or not.
*
*ARGUMENTS:
* fileName - File name and path to map to open
*  game - The game type-Open/tournament/strict tournament
*  hiddenMines - Are hidden mines allowed
*  srtDelay    - Game start delay (50th second increments)
*  gmeLen      - Length of the game (in 50ths) 
*                (-1 =unlimited)
*  playerName  - Name of the player
*  wantFree    - Should we free the backend after loading
*                Usually TRUE if you only want to check
*                if a map is valid
*********************************************************/
bool screenLoadMapCS(ClientSim *csPtr, char *fileName, gameType game, bool hiddenMines, int32_t srtDelay, int32_t gmeLen, char *playerName, bool wantFree) {
  bool returnValue; /* Value to return */
  bool doneFree;    /* If we have done the free */

  returnValue = FALSE;
  doneFree = FALSE;
  screenSetupCS(csPtr, game, hiddenMines,srtDelay,gmeLen);
  returnValue = mapRead(fileName, &clientSimGetGameSim(csPtr)->mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &clientSimGetGameSim(csPtr)->ss);

/*
 * Used to write out linux compressed map file.
  {
  BYTE buff[16*1024];
  int buffLen;
  FILE *fp;

  fp = fopen("c:\\map.txt", "wb");
  buffLen = mapSaveCompressedMap(&mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &ss, buff);
  fwrite(buff, buffLen, 1, fp);
  fclose(fp);

  }   */

  if (returnValue == TRUE) {
    utilExtractMapName(fileName, clientSimGetMapNameMutable(csPtr));
    utilStripNameReplace(playerName);
    screenSetupTankCS(csPtr, playerName, 0);
    screenUpdateViewCS(csPtr, redraw);
    basesClearMines(clientSimGetGameSim(csPtr));

  } else {
    screenDestroyCS(csPtr);
    doneFree = TRUE;
  }
  if (doneFree == FALSE && wantFree == TRUE) {
    screenDestroyCS(csPtr);
  }

  return returnValue;
}


bool screenLoadCompressedMapCS(ClientSim *csPtr, BYTE *buff, int buffLen, char *mapn, gameType game, bool hiddenMines, int32_t srtDelay, int32_t gmeLen, char *playerName, BYTE playerNum, bool wantFree) {
  bool returnValue;
  bool doneFree = FALSE;

  screenSetupCS(csPtr, game, hiddenMines, srtDelay, gmeLen);
  /* clientSimCreate (inside screenSetupCS) resets myPlayerNum to 0.
   * Restore the correct player number before creating the tank so it
   * ends up in the right sim.tanks[] slot. This also moves the LGM
   * and sets the base refuel timer for the correct index. */
  if (playerNum != 0) {
    clientSimSetPlayerNum(csPtr, playerNum);
  }
  returnValue = mapLoadCompressedMap(&clientSimGetGameSim(csPtr)->mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &clientSimGetGameSim(csPtr)->ss, buff, buffLen);

  if (returnValue == TRUE) {
    strncpy(clientSimGetMapNameMutable(csPtr), mapn, MAP_STR_SIZE - 1);
    clientSimGetMapNameMutable(csPtr)[MAP_STR_SIZE - 1] = '\0';
    utilStripNameReplace(playerName);
    screenSetupTankCS(csPtr, playerName, playerNum);
    screenUpdateViewCS(csPtr, redraw);
    basesClearMines(clientSimGetGameSim(csPtr));
  } else {
    screenDestroyCS(csPtr);
    doneFree = TRUE;
  }
  if (doneFree == FALSE && wantFree == TRUE) {
    screenDestroyCS(csPtr);
  }
  return returnValue;
}


/*********************************************************
*NAME:          screenTranslateBrainButtons
*AUTHOR:        John Morrison
*CREATION DATE: 27/11/99
*LAST MODIFIED: 27/11/99
*PURPOSE:
* Translates the brain's key pressed into our keys presses
*.
*ARGUMENTS:
* isShoot    - Pointer to hold if we are shooting
* isGameTick - TRUE if this is a game tick
*********************************************************/
tankButton screenTranslateBrainButtonsCS(ClientSim *csPtr, bool *isShoot, bool isGameTick) {
  tankButton returnValue; /* Value to return */
  unsigned long temp;     /* Used to temp store brainHoldKeys */
  unsigned long temp2;    /* Used to temp store keyShoot if not game tick */
  uint32_t *holdKeys = clientSimGetBrainHoldKeys(csPtr);
  uint32_t *tapKeys = clientSimGetBrainTapKeys(csPtr);

  temp = *holdKeys;
  temp2 = 0;

  /* Handle tap keys */
  if (testkey(*tapKeys, KEY_faster)) {
    setkey(*holdKeys, KEY_faster);
  }
  if (testkey(*tapKeys, KEY_slower)) {
    setkey(*holdKeys, KEY_slower);
  }
  if (testkey(*tapKeys, KEY_turnleft)) {
    setkey(*holdKeys, KEY_turnleft);
  }
  if (testkey(*tapKeys, KEY_turnright)) {
    setkey(*holdKeys, KEY_turnright);
  }
  if (testkey(*tapKeys, KEY_morerange)) {
    tankGunsightIncrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*tapKeys, KEY_lessrange)) {
    tankGunsightDecrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*tapKeys, KEY_shoot)) {
    if (isGameTick == TRUE) {
      *isShoot = TRUE;
    } else {
      setkey(temp2, KEY_shoot);
    }
  }
  if (testkey(*tapKeys, KEY_dropmine)) {
    tankLayMine(clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*tapKeys, KEY_TankView)) {
    clientSimSetInPillView(csPtr, FALSE);
    clientCenterTankCS(csPtr);
  }
  if (testkey(*tapKeys, KEY_PillView)) {
    screenPillViewCS(csPtr, 0, 0);
  }

  *tapKeys = temp2;

  /* Do the lookups */
  if (testkey(*holdKeys, KEY_faster) && testkey(*holdKeys, KEY_turnright)) {
    returnValue = TRIGHTACCEL;
  } else if (testkey(*holdKeys, KEY_faster) && testkey(*holdKeys, KEY_turnleft)) {
    returnValue = TLEFTACCEL;
  } else if (testkey(*holdKeys, KEY_slower) && testkey(*holdKeys, KEY_turnleft)) {
    returnValue = TLEFTDECEL;
  } else if (testkey(*holdKeys, KEY_slower) && testkey(*holdKeys, KEY_turnright)) {
    returnValue = TRIGHTDECEL;
  } else if (testkey(*holdKeys, KEY_faster)) {
    returnValue = TACCEL;
  } else if (testkey(*holdKeys, KEY_slower)) {
    returnValue = TDECEL;
  } else if (testkey(*holdKeys, KEY_turnleft)) {
    returnValue = TLEFT;
  } else if (testkey(*holdKeys, KEY_turnright)) {
    returnValue = TRIGHT;
  } else {
    returnValue = TNONE;
  }

  /* Restore it */
  *holdKeys = temp;

  /* Handle remaining keys */
  if (testkey(*holdKeys, KEY_morerange)) {
    tankGunsightIncrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*holdKeys, KEY_lessrange)) {
    tankGunsightDecrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*holdKeys, KEY_shoot) && isGameTick == TRUE) {
    *isShoot = TRUE;
  }
  if (testkey(*holdKeys, KEY_dropmine)) {
    tankLayMine(clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
  if (testkey(*holdKeys, KEY_TankView)) {
    clientSimSetInPillView(csPtr, FALSE);
    clientCenterTankCS(csPtr);
  }
  if (testkey(*holdKeys, KEY_PillView)) {
    screenPillViewCS(csPtr, 0, 0);
  }

  return returnValue;
}

/* screenGameTick and screenKeysTick have been removed.
 * All frontends now use the server-authoritative path:
 *   clientSimKeysTick / clientSimGameTick (prediction)
 *   + transport->sendInput / transport->tick / transport->getSnapshot
 *   + clientSimSyncFromSnapshot (reconciliation)
 *   + clientSimDisplayTick (display updates)
 */

/*********************************************************
*NAME:          screenBaseAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the base alliance of a particular base for 
*  drawing.
*
*ARGUMENTS:
*  baseNum - The base number to get
*********************************************************/
baseAlliance screenBaseAllianceCS(ClientSim *csPtr, BYTE baseNum) {
  return basesGetStatusNum(clientSimGetGameSim(csPtr), baseNum);
}


/*********************************************************
*NAME:          screenPillAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the pill alliance of a particular pill for 
*  status drawing.
*
*ARGUMENTS:
*  pillNum - The pillbox number to get
*********************************************************/
pillAlliance screenPillAllianceCS(ClientSim *csPtr, BYTE pillNum) {
  return pillsGetAllianceNum(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, pillNum);
}


/*********************************************************
*NAME:          screenGetTankStats
*AUTHOR:        John Morrison
*CREATION DATE: 22/12/98
*LAST MODIFIED: 22/12/98
*PURPOSE:
*  Returns the tank shells, mines, armour and trees 
*
*ARGUMENTS:
*  shellsAmount - Pointer to hold number of shells
*  minesAmount  - Pointer to hold number of mines
*  armourAmount - Pointer to hold amount of armour
*  treesAmount  - Pointer to hold amount of trees
*********************************************************/
void screenGetTankStatsCS(ClientSim *csPtr, BYTE *shellsAmount, BYTE *minesAmount, BYTE *armourAmount, BYTE *treesAmount) {
  tankGetStats(&MY_TANK(csPtr), shellsAmount, minesAmount, armourAmount, treesAmount);
  if (*armourAmount > TANK_FULL_ARMOUR) {
    *armourAmount = 0;
  }
}

/*********************************************************
*NAME:          screenGunsightRange
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 24/12/98
*PURPOSE:
*  Changes the gunsight range
*
*ARGUMENTS:
*  increase - Set to TRUE if should increase else 
*             decrease range
*********************************************************/
void screenGunsightRangeCS(ClientSim *csPtr, bool increase) {
  if (increase == TRUE) {
    tankGunsightIncrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  } else {
    tankGunsightDecrease(csPtr, clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  }
}


/*********************************************************
*NAME:          screenGunsightRange
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 24/12/98
*PURPOSE:
*  Shows / Hides the gunsight
*
*ARGUMENTS:
*  shown - TRUE = Gunsight on.
*********************************************************/
void screenSetGunsightCS(ClientSim *csPtr, bool shown) {
  tankSetGunsight(&MY_TANK(csPtr), shown);
}


/*********************************************************
*NAME:          screenReCalc
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED: 30/12/98
*PURPOSE:
*  Recalculates the screen data
*
*ARGUMENTS:
*
*********************************************************/
void screenReCalcCS(ClientSim *csPtr) {
  clientSimSetNeedScreenReCalc(csPtr, TRUE);
}


/*********************************************************
*NAME:          screenGetMessages
*AUTHOR:        John Morrison
*CREATION DATE: 3/1/99
*LAST MODIFIED: 3/1/99
*PURPOSE:
*  Gets the messages on screen
*
*ARGUMENTS:
*  top - Top message line to get
*  top - Top message line to get
*********************************************************/
void screenGetMessages(ClientSim *csPtr, char *top, char *bottom) {
  messageGetMessage(clientSimGetMessages(csPtr), top,bottom);
}

/*********************************************************
*NAME:          clientCenterTank
*AUTHOR:        John Morrison
*CREATION DATE: 5/11/99
*LAST MODIFIED: 5/11/99
*PURPOSE:
*  Centers the screen around the tank.
*
*ARGUMENTS:
*
*********************************************************/
void clientCenterTankCS(ClientSim *csPtr) {
  BYTE high, low, health, dummy;

  tankGetStats(&MY_TANK(csPtr), &high, &low, &health, &dummy);
  if (health <= TANK_FULL_ARMOUR) {
    /* Tank isn't dead */
    scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), (tankGetMX(&MY_TANK(csPtr))), (tankGetMY(&MY_TANK(csPtr))));
    screenReCalcCS(csPtr);
  }
}

/*********************************************************
*NAME:          screenSetupTank
*AUTHOR:        John Morrison
*CREATION DATE: 3/1/99
*LAST MODIFIED: 21/1/01
*PURPOSE:
*  Creates and sets up a tank. MUST be called after
*  map is loaded and screen setup is called.
*  Should only be called directly by single player
*  games.
*
*ARGUMENTS:
*  playerName - The player name controling the tank
*********************************************************/
void screenSetupTankCS(ClientSim *csPtr, char *playerName, BYTE playerNum) {
  /* Self is the local source of truth for client identity — PLAYER_JOINED
   * and PLAYER_LIST receive paths skip self, so without this the local
   * row would stay CLIENT_TYPE_UNKNOWN. */
  uint8_t selfType  = bolo_detect_client_type();
  uint8_t selfFlags = 0;
#ifdef HAVE_STEAM
  selfFlags |= PLAYER_FLAG_STEAM_BUILD;
#endif
  if (bolo_steam_has_supporter_dlc()) selfFlags |= PLAYER_FLAG_SUPPORTER;

  clientSimSetupSelf(csPtr, playerNum, playerName, selfType, selfFlags);

  { BYTE sh, mi, ar, tr;
    tankGetStats(&MY_TANK(csPtr), &sh, &mi, &ar, &tr);
    frontEndUpdateTankStatusBars(csPtr, sh, mi, ar, tr);
  }
  frontEndSetPlayer(csPtr, (playerNumbers) clientSimGetMyPlayerNum(csPtr), playerName, "", 0, selfType, selfFlags);
}

/*********************************************************
*NAME:          screenGetKillsDeaths
*AUTHOR:        John Morrison
*CREATION DATE:  8/1/99
*LAST MODIFIED:  8/1/99
*PURPOSE:
*  Gets the tanks kills/deaths
*
*ARGUMENTS:
*  kills  - The number of kills the tank has.
*  deaths - The number of times the tank has died
*********************************************************/
void screenGetKillsDeathsCS(ClientSim *csPtr, int *kills, int *deaths) {
  tankGetKillsDeaths(&MY_TANK(csPtr), kills, deaths);
}

/*********************************************************
*NAME:          screenShowMessages
*AUTHOR:        John Morrison
*CREATION DATE:  8/1/99
*LAST MODIFIED:  1/6/00
*PURPOSE:
*  Turns on/off messages menus stuff
*
*ARGUMENTS:
*  msgType - The message type that is being set
*  isShown - Is it being turned on or off
*********************************************************/
void screenShowMessages(ClientSim *csPtr, BYTE msgType, bool isShown) {
  switch (msgType) {
  case MSG_NEWSWIRE:
    messageSetNewswire(clientSimGetMessages(csPtr), isShown);
    break;
  case MSG_ASSISTANT:
    messageSetAssistant(clientSimGetMessages(csPtr), isShown);
    break;
  case MSG_AI:
    messageSetAI(clientSimGetMessages(csPtr), isShown);
    break;
  case MSG_NETSTATUS:
    messageSetNetStatus(clientSimGetMessages(csPtr), isShown);
    break;
  default:
    /* MSG_Network */
    messageSetNetwork(clientSimGetMessages(csPtr), isShown);
    break;
  }
}

/*********************************************************
*NAME:          screenManMove
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 27/5/00
*PURPOSE:
* The mouse has been clicked indicating a build operation
* is requested. Send request to lgm structure
*
*ARGUMENTS:
*  buildS - The building type selected
*********************************************************/
void screenManMoveCS(ClientSim *csPtr, buildSelect buildS) {
  if (tankGetArmour(&MY_TANK(csPtr)) <= TANK_FULL_ARMOUR && clientSimGetNetStatus(csPtr) != netFailed) {
    /* Route build request through InputPacket so the server sim
     * processes it authoritatively (matches brain build path). */
    clientSimSetPendingBuild(csPtr,
                             (BYTE) buildS + 1,  /* 1-based in InputPacket (0=none) */
                             (BYTE) (clientSimGetCursorPosX(csPtr) + clientSimGetXOffset(csPtr)),
                             (BYTE) (clientSimGetCursorPosY(csPtr) + clientSimGetYOffset(csPtr)));
  }
}


/*********************************************************
*NAME:          screenSetAutoScroll
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 16/1/99
*PURPOSE:
* The autoscrolling item has been checked on the frontend
* Pass the value onto the scroll module.
*
*ARGUMENTS:
*  isAuto - Is the scrolling option automatic or not?
*********************************************************/
void screenSetAutoScroll(ClientSim *csPtr, bool isAuto) {
  scrollSetScrollType(clientSimGetScroll(csPtr), isAuto);
}

/*********************************************************
*NAME:          screenLgmDropPill
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 14/3/99
*PURPOSE:
* Man has died and needs to drop a pill. Pass this onto
* the pillbox structure
*
*ARGUMENTS:
*  mx      - Pointer to hold Map X Co-ordinates
*  my      - Pointer to hold Map Y Co-ordinates
*  owner   - Who owns the pill
*  pillNum - Pill number to place 
*********************************************************/
void screenLgmDropPillCS(ClientSim *csPtr, BYTE mx, BYTE my, BYTE owner, BYTE pillNum) {
  pillbox item;   /* Item to add to the pillbox */

  item.armour = 0;
  item.owner = owner;
  item.speed = PILLBOX_ATTACK_NORMAL;
  item.reload = PILLBOX_ATTACK_NORMAL;
  item.coolDown = 0;
  item.inTank = FALSE;
  item.x = mx;
  item.y = my;
  item.justSeen = FALSE;
  pillsSetPill(&clientSimGetGameSim(csPtr)->pb,&item, pillNum);
  frontEndStatusPillbox(csPtr, pillNum, (pillsGetAllianceNum(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, pillNum)));
}

/*********************************************************
*NAME:          screenTankLayMine
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 21/1/99
*PURPOSE:
* Request for tank to lay mine has occured
*
*ARGUMENTS:
*
*********************************************************/
void screenTankLayMineCS(ClientSim *csPtr) {
  tankLayMine(clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
}

/*********************************************************
*NAME:          screenCheckTankMineDamage
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 21/1/99
*PURPOSE:
* Check for damage to a tank by a mine being set off.
*
*ARGUMENTS:
*  mx - X Map Co-ordinate
*  my - Y Map Co-ordinate
*********************************************************/
void screenCheckTankMineDamageCS(ClientSim *csPtr, BYTE mx, BYTE my) {
  tankMineDamage(clientSimGetGameSim(csPtr), &MY_TANK(csPtr), mx, my);
}

/*********************************************************
*NAME:          screenPillNumPos
*AUTHOR:        John Morrison
*CREATION DATE: 23/1/99
*LAST MODIFIED: 23/1/99
*PURPOSE:
* The front end needs the pillbox number at a specific
* location for drawing its label
*
*ARGUMENTS:
*  mx - X Map Co-ordinate relative to the screen
*  my - Y Map Co-ordinate relative to the screen
*********************************************************/
BYTE screenPillNumPosCS(ClientSim *csPtr, BYTE mx, BYTE my) {
  return pillsGetPillNum(&clientSimGetGameSim(csPtr)->pb, (BYTE) (clientSimGetXOffset(csPtr)+mx), (BYTE) (clientSimGetYOffset(csPtr)+my), FALSE, FALSE);
}


/*********************************************************
*NAME:          screenBaseNumPos
*AUTHOR:        John Morrison
*CREATION DATE: 23/1/99
*LAST MODIFIED: 23/1/99
*PURPOSE:
* The front end needs the base number at a specific
* location for drawing its label
*
*ARGUMENTS:
*  mx - X Map Co-ordinate relative to the screen
*  my - Y Map Co-ordinate relative to the screen
*********************************************************/
BYTE screenBaseNumPosCS(ClientSim *csPtr, BYTE mx, BYTE my) {
  return basesGetBaseNum(&clientSimGetGameSim(csPtr)->bs, (BYTE) (clientSimGetXOffset(csPtr)+mx), (BYTE) (clientSimGetYOffset(csPtr)+my));
}


/*********************************************************
*NAME:          screenGetMapName
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
* The front end eants to know what the map name is.
* Make a copy for it.
*
*ARGUMENTS:
*  value - Place to hold copy of the map name
*********************************************************/
void screenGetMapNameCS(ClientSim *csPtr, char *value) {
  strcpy(value, clientSimGetMapName(csPtr));
}


/*********************************************************
*NAME:          screenGetNumPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
* Returns the number of players in the game
*
*ARGUMENTS:
*
*********************************************************/
BYTE screenGetNumPlayersCS(ClientSim *csPtr) {
  return playersGetNumPlayers(&clientSimGetGameSim(csPtr)->plyrs);
}


/*********************************************************
*NAME:          screenGetAllowHiddenMines
*AUTHOR:        John Morrison
*CREATION DATE: 26/1/99
*LAST MODIFIED: 26/1/99
*PURPOSE:
* Returns whether hidden mines are allowed in the game
* or not
*
*ARGUMENTS:
*
*********************************************************/
bool screenGetAllowHiddenMinesCS(ClientSim *csPtr) {
  return minesGetAllowHiddenMines(&clientSimGetGameSim(csPtr)->mns);
}


/*********************************************************
*NAME:          screenSetAllowHiddenMines
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets whether hidden mines are allowed or not 
*
*ARGUMENTS:
*  hidden - TRUE if hidden mines are allowed
*********************************************************/
void screenSetAllowHiddenMinesCS(ClientSim *csPtr, bool hidden) {
  minesDestroy(&clientSimGetGameSim(csPtr)->mns);
  minesCreate(&clientSimGetGameSim(csPtr)->mns, hidden);
}


/*********************************************************
*NAME:          screenGetGameTimeLeft
*AUTHOR:        John Morrison
*CREATION DATE: 27/1/99
*LAST MODIFIED: 27/1/99
*PURPOSE:
* Returns the time remaining the current game
*
*ARGUMENTS:
*
*********************************************************/
int32_t screenGetGameTimeLeftCS(ClientSim *csPtr) {
  return clientSimGetGmeLength(csPtr);
}


/*********************************************************
*NAME:          screenGetGameStartDelay
*AUTHOR:        John Morrison
*CREATION DATE: 27/1/99
*LAST MODIFIED: 27/1/99
*PURPOSE:
* Returns the time remaining to start the current game
*
*ARGUMENTS:
*
*********************************************************/
int32_t screenGetGameStartDelayCS(ClientSim *csPtr) {
  return clientSimGetGmeStartDelay(csPtr);
}



/*********************************************************
*NAME:          screenGetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 1/2/99
*LAST MODIFIED: 1/2/99
*PURPOSE:
* Copies the player name into the value passed
*
*ARGUMENTS:
*  value - String to hold copy of the player name
*********************************************************/
void screenGetPlayerNameCS(ClientSim *csPtr, char *value) {
  if (clientSimGetGameSim(csPtr)->plyrs == NULL) {
    strcpy(value, clientSimGetMyLastPlayerName(csPtr));
  } else {
    playersGetPlayerName(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), value, FALSE);
  }
}


/*********************************************************
*NAME:          screenSetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 01/02/99
*LAST MODIFIED: 30/01/02
*PURPOSE:
* Checks to see if the new name is taken. If not the it
* changes the players name to the new name. Returns whether
* the change took place or not
*
*ARGUMENTS:
*  value - New Player Name
*********************************************************/
bool screenSetPlayerNameCS(ClientSim *csPtr, char *value) {
  bool returnValue;              /* Value to return */

  utilStripNameReplace(value);
  returnValue = playersSetPlayerName(csPtr, clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), value, FALSE);
  if (returnValue == TRUE) {
    if (clientSimGetNetType(csPtr) != netSingle) {
      clientSimSendChangePlayerName(csPtr, clientSimGetMyPlayerNum(csPtr), value);
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          screenSetLabelOwnTank
*AUTHOR:        John Morrison
*CREATION DATE: 2/2/99
*LAST MODIFIED: 2/2/99
*PURPOSE:
* Sets whether should label itself to the value passed
*
*ARGUMENTS:
*  value - Should the tank be labeled?
*********************************************************/
void screenSetLabelOwnTank(ClientSim *csPtr, bool value) {
  labelSetLabelOwnTank(csPtr, value);
}

/*********************************************************
*NAME:          screenSetMesageLabelLen
*AUTHOR:        John Morrison
*CREATION DATE: 2/2/99
*LAST MODIFIED: 2/2/99
*PURPOSE:
* Sets the message label len to the value passed
*
*ARGUMENTS:
*  value - New length of the labels
*********************************************************/
void screenSetMesageLabelLen(ClientSim *csPtr, labelLen value) {
  labelSetSenderLength(csPtr, value);
}

/*********************************************************
*NAME:          screenSetTankLabelLen
*AUTHOR:        John Morrison
*CREATION DATE: 2/2/99
*LAST MODIFIED: 2/2/99
*PURPOSE:
* Sets the tank label len to the value passed
*
*ARGUMENTS:
*  value - New length of the labels
*********************************************************/
void screenSetTankLabelLen(ClientSim *csPtr, labelLen value) {
  labelSetTankLength(csPtr, value);
}

/*********************************************************
*NAME:          screenTankView
*AUTHOR:        John Morrison
*CREATION DATE: 3/2/99
*LAST MODIFIED: 3/2/99
*PURPOSE:
* Frontend has requested a tank view.
*
*ARGUMENTS:
*
*********************************************************/
void screenTankViewCS(ClientSim *csPtr) {
  clientSimSetInPillView(csPtr, FALSE);
  clientCenterTankCS(csPtr);
}


/*********************************************************
*NAME:          screenPillView
*AUTHOR:        John Morrison
*CREATION DATE: 03/02/99
*LAST MODIFIED: 21/01/01
*PURPOSE:
* Front end has requested a pill view.
*
*ARGUMENTS:
*  horz - If we are moving left or right (0 for neither)
*  vert - If we are moving up or down (0 for neither)
*********************************************************/
void screenPillViewCS(ClientSim *csPtr, int horz, int vert) {
  bool result; /* Was the operation successful */

  if (clientSimIsInPillView(csPtr) == FALSE) {
    if (pillsCheckView(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr)) == TRUE) {
      clientSimSetInPillView(csPtr, TRUE);
      scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr));
      screenReCalcCS(csPtr);
    } else {
      result = pillsGetNextView(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, clientSimGetPillViewXPtr(csPtr), clientSimGetPillViewYPtr(csPtr), FALSE);
      if (result == TRUE) {
        /* Center on the object */
        clientSimSetInPillView(csPtr, TRUE);
        scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr));
        screenReCalcCS(csPtr);
      } else {
        clientSimSetInPillView(csPtr, FALSE);
      }
    }
  } else {
    if (horz == 0 && vert == 0) {
      result = pillsGetNextView(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, clientSimGetPillViewXPtr(csPtr), clientSimGetPillViewYPtr(csPtr), TRUE);
      if (result == FALSE) {
        screenTankViewCS(csPtr);
      } else {
        /* Center on the object */
        scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr));
        screenReCalcCS(csPtr);
      }
    } else {
      if (pillsMoveView(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, clientSimGetPillViewXPtr(csPtr), clientSimGetPillViewYPtr(csPtr), horz, vert) == TRUE) {
        scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr));
        screenReCalcCS(csPtr);
      }
    }
  }
}


/*********************************************************
*NAME:          screenSendMessageAllPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 5/2/99
*LAST MODIFIED: 24/4/99
*PURPOSE:
* Front end wants to send a message
*
*ARGUMENTS:
*  messageStr - Message to send
*********************************************************/
void screenSendMessageAllPlayersCS(ClientSim *csPtr, char *messageStr) {
  char topLine[FILENAME_MAX];       /* The message topline */

  topLine[0] = '\0';
  playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), topLine);
  clientMessageAdd(clientSimGetMessages(csPtr), (messageType) (clientSimGetMyPlayerNum(csPtr) + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  clientSimMessageSendAllPlayers(csPtr, clientSimGetMyPlayerNum(csPtr), messageStr);
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
*********************************************************/
bool screenSaveMapCS(ClientSim *csPtr, char *fileName) {
  bool returnValue;                 /* Value to return */

  returnValue = mapWrite(fileName, &clientSimGetGameSim(csPtr)->mp, &clientSimGetGameSim(csPtr)->pb, &clientSimGetGameSim(csPtr)->bs, &clientSimGetGameSim(csPtr)->ss);
  if (returnValue == TRUE) {
    if (clientSimGetNetType(csPtr) == netSingle) {
      MessageArgs args;
      memset(&args, 0, sizeof(args));
      playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), args.playerName);
      args.playerFlags = playersGetAccountFlags(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
      playersGetCountryCode(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), args.playerCountry);
      clientSimGetGameSim(csPtr)->callbacks.messageAdd(clientSimGetGameSim(csPtr)->callbacks.ctx, newsWireMessage, MESSAGE_NEWSWIRE, MESSAGE_SAVED_MAP, &args);
    }
  }
  return returnValue;
}


/*********************************************************
*NAME:          screenTanksAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 18/2/99
*LAST MODIFIED: 18/2/99
*PURPOSE:
* Returns the tank allience of a specific player slot.
* sucessful or not.
*
*ARGUMENTS:
*  fileName - path and filename to save
*********************************************************/
tankAlliance screenTankAllianceCS(ClientSim *csPtr, BYTE playerNum) {
  return playersScreenAllience(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), (BYTE) (playerNum-1));
}


/*********************************************************
*NAME:          screenIsItemInTrees
*AUTHOR:        John Morrison
*CREATION DATE: 19/2/99
*LAST MODIFIED:  6/1/00
*PURPOSE:
* Returns whether a item is surrounded by trees.
*
*ARGUMENTS:
*  bmx - X position
*  bmy - Y position
*********************************************************/
bool screenIsItemInTrees(GameSim *sim, tank viewerTank, WORLD bmx, WORLD bmy) {
  bool returnValue; /* Value to return */
  int xDiff;        /* X and Y differences in location */
  int yDiff;

  xDiff = tankGetScreenMX(&viewerTank) - bmx;
  yDiff = tankGetScreenMY(&viewerTank) - bmy;
  if (xDiff >= MIN_SIGHT_DISTANCE_LEFT && xDiff <= MIN_SIGHT_DISTANCE_RIGHT && yDiff >= MIN_SIGHT_DISTANCE_LEFT && yDiff <= MIN_SIGHT_DISTANCE_RIGHT) {
    returnValue = FALSE;
  } else {
    returnValue = utilIsTankInTrees(&sim->mp, &sim->pb, &sim->bs, bmx, bmy);
  }
  return returnValue;
}

/*********************************************************
*NAME:          screenGetNumNeutralPills
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the number of neutral pills
*
*ARGUMENTS:
*
*********************************************************/
BYTE screenGetNumNeutralPillsCS(ClientSim *csPtr) {
  return pillsGetNumNeutral(&clientSimGetGameSim(csPtr)->pb);
}

/*********************************************************
*NAME:          screenGetTimeGameCreated
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the time the game was created
*
*ARGUMENTS:
*
*********************************************************/
int32_t screenGetTimeGameCreatedCS(ClientSim *csPtr) {
  return (int32_t) clientSimGetTimeStart(csPtr);
}

/*********************************************************
*NAME:          screenSetTimeGameCreated
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Sets the time the game was created
*
*ARGUMENTS:
*  value - Time to set it to
*********************************************************/
void screenSetTimeGameCreatedCS(ClientSim *csPtr, int32_t value) {
  clientSimSetTimeStart(csPtr, value);
}

/*********************************************************
*NAME:          screenSetMapName
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets the map name to the parameter passed.
* (Came from network module)
*
*ARGUMENTS:
*  name - Map name
*********************************************************/
void screenSetMapNameCS(ClientSim *csPtr, char *name) {
  strcpy(clientSimGetMapNameMutable(csPtr), name);
}

/*********************************************************
*NAME:          screenSetTimeLengths
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets the time lengths. (Came from network module)
*
*ARGUMENTS:
*  srtDelay    - Game start delay (50th second increments)
*  gmeLen      - Length of the game (in 50ths) 
*                (-1 =unlimited)
*********************************************************/
void screenSetTimeLengthsCS(ClientSim *csPtr, int srtDelay, int32_t gmeLen) {
  clientSimSetGmeStartDelay(csPtr, srtDelay);
  clientSimSetGmeLength(csPtr, gmeLen);
}


/*********************************************************
*NAME:          screenSetGameType
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets the game type. (Came from network module)
*
*ARGUMENTS:
*  gt - The game type to set to
*********************************************************/
void screenSetGameTypeCS(ClientSim *csPtr, gameType gt) {
  gameTypeSet(&clientSimGetGameSim(csPtr)->game, gt);
}

/*********************************************************
*NAME:          screenNetSetupTank
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 13/12/99
*PURPOSE:
*  Sets up the tank so it exists while we are download map
*
*ARGUMENTS:
* isInStart - Are we in In start mode?
*********************************************************/
void screenNetSetupTankCS(ClientSim *csPtr, bool isInStart) {
  clientSimGetGameSim(csPtr)->inStartFind = isInStart;
  tankCreate(clientSimGetGameSim(csPtr), &MY_TANK(csPtr));
  tankSetWorld(clientSimGetGameSim(csPtr), &MY_TANK(csPtr), 0, 0, (TURNTYPE) 0, FALSE);
}

/*********************************************************
*NAME:          screenNetSetupTankGo
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 27/11/99
*PURPOSE:
*  Map download is complete and we are ready to start
*  playing
*
*ARGUMENTS:
*
*********************************************************/
void screenNetSetupTankGoCS(ClientSim *csPtr) {
  BYTE count;   /* Looping variables */
  BYTE count2;

  /* The server is authoritative for tank placement: serverSimAddPlayer
   * has already chosen the start and the first snapshot has copied the
   * position into MY_TANK. Calling startsGetStart on the client here
   * would re-pick locally and, if it disagrees with the server (different
   * sim state at the moment of call), leave the view centered on a spot
   * the tank jumps away from on the next snapshot. Just centre on the
   * existing position. */
  clientCenterTankCS(csPtr);

  for (count = 0; count < MAIN_BACK_BUFFER_SIZE_X; count++) {
    for (count2 = 0; count2 < MAIN_BACK_BUFFER_SIZE_Y; count2++) {
      (*clientSimGetMineView(csPtr))->mineItem[count][count2] = FALSE;
    }
  }

  screenReCalcCS(csPtr);
}

/*********************************************************
*NAME:          screenSetBaseNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 27/2/99
*PURPOSE:
*  We now have the bases data. Set it here
*
*ARGUMENTS:
*  buff - Data buffer containing base data
*  len  - Length of the data
*********************************************************/
void screenSetBaseNetDataCS(ClientSim *csPtr, BYTE *buff, int length) {
  BYTE count; /* Looping variable */
  BYTE max;   /* Max items */
  char pn[PLAYER_NAME_LEN];
  
  basesSetBaseNetData(&clientSimGetGameSim(csPtr)->bs, buff, length);
  count = 1;
  max = basesGetNumBases(&clientSimGetGameSim(csPtr)->bs);
  while (count <= max) {
    frontEndStatusBase(csPtr, count, basesGetStatusNum(clientSimGetGameSim(csPtr), count));
    count++;
  }
  /* Set our player name in the menu */
  max = clientSimGetMyPlayerNum(csPtr);
  playersGetPlayerName(&clientSimGetGameSim(csPtr)->plyrs, max, pn, FALSE);
  {
    char cc[3];
    playersGetCountryCode(&clientSimGetGameSim(csPtr)->plyrs, max, cc);
    frontEndSetPlayer(csPtr, (playerNumbers) max, pn, cc,
                      playersGetPing(&clientSimGetGameSim(csPtr)->plyrs, max),
                      playersGetClientType(&clientSimGetGameSim(csPtr)->plyrs, max),
                      playersGetClientFlags(&clientSimGetGameSim(csPtr)->plyrs, max));
  }
  /* Set The other players in the menu */
  playersSetMenuItems(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), FALSE);
}


/*********************************************************
*NAME:          screenSetPillNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
*  We now have the pillbox data. Set it here
*
*ARGUMENTS:
*  buff    - Data buffer containing pill data
*  dataLen - Length of the data
*********************************************************/
void screenSetPillNetDataCS(ClientSim *csPtr, BYTE *buff, BYTE dataLen) {
  BYTE count; /* Looping variable */
  BYTE max;   /* Max items */
  
  pillsSetPillNetData(&clientSimGetGameSim(csPtr)->pb, buff, dataLen);
  count = 1;
  max = pillsGetNumPills(&clientSimGetGameSim(csPtr)->pb);
  while (count <= max) {
    frontEndStatusPillbox(csPtr, count, pillsGetAllianceNum(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, count));
    count++;
  }
}

/*********************************************************
*NAME:          screenSetStartsNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
*  We now have the starts data. Set it here
*
*ARGUMENTS:
*  buff    - Data buffer containing starts data
*  dataLen - Length of the data
*********************************************************/
void screenSetStartsNetDataCS(ClientSim *csPtr, BYTE *buff, BYTE dataLen) {
  startsSetStartNetData(&clientSimGetGameSim(csPtr)->ss, buff, dataLen);
}

/*********************************************************
*NAME:          screenMakeShellData
*AUTHOR:        John Morrison
*CREATION DATE: 6/3/99
*LAST MODIFIED: 6/3/99
*PURPOSE:
*  Makes the shells token data. Returns the length of the
*  data
*
*ARGUMENTS:
*  buff - Pointer to hold Packet data
*********************************************************/
BYTE screenMakeShellDataCS(ClientSim *csPtr, BYTE *buff) {
  return shellsNetMake(&clientSimGetGameSim(csPtr)->shs, buff, 0xFF, TRUE);
}

/*********************************************************
*NAME:          screenExtractShellData
*AUTHOR:        John Morrison
*CREATION DATE:  6/3/99
*LAST MODIFIED: 18/3/99
*PURPOSE:
*  Extracts shell data from a network packet here
*
*ARGUMENTS:
*  buff   - Pointer that holds Packet data
*  datLen - Length of the packet
*********************************************************/
void screenExtractShellDataCS(ClientSim *csPtr, BYTE *buff, BYTE dataLen) {
  shellsNetExtract(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->shs, &clientSimGetGameSim(csPtr)->pb, buff, dataLen, FALSE, NULL);
}

/*********************************************************
*NAME:          screenExtractPNBData
*AUTHOR:        John Morrison
*CREATION DATE: 10/3/99
*LAST MODIFIED:  8/1/00
*PURPOSE:
*  Extracts Pills and base data from a network packet
*
*ARGUMENTS:
*  buff   - Pointer that holds Packet data
*  datLen - Length of the packet
*  isTcp  - Is this TCP data
*********************************************************/
bool screenExtractPNBData(BYTE *buff, BYTE dataLen, bool isTcp) {
  (void)buff; (void)dataLen; (void)isTcp;
  return TRUE;
}

/*********************************************************
*NAME:          screenExtractMNTData
*AUTHOR:        John Morrison
*CREATION DATE: 20/3/99
*LAST MODIFIED:  8/1/00
*PURPOSE:
*  Extracts netMNT data from a network packet
*
*ARGUMENTS:
*  buff   - Pointer that holds Packet data
*  datLen - Length of the packet
*  isTcp  - Is this TCP data
*********************************************************/
bool screenExtractMNTData(BYTE *buff, BYTE dataLen, bool isTcp) {
  (void)buff; (void)dataLen; (void)isTcp;
  return TRUE;
}


/*********************************************************
*NAME:          screenLeaveGame
*AUTHOR:        John Morrison
*CREATION DATE: 20/3/99
*LAST MODIFIED: 20/3/99
*PURPOSE:
* We want to quit a network game. Post the notification
* to all players.
*
*ARGUMENTS:
*
*********************************************************/
void screenLeaveGame(void) {

}

void screenIncomingMessageCS(ClientSim *csPtr, BYTE playerNum, char *messageStr) {
  char topLine[FILENAME_MAX];       /* The message topline */

  topLine[0] = '\0';
  playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), playerNum, topLine);

  if (clientSimIsInLobby(csPtr) && playerNum < 16) {
    clientSimAppendLobbyChat(csPtr, clientSimGetLobbySlot(csPtr, playerNum)->playerName, messageStr);
  } else {
    clientMessageAdd(clientSimGetMessages(csPtr), (messageType) (playerNum + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  }
}

/*********************************************************
*NAME:          screenTogglePlayerCheckState
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Toggles the check mark state on a player
*
*ARGUMENTS:
*
*********************************************************/
void screenTogglePlayerCheckStateCS(ClientSim *csPtr, BYTE playerNum) {
  playersToggleCheckedState(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), playerNum, FALSE);
}

/*********************************************************
*NAME:          screenCheckAllNonePlayers
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Checks/UnChecks all players.
*
*ARGUMENTS:
*  isChecked - TRUE if check all
*********************************************************/
void screenCheckAllNonePlayersCS(ClientSim *csPtr, bool isChecked) {
  playersCheckAllNone(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), isChecked, FALSE);
}

/*********************************************************
*NAME:          screenCheckAlliedPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Checks your allies
*
*ARGUMENTS:
*
*********************************************************/
void screenCheckAlliedPlayersCS(ClientSim *csPtr) {
  playersCheckAllies(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), FALSE);
}

/*********************************************************
*NAME:          screenCheckAlliedPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Checks nearby players
*
*ARGUMENTS:
*
*********************************************************/
void screenCheckNearbyPlayersCS(ClientSim *csPtr) {
  playersCheckNearbyPlayers(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), tankGetMX(&MY_TANK(csPtr)), tankGetMY(&MY_TANK(csPtr)), FALSE);
}

/*********************************************************
*NAME:          screenNumCheckedPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Returns the number of checked players
*
*ARGUMENTS:
*
*********************************************************/
int screenNumCheckedPlayersCS(ClientSim *csPtr) {
  return playersGetNumChecked(&clientSimGetGameSim(csPtr)->plyrs);
}

/*********************************************************
*NAME:          screenNumCheckedPlayers
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Returns the number of allied players
*
*ARGUMENTS:
*
*********************************************************/
int screenNumAlliesCS(ClientSim *csPtr) {
  return playersGetNumAllies(&clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
}

/*********************************************************
*NAME:          screenNumNearbyTanks
*AUTHOR:        John Morrison
*CREATION DATE: 6/4/99
*LAST MODIFIED: 6/4/99
*PURPOSE:
* Returns the number of nearby tanks
*
*ARGUMENTS:
*
*********************************************************/
int screenNumNearbyTanksCS(ClientSim *csPtr) {
  return playersNumNearbyPlayers(&clientSimGetGameSim(csPtr)->plyrs, tankGetMX(&MY_TANK(csPtr)), tankGetMY(&MY_TANK(csPtr)));
}

/*********************************************************
*NAME:          screenSendMessageAllAllies
*AUTHOR:        John Morrison
*CREATION DATE: 7/4/99
*LAST MODIFIED: 7/4/99
*PURPOSE:
* Sends a message to all allied players
*
*ARGUMENTS:
*  message - The message to send
*********************************************************/
void screenSendMessageAllAlliesCS(ClientSim *csPtr, char *messageStr) {
  char topLine[FILENAME_MAX];       /* The message topline */

  topLine[0] = '\0';
  playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), topLine);
  clientMessageAdd(clientSimGetMessages(csPtr), (messageType) (clientSimGetMyPlayerNum(csPtr) + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  playersSendMessageAllAllies(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), messageStr);
}

/*********************************************************
*NAME:          screenSendMessageAllSelected
*AUTHOR:        John Morrison
*CREATION DATE: 7/4/99
*LAST MODIFIED: 7/4/99
*PURPOSE:
* Sends a message to all selected players
*
*ARGUMENTS:
*  messageStr - The message to send
*********************************************************/
void screenSendMessageAllSelectedCS(ClientSim *csPtr, char *messageStr) {
  playersSendMessageAllSelected(csPtr, clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), messageStr);
}

/*********************************************************
*NAME:          screenSendMessageAllNearby
*AUTHOR:        John Morrison
*CREATION DATE: 7/4/99
*LAST MODIFIED: 7/4/99
*PURPOSE:
* Sends a message to all nearby players
*
*ARGUMENTS:
*  message - The message to send
*********************************************************/
void screenSendMessageAllNearbyCS(ClientSim *csPtr, char *messageStr) {
  char topLine[FILENAME_MAX];       /* The message topline */

  topLine[0] = '\0';
  playersMakeMessageName(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), clientSimGetMyPlayerNum(csPtr), topLine);
  clientMessageAdd(clientSimGetMessages(csPtr), (messageType) (clientSimGetMyPlayerNum(csPtr) + PLAYER_MESSAGE_OFFSET), topLine, messageStr);
  playersSendMessageAllNearby(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), tankGetMX(&MY_TANK(csPtr)), tankGetMY(&MY_TANK(csPtr)), messageStr);
}

/*********************************************************
*NAME:          screenRequestAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Frontend has clicked the request alliance menu item
*
*ARGUMENTS:
*
*********************************************************/
void screenRequestAllianceCS(ClientSim *csPtr) {
  playersRequestAlliance(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
}

/*********************************************************
*NAME:          screenLeaveAlliance
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
* Frontend has clicked the leave alliance menu item
*
*ARGUMENTS:
*
*********************************************************/
void screenLeaveAllianceCS(ClientSim *csPtr) {
  clientSimLeaveAlliance(csPtr, clientSimGetMyPlayerNum(csPtr));
}

/*********************************************************
*NAME:          screenChangeOwnership
*AUTHOR:        John Morrison
*CREATION DATE: 2/11/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Called because we have lost our connection to the server
* Set all of our allies stuff to be owned by us.
*
*ARGUMENTS:
*  oldOwner - The old Owner of the stuff
*********************************************************/
void screenChangeOwnershipCS(ClientSim *csPtr, BYTE oldOwner) {
  basesMigrate(clientSimGetGameSim(csPtr), oldOwner, clientSimGetMyPlayerNum(csPtr));
  pillsMigrate(clientSimGetGameSim(csPtr), oldOwner, clientSimGetMyPlayerNum(csPtr));
}

/*********************************************************
*NAME:          screenMoveViewOffsetLeft
*AUTHOR:        John Morrison
*CREATION DATE: 2/11/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Moves our view left or right depending on the argument
*
*ARGUMENTS:
*  isLeft - TRUE for left, FALSE for right
*********************************************************/
void screenMoveViewOffsetLeftCS(ClientSim *csPtr, bool isLeft) {
  if (isLeft == TRUE) {
    clientSimSetXOffset(csPtr, clientSimGetXOffset(csPtr) - 1);
  } else {
    clientSimSetXOffset(csPtr, clientSimGetXOffset(csPtr) + 1);
  }
}

/*********************************************************
*NAME:          screenMoveViewOffsetUp
*AUTHOR:        John Morrison
*CREATION DATE: 2/11/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Moves our view up or down depending on the argument
*
*ARGUMENTS:
*  isup - TRUE for up, FALSE for dpwm
*********************************************************/
void screenMoveViewOffsetUpCS(ClientSim *csPtr, bool isUp) {
  if (isUp == TRUE) {
    clientSimSetYOffset(csPtr, clientSimGetYOffset(csPtr) - 1);
  } else {
    clientSimSetYOffset(csPtr, clientSimGetYOffset(csPtr) + 1);
  }
}


/*********************************************************
*NAME:          screenTankIsDead
*AUTHOR:        John Morrison
*CREATION DATE: 2/11/99
*LAST MODIFIED: 2/11/99
*PURPOSE:
* Returns if our tank is dead or not
*
*ARGUMENTS:
*
*********************************************************/
bool screenTankIsDeadCS(ClientSim *csPtr) {
  bool returnValue;
  BYTE high, low, health, dummy;

  returnValue = FALSE;
  tankGetStats(&MY_TANK(csPtr), &high, &low, &health, &dummy);
  if (health > TANK_FULL_ARMOUR) {
    /* Tank is dead */
    returnValue = TRUE;
  }
  return returnValue;
}

/*********************************************************
*NAME:          screenGetLgmStatus
*AUTHOR:        John Morrison
*CREATION DATE: 14/11/99
*LAST MODIFIED: 14/11/99
*PURPOSE:
*  Gets the man status for drawing on the status bars
*
*ARGUMENTS:
*  isOut  - TRUE if man is out of tank
*  isDead - TRUE if man is dead
*  angle  - Angle man is travelling on
*********************************************************/
void screenGetLgmStatusCS(ClientSim *csPtr, bool *isOut, bool *isDead, TURNTYPE *angle) {
  lgmGetStatus(&MY_LGM(csPtr), &MY_TANK(csPtr), isOut, isDead, angle);
}

/*********************************************************
*NAME:          screenTankScroll
*AUTHOR:        John Morrison
*CREATION DATE: 21/11/99
*LAST MODIFIED: 10/06/01
*PURPOSE:
*  Does an scroll update check/move for the tank. Returns
*  if a move occurs
*
*ARGUMENTS:
*
*********************************************************/
bool screenTankScrollCS(ClientSim *csPtr) {
  BYTE x;  /* Tank X and Y Co-ordinated       */
  BYTE y;

  /* Don't scroll the view while in pill view — the view is locked on the pill */
  if (clientSimIsInPillView(csPtr) == TRUE) {
    return FALSE;
  }

  x = tankGetScreenMX(&MY_TANK(csPtr));
  y = tankGetScreenMY(&MY_TANK(csPtr));
/*  if (px >3 || (x - xOffset -1) == 0) {
    x++;
  }
  if (py >2) {
    y++;
  } */
  return scrollManual(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), x, y, (TURNTYPE) tankGetTravelAngel(&MY_TANK(csPtr)));
}

/*********************************************************
*NAME:          screenGetSubMapSquareOffset
*AUTHOR:        John Morrison
*CREATION DATE: 18/11/99
*LAST MODIFIED: 18/11/99
*PURPOSE:
*  Gets the map sqaure sub offsets.
*
*ARGUMENTS:
*  xPos - Pointer to hold X maps square offset
*  yPos - Pointer to hold Y maps square offset
*********************************************************/
void screenGetSubMapSquareOffset(int *xPos, int *yPos) {
  *xPos = 0; /* edgeX; */
  *yPos = 0; /* edgeY; */
}




/*********************************************************
*NAME:          screenSetAiType
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Sets the AI type for this game
*
*ARGUMENTS:
*
*********************************************************/
void screenSetAiTypeCS(ClientSim *csPtr, aiType value) {
  *clientSimGetAllowComputerTanks(csPtr) = value;
}


/*********************************************************
*NAME:          screenGetAiType
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
*  Returns the AI type for this game
*
*ARGUMENTS:
*
*********************************************************/
aiType screenGetAiTypeCS(ClientSim *csPtr) {
  return *clientSimGetAllowComputerTanks(csPtr);
}



BYTE screenGetTank256DirCS(ClientSim *csPtr) {
  return tankGet256Dir(&MY_TANK(csPtr));
}

/*********************************************************
*NAME:          screenGetTankAutoSlowdown
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Returns whether tank autoslowdown is enabled or not
*
*ARGUMENTS:
*
*********************************************************/
bool screenGetTankAutoSlowdownCS(ClientSim *csPtr) {
  return tankGetAutoSlowdown(&MY_TANK(csPtr));
}


/*********************************************************
*NAME:          screenGetTankAutoSlowdown
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Sets whether tank autoslowdown is enabled or not
*
*ARGUMENTS:
*  useSlowdown - TRUE if auto slowdown is used
*********************************************************/
void screenSetTankAutoSlowdownCS(ClientSim *csPtr, bool useSlowdown) {
  tankSetAutoSlowdown(&MY_TANK(csPtr), useSlowdown);
}


/*********************************************************
*NAME:          screenGetTankAutoHideGunsight
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Returns whether tank auto show/hide gunsight is enabled 
*  or not
*
*ARGUMENTS:
*
*********************************************************/
bool screenGetTankAutoHideGunsightCS(ClientSim *csPtr) {
  return tankGetAutoHideGunsight(&MY_TANK(csPtr));
}


/*********************************************************
*NAME:          screenSetTankAutoHideGunsight
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Sets whether tank auto show/hide gunsight is enabled 
*  or not
*
*ARGUMENTS:
*  useAutohide - TRUE if auto show/hide is used
*********************************************************/
void screenSetTankAutoHideGunsightCS(ClientSim *csPtr, bool useAutohide) {
  tankSetAutoHideGunsight(&MY_TANK(csPtr), useAutohide);
}



/*********************************************************
*NAME:          screenSetCursorPos
*AUTHOR:        John Morrison
*CREATION DATE: 27/05/00
*LAST MODIFIED: 27/05/00
*PURPOSE:
*  Sets the cursor position in steps from the top and 
*  left corner position of the active screen
*
*ARGUMENTS:
*  posX - Left position
*  posY - Top position
*********************************************************/
void screenSetCursorPosCS(ClientSim *csPtr, BYTE posX, BYTE posY) {
/*  char str[255];
  static BYTE oldCursorXPos = 255;
  static BYTE oldCursorYPos = 255;

  sprintf(str, "pos (%d, %d)", posX, posY); */
  if(posX != 0 && posY != 0) {
/*    if (posX >15  || posY > 15) {
      strcat(str, " bad");
    } */
    clientSimSetCursorPosX(csPtr, posX);
    clientSimSetCursorPosY(csPtr, posY);
/*    if (oldCursorXPos != clientSimGetCursorPosX(csPtr) || oldCursorYPos != clientSimGetCursorPosY(csPtr)) {
      oldCursorXPos = clientSimGetCursorPosX(csPtr);
      oldCursorYPos = clientSimGetCursorPosY(csPtr);
      messageAdd(globalMessage, "mouse", str);
    } */
  }
}

/*********************************************************
*NAME:          screenGetCursorPos
*AUTHOR:        John Morrison
*CREATION DATE: 27/05/00
*LAST MODIFIED: 27/05/00
*PURPOSE:
*  Gets the cursor position in steps from the top and 
*  left corner position of the active screen. Returns if
*  the cursor should be shown or not
*
*ARGUMENTS:
*  posX - Pointer to hold left position
*  posY - Pointer to hold top position
*********************************************************/
bool screenGetCursorPosCS(ClientSim *csPtr, BYTE *posX, BYTE *posY) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if (clientSimGetCursorPosX(csPtr) >= 0 && clientSimGetCursorPosX(csPtr) <= MAIN_SCREEN_SIZE_X && clientSimGetCursorPosY(csPtr) >= 0 && clientSimGetCursorPosY(csPtr) <= MAIN_SCREEN_SIZE_Y) {
    returnValue = TRUE;
    *posX = (BYTE) clientSimGetCursorPosX(csPtr);
    *posY = (BYTE) clientSimGetCursorPosY(csPtr);
  } else {
    *posX = 0;
    *posY = 0;
  }

  return returnValue;
}


/*********************************************************
*NAME:          screenNetStatusMessage
*AUTHOR:        John Morrison
*CREATION DATE: 1/6/00
*LAST MODIFIED: 1/6/00
*PURPOSE:
*  A network status message has arrived from the server
*
*ARGUMENTS:
*  messageStr - The message text
*********************************************************/
void screenNetStatusMessage(ClientSim *csPtr, char *messageStr) {
  clientMessageAdd(clientSimGetMessages(csPtr), networkStatus, (char *) "Network Status", messageStr);
}

/*********************************************************
*NAME:          screenTankStopCarryingPill
*AUTHOR:        John Morrison
*CREATION DATE: 21/6/00
*LAST MODIFIED: 21/6/00
*PURPOSE:
* Someone else has picked up a pill. We should check that
* we aren't carrying it ourselves and if so drop it (The
* server said so) because this can lead to desync problems
*
*ARGUMENTS:
*  message - The message text
*********************************************************/
void screenTankStopCarryingPillCS(ClientSim *csPtr, BYTE itemNum) {
  tankStopCarryingPill(&MY_TANK(csPtr), itemNum);
}

/*********************************************************
*NAME:          screenGenerateMapPreview
*AUTHOR:        John Morrison
*CREATION DATE: 2/7/00
*LAST MODIFIED: 2/7/00
*PURPOSE:
* Generates a map preview for the front end. A map preview
* is a 256x256 byte buff with each square equal to a map
* tile. tile numbers are identical to map square numbers
* except DEEP_SEA = 16, pills = 17, bases = 18 and starts
* = 19. Returns success
*
*ARGUMENTS:
*  fileName - Map file name to open
*  buff     - Buffer to copy into
*********************************************************/
bool screenGenerateMapPreview(char *fileName, BYTE *buff) {
  bool returnValue; /* Value to return */
  map prevmp;       /* Items in loading */
  bases prevbs;
  pillboxes prevpb;
  starts prevss;
  BYTE xValue;           /* Looping variables */
  BYTE yValue;
  BYTE *ptr;


  /* Create */
  mapCreate(&prevmp);
  startsCreate(&prevss);
  basesCreate(&prevbs);
  pillsCreate(&prevpb);

  /* Load */
  returnValue = mapRead(fileName, &prevmp, &prevpb, &prevbs, &prevss);
  if (returnValue == TRUE) {
    /* Make preview map info */
    xValue = 0;
    yValue = 0;
    ptr = buff;
    /* Set up Items */
    while (yValue < 255) {
      while (xValue < 255) {
        if ((pillsExistPos(&prevpb, xValue, yValue)) == TRUE) {
          *ptr = 17;
        } else if ((basesExistPos(&prevbs, xValue, yValue)) == TRUE) {
          *ptr = 18;
        } else if ((startsExistPos(&prevss, xValue, yValue)) == TRUE) {
          *ptr = 19;
        } else {
          *ptr = mapGetPos(&prevmp, xValue, yValue);
          if (*ptr == DEEP_SEA) {
            *ptr = 16;
          }

        }
        xValue++;
        ptr++;
      }
      xValue = 0;
      yValue++;
    }
  }

  /* Clean up */
  mapDestroy(&prevmp);
  startsDestroy(&prevss);
  basesDestroy(&prevbs);
  pillsDestroy(&prevpb);

  return returnValue;

}

/*********************************************************
*NAME:          screenNetLgmReturn
*AUTHOR:        John Morrison
*CREATION DATE: 2/12/00
*LAST MODIFIED: 2/12/00
*PURPOSE:
*  Network message. LGM Back in tank
*
*ARGUMENTS:
*  numTrees - Amount of trees lgm is carrying
*  numMines - Amount of mines the lgm is carrying
*  pillNum  - Pillbox being carries
*********************************************************/
void screenNetLgmReturnCS(ClientSim *csPtr, BYTE numTrees, BYTE numMines, BYTE pillNum) {
  lgmNetBackInTank(clientSimGetGameSim(csPtr), &MY_LGM(csPtr), &MY_TANK(csPtr), numTrees, numMines, pillNum);
}

/*********************************************************
*NAME:          lgmNetManWorking
*AUTHOR:        John Morrison
*CREATION DATE: 01/02/03
*LAST MODIFIED: 01/02/03
*PURPOSE:
*  Network message. LGM Back in tank
*
*ARGUMENTS:
*  mapX     - Bless X map position 
*  mapY     - Bless Y map position 
*  numMines - The number of mines
*  pillNum  - Pillbox being carries
*  numTrees - The number of trees
*********************************************************/
void screenNetManWorkingCS(ClientSim *csPtr, BYTE mapX, BYTE mapY, BYTE numMines, BYTE pillNum, BYTE numTrees) {
  lgmNetManWorking(clientSimGetGameSim(csPtr), &MY_LGM(csPtr), &MY_TANK(csPtr), mapX, mapY, numTrees, numMines, pillNum);
}

/*********************************************************
*NAME:          screenSetTankStartPosition
*AUTHOR:        John Morrison
*CREATION DATE: 3/10/00
*LAST MODIFIED: 2/04/01
*PURPOSE:
* Start position recieved
*
*ARGUMENTS:
*  xValue    - X Value
*  yValue    - Y Value
*  angel     - Angel the tank is facing
*  numShells - Number of shells
*  numMines  - Number of mines
*********************************************************/
void screenSetTankStartPositionCS(ClientSim *csPtr, BYTE xValue, BYTE yValue, TURNTYPE angle, BYTE numShells, BYTE numMines) {
  BYTE numTrees;
  tankSetLocationData(&MY_TANK(csPtr), (WORLD) ((xValue << TANK_SHIFT_MAPSIZE ) + MAP_SQUARE_MIDDLE), (WORLD) ((yValue << TANK_SHIFT_MAPSIZE ) + MAP_SQUARE_MIDDLE), (TURNTYPE) angle, (SPEEDTYPE) 0, (bool) TRUE);
  numTrees = 0;
  if (gameTypeGet(&clientSimGetGameSim(csPtr)->game) == gameOpen) {
    numTrees = TANK_FULL_TREES;
  }
  tankSetStats(&MY_TANK(csPtr), numShells, numMines, TANK_FULL_ARMOUR, numTrees);
  frontEndUpdateTankStatusBars(csPtr, numShells, numMines, TANK_FULL_ARMOUR, numTrees);
  screenTankViewCS(csPtr);
  clientSimGetGameSim(csPtr)->inStartFind = FALSE;
}

/*********************************************************
*NAME:          screenSetPlayersMenu
*AUTHOR:        John Morrison
*CREATION DATE: 9/02/02
*LAST MODIFIED: 9/02/02
*PURPOSE:
* Sets the player menu entries
*
*ARGUMENTS:
*
*********************************************************/
void screenSetPlayersMenuCS(ClientSim *csPtr) {
  playersSetPlayersMenu(csPtr, &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr), FALSE);
}

/*********************************************************
*NAME:          screenGetGameRunning
*AUTHOR:        John Morrison
*CREATION DATE: 20/02/03
*LAST MODIFIED: 20/02/03
*PURPOSE:
* Returns if a game is running
*
*ARGUMENTS:
*
*********************************************************/
bool screenGetGameRunningCS(ClientSim *csPtr) {
  return clientSimIsRunning(csPtr);
}


/*********************************************************
*NAME:          screenGetGameRunning
*AUTHOR:        John Morrison
*CREATION DATE: 20/02/03
*LAST MODIFIED: 20/02/03
*PURPOSE:
* Called when we have lost our connection to the server 
*
*ARGUMENTS:
*
*********************************************************/
void screenConnectionLostCS(ClientSim *csPtr) {
  lgmConnectionLost(clientSimGetGameSim(csPtr), &MY_LGM(csPtr), &MY_TANK(csPtr), &clientSimGetGameSim(csPtr)->ss);
  playersConnectionLost(csPtr, clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->plyrs, clientSimGetMyPlayerNum(csPtr));
}


//FIXME
/*********************************************************
*NAME:          serverCoreSoundDist
*AUTHOR:        John Morrison
*CREATION DATE: 19/01/99
*LAST MODIFIED: 05/05/01
*PURPOSE:
*  Calculates whether a soft sound of a loud sound should 
*  be played and passes paremeters to frontend
*
*ARGUMENTS:
*  value - Sound effect to be played
*  mx    - Map X co-ordinatate for the sound origin
*  my    - Map Y co-ordinatate for the sound origin
*********************************************************/
void serverCoreSoundDist(sndEffects value, BYTE mx, BYTE my) {
  BYTE logMessageType = 0; /* Log item type */
  
  switch (value) {
  case shootSelf:
  case shootNear:
  case shootFar:
    logMessageType = log_SoundShoot;
    break;

  case shotTreeNear:
  case shotTreeFar:
    logMessageType = log_SoundHitTree;
    break;

  case shotBuildingNear:
  case shotBuildingFar:
    logMessageType = log_SoundHitWall;
    break;

  case hitTankNear:
  case hitTankFar:
  case hitTankSelf:
    logMessageType = log_SoundHitTank;
    break;
  case bubbles:
  case tankSinkNear:
  case tankSinkFar:
    break;
  case bigExplosionNear:
    logMessageType = log_SoundBigExplosion;
    break;
  case bigExplosionFar:
    logMessageType = log_SoundExplosion;
    break;
  case farmingTreeNear:
  case farmingTreeFar:
    logMessageType = log_SoundFarm;
    break;
  case manBuildingNear:
  case manBuildingFar:
    logMessageType = log_SoundBuild;
    break;
  case manDyingNear:
  case manDyingFar:
    logMessageType = log_SoundManDie;
    break;
  case manLayingMineNear:
    logMessageType = log_SoundMineLay;
    break;
  case mineExplosionNear:
  case mineExplosionFar:
    logMessageType = log_SoundMineExplode;
    break;
  }

  if (logMessageType) {
    logAddEvent(logMessageType, mx, my, 0, 0, 0, NULL);
  }

}




/*********************************************************
*NAME:          screenSimDisplayTick
*PURPOSE:
*  Performs display-only updates after a ServerSim tick.
*  Handles scrolling, messages, status bars, pillbox view.
*********************************************************/
void screenSimDisplayTickCS(ClientSim *csPtr, bool isBrain) {
  WORLD tankX, tankY;
  BYTE armour, shellsAmount, minesAmount;
  BYTE tmx, tmy, pmx, pmy;

  if (clientSimIsRunning(csPtr) == FALSE || clientSimGetNetStatus(csPtr) == netFailed) {
    return;
  }

  if (clientSimGetGmeStartDelay(csPtr) > 0) {
    clientSimGetMessages(csPtr)->messageTime++;
    if (clientSimGetMessages(csPtr)->messageTime == MESSAGE_SCROLL_TIME) {
      messageUpdate(csPtr, clientSimGetMessages(csPtr));
      clientSimGetMessages(csPtr)->messageTime = 0;
    }
    return;
  }

  if (clientSimGetGmeLength(csPtr) == 0) {
    clientSimSetRunning(csPtr, FALSE);
    frontEndGameOver(csPtr);
    return;
  }

  if (clientSimGetNetStatus(csPtr) != netRunning) {
    clientSimGetMessages(csPtr)->messageTime++;
    if (clientSimGetMessages(csPtr)->messageTime == MESSAGE_SCROLL_TIME) {
      messageUpdate(csPtr, clientSimGetMessages(csPtr));
      clientSimGetMessages(csPtr)->messageTime = 0;
    }
    return;
  }

  /* Update scrolling based on tank position */
  if (tankGetSpeed(&MY_TANK(csPtr)) > 0) {
    tankGetGunsight(&MY_TANK(csPtr), &tmx, &tmy, &pmx, &pmy);
    if (clientSimIsInPillView(csPtr) == FALSE) {
      int oldXOffset = clientSimGetXOffset(csPtr);
      int oldYOffset = clientSimGetYOffset(csPtr);
      if (scrollUpdate(clientSimGetScroll(csPtr), clientSimGetGameSim(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), tankGetScreenMX(&MY_TANK(csPtr)), tankGetScreenMY(&MY_TANK(csPtr)), TRUE, tmx, tmy, tankGetSpeed(&MY_TANK(csPtr)), tankGetArmour(&MY_TANK(csPtr)), (TURNTYPE)(tankGetTravelAngel(&MY_TANK(csPtr))), FALSE, screenTankIsDeadCS(csPtr)) == TRUE) {
        if (oldXOffset < clientSimGetXOffset(csPtr)) { clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) - 1); moveMousePointer(right); }
        else if (oldXOffset > clientSimGetXOffset(csPtr)) { clientSimSetCursorPosX(csPtr, clientSimGetCursorPosX(csPtr) + 1); moveMousePointer(left); }
        if (oldYOffset < clientSimGetYOffset(csPtr)) { clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) - 1); moveMousePointer(down); }
        else if (oldYOffset > clientSimGetYOffset(csPtr)) { clientSimSetCursorPosY(csPtr, clientSimGetCursorPosY(csPtr) + 1); moveMousePointer(up); }
        screenReCalcCS(csPtr);
      }
    }
  }

  /* Follow the death fireball when the tank is dead */
  if (clientSimIsInPillView(csPtr) == FALSE && screenTankIsDeadCS(csPtr)) {
    BYTE expMX, expMY;
    if (tkExplosionGetOwnPosition(&clientSimGetGameSim(csPtr)->tankExplosions, clientSimGetMyPlayerNum(csPtr), &expMX, &expMY)) {
      scrollCenterObject(clientSimGetScroll(csPtr), clientSimGetXOffsetPtr(csPtr), clientSimGetYOffsetPtr(csPtr), expMX, expMY);
      screenReCalcCS(csPtr);
    }
  }

  /* Check we are still allowed to be in pillbox view */
  if (clientSimIsInPillView(csPtr) == TRUE) {
    if (pillsCheckView(clientSimGetGameSim(csPtr), &clientSimGetGameSim(csPtr)->pb, clientSimGetPillViewX(csPtr), clientSimGetPillViewY(csPtr)) == FALSE) {
      screenTankViewCS(csPtr);
    }
  }

  /* Update tank status bars — the server runs tankDeath/tankUpdate with
   * isServer=TRUE so frontEndUpdateTankStatusBars is not called from game
   * logic.  The client must push synced state to the display each tick. */
  frontEndUpdateTankStatusBars(csPtr, tankGetShells(&MY_TANK(csPtr)), tankGetMines(&MY_TANK(csPtr)),
                               tankGetArmour(&MY_TANK(csPtr)), tankGetTrees(&MY_TANK(csPtr)));

  /* Update kills/deaths display — same reason as above */
  {
    int kills, deaths;
    tankGetKillsDeaths(&MY_TANK(csPtr), &kills, &deaths);
    frontEndKillsDeaths(csPtr, kills, deaths);
  }

  /* Update LGM status indicator — the server runs lgmUpdate with the
   * server context so frontEndManStatus calls inside lgm.c may not fire
   * on the client.  Query the synced LGM state and update the display. */
  {
    bool lgmIsOut, lgmIsDead;
    TURNTYPE lgmAngle = 0;
    lgmGetStatus(&MY_LGM(csPtr), &MY_TANK(csPtr), &lgmIsOut, &lgmIsDead, &lgmAngle);
    if (!lgmIsOut) {
      frontEndManClear(csPtr);
    } else {
      frontEndManStatus(csPtr, lgmIsDead, lgmAngle);
    }
  }

  /* Update base status bars */
  tankGetWorld(&MY_TANK(csPtr), &tankX, &tankY);
  basesGetStats(&clientSimGetGameSim(csPtr)->bs, basesGetClosest(clientSimGetGameSim(csPtr), tankX, tankY), &shellsAmount, &minesAmount, &armour);
  frontEndUpdateBaseStatusBars(csPtr, shellsAmount, minesAmount, armour);

  /* Advance tank explosions locally for smooth animation/sounds between snapshots.
   * Destructive operations (pill damage, lgm death) are gated behind isServer
   * inside tkExplosionUpdate, so passing NULL/0 for lgm args is safe. */
  tkExplosionUpdate(clientSimGetGameSim(csPtr), NULL, 0, NULL, &clientSimGetGameSim(csPtr)->ss);
  explosionsUpdate(&clientSimGetGameSim(csPtr)->expl);

  /* Update network LGM frames */
  playersGameTickUpdate(&clientSimGetGameSim(csPtr)->plyrs);

  /* Messaging */
  clientSimGetMessages(csPtr)->messageTime++;
  if (clientSimGetMessages(csPtr)->messageTime == MESSAGE_SCROLL_TIME) {
    messageUpdate(csPtr, clientSimGetMessages(csPtr));
    clientSimGetMessages(csPtr)->messageTime = 0;
  }
}

/*********************************************************
*NAME:          getBuildCurrentSelect
*PURPOSE:
*  Returns the current build selection
*
*ARGUMENTS:
*
*********************************************************/
buildSelect getBuildCurrentSelectCS(ClientSim *csPtr) {
  return clientSimGetCurrentBuildSelect(csPtr);
}


/*********************************************************
*NAME:          setBuildCurrentSelect
*PURPOSE:
*  Sets the current build selection
*
*ARGUMENTS:
*  bs - The new build selection
*********************************************************/
void setBuildCurrentSelectCS(ClientSim *csPtr, buildSelect bs) {
  if ((bs != BsTrees) && (bs != BsRoad) && (bs != BsBuilding) && (bs != BsPillbox) && (bs != BsMine)) {
    return;
  }
  clientSimSetCurrentBuildSelect(csPtr, bs);
}


void screenSetLocalTransportCS(ClientSim *csPtr, bool isLocal) {
  clientSimGetGameSim(csPtr)->isLocalTransport = isLocal;
}

