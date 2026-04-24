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
*Name:          TankExplosions
*Filename:      tankexp.c
*Author:        John Morrison
*Creation Date: 15/1/99
*Last Modified: 04/04/02
*Purpose:
*  Responsable for moving dead tank explosions
*********************************************************/


#include "global.h"
#include "tank.h"
#include "screen.h"
#include "explosions.h"
#include "util.h"
#include "messages.h"
#include "frontend.h"
#include "building.h"
#include "grass.h"
#include "rubble.h"
#include "swamp.h"
#include "lgm.h"
#include "floodfill.h"
#include "sounddist.h"
#include "players.h"
#include "tankexp.h"
#include "game_sim.h"

/*********************************************************
*NAME:          tkExplosionCreate
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Sets up the tkExplosion data structure
*
*ARGUMENTS:
*  tke - Pointer to the tank explosions object
*********************************************************/
void tkExplosionCreate(tkExplosion *tke) {
  *tke = NULL;
}

/*********************************************************
*NAME:          tkExplosionDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Destroys and frees memory for the tkExplosion data 
*  structure
*
*ARGUMENTS:
*  tke - Pointer to the tank explosions object
*********************************************************/
void tkExplosionDestroy(tkExplosion *tke) {
  tkExplosion q;

  while (!IsEmpty(*tke)) {
    q = *tke;
    *tke = TkExplosionTail(q);
    Dispose(q);
  }
}

/*********************************************************
*NAME:          tkExplosionAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
*  Adds an item to the tkExplosion data structure. 
*
*ARGUMENTS:
*  tke         - Pointer to the tank explosions object
*  x           - World X Co-orindate
*  y           - World X Co-orindate
*  angle       - Angle of travel
*  length      - Length of travel
*  explodeType - Type of explosions (big - small)
*  creator     - Player number whose tank is exploding
*********************************************************/
void tkExplosionAddItem(GameSim *sim, WORLD x, WORLD y, TURNTYPE angle, BYTE length, BYTE explodeType, BYTE creator) {
  tkExplosion *tke;
  tkExplosion q;

  /* Server-authoritative: clients receive fireballs via EVENT_TK_EXPLOSION
   * (handled in screen.c) which calls tkExplosionAddItemFromSnapshot. */
  if (!sim->isServer) {
    return;
  }

  tke = &sim->tankExplosions;
  New (q);
  q->x = x;
  q->y = y;
  q->angle = angle;
  q->length = length;
  q->next = *tke;
  q->explodeType = explodeType;
  q->creator = creator;
  q->prev = NULL;
  if (NonEmpty(*tke)) {
    (*tke)->prev = q;
  }

  *tke = q;

  if (sim->callbacks.tkExplosion) {
    sim->callbacks.tkExplosion(sim->callbacks.ctx, x, y, angle, length,
                               explodeType, creator);
  }
}

void tkExplosionAddItemFromSnapshot(GameSim *sim, WORLD x, WORLD y,
                                    TURNTYPE angle, BYTE length,
                                    BYTE explodeType, BYTE creator) {
  tkExplosion *tke = &sim->tankExplosions;
  tkExplosion q;

  New (q);
  q->x = x;
  q->y = y;
  q->angle = angle;
  q->length = length;
  q->next = *tke;
  q->explodeType = explodeType;
  q->creator = creator;
  q->prev = NULL;
  if (NonEmpty(*tke)) {
    (*tke)->prev = q;
  }

  *tke = q;
}

/*********************************************************
*NAME:          tkExplosionUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 15/01/99
*LAST MODIFIED: 04/04/02
*PURPOSE:
*  Updates each tkExplosion position.
*
*ARGUMENTS:
*  tke    - Pointer to the tank explosions object
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the bases structure
*  lgms   - Array of lgms
*  numLgm - Number of lgms in the array
*  tank   - Pointer to the tank object
*********************************************************/
void tkExplosionUpdate(GameSim *sim, lgm **lgms, BYTE numLgm, tank *tank, starts *sts) {
  tkExplosion *tke = &sim->tankExplosions;
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  BYTE *updateTime = &sim->tkExpUpdateTime; /* Per-sim throttle */
  tkExplosion position;     /* Position throught the items */
  bool needUpdate;          /* Whether an update is needed or not */
  int moveX;                /* Amount to move */
  int moveY;
  WORLD conv;               /* Used in world-> map conversions */
  WORLD newX;               /* New positions */
  WORLD newY;
  BYTE mx;                  /* When a item is destoyed this is its map squares */
  BYTE my;
  BYTE px;                  /* Pixel x & y co-ordinates */
  BYTE py;
  BYTE newmx;
  BYTE newmy;
  BYTE currentPos;          /* Current map square terrain */
  BYTE playerNum;           /* Our player number */
  BYTE testX;               /* Screen Move Position Checks */
  BYTE testY;
  BYTE count;               /* Looping variable */



  /* Update only so often - Not every game tick */
  (*updateTime)++;
  if (*updateTime < TK_UPDATE_TIME) {
    return;
  }

  testX= 0;
  testY = 0;
  *updateTime = 0;
  playerNum = sim->viewPlayer;
  position = *tke;

  while (NonEmpty(position)) {
    needUpdate = TRUE;
    utilCalcDistance(&moveX, &moveY, position->angle, TK_MOVE_AMOUNT);
    if (position->length > TK_EXPLODE_DEATH) {
      /* Add the "flame trail" */
      conv = position->x;
      conv >>= TANK_SHIFT_MAPSIZE;
      mx = (BYTE) conv;
      conv = position->y;
      conv >>= TANK_SHIFT_MAPSIZE;
      my = (BYTE) conv;
      conv = position->x;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;
      px = (BYTE) conv;
      conv = position->y;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;
      py = (BYTE) conv;
      explosionsAddItem(&sim->expl, mx, my, px, py,EXPLOSION_START);
      /* Check for colisions then update position */
      newX = (WORLD) (position->x + moveX);
      newY = (WORLD) (position->y + moveY);

      /*  Moving screen position check */
      if (sim->isServer == FALSE) {
        conv = newX;
        conv >>= TANK_SHIFT_MAPSIZE;
        testX = (BYTE) conv;
        conv = newY;
        conv >>= TANK_SHIFT_MAPSIZE;
        testY = (BYTE) conv;
      }

      /* Collision Test */
      if (newX > 0) {
        newX += TK_WIDTH_CHECK;
      } else {
        newX -= TK_WIDTH_CHECK;
      }
      if (newY > 0) {
        newY += TK_HEIGHT_CHECK;
      } else {
        newY -= TK_HEIGHT_CHECK;
      }

      conv = newX;
      conv >>= TANK_SHIFT_MAPSIZE;
      newmx = (BYTE) conv;
      conv = newY;
      conv >>= TANK_SHIFT_MAPSIZE;
      newmy = (BYTE) conv;

      if ((mapGetSpeed(sim,mp,pb,bs,mx,newmy, FALSE, NEUTRAL)) > 0) {
        position->y = (WORLD) (position->y + moveY);
        if (sim->isServer == FALSE && position->creator == playerNum) {
          if (testY > my) {
            screenMoveViewOffsetUpCS((struct ClientSim *)sim, FALSE);
          } else if (testY < my) {
            screenMoveViewOffsetUpCS((struct ClientSim *)sim, TRUE);
          } else {
            my = my;
          }
        }
        my = newmy;
      }
      if ((mapGetSpeed(sim,mp,pb,bs,newmx,my, FALSE, NEUTRAL)) > 0) {
        position->x = (WORLD) (position->x + moveX);
        if (sim->isServer == FALSE && position->creator == playerNum) {
          if (testX > mx) {
            screenMoveViewOffsetLeftCS((struct ClientSim *)sim, FALSE);
          } else if (testX < mx) {
            screenMoveViewOffsetLeftCS((struct ClientSim *)sim, TRUE);
          }
        }
        mx = newmx;
      }
      /* Update the length */
      position->length--;
      /* Check for damage to stuff etc. */
      currentPos = mapGetPos(mp,mx,my);
      if (currentPos == DEEP_SEA) {
        /* Check for deep sea death */
        needUpdate = FALSE;
        if (position->creator == playerNum) {
		  if (sim->isServer == FALSE) {
            sim->callbacks.soundDist(sim->callbacks.ctx, tankSinkNear, mx, my);
		    tankSetLastTankDeath(&sim->tanks[playerNum],LAST_DEATH_BY_DEEPSEA); /* Override LAST_DEATH_BY_SHELL */
            sim->callbacks.messageAdd(sim->callbacks.ctx, assistantMessage, langGetText(MESSAGE_ASSISTANT), langGetText2(MESSAGE_TANKSUNK));
		  }
        }
        tkExplosionDeleteItem(tke, &position);
      } else if (currentPos == FOREST) {
        /* Check for destroy trees */
        mapSetPos(sim, mp, mx, my, GRASS, FALSE, FALSE);
        if (!sim->isServer) { sim->callbacks.soundDist(sim->callbacks.ctx, shotTreeNear, mx, my); }
      } else if (currentPos == BOAT) {
        /* Check for destroy boat */
        mapSetPos(sim, mp, mx, my, RIVER, FALSE, FALSE);
        if (!sim->isServer) { sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mx, my); }
      }
      if (!sim->isServer) { screenReCalcCS((struct ClientSim *)sim); }
    } else {
      /* Remove from data structure */
      needUpdate = FALSE;
      conv = position->x;
      conv >>= TANK_SHIFT_MAPSIZE;
      mx = (BYTE) conv;
      conv = position->y;
      conv >>= TANK_SHIFT_MAPSIZE;
      my = (BYTE) conv;
      if (position->explodeType == TK_SMALL_EXPLOSION) {
        explosionsAddItem(&sim->expl, mx, my, 0, 0 ,EXPLOSION_START);
        currentPos = mapGetPos(mp, mx, my);
        if (currentPos != RIVER && currentPos != DEEP_SEA) {
            mapSetPos(sim, mp, mx, my, CRATER, FALSE, FALSE);
            floodAddItem(&sim->ff, mx, my);
            if (!sim->isServer) { screenReCalcCS((struct ClientSim *)sim); }
        }
        if (sim->isServer) {
          count = 1;
          while (count < numLgm) {
            lgmDeathCheck(sim, lgms[count-1], position->x, position->y, NEUTRAL, &tank[count-1]);
            count++;
          }
        }
        if (!sim->isServer) { sim->callbacks.soundDist(sim->callbacks.ctx, mineExplosionNear, mx, my); }
      } else {
        tkExplosionBigExplosion(sim, mx, my, moveX, moveY, lgms, numLgm, tank, sts);
      }
      tkExplosionDeleteItem(tke, &position);
    }
    /* Get the next Item */
    if (*tke != NULL && needUpdate == TRUE) {
      position = TkExplosionTail(position);
    }
  }
}


/*********************************************************
*NAME:          tkExplosionDeleteItem
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Deletes the item for the given number
*
*ARGUMENTS:
*  tke     - Pointer to the tank explosions object
*  itemNum - The item number to get
*********************************************************/
void tkExplosionDeleteItem(tkExplosion *tke, tkExplosion *value) {
  tkExplosion del;  /* The item to delete */
  
  del = *value;
  (*value) = TkExplosionTail(del);
  if (del->prev != NULL) {
    del->prev->next = del->next;
  } else {
    /* Must be the first item - Move the master position along one */
    *tke = TkExplosionTail(*tke);
    if (NonEmpty(*tke)) {
      (*tke)->prev = NULL;
    }
  }

  if (del->next != NULL) {
    del->next->prev = del->prev;
  }
  Dispose(del);

}

/*********************************************************
*NAME:          tkExplosionGetOwnPosition
*PURPOSE:
*  Returns the map position of the fireball belonging to
*  the given player, if one exists.
*
*ARGUMENTS:
*  tke       - Pointer to the tank explosions object
*  playerNum - Player number to search for
*  mx        - Output map X
*  my        - Output map Y
*********************************************************/
bool tkExplosionGetOwnPosition(tkExplosion *tke, BYTE playerNum, BYTE *mx, BYTE *my) {
  tkExplosion q = *tke;

  while (NonEmpty(q)) {
    if (q->creator == playerNum) {
      *mx = (BYTE) (q->x >> TANK_SHIFT_MAPSIZE);
      *my = (BYTE) (q->y >> TANK_SHIFT_MAPSIZE);
      return TRUE;
    }
    q = TkExplosionTail(q);
  }
  return FALSE;
}

/*********************************************************
*NAME:          tkExplosionCalcScreenBullets
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/98
*LAST MODIFIED: 15/1/98
*PURPOSE:
*  Adds items to the sceenBullets data structure if they
*  are on screen
*
*ARGUMENTS:
*  tke       - Pointer to the tank explosions object
*  sBullet   - The screenBullets Data structure
*  leftPos   - X Map offset start
*  rightPos  - X Map offset end
*  topPos    - Y Map offset end
*  bottomPos - Y Map offset end
*********************************************************/
void tkExplosionCalcScreenBullets(tkExplosion *tke, screenBullets *sBullets, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos) {
  tkExplosion q; /* Temp Pointer */
  WORLD conv;    /* Used for conversions */ 
  BYTE mx;       /* Map Positions */
  BYTE my; 
  BYTE px;       /* Pixel Positions */
  BYTE py; 

  q = *tke;
  while (NonEmpty(q)) {
    conv = q->x;
    conv >>= TANK_SHIFT_MAPSIZE;
    mx = (BYTE) conv;
    conv = q->y;
    conv >>= TANK_SHIFT_MAPSIZE;
    my = (BYTE) conv;
    if (mx >= leftPos && mx < rightPos && my >= topPos && my < bottomPos) {
      conv = q->x;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;  
      px = (BYTE) conv;
      conv = q->y;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;
      py = (BYTE) conv;

      screenBulletsAddItem(sBullets, (BYTE) (mx-leftPos), (BYTE) (my-topPos), px, py, TANK_EXPLOSION_FRAME);
    }
    q = TkExplosionTail(q);
  }
}


/*********************************************************
*NAME:          tkExplosionCheckRemove
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/98
*LAST MODIFIED: 18/1/98
*PURPOSE:
*  An explosion has happened. Check to see if it should
*  remove from grass/building data structures etc.
*
*ARGUMENTS:
*  tke     - Pointer to the tank explosions object
*  terrain - Terrain type of the sqaure
*  mx      - Map X position 
*  my      - Map Y position
*********************************************************/
void tkExplosionCheckRemove(GameSim *sim, BYTE terrain, BYTE mx, BYTE my) {
  switch (terrain) {
  case BUILDING:
    buildingRemovePos(&sim->blds, mx,my);
    break;
  case GRASS:
    grassRemovePos(&sim->grs, mx, my);
    break;
  case RUBBLE:
    rubbleRemovePos(&sim->rbl, mx, my);
    break;
  case SWAMP:
    swampRemovePos(&sim->swp, mx, my);
    break;
  default:
    /* Do nothing */
    break;
  }
}

/*********************************************************
*NAME:          tkExplosionBigExplosion
*AUTHOR:        John Morrison
*CREATION DATE: 18/01/98
*LAST MODIFIED: 04/04/02
*PURPOSE:
*  An explosion has happened. do all the work involved 
*  to do it.
*
*ARGUMENTS:
*  tke     - Pointer to the tank explosions object
*  mp      - Pointer to map structure 
*  pb      - Pointer to pillboxes structure
*  bs      - Pointer to bases strucutre
*  mx      - Map X position 
*  my      - Map Y position
*  moveX   - Moving X direction (positive/Negative)
*  moveY   - Moving Y direction (positive/Negative)
*  lgms   - Array of lgms
*  numLgm - Number of lgms in the array
*********************************************************/
void tkExplosionBigExplosion(GameSim *sim, BYTE mx, BYTE my, int moveX, int moveY, lgm **lgms, BYTE numLgm, tank *tanks, starts *sts) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  BYTE currentPos; /* Current position */
  BYTE count;      /* Looping variable */

  if (moveX > 0) {
    moveX = TK_MOVE_RIGHT;
  } else {
    moveX = TK_MOVE_LEFT;
  }
  if (moveY > 0) {
    moveY = TK_MOVE_RIGHT;
  } else {
    moveY = TK_MOVE_LEFT;
  }
  explosionsAddItem(&sim->expl, (BYTE) (mx+moveX), (BYTE) (my+moveY), 0, 0,EXPLOSION_START);
  currentPos = mapGetPos(mp, (BYTE) (mx+moveX), (BYTE) (my+moveY));
  tkExplosionCheckRemove(sim, currentPos, (BYTE) (mx + moveX), (BYTE) (my +moveY));
  if (sim->isServer && pillsExistPos(pb, (BYTE) (mx+moveX), (BYTE) (my + moveY))) {
    pillsGetDamagePos(pb, (BYTE) (mx+moveX), (BYTE) (my+moveY), TK_DAMAGE, sim->isServer);
  } else if (currentPos != BOAT && currentPos != RIVER && currentPos != DEEP_SEA) {
      mapSetPos(sim, mp,(BYTE) (mx+moveX), (BYTE) (my+moveY), CRATER, FALSE, FALSE);
      floodAddItem(&sim->ff, (BYTE) (mx+moveX), (BYTE) (my+moveY));
  }

  if (sim->isServer) {
    count = 1;
    while (count <= numLgm) {
      lgmDeathCheck(sim, lgms[count-1], (WORLD) (((WORLD) (mx+moveX) << M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), (WORLD) (((WORLD) (my + moveY)<< M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), NEUTRAL, tanks ? &tanks[count-1] : NULL);
      count++;
    }
  }

  explosionsAddItem(&sim->expl, (BYTE) (mx+moveX), my, 0, 0,EXPLOSION_START);
  currentPos = mapGetPos(mp, (BYTE) (mx+moveX), my);
  tkExplosionCheckRemove(sim, currentPos, (BYTE) (mx + moveX), my);
  if (sim->isServer && pillsExistPos(pb, (BYTE) (mx+moveX), my)) {
    pillsGetDamagePos(pb, (BYTE) (mx+moveX), my, TK_DAMAGE, sim->isServer);
  } else if (currentPos != BOAT && currentPos != RIVER && currentPos != DEEP_SEA) {
      mapSetPos(sim, mp,(BYTE) (mx+moveX), my, CRATER, FALSE, FALSE);
    floodAddItem(&sim->ff, (BYTE) (mx+moveX), my);
  }
  if (sim->isServer) {
    count = 1;
    while (count <= numLgm) {
      lgmDeathCheck(sim, lgms[count-1], (WORLD) (((WORLD) (mx+moveX) << M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), (WORLD) ((my<< M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), NEUTRAL, tanks ? &tanks[count-1] : NULL);
      count++;
    }
  }

  explosionsAddItem(&sim->expl, mx, (BYTE) (my+moveY), 0, 0,EXPLOSION_START);
  currentPos = mapGetPos(mp, mx, (BYTE) (my+moveY));
  tkExplosionCheckRemove(sim, currentPos, mx, (BYTE) (my +moveY));
  if (sim->isServer && pillsExistPos(pb, mx, (BYTE) (my + moveY))) {
    pillsGetDamagePos(pb, mx, (BYTE) (my + moveY), TK_DAMAGE, sim->isServer);
  } else if (currentPos != BOAT && currentPos != RIVER && currentPos != DEEP_SEA) {
      mapSetPos(sim, mp, mx, (BYTE) (my+moveY), CRATER, FALSE, FALSE);
    floodAddItem(&sim->ff, mx, (BYTE) (my+moveY));
  }

  if (sim->isServer) {
    count = 1;
    while (count <= numLgm) {
      lgmDeathCheck(sim, lgms[count-1], (WORLD) ((mx<< M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), (WORLD) (((my+moveY)<< M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), NEUTRAL, tanks ? &tanks[count-1] : NULL);
      count++;
    }
  }

  explosionsAddItem(&sim->expl, mx, my, 0, 0,EXPLOSION_START);
  if (sim->isServer) {
    count = 1;
    while (count <= numLgm) {
      lgmDeathCheck(sim, lgms[count-1], (WORLD) ((WORLD) (mx << M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), (WORLD) ((WORLD) (my << M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), NEUTRAL, tanks ? &tanks[count-1] : NULL);
      count++;
    }
  }

  currentPos = mapGetPos(mp, mx, my);
  tkExplosionCheckRemove(sim, currentPos, mx, my);
  if (sim->isServer && pillsExistPos(pb, mx, my)) {
    pillsGetDamagePos(pb, mx, my, TK_DAMAGE, sim->isServer);
  } else if (currentPos != BOAT && currentPos != RIVER && currentPos != DEEP_SEA) {
      mapSetPos(sim, mp, mx, my, CRATER, FALSE, FALSE);
    floodAddItem(&sim->ff, mx, my);
  }
  if (!sim->isServer) {
    sim->callbacks.soundDist(sim->callbacks.ctx, bigExplosionNear, mx, my);
    screenReCalcCS((struct ClientSim *)sim);
  }
}


