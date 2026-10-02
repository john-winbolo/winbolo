/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


/*********************************************************
*Name:          lgm
*Filename:      lgm.c
*Author:        John Morrison
*Creation Date: 17/01/99
*Last Modified: 01/02/03
*Purpose:
*  Operations on tanks LGM are handled by this module
*********************************************************/

#include <string.h>
#include "game_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "bases.h"
#include "bolo_map.h"
#include "building.h"
#include "explosions.h"
#include "floodfill.h"
#include "frontend.h"
#include "gametype.h"
#include "../gui/lang.h"
#include "global.h"
#include "grass.h"
#include "labels.h"
#include "lgm.h"
#include "log.h"
#include "messages.h"
#include "mines.h"
#include "minesexp.h"
#include "players.h"
#include "pillbox.h"
#include "rubble.h"
#include "sounddist.h"
#include "starts.h"
#include "swamp.h"
#include "tank.h"
#include "types.h"
#include "util.h"
#include "../winbolonet/winbolonet_core.h"



/* Prototypes */

/*********************************************************
*NAME:          lgmCreate 
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 3/12/00
*PURPOSE:
*  Sets up the LGM structure
*
*ARGUMENTS:
*
*********************************************************/
lgm lgmCreate(BYTE playerNum) {
  lgm lgman;

  New(lgman);
  lgman->playerNum = playerNum;
  lgman->x = 0;
  lgman->y = 0;
  lgman->inTank = TRUE;
  lgman->isDead = FALSE;
  lgman->nextAction = LGM_IDLE;
  lgman->action = LGM_IDLE;
  lgman->state = LGM_STATE_IDLE;
  lgman->frame = 0;
  lgman->numTrees = 0;
  lgman->numMines = 0;
  lgman->numPills = LGM_NO_PILL;
  lgman->waitTime = 0;
  lgman->blessX = 0;
  lgman->blessY = 0;
  lgman->onTop = 0;
  lgman->obstructed = 0;

  return lgman;
}

/*********************************************************
*NAME:          lgmDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
* Destroys the LGM structure
*
*ARGUMENTS:
*  value - Pointer to the lgm sturcture
*********************************************************/
void lgmDestroy(lgm *value) {
  if (*value != NULL) {
    Dispose(*value);
  }
}

/*********************************************************
*NAME:          lgmUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Game tick has passed. Update the lgm position
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmUpdate(GameSim *sim, lgm *lgman, tank *tnk) {
	map *mp = &sim->mp;
	pillboxes *pb = &sim->pb;
	bases *bs = &sim->bs;
	bool isServer = sim->isServer;
	BYTE pos = 0;
	BYTE mx = 0;
	BYTE my= 0;
	WORLD wx;
	WORLD wy;

	/* Server-authoritative: server sim runs full LGM logic.
	 * isServer is TRUE for both single-player (transport_local
	 * sets it before calling serverSimTick) and networked server. */
	if (isServer == TRUE) {

		/* Man is parachuting in */
		if ((*lgman)->isDead == TRUE) {
			lgmParchutingIn(sim, lgman);
			/* check to see if lgm will land ontop of a building/pillbox/or base */
			mx = lgmGetMX(lgman);
			my = lgmGetMY(lgman);
			pos = mapGetPos(mp, mx, my);
			if (pos == BUILDING || pos == HALFBUILDING || pillsExistPos(pb, mx, my) == TRUE || basesExistPos(bs, mx, my) == TRUE) {
				(*lgman)->onTop = TRUE;
				// if base is owned by player, then, we're not ontop.
				if(basesAmOwner(sim,(*lgman)->playerNum, mx, my)==TRUE){
					(*lgman)->onTop = FALSE;
				}
			} else {
				(*lgman)->onTop = FALSE;
			}
		} else if ((*lgman)->state != LGM_STATE_IDLE) {
			/* LGM is either going to a destination or returning to the tank */
			/* Update frame */
			(*lgman)->frame++;
			if ((*lgman)->frame > LGM_MAX_FRAMES) {
				(*lgman)->frame = 0;
			}

			/* Check his not waiting */
			(*lgman)->obstructed = LGM_BRAIN_FREE;

			if ((*lgman)->waitTime > 0) {
				/* LGM is currently building something (at a destination) */
				(*lgman)->waitTime--;
			} else if ((*lgman)->state == LGM_STATE_GOING) {
				/* LGM is going to a destination */
				lgmMoveAway(sim, lgman, tnk);
			} else if (((*lgman)->state == LGM_STATE_RETURN) && ((*tnk)->deathWait == 0)) {
				/* LGM is returning to a non-dead tank */
				lgmReturn(sim, lgman, tnk);
			} else if (((*lgman)->state == LGM_STATE_RETURN) && ((*tnk)->deathWait > 0)) {
				/* LGM should sit and wait until tank is alive again */
				(*lgman)->waitTime = 1;
			}
		}
	} else {
		/* We do the following if we are a client only */
		if ((*lgman)->state != LGM_STATE_IDLE) {
			/* Man is out and about doing stuff */
			/* Update frame */
			(*lgman)->frame++;
			if ((*lgman)->frame > LGM_MAX_FRAMES) {
				(*lgman)->frame = 0;
			}
		}
		/* LGM isn't dead and he's not in the tank */
		if ((*lgman)->isDead == FALSE && (*lgman)->inTank == FALSE) {
			tankGetWorld(tnk, &wx, &wy);
			/* Multiplayer game but just a client */
			if (isServer == FALSE) {
				frontEndManStatus(clientSimFromSim(sim), FALSE, utilCalcAngle((*lgman)->x, (*lgman)->y, wx, wy));
			}
		}
	}
}

bool lgmCheckNewRequest(GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE *action, BYTE *pillNum, bool *isMine, BYTE *trees, BYTE *mines, bool perform, bool announce, BYTE *refusal);

/*********************************************************
*NAME:          lgmAddRequest
*AUTHOR:        John Morrison
*CREATION DATE: 17/01/99
*LAST MODIFIED: 01/02/03
*PURPOSE:
*  Adds a new request to the lgm structure
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  tnk    - Pointer to the tank structure
*  bs     - Pointer to the base structure
*  mapX   - X Co-ordinate of the new action
*  mapY   - Y Co-ordinate of the new action
*  action - What the new action is
*********************************************************/
void lgmAddRequest(GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action) {
  bool isServer = sim->isServer;

  if ((*lgman)->isDead == TRUE && !tankIsDestroyed(tnk)) {
	/* LGM is parachuting in and tank is alive */
    sim->callbacks.messageAdd(sim->callbacks.ctx, assistantMessage, MESSAGE_ASSISTANT, LGM_MAN_DEAD, NULL);
  } else if ((*lgman)->action != LGM_IDLE) {
    /* Busy doing something else  Place in second request */
    (*lgman)->nextX = mapX;
    (*lgman)->nextY = mapY;
    (*lgman)->nextAction = action;
  } else if (isServer == TRUE) {
    /* Server-authoritative: process the request directly */
    lgmNewPrimaryRequest(sim, lgman, tnk, mapX, mapY, action);
  } else {
    /* Network game */
    BYTE pillNum;
    bool isMine;
    BYTE trees;
    BYTE minesAmount;
    bool ok;
    ok = lgmCheckNewRequest(sim, lgman, tnk, mapX, mapY, &action, &pillNum, &isMine, &trees, &minesAmount, FALSE, TRUE, NULL);
    (*lgman)->numTrees = trees;
    (*lgman)->numMines = minesAmount;
    (*lgman)->numPills = pillNum;

    if (ok) {
      (*lgman)->action = action;
    } else {
      lgmBackInTank(sim, lgman, tnk, TRUE);
    }
  }
}


/*********************************************************
*NAME:          lgmTankDied
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  The tank has died. Cancel all pending orders if any
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmTankDied(lgm *lgman) {
  (*lgman)->nextAction = LGM_IDLE;
}


/*********************************************************
*NAME:          lgmCheckNewRequest
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  ???
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*  mapX   -
*  mapY   -
*  action - Pointer to
*  pillNum - Pointer to
*  isMine  - Pointer to
*  trees   - Pointer to
*  minesAmount - Pointer to
*  perform     -
*********************************************************/
/* Send an assistant message to the requesting player, unless this call is
   only asking whether the request would be valid. A caller that wants the
   answer without talking to the player passes announce == FALSE. */
static void lgmAssist(GameSim *sim, bool announce, langid bodyId) {
  if (announce == TRUE) {
    sim->callbacks.messageAdd(sim->callbacks.ctx, assistantMessage, MESSAGE_ASSISTANT, bodyId, NULL);
  }
}

bool lgmCheckNewRequest(GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE *action, BYTE *pillNum, bool *isMine, BYTE *trees, BYTE *minesAmount, bool perform, bool announce, BYTE *refusal) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool proceed;  /* Is it OK to proceed with the action */
  bool isBase;   /* Is the present co-ordinate a base */
  bool isPill;   /* Is the present co-ordinate a pill */
  BYTE tankX;    /* Tank co-ordinates */
  BYTE tankY;
  BYTE tankTrees;
  BYTE pos;      /* Map terrain at build request place */
  BYTE why;      /* Why it was refused, for a caller that asked for it */
  BYTE wantPill; /* The pill a place-pill order would put down */

  tankX = tankGetMX(tnk);
  tankY = tankGetMY(tnk);
  tankTrees = (*tnk)->trees;

  proceed = TRUE;
  /* Everything the square can be wrong about is the default; the branches
     that weigh the tank's stores say so where they turn the order down. */
  why = LGM_REFUSE_SQUARE;
  *isMine = FALSE;
  *trees = 0;
  *minesAmount = 0;
  *pillNum = LGM_NO_PILL;

  /* The host's say on the order, asked before the switch below rather than
     after it: the branches there spend the tank's trees and mines and take
     the carried pill off it, so a refusal arriving later would charge the
     player for an order that never happened. Every caller reaches this one
     function, so the dry run and the acting path get the same answer without
     either asking twice. A refused order is turned down the way one aimed at
     a square that cannot take it is. */
  wantPill = LGM_NO_PILL;
  if (*action == LGM_PILL_REQUEST &&
      tankPeekCarriedPill(tnk, &wantPill) != FALSE) {
    /* tankPeekCarriedPill counts from one, as the carry list and every other
       pill call in this file do; the policy surface counts from zero, so the
       question takes one off. Only where a pill was named: a tank carrying
       none leaves LGM_NO_PILL standing, which is not an index and must not be
       turned into one. Leave the subtraction alone. */
    wantPill = (BYTE)(wantPill - 1);
  }
  if (gameSimCanBuild(sim, gameSimGetTankPlayer(sim, tnk), *action, mapX, mapY,
                      wantPill) == FALSE) {
    lgmAssist(sim, announce, LGM_NO_BUILD);
    if (refusal != NULL) {
      *refusal = why;
    }
    return FALSE;
  }

  /* Get the terrain of the map square that the user clicked on */
  pos = mapGetPos(mp,mapX,mapY);
  if (pos == MINE_FOREST) {
    pos = FOREST;
  }
  isPill = pillsExistPos(pb, mapX, mapY);
  isBase = basesExistPos(bs, mapX, mapY);

  switch (*action) {
  case LGM_TREE_REQUEST:
    if (pos != FOREST || isBase == TRUE || isPill == TRUE) {
      lgmAssist(sim, announce, LGM_NO_TREE);
      proceed = FALSE;
    }
    break;
  case LGM_ROAD_REQUEST:
    if (pos == FOREST && isPill == FALSE && isBase == FALSE) {
	  /* Clicked on a tree that doesn't contain a base or pill on it */
      *action = LGM_TREE_REQUEST;
    } else if (pos == BOAT || pos == DEEP_SEA || pos == BUILDING || pos == HALFBUILDING || isPill == TRUE || isBase == TRUE) {
	  /* Clicked one of the following a boat, deep sea, building, half building, pill, base  */
      proceed = FALSE;
      lgmAssist(sim, announce, LGM_NO_BUILD);
    } else if (pos == ROAD) {
	  /* Clicked on a road */
      proceed = FALSE;
    } else if (pos == RIVER && mapX == tankX && mapY == tankY && tankIsOnBoat(tnk)) {
	  /* Clicked on a square that has a tank on a boat on it */
	  proceed = FALSE;
	  lgmAssist(sim, announce, LGM_NO_BUILD_UNDER_BOAT);
    } else if (tankGetLgmTrees(sim, tnk, sim->rules.lgm_cost_road, perform) == FALSE) {
      proceed = FALSE;
      why = LGM_REFUSE_STOCK;
      lgmAssist(sim, announce, LGM_INSUFFICIENT_TREES);
	} else {
      *trees = sim->rules.lgm_cost_road;
    }
    break;
  case LGM_BUILDING_REQUEST:
    if (pos == FOREST && isPill == FALSE && isBase == FALSE) {
      *action = LGM_TREE_REQUEST;
    } else if (pos == BOAT || pos == DEEP_SEA || isPill == TRUE || isBase == TRUE) {
      proceed = FALSE;
      lgmAssist(sim, announce, LGM_NO_BUILD);
    } else if (pos == RIVER && mapX == tankX && mapY == tankY && tankIsOnBoat(tnk)) {
	  /* Clicked on a square that has a tank on a boat on it */
	  proceed = FALSE;
	  lgmAssist(sim, announce, LGM_NO_BUILD_UNDER_BOAT);
    } else if (pos == RIVER) {
	  /* Build a wall on a river, that means build a boat */
      *action = LGM_BOAT_REQUEST;
      if (tankGetLgmTrees(sim, tnk, sim->rules.lgm_cost_boat, perform) == FALSE) {
        proceed = FALSE;
        why = LGM_REFUSE_STOCK;
        lgmAssist(sim, announce, LGM_INSUFFICIENT_TREES);
      } else {
        *trees = sim->rules.lgm_cost_boat;
      }

    } else if (tankX == mapX && tankY == mapY) {
      proceed = FALSE;
      lgmAssist(sim, announce, LGM_BUILDTANK);
    } else if (pos == HALFBUILDING) {
      if (tankGetLgmTrees(sim, tnk, sim->rules.lgm_cost_repair_building, perform) == FALSE) {
        proceed = FALSE;
        why = LGM_REFUSE_STOCK;
        lgmAssist(sim, announce, LGM_INSUFFICIENT_TREES);
      } else {
        *trees = sim->rules.lgm_cost_repair_building;
      }
    } else if (pos == BUILDING) {
      proceed = FALSE;
    } else if (tankGetLgmTrees(sim, tnk, sim->rules.lgm_cost_building, perform) == FALSE) {
        proceed = FALSE;
        why = LGM_REFUSE_STOCK;
        lgmAssist(sim, announce, LGM_INSUFFICIENT_TREES);
    } else {
      *trees = sim->rules.lgm_cost_building;
    }

    break;
  case LGM_PILL_REQUEST:
    if (pos == BOAT || pos == DEEP_SEA || pos == BUILDING || pos == HALFBUILDING || pos == RIVER || isBase == TRUE) {
      proceed = FALSE;
      lgmAssist(sim, announce, LGM_NO_BUILD);
    } else if (pos == FOREST && isPill == FALSE) {
      *action = LGM_TREE_REQUEST;
    } else if (tankX == mapX && tankY == mapY) {
      proceed = FALSE;
      lgmAssist(sim, announce, LGM_BUILDTANK);
    } else if (isPill == TRUE) {
      if (pillsGetArmourPos(pb, mapX, mapY) == sim->rules.pill_max_armour) {
        proceed= FALSE;
        lgmAssist(sim, announce, LGM_PILL_NO_NEED_REPAIR);
      } else if (tankTrees<sim->rules.lgm_cost_pill_repair) {
        proceed = FALSE;
        why = LGM_REFUSE_STOCK;
        lgmAssist(sim, announce, LGM_INSUFFICIENT_TREES);
      } else {
        /* Take a full load rather than sizing it to the damage we can see
           now. The pill can be shot a lot more while the man walks over, and
           a load picked from today's armour would arrive short. */
        *trees = sim->rules.lgm_cost_pill_repair *
                 sim->rules.lgm_pill_repair_load;
        if (tankTrees < *trees) {
          *trees = tankTrees;
        }
        tankGetLgmTrees(sim, tnk, *trees, perform);
      }
      *pillNum = LGM_NO_PILL;
    } else if ((tankGetCarriedPill(tnk, pillNum, perform)) == FALSE) {
      /* A pill the tank is not carrying is one more thing it cannot pay
         the order with, so it reads as a store the tank is short of. */
      proceed = FALSE;
      why = LGM_REFUSE_STOCK;
      lgmAssist(sim, announce, LGM_NO_PILLS);
    } else if (tankGetLgmTrees(sim, tnk, sim->rules.lgm_cost_pill_new, perform) == FALSE) {
      proceed = FALSE;
      why = LGM_REFUSE_STOCK;
      lgmAssist(sim, announce, LGM_INSUFFICIENT_TREES);
    } else {
      *trees = sim->rules.lgm_cost_pill_new;
    }
    break;
  case LGM_BOAT_REQUEST:
    if (pos != RIVER || isPill == TRUE || isBase == TRUE) {
      proceed = FALSE;
    } else if (tankGetLgmTrees(sim, tnk, sim->rules.lgm_cost_boat, perform) == FALSE) {
      proceed = FALSE;
      why = LGM_REFUSE_STOCK;
    } else {
      *trees = sim->rules.lgm_cost_boat;
    }
    break;
  default:
    /* Case LGM_REQUEST_MINE */
    if (pos == DEEP_SEA || pos == RIVER || pos == BUILDING || pos == BOAT || pos == HALFBUILDING || isPill == TRUE || isBase == TRUE) {
      proceed = FALSE;
      lgmAssist(sim, announce, LGM_NO_BUILD);
    } else if (tankGetLgmMines(sim, tnk, sim->rules.lgm_cost_mine, perform) == FALSE) {
      proceed = FALSE;
      why = LGM_REFUSE_STOCK;
      lgmAssist(sim, announce, LGM_INSUFFICIENT_MINES);
    } else {
      *minesAmount = sim->rules.lgm_cost_mine;
    }
    break;
  }
  if (proceed == TRUE) {
    /* Check for a visible mine */
    if ((minesExistPos(&sim->mns, &sim->mp, mapX, mapY)) == TRUE) {
      *isMine = TRUE;
      proceed = FALSE;
      lgmAssist(sim, announce, LGM_PILL_NO_BUILD_ON_MINE);
      if (perform == TRUE && (*lgman)->nextAction == LGM_MINE_REQUEST && (*lgman)->nextX == mapX && (*lgman)->nextY == mapY) {
        (*lgman)->nextAction = LGM_IDLE;
      }
    }
  }

  if (refusal != NULL) {
    *refusal = (proceed == TRUE) ? LGM_REFUSE_NONE : why;
  }
  return proceed;
}

/*********************************************************
*NAME:          lgmRequestIsValid
*PURPOSE:
*  Answers whether a build request at mapX,mapY would be
*  accepted right now, without acting on it and without
*  sending the player an assistant message.
*
*  For a caller replaying an order the player commanded
*  earlier: the target was frozen when they clicked, so it
*  has to be re-tested against the map as it stands before
*  it is dispatched. Asking here rather than repeating the
*  terrain and cost conditions keeps the one copy in
*  lgmCheckNewRequest.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm structure
*  tnk    - Pointer to the tank structure
*  mapX   - X Co-ordinate of the request
*  mapY   - Y Co-ordinate of the request
*  action - What the request is (LGM_*_REQUEST)
*********************************************************/
bool lgmRequestIsValid(GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action) {
  return lgmRequestRefusal(sim, lgman, tnk, mapX, mapY, action) ==
         LGM_REFUSE_NONE;
}

/*********************************************************
*NAME:          lgmRequestRefusal
*PURPOSE:
*  Why a build request at mapX,mapY would be turned down
*  right now, or LGM_REFUSE_NONE when it would be accepted.
*  Asked without acting on the request and without sending
*  the player an assistant message.
*
*  A caller acting for the player only needs the yes or no,
*  which is lgmRequestIsValid. A caller answering something
*  that is not a player has to say which of the two things
*  was wrong, so it asks here instead.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm structure
*  tnk    - Pointer to the tank structure
*  mapX   - X Co-ordinate of the request
*  mapY   - Y Co-ordinate of the request
*  action - What the request is (LGM_*_REQUEST)
*********************************************************/
BYTE lgmRequestRefusal(GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action) {
  BYTE pillNum;
  bool isMine;
  BYTE trees;
  BYTE minesAmount;
  BYTE why;

  why = LGM_REFUSE_NONE;
  /* action is taken by value: lgmCheckNewRequest rewrites it for the
     substitutions the game makes (a road order on forest becomes a tree
     harvest), and a caller only asking the question keeps its own copy.
     perform FALSE spends nothing and announce FALSE keeps lgmAssist quiet,
     so asking leaves the tank and the player exactly as they were. */
  lgmCheckNewRequest(sim, lgman, tnk, mapX, mapY, &action, &pillNum,
                     &isMine, &trees, &minesAmount, FALSE, FALSE, &why);
  return why;
}

/*********************************************************
*NAME:          lgmNewPrimaryRequest
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  A new primary request is wanted. Check if it is
*  possible. If so then make it so.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  tnk    - Pointer to the tank structure
*  bs     - Pointer to the base structure
*  mapX   - X Co-ordinate of the new action
*  mapY   - Y Co-ordinate of the new action
*  action - What the new action is
*********************************************************/
void lgmNewPrimaryRequest(GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE action) {
  map *mp = &sim->mp;
  BYTE pillNum;
  bool isMine;
  BYTE trees;
  BYTE minesAmount;

  pillNum = LGM_NO_PILL;

  /* If its OK to proceed then set it up */
  if (lgmCheckNewRequest(sim, lgman, tnk, mapX, mapY, &action, &pillNum, &isMine, &trees, &minesAmount, TRUE, TRUE, NULL) == TRUE) {
    (*lgman)->numTrees = trees;
    (*lgman)->numMines = minesAmount;
    if (isMine == TRUE) {
      (*lgman)->state = LGM_STATE_IDLE;
      (*lgman)->inTank = TRUE;
      lgmBackInTank(sim, lgman, tnk, FALSE);
    } else {
      (*lgman)->destX = mapX;
      (*lgman)->destX <<= TANK_SHIFT_MAPSIZE;
      (*lgman)->destY = mapY;
      (*lgman)->destY <<= TANK_SHIFT_MAPSIZE;
      (*lgman)->destX += MAP_SQUARE_MIDDLE;
      (*lgman)->destY += MAP_SQUARE_MIDDLE;
      tankGetWorld(tnk, &((*lgman)->x), &((*lgman)->y));
      (*lgman)->state = LGM_STATE_GOING;
      (*lgman)->action = action;
      (*lgman)->inTank = FALSE;
      switch (action) {
      case LGM_ROAD_REQUEST:
        if (mapGetPos(mp, mapX, mapY) == RIVER) {
          (*lgman)->blessX = mapX;
          (*lgman)->blessY = mapY;
        }
        break;
       case LGM_BOAT_REQUEST:
        (*lgman)->blessX = mapX;
        (*lgman)->blessY = mapY;
        break;
      case LGM_BUILDING_REQUEST:
        (*lgman)->blessX = mapX;
        (*lgman)->blessY = mapY;
        break;
      case LGM_PILL_REQUEST:
        (*lgman)->numPills = pillNum;
        (*lgman)->blessX = mapX;
        (*lgman)->blessY = mapY;
        break;
      case LGM_MINE_REQUEST:
        break;
      default:
        /* Do nothing */
        break;
      }
    }
  } else {
    (*lgman)->state = LGM_STATE_IDLE;
    tankGiveTrees(sim, tnk, trees);
    tankGiveMines(sim, tnk, minesAmount);
    if (pillNum != LGM_NO_PILL) {
      tankPutCarriedPill(tnk, pillNum);
    }
  }
}


bool lgmCheckBlessedSquare(BYTE xValue, BYTE yValue, BYTE action, map *mp, pillboxes *pb, bases *bs, tank *tnk) {
  bool returnValue; /* Value to return */
  BYTE pos;
  bool isBase;
  bool isPill;

  returnValue = TRUE;
  pos = mapGetPos(mp, xValue, yValue);
  isBase = basesExistPos(bs, xValue, yValue);
  isPill = pillsExistPos(pb, xValue, yValue);

  switch (action) {
  case LGM_TREE_REQUEST:
    if (pos == BUILDING || pos == HALFBUILDING || pos == RIVER || pos == BOAT || pos == DEEP_SEA || isPill == TRUE ) {
      returnValue = FALSE;
    }
    break;
  case LGM_ROAD_REQUEST:
    if (pos == BUILDING || pos == HALFBUILDING || pos == BOAT || pos == DEEP_SEA || isPill == TRUE ) {
      returnValue = FALSE;
    }
    break;
  case LGM_BUILDING_REQUEST:
    if (pos == BUILDING || pos == RIVER || pos == BOAT || pos == DEEP_SEA || isPill == TRUE) {
      returnValue = FALSE;
    } 

    break;
  case LGM_PILL_REQUEST:
    if (pos == BUILDING || pos == HALFBUILDING || pos == RIVER || pos == BOAT || pos == DEEP_SEA) {
      returnValue = FALSE;
    }
    break;
  case LGM_BOAT_REQUEST:
    if (pos != RIVER || isPill == TRUE) {
      returnValue = FALSE;
    }
    break;
  default:
    /* Case LGM_REQUEST_MINE */
    if (pos == DEEP_SEA || pos == RIVER || pos == BUILDING || pos == BOAT || pos == HALFBUILDING || isPill == TRUE || isBase == TRUE) {
      returnValue = FALSE;
    }
    break;
  }
  return returnValue;
}

/*********************************************************
*NAME:          lgmMoveAway
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Man is moving towrads his destination.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmMoveAway(GameSim *sim, lgm *lgman, tank *tnk) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  WORLD conv;
  BYTE speed;     /* speed of terrain currently on */
  TURNTYPE angle; /* Angle of travel */
  TURNTYPE frontAngle; /* Front end angle of travel */
  BYTE bmx;       /* Man Map co-ords */
  BYTE bmy;
  WORLD newmx;
  WORLD newmy;
  BYTE newbmx;
  BYTE newbmy;
  int xAdd;       /* Add amounts */
  int yAdd;
  bool noGo;      /* Set to true if one drection couldn't move */
  bool onBoat;    /* Is the tank on a boat */

  /* Deal with the tank being on a boat */
  onBoat = lgmCheckTankBoat(lgman, tnk,
                            (WORLD) sim->rules.lgm_boat_leave_offset);

  noGo = FALSE;
  tankGetWorld(tnk, &newmx, &newmy);
  angle = utilCalcAngle((*lgman)->x, (*lgman)->y, (*lgman)->destX, (*lgman)->destY);
  frontAngle = utilCalcAngle((*lgman)->x, (*lgman)->y, newmx, newmy);
  if (isServer == FALSE) {
    frontEndManStatus(clientSimFromSim(sim), FALSE, frontAngle);
  }
  conv = (*lgman)->x;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmx = (BYTE) conv;
  conv = (*lgman)->y;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmy = (BYTE) conv;

  if (onBoat == TRUE) {
    speed = (BYTE) sim->rules.man_speed_refuel_base;
  } else if (bmx == (*lgman)->blessX && bmy == (*lgman)->blessY) {
    speed = (BYTE) sim->rules.man_speed_refuel_base;
    if (sim->rules.man_bless_tile_terrain_speed) {
      /* man_bless_tile_terrain_speed: on the square he is going to build
         on he walks up to its centre at that square's own terrain speed,
         as he does on every other square. A square that reads 0 (a river,
         a wall, a live pillbox) keeps the base speed above, so he can
         never be left standing still on it. */
      BYTE blessSpeed = mapGetManSpeed(sim, mp, pb, bs, bmx, bmy, (*lgman)->playerNum);
      if (blessSpeed > 0) {
        speed = blessSpeed;
      }
    }
  } else {
    speed = mapGetManSpeed(sim, mp, pb, bs, bmx, bmy, (*lgman)->playerNum);
  }

  utilCalcDistance(&xAdd, &yAdd, angle, speed);
  newmx = (WORLD) ((*lgman)->x + xAdd);
  newmy = (WORLD) ((*lgman)->y + yAdd);

  newmx >>= TANK_SHIFT_MAPSIZE;
  newmy >>= TANK_SHIFT_MAPSIZE;
  newbmx = (BYTE) newmx;
  newbmy = (BYTE) newmy;

  /* Huh? */
  if ((mapGetManSpeed(sim, mp, pb, bs, bmx, newbmy, (*lgman)->playerNum)) > 0 || onBoat == TRUE) {

  }

  
/*  utilCalcDistance(&xAdd, &yAdd, angle, speed);
  newmx = (WORLD) ((*lgman)->x + xAdd);
  newmy = (WORLD) ((*lgman)->y + yAdd);

  newmx >>= TANK_SHIFT_MAPSIZE;
  newmy >>= TANK_SHIFT_MAPSIZE;
  newbmx = (BYTE) newmx;
  newbmy = (BYTE) newmy; */

  if ((mapGetManSpeed(sim, mp, pb, bs, bmx, newbmy, (*lgman)->playerNum)) > 0 || onBoat == TRUE) {
    (*lgman)->y = (WORLD) ((*lgman)->y  + yAdd);
  } else if (bmx == (*lgman)->blessX && newbmy == (*lgman)->blessY && lgmCheckBlessedSquare(bmx, newbmy, (*lgman)->action, mp, pb, bs, tnk) == TRUE) {
    (*lgman)->y = (WORLD) ((*lgman)->y + yAdd);
    
//  } else if (bmx == (*lgman)->blessX && newbmy == (*lgman)->blessY) {
//    (*lgman)->y = (WORLD) ((*lgman)->y + yAdd);
  } else {
    (*lgman)->obstructed = LGM_BRAIN_PARTIAL;
    noGo = TRUE;
    newbmy = bmy;
  }
  if ((((mapGetManSpeed(sim, mp, pb, bs, newbmx, newbmy,(*lgman)->playerNum) )) > 0 && xAdd != 0) || onBoat == TRUE) {
    (*lgman)->x = (WORLD) ((*lgman)->x + xAdd);
  } else if (newbmx == (*lgman)->blessX && bmy == (*lgman)->blessY && lgmCheckBlessedSquare(newbmx, newbmy, (*lgman)->action, mp, pb, bs, tnk) == TRUE) {
    (*lgman)->x = (WORLD) ((*lgman)->x + xAdd);
//  } else if (newbmx == (*lgman)->blessX && bmy == (*lgman)->blessY) {
//    (*lgman)->x = (WORLD) ((*lgman)->x + xAdd);
  } else if (noGo == TRUE || yAdd == 0) {
    (*lgman)->state = LGM_STATE_RETURN;
  } 

  /* Check for achieved goal */
  if (((*lgman)->x - (*lgman)->destX) >= -sim->rules.lgm_arrive_tolerance &&
      ((*lgman)->x - (*lgman)->destX) <= sim->rules.lgm_arrive_tolerance &&
      ((*lgman)->y - (*lgman)->destY) >= -sim->rules.lgm_arrive_tolerance &&
      ((*lgman)->y - (*lgman)->destY) <= sim->rules.lgm_arrive_tolerance) {
    /* Arrived */
    (*lgman)->waitTime = (BYTE) sim->rules.lgm_build_ticks;
    (*lgman)->state = LGM_STATE_RETURN;
    lgmDoWork(sim, lgman, tnk);
  }

}

/*********************************************************
*NAME:          lgmRecall
*AUTHOR:        John Morrison
*CREATION DATE: 11/09/26
*LAST MODIFIED: 11/09/26
*PURPOSE:
*  Turns the man round wherever he is, so lgmReturn walks
*  him back to the tank from the next tick. The turn is the
*  one lgmMoveAway makes above when it finds the way
*  blocked. The order waiting behind the one in hand goes
*  with him, because a man called back is not to set out
*  again the moment he arrives.
*
*ARGUMENTS:
*  sim    - Pointer to the game sim structure
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
void lgmRecall(GameSim *sim, lgm *lgman) {
  (void)sim;
  (*lgman)->state = LGM_STATE_RETURN;
  (*lgman)->nextAction = LGM_IDLE;
}


/*********************************************************
*NAME:          lgmReturn
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 2/12/00
*PURPOSE:
*  Man is moving towards the tank
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pillbox structure
*  bs     - Pointer to the base structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void lgmReturn(GameSim *sim, lgm *lgman, tank *tnk) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  WORLD conv;
  BYTE speed;     /* speed of terrain currently on */
  TURNTYPE angle; /* Angle of travel */
  BYTE bmx;       /* Man Map co-ords */
  BYTE bmy;
  WORLD newmx;
  WORLD newmy;
  BYTE newbmx;
  BYTE newbmy;
  BYTE newTerrainX=0,newTerrainY=0,currentTerrain=0;
  BYTE onTopEdgeX=0,onTopEdgeY=0;
  bool sameTerrainTypeX = FALSE,sameTerrainTypeY = FALSE;
  int xAdd;       /* Add amounts */
  int yAdd;
  bool onBoat;    /* Is the tank on a boat */

  tankGetWorld(tnk, &newmx, &newmy);

  /* Deal with the tank being on a boat */
  onBoat = lgmCheckTankBoat(lgman, tnk,
                            (WORLD) sim->rules.lgm_boat_return_offset);
  
  angle = utilCalcAngle((*lgman)->x, (*lgman)->y, newmx, newmy);

  if (isServer == FALSE) {
    frontEndManStatus(clientSimFromSim(sim), FALSE, angle);
  }

  conv = (*lgman)->x;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmx = (BYTE) conv;
  conv = (*lgman)->y;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmy = (BYTE) conv;
  
  if (tankIsDestroyed(tnk) || newmx == 0 || newmy == 0) {
    speed = 0;
    return;
  } else if ((bmx == (*lgman)->blessX && bmy == (*lgman)->blessY) || onBoat == TRUE) {
    speed = (BYTE) sim->rules.man_speed_refuel_base;
  } else {
    speed = mapGetManSpeed(sim, mp, pb, bs, bmx, bmy, (*lgman)->playerNum);
  }
  if (speed == 0) {
    /* Do Nothing */

  }
  utilCalcDistance(&xAdd, &yAdd, angle, speed);
  newmx = (WORLD) ((*lgman)->x + xAdd);
  newmy = (WORLD) ((*lgman)->y + yAdd);

  newmx >>= TANK_SHIFT_MAPSIZE;
  newmy >>= TANK_SHIFT_MAPSIZE;
  newbmx = (BYTE) newmx;
  newbmy = (BYTE) newmy;

  if ((mapGetManSpeed(sim,mp,pb,bs,bmx,newbmy, (*lgman)->playerNum)) > 0 || onBoat == TRUE) {
    (*lgman)->y = (WORLD) ((*lgman)->y + yAdd);
  } else if (bmx == (*lgman)->blessX && newbmy == (*lgman)->blessY) {
    (*lgman)->y = (WORLD) ((*lgman)->y + yAdd);
  } else {
    (*lgman)->obstructed = LGM_BRAIN_PARTIAL;
    newbmy = bmy;
  }
  if ((mapGetManSpeed(sim,mp,pb,bs,newbmx,newbmy, (*lgman)->playerNum)) > 0 || onBoat == TRUE) {
    (*lgman)->x = (WORLD) ((*lgman)->x + xAdd);
  } else if (newbmx == (*lgman)->blessX && newbmy == (*lgman)->blessY) {
    (*lgman)->x = (WORLD) ((*lgman)->x + xAdd);
  } else {
    /* Totally obstructed */
    (*lgman)->obstructed = LGM_BRAIN_TOTAL;
  }

  /* If we have moved off the blesssed build square its no longer blessed */
  if (((*lgman)->x >> TANK_SHIFT_MAPSIZE) != (*lgman)->blessX || ((*lgman)->y >> TANK_SHIFT_MAPSIZE) != (*lgman)->blessY) {
    (*lgman)->blessX = 0;
    (*lgman)->blessY = 0;
  }

  if((*lgman)->onTop == TRUE){
	/*
	  ok we landed ontop of a obstruction, so, we're going to keep moving get speed, calculate movement.
	*/
    speed = 6; 
	/* speed is a magic number currently, once we detemine what the ontop speed for the various types of 
	   terrain are, this will be adjusted
	*/ 
	utilCalcDistance(&xAdd, &yAdd, angle, speed);
	/* once this algorithm is completed, the below edge numbers need to be defined instead of magic */
	onTopEdgeX = 0;
	onTopEdgeY = 0;
    newmx = (WORLD) ((*lgman)->x + xAdd + onTopEdgeX); /* ontopedge x and y are to give an 'edge' to the obstruction */
    newmy = (WORLD) ((*lgman)->y + yAdd + onTopEdgeY); 

    newmx >>= TANK_SHIFT_MAPSIZE;
    newmy >>= TANK_SHIFT_MAPSIZE;
    newbmx = (BYTE) newmx;
    newbmy = (BYTE) newmy;
    /*
	  ok here we're going to have to check for edges, if we see the edge of the obstruction, we have to stop
	  this is so we don't go outside of the obstruction

      we have to check for edges on the x sides, and the y sides. edges will be determined by weither the terrain next to us
	  is a obstructed object also, if it is, we will run over onto it also, this will allow us to run around ontop
	  of blocks of pills, or bases, so we have to check for any pillbox, enemy bases, and buildings or half buildings.
	
	  so, we will need to know first how close we are to the edge, if we're within a certain number of world coordinates to the 
	  edge of the tile we're on, do a check for a tile we can run onto in the neighbouring area.
	*/
	/* detect the terrain types, don't check diagonal squares. becuase the lgm can't run ontop of diagonal squares */
	currentTerrain = mapGetPos(mp,bmx,bmy);
	newTerrainX = mapGetPos(mp,newbmx,bmy);
   	newTerrainY = mapGetPos(mp,bmx,newbmy);

    if (currentTerrain == BUILDING||currentTerrain == HALFBUILDING){
		if (newTerrainX == BUILDING||newTerrainX == HALFBUILDING){
			sameTerrainTypeX = TRUE;
		}
		if (newTerrainY == BUILDING||newTerrainY == HALFBUILDING){
			sameTerrainTypeY = TRUE;
		}
	} else {
		/* do the check to see if pills/bases are beside each other, to allow the lgm to run around ontop of pills or bases.
		   if pills exist in the current location, then check to see if the new location is also a pill, if it is, you can go onto it.
		*/
		if (pillsExistPos(pb, bmx, bmy) == TRUE){
			if (pillsExistPos(pb, newbmx, bmy) == TRUE){
				sameTerrainTypeX = TRUE;
			}
			if (pillsExistPos(pb, bmx, newbmy) == TRUE){
				sameTerrainTypeY = TRUE;
			}
		} else {
			/* bases will be a bit more complicated, I will have to check for base ownership.
			   can't have tbe lgm running over onto a based owned by the player.
			*/
			if (basesExistPos(bs, bmx, bmy) == TRUE)
			{
				if (basesExistPos(bs, newbmx, bmy) == TRUE){
					if(basesAmOwner(sim,(*lgman)->playerNum, newbmx, bmy)==TRUE){
					  sameTerrainTypeX = FALSE;
					} else {
  					  sameTerrainTypeX = TRUE;
					}
				}
				if (basesExistPos(bs, bmx, newbmy) == TRUE){
					if(basesAmOwner(sim,(*lgman)->playerNum, bmx, newbmy)==TRUE){
					  sameTerrainTypeY = FALSE;
					} else {
					  sameTerrainTypeY = TRUE;
					}
				}
			} else {
			  /* must not be ontop of a building anymore, since its failed all the checks, onTop = false*/
			  (*lgman)->onTop = FALSE;
			}
		} 
	}
	// if the lgm's old mapx coordinate is still the same, then we can add xAdd and yAdd to the lgm's coordinates)
	// ok so the below works, now we need to modify it, to allow us to run onto tiles that are next to us.
	if (bmx == newbmx||sameTerrainTypeX == TRUE){
	  (*lgman)->x = (WORLD) ((*lgman)->x + xAdd);
	}
	if (bmy == newbmy||sameTerrainTypeY == TRUE){
	  (*lgman)->y = (WORLD) ((*lgman)->y + yAdd);
	}
	speed = 0;
  }

  /* Check for achieved goal */
  tankGetWorld(tnk, &newmx, &newmy);
  if (((*lgman)->x - newmx) >= -sim->rules.lgm_return_tolerance &&
      ((*lgman)->x - newmx) <= sim->rules.lgm_return_tolerance &&
      ((*lgman)->y - newmy) >= -sim->rules.lgm_return_tolerance &&
      ((*lgman)->y - newmy) <= sim->rules.lgm_return_tolerance) {
    /* Arrived back at tank */
    (*lgman)->state = LGM_STATE_IDLE;
    (*lgman)->inTank = TRUE;
    (*lgman)->action = LGM_IDLE;
    (*lgman)->blessX = 0;
    (*lgman)->blessY = 0;
    lgmBackInTank(sim, lgman, tnk, TRUE);
    if (isServer == FALSE) {
      frontEndManClear(clientSimFromSim(sim));
    }
  }
}



/*********************************************************
*NAME:          lgmDoWork
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Man has arrived at objective. Do whatever it is he
*  came to do.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  mp - Pointer to the map structure
*  pb - Pointer to the pillbox structure 
*  bs - Pointer to the bases structure 
*********************************************************/
void lgmDoWork(GameSim *sim, lgm *lgman, tank *tnk) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  WORLD conv;      /* Used for conversion */
  BYTE bmx;        /* Dest Map co-ords */
  BYTE bmy;
  pillbox addPill; /* Pill to add if required */
  bool isMine;     /* Is the item a mine */
  bool isPill;
  bool isBase;
  BYTE terrain;

  conv = (*lgman)->destX;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmx = (BYTE) conv;
  conv = (*lgman)->destY;
  conv >>= TANK_SHIFT_MAPSIZE;
  bmy = (BYTE) conv;
  isPill = pillsExistPos(pb, bmx, bmy);
  isBase = basesExistPos(bs, bmx, bmy);

  isMine = FALSE;
  terrain = mapGetPos(mp, bmx, bmy);
  if (terrain >= MINE_START && terrain <= MINE_END) {
    terrain -= MINE_SUBTRACT;
    isMine = TRUE;
  }


  switch ((*lgman)->action) {
  case LGM_TREE_REQUEST:
    minesExpAddItem(sim, &sim->minesExplosions, mp, bmx, bmy);
    if (terrain == FOREST && isBase == FALSE && isPill == FALSE) {
      if (isMine == TRUE) {
        mapSetPos(sim, mp, bmx, bmy, (BYTE) (GRASS+MINE_SUBTRACT), TRUE, FALSE);
      } else {
        mapSetPos(sim, mp, bmx, bmy, GRASS, TRUE, FALSE);
      }
      (*lgman)->numTrees = (BYTE) sim->rules.lgm_gather_trees;
      sim->callbacks.soundDist(sim->callbacks.ctx, farmingTreeNear, bmx, bmy);
      if (sim->callbacks.recordPlayerAction) sim->callbacks.recordPlayerAction(sim->callbacks.ctx, (*lgman)->playerNum, PLAYER_ACTION_FARM, bmx, bmy);
      if (isServer && sim->callbacks.built) {
        sim->callbacks.built(sim->callbacks.ctx, (*lgman)->playerNum,
                             (*lgman)->action, bmx, bmy);
      }
    }
    if (!sim->isServer) { clientSimRecalc((struct ClientSim *)sim); }
    break;
  case LGM_ROAD_REQUEST:
/* HUH?    minesExpAddItem(mp, bmx, bmy); */
    if (terrain != BUILDING && terrain != HALFBUILDING && terrain != BOAT && terrain != DEEP_SEA && isPill == FALSE && isBase == FALSE) {
      if (isMine == TRUE) {
        mapSetPos(sim, mp, bmx, bmy, (BYTE) (ROAD+MINE_SUBTRACT), TRUE, FALSE);
      } else {
        mapSetPos(sim, mp, bmx, bmy, ROAD, TRUE, FALSE);
      }
      (*lgman)->numTrees = 0;
      sim->callbacks.soundDist(sim->callbacks.ctx, manBuildingNear, bmx, bmy);

      lgmCheckRemove(sim, terrain, bmx, bmy);
      if (isServer && sim->callbacks.built) {
        sim->callbacks.built(sim->callbacks.ctx, (*lgman)->playerNum,
                             (*lgman)->action, bmx, bmy);
      }
      if (!sim->isServer) { clientSimRecalc((struct ClientSim *)sim); }
    }
    break;
  case LGM_BUILDING_REQUEST:
    if (terrain != BUILDING && terrain != RIVER && terrain != BOAT && terrain != DEEP_SEA && isPill == FALSE && isBase == FALSE) {
      if (isMine != TRUE) {
        mapSetPos(sim, mp, bmx, bmy, BUILDING, TRUE, FALSE);
        /* Only this branch builds anything — the other one sets off the mine
           that was under the square and leaves it a crater. */
        if (isServer && sim->callbacks.built) {
          sim->callbacks.built(sim->callbacks.ctx, (*lgman)->playerNum,
                               (*lgman)->action, bmx, bmy);
        }
      } else {
        minesExpAddItem(sim, &sim->minesExplosions, mp, bmx, bmy);
      }
      (*lgman)->numTrees = 0;
      sim->callbacks.soundDist(sim->callbacks.ctx, manBuildingNear, bmx, bmy);
      lgmCheckRemove(sim, terrain, bmx, bmy);
      if (!sim->isServer) { clientSimRecalc((struct ClientSim *)sim); }
    }
    break;
  case LGM_BOAT_REQUEST:
    if (terrain == RIVER) {
      mapSetPos(sim, mp, bmx, bmy, BOAT, TRUE, FALSE);
      (*lgman)->numTrees = 0;
      if (isServer && sim->callbacks.built) {
        sim->callbacks.built(sim->callbacks.ctx, (*lgman)->playerNum,
                             (*lgman)->action, bmx, bmy);
      }
      if (!sim->isServer) { clientSimRecalc((struct ClientSim *)sim); }
    }
    break;
  case LGM_MINE_REQUEST:
    if ((isPill == FALSE && isBase == FALSE) && (terrain == SWAMP || terrain == CRATER || terrain == ROAD || terrain == FOREST || terrain == RUBBLE || terrain == GRASS)) {
      if (isMine == TRUE) {
        minesExpAddItem(sim, &sim->minesExplosions, mp, bmx, bmy);
      } else {
        mapSetPos(sim, mp, bmx, bmy, (BYTE) (terrain + MINE_SUBTRACT), FALSE, FALSE);
        minesAddItem(&sim->mns, bmx, bmy);
        minesSetOwner(&sim->mns, bmx, bmy, (*lgman)->playerNum);
        if (sim->callbacks.recordPlayerAction) sim->callbacks.recordPlayerAction(sim->callbacks.ctx, (*lgman)->playerNum, PLAYER_ACTION_MINE, bmx, bmy);
        if (isServer && sim->callbacks.mineLaid) {
          sim->callbacks.mineLaid(sim->callbacks.ctx, (*lgman)->playerNum, bmx, bmy);
        }
        (*lgman)->numMines = 0;
        if (sim->isServer && sim->hiddenMines) {
          sim->callbacks.mineVisible(sim->callbacks.ctx, bmx, bmy, (*lgman)->playerNum);
        }
        sim->callbacks.soundDist(sim->callbacks.ctx, manLayingMineNear, bmx, bmy);
      }
      if (!sim->isServer) { clientSimRecalc((struct ClientSim *)sim); }
    }
    break;
  case LGM_PILL_REQUEST:
    addPill.x = bmx;
    addPill.y = bmy;
    if ((*lgman)->numPills == LGM_NO_PILL) {
      /* Repair pill */
      if (isPill == TRUE) {
        /* Keep whatever the repair didn't need — it rides back to the tank */
        (*lgman)->numTrees = pillsRepairPos(sim, pb, bmx, bmy, (*lgman)->numTrees);
        sim->callbacks.soundDist(sim->callbacks.ctx, manBuildingNear, bmx, bmy);
        if (isServer && sim->callbacks.built) {
          sim->callbacks.built(sim->callbacks.ctx, (*lgman)->playerNum,
                               (*lgman)->action, bmx, bmy);
        }
      }
    } else {
      if (isPill == FALSE && isBase == FALSE && minesExistPos(&sim->mns, &sim->mp, bmx, bmy) == FALSE && terrain != BUILDING && terrain != HALFBUILDING && terrain != RIVER && terrain != BOAT && terrain != DEEP_SEA) {
        if (isMine == TRUE) {
          /* Whose mine he set off, read before anything below can take it off
             the field. The can_die question names the layer; nobody is
             credited, as with every other mine. */
          BYTE mineLayer = minesGetOwner(&sim->mns, bmx, bmy);
          minesExpAddItem(sim, &sim->minesExplosions, mp, bmx, bmy);
          floodAddItem(&sim->ff, bmx, bmy,
                       (BYTE) sim->rules.flood_fill_ticks);
          lgmDeathCheck(sim, lgman, (WORLD) ((bmx << M_W_SHIFT_SIZE) +MAP_SQUARE_MIDDLE), (WORLD) ((bmy<< M_W_SHIFT_SIZE)+MAP_SQUARE_MIDDLE), NEUTRAL, mineLayer, DMG_SRC_MINE, DMG_NO_PILL, tnk);
          sim->callbacks.soundDist(sim->callbacks.ctx, mineExplosionNear, bmx, bmy);
          mapSetPos(sim, mp, bmx, bmy, CRATER, TRUE, FALSE);
        } else {
          (*lgman)->numTrees = 0;
          addPill.owner = (*lgman)->playerNum;
          addPill.armour = (BYTE) sim->rules.pill_max_armour;
          addPill.speed = pillsGetAttackSpeed(pb, (*lgman)->numPills);
          addPill.coolDown = 0;
          addPill.inTank = FALSE;
          addPill.justSeen = FALSE;
          pillsSetPill(sim, pb, &addPill, (*lgman)->numPills);
          sim->callbacks.soundDist(sim->callbacks.ctx, manBuildingNear, bmx, bmy);
          if (isServer == FALSE) {
            frontEndStatusPillbox(clientSimFromSim(sim), (*lgman)->numPills, (pillsGetAllianceNum(sim, pb, (*lgman)->numPills)));
          }
          /* numPills is the pillbox number, counted from one, and is about to
             be cleared; the event carries the 0-based item[] slot. The armour
             is read back off the square rather than from addPill: pillsSetPill
             clamps what it is handed to the sim's cap, and the event has to
             say what the pillbox actually has on it. */
          if (isServer && sim->callbacks.pillPlaced &&
              (*lgman)->numPills > 0) {
            sim->callbacks.pillPlaced(sim->callbacks.ctx, (*lgman)->playerNum,
                                      (BYTE)((*lgman)->numPills - 1), bmx, bmy,
                                      pillsGetArmourPos(pb, bmx, bmy));
          }
          (*lgman)->numPills = LGM_NO_PILL;
          if (sim->callbacks.recordPlayerAction) sim->callbacks.recordPlayerAction(sim->callbacks.ctx, (*lgman)->playerNum, PLAYER_ACTION_BUILD, bmx, bmy);
        }
      }
    }
    lgmCheckRemove(sim, terrain, bmx, bmy);
    if (!sim->isServer) { clientSimRecalc((struct ClientSim *)sim); }
    break;
  default:
    /* do nothing */
    break;
  }
}


/*********************************************************
*NAME:          lgmBackInTank
*AUTHOR:        John Morrison
*CREATION DATE: 18/01/99
*LAST MODIFIED: 01/02/03
*PURPOSE:
*  Man has arrived back in tank. Dump stuff off
*
*ARGUMENTS:
*  lgman     - Pointer to the lgm sturcture
*  mp        - Pointer to the map structure
*  pb        - Pointer to the pillbox structure
*  bs        - Pointer to the base structure
*  tnk       - Pointer to the tank structure
*  sendItems - If TRUE, send the carriend items back
*              to the client
*********************************************************/
void lgmBackInTank(GameSim *sim, lgm *lgman, tank *tnk, bool sendItems) {
  bool isServer = sim->isServer;
  (void)sendItems;

  if ((*lgman)->numTrees > 0) {
    tankGiveTrees(sim, tnk, (*lgman)->numTrees);
    (*lgman)->numTrees = 0;
  }
  if ((*lgman)->numPills != LGM_NO_PILL) {
    tankPutCarriedPill(tnk, (*lgman)->numPills);
    (*lgman)->numPills = LGM_NO_PILL;
  }
  if ((*lgman)->numMines > 0) {
    tankAddMines(sim, tnk, (*lgman)->numMines);
    (*lgman)->numMines = 0;
  }

  if (isServer == TRUE) {
    if ((*lgman)->nextAction != LGM_IDLE) {
      /* Server-authoritative: immediately start the queued action */
      BYTE nextX = (*lgman)->nextX;
      BYTE nextY = (*lgman)->nextY;
      BYTE nextAct = (*lgman)->nextAction;
      (*lgman)->nextAction = LGM_IDLE;
      lgmNewPrimaryRequest(sim, lgman, tnk, nextX, nextY, nextAct);
    }
  } else {
    if ((*lgman)->nextAction != LGM_IDLE) {
      /* Network game — client side */
      BYTE pillNum;
      bool isMine;
      BYTE trees;
      BYTE minesAmount;
      bool ok;
      BYTE action;
      action = (*lgman)->nextAction;
      ok = lgmCheckNewRequest(sim, lgman, tnk, (*lgman)->nextX, (*lgman)->nextY, &action, &pillNum, &isMine, &trees, &minesAmount, FALSE, TRUE, NULL);
      (*lgman)->numTrees = trees;
      (*lgman)->numMines = minesAmount;

      if (ok) {
        (*lgman)->action = action;
      }
      (*lgman)->nextAction = LGM_IDLE;
    }
  }
}

/*********************************************************
*NAME:          lgmSetCarried
*AUTHOR:        John Morrison
*CREATION DATE: 11/09/26
*LAST MODIFIED: 11/09/26
*PURPOSE:
*  Writes what the man is carrying out to his job. Both
*  amounts are capped at what a tank can hold, because
*  everything he carries came out of one and lgmBackInTank
*  above unloads it into one.
*
*ARGUMENTS:
*  sim    - The game whose rules the caps come from
*  lgman  - Pointer to the lgm sturcture
*  trees  - Trees he is to carry
*  mines  - Mines he is to carry
*********************************************************/
void lgmSetCarried(GameSim *sim, lgm *lgman, BYTE trees, BYTE mines) {
  BYTE maxTrees = (BYTE) sim->rules.tank_full_trees;
  BYTE maxMines = (BYTE) sim->rules.tank_full_mines;
  (*lgman)->numTrees = (trees > maxTrees) ? maxTrees : trees;
  (*lgman)->numMines = (mines > maxMines) ? maxMines : mines;
}

/*********************************************************
*NAME:          lgmOnScreen
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns wehther the man is on screen
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  leftPos   - Left bounds of screen
*  rightPos  - Right bounds of screen
*  top    - Top bounds of screen
*  bottom - Bottom bounds of screen
*********************************************************/
bool lgmOnScreen(lgm *lgman, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  bool returnValue; /* Value to return */
  BYTE x;     /* Map X and Y Positions (relative to screen) */
  BYTE y;
 
  returnValue = FALSE;
  x = (BYTE) (( (unsigned int)(*lgman)->x -1) >> TANK_SHIFT_MAPSIZE);
  y = (BYTE) (((unsigned int) (*lgman)->y -2) >> TANK_SHIFT_MAPSIZE);
  if (x >= leftPos && x < rightPos && y >= top && y < bottom && (*lgman)->state != LGM_STATE_IDLE && (*lgman)->inTank == FALSE) {
    returnValue = TRUE; 
  }
  return returnValue;
}

/*********************************************************
*NAME:          lgmGetScreenCoords
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns wehther the man is on screen
*
*ARGUMENTS:
*  lgman   - Pointer to the lgm sturcture
*  leftPos - Left bounds of screen
*  topPos  - Top bounds of screen
*  mx      - X Map co ord
*  my      - Y Map co ord
*  px      - X pixel co ord
*  py      - Y pixel co ord
*********************************************************/
void lgmGetScreenCoords(lgm *lgman, BYTE leftPos, BYTE topPos, BYTE *mx, BYTE *my, BYTE *px, BYTE *py, BYTE *frame) {
  WORLD conv; /* Used for Bit shifting */

  if ((*lgman)->isDead == TRUE) {
    *frame = LGM_HELICOPTER_FRAME;
    conv = (*lgman)->x-MAP_SQUARE_MIDDLE;
    conv >>= TANK_SHIFT_MAPSIZE;
    *mx = (BYTE) conv - leftPos;
    conv = (*lgman)->y-MAP_SQUARE_MIDDLE;
    conv >>= TANK_SHIFT_MAPSIZE;
    *my = (BYTE) conv - topPos;
    conv = (*lgman)->x-MAP_SQUARE_MIDDLE;
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;
    *px = (BYTE) conv;
    conv = (*lgman)->y-MAP_SQUARE_MIDDLE;
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;
    *py = (BYTE) conv;
  } else {
    *frame = (*lgman)->frame;
    conv = (*lgman)->x - 1;
    conv >>= TANK_SHIFT_MAPSIZE;
    *mx = (BYTE) conv - leftPos;
    conv = (*lgman)->y - 2;
    conv >>= TANK_SHIFT_MAPSIZE;
    *my = (BYTE) conv - topPos;
    conv = (*lgman)->x -1 ;
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;
    *px = (BYTE) conv;
    conv = (*lgman)->y -2;
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;
    *py = (BYTE) conv;
  }
}


/*********************************************************
*NAME:          lgmDeathCheckAtPosition
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 17/04/26
*PURPOSE:
*  Called when an item explodes to check to see if the
*  lgm should be killed. Tests against supplied world
*  position (for lag compensation). Death effects use the
*  LGM's real current position.
*
*ARGUMENTS:
*  sim       - Pointer to the game sim structure
*  lgman     - Pointer to the lgm pointer
*  lgmWorldX - LGM X world position to test against
*  lgmWorldY - LGM Y world position to test against
*  wx        - X World co ord of explosion
*  wy        - Y World co ord of explosion
*  owner     - Who is credited with the kill (NEUTRAL for
*              mines and blasts)
*  attacker  - Who the can_die question names: the shell's
*              owner, the mine's layer or the tank whose
*              blast it was
*  cause     - The DMG_SRC_* the can_die question is handed
*  pill      - The pill index whose shell it was, or
*              DMG_NO_PILL
*  tnk       - Pointer to the tank
*********************************************************/
void lgmDeathCheckAtPosition(GameSim *sim, lgm *lgman, WORLD lgmWorldX, WORLD lgmWorldY, WORLD wx, WORLD wy, BYTE owner, BYTE attacker, BYTE cause, BYTE pill, tank *tnk) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  bool isServer = sim->isServer;
  BYTE checkMapX;                   /* LGM X Map co-ordinate (from check position) */
  BYTE checkMapY;                   /* LGM Y Map co-ordinate (from check position) */
  bool solid;                       /* Is the map square the lgm on solid or not */
  bool dead;                        /* Are we dead */
  double distance;
  BYTE pos;
  BYTE mx, my;


  if (isServer == TRUE && (*lgman)->isDead == FALSE && (*lgman)->inTank == FALSE) {
    WORLD conv;
    dead = FALSE;
    /* Map coords from check position — used for hit detection */
    conv = lgmWorldX;
    conv >>= 8;
    checkMapX = (BYTE) conv;
    checkMapY = (BYTE) ((unsigned int) (lgmWorldY) >> 8);
    mx = (BYTE) (wx >> 8);
    my = (BYTE) (wy >> 8);

    utilIsItemInRange(lgmWorldX, lgmWorldY, wx, wy, (WORLD) sim->rules.pill_range, &distance);
    pos = mapGetPos(mp, mx, my);
    solid = FALSE;
    if (pos == BUILDING || pos == HALFBUILDING || pillsExistPos(pb, mx, my) == TRUE || basesExistPos(bs, mx, my) == TRUE) {
      solid = TRUE;
    }
    if (solid == FALSE && distance <= MAP_SQUARE_MIDDLE) {
      dead = TRUE;
    } else if (solid == TRUE && checkMapX == mx && checkMapY == my) {
      dead = TRUE;
    }

    if (dead == TRUE) {
      /* The engine's own test says the blast caught him; the host has the
         last word, and a refusal leaves him untouched. Asked here rather
         than inside lgmKill, because a death a script orders outright goes
         straight to lgmKill and is not the host's to reconsider. Every
         builder death in the engine comes through this test, so this covers
         shells, mine and tank explosions and his own mine alike. The
         question may name somebody the kill below does not credit: a mine's
         layer and a blast's tank are asked about and credited with nothing. */
      if (gameSimCanDie(sim, DIE_KIND_BUILDER, (*lgman)->playerNum, attacker,
                        cause, pill) != FALSE) {
        lgmKill(sim, lgman, tnk, owner);
      }
    }
  }
}

/*********************************************************
*NAME:          lgmDropCarriedPill
*AUTHOR:        John Morrison
*CREATION DATE: 16/09/26
*LAST MODIFIED: 16/09/26
*PURPOSE:
*  Puts the pillbox the man is carrying down on the map and
*  leaves him carrying nothing. It lands on the first square
*  at or below his feet that will hold one, dead, owned by
*  him. Does nothing if he is carrying no pillbox.
*
*  Dying does this — lgmKill calls it — and so must his
*  player leaving while he is out on the errand. A pillbox in
*  his hands is in no tank's carry list, so the tank teardown
*  that drops the rest of a leaver's cargo never sees it.
*  Left undone, its record survives still marked as carried
*  by a man who no longer exists: off the map, nobody's to
*  pick up, gone for the rest of the round.
*
*ARGUMENTS:
*  sim    - The game the man belongs to
*  lgman  - Pointer to the lgm pointer
*********************************************************/
void lgmDropCarriedPill(GameSim *sim, lgm *lgman) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  pillbox item;           /* Item to write back into the pillbox list */
  BYTE lgmMapX;           /* The square he is standing on */
  BYTE lgmMapY;
  BYTE pos;
  bool finishedPillPlace; /* Used to place pills properly */
  BYTE pillPlaceX;
  BYTE pillPlaceY;
  BYTE count;
  WORLD conv;

  if (*lgman == NULL || (*lgman)->numPills == LGM_NO_PILL) {
    return;
  }

  conv = (*lgman)->x;
  conv >>= 8;
  lgmMapX = (BYTE) conv;
  lgmMapY = (BYTE) ((unsigned int) ((*lgman)->y) >> 8);

  if (sim->isServer == TRUE) {
    finishedPillPlace = FALSE;
    count = 0;
    pillPlaceX = lgmMapX;
    pillPlaceY = lgmMapY;
    while (finishedPillPlace == FALSE) {
      item.x = pillPlaceX;
      item.y = pillPlaceY+count;
      if (item.x > MAP_MINE_EDGE_LEFT && item.x < MAP_MINE_EDGE_RIGHT && item.y > MAP_MINE_EDGE_TOP && item.y < MAP_MINE_EDGE_BOTTOM) {
        pos = mapGetPos(mp, item.x, item.y);
        if (pillsExistPos(pb, item.x, item.y) == FALSE && basesExistPos(bs, item.x, item.y) == FALSE && pos != BUILDING && pos != HALFBUILDING && pos != BOAT) {
          finishedPillPlace = TRUE;
        }
      }
      count++;
      if (count == sim->rules.lgm_pill_drop_search &&
          finishedPillPlace == FALSE) {
        count = 0;
        item.y = pillPlaceY;
        pillPlaceX++;
      }
    }
    item.armour = 0;
    item.owner = (*lgman)->playerNum;
    item.speed = (BYTE) sim->rules.pill_attack_ticks;
    item.reload = (BYTE) sim->rules.pill_attack_ticks;
    item.coolDown = 0;
    item.inTank = FALSE;
    item.justSeen = FALSE;
    pillsSetPill(sim, pb, &item, (*lgman)->numPills);
    /* Same event a builder finishing the job raises, because it is the same
       fact: a pillbox that was in somebody's hands is on the map again. The
       armour byte is what says this one is dead rather than built. numPills
       is counted from one and the event carries the 0-based item[] slot. */
    if (sim->callbacks.pillPlaced && (*lgman)->numPills > 0) {
      sim->callbacks.pillPlaced(sim->callbacks.ctx, (*lgman)->playerNum,
                                (BYTE)((*lgman)->numPills - 1), item.x, item.y,
                                pillsGetArmourPos(pb, item.x, item.y));
    }
  }
  (*lgman)->numPills = LGM_NO_PILL;
  if (sim->isServer == FALSE) { clientSimRecalc((struct ClientSim *)sim); }
}

/*********************************************************
*NAME:          lgmKill
*AUTHOR:        John Morrison
*CREATION DATE: 11/09/26
*LAST MODIFIED: 11/09/26
*PURPOSE:
*  Kills the man where he stands. The caller decides whether
*  he dies — lgmDeathCheckAtPosition above tests an
*  explosion against him — and this is what dying does: the
*  dying sound, the pillbox he was carrying put down on the
*  nearest square that will hold one, the dead flag and the
*  helicopter frame, the tank marked as where he is headed
*  and a random start to fly in from, then the record, the
*  WinBolo.net reports and the newswire event.
*
*ARGUMENTS:
*  sim    - Pointer to the game sim structure
*  lgman  - Pointer to the lgm pointer
*  tnk    - Pointer to the tank, or NULL when the man has
*           none left to fly back to
*  owner  - Who is credited with the kill (NEUTRAL for a
*           death nobody caused, as a mine is)
*********************************************************/
void lgmKill(GameSim *sim, lgm *lgman, tank *tnk, BYTE owner) {
  bool isServer = sim->isServer;
  starts *sts = &sim->ss;
  BYTE lgmMapX;                     /* LGM X Map co-ordinate (from real position) */
  BYTE deathMX;                     /* The square he died on, for the record */
  BYTE deathMY;
  BYTE lgmMapY;                     /* LGM Y Map co-ordinate (from real position) */
  TURNTYPE dummy;                   /* Dummy variable used for paremeter passing */
  WORLD conv;

  /* Map coords from real position — used for pill drop, sound, etc.
   * Raw >>8, the same mapping the movement code uses (lgmMoveAway /
   * lgmReturn): the man's x/y is his authoritative hit point. The old
   * -1/-2 world-unit nudge (1/16th of a game pixel) could map a man
   * pinned flush against a wall's south/east edge INTO the wall square,
   * so a shell demolishing that wall killed a man standing on open
   * ground beside it — movement never lets him enter a solid square. */
  conv = (*lgman)->x;
  conv >>= 8;
  lgmMapX = (BYTE) conv;
  lgmMapY = (BYTE) ((unsigned int) ((*lgman)->y) >> 8);

  sim->callbacks.soundDist(sim->callbacks.ctx, manDyingNear, lgmMapX, lgmMapY);
  (*lgman)->isDead = TRUE;
  (*lgman)->frame = LGM_HELICOPTER_FRAME;
  (*lgman)->numTrees = 0;
  (*lgman)->numMines = 0;
  (*lgman)->nextAction = LGM_IDLE;
  lgmDropCarriedPill(sim, lgman);
  if (tnk != NULL && *tnk != NULL) {
    tankGetWorld(tnk, &((*lgman)->destX), &((*lgman)->destY));
  } else {
    /* Owner has no live tank (e.g. the player left while their man was
       out of the tank). Fall back to the man's current position rather
       than dereferencing a destroyed tank. */
    (*lgman)->destX = (*lgman)->x;
    (*lgman)->destY = (*lgman)->y;
  }

  /* Check for tank in mines (i.e. dead) send builder back to spoke it died*/
  if ((*lgman)->destX <= ((MAP_MINE_EDGE_LEFT+1) << 8) || (*lgman)->destX >= ((MAP_MINE_EDGE_RIGHT-1) << 8) || (*lgman)->destY <= ((MAP_MINE_EDGE_TOP+1) << 8) || (*lgman)->destY >= ((MAP_MINE_EDGE_BOTTOM-1) << 8)) {
    (*lgman)->destX = (*lgman)->x;
    (*lgman)->destY = (*lgman)->y;
  }

  /* Where he died, before the fly-in start below overwrites his position:
     the record and the loss event name this square, not the one he flies
     back in from. */
  deathMX = (BYTE)((*lgman)->x >> M_W_SHIFT_SIZE);
  deathMY = (BYTE)((*lgman)->y >> M_W_SHIFT_SIZE);

  startsGetRandStart(sim, sts, &lgmMapX, &lgmMapY, &dummy);
  (*lgman)->x = lgmMapX;
  (*lgman)->x <<= TANK_SHIFT_MAPSIZE;
  (*lgman)->x += MAP_SQUARE_MIDDLE;
  (*lgman)->y = lgmMapY;
  (*lgman)->y <<= TANK_SHIFT_MAPSIZE;
  (*lgman)->y += MAP_SQUARE_MIDDLE;
  if (isServer == FALSE) {
    frontEndManStatus(clientSimFromSim(sim), TRUE, 0.0f);
  }

  /* Log it */
  logAddEvent(log_LostMan, (*lgman)->playerNum, 0, 0, 0, 0, NULL);
  /* WinBolo.net it */
  winbolonetAddEvent(WINBOLO_NET_EVENT_LGM_LOST, TRUE, (*lgman)->playerNum, WINBOLO_NET_NO_PLAYER,
                     playersIsBot(&sim->plyrs, (*lgman)->playerNum), FALSE);
  if (owner != NEUTRAL) {
    winbolonetAddEvent(WINBOLO_NET_EVENT_LGM_KILL, TRUE, owner, (*lgman)->playerNum,
                       playersIsBot(&sim->plyrs, owner), playersIsBot(&sim->plyrs, (*lgman)->playerNum));
  }
  /* Report the loss. Every client's newswire line comes from the event this
   * raises, read against its quiet byte; the server's own message callback
   * drops newswire text, so there is no line written here. */
  if (sim->isServer) {
    if (sim->callbacks.lgmDied) {
      sim->callbacks.lgmDied(sim->callbacks.ctx, (*lgman)->playerNum, owner,
                             deathMX, deathMY);
    }
  }
}

void lgmDeathCheck(GameSim *sim, lgm *lgman, WORLD wx, WORLD wy, BYTE owner, BYTE attacker, BYTE cause, BYTE pill, tank *tnk) {
  if (*lgman == NULL) return;
  lgmDeathCheckAtPosition(sim, lgman, (*lgman)->x, (*lgman)->y, wx, wy, owner,
                          attacker, cause, pill, tnk);
}

/*********************************************************
*NAME:          lgmParchutingIn
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Man is parachuting back in.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
void lgmParchutingIn(GameSim *sim, lgm *lgman) {
  bool isServer = sim->isServer;
  TURNTYPE angle; /* Angle of travel */
  int xAdd;       /* Add amounts */
  int yAdd;


  angle = utilCalcAngle((*lgman)->x, (*lgman)->y, (*lgman)->destX, (*lgman)->destY);
  utilCalcDistance(&xAdd, &yAdd, angle, sim->rules.lgm_helicopter_speed);
  (*lgman)->x = (WORLD) ((*lgman)->x + xAdd);
  (*lgman)->y = (WORLD) ((*lgman)->y + yAdd);

  /* Check for achieved goal */
  /* The same tolerance the goal test above uses; this site spelled it out
     rather than reading the constant. */
  if (((*lgman)->x - (*lgman)->destX) >= -sim->rules.lgm_arrive_tolerance &&
      ((*lgman)->x - (*lgman)->destX) <= sim->rules.lgm_arrive_tolerance &&
      ((*lgman)->y - (*lgman)->destY) >= -sim->rules.lgm_arrive_tolerance &&
      ((*lgman)->y - (*lgman)->destY) <= sim->rules.lgm_arrive_tolerance) {
    /* Arrived at drop off spot. Begin trek back to tank */
    if (isServer == TRUE) {
      /* The square he actually reached, read here before anything moves him
         again. */
      if (sim->callbacks.lgmLanded) {
        sim->callbacks.lgmLanded(sim->callbacks.ctx, (*lgman)->playerNum,
                                 (BYTE)((*lgman)->x >> M_W_SHIFT_SIZE),
                                 (BYTE)((*lgman)->y >> M_W_SHIFT_SIZE));
      }
    }

    (*lgman)->isDead = FALSE;
    (*lgman)->state = LGM_STATE_RETURN;
    (*lgman)->frame = 0;
  }
}


/*********************************************************
*NAME:          lgmCheckRemove
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  An building operation has happened. Check to see if 
*  it should remove from grass/building data structures.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  terrain - Terrain type of the sqaure
*  mx      - Map X position 
*  my      - Map Y position
*********************************************************/
void lgmCheckRemove(GameSim *sim, BYTE terrain, BYTE mx, BYTE my) {
  switch (terrain) {
  case HALFBUILDING:
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
*NAME:          lgmCheckTankBoat
*AUTHOR:        John Morrison
*CREATION DATE: 19/1/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  This is to check the builder is close to the tank. If 
*  he is then it is considered "blessed" and can freely
*  travel to it.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  tnk  - Pointer to the tank structure
*  dist - Distance to check
*********************************************************/
bool lgmCheckTankBoat(lgm *lgman, tank *tnk, WORLD dist) {
  WORLD newmx;      /* Used for gap calculations */
  WORLD newmy;
  bool returnValue; /* Value to Return */

  returnValue = FALSE;
  tankGetWorld(tnk, &newmx, &newmy);
  if (tankIsOnBoat(tnk) == TRUE) {
    if ((newmx - (*lgman)->x) < 0) {
      newmx = (*lgman)->x - newmx;
    } else {
      newmx = newmx - (*lgman)->x;
    }
    if ((newmy - (*lgman)->y) < 0) {
      newmy = (*lgman)->y - newmy;
    } else {
      newmy = newmy - (*lgman)->y;
    }
    if (newmx < dist && newmy < dist) {
      returnValue = TRUE;
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          lgmPutWorld
*AUTHOR:        John Morrison
*CREATION DATE: 23/9/00
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm map X position
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  x      - World X Co-ordinate
*  y      - World Y Co-ordinate
*********************************************************/
void lgmPutWorld(lgm *lgmman, WORLD x, WORLD y, BYTE frame) {
  if ((*lgmman)->inTank == FALSE) {
    (*lgmman)->x = x;
    (*lgmman)->y = y;
    /* We don't set the frame from the server because its all handled locally */
  /*  (*lgmman)->frame = frame; */
/*    (*lgmman)->inTank = FALSE; */
    (*lgmman)->state = LGM_STATE_GOING;
  }
}

/*********************************************************
*NAME:          lgmGetMX
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm map X position
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
BYTE lgmGetMX(lgm *lgman) {
  BYTE returnValue; /* Value to return */
  WORLD conv;       /* Used in the conversion */

  if ((*lgman)->state == LGM_STATE_IDLE) {
    returnValue = 0;
  } else {
    if ((*lgman)->frame == LGM_HELICOPTER_FRAME) {
      conv = (*lgman)->x - MAP_SQUARE_MIDDLE;
    } else {
      conv = (*lgman)->x - 1;
    }
    conv >>= TANK_SHIFT_MAPSIZE;
    returnValue = (BYTE) conv;
  }
  return returnValue;
}

/*********************************************************
*NAME:          lgmGetMY
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm map Y position
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
BYTE lgmGetMY(lgm *lgman) {
  BYTE returnValue; /* Value to return */
  WORLD conv;       /* Used in the conversion */

  if ((*lgman)->state == LGM_STATE_IDLE) {
    returnValue = 0;
  } else {
    if ((*lgman)->frame == LGM_HELICOPTER_FRAME) {
      conv = (*lgman)->y - MAP_SQUARE_MIDDLE;
    } else {
      conv = (*lgman)->y - 2;
    }
    conv >>= TANK_SHIFT_MAPSIZE;
    returnValue = (BYTE) conv;
  }

  return returnValue;
}

/*********************************************************
*NAME:          lgmGetPX
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm pixel X position
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
BYTE lgmGetPX(lgm *lgman) {
  BYTE returnValue; /* Value to return */
  WORLD conv;       /* Used in the conversion */

  if ((*lgman)->state == LGM_STATE_IDLE) {
    returnValue = 0;
  } else {
    if ((*lgman)->frame == LGM_HELICOPTER_FRAME) {
      conv = (*lgman)->x - MAP_SQUARE_MIDDLE;
    } else {
      conv = (*lgman)->x - 1;
    }
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;
    returnValue = (BYTE) conv;

  }
  return returnValue;
}

/*********************************************************
*NAME:          lgmGetPY
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm pixel Y position
*
*ARGUMENTS:
*
*********************************************************/
BYTE lgmGetPY(lgm *lgman) {
  BYTE returnValue; /* Value to return */
  WORLD conv;       /* Used in the conversion */

  if ((*lgman)->state == LGM_STATE_IDLE) {
    returnValue = 0;
  } else {
    if ((*lgman)->frame == LGM_HELICOPTER_FRAME) {
      conv = (*lgman)->y - MAP_SQUARE_MIDDLE;
    } else {
      conv = (*lgman)->y - 2;
    }
    conv <<= TANK_SHIFT_MAPSIZE;
    conv >>= TANK_SHIFT_PIXELSIZE;
    returnValue = (BYTE) conv;
  }
  return returnValue;
}

/*********************************************************
*NAME:          lgmGetFrame
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns the lgm animation frame
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
BYTE lgmGetFrame(lgm *lgman) {
  return (*lgman)->frame;
}

bool lgmIsOut(lgm *lgman) {
  return !((*lgman)->inTank); 
}

bool lgmIsIdle(lgm *lgman) {
  return (*lgman)->action == LGM_IDLE;
}

/*********************************************************
*NAME:          lgmGetStatus
*AUTHOR:        John Morrison
*CREATION DATE: 14/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Gets the man status for drawing on the status bars
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  tnk    - Pointer to this LGM's tank.
*  isOut  - TRUE if man is out of tank
*  isDead - TRUE if man is dead
*  angle  - Angle man is travelling on
*********************************************************/
void lgmGetStatus(lgm *lgman, tank *tnk, bool *isOut, bool *isDead, TURNTYPE *angle) {
  WORLD wx; /* Tanks world co-ordinates */
  WORLD wy;

  *isOut = TRUE;
  *isDead = FALSE;
  if ((*lgman)->inTank == TRUE) {
    *isOut = FALSE;
  } else if ((*lgman)->isDead == TRUE) {
    *isDead = TRUE;
  } else if ((*lgman)->state == LGM_STATE_GOING) {
    tankGetWorld(tnk, &wx, &wy);
    *angle = utilCalcAngle((*lgman)->x, (*lgman)->y, wx, wy);
  } else {
    /* Returning to tank */
     tankGetWorld(tnk, &wx, &wy);
     *angle = utilCalcAngle((*lgman)->x, (*lgman)->y, wx, wy);
  }
}

/*********************************************************
*NAME:          lgmGetWX
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm World X co-ord
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
WORLD lgmGetWX(lgm *lgman) {
  return (*lgman)->x;
}

/*********************************************************
*NAME:          lgmGetWY
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm World Y co-ord
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
WORLD lgmGetWY(lgm *lgman) {
  return (*lgman)->y;
}

/*********************************************************
*NAME:          lgmGetDir
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm direction
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  tnk - Pointer to the tank object
*********************************************************/
BYTE lgmGetDir(lgm *lgman, tank *tnk) {
  TURNTYPE angle;
  if ((*lgman)->state == LGM_STATE_GOING) {
    angle = utilCalcAngle((*lgman)->x, (*lgman)->y, (*lgman)->destX, (*lgman)->destY);
  } else {
    angle = utilCalcAngle((*lgman)->x, (*lgman)->y, tankGetMX(tnk), tankGetMY(tnk));
  }
  return utilGetDir(angle);
}

/*********************************************************
*NAME:          lgmGetBrainState
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm's state required for brain.
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
BYTE lgmGetBrainState(lgm *lgman) {
  BYTE returnValue; /* Value to return */

  returnValue = LGM_BRAIN_MOVING;
  if ((*lgman)->isDead == TRUE) {
    returnValue = LGM_BRAIN_DEAD;
  } else if ((*lgman)->inTank == TRUE) {
    returnValue = LGM_BRAIN_INTANK;
  }
  return returnValue;
}

/*********************************************************
*NAME:          lgmGetBrainObstructed
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
*  Returns lgm's obstructed state required for brain
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*********************************************************/
BYTE lgmGetBrainObstructed(lgm *lgman) {
  return (*lgman)->obstructed;
}

/*********************************************************
*NAME:          lgmSetBrainObstructed
*AUTHOR:        John Morrison
*CREATION DATE: 16/4/01
*LAST MODIFIED: 16/4/01
*PURPOSE:
*  Stores lgm's obstructed state required for brain
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  obstructed - The obstructed state to set
*********************************************************/
void lgmSetBrainObstructed(lgm *lgman, BYTE obstructed) {
  (*lgman)->obstructed = obstructed;
}

/*********************************************************
*NAME:          lgmNetBackInTank
*AUTHOR:        John Morrison
*CREATION DATE: 2/12/00
*LAST MODIFIED: 2/12/00
*PURPOSE:
*  Network message. LGM Back in tank
*
*ARGUMENTS:
*  lgman    - Pointer to the lgm sturcture
*  mp       - Pointer to the map structure
*  pb       - Pointer to the pillbox structure
*  bs       - Pointer to the base structure
*  tnk      - Pointer to the tank structure
*  numTrees - Amount of trees lgm is carrying
*  numMines - Amount of mines the lgm is carrying
*  pillNum  - Pillbox being carries
*********************************************************/
void lgmNetBackInTank(GameSim *sim, lgm *lgman, tank *tnk, BYTE numTrees, BYTE numMines, BYTE pillNum) {
  bool isServer = sim->isServer;
  (*lgman)->isDead = FALSE;
  (*lgman)->state = LGM_STATE_IDLE;
  (*lgman)->inTank = TRUE;
  (*lgman)->action = LGM_IDLE;
  (*lgman)->blessX = 0;
  (*lgman)->blessY = 0;
  (*lgman)->onTop = 0;
  (*lgman)->numPills = pillNum;
  (*lgman)->numTrees = numTrees;
  (*lgman)->numMines = numMines;
  lgmBackInTank(sim, lgman, tnk, TRUE);

  if (isServer == FALSE) {
    frontEndManClear(clientSimFromSim(sim));
  }
}

/*********************************************************
*NAME:          lgmNetManWorking
*AUTHOR:        John Morrison
*CREATION DATE: 01/02/03
*LAST MODIFIED: 01/02/03
*PURPOSE:
*  Network message. LGM out doing work 
*
*ARGUMENTS:
*  lgman    - Pointer to the lgm sturcture
*  tnk      - Pointer to the tank structure
*  mapX     - Bless X map position 
*  mapY     - Bless Y map position 
*  numTrees - Amount of trees lgm is carrying
*  numMines - Amount of mines the lgm is carrying
*  pillNum  - Pillbox being carries
*********************************************************/
void lgmNetManWorking(GameSim *sim, lgm *lgman, tank *tnk, BYTE mapX, BYTE mapY, BYTE numTrees, BYTE numMines, BYTE pillNum) {
  (*lgman)->isDead = FALSE;
  (*lgman)->state = LGM_STATE_GOING;
  (*lgman)->inTank = FALSE;
  (*lgman)->blessX = mapX;
  (*lgman)->blessY = mapY;
  (*lgman)->numPills = pillNum;
  (*lgman)->numTrees = numTrees;
  (*lgman)->numMines = numMines;
  /* Update the statuses */
  if (numTrees > 0) {
    tankGetLgmTrees(sim, tnk, numTrees, TRUE);
  }
  if (numMines > 0) {
    tankGetLgmMines(sim, tnk, numMines, TRUE);
  }
  if (pillNum != LGM_NO_PILL) {
    tankGetCarriedPillNum(tnk, pillNum);
  }
  tankGetWorld(tnk, &(*lgman)->x, &(*lgman)->y);   
}

/*********************************************************
*NAME:          lgmSetPlayerNum
*AUTHOR:        John Morrison
*CREATION DATE: 3/12/00
*LAST MODIFIED: 3/12/00
*PURPOSE:
*  Sets the player number
*
*ARGUMENTS:
*  lgman     - Pointer to the lgm sturcture
*  playerNum - Player number to set to
*********************************************************/
void lgmSetPlayerNum(lgm *lgman, BYTE playerNum) {
  if ((*lgman) != NULL) {
    (*lgman)->playerNum = playerNum;
  }
}

/*********************************************************
*NAME:          lgmSetIsDead
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/00
*LAST MODIFIED: 28/12/00
*PURPOSE:
*  Sets if the lgm is dead of not
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  isDead - Flag whether the LGM is dead or not
*********************************************************/
void lgmSetIsDead(GameSim *sim, lgm *lgman, bool isDead) {
  bool isServer = sim->isServer;
  (*lgman)->isDead = isDead;
  (*lgman)->nextAction = LGM_IDLE;
  if (isDead == TRUE) {
    (*lgman)->x = 0;
    (*lgman)->y = 0;
    if (isServer == FALSE) {
      frontEndManStatus(clientSimFromSim(sim), TRUE, 0.0f);
    }
  } else {
    if (isServer == FALSE) {
      frontEndManClear(clientSimFromSim(sim));
    }
  }
}

/*********************************************************
*NAME:          lgmConnectionLost
*AUTHOR:        John Morrison
*CREATION DATE: 24/02/03
*LAST MODIFIED: 24/02/03
*PURPOSE:
* Called if our connection is lost from the server 
*
*ARGUMENTS:
*  lgman  - Pointer to the lgm sturcture
*  tnk    - Pointer to our tank structure
*********************************************************/
void lgmConnectionLost(GameSim *sim, lgm *lgman, tank *tnk, starts *sts) {
  BYTE lgmX;  /* Used to get start position */
  BYTE lgmY;
  TURNTYPE dummy;
  if ((*lgman)->isDead == TRUE) {
    tankGetWorld(tnk, &((*lgman)->destX), &((*lgman)->destY));
    if ((*lgman)->x == 0 || (*lgman)->y == 0) {
      startsGetRandStart(sim, sts, &lgmX, &lgmY, &dummy);
      (*lgman)->x = (lgmX << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
      (*lgman)->y = (lgmY << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE;
    }
  } else if ((*lgman)->state != LGM_STATE_IDLE) {
    (*lgman)->state = LGM_STATE_RETURN;
  }

}

