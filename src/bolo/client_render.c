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
*Name:          ClientRender
*Filename:      client_render.c
*Author:        John Morrison
*Purpose:
*  Per-frame render entry point and small screen/mine-view
*  buffer accessors. Extracted from screen.c so the render
*  pipeline lives in its own translation unit.
*********************************************************/

/* Includes */
#include "global.h"
#include "client_render.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "tank.h"
#include "shells.h"
#include "explosions.h"
#include "tankexp.h"
#include "scroll.h"
#include "screenbullet.h"
#include "screentank.h"
#include "screenlgm.h"
#include "frontend.h"
#include "interpolation.h"
#include "util.h"

/* Forward declaration — implemented in gui/sdl3/cursor.c */
extern void moveMousePointer(updateType value);

/*********************************************************
*NAME:          clientRenderFrame
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 21/01/01
*PURPOSE:
*  Updates the screen. Takes numerous directions
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void clientRenderFrame(ClientSim *csPtr, updateType value) {
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
    clientSimUpdateView(csPtr, (updateType) 0);
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
      clientSimPillView(csPtr, -1, 0);
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
      clientSimPillView(csPtr, 1, 0);
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
      clientSimPillView(csPtr, 0, -1);
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
      clientSimPillView(csPtr, 0, 1);
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
     * during snapshot sync in clientApplySnapshot) */
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
