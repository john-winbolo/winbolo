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
*Name:          Tank
*Filename:      tank.c
*Author:        John Morrison
*Creation Date: 23/11/98
*Last Modified: 01/02/01
*Purpose:
*  Provides operations on your tank
*********************************************************/

/* Inludes */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
/* CRC cheat detection removed — server-authoritative state makes it obsolete */
#include "screen.h"
#include "debug_file_output.h"
#include "explosions.h"
#include "frontend.h"
#include "gametype.h"
#include "global.h"
#include "lgm.h"
#include "labels.h"
#include "log.h"
#include "messages.h"
#include "mines.h"
#include "minesexp.h"
#include "players.h"
#include "tank.h"
#include "game_sim.h"

typedef struct ClientSim ClientSim;
#include "tankexp.h"
#include "tilenum.h"
#include "shells.h"
#include "sounddist.h"
#include "util.h"



bool tankShuttingDown = FALSE; // Enourmouse HACK. Please Fix Me FIXME
BYTE ct[50];

/*********************************************************
*NAME:          tankCreate
*AUTHOR:        John Morrison
*CREATION DATE: 23/11/98
*LAST MODIFIED: 15/12/99
*PURPOSE:
*  Creates a new tank and sets its armour/mines etc. level 
*  to the arguments. New tanks always start with full armour
*
*ARGUMENTS:
*  value  - Pointer to the tank structure 
*  sts    - Pointer to player starts structure
*********************************************************/
void tankCreate(GameSim *sim, tank *value) {
  starts *sts = &sim->ss;
  BYTE minesAmount; /* Stuff the new tank is to start with */
  BYTE shellsAmount;
  BYTE armourAmount;
  BYTE treesAmount;
  BYTE x;    /* Things to pass to get the player start position */
  BYTE y;
  TURNTYPE dir;

  tankShuttingDown = FALSE;

  New(*value);
  (*value)->x = 0;
  (*value)->y = 0;
  gameTypeGetItems(sim, &sim->game, &shellsAmount, &minesAmount, &armourAmount, &treesAmount);
  (*value)->armour = armourAmount;
  (*value)->shells = shellsAmount;
  (*value)->mines = minesAmount;
  (*value)->trees = treesAmount;
  (*value)->onBoat = TRUE;
  (*value)->showSight = FALSE;
  (*value)->sightLen = GUNSIGHT_MAX;
  (*value)->numKills = 0;
  (*value)->numDeaths = 0;
  (*value)->reload = 0;
  (*value)->speed = 0;
  (*value)->waterCount = 0;
  (*value)->deathWait = 0;
  (*value)->carryPills= NULL;
  (*value)->obstructed = FALSE;
  (*value)->newTank = TRUE;
  (*value)->autoSlowdown = FALSE;
  (*value)->autoHideGunsight = FALSE;
  (*value)->justFired = FALSE;
  (*value)->tankHitCount = 0;
  (*value)->tankSlideTimer = 0;
  (*value)->tankSlideAngle = 0;
  (*value)->firstLeft = 0;
  (*value)->firstRight = 0;
  (*value)->lastTankDeath = 0;

  /* Get the start position */
  sim->inStartFind = TRUE;
  startsGetStart(sim, sts, &x, &y, &dir, gameSimGetTankPlayer(sim, value));
  (*value)->x = x;
  (*value)->x <<= TANK_SHIFT_MAPSIZE;
  (*value)->x += MAP_SQUARE_MIDDLE;
  (*value)->y = y;
  (*value)->y <<= TANK_SHIFT_MAPSIZE;
  (*value)->y += MAP_SQUARE_MIDDLE;
  (*value)->angle = dir;
  (*value)->crc = 0;
  
  sim->callbacks.centerTank(sim->callbacks.ctx);
  sim->inStartFind = FALSE;
}

/*********************************************************
*NAME:          tankDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 23/11/98
*LAST MODIFIED: 20/6/00
*PURPOSE:
*  Destroys and frees the memory for the tank
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*  mp    - Pointer to the map structure
*  pb    - Pointer to the pillbox structure
*  bs    - Pointer to the bases structure
*********************************************************/
void tankDestroy(GameSim *sim, tank *value) {
  bool isServer = sim->isServer;
  tankCarryPb q;

  tankShuttingDown = TRUE;
  if ((*value) != NULL && isServer) {
    tankDropPills(sim, value);
  }
  tankShuttingDown = FALSE;
  while ((*value) != NULL && !IsEmpty((*value)->carryPills)) {
    q = (*value)->carryPills;
    (*value)->carryPills = TankPillsTail(q);
    Dispose(q);
  }
  
  if ((*value) != NULL) {
    Dispose(*value);
  }
}


/* extern bool netSendNow; */

/*********************************************************
*NAME:          tankUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 23/11/98
*LAST MODIFIED:   4/1/00
*PURPOSE:
*  The timer has passed. Update the location and reload
*  if required.
*
*ARGUMENTS:
*  value      - Pointer to the tank structure
*  mp         - Pointer to the map structure
*  bs         - Pointer to the bases structure 
*  pb         - Pointer to the pillboxes structure 
*  shs        - Pointer to the shells structure 
*  sts        - Pointer to the starts structure
*  tb         - Whether the left/right/forward etc keys 
*               is being held down
*  tankShoot  - Is the fire button down  
*  inBrain    - TRUE if a brain is running 
*               (Ignore autoslowdown)
*********************************************************/
void tankUpdate(GameSim *sim, tank *value, tankButton tb, bool tankShoot, bool inBrain) {
  map *mp = &sim->mp;
  shells *shs = &sim->shs;
  bool isServer = sim->isServer;
  WORLD conv;                   /* Used for conversions */
  BYTE bmx;                   /* Map x and y co-ords as bytes */
  BYTE bmy;



  (*value)->obstructed = FALSE;
  tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, FALSE);
  (*value)->justFired = FALSE;
  tankRegisterChangeByte(value, CRC_JUSTFIRED_OFFSET, FALSE);
  /* Extract MAP co-ords from WORLD co-ords */
  conv = (*value)->x;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmx = (BYTE) conv;
  conv = (*value)->y;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmy = (BYTE) conv;

  /* Update the reload wait if required */
  if (((*value)->reload) > 0) {
    ((*value)->reload)--;
    tankRegisterChangeByte(value, CRC_RELOAD_OFFSET, (*value)->reload);
  }

  
  /* Shoot if required */
  if (tankShoot == TRUE && (*value)->reload == 0 && (*value)->shells > 0 && (*value)->armour <= TANK_FULL_ARMOUR)  {
    TURNTYPE a;
    TURNTYPE b = 2;
    TURNTYPE c;
    a = (*value)->sightLen;
    c = a / b;
    shellsAddItem(sim, shs, (*value)->x, (*value)->y, (*value)->angle, c, gameSimGetTankPlayer(sim, value), (*value)->onBoat);
    (*value)->reload = TANK_RELOAD_TIME;
    tankRegisterChangeByte(value, CRC_RELOAD_OFFSET, TANK_RELOAD_TIME);
    (*value)->shells--;
    tankRegisterChangeByte(value, CRC_SHELLS_OFFSET, (*value)->shells);

    if (!isServer) {
      frontEndPlaySound(shootSelf);
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
    }
    (*value)->justFired = TRUE;
    tankRegisterChangeByte(value, CRC_JUSTFIRED_OFFSET, TRUE);
  }



  if ((*value)->deathWait > 0) {
	/* Tank is still waiting to respawn */
    (*value)->deathWait--;
    tankRegisterChangeByte(value, CRC_DEATHWAIT_OFFSET, (*value)->deathWait);
    if ((*value)->deathWait == 0) {
      (*value)->newTank = TRUE;
	  tankRegisterChangeByte(value, CRC_NEWTANK_OFFSET, TRUE);
      /* Server: respawn immediately so position updates in the same tick
       * deathWait hits zero — avoids a one-tick flash at the old death
       * position before the spawn location is applied. */
      if (isServer && (*value)->armour > TANK_FULL_ARMOUR
          && sim->inStartFind == FALSE) {
        tankDeath(sim, value);
      }
    }
  } else if ((*value)->armour > TANK_FULL_ARMOUR) {
	/* Tank just took enough damage to die */
    if (sim->inStartFind == FALSE) {
	  tankDeath(sim, value);
    }
  } else if ((*value)->onBoat == FALSE && (mapGetPos(mp,bmx, bmy)) == DEEP_SEA) {
      /* Death by drowning */
      tankSetLastTankDeath(value,LAST_DEATH_BY_DEEPSEA);
      sim->callbacks.soundDist(sim->callbacks.ctx, tankSinkNear, bmx, bmy);
      if (!isServer) {
        sim->callbacks.messageAdd(sim->callbacks.ctx, assistantMessage, langGetText(MESSAGE_ASSISTANT), langGetText2(MESSAGE_TANKSUNK));
      }
      tankDropPills(sim, value);
      (*value)->armour = TANK_FULL_ARMOUR+1;
      tankRegisterChangeByte(value, CRC_ARMOUR_OFFSET, (*value)->armour);
      (*value)->deathWait = TANK_DEATH_WAIT;
      tankRegisterChangeByte(value, CRC_DEATHWAIT_OFFSET, TANK_DEATH_WAIT);
  } else if ((*value)->onBoat == TRUE) {
    /* Tank Movement on Boat */
    (*value)->newTank = FALSE;
    tankRegisterChangeByte(value, CRC_NEWTANK_OFFSET, FALSE);
    tankMoveOnBoat(sim, value, bmx, bmy, tb, inBrain);
  } else {
    /* Tank Movement on Land */
    (*value)->newTank = FALSE;
    tankRegisterChangeByte(value, CRC_NEWTANK_OFFSET, FALSE);
    tankMoveOnLand(sim, value, bmx, bmy, tb, inBrain);
  }
}

/*********************************************************
*NAME:          tankisMoving
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
*  Returns whether the tank is in motion or not 
*
*ARGUMENTS:
*  value      - Pointer to the tank structure
*********************************************************/
bool tankIsMoving(tank *value) {
  bool returnValue = FALSE; /* Value to return */

  if ((*value)->speed != 0) {
    returnValue = TRUE;
  }
  return returnValue;
}

/*********************************************************
*NAME:          tankGetAngle
*AUTHOR:        John Morrison
*CREATION DATE: 30/8/98
*LAST MODIFIED: 30/8/98
*PURPOSE:
*  Returns the tanks actual angle
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
TURNTYPE tankGetAngle(tank *value) {
  return (*value)->angle;
}


/*********************************************************
*NAME:          tankGetDir
*AUTHOR:        John Morrison
*CREATION DATE: 23/11/98
*LAST MODIFIED: 23/11/98
*PURPOSE:
*  Returns the tank direction (16 frames)
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetDir(tank *value) {
  return utilGetDir((*value)->angle);
}

/*********************************************************
*NAME:          tankGet256Dir
*AUTHOR:        John Morrison
*CREATION DATE: 23/11/98
*LAST MODIFIED:  29/4/00
*PURPOSE:
*  Returns the tank direction 0-255
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGet256Dir(tank *value) {
  return (BYTE) ((*value)->angle);
}

/*********************************************************
*NAME:          tankGetTravelAngel
*AUTHOR:        John Morrison
*CREATION DATE: 31/12/98
*LAST MODIFIED: 31/12/98
*PURPOSE:
*  Returns the tank direction (16 frames)
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetTravelAngel(tank *value) {
  return utilGet16Dir((*value)->angle);
}

/*********************************************************
*NAME:          tankGetSpeed
*AUTHOR:        John Morrison
*CREATION DATE: 31/12/98
*LAST MODIFIED: 31/12/98
*PURPOSE:
*  Returns the tank speed
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetSpeed(tank *value) {
  return (BYTE) (*value)->speed;
}

/*********************************************************
*NAME:          tankGetActualSpeed
*AUTHOR:        John Morrison
*CREATION DATE: 31/8/99
*LAST MODIFIED: 31/8/99
*PURPOSE:
*  Returns the actual tank speed
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
SPEEDTYPE tankGetActualSpeed(tank *value) {
  return (*value)->speed;
}

/*********************************************************
*NAME:          tankGetArmour
*AUTHOR:        John Morrison
*CREATION DATE: 23/11/98
*LAST MODIFIED: 23/11/98
*PURPOSE:
*  Returns the tanks armour
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetArmour(tank *value) {
  return ((*value)->armour);
}


/*********************************************************
*NAME:          tankGetScreenMX
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks X Co-ord on the map
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetScreenMX(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  conv = (WORLD) (*value)->x - TANK_SUBTRACT;
  conv >>= TANK_SHIFT_MAPSIZE;
  returnValue = (BYTE) conv;
  return returnValue;
}

/*********************************************************
*NAME:          tankGetScreenPX
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks x pixel offset
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetScreenPX(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  conv = (*value)->x - TANK_SUBTRACT;
  conv <<= TANK_SHIFT_MAPSIZE;
  conv >>= TANK_SHIFT_PIXELSIZE;
  returnValue = (BYTE) conv;
  return returnValue;
}

/*********************************************************
*NAME:          tankGetScreenMY
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks y Co-ord on the map
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetScreenMY(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  conv = (WORLD) (*value)->y - TANK_SUBTRACT;
  conv >>= TANK_SHIFT_MAPSIZE;
  returnValue = (BYTE) conv;
  return returnValue;
}

/*********************************************************
*NAME:          tankGetScreenPY
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks y pixel offset
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetScreenPY(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  conv = (*value)->y - TANK_SUBTRACT;
  conv <<= TANK_SHIFT_MAPSIZE;
  conv >>= TANK_SHIFT_PIXELSIZE;
  returnValue = (BYTE) conv;
  return returnValue;
}

/*********************************************************
*NAME:          tankGetMX
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks X Co-ord on the map
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetMX(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  returnValue = 0;
  if ((*value)->armour <= TANK_FULL_ARMOUR) {
    conv = (*value)->x;
    conv >>= TANK_SHIFT_MAPSIZE;
    returnValue = (BYTE) conv;
  }
  return returnValue;
}

/*********************************************************
*NAME:          tankGetPX
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks x pixel offset
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetPX(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  conv = (*value)->x;
  conv <<= TANK_SHIFT_MAPSIZE;
  conv >>= TANK_SHIFT_PIXELSIZE;
  returnValue = (BYTE) conv;
  return returnValue;
}

/*********************************************************
*NAME:          tankGetMY
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks y Co-ord on the map
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetMY(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  returnValue = 0;
  if ((*value)->armour <= TANK_FULL_ARMOUR) {
    conv = (*value)->y;
    conv >>= TANK_SHIFT_MAPSIZE;
    returnValue = (BYTE) conv;
  }
  return returnValue;
}

/*********************************************************
*NAME:          tankGetPY
*AUTHOR:        John Morrison
*CREATION DATE: 24/11/98
*LAST MODIFIED: 24/11/98
*PURPOSE:
* Returns whether the tanks y pixel offset
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetPY(tank *value) {
  WORLD conv;       /* Useful in converting short to unsigned char */
  BYTE returnValue; /* Value to return */
  
  conv = (*value)->y;
  conv <<= TANK_SHIFT_MAPSIZE;
  conv >>= TANK_SHIFT_PIXELSIZE;
  returnValue = (BYTE) conv;
  return returnValue;
}

/*********************************************************
*NAME:          tankGetStats
*AUTHOR:        John Morrison
*CREATION DATE: 22/12/98
*LAST MODIFIED: 22/12/98
*PURPOSE:
*  Returns the tank shells, mines, armour and trees 
*
*ARGUMENTS:
*  value        - Pointer to the tank structure
*  shellsAmount - Pointer to hold number of shells
*  minesAmount  - Pointer to hold number of mines
*  armourAmount - Pointer to hold amount of armour
*  treesAmount  - Pointer to hold amount of trees
*********************************************************/
void tankGetStats(tank *value, BYTE *shellsAmount, BYTE *minesAmount, BYTE *armourAmount, BYTE *treesAmount) {
	if(*value!=NULL){
	  *shellsAmount = (*value)->shells;
	  *minesAmount = (*value)->mines;
	  *armourAmount = (*value)->armour;
	  *treesAmount = (*value)->trees;
	}
}

/*********************************************************
*NAME:          tankSetStats
*AUTHOR:        John Morrison
*CREATION DATE: 22/12/98
*LAST MODIFIED: 22/12/98
*PURPOSE:
*  Returns the tank shells, mines, armour and trees 
*
*ARGUMENTS:
*  value        - Pointer to the tank structure
*  shellsAmount - Number of shells
*  minesAmount  - Number of mines
*  armourAmount - Amount of armour
*  treesAmount  - Amount of trees
*********************************************************/
void tankSetStats(tank *value, BYTE shellsAmount, BYTE minesAmount, BYTE armourAmount, BYTE treesAmount) {
  (*value)->shells = shellsAmount;
  tankRegisterChangeByte(value, CRC_SHELLS_OFFSET, shellsAmount);
  (*value)->mines = minesAmount;
  tankRegisterChangeByte(value, CRC_MINES_OFFSET, minesAmount);
  (*value)->armour = armourAmount;
  tankRegisterChangeByte(value, CRC_ARMOUR_OFFSET, armourAmount);
  (*value)->trees = treesAmount;
  tankRegisterChangeByte(value, CRC_TREES_OFFSET, treesAmount);
}

/*********************************************************
*NAME:          tankIsGunsightShow
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 24/12/98
*PURPOSE:
*  Returns whether the gunsight is visible or not
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*********************************************************/
bool tankIsGunsightShow(tank *value) {
  return (*value)->showSight;
}

/*********************************************************
*NAME:          tankGetGunsight
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 24/12/98
*PURPOSE:
*  Returns the Map and Pixel co-ordinates for the tanks
*  gunsight.
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  xMap   - Pointer to hold Map X Co-ord
*  yMap   - Pointer to hold Map X Co-ord
*  xPixel - Pointer to hold X Pixel
*  yPixel - Pointer to hold Y Pixel
*********************************************************/
void tankGetGunsight(tank *value, BYTE *xMap, BYTE *yMap, BYTE *xPixel, BYTE *yPixel) {
  WORLD x;
  WORLD y;
  WORLD conv;
  int xAmount;
  int yAmount;
  unsigned int speed;

  if ((*value)->armour <= TANK_FULL_ARMOUR) {
    speed = (*value)->sightLen;
    speed <<= 7; /* TANK_SHIFT_MAPSIZE */
    utilCalcDistance(&xAmount, &yAmount, (*value)->angle, (int) speed);

    x = (WORLD) ((*value)->x + xAmount - TANK_SUBTRACT);
    y = (WORLD) ((*value)->y + yAmount - TANK_SUBTRACT);

    conv = x;
    conv >>= TANK_SHIFT_MAPSIZE;
    *xMap = (BYTE) conv;

    conv = y;
    conv >>= TANK_SHIFT_MAPSIZE;
    *yMap = (BYTE) conv;

    conv = x;
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;

    *xPixel = (BYTE) conv; 

    conv = y;
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;
    *yPixel = (BYTE) conv;
  } else {
    *xMap =0;
    *yMap =0;
    *xPixel =0;
    *yPixel =0;
  }
}

/*********************************************************
*NAME:          tankGunsightIncrease
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Adds a map unit on to the tank gunsight range 
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*********************************************************/
void tankGunsightIncrease(ClientSim *csParam, GameSim *sim, tank *value) {
  bool isServer = sim->isServer;
  if ((*value)->sightLen < GUNSIGHT_MAX) {
    (*value)->sightLen++;
    tankRegisterChangeByte(value, CRC_SIGHTLEN_OFFSET, (*value)->sightLen);
  } else if ((*value)->autoHideGunsight == TRUE && (*value)->showSight == TRUE) {
    (*value)->showSight = FALSE;
    tankRegisterChangeByte(value, CRC_SHOWSIGHT_OFFSET, FALSE);
    if (!isServer) {
      frontEndShowGunsight(csParam, FALSE);
    }
  }
}

/*********************************************************
*NAME:          tankGunsightDecrease
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Adds a map unit on to the tank gunsight range 
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*********************************************************/
void tankGunsightDecrease(ClientSim *csParam, GameSim *sim, tank *value) {
  bool isServer = sim->isServer;
  if ((*value)->sightLen > GUNSIGHT_MIN) {
    (*value)->sightLen--;
    tankRegisterChangeByte(value, CRC_SIGHTLEN_OFFSET, (*value)->sightLen);
  }
  if ((*value)->showSight == FALSE && (*value)->autoHideGunsight == TRUE) {
    (*value)->showSight = TRUE;
    tankRegisterChangeByte(value, CRC_SHOWSIGHT_OFFSET, TRUE);
    if (!isServer) {
      frontEndShowGunsight(csParam, TRUE);
    }
  }
}

/*********************************************************
*NAME:          tankSetGunsight
*AUTHOR:        John Morrison
*CREATION DATE: 24/12/98
*LAST MODIFIED: 24/12/98
*PURPOSE:
*  Sets the gunsight on or off
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  shown  - if TRUE then gunsight shown
*********************************************************/
void tankSetGunsight(tank *value, bool shown) {
 if ((*value) != NULL) { 
   (*value)->showSight = shown;
   tankRegisterChangeByte(value, CRC_SHOWSIGHT_OFFSET, shown);
   if (shown == FALSE) {
     (*value)->sightLen = GUNSIGHT_MAX;
     tankRegisterChangeByte(value, CRC_SIGHTLEN_OFFSET, GUNSIGHT_MAX);
   }
 }
}

/*********************************************************
*NAME:          tankGetWorld
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 26/12/98
*PURPOSE:
*  Gets the tanks world co-ordinates
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  x      - Pointer to hold X co-ord
*  y      - Pointer to hold Y co-ord
*********************************************************/
void tankGetWorld(tank *value, WORLD *x, WORLD *y) {
  *x = (*value)->x; 
  *y = (*value)->y;
}

/*********************************************************
*NAME:          tankSetWorld
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED:  8/1/00
*PURPOSE:
*  Sets the tanks world co-ordinates and angle
*
*ARGUMENTS:
*  value        - Pointer to the tank structure
*  x            - X co-ord
*  y            - Y co-ord
*  angle        - Angle to set to
*  setResources - TRUE if we should call gameTypeGetItems
*                 to fuel up the tank
*********************************************************/
void tankSetWorld(GameSim *sim, tank *value, WORLD x, WORLD y, TURNTYPE angle, bool setResources) {
  bool isServer = sim->isServer;
  BYTE armour;
  BYTE mines;
  BYTE shells;
  BYTE trees;

  (*value)->x = x;
  tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, x);
  (*value)->y = y;
  tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, y);
  (*value)->angle = angle;
  tankRegisterChangeFloat(value, CRC_ANGLE_OFFSET, angle);
  if (setResources == TRUE) {
    gameTypeGetItems(sim, &sim->game, &shells, &mines, &armour, &trees);
    (*value)->shells = shells;
    tankRegisterChangeByte(value, CRC_SHELLS_OFFSET, shells);
    (*value)->mines = mines;
    tankRegisterChangeByte(value, CRC_MINES_OFFSET, mines);
    (*value)->armour = armour;
    tankRegisterChangeByte(value, CRC_ARMOUR_OFFSET, armour);
    (*value)->trees = trees;
    tankRegisterChangeByte(value, CRC_TREES_OFFSET, trees);
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
    }
  }
}

/*********************************************************
*NAME:          tankIsTankHit
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED: 29/7/00
*PURPOSE:
*  Returns whether the tank has been hit or not, if it
*  it is killed etc. 
*  Also updates its location if hit but not dead.
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the bases structure
*  x      - X co-ord of shell
*  y      - Y co-ord of shell
*  angle  - The direction the shell came from
*  owner  - Shells owner
*********************************************************/
tankHit tankIsTankHit(GameSim *sim, tank *value, WORLD x, WORLD y, TURNTYPE angle, BYTE owner) {
	map *mp = &sim->mp;
	pillboxes *pb = &sim->pb;
	bases *bs = &sim->bs;
	bool isServer = sim->isServer;
	tankHit returnValue; /* Value to return */
	WORLD conv;          /* Used in the conversion */
	int newX;            /* Amount to add because the tank has been hit */
	int newY;
	WORLD newmx;
	WORLD newmy;
	BYTE bmx;
	BYTE bmy;
	BYTE newbmx;       /* Test locations to check for a collision */
	BYTE newbmy; 


	returnValue = TH_MISSED;

	/* If no tank was passed, it missed. */
	if (*value == NULL) {
		return TH_MISSED;
	}

	if (!isServer) {
		if (owner == gameSimGetTankPlayer(sim, value)) {
			return TH_MISSED;
		}
	}

	returnValue = TH_MISSED;

	/*
	* TODO: here is where we would call a collision-detection function.  For now, we check to see
	* if the shell is within 128 WORLD coordinates of a tank's WORLD coordinates.  Since a tank's
	* WORLD coordinates are from the center, we assume that the tank is basically a circle.
	*
	*
	*/
	if (abs((*value)->x - x) < 128 && abs((*value)->y - y) < 128  && (*value)->armour <= TANK_FULL_ARMOUR) {
		returnValue = TH_HIT;
		(*value)->armour -= DAMAGE;
		tankRegisterChangeByte(value, CRC_ARMOUR_OFFSET, (*value)->armour);
		if ((*value)->onBoat == TRUE) {
			(*value)->onBoat = FALSE;
			tankRegisterChangeByte(value, CRC_ONBOAT_OFFSET, FALSE);
			(*value)->speed = 0;
			tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, 0);
			if (!isServer) {
				screenReCalcCS((struct ClientSim *)sim);
			}
		}

		/*
		* Tank was at zero armor before it was hit with a shell.  When we decrement the armor counter, it
		* "wraps" around and becomes larger than what a tank is supposed to have.  This signals that the
		* tank should die.
		*/
		if ((*value)->armour > TANK_FULL_ARMOUR) {
			if (((*value)->shells + (*value)->mines) > TANK_BIG_EXPLOSION_THRESHOLD) {
				returnValue = TH_KILL_BIG;
			} else {
				returnValue = TH_KILL_SMALL;
			}

			tankSetLastTankDeath(value,LAST_DEATH_BY_SHELL);
			(*value)->deathWait = TANK_DEATH_WAIT;
			tankRegisterChangeByte(value, CRC_DEATHWAIT_OFFSET, TANK_DEATH_WAIT);

			/*      netSendNow = TRUE; */
			tankDropPills(sim, value);
		} else { /* if ((*value)->armour <= TANK_FULL_ARMOUR)  */
			/* Tank was hit and survived */
			(*value)->tankSlideTimer = TANK_SLIDE_TICKS;
			(*value)->tankSlideAngle = angle;

			utilCalcDistance(&newX, &newY, angle, TANK_SLIDE); //MAP_SQUARE_MIDDLE

			/* Check for Colisions */
			conv = (*value)->x;
			conv >>= TANK_SHIFT_MAPSIZE;
			bmx = (BYTE) conv;
			conv = (*value)->y;
			conv >>= TANK_SHIFT_MAPSIZE;
			bmy = (BYTE) conv;

			newmx = (WORLD) ((*value)->x + newX);
			newmy = (WORLD) ((*value)->y + newY);


			newmx >>= TANK_SHIFT_MAPSIZE;
			newmy >>= TANK_SHIFT_MAPSIZE;
			newbmx = (BYTE) newmx;
			newbmy = (BYTE) newmy;

			if ((mapGetSpeed(sim,mp,pb,bs,bmx,newbmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0) {
				(*value)->y = (WORLD) ((*value)->y + newY);
				tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);
				bmy = newbmy;
			}
			if ((mapGetSpeed(sim,mp,pb,bs,newbmx,bmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0) {
				(*value)->x = (WORLD) ((*value)->x + newX);
				tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);
				bmx = newbmx;
			}
			/* Check for scroll of screen */
			if (!isServer) {
				screenTankScrollCS((struct ClientSim *)sim);
			}
		}
		if ((*value)->armour <= TANK_FULL_ARMOUR) {
			if (!isServer) {
				frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
			}
		} else {
			if (!isServer) {
				frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, 0, (*value)->trees);
			}
		}
	} else if (abs((*value)->x - x) < 128 && abs((*value)->y - y) < 128  && (*value)->armour > TANK_FULL_ARMOUR) {
		/* Do crazy shit here */
	}
	return returnValue;
}


/*********************************************************
*NAME:          tankInWater
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/99
*LAST MODIFIED: 4/1/99
*PURPOSE:
*  The tank is in water. Reduce ammo and mines count.
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
********************************************************/
void tankInWater(GameSim *sim, tank *value) {
  bool isServer = sim->isServer;
  bool modsMade; /* Has any modifications been made */

  modsMade = FALSE;
  if ((*value)->shells > 0) {
    (*value)->shells--;
    tankRegisterChangeByte(value, CRC_SHELLS_OFFSET, (*value)->shells);
    modsMade = TRUE;
  }
  if ((*value)->mines > 0) {
    (*value)->mines--;
    tankRegisterChangeByte(value, CRC_MINES_OFFSET, (*value)->mines);
    modsMade = TRUE;
  }

  if (modsMade == TRUE) {
    /* Update view and play sound */
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
      frontEndPlaySound(bubbles);
    }
  }
}


/*********************************************************
*NAME:          tankGetFrame
*AUTHOR:        John Morrison
*CREATION DATE: 6/1/99
*LAST MODIFIED: 6/1/99
*PURPOSE:
*  Returns the tank frame to draw (16 frames)
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetFrame(tank *value) {
  BYTE returnValue; /* Value to return */
  
  if ((*value)->armour > TANK_FULL_ARMOUR) {
    returnValue = TANK_TRANSPARENT;
  } else {
    returnValue = utilGetDir((*value)->angle);
    if ((*value)->onBoat == TRUE) {
      returnValue += TANK_BOAT_ADD;
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          tankDeath
*AUTHOR:        John Morrison
*CREATION DATE: 7/1/99
*LAST MODIFIED: 9/1/00
*PURPOSE:
*  The tank has died. Reinit its location and increment
*  the death count
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*  sts   - Pointer to the starts structure
*********************************************************/
void tankDeath(GameSim *sim, tank *value) {
  starts *sts = &sim->ss;
  bool isServer = sim->isServer;
  BYTE shellAmount; /* Stuff the new tank is to start with */
  BYTE minesAmount;
  BYTE armourAmount;
  BYTE treesAmount;
  BYTE x;       /* New location of the tank */
  BYTE y;
  TURNTYPE dir;

  /* Client path (legacy single-player or networked client) */
  if (!isServer) {
    logAddEvent(log_PlayerLocation, gameSimGetTankPlayer(sim, value), 0, 0, 0, 0, NULL);
    lgmTankDied(&sim->lgmen[gameSimGetTankPlayer(sim, value)]);
	/* Playing on a client */
    if (!isServer) {
        /* Server-authoritative: server sim handles respawn for both local
         * and UDP transport.  The client just marks inStartFind so the
         * death/respawn transition is tracked; the server picks the new
         * start position and sends it in the next snapshot. */
        if (sim->inStartFind == FALSE) {
          sim->inStartFind = TRUE;
        }
      }
    (*value)->onBoat = TRUE;
    tankRegisterChangeByte(value, CRC_ONBOAT_OFFSET, TRUE);
    (*value)->numDeaths++;
    tankRegisterChangeInt(value, CRC_NUMDEATHS_OFFSET, (*value)->numDeaths);
    (*value)->reload = 0;
    tankRegisterChangeByte(value, CRC_RELOAD_OFFSET, 0);
    (*value)->speed = 0;
    tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, 0);
    (*value)->waterCount = 0;
    tankRegisterChangeByte(value, CRC_WATERCOUNT_OFFSET, 0);
    /* Get the start position */
    if (!isServer) {
      frontEndKillsDeaths((*value)->numKills, (*value)->numDeaths);
    }
  } else if (isServer) {
    /* Server-authoritative respawn: pick a new start and reset resources */
    lgmTankDied(&sim->lgmen[gameSimGetTankPlayer(sim, value)]);
    gameTypeGetItems(sim, &sim->game, &shellAmount, &minesAmount, &armourAmount, &treesAmount);
    (*value)->armour = armourAmount;
    (*value)->tankHitCount = 0;
    (*value)->shells = shellAmount;
    (*value)->mines = minesAmount;
    (*value)->trees = treesAmount;
    sim->inStartFind = TRUE;
    startsGetStart(sim, sts, &x, &y, &dir, gameSimGetTankPlayer(sim, value));
    (*value)->x = x;
    (*value)->x <<= TANK_SHIFT_MAPSIZE;
    (*value)->x += MAP_SQUARE_MIDDLE;
    (*value)->y = y;
    (*value)->y <<= TANK_SHIFT_MAPSIZE;
    (*value)->y += MAP_SQUARE_MIDDLE;
    (*value)->angle = dir;
    sim->inStartFind = FALSE;
    (*value)->onBoat = TRUE;
    (*value)->newTank = TRUE;
    (*value)->numDeaths++;
    (*value)->reload = 0;
    (*value)->speed = 0;
    (*value)->waterCount = 0;
    (*value)->tankSlideTimer = 0;
  }
  /* Stop the tank from sliding if it was sliding when it died */
  (*value)->tankSlideTimer = 0;
}



/*********************************************************
*NAME:          tankGetKillsDeaths
*AUTHOR:        John Morrison
*CREATION DATE: 8/1/99
*LAST MODIFIED: 8/1/99
*PURPOSE:
*  Returns the number of kills and deaths the tank has had
*  into the passed pointers
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  kills  - Pointer to hold the number of kills
*  deaths - Pointer to hold the number of deaths
*********************************************************/
void tankGetKillsDeaths(tank *value, int *kills, int *deaths) {
	if(*value != NULL){
		*kills = (*value)->numKills;
		*deaths = (*value)->numDeaths;
    }
    else { // don't leave them uninitialized
        *kills = 0;
        *deaths = 0;
    }
}

/*********************************************************
*NAME:          tankAddArmour
*AUTHOR:        John Morrison
*CREATION DATE: 11/1/99
*LAST MODIFIED: 11/1/99
*PURPOSE:
*  Adds the amount of armour specified to the tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - Amount to add 
*********************************************************/
void tankAddArmour(GameSim *sim, tank *value, BYTE amount) {
  bool isServer = sim->isServer;
  if ((*value)->armour + amount <= TANK_FULL_ARMOUR) {
    (*value)->armour += amount;
    tankRegisterChangeByte(value, CRC_ARMOUR_OFFSET, (*value)->armour);
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
    }
  }
}

/*********************************************************
*NAME:          tankAddShells
*AUTHOR:        John Morrison
*CREATION DATE: 11/1/99
*LAST MODIFIED: 11/1/99
*PURPOSE:
*  Adds the amount of shells specified to the tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - Amount to add 
*********************************************************/
void tankAddShells(GameSim *sim, tank *value, BYTE amount) {
  bool isServer = sim->isServer;
  if ((*value)->shells + amount <= TANK_FULL_SHELLS) {
    (*value)->shells += amount;
    tankRegisterChangeByte(value, CRC_SHELLS_OFFSET, (*value)->shells);
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
    }
  }
}

/*********************************************************
*NAME:          tankAddMines
*AUTHOR:        John Morrison
*CREATION DATE: 11/1/99
*LAST MODIFIED: 11/1/99
*PURPOSE:
*  Adds the amount of mines specified to the tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - Amount to add 
*********************************************************/
void tankAddMines(GameSim *sim, tank *value, BYTE amount) {
  bool isServer = sim->isServer;
  if ((*value)->mines + amount <= TANK_FULL_MINES) {
    (*value)->mines += amount;
    tankRegisterChangeByte(value, CRC_MINES_OFFSET, (*value)->mines);
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
    }
  }
}

/*********************************************************
*NAME:          tankMoveOnBoat
*AUTHOR:        John Morrison
*CREATION DATE: 13/1/99
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  The tank is moving on a boat
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  mp      - Pointer to the map structure
*  pb      - Pointer to the pillbox structure
*  bs      - Pointer to the bases structure
*  bmx     - X Map Position
*  bmy     - Y Map position
*  tb      - The tank buttons being pressed
*  inBrain - TRUE if a brain is running (ignore autoslow)
*********************************************************/
void tankMoveOnBoat(GameSim *sim, tank *value, BYTE bmx, BYTE bmy, tankButton tb, bool inBrain) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  WORLD newmx;   /* Move world co-ords */
  WORLD newmy;
  int xAmount;   /* The distance to add or subtract to th location */
  int yAmount;   /* depending on the speed of the tank */
  BYTE ang;
  BYTE newbmx;
  BYTE newbmy;
  BYTE boatExitSquare;

  /* Check terrain is clear */
  tankCheckGroundClear(sim, value);

  tankTurn(sim, value, bmx, bmy, tb);
  tankAccel(sim, value, bmx, bmy, tb);


  /* Update location if speed > 0 */
  if (((*value)->speed) > 0) {
    /* If we have autoslowdown turned on - SLOW DOWN! */
    if ((*value)->autoSlowdown == TRUE && inBrain == FALSE) {
      if (tb != TDECEL && tb != TLEFTDECEL && tb != TRIGHTDECEL && tb != TACCEL && tb != TLEFTACCEL && tb != TRIGHTACCEL) {
        (*value)->speed -= TANK_AUTOSLOW_SPEED;
        if ((*value)->speed < 0) {
          (*value)->speed = 0;
        }
        tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, (*value)->speed);
      }
    }

    ang = utilGet16Dir((*value)->angle);
    utilCalcDistance(&xAmount, &yAmount, (TURNTYPE) ang, (int) (*value)->speed);

    /* Tank-to-tank collision: project velocity to slide along other tanks */
    {
      int pushX, pushY;
      if (playersCalcTankCollision(sim, gameSimGetTankPlayer(sim, value), (*value)->x, (*value)->y, &xAmount, &yAmount, &pushX, &pushY)) {
        (*value)->obstructed = TRUE;
        tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
        if (pushX != 0 || pushY != 0) {
          (*value)->x = (WORLD)((int)(*value)->x + pushX);
          (*value)->y = (WORLD)((int)(*value)->y + pushY);
          tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);
          tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);
        }
      }
    }

    /* Check to make sure updating isn't going to runinto something. If not update co-ordinates */
    newmx = (WORLD) ((*value)->x + xAmount);
    newmy = (WORLD) ((*value)->y + yAmount);

    if (yAmount > 0) {
      newmy += TANK_MOVE_BOAT_SUB;
    } else {
      newmy -= TANK_MOVE_BOAT_SUB;
    }

    if (xAmount >= 0) {
      newmx += TANK_MOVE_BOAT_SUB;
    } else {
      newmx -= TANK_MOVE_BOAT_SUB;
    }

    newmx >>= TANK_SHIFT_MAPSIZE;
    newmy >>= TANK_SHIFT_MAPSIZE;
    newbmx = (BYTE) newmx;
    newbmy = (BYTE) newmy;

    if ((mapIsLand(mp, pb, bs, bmx, newbmy)) == FALSE) {
      (*value)->y = (WORLD) ((*value)->y + yAmount);
    } else if ((mapGetSpeed(sim,mp,pb,bs,bmx,newbmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0 && (*value)->speed >= BOAT_EXIT_SPEED ) {
      (*value)->y = (WORLD) ((*value)->y + yAmount);
    } else {
      newbmy = bmy;
    }
    tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);

    if ((mapIsLand(mp, pb, bs, newbmx,newbmy)) == FALSE) {
      (*value)->x = (WORLD) ((*value)->x + xAmount);
    } else if ((mapGetSpeed(sim,mp,pb,bs,newbmx,newbmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0 && (*value)->speed >= BOAT_EXIT_SPEED ) {
      (*value)->x = (WORLD) ((*value)->x + xAmount);
    } else {
      newbmx = bmx;
    }
    tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);

    /* Reget newbmx and newbmy */
    newmx = (*value)->x;
    newmy = (*value)->y;

    newmx >>= TANK_SHIFT_MAPSIZE;
    newmy >>= TANK_SHIFT_MAPSIZE;
    newbmx = (BYTE) newmx;
    newbmy = (BYTE) newmy;

    if (newbmy < bmy) {
      if (!isServer) {
        if (frontEndTutorial(newbmy) == TRUE) {
          (*value)->speed = 0;
        }
      }
    }
    if (isServer) {
      tankCheckPillCapture(sim, value);
    }
    /* Check for leaving boat */
    if ((mapIsLand(mp, pb, bs, newbmx, newbmy)) == TRUE) {
      boatExitSquare = mapGetPos(mp,newbmx,newbmy);
      if (boatExitSquare == BOAT) {
        mapSetPos(sim, mp,newbmx,newbmy,RIVER, TRUE, FALSE);
        explosionsAddItem(&sim->expl, newbmx, newbmy, 0, 0, EXPLOSION_START);

        sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, newbmx, newbmy);
        if (!isServer) { screenReCalcCS((struct ClientSim *)sim); }
      } else if (boatExitSquare != BUILDING && boatExitSquare != HALFBUILDING) {
        if (mapGetPos(mp,bmx,bmy) == RIVER) {
          mapSetPos(sim, mp,bmx,bmy,BOAT, TRUE, FALSE);
        }
        (*value)->onBoat = FALSE;
        tankRegisterChangeByte(value, CRC_ONBOAT_OFFSET, FALSE);
        if (!isServer) { screenReCalcCS((struct ClientSim *)sim); }
      }
      /* OK We have successfully left the boat */
      if ((*value)->onBoat == FALSE) {
        /* Check for Mine hit */
        if (mapIsMine(mp, newbmx, newbmy) == TRUE) {
          minesExpAddItem(&sim->minesExplosions, mp, newbmx, newbmy);
        }
      }
    }

    /* Check for hit mine on outer map edges */
    if (mapIsMine(mp, bmx, bmy) == TRUE) {
      sim->callbacks.soundDist(sim->callbacks.ctx, mineExplosionNear, bmx, bmy);
      explosionsAddItem(&sim->expl, bmx, bmy, 0, 0, EXPLOSION_START);
      (*value)->onBoat = FALSE;
      tankRegisterChangeByte(value, CRC_ONBOAT_OFFSET, FALSE);
      (*value)->speed = 0;
      tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, 0);
    }

   /* Check for pb capture */
  }

  tankNearMines(sim, bmx, bmy);

}

/*********************************************************
*NAME:          tankMoveOnLand
*AUTHOR:        John Morrison
*CREATION DATE: 13/1/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
*  The tank is moving on land
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  mp      - Pointer to the map structure
*  pb      - Pointer to the pillbox structure
*  bs      - Pointer to the bases structure
*  bmx     - X Map Position
*  bmy     - Y Map position
*  tb      - The tank buttons being pressed
*  inBrain - TRUE if a brain is running (ignore autoslow)
*********************************************************/
void tankMoveOnLand(GameSim *sim, tank *value, BYTE bmx, BYTE bmy, tankButton tb, bool inBrain) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  WORLD newmx;   /* Move world co-ords */
  WORLD newmy;
  int xAmount;   /* The distance to add or subtract to th location */
  int yAmount;   /* depending on the speed of the tank */
  int xslideAmount;
  int yslideAmount;
  WORLD conv;
  BYTE ang;
  BYTE newbmx;
  BYTE newbmy;
  BYTE baseNum;  /* The base number for a capture */
  bool slowDown; /* Only if we can't move in either direction we should slow down */

  xslideAmount = 0;
  yslideAmount = 0;

  /* Check terrain is clear */
  tankCheckGroundClear(sim, value);

  tankTurn(sim, value, bmx, bmy, tb);
  tankAccel(sim, value, bmx, bmy, tb);
  slowDown = FALSE;

  
  /* Update location if speed > 0 */
  if (((*value)->speed) > 0) {
    /* If we have autoslowdown turned on - SLOW DOWN! */
    if ((*value)->autoSlowdown == TRUE && inBrain == FALSE) {
      if (tb != TDECEL && tb != TLEFTDECEL && tb != TRIGHTDECEL && tb != TACCEL && tb != TLEFTACCEL && tb != TRIGHTACCEL) {
        (*value)->speed -= TANK_AUTOSLOW_SPEED;
        if ((*value)->speed < 0) {
          (*value)->speed = 0;
        }
      }
      tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, (*value)->speed);
    }
    ang = utilGet16Dir((*value)->angle);
    
	utilCalcDistance(&xAmount, &yAmount, (TURNTYPE) ang, (int) (*value)->speed);

    /* Tank-to-tank collision: project velocity to slide along other tanks */
    {
      int pushX, pushY;
      if (playersCalcTankCollision(sim, gameSimGetTankPlayer(sim, value), (*value)->x, (*value)->y, &xAmount, &yAmount, &pushX, &pushY)) {
        (*value)->obstructed = TRUE;
        tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
        if (pushX != 0 || pushY != 0) {
          (*value)->x = (WORLD)((int)(*value)->x + pushX);
          (*value)->y = (WORLD)((int)(*value)->y + pushY);
          tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);
          tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);
        }
      }
    }

    /* Check to make sure updating isn't going to run into something. If not update co-ordinates */
    newmx = (WORLD) ((*value)->x + xAmount + xslideAmount);
    newmy = (WORLD) ((*value)->y + yAmount + yslideAmount);

    if (yAmount > 0) {
      newmy += TANK_MOVE_LAND_SUB;
    } else if (yAmount < 0){
      newmy -= TANK_MOVE_LAND_SUB;
    }  else if (ang > BRADIANS_EAST && ang < BRADIANS_WEST && ang != BRADIANS_SOUTH) {
      newmy += TANK_MOVE_LAND_SUB;
    } else if (ang != BRADIANS_NORTH) {
      newmy -= TANK_MOVE_LAND_SUB;
    }

    if (xAmount >= 0) {
      newmx += TANK_MOVE_LAND_SUB;
    } else if (xAmount < 0) {
      newmx -= TANK_MOVE_LAND_SUB;
    } else if (ang > BRADIANS_NORTH && ang < BRADIANS_SOUTH && ang != BRADIANS_EAST) {
      newmx += TANK_MOVE_LAND_SUB;
    } else if (ang != BRADIANS_WEST) {
      newmx -= TANK_MOVE_LAND_SUB;
    } 

    newmx >>= TANK_SHIFT_MAPSIZE;
    newmy >>= TANK_SHIFT_MAPSIZE;
    newbmx = (BYTE) newmx;
    newbmy = (BYTE) newmy;

    if ((mapGetSpeed(sim,mp,pb,bs,bmx,newbmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0) {
      (*value)->y = (WORLD) ((*value)->y + yAmount);
      tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);
    } else {
      slowDown = TRUE; 
      (*value)->obstructed = TRUE;
      tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
/*
      (*value)->speed -= TANK_WALL_SLOW_DOWN;
      if ((*value)->speed < 0) {
        (*value)->speed = 0;
      }
*/
    }
    if ((mapGetSpeed(sim,mp,pb,bs,newbmx,bmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0) {
      (*value)->x = (WORLD) ((*value)->x + xAmount);
      tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);
    } else if (slowDown == TRUE) {
      (*value)->speed-= TANK_WALL_SLOW_DOWN;
      if ((*value)->speed < 0) {
        (*value)->speed = 0;
      }
      tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, (*value)->speed);
      (*value)->obstructed = TRUE;
      tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
    } else {
      (*value)->obstructed = TRUE;
      tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
    } 
    
    newmy = (*value)->y;
    newmx = (*value)->x;
    newmx >>= TANK_SHIFT_MAPSIZE;
    newmy >>= TANK_SHIFT_MAPSIZE;
    newbmx = (BYTE) newmx;
    newbmy = (BYTE) newmy;
    if (newbmy < bmy) {
      if (!isServer) {
        if (frontEndTutorial(newbmy) == TRUE) {
          (*value)->speed = 0;
        }
      }
    }


    /* Check for entering Boat */
    if ((mapGetPos(mp,newbmx, newbmy)) == BOAT) {
      mapSetPos(sim, mp,newbmx,newbmy,RIVER, TRUE, FALSE);
      (*value)->onBoat = TRUE;
      tankRegisterChangeByte(value, CRC_ONBOAT_OFFSET, TRUE);
      if (!isServer) { screenReCalcCS((struct ClientSim *)sim); }
    }

    /* Check for hit mine */
    if (newbmx != bmx || newbmy != bmy) { /* && isServer == FALSE */
      if (mapIsMine(mp, newbmx, newbmy) == TRUE) {
        minesExpAddItem(&sim->minesExplosions, mp, newbmx, newbmy);
      }
    }
  }

	/* Was the tank hit by a shell recently? */
	if ((*value)->tankSlideTimer > 0) {
		utilCalcDistance(&xslideAmount, &yslideAmount, (TURNTYPE)(*value)->tankSlideAngle, TANK_SLIDE);
		(*value)->tankSlideTimer--;

		/* Check for Colisions */
		conv = (*value)->x;  
		conv >>= TANK_SHIFT_MAPSIZE;
		bmx = (BYTE) conv;
		conv = (*value)->y;
		conv >>= TANK_SHIFT_MAPSIZE;
		bmy = (BYTE) conv;

		newmx = (WORLD) ((*value)->x + xslideAmount);
		newmy = (WORLD) ((*value)->y + yslideAmount);


		newmx >>= TANK_SHIFT_MAPSIZE;
		newmy >>= TANK_SHIFT_MAPSIZE;
		newbmx = (BYTE) newmx;
		newbmy = (BYTE) newmy;

		if ((mapGetSpeed(sim,mp,pb,bs,bmx,newbmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0) {
			(*value)->y = (WORLD) ((*value)->y + yslideAmount);
			tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);
			bmy = newbmy;
		}
		if ((mapGetSpeed(sim,mp,pb,bs,newbmx,bmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value))) > 0) {
			(*value)->x = (WORLD) ((*value)->x + xslideAmount);
			tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);
			bmx = newbmx;
		}
		/* Check for scroll of screen */
		if (!isServer) {
			screenTankScrollCS((struct ClientSim *)sim);
		}
  }


  /* Check for tank in water */
  if (((mapGetPos(mp, bmx, bmy)) == RIVER) && (*value)->speed <= MAP_SPEED_TRIVER && (*value)->onBoat == FALSE) {
    if (basesExistPos(bs, bmx, bmy) == FALSE) {
      (*value)->waterCount++;
      tankRegisterChangeByte(value, CRC_WATERCOUNT_OFFSET, (*value)->waterCount);
      if ((*value)->waterCount == TANK_WATER_TIME) {
        (*value)->waterCount = 0;
        tankRegisterChangeByte(value, CRC_WATERCOUNT_OFFSET, 0);
        tankInWater(sim, value);
      }
    }
  }
  /* Check for capture base */
  if (isServer == TRUE) {
    if (baseIsCapturable(bs, bmx, bmy) == TRUE) {
	  /* This checks to see if another player is detected in this same square, if they are, this base is not capturable. 
	     having this check prevents the game from swapping bases back and forth between players and crashing the server.
	  */
	  if(playersCheckSameSquare(&sim->plyrs, gameSimGetTankPlayer(sim, value), bmx, bmy) == FALSE){
		  if (basesAmOwner(sim, gameSimGetTankPlayer(sim, value), bmx, bmy) == FALSE) {
			basesSetOwner(sim, bmx, bmy, gameSimGetTankPlayer(sim, value), FALSE);
			baseNum = basesGetBaseNum(bs, bmx, bmy);
			if (!isServer) {
			  frontEndStatusBase(baseNum, (basesGetStatusNum(sim, baseNum)));
			}
			if (!isServer) { screenReCalcCS((struct ClientSim *)sim); }
		  }
	  }
    }
  }


  /* Check for pb capture */
  if (isServer == TRUE) {
    tankCheckPillCapture(sim, value);
  }
  /* Check for near mines */
  tankNearMines(sim, bmx, bmy);
}

/*********************************************************
*NAME:          tankTurn
*AUTHOR:        John Morrison
*CREATION DATE: 13/1/99
*LAST MODIFIED: 13/1/99
*PURPOSE:
*  Function called to update the tanks facing angle
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the bases structure
*  bmx    - X Map Position
*  bmy    - Y Map position
*  tb     - The tank buttons being pressed
*********************************************************/
void tankTurn(GameSim *sim, tank *value, BYTE bmx, BYTE bmy, tankButton tb) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  TURNTYPE turnAmount; /* Amount to turn */

  /* Left turn */
  if (tb == TLEFT || tb == TLEFTACCEL || tb == TLEFTDECEL) {
    turnAmount = mapGetTurnRate(sim,mp,pb,bs,bmx,bmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value));
    if ((*value)->firstLeft < 10) {
      (*value)->firstLeft++;
      turnAmount /=2;
    }
    (*value)->angle -= turnAmount;
    if ((*value)->angle < 0) {
      (*value)->angle = (TURNTYPE) (BRADIANS_MAX + (*value)->angle);
    }
    tankRegisterChangeFloat(value, CRC_ANGLE_OFFSET, (*value)->angle);
  } else {
    (*value)->firstLeft = 0;
  }
  /* Right Turn */
  if (tb == TRIGHT || tb == TRIGHTACCEL || tb == TRIGHTDECEL) {
    turnAmount = mapGetTurnRate(sim,mp,pb,bs,bmx,bmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value));
    if ((*value)->firstRight < 10) {
      (*value)->firstRight++;
      turnAmount /=2;
    }
    (*value)->angle += turnAmount;
    if ((*value)->angle > BRADIANS_MAX) {
      (*value)->angle = (TURNTYPE) ((*value)->angle - BRADIANS_MAX);
    }
    tankRegisterChangeFloat(value, CRC_ANGLE_OFFSET, (*value)->angle);
  } else {
    (*value)->firstRight = 0;
  }
}

/*********************************************************
*NAME:          tankAccel
*AUTHOR:        John Morrison
*CREATION DATE: 13/1/99
*LAST MODIFIED: 13/1/99
*PURPOSE:
* Apply accleration or decalleration as required
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the bases structure
*  bmx    - X Map Position
*  bmy    - Y Map position
*  tb     - The tank buttons being pressed
*********************************************************/
void tankAccel(GameSim *sim, tank *value, BYTE bmx, BYTE bmy, tankButton tb) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  BYTE displace; /* Amount to move */
  SPEEDTYPE subAmount;     /* Amount to subtract */

  displace = mapGetSpeed(sim,mp,pb,bs,bmx,bmy,(*value)->onBoat, gameSimGetTankPlayer(sim, value));
  if ((tb == TDECEL || tb == TLEFTDECEL || tb == TRIGHTDECEL) || (*value)->speed > displace)  {
    subAmount = (*value)->speed;
    if ((*value)->speed > displace) {
      subAmount = (SPEEDTYPE) ((*value)->speed - TANK_TERRAIN_DECEL_RATE);
    }
    if (tb == TDECEL || tb == TLEFTDECEL || tb == TRIGHTDECEL) {
      subAmount -= (float) TANK_SLOWKEY_RATE;
    }
    if (subAmount > (*value)->speed) {
      ((*value)->speed) = 0;      
    } else {
      (*value)->speed = subAmount;
      if ((*value)->speed < 0) {
        (*value)->speed = 0;
      }
    }
    tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, (*value)->speed);
  } else if ((*value)->speed < displace && (tb == TACCEL || tb == TLEFTACCEL || tb == TRIGHTACCEL))  {
    ((*value)->speed) += TANK_ACCELERATE_RATE;
    if ((*value)->speed > displace) {
      (*value)->speed = displace;
    }
    tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, (*value)->speed);
  }
}

bool pillsIsCapturable(pillboxes *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          tankCheckPillCapture
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 23/6/00
*PURPOSE:
* Function checks for pillbox captures. If it does capture
* one then it updates everything.
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  pb     - Pointer to the pillbox structure
*********************************************************/
void tankCheckPillCapture(GameSim *sim, tank *value) {
	pillboxes *pb = &sim->pb;
	bool isServer = sim->isServer;
	WORLD conv;     /* Used for conversion */
	BYTE bmx;       /* Current MAP x-coord of tank */
	BYTE bmy;       /* Current MAP y-coord of tank */
	BYTE pillNum;   /* The pill number */
	tankCarryPb q;  /* Temp pointer for adding PBs to tank */

	/* Tank is alive and we are either in a server context or a non-network game */
	if ((*value)->armour <= TANK_FULL_ARMOUR && (isServer)) {

		conv = (*value)->x;
		conv >>= TANK_SHIFT_MAPSIZE;
		bmx = (BYTE) conv;
		conv = (*value)->y;
		conv >>= TANK_SHIFT_MAPSIZE;
		bmy = (BYTE) conv;

		/* The tank is not at the origin and the pill is capturable */
		if (bmx != 0 && bmy != 0 && pillsIsCapturable(pb, bmx,bmy) == TRUE) {
			pillNum = pillsGetPillNum(pb, bmx, bmy, TRUE, FALSE);
			while (pillNum != PILL_NOT_FOUND) {
				pillsSetPillInTank(pb,pillNum, TRUE);
				/* We are a client.. which should only happen in a single player game */
				if (!isServer) {
					frontEndStatusPillbox(pillNum, (pillsGetAllianceNum(sim, pb, pillNum)));
				}
				New(q);
				q->pillNum = pillNum;
				q->next = (*value)->carryPills;
				(*value)->carryPills = q;
				if ((pillsGetPillOwner(pb, pillNum)) != gameSimGetTankPlayer(sim, value)) {
					pillsSetPillOwner(sim, pb, pillNum, gameSimGetTankPlayer(sim, value), FALSE);
				}
				if (pillsExistPos(pb, bmx, bmy) == TRUE) {
					pillNum = pillsGetPillNum(pb, bmx, bmy, TRUE, FALSE);
				} else {
					pillNum = PILL_NOT_FOUND;
				}
			}
			if (!sim->isServer) { screenReCalcCS((struct ClientSim *)sim); }
		}
	}
}

/*********************************************************
*NAME:          tankDropPills
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 21/6/00
*PURPOSE:
* Drops all the pillboxes at the tanks current location.
* Called when tank dies
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the bases structure
*********************************************************/
void tankDropPills(GameSim *sim, tank *value) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  WORLD conv;     /* Used for conversion */
  BYTE bmx;       /* Current Position of Tank */
  BYTE bmy;
  BYTE numPills;  /* The number of pills on the tank */
  BYTE width;     /* How wide the pills length should be */
  tankCarryPb q;  /* Temp pointer for removing pills */
  pillbox item;   /* Item to add to the pillbox */
  BYTE count;     /* Looping variable */
  BYTE pos;       /* The Map position */


  /* Get the number of pills on the tank */
  numPills = 0;
  q = (*value)->carryPills;
  while (NonEmpty(q)) {
    numPills++;
    q = TankPillsTail(q);
  }
  if (numPills > 0 && (isServer)) {
    count = 0;
    item.armour = 0;
    item.owner = gameSimGetTankPlayer(sim, value);
    item.speed = PILLBOX_ATTACK_NORMAL;
    item.reload = PILLBOX_ATTACK_NORMAL;
    item.coolDown = 0;
    item.inTank = FALSE;
    item.justSeen = FALSE;
    /* Get tank location */
    conv = (*value)->x;
    conv >>= TANK_SHIFT_MAPSIZE;
    bmx = (BYTE) conv;
    conv = (*value)->y;
    conv >>= TANK_SHIFT_MAPSIZE;
    bmy = (BYTE) conv;
    /* Get the width for depositing them */
    width = (BYTE) sqrt((double) numPills);
    bmx -= width/2;
    bmy -= width/2;

    if (bmx <= MAP_MINE_EDGE_LEFT) { /*bmx >= 0 is always true becuase of the data type.*/
      bmx = MAP_MINE_EDGE_LEFT+1;
    } else if (bmx >= MAP_MINE_EDGE_RIGHT) {
      bmx = MAP_MINE_EDGE_RIGHT - 10;
    }

    if (bmy <= MAP_MINE_EDGE_TOP) { /*bmx >= 0 is always true becuase of the data type.*/
      bmy = MAP_MINE_EDGE_TOP+1;
    } else if (bmy >= MAP_MINE_EDGE_BOTTOM) {
      bmy = MAP_MINE_EDGE_BOTTOM - 10;
    }

    while (NonEmpty((*value)->carryPills)) {
      q = (*value)->carryPills;
      if (isServer) {
        item.x = bmx;
        item.y = bmy+count;
        if (item.x > MAP_MINE_EDGE_LEFT && item.x < MAP_MINE_EDGE_RIGHT && item.y > MAP_MINE_EDGE_TOP && item.y < MAP_MINE_EDGE_BOTTOM) {
          pos = mapGetPos(mp, item.x, item.y);
          if (pillsExistPos(pb, item.x, item.y) == FALSE && basesExistPos(bs, item.x, item.y) == FALSE && pos != BUILDING && pos != HALFBUILDING && pos != BOAT) {
            if (isServer) {
              pillsSetPill(pb,&item,q->pillNum);
            }
            if (!isServer) {
              frontEndStatusPillbox(q->pillNum, (pillsGetAllianceNum(sim, pb, q->pillNum)));
            }
            (*value)->carryPills = TankPillsTail(q);
            Dispose(q);
          }
        }
        count++;
        if (count == width) {
          count = 0;
          item.y = bmy;
          bmx++;
        }
      } else {
//        (*value)->carryPills = TankPillsTail(q);
//        Dispose(q);
      }

    }
    if (!sim->isServer) { screenReCalcCS((struct ClientSim *)sim); }
  }
  return;
}

/*********************************************************
*NAME:          tankIsOnBoat
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 17/1/99
*PURPOSE:
* Returns wether the tank is on a boat or not 
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*********************************************************/
bool tankIsOnBoat(tank *value) {
  return ((*value)->onBoat);
}

/*********************************************************
*NAME:          tankGetLgmTrees
*AUTHOR:        John Morrison
*CREATION DATE: 17/01/99
*LAST MODIFIED: 01/02/03
*PURPOSE:
* Returns whether the amount of trees request is availble
* if it is then it subtracts the amount then updates the
* display
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  amount  - Amount of trees requested
*  perform - If perform is FALSE then only test if this
*            will work. Don't actually deduct
*********************************************************/
bool tankGetLgmTrees(GameSim *sim, tank *value, BYTE amount, bool perform) {
  bool isServer = sim->isServer;
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if(((*value)->trees - amount) >= 0) {
    returnValue = TRUE;
    if (perform == TRUE) {
      (*value)->trees -= amount;
      tankRegisterChangeByte(value, CRC_TREES_OFFSET, (*value)->trees);
      if ((*value)->armour <= TANK_FULL_ARMOUR) {
        if (!isServer) {
          frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
        }
      } else {
        if (!isServer) {
          frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, 0, (*value)->trees);
        }
      }
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          tankGiveTrees
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 17/1/99
*PURPOSE:
* Adds the amount given in the amount to the tanks stocks
* and updates the displat
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - Amount of trees to add
*********************************************************/
void tankGiveTrees(GameSim *sim, tank *value, BYTE amount) {
  bool isServer = sim->isServer;
  (*value)->trees += amount;
  if ((*value)->trees > TANK_FULL_TREES) {
    (*value)->trees = TANK_FULL_TREES;
  }
  tankRegisterChangeByte(value, CRC_TREES_OFFSET, (*value)->trees);
  if ((*value)->armour <= TANK_FULL_ARMOUR) {
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
    }
  } else {
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, 0, (*value)->trees);
    }
  }
}

/*********************************************************
*NAME:          tankGetLgmMines
*AUTHOR:        John Morrison
*CREATION DATE: 17/01/99
*LAST MODIFIED: 01/02/03
*PURPOSE:
* Returns whether the amount of mines request is availble
* if it is then it subtracts the amount then updates the
* display
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  amount  - Amount of mines requested
*  perform - If perform is FALSE then only test if this
*            will work. Don't actually deduct
*********************************************************/
bool tankGetLgmMines(GameSim *sim, tank *value, BYTE amount, bool perform) {
  bool isServer = sim->isServer;
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if(((*value)->mines - amount) >= 0) {
    returnValue = TRUE;
    if (perform == TRUE) {
      (*value)->mines -= amount;
      tankRegisterChangeByte(value, CRC_MINES_OFFSET, (*value)->mines);
      if ((*value)->armour <= TANK_FULL_ARMOUR) {
        if (!isServer) {
          frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
        }
      } else {
        if (!isServer) {
          frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, 0, (*value)->trees);
        }
      }
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          tankGiveMines
*AUTHOR:        John Morrison
*CREATION DATE: 17/01/99
*LAST MODIFIED: 17/01/99
*PURPOSE:
* Adds the amount given in the amount to the tanks stocks
* and updates the displat
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - Amount of mines to add
*********************************************************/
void tankGiveMines(GameSim *sim, tank *value, BYTE amount) {
  bool isServer = sim->isServer;
  (*value)->mines += amount;
  if ((*value)->mines > TANK_FULL_MINES) {
    (*value)->mines = TANK_FULL_MINES;
  }
  tankRegisterChangeByte(value, CRC_MINES_OFFSET, (*value)->mines);
  if ((*value)->armour <= TANK_FULL_ARMOUR) {
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
    }
  } else {
    if (!isServer) {
      frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, 0, (*value)->trees);
    }
  }
}

/*********************************************************
*NAME:          tankGetCarriedPill
*AUTHOR:        John Morrison
*CREATION DATE: 17/01/99
*LAST MODIFIED: 01/02/03
*PURPOSE:
* Gets the first available carried pill. If none are 
* avaiable it returns FALSE
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  pillNum - Pointer to hold the pillbox number
*  perform - If perform is FALSE then only test if this
*            will work. Don't actually deduct
*********************************************************/
bool tankGetCarriedPill(tank *value, BYTE *pillNum, bool perform) {
  bool returnValue; /* Value to return */
  tankCarryPb q;    /* temp pointer */

  returnValue = FALSE;
  if (!IsEmpty((*value)->carryPills)) {
    if (perform == TRUE) {
      q = (*value)->carryPills;
      (*value)->carryPills = TankPillsTail(q);
      *pillNum = q->pillNum;
      Dispose(q);
    }
    returnValue = TRUE;
  }
  return returnValue;
}

void tankGetCarriedPillNum(tank *value, BYTE pillNum) {
  tankCarryPb q;
  tankCarryPb prev;
  bool done;
  done = FALSE;
  prev = NULL;
  q = (*value)->carryPills;
  while (!IsEmpty(q) && done == FALSE) {
    if (q->pillNum == pillNum) {
      done = TRUE;
      if (prev == NULL) {
        (*value)->carryPills = q->next;
      } else {
        prev->next = q->next;
      }
      Dispose(q);
    } else {
      prev = q;
      q = q->next;
    }
  }
}
/*********************************************************
*NAME:          tankPutCarriedPill
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 17/1/99
*PURPOSE:
* Puts a pillbox in the tank (Comes from LGM)
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  pillNum - The pillbox number to add
*********************************************************/
void tankPutCarriedPill(tank *value, BYTE pillNum) {
  tankCarryPb q;  /* Temp pointer for adding PBs to tank */

  New(q);
  q->pillNum = pillNum;
  q->next = (*value)->carryPills;
  (*value)->carryPills = q;
}


/*********************************************************
*NAME:          tankStopCarryingPill
*AUTHOR:        John Morrison
*CREATION DATE: 21/6/00
*LAST MODIFIED: 21/6/00
*PURPOSE:
* Someone else has picked up a pill. We should check that
* we aren't carrying it ourselves and if so drop it (The
* server said so) because this can lead to desync problems
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  pillNum - Pillbox number to drop if we are holding it
*********************************************************/
void tankStopCarryingPill(tank *value, BYTE pillNum) {
	bool done;        /* Have we done looping */
	tankCarryPb q;    /* temp pointer */
	tankCarryPb prev; /* temp pointer */

	done = FALSE;
	pillNum++;
	if (*value != NULL) {
		if (!IsEmpty((*value)->carryPills)) {
			prev = NULL;
			q = (*value)->carryPills;
			while (NonEmpty(q) && done == FALSE) {
				if (q->pillNum == pillNum) {
					/* We are carrying it */
					done = TRUE;
					if (prev == NULL) {
						(*value)->carryPills = q->next;
					} else {
						prev->next = q->next;
					}
					Dispose(q);
				} else {
					prev = q;
					q = q->next;
				}
			}
		}
	}
}

/*********************************************************
*NAME:          tankLayMine
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 19/1/00
*PURPOSE:
* Tank has been requested to lay a mine
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*  mp    - Pointer to the map strructure
*  pb    - Pointer to the pillboxes structure
*  bs    - Pointer to the bases structure
*********************************************************/
void tankLayMine(GameSim *sim, tank *value) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  WORLD conv;   /* Used in the W->M conversion */
  BYTE bmx;     /* Tank X and Y Co-ordintes */
  BYTE bmy;
  BYTE terrain; /* The current terrain */

  conv = (*value)->x;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmx = (BYTE) conv;
  conv = (*value)->y;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmy = (BYTE) conv;

  if (mapIsMine(mp, bmx, bmy) == FALSE) {
    terrain = mapGetPos(mp, bmx, bmy);

    if (terrain != BUILDING && terrain != HALFBUILDING && terrain != BOAT && terrain != RIVER && terrain < MINE_START && (*value)->mines > 0 && (*value)->onBoat == FALSE && pillsExistPos(pb, bmx, bmy) == FALSE && basesExistPos(bs, bmx, bmy) == FALSE && (*value)->armour <= TANK_FULL_ARMOUR) {
      (*value)->mines--;
      tankRegisterChangeByte(value, CRC_MINES_OFFSET, (*value)->mines);
      mapSetPos(sim, mp, bmx, bmy, (BYTE) (terrain + MINE_SUBTRACT), FALSE, FALSE);
      sim->callbacks.soundDist(sim->callbacks.ctx, manLayingMineNear, bmx, bmy);
      if ((*value)->armour <= TANK_FULL_ARMOUR) {
        if (!isServer) {
          frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
        }
      } else {
        if (!isServer) {
          frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, 0, (*value)->trees);
        }
      }
      if (!isServer) { screenReCalcCS((struct ClientSim *)sim); }
    }
  }
}

/*********************************************************
*NAME:          tankMineDamage
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/99
*LAST MODIFIED: 20/6/00
*PURPOSE:
* A mine has exploded. Check to see if it has hurt the 
* tank
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*  mp    - Pointer to the map structure
*  pb    - Pointer to the pillboxes structure
*  bs    - Pointer to the bases structure
*  mx    - Map X Co-ordinate
*  my    - Map Y Co-ordinate
*********************************************************/
void tankMineDamage(GameSim *sim, tank *value, BYTE mx, BYTE my) {
  bool isServer = sim->isServer;
  WORLD mineX; /* Mine X and Y World Co-ords */
  WORLD mineY;
  WORLD diffY; /* Difference beteween tank and mine */
  WORLD diffX; 
 
  mineX =(mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
  mineY =(my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;

  if ((*value)->x > mineX) {
    diffX = (*value)->x - mineX;
  } else {
    diffX = mineX - (*value)->x;
  }

  if ((*value)->y > mineY) {
    diffY = (*value)->y - mineY;
  } else {
    diffY = mineY - (*value)->y;
  }
  
  
  if (diffX < 384 && diffY < 384 && (*value)->armour <= TANK_FULL_ARMOUR) {
    (*value)->armour -= MINE_DAMAGE;
    tankRegisterChangeByte(value, CRC_ARMOUR_OFFSET, (*value)->armour);
    if ((*value)->armour > TANK_FULL_ARMOUR) {
      if (((*value)->shells + (*value)->mines) > TANK_BIG_EXPLOSION_THRESHOLD) {
        tkExplosionAddItem(sim, (*value)->x, (*value)->y, (TURNTYPE) ((*value)->angle), (BYTE) ((*value)->speed), (BYTE) TH_KILL_BIG);
      } else {
        tkExplosionAddItem(sim, (*value)->x, (*value)->y, (TURNTYPE) ((*value)->angle), (BYTE) ((*value)->speed), (BYTE) TH_KILL_SMALL);
      }
      (*value)->deathWait = TANK_DEATH_WAIT;
      tankRegisterChangeByte(value, CRC_DEATHWAIT_OFFSET, TANK_DEATH_WAIT);
      tankDropPills(sim, value);
    }
    if ((*value)->onBoat == TRUE) {
      (*value)->onBoat = FALSE;
      tankRegisterChangeByte(value, CRC_ONBOAT_OFFSET, FALSE);
      (*value)->speed = 0;
      tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, 0);
    }
    if ((*value)->armour <= TANK_FULL_ARMOUR) {
      if (!isServer) {
        frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, (*value)->armour, (*value)->trees);
      }
    } else {
      if (!isServer) {
        frontEndUpdateTankStatusBars((*value)->shells, (*value)->mines, 0, (*value)->trees);
      }
    }

    if (!isServer) { screenReCalcCS((struct ClientSim *)sim); }
  }
}

/*********************************************************
*NAME:          tankNearMines
*AUTHOR:        John Morrison
*CREATION DATE: 29/1/99
*LAST MODIFIED: 29/1/99
*PURPOSE:
* Check to see if the tank is near any hidden mines 
* tank
*
*ARGUMENTS:
*  mp    - Pointer to the map
*  mx    - Map X Co-ordinate
*  my    - Map Y Co-ordinate
*********************************************************/
void tankNearMines(GameSim *sim, BYTE mx, BYTE my) {
  map *mp = &sim->mp;
  bool needRecalc; /* Is a screen recalc required */

  needRecalc = FALSE;
  if (mapIsMine(mp, mx, my) == TRUE) {
    if ((minesAddItem(&sim->mns, mx, my)) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, (BYTE) (mx-1), (BYTE) (my-1)) == TRUE) {
    if ((minesAddItem(&sim->mns, (BYTE) (mx-1), (BYTE) (my-1))) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, (BYTE) (mx-1), my) == TRUE) {
    if ((minesAddItem(&sim->mns, (BYTE) (mx-1), my)) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, (BYTE) (mx-1), (BYTE) (my+1)) == TRUE) {
    if ((minesAddItem(&sim->mns, (BYTE) (mx-1), (BYTE) (my+1))) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, mx, (BYTE) (my-1)) == TRUE) {
    if ((minesAddItem(&sim->mns, mx, (BYTE) (my-1))) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, mx, (BYTE) (my+1)) == TRUE) {
    if ((minesAddItem(&sim->mns, mx, (BYTE) (my+1))) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, (BYTE) (mx+1), (BYTE) (my-1)) == TRUE) {
    if ((minesAddItem(&sim->mns, (BYTE) (mx+1), (BYTE) (my-1))) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, (BYTE) (mx+1), my) == TRUE) {
    if ((minesAddItem(&sim->mns, (BYTE) (mx+1), my)) == FALSE) {
      needRecalc = TRUE;
    }
  }
  if (mapIsMine(mp, (BYTE) (mx+1), (BYTE) (my+1)) == TRUE) {
    if ((minesAddItem(&sim->mns, (BYTE) (mx+1), (BYTE) (my+1))) == FALSE) {
      needRecalc = TRUE;
    }
  }

  if (needRecalc == TRUE && !sim->isServer) {
    screenReCalcCS((struct ClientSim *)sim);
  }
}

/*********************************************************
*NAME:          tankCheckGroundClear
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/1/00
*PURPOSE:
* Checks that the ground beneath the tank is clear (ie no
* one has built under our tank) If it is not it moves the
* tank.
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*  mp    - Pointer to the map structure
*  pb    - Pointer to the pillboxes structure
*  bs    - Pointer to the bases structure
*********************************************************/
void tankCheckGroundClear(GameSim *sim, tank *value) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  WORLD conv;   /* Used for conversions */
  BYTE bmx;     /* Tank map offsets */
  BYTE bmy;
  BYTE px;      /* Tank pixel offsets */
  BYTE py;
  BYTE terrain; /* The terrain we are on */
  bool needFix; /* True if location is obstructed and we must move the tank */

  needFix = FALSE;
  conv = (*value)->x;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmx = (BYTE) conv;
  conv = (*value)->y;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmy = (BYTE) conv;

  /* Pill check */
  if ((pillsExistPos(pb, bmx, bmy)) == TRUE) {
    /* Check to make sure its not dead */
    if ((pillsDeadPos(pb, bmx, bmy)) == FALSE) {
      needFix = TRUE;
    }
  }
  
  /* Base check */
  if ((basesExistPos(bs, bmx, bmy)) == TRUE) {
    /* Check to make sure its not allied to us */
    terrain = basesGetOwnerPos(bs, bmx, bmy);
    if (playersIsAllie(&sim->plyrs, terrain, gameSimGetTankPlayer(sim, value)) == FALSE && terrain != NEUTRAL) {
      (*value)->obstructed = TRUE;
      tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
      needFix = TRUE;
    } else {
      //return;
    }
  }

  /* Terrain check */
  if (needFix == FALSE) {
    terrain = mapGetPos(mp, bmx, bmy);
    if (terrain == BUILDING || terrain == HALFBUILDING) {
      needFix = TRUE;
      (*value)->obstructed = TRUE;
      tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
    }
  }

	/* Fix if required */
	if (needFix == TRUE) {
		(*value)->obstructed = TRUE;
		tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);

		conv = (*value)->x;
		conv <<= TANK_SHIFT_MAPSIZE;
		conv >>= TANK_SHIFT_PIXELSIZE;
		px = (BYTE) conv;
		conv = (*value)->y;
		conv <<= TANK_SHIFT_MAPSIZE;
		conv >>= TANK_SHIFT_PIXELSIZE;
		py = (BYTE) conv;
		
		if (px >= MIDDLE_PIXEL) {
			(*value)->x += 1;
		} else {
			(*value)->x -= 1;
		}
		tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);
		if (py >= MIDDLE_PIXEL) {
			(*value)->y += 1;
		} else {
			(*value)->y -= 1;
		}
		tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);
		(*value)->speed--;
		
		if ((*value)->speed < 0) {
			(*value)->speed = 0;
		}
		tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, (*value)->speed);
	}
  /* Tank-to-tank collision: separation push if overlapping while stationary */
  if (needFix == FALSE) {
    int dummyVelX = 0, dummyVelY = 0, pushX, pushY;
    if (playersCalcTankCollision(sim, gameSimGetTankPlayer(sim, value), (*value)->x, (*value)->y, &dummyVelX, &dummyVelY, &pushX, &pushY)) {
      if (pushX != 0 || pushY != 0) {
        (*value)->x = (WORLD)((int)(*value)->x + pushX);
        (*value)->y = (WORLD)((int)(*value)->y + pushY);
        tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, (*value)->x);
        tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, (*value)->y);
        (*value)->obstructed = TRUE;
        tankRegisterChangeByte(value, CRC_OBSTRUCTED_OFFSET, TRUE);
      }
    }
  }

}

/*********************************************************
*NAME:          tankAddKill
*AUTHOR:        John Morrison
*CREATION DATE: 20/3/99
*LAST MODIFIED: 20/3/99
*PURPOSE:
* We just killed a player. Add it here and update the 
* frontend repectively.
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
void tankAddKill(GameSim *sim, tank *value) {
  (*value)->numKills++;
  tankRegisterChangeInt(value, CRC_NUMKILLS_OFFSET, (*value)->numKills);
}

void tankAddDeath(GameSim *sim, tank *value) {
  (*value)->numDeaths++;
  tankRegisterChangeInt(value, CRC_NUMDEATHS_OFFSET, (*value)->numDeaths);
}


/*********************************************************
*NAME:          tankSetLocationData
*AUTHOR:        John Morrison
*CREATION DATE: 20/3/99
*LAST MODIFIED: 20/3/99
*PURPOSE:
* We just killed a player. Add it here and update the 
* frontend repectively.
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  wx     - World X co-ord
*  wy     - World Y co-ord
*  tt     - Angle
*  speed  - Speed of tank
*  onBoat - are we on a boat
*********************************************************/
void tankSetLocationData(tank *value, WORLD wx, WORLD wy, TURNTYPE tt, SPEEDTYPE speed, bool onBoat) {
  (*value)->x = wx;
  tankRegisterChangeWorld(value, CRC_WORLDX_OFFSET, wx);
  (*value)->y = wy;
  tankRegisterChangeWorld(value, CRC_WORLDY_OFFSET, wy);
  (*value)->angle = tt;
  tankRegisterChangeFloat(value, CRC_ANGLE_OFFSET, tt);
  (*value)->speed = speed;
  tankRegisterChangeFloat(value, CRC_SPEED_OFFSET, speed);
  (*value)->onBoat = onBoat;
  tankRegisterChangeByte(value, CRC_ONBOAT_OFFSET, onBoat);
}


/*********************************************************
*NAME:          tankGetNumCarriedPills
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 17/1/99
*PURPOSE:
* Returns the number of carried pills
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*********************************************************/
BYTE tankGetNumCarriedPills(tank *value) {
  BYTE returnValue; /* Value to return */
  tankCarryPb q;    /* temp pointer */

  q = (*value)->carryPills;
  returnValue =0;
  while (NonEmpty(q)) {
    returnValue++;
    q = TankPillsTail(q);
  }
 
  return returnValue;
}

/*********************************************************
*NAME:          tankGetGunsightLength
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 25/11/99
*PURPOSE:
* Returns the gunsight length
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*********************************************************/
BYTE tankGetGunsightLength(tank *value) {
  return (*value)->sightLen;
}

void tankSetGunsightLength(tank *value, BYTE len) {
  if ((*value) != NULL) {
    (*value)->sightLen = len;
  }
}

/*********************************************************
*NAME:          tankGetGunsightLength
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 25/11/99
*PURPOSE:
* Returns the tank reloading time (0 for ready to shoot)
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*********************************************************/
BYTE tankGetReloadTime(tank *value) {
  return (*value)->reload;
}

/*********************************************************
*NAME:          tankSetReload
*PURPOSE:
* Sets the tank reloading time
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*  reload  - The reload value to set
*********************************************************/
void tankSetReload(tank *value, BYTE reload) {
  (*value)->reload = reload;
}

/*********************************************************
*NAME:          tankIsObstructed
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 25/11/99
*PURPOSE:
* Returns whether the tank is obstructed or not
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*********************************************************/
bool tankIsObstructed(tank *value) {
  return (*value)->obstructed;
}

/*********************************************************
*NAME:          tankIsNewTank
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 26/11/99
*PURPOSE:
* Returns if this is a new tank or not (ie just died)
*
*ARGUMENTS:
*  value   - Pointer to the tank structure
*********************************************************/
bool tankIsNewTank(tank *value) {
  return (*value)->newTank;
}

/*********************************************************
*NAME:          tankGetAutoSlowdown
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/0
*PURPOSE:
*  Returns whether tank autoslowdown is enabled or not
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
bool tankGetAutoSlowdown(tank *value) {
  return (*value)->autoSlowdown;
}

/*********************************************************
*NAME:          tankSetAutoSlowdown
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Sets whether tank autoslowdown is enabled or not
*
*ARGUMENTS:
*  value       - Pointer to the tank structure
*  useSlowdown - TRUE if auto slowdown is used
*********************************************************/
void tankSetAutoSlowdown(tank *value, bool useSlowdown) {
  if ((*value) != NULL) {
    (*value)->autoSlowdown = useSlowdown;
    tankRegisterChangeByte(value, CRC_AUTOSLOWDOWN_OFFSET, useSlowdown);
  }
}

/*********************************************************
*NAME:          tankGetAutoHideGunsight
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Returns whether tank auto show/hide gunsight is enabled 
*  or not
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
bool tankGetAutoHideGunsight(tank *value) {
  return (*value)->autoHideGunsight;
}

/*********************************************************
*NAME:          tankSetAutoHideGunsight
*AUTHOR:        John Morrison
*CREATION DATE: 4/1/00
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Sets whether tank auto show/hide gunsight is enabled 
*  or not
*
*ARGUMENTS:
*  value       - Pointer to the tank structure
*  useAutohide - TRUE if auto slowdown is used
*********************************************************/
void tankSetAutoHideGunsight(tank *value, bool useAutohide) {
  if ((*value) != NULL) {
    (*value)->autoHideGunsight = useAutohide;
    tankRegisterChangeByte(value, CRC_AUTOHIDE_OFFSET, useAutohide);
  }
}

/*********************************************************
*NAME:          tankJustFired
*AUTHOR:        John Morrison
*CREATION DATE: 24/5/00
*LAST MODIFIED: 24/5/00
*PURPOSE:
*  Returns if the tank just fired or not
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
bool tankJustFired(tank *value) {
  return (*value)->justFired;
}

/*********************************************************
*NAME:          tankGetShells
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Returns the number of shells in tank
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetShells(tank *value) {
  return (*value)->shells;
}

/*********************************************************
*NAME:          tankGetMines
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Returns the number of mines in tank
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetMines(tank *value) {
  return (*value)->mines;
}

/*********************************************************
*NAME:          tankGetTrees
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Returns the number of trees in tank
*
*ARGUMENTS:
*  value - Pointer to the tank structure
*********************************************************/
BYTE tankGetTrees(tank *value) {
  return (*value)->trees;
}

/*********************************************************
*NAME:          tankSetShells
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Sets the number of shells in tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - The amount to set to
*********************************************************/
void tankSetShells(tank *value, BYTE amount) {
  if (amount <= TANK_FULL_SHELLS) {
    (*value)->shells = amount;
  } else {
    (*value)->shells = TANK_FULL_SHELLS;
  }
  tankRegisterChangeByte(value, CRC_SHELLS_OFFSET, (*value)->shells);
}

/*********************************************************
*NAME:          tankSetArmour
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Sets the amount of armour in tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - The amount to set to
*********************************************************/
void tankSetArmour(tank *value, BYTE amount) {
  (*value)->armour = amount;
  tankRegisterChangeByte(value, CRC_ARMOUR_OFFSET, amount);
}

/*********************************************************
*NAME:          tankSetMines
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Sets the number of mines in tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - The amount to set to
*********************************************************/
void tankSetMines(tank *value, BYTE amount) {
  if (amount <= TANK_FULL_MINES) {
    (*value)->mines = amount;
  } else {
    (*value)->mines = TANK_FULL_MINES;
  }
  tankRegisterChangeByte(value, CRC_MINES_OFFSET, (*value)->mines);
}

/*********************************************************
*NAME:          tankSetTrees
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Sets the number of trees in tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  amount - The amount to set to
*********************************************************/
void tankSetTrees(tank *value, BYTE amount) {
  if (amount <= TANK_FULL_TREES) {
    (*value)->trees = amount;
  } else {
    (*value)->trees = TANK_FULL_TREES;
  }
  tankRegisterChangeByte(value, CRC_TREES_OFFSET, (*value)->trees);
}


void tankPutPill(GameSim *sim, tank *value, BYTE pillNum) {
  pillboxes *pb = &sim->pb;
  bool isServer = sim->isServer;
  tankCarryPb q;  /* Temp pointer for adding PBs to tank */

  if (!isServer) {
    frontEndStatusPillbox(pillNum, (pillsGetAllianceNum(sim, pb, pillNum)));
  }
  New(q);
  q->pillNum = pillNum;
  q->next = (*value)->carryPills;
  (*value)->carryPills = q;
}

void tankResetHitCount(tank *value) {
  (*value)->tankHitCount = 0;
}

void tankAddHit(tank *value, int amount) {
  /* Cheat detection obsolete — server-authoritative state handles this */
  (void)value;
  (void)amount;
}

int tankCalcCRCSetup(tank *value) {
  /* CRC cheat detection removed — server-authoritative state makes it obsolete */
  (void)value;
  return 0;
}

void tankRegisterChangeFloat(tank *value, int offset, float newValue) {
  /* CRC cheat detection removed — server-authoritative state makes it obsolete */
  (void)value; (void)offset; (void)newValue;
}

void tankRegisterChangeWorld(tank *value, int offset, WORLD newValue) {
  /* CRC cheat detection removed — server-authoritative state makes it obsolete */
  (void)value; (void)offset; (void)newValue;
}

void tankRegisterChangeInt(tank *value, int offset, int newValue) {
  /* CRC cheat detection removed — server-authoritative state makes it obsolete */
  (void)value; (void)offset; (void)newValue;
}

void tankRegisterChangeByte(tank *value, int offset, BYTE newValue) {
  /* CRC cheat detection removed — server-authoritative state makes it obsolete */
  (void)value; (void)offset; (void)newValue;
}


/*********************************************************
*NAME:          tankSetOnBoat
*AUTHOR:        John Morrison
*CREATION DATE: 21/8/00
*LAST MODIFIED: 21/8/00
*PURPOSE:
*  Sets whether the tank is on a boat or not
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  onBoat - On Boat value to set
*********************************************************/
void tankSetOnBoat(tank *value, bool onBoat) {
  (*value)->onBoat = onBoat;
}

void tankSetSpeed(tank *value, SPEEDTYPE speed) {
  (*value)->speed = speed;
}

BYTE tankGetFirstLeft(tank *value) {
  return (*value)->firstLeft;
}

BYTE tankGetFirstRight(tank *value) {
  return (*value)->firstRight;
}

void tankSetFirstLeft(tank *value, BYTE val) {
  (*value)->firstLeft = val;
}

void tankSetFirstRight(tank *value, BYTE val) {
  (*value)->firstRight = val;
}

/*********************************************************
*NAME:          tankSetLastTankDeath
*AUTHOR:        Chris Lesnieski
*CREATION DATE: 04/2/09
*LAST MODIFIED: 04/2/09
*PURPOSE:
*  Sets the previous death type of the tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*  deathType - deathType value to set
*********************************************************/

void tankSetLastTankDeath(tank *value, int deathType) {
  if ((*value) != NULL) {
    (*value)->lastTankDeath = deathType;
  }
}

/*********************************************************
*NAME:          tankGetLastTankDeath
*AUTHOR:        Chris Lesnieski
*CREATION DATE: 04/2/09
*LAST MODIFIED: 04/2/09
*PURPOSE:
*  Gets the previous death type of the tank
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*********************************************************/
int tankGetLastTankDeath(tank *value) {
	return (*value)->lastTankDeath;
}


/*********************************************************
*NAME:          tankGetDeathWait
*AUTHOR:        Chris Lesnieski
*CREATION DATE: 04/2/09
*LAST MODIFIED: 04/2/09
*PURPOSE:
*  Gets the previous number of ticks left until respawn
*
*ARGUMENTS:
*  value  - Pointer to the tank structure
*********************************************************/
int tankGetDeathWait(tank *value) {
	return (*value)->deathWait;
}

void tankSetDeathWait(tank *value, int wait) {
	(*value)->deathWait = (BYTE)wait;
}

tank tankDeepCopy(tank src) {
    tank dst;
    tankCarryPb srcPill, dstPill, prev;

    if (src == NULL) {
        return NULL;
    }

    New(dst);
    *dst = *src;

    /* Deep copy the carried pills linked list */
    dst->carryPills = NULL;
    prev = NULL;
    srcPill = src->carryPills;
    while (srcPill != NULL) {
        New(dstPill);
        dstPill->pillNum = srcPill->pillNum;
        dstPill->next = NULL;
        if (prev == NULL) {
            dst->carryPills = dstPill;
        } else {
            prev->next = dstPill;
        }
        prev = dstPill;
        srcPill = srcPill->next;
    }

    /* vectorBody fields are unused (always NULL) — no copy needed */

    return dst;
}

void tankSnapToServer(tank dst, tank src) {
    tankCarryPb srcPill, dstPill, prev, q;
    bool savedAutoSlowdown;
    bool savedAutoHideGunsight;
    bool savedShowSight;

    if (dst == NULL || src == NULL) {
        return;
    }

    /* Save client-only settings (UI preferences, not game state) */
    savedAutoSlowdown = dst->autoSlowdown;
    savedAutoHideGunsight = dst->autoHideGunsight;
    savedShowSight = dst->showSight;

    /* Free dst's carried pills list */
    while (NonEmpty(dst->carryPills)) {
        q = dst->carryPills;
        dst->carryPills = TankPillsTail(q);
        Dispose(q);
    }

    /* Copy all fields from server */
    *dst = *src;

    /* Deep copy carried pills so we have our own list */
    dst->carryPills = NULL;
    prev = NULL;
    srcPill = src->carryPills;
    while (srcPill != NULL) {
        New(dstPill);
        dstPill->pillNum = srcPill->pillNum;
        dstPill->next = NULL;
        if (prev == NULL) {
            dst->carryPills = dstPill;
        } else {
            prev->next = dstPill;
        }
        prev = dstPill;
        srcPill = srcPill->next;
    }

    /* Restore client-only settings (UI preferences, not game state) */
    dst->autoSlowdown = savedAutoSlowdown;
    dst->autoHideGunsight = savedAutoHideGunsight;
    dst->showSight = savedShowSight;
}

void tankSyncResources(tank dst, tank src) {
    tankCarryPb srcPill, dstPill, prev, q;

    if (dst == NULL || src == NULL) {
        return;
    }

    dst->armour = src->armour;
    dst->shells = src->shells;
    dst->mines = src->mines;
    dst->trees = src->trees;
    dst->reload = src->reload;
    dst->numKills = src->numKills;
    dst->numDeaths = src->numDeaths;
    dst->deathWait = src->deathWait;
    dst->newTank = src->newTank;
    dst->waterCount = src->waterCount;
    dst->tankHitCount = src->tankHitCount;
    dst->onBoat = src->onBoat;
    dst->sightLen = src->sightLen;

    /* Deep-copy carried pills list from server to client */
    while (NonEmpty(dst->carryPills)) {
        q = dst->carryPills;
        dst->carryPills = TankPillsTail(q);
        Dispose(q);
    }
    dst->carryPills = NULL;
    prev = NULL;
    srcPill = src->carryPills;
    while (srcPill != NULL) {
        New(dstPill);
        dstPill->pillNum = srcPill->pillNum;
        dstPill->next = NULL;
        if (prev == NULL) {
            dst->carryPills = dstPill;
        } else {
            prev->next = dstPill;
        }
        prev = dstPill;
        srcPill = srcPill->next;
    }
}
