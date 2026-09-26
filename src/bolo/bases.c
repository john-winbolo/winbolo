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
*Name:          Bases 
*Filename:      bases.c 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 24/07/04
*Purpose:
*  Provides operations on bases 
*********************************************************/

/* Includes */
#include <math.h>
#include <memory.h>
#include "global.h"
#include "tank.h"
#include "frontend.h"
#include "../gui/lang.h"
#include "messages.h"
#include "players.h"
#include "brain_data.h"
#include "log.h"
#include "../winbolonet/winbolonet_core.h"
#include "bases.h"
#include "bolo_map.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "server_sim.h"

void basesUpdateTimer(GameSim *sim, int playerNumber){
	sim->baseTimer[playerNumber]=sim->rules.base_regen_ticks;
}


void basesRemoveTimer(GameSim *sim, int playerNumber){
	sim->baseTimer[playerNumber]=BASE_TIMER_OFF;
}
/*********************************************************
*NAME:         basesCreate 
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the bases structure.
*  Sets number of bases to zero
*
*ARGUMENTS:
*  value - Pointer to the bases structure 
*********************************************************/
void basesCreate(bases *value) {
  BYTE count; /* Looping variable */

  New(*value);
  memset(*value, 0, sizeof(**value));
  (*value)->numBases = 0;

  for (count=0;count<MAX_BASES;count++) {
    (*value)->item[count].baseTime = 0;
    (*value)->item[count].justStopped = TRUE;
  }
}

/*********************************************************
*NAME:          basesDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the bases data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
void basesDestroy(bases *value) {
  if (*value != NULL) {
    Dispose(*value);
  }
}

/*********************************************************
*NAME:          basesSetNumBases
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of bases in the structure 
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  numBases - The number of bases  
*********************************************************/
void basesSetNumBases(bases *value, BYTE numBases) {
  BYTE count; /* Looping variable */

  if (numBases <= MAX_BASES) {
    (*value)->numBases = numBases;
    /* Every base a map brings in is live. Removal happens after the list is
       loaded, so the count and the live set agree here. */
    for (count = 0; count < numBases; count++) {
      (*value)->active[count] = TRUE;
    }
  }
}


/*********************************************************
*NAME:          basesGetNumBases 
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of bases in the structure
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*********************************************************/
BYTE basesGetNumBases(bases *value) {
  if ((*value) != NULL) {
    return (*value)->numBases;
  }
  return 0;
}

/*********************************************************
*NAME:          basesSetBase
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED:  29/4/00
*PURPOSE:
*  Sets a specific base with its item data
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void basesSetBase(bases *value, base *item, BYTE baseNum) {
  if (baseNum > 0 && baseNum <= (*value)->numBases) {
    baseNum--;
    (((*value)->item[baseNum]).x) = item->x;
    (((*value)->item[baseNum]).y) = item->y;
    if (item->owner > (MAX_TANKS-1) && item->owner != NEUTRAL) {
      item->owner = NEUTRAL;
    }
    logAddEvent(log_BaseSetOwner, baseNum, item->owner, TRUE, 0, 0, NULL);
    (((*value)->item[baseNum]).owner) = item->owner;
    /* The stocks are stored as handed over. What a base may hold is a
     * gameplay cap, and this runs on the file-load path, where there is
     * no sim to ask — basesClampToRules caps the list once one owns it.
     * Nothing here needs the cap for safety: drain math is straightforward
     * subtraction with no wrap, so an out-of-range stock simply takes
     * longer to deplete than any legitimate one could. pillsSetPill beside
     * it does clamp, because the scenario arms that write a pill's armour
     * and its speed are written against that clamp; no arm writes a base
     * through here. */
    (((*value)->item[baseNum]).armour) = item->armour;
    (((*value)->item[baseNum]).shells) = item->shells;
    (((*value)->item[baseNum]).mines) = item->mines;
    (*value)->item[baseNum].refuelTime = 0;
    (*value)->item[baseNum].baseTime = item->baseTime;
    (*value)->item[baseNum].justStopped = TRUE;
    logAddEvent(log_BaseSetStock, baseNum, item->shells, item->mines, item->armour, 0, NULL);
  }
}

/*********************************************************
*NAME:          basesGetBase
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/98
*LAST MODIFIED: 9/2/98
*PURPOSE:
*  Gets a specific base
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void basesGetBase(bases *value, base *item, BYTE baseNum) {
  if (baseNum > 0 && baseNum <= (*value)->numBases) {
    baseNum--;
    item->x = ((*value)->item[baseNum]).x;
    item->y = ((*value)->item[baseNum]).y;
    item->owner = ((*value)->item[baseNum]).owner;
    item->armour = ((*value)->item[baseNum]).armour;
    item->shells = ((*value)->item[baseNum]).shells;
    item->mines = ((*value)->item[baseNum]).mines;
  }
}

/*********************************************************
*NAME:          basesAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Puts a base into the list and returns its number in
*  outBaseNum. The lowest removed slot is reused; when
*  every slot in the count is live the list is extended and
*  the count raised. Returns FALSE with outBaseNum
*  untouched when all MAX_BASES bases are live.
*
*ARGUMENTS:
*  value      - Pointer to the bases structure
*  item       - The base to store
*  outBaseNum - Receives the base number, 1 based
*********************************************************/
bool basesAddItem(bases *value, const base *item, BYTE *outBaseNum) {
  BYTE count; /* Looping variable */

  for (count = 0; count < (*value)->numBases; count++) {
    if ((*value)->active[count] == FALSE) {
      (*value)->item[count] = *item;
      (*value)->active[count] = TRUE;
      *outBaseNum = (BYTE) (count + 1);
      return TRUE;
    }
  }
  if ((*value)->numBases >= MAX_BASES) {
    return FALSE;
  }
  count = (*value)->numBases;
  (*value)->item[count] = *item;
  (*value)->active[count] = TRUE;
  (*value)->numBases++;
  *outBaseNum = (BYTE) (count + 1);
  return TRUE;
}

/*********************************************************
*NAME:          basesInstallItem
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Writes a base at the number it is given and marks that
*  slot live, whatever the slot held before. A number past
*  the count raises the count to cover it and leaves every
*  slot the gap opens up removed: a number arrives from a
*  list that has already filled it, so the gap is the set of
*  bases this list has not been told about. Returns FALSE
*  for number 0 or a number past MAX_BASES.
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - The base to store
*  baseNum - The base number, 1 based
*********************************************************/
bool basesInstallItem(bases *value, const base *item, BYTE baseNum) {
  BYTE slot;  /* The array index the number names */
  BYTE count; /* Looping variable */

  if (baseNum == 0 || baseNum > MAX_BASES) {
    return FALSE;
  }
  slot = (BYTE) (baseNum - 1);
  for (count = (*value)->numBases; count < slot; count++) {
    (*value)->active[count] = FALSE;
  }
  if (baseNum > (*value)->numBases) {
    (*value)->numBases = baseNum;
  }
  (*value)->item[slot] = *item;
  (*value)->active[slot] = TRUE;
  return TRUE;
}

/*********************************************************
*NAME:          basesRemoveItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Clears a base's live flag. The slot, the count and every
*  base number above it are left alone, so the numbers the
*  wire and the recordings use keep meaning the same base.
*  Returns FALSE for a number out of range or one already
*  removed.
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number, 1 based
*********************************************************/
bool basesRemoveItem(bases *value, BYTE baseNum) {
  if (baseNum == 0 || baseNum > (*value)->numBases) {
    return FALSE;
  }
  baseNum--;
  if ((*value)->active[baseNum] == FALSE) {
    return FALSE;
  }
  (*value)->active[baseNum] = FALSE;
  return TRUE;
}

/*********************************************************
*NAME:          basesIsActive
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Returns whether a base number names a base that is on the
*  map. A removed base keeps its slot and its number, so a
*  number in range is not on its own enough. A number out of
*  range returns FALSE.
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number, 1 based
*********************************************************/
bool basesIsActive(bases *value, BYTE baseNum) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (baseNum == 0 || baseNum > (*value)->numBases) {
    return FALSE;
  }
  return ((*value)->active[baseNum - 1] != FALSE);
}

/*********************************************************
*NAME:          basesSetActive
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Puts a base on the map or takes it off it, leaving its
*  record alone either way. The flag is all that moves, so a
*  base put back is the one the slot already held. Returns
*  FALSE for a number out of range.
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number, 1 based
*  onMap   - TRUE for on the map, FALSE for off it
*********************************************************/
bool basesSetActive(bases *value, BYTE baseNum, bool onMap) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (baseNum == 0 || baseNum > (*value)->numBases) {
    return FALSE;
  }
  (*value)->active[baseNum - 1] = onMap ? TRUE : FALSE;
  return TRUE;
}

/*********************************************************
*NAME:          basesExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a base exist at a specific location
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool basesExistPos(bases *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  returnValue = FALSE;
  count = 0;
  while (returnValue == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      returnValue = TRUE;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesGetAlliancePos
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 21/12/98
*PURPOSE:
*  Returns the type of a base (good, neutral, evil) 
*  depending on the map position. Returns base neutral if
*  base not found.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
baseAlliance basesGetAlliancePos(GameSim *sim, BYTE xValue, BYTE yValue, BYTE viewPlayer) {
  bases *value = &sim->bs;
  baseAlliance returnValue; /* Value to return */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = baseNeutral;
  count = 0;
  done = FALSE;
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
     if ((*value)->item[count].armour <= sim->rules.base_capture_armour) {
        returnValue = baseDead;
      } else if ((*value)->item[count].owner == NEUTRAL) {
        returnValue = baseNeutral;
      } else if ((*value)->item[count].owner == viewPlayer) {
        returnValue = baseOwnGood;
      } else if (playersIsAllie(&sim->plyrs, (*value)->item[count].owner, viewPlayer) == TRUE) {
        returnValue = baseAllieGood;
      } else {
        returnValue = baseEvil;
      }
      done = TRUE;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesGetStatusNum
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 11/1/99
*PURPOSE:
*  Returns the type of a base (good, neutral, dead, evil) 
*  depending on the baseNum. Returns base neutral if
*  base not found.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
baseAlliance basesGetStatusNum(GameSim *sim, BYTE baseNum) {
  bases *value = &sim->bs;
  baseAlliance returnValue; /* Value to return */

  baseNum--;
  returnValue = baseNeutral;
  /* A base off the map has no status to draw; it reads as neutral, which is
     what the panel shows for a slot the map does not use. */
  if (baseNum < ((*value)->numBases) && (*value)->active[baseNum] != FALSE) {
    if ((*value)->item[baseNum].armour <= sim->rules.base_capture_armour) {
      returnValue = baseDead;
    } else if ((*value)->item[baseNum].owner == NEUTRAL) {
      returnValue = baseNeutral;
    } else if ((*value)->item[baseNum].owner == sim->viewPlayer) {
      returnValue = baseOwnGood;
    } else if (playersIsAllie(&sim->plyrs, (*value)->item[baseNum].owner, sim->viewPlayer) == TRUE) {
      returnValue = baseAllieGood;
    } else {
      returnValue = baseEvil;
    }
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 4/1/00
*PURPOSE:
*  Called once every Bolo tick to update stocks, give
*  stuff to tanks etc.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  tnk    - Pointer to the tank structure
*********************************************************/
void basesUpdate(GameSim *sim, tank *tnk) {
  bases *value = &sim->bs;
  bool isServer = sim->isServer;
  WORLD twx;               /* Tank World Co-ordinates */
  WORLD twy;
  BYTE tx;                 /* Tank Map X and Y Co-ordinates */
  BYTE ty;
  BYTE baseNum;            /* The base number if the tank is on a base */
  BYTE count;              /* Looping Variable */
  int secondCounter;       /* another looping variable */

  count = 0;
  secondCounter = 0;
  /* Old Algorithm */
/*  maxTime = (800.0 / numPlayers); // maxTime was defined as a double
  while (count < (*value)->numBases) {
    (*value)->item[count].baseTime++;
    if ((*value)->item[count].baseTime >= maxTime) {
      if (isServer == TRUE) {
        basesUpdateStock(value, (BYTE) (count+1));
      }
      (*value)->item[count].baseTime = 0;
    }
    if ((*value)->item[count].refuelTime > 0) {
      (*value)->item[count].refuelTime--;
    }
    count++;
  }
  */

  while (secondCounter < MAX_TANKS)
  {
	  if(sim->baseTimer[secondCounter] != BASE_TIMER_OFF)
	  {
		  sim->baseTimer[secondCounter]--;
		  if(sim->baseTimer[secondCounter]<=0)
		  {
			if (isServer == TRUE)
			{
				count = 0;
				while (count < (*value)->numBases)
				{
					basesUpdateStock(sim, (BYTE) (count+1));
					count++;
				}
			}
			sim->baseTimer[secondCounter]=sim->rules.base_regen_ticks;
		  }
	  }

	secondCounter++;
  }

  count = 0;

  while (count < (*value)->numBases)
  {
	  if ((*value)->active[count] != FALSE && (*value)->item[count].refuelTime > 0) {
	      (*value)->item[count].refuelTime--;
	  }
	  count++;
  }

  if (isServer == FALSE && tnk != NULL) {
    /* Client - Check for refuelling */
    /* Get tanks location */
    tankGetWorld(tnk, &twx, &twy);
    twx >>= TANK_SHIFT_MAPSIZE;
    tx = (BYTE) twx;
    twy >>= TANK_SHIFT_MAPSIZE;
    ty = (BYTE) twy;
    baseNum = basesGetBaseNum(value,tx,ty);
    if (baseNum != BASE_NOT_FOUND && !tankIsDestroyed(tnk)) {
      /* On base */
      if ((*value)->item[baseNum-1].justStopped == FALSE) {
        basesRefueling(sim, tnk, baseNum);
      } else {
        (*value)->item[baseNum-1].justStopped = FALSE;
        (*value)->item[baseNum-1].refuelTime = basesHalfTickCalulator(sim, BASES_HALFTICK_TYPE_ARMOUR);
      }
    }
  } else if (tnk != NULL) {
    baseNum = BASE_NOT_FOUND;
  } else {
    /* No tank passed (server sim) — skip justStopped management;
     * the server-side refueling loop handles it. */
    return;
  }

  count = 0;

  /* Set them back to empty  */
  while (count < (*value)->numBases) {
    if (count != (baseNum-1)) {
      (*value)->item[count].justStopped = TRUE;
    }
    count++;
  }

}

/* Steal-message debounce. The newswire message is generic — "X stole base
 * from Y" with no base reference — so collapsing by (newOwner, prevOwner)
 * pair is functionally equivalent to per-base for what the player sees.
 * Cooldown = window after an emit during which further pair-matching
 * steals are suppressed. Quiet = no-new-activity period that triggers a
 * trailing flush of the latest suppressed steal. */
/* Counters advance from clientSimDisplayTick (game tick, 50 Hz). */
#define BASE_STEAL_COOLDOWN_TICKS 150  /* 3s */
#define BASE_STEAL_QUIET_TICKS    100  /* 2s */

static void basesEmitCaptureMessage(GameSim *sim, struct ClientSim *cs,
                                    BYTE newOwner, BYTE prevOwner) {
  MessageArgs args;
  BYTE selfPlayer = (cs != NULL) ? clientSimGetMyPlayerNum(cs) : sim->viewPlayer;
  memset(&args, 0, sizeof(args));
  playersMakeMessageName(cs, &sim->plyrs, selfPlayer, newOwner, args.playerName);
  args.playerFlags = playersGetAccountFlags(&sim->plyrs, newOwner);
  playersGetCountryCode(&sim->plyrs, newOwner, args.playerCountry);
  if (prevOwner != NEUTRAL) {
    playersGetPlayerName(&sim->plyrs, prevOwner, args.otherName,
                         sizeof(args.otherName), sim->isServer);
    args.otherFlags = playersGetAccountFlags(&sim->plyrs, prevOwner);
    playersGetCountryCode(&sim->plyrs, prevOwner, args.otherCountry);
    sim->callbacks.messageAdd(sim->callbacks.ctx, newsWireMessage,
                              MESSAGE_NEWSWIRE, MESSAGE_STOLE_BASE, &args);
  } else {
    sim->callbacks.messageAdd(sim->callbacks.ctx, newsWireMessage,
                              MESSAGE_NEWSWIRE, MESSAGE_CAPTURE_BASE, &args);
  }
}

/* Returns the slot for this pair, allocating one if absent. When the table
 * is full the slot with the highest lastEmitTicks (least-recently emitted)
 * is evicted. Newly allocated slots start with lastEmitTicks at the
 * cooldown threshold so the very first steal of a pair always emits. */
static baseStealDebounceSlot *basesFindOrAllocStealSlot(bases *value,
                                                       BYTE newOwner,
                                                       BYTE prevOwner) {
  baseStealDebounceSlot *table = (*value)->stealDebounce;
  baseStealDebounceSlot *firstFree = NULL;
  baseStealDebounceSlot *oldest = &table[0];
  int i;

  for (i = 0; i < BASE_STEAL_TABLE_SIZE; i++) {
    if (table[i].active && table[i].newOwner == newOwner
        && table[i].prevOwner == prevOwner) {
      return &table[i];
    }
    if (!table[i].active && firstFree == NULL) {
      firstFree = &table[i];
    }
    if (table[i].lastEmitTicks > oldest->lastEmitTicks) {
      oldest = &table[i];
    }
  }

  baseStealDebounceSlot *slot = (firstFree != NULL) ? firstFree : oldest;
  slot->active = TRUE;
  slot->newOwner = newOwner;
  slot->prevOwner = prevOwner;
  slot->pendingActive = FALSE;
  slot->lastEmitTicks = BASE_STEAL_COOLDOWN_TICKS;
  slot->pendingAge = 0;
  return slot;
}

void basesEnqueueCaptureMessage(GameSim *sim, struct ClientSim *cs,
                                BYTE newOwner, BYTE prevOwner) {
  if (prevOwner == NEUTRAL) {
    basesEmitCaptureMessage(sim, cs, newOwner, prevOwner);
    return;
  }

  /* Allied steals are silent — mirrors the pillbox alliance gate. */
  if (playersIsAllie(&sim->plyrs, newOwner, prevOwner) == TRUE) {
    return;
  }

  bases *value = &sim->bs;
  baseStealDebounceSlot *slot = basesFindOrAllocStealSlot(value, newOwner,
                                                          prevOwner);

  if (slot->lastEmitTicks >= BASE_STEAL_COOLDOWN_TICKS) {
    basesEmitCaptureMessage(sim, cs, newOwner, prevOwner);
    slot->lastEmitTicks = 0;
    slot->pendingActive = FALSE;
  } else {
    slot->pendingActive = TRUE;
    slot->pendingAge = 0;
  }
}

void basesTickMessageQueue(GameSim *sim, struct ClientSim *cs) {
  bases *value = &sim->bs;
  baseStealDebounceSlot *table = (*value)->stealDebounce;
  int i;

  for (i = 0; i < BASE_STEAL_TABLE_SIZE; i++) {
    baseStealDebounceSlot *slot = &table[i];
    if (!slot->active) continue;

    if (slot->lastEmitTicks < 0xFFFE) slot->lastEmitTicks++;

    if (slot->pendingActive) {
      slot->pendingAge++;
      if (slot->pendingAge >= BASE_STEAL_QUIET_TICKS) {
        basesEmitCaptureMessage(sim, cs, slot->newOwner, slot->prevOwner);
        slot->lastEmitTicks = 0;
        slot->pendingActive = FALSE;
      }
    } else if (slot->lastEmitTicks > BASE_STEAL_COOLDOWN_TICKS * 4) {
      /* Long idle — recycle the slot. */
      slot->active = FALSE;
    }
  }
}

/*********************************************************
*NAME:          basesUpdateStock
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 19/2/99
*PURPOSE:
*  Called when a stock update is needed
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base to update
*********************************************************/
void basesUpdateStock(GameSim *sim, BYTE baseNum) {
  bases *value = &sim->bs;
  bool isServer = sim->isServer;
  BYTE oldArmour;          /* Used for checking base armour for updates */
  BYTE addAmount;

  baseNum--;
  addAmount = 1; /* (BYTE) playersGetNumPlayers(); - 1.09 was but halved rechar time (2*playersGetNumPlayers()); * FIXME: Constant rate */
  if (baseNum < (*value)->numBases && (*value)->active[baseNum] != FALSE) {
    if ((*value)->item[baseNum].armour < sim->rules.base_full_armour) {
      oldArmour = (*value)->item[baseNum].armour;
      (*value)->item[baseNum].armour += addAmount;
      if ((*value)->item[baseNum].armour > sim->rules.base_full_armour) {
        (*value)->item[baseNum].armour = (BYTE) sim->rules.base_full_armour;
      }
      /* Update the frontend status as required: this tick is where the base
         came back off the capture threshold, so its icon changes. */
      if (oldArmour == sim->rules.base_capture_armour &&
          (*value)->item[baseNum].armour > sim->rules.base_capture_armour) {
        if (isServer == FALSE) {
          frontEndStatusBase(clientSimFromSim(sim), (BYTE) (baseNum+1), (basesGetStatusNum(sim, (BYTE) (baseNum+1))));
        }
      }
    }
    if ((*value)->item[baseNum].shells < sim->rules.base_full_shells) {
      (*value)->item[baseNum].shells += addAmount;
      if ((*value)->item[baseNum].shells > sim->rules.base_full_shells) {
        (*value)->item[baseNum].shells = (BYTE) sim->rules.base_full_shells;
      }
    }

    if ((*value)->item[baseNum].mines < sim->rules.base_full_mines) {
      (*value)->item[baseNum].mines += addAmount;
      if ((*value)->item[baseNum].mines > sim->rules.base_full_mines) {
        (*value)->item[baseNum].mines = (BYTE) sim->rules.base_full_mines;
      }
    }
    logAddEvent(log_BaseSetStock, baseNum, (*value)->item[baseNum].shells, (*value)->item[baseNum].mines, (*value)->item[baseNum].armour, 0, NULL);
  }
}

/*********************************************************
*NAME:          basesAmOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 31/10/99
*PURPOSE:
*  Returns whether the bases at the particular location
*  is owned by the player
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  owner  - Owner to check with
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool basesAmOwner(GameSim *sim, BYTE owner, BYTE xValue, BYTE yValue) {
  bases *value = &sim->bs;
  bool returnValue;         /* Value to return */
  BYTE self;                /* Our player number */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = FALSE;
  count = 0;
  done = FALSE;
  /* FIXME: This is redundent. */
  self = owner;
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      if ((*value)->item[count].owner == self || (playersIsAllie(&sim->plyrs, (*value)->item[count].owner, self) == TRUE)) {
        returnValue = TRUE;
      }
      done = TRUE;
    }
    count++;
  }

  return returnValue;
}


/*********************************************************
*NAME:          basesSetBaseOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/01/99
*LAST MODIFIED: 04/04/02
*PURPOSE:
* Sets the base to be owned by paremeter passed.
* Returns the previous owner. A base taken off another
* player is "stolen" and loses everything it was holding,
* unless keepStock says to hand it over as it stands.
* If migrate is set to TRUE then it has migrated from a
* alliance when a player left and we shouldn't make a
* message.
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  baseNum   - Base number to set
*  owner     - Who owns it
*  migrate   - TRUE if it has migrated from an alliance
*  keepStock - TRUE to leave the base's stock alone
*********************************************************/
BYTE basesSetBaseOwner(GameSim *sim, BYTE baseNum, BYTE owner, BYTE migrate, BYTE keepStock) {
  bases *value = &sim->bs;
  bool isServer = sim->isServer;
  BYTE returnValue;         /* Value to return */

  returnValue = FALSE;
  if (baseNum > 0 && baseNum <= (*value)->numBases &&
      (*value)->active[baseNum - 1] != FALSE) {
    baseNum--;
    returnValue = (*value)->item[baseNum].owner;
    /* Taking a base off another player empties it. Neutralising one, or
       handing it to the player who already holds it, takes nothing; nor does
       a caller that asked to keep the stock. */
    if (keepStock == FALSE && owner != NEUTRAL && returnValue != NEUTRAL &&
        returnValue != owner) {
      (*value)->item[baseNum].armour = 0;
      (*value)->item[baseNum].shells = 0;
      (*value)->item[baseNum].mines = 0;
      (*value)->item[baseNum].baseTime = 0;
      logAddEvent(log_BaseSetStock, baseNum, (*value)->item[baseNum].shells, (*value)->item[baseNum].mines, (*value)->item[baseNum].armour, 0, NULL);
    }
    (*value)->item[baseNum].owner = owner;
    logAddEvent(log_BaseSetOwner, baseNum, owner, migrate, 0, 0, NULL);

    /* Report the change, which is what networked clients get the message from.
       A base going neutral is reported the same way, with owner as the new
       owner; the client draws no newswire line for that one. */
    if (migrate == FALSE && sim->isServer) {
      if (sim->callbacks.baseOwnerChanged) {
        BYTE captureClass;
        captureClass = (returnValue == NEUTRAL)                                ? CAPTURE_CLASS_NEUTRAL
                     : (playersIsAllie(&sim->plyrs, owner, returnValue) == FALSE) ? CAPTURE_CLASS_ENEMY
                     :                                                           CAPTURE_CLASS_ALLY;
        sim->callbacks.baseOwnerChanged(sim->callbacks.ctx, baseNum,
                                        returnValue, owner, captureClass,
                                        (*value)->item[baseNum].x,
                                        (*value)->item[baseNum].y);
      }
    }

    /* WinBolo.net Stuff */
    if (migrate == FALSE && owner != NEUTRAL) {
      winbolonetAddEvent(WINBOLO_NET_EVENT_BASE_CAPTURE, isServer, owner, WINBOLO_NET_NO_PLAYER,
                         playersIsBot(&sim->plyrs, owner), FALSE);
    }

  }

  return returnValue;
}

/*********************************************************
*NAME:          basesSetOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/01/99
*LAST MODIFIED: 04/04/02
*PURPOSE:
* Sets the base to be owned by paremeter passed.
* Returns the previous owner. If it was not neutral we
* assume then it was "stolen" and subsequently remove
* all its possessions. If migrate is set to TRUE then
* it has migrated from a alliance when a player left 
* and we shouldn't make a message
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  xValue  - X Location
*  yValue  - Y Location
*  owner   - Who owns it
*  migrate - TRUE if it has migrated from an alliance
*********************************************************/
BYTE basesSetOwner(GameSim *sim, BYTE xValue, BYTE yValue, BYTE owner, BYTE migrate) {
  bases *value = &sim->bs;
  bool isServer = sim->isServer;
  BYTE returnValue;         /* Value to return */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = FALSE;
  count = 0;
  done = FALSE;
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      returnValue = (*value)->item[count].owner;
      if (migrate == TRUE) {
        (*value)->item[count].owner = owner;
        done = TRUE;
      } else if (owner == NEUTRAL) {
        /* Nothing is reported: the callback below is for a base being taken
           and there is no taker here. basesSetBaseOwner is the entry that
           does report a base going neutral. */
        (*value)->item[count].owner = owner;
        done = TRUE;
      } else if ((*value)->item[count].owner != owner) {
        if (returnValue != NEUTRAL) {
          (*value)->item[count].armour = 0;
          (*value)->item[count].shells = 0;
          (*value)->item[count].mines = 0;
          (*value)->item[count].baseTime = 0;
          logAddEvent(log_BaseSetStock, count, (*value)->item[count].shells, (*value)->item[count].mines, (*value)->item[count].armour, 0, NULL);
        }
        (*value)->item[count].owner = owner;
        logAddEvent(log_BaseSetOwner, count, owner, migrate, 0, 0, NULL);
        /* Report the change, which is what clients get the message from. A
           base taken from nobody is reported the same way as one stolen from
           a player; the class beside it is what tells the two apart. */
        if (migrate == FALSE && sim->isServer) {
          if (sim->callbacks.baseOwnerChanged) {
            BYTE captureClass;
            captureClass = (returnValue == NEUTRAL)                                ? CAPTURE_CLASS_NEUTRAL
                         : (playersIsAllie(&sim->plyrs, owner, returnValue) == FALSE) ? CAPTURE_CLASS_ENEMY
                         :                                                           CAPTURE_CLASS_ALLY;
            sim->callbacks.baseOwnerChanged(sim->callbacks.ctx, count,
                                            returnValue, owner, captureClass,
                                            (*value)->item[count].x,
                                            (*value)->item[count].y);
          }
        }
        done = TRUE;
        /* WinBolo.net Stuff */
        if (migrate == FALSE && owner != NEUTRAL) {
          if (returnValue == NEUTRAL) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_BASE_CAPTURE, isServer, owner, WINBOLO_NET_NO_PLAYER,
                               playersIsBot(&sim->plyrs, owner), FALSE);
          } else {
            winbolonetAddEvent(WINBOLO_NET_EVENT_BASE_STEAL, isServer, owner, returnValue,
                               playersIsBot(&sim->plyrs, owner), playersIsBot(&sim->plyrs, returnValue));
          }
        }

      }
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesGetBaseNum
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 10/1/99
*PURPOSE:
* Returns the bases number.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE basesGetBaseNum(bases *value, BYTE xValue, BYTE yValue) {
  BYTE returnValue;         /* Value to return */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = BASE_NOT_FOUND-1;
  count = 0;
  done = FALSE;
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      returnValue = count;
      done = TRUE;
    }
    count++;
  }

  return (returnValue+1);
}

/*********************************************************
*NAME:          basesCanView
*AUTHOR:        John Morrison
*PURPOSE:
* The one base-view predicate: a base can be watched when it
* belongs to somebody the view player is allied with. A
* neutral base belongs to nobody, so it never qualifies.
*
*ARGUMENTS:
*  sim        - Pointer to the game sim
*  value      - Pointer to the bases structure
*  baseIdx    - Base index (0 based)
*  viewPlayer - Player doing the watching
*********************************************************/
bool basesCanView(GameSim *sim, bases *value, BYTE baseIdx, BYTE viewPlayer) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if (baseIdx < (*value)->numBases) {
    if ((*value)->active[baseIdx] != FALSE && ((*value)->item[baseIdx].owner) != NEUTRAL && (playersIsAllie(&sim->plyrs, viewPlayer, (*value)->item[baseIdx].owner) == TRUE)) {
      returnValue = TRUE;
    }
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesCheckView
*AUTHOR:        John Morrison
*PURPOSE:
* We are currently watching the base at position mx and my.
* This function checks it is still ours to watch, so we can
* carry on viewing through it.
*
*ARGUMENTS:
*  sim   - Pointer to the game sim
*  value - Pointer to the bases structure
*  mx    - X Map position
*  my    - Y Map position
*********************************************************/
bool basesCheckView(GameSim *sim, bases *value, BYTE mx, BYTE my) {
  bool returnValue; /* Value to return */
  BYTE baseNum;     /* The base at that square, 1 based */

  returnValue = FALSE;
  baseNum = basesGetBaseNum(value, mx, my);
  if (baseNum != BASE_NOT_FOUND) {
    returnValue = basesCanView(sim, value, (BYTE)(baseNum - 1), sim->viewPlayer);
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesMoveView
*AUTHOR:        John Morrison
*PURPOSE:
* Allows players to step through their bases in a direction.
* Returns whether a base was found that way. The base
* equivalent of pillsMoveView.
*
*ARGUMENTS:
*  sim      - Pointer to the game sim
*  value    - Pointer to the bases structure
*  eligible - Bit per base index: that base may be selected
*  mx       - Pointer to hold X Map position (and prev)
*  my       - Pointer to hold Y Map position (and prev)
*  xMove    - -1 for moving left, 1 for right, 0 for neither
*  yMove    - -1 for moving up, 1 for down, 0 for neither
*********************************************************/
bool basesMoveView(GameSim *sim, bases *value, PlayerBitMap eligible, BYTE *mx, BYTE *my, int xMove, int yMove) {
  bool returnValue; /* Value to return */
  double nearest;   /* Nearest */
  BYTE count;       /* Looping variable */
  BYTE found;       /* Have we found the item */
  double dist;
  BYTE oldBase;     /* The base we are on now, 0 based */
  bool matches;     /* Is this base the way we are looking */

  nearest = 65000;
  returnValue = FALSE;
  found = 0;
  count = 0;
  oldBase = basesGetBaseNum(value, *mx, *my);
  oldBase--;
  while (count < (*value)->numBases) {
    if (count != oldBase && (eligible & ((PlayerBitMap)1 << count)) != 0 && basesCanView(sim, value, count, sim->viewPlayer) == TRUE) {
      /* One axis at a time: a horizontal press only considers bases to the
       * left or right, a vertical press only ones above or below. */
      matches = FALSE;
      if (yMove == 0) {
        if ((xMove < 0 && ((*value)->item[count].x < *mx)) ||
            (xMove > 0 && ((*value)->item[count].x > *mx))) {
          matches = TRUE;
        }
      }
      if (xMove == 0) {
        if ((yMove < 0 && ((*value)->item[count].y < *my)) ||
            (yMove > 0 && ((*value)->item[count].y > *my))) {
          matches = TRUE;
        }
      }
      if (matches == TRUE) {
        if (utilIsItemInRange(*mx, *my, (*value)->item[count].x, (*value)->item[count].y, (WORLD) nearest, &dist) == TRUE) {
          nearest = dist;
          found = count;
          returnValue = TRUE;
        }
      }
    }
    count++;
  }

  if (returnValue == TRUE) {
    *mx = (*value)->item[found].x;
    *my = (*value)->item[found].y;
  }
  return returnValue;
}

/*********************************************************
*NAME:          basesGetNextView
*AUTHOR:        John Morrison
*PURPOSE:
* Returns whether a next allied base exists. If so then it
* puts its map co-ordinates into the parameters passed. If a
* previous base is being used then the parameter 'prev' is
* true and mx & my are set to the last base's location. The
* base equivalent of pillsGetNextView.
*
*ARGUMENTS:
*  sim      - Pointer to the game sim
*  value    - Pointer to the bases structure
*  eligible - Bit per base index: that base may be selected
*  mx       - Pointer to hold X Map position (and prev)
*  my       - Pointer to hold Y Map position (and prev)
*  prev     - Whether a previous base is being passed
*********************************************************/
bool basesGetNextView(GameSim *sim, bases *value, PlayerBitMap eligible, BYTE *mx, BYTE *my, bool prev) {
  bool returnValue; /* Value to return */
  bool done;        /* Finished */
  bool okLoop;      /* Ok to loop */
  BYTE playNumber;  /* My player number */
  BYTE count;       /* Counting variable */

  count = 0;
  returnValue = TRUE;
  done = FALSE;
  okLoop = FALSE;
  playNumber = sim->viewPlayer;

  /* Find out the previous amount */
  if (prev == TRUE) {
    count = basesGetBaseNum(value, *mx, *my);
    if (count == BASE_NOT_FOUND) {
      count = 0;
    } else {
      count--;
      if (basesCanView(sim, value, count, playNumber) == TRUE) {
        okLoop = TRUE;
        count++;
      } else {
        count = 0;
      }
    }
  }

  /* Find the next item */
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((eligible & ((PlayerBitMap)1 << count)) != 0 && basesCanView(sim, value, count, playNumber) == TRUE) {
      done = TRUE;
      *mx = (*value)->item[count].x;
      *my = (*value)->item[count].y;
    }
    count++;
  }

  /* If not found still and we are looping do it here */
  if (done == FALSE && okLoop == TRUE) {
    count = 0;
    while (done == FALSE && count < ((*value)->numBases)) {
      if ((eligible & ((PlayerBitMap)1 << count)) != 0 && basesCanView(sim, value, count, playNumber) == TRUE) {
        done = TRUE;
        *mx = (*value)->item[count].x;
        *my = (*value)->item[count].y;
      }
      count++;
    }
  }

  /* If we still haven't found one then one doesn't exist at all */
  if (done == FALSE) {
    returnValue = FALSE;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesRefueling
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 22/12/99
*PURPOSE:
* The baseNum is being occupied by a tank. Refuel it
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
void basesRefueling(GameSim *sim, tank *tnk, BYTE baseNum) {
  bases *value = &sim->bs;
  bool isServer = sim->isServer;
  BYTE shellsAmount;  /* Tank Shells, mines and armour */
  BYTE mines;
  BYTE armour;
  BYTE trees;

  baseNum--;

  if ((*value)->active[baseNum] == FALSE) {
    return;
  }
  if ((*value)->item[baseNum].refuelTime == 0) {
    tankGetStats(tnk, &shellsAmount, &mines, &armour, &trees);
    if (playersIsAllie(&sim->plyrs, (*value)->item[baseNum].owner, gameSimGetTankPlayer(sim, tnk))) {
      /* A destroyed tank draws nothing from the base. Its armour reads as a
       * real 0 rather than a wrapped value, so the capacity test below no
       * longer rejects it on its own. */
      if (!tankIsDestroyed(tnk) && armour < sim->rules.tank_full_armour && ((*value)->item[baseNum].armour - sim->rules.base_armour_give) >= sim->rules.base_min_armour) {
        (*value)->item[baseNum].armour -= sim->rules.base_armour_give;
        tankAddArmour(sim, tnk, sim->rules.base_armour_give);
        (*value)->item[baseNum].refuelTime = basesHalfTickCalulator(sim, BASES_HALFTICK_TYPE_ARMOUR);
        if (isServer == FALSE) {
          frontEndUpdateBaseStatusBars(clientSimFromSim(sim), ((*value)->item[baseNum].shells), ((*value)->item[baseNum].mines), ((*value)->item[baseNum].armour));
        }
      } else if (shellsAmount < sim->rules.tank_full_shells && ((*value)->item[baseNum].shells - sim->rules.base_shells_give) >= sim->rules.base_min_shells) {
        (*value)->item[baseNum].shells -= sim->rules.base_shells_give;
        tankAddShells(sim, tnk, sim->rules.base_shells_give);
        (*value)->item[baseNum].refuelTime = basesHalfTickCalulator(sim, BASES_HALFTICK_TYPE_SHELL);
        if (isServer == FALSE) {
          frontEndUpdateBaseStatusBars(clientSimFromSim(sim), ((*value)->item[baseNum].shells), ((*value)->item[baseNum].mines), ((*value)->item[baseNum].armour));
        }
      } else if (mines < sim->rules.tank_full_mines && ((*value)->item[baseNum].mines - sim->rules.base_mines_give) >= sim->rules.base_min_mines) {
        (*value)->item[baseNum].mines -= sim->rules.base_mines_give;
        tankAddMines(sim, tnk, sim->rules.base_mines_give);
        (*value)->item[baseNum].refuelTime = basesHalfTickCalulator(sim, BASES_HALFTICK_TYPE_MINE);
        if (isServer == FALSE) {
          frontEndUpdateBaseStatusBars(clientSimFromSim(sim), ((*value)->item[baseNum].shells), ((*value)->item[baseNum].mines), ((*value)->item[baseNum].armour));
        }
      }
      logAddEvent(log_BaseSetStock, baseNum, (*value)->item[baseNum].shells, (*value)->item[baseNum].mines, (*value)->item[baseNum].armour, 0, NULL);
    }
  }
}

/*********************************************************
*NAME:          basesGetClosest
*AUTHOR:        John Morrison
*CREATION DATE: 11/1/99
*LAST MODIFIED: 27/5/00
*PURPOSE:
* Returns the base Number of the base closest and is 
* either neutral or allied to the tank and inside the 
* range. If no base is inside the range it returns 
* BASE_NOT_FOUND.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Map Location of the tank
*  yValue - Y Map Location of the tank
*********************************************************/
BYTE basesGetClosest(GameSim *sim, WORLD tankX, WORLD tankY) {
  return basesGetClosestForPlayer(sim, sim->viewPlayer, tankX, tankY,
                                  (WORLD) sim->rules.base_status_range);
}

/*********************************************************
*NAME:          basesGetClosestForPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 11/1/99
*LAST MODIFIED: 11/1/99
*PURPOSE:
* Explicit-player variant of basesGetClosest: evaluates the
* closest neutral/allied-in-range base for the given player
* rather than sim->viewPlayer.
*
*ARGUMENTS:
*  sim    - Pointer to the game sim
*  player - Player number to evaluate alliances against
*  tankX  - X Map Location of the tank
*  tankY  - Y Map Location of the tank
*  range  - Selection ceiling in world units: a base is only returned if it
*           is strictly nearer than this. Callers displaying the closest base
*           pass BASE_STATUS_RANGE; server stock-send gating passes a wider
*           ceiling so stock is revealed before the client switches to it.
*********************************************************/
BYTE basesGetClosestForPlayer(GameSim *sim, BYTE player, WORLD tankX, WORLD tankY, WORLD range) {
  bases *value = &sim->bs;
  BYTE returnValue; /* Value to return */
  WORLD x;
  WORLD y;
  WORLD gapX;       /* Gap from base to tank */
  WORLD gapY;
  double distance;
  double oldDistance;
  BYTE count; /* Looping Variable */
  BYTE self;  /* Yourselfs player number */

  oldDistance = range; /* Caller-chosen selection ceiling (BASE_STATUS_RANGE for display) */
  returnValue = BASE_NOT_FOUND-1;
  count = 0;
  self = player;

  while (count < (*value)->numBases) {
    /* Check for neutral or allied */
    if ((*value)->active[count] != FALSE && ((*value)->item[count].owner == NEUTRAL || (playersIsAllie(&sim->plyrs, self, (*value)->item[count].owner) == TRUE))) {
      x = (*value)->item[count].x;
      y = (*value)->item[count].y;
      x <<= 8;
      x += MAP_SQUARE_MIDDLE;
      y <<= 8;
      y += MAP_SQUARE_MIDDLE;
      if (tankX - x < 0) {
        gapX = x - tankX;
      } else {
        gapX = tankX - x;
      }
      if (tankY - y < 0) {
        gapY = y - tankY;
      } else {
        gapY = tankY - y;
      }

      distance = sqrt((double) ((gapX * gapX) + (gapY * gapY)));
      if (distance >=0 && distance < oldDistance) {
        oldDistance = distance;
        returnValue = count;
      }
    }
    count++;
  }
  return (returnValue+1);
}

/*********************************************************
*NAME:          basesGetStats
*AUTHOR:        John Morrison
*CREATION DATE: 12/1/99
*LAST MODIFIED: 12/1/99
*PURPOSE:
* Gets the base statistics (armour, shells, mines) for
* a given base number
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number to get
*  shellsAmount  - Pointer to hold the shells amount
*  mines   - Pointer to hold the mines amount
*  armour  - Pointer to hold the armour amount
*********************************************************/
void basesGetStats(bases *value, BYTE baseNum, BYTE *shellsAmount, BYTE *mines, BYTE *armour) {
  /* A number in range names a slot; a slot off the map holds no stock a
     reader should see. */
  if (baseNum > 0 && baseNum <= (*value)->numBases &&
      (*value)->active[baseNum - 1] != FALSE) {
    baseNum--;
    *shellsAmount = (*value)->item[baseNum].shells;
    *mines = (*value)->item[baseNum].mines;
    *armour = (*value)->item[baseNum].armour;
  } else {
    *shellsAmount = 0;
    *mines = 0;
    *armour = 0;
  }
}

/*********************************************************
*NAME:          basesDamagePos
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 28/2/99
*PURPOSE:
* This base has been hit. Do some damage to it.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
void basesDamagePos(GameSim *sim, BYTE xValue, BYTE yValue, BYTE owner) {
  bases *value = &sim->bs;
  bool isServer = sim->isServer;
  bool done;                /* Are we finished searching for the base */
  BYTE count;               /* Looping Variable */

  count = 0;
  done = FALSE;
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && (*value)->item[count].armour > 0) {
      BYTE before = (*value)->item[count].armour;  /* > 0 here */
      /* Ask whether the shell takes more than is left rather than
         subtracting first and reading the wrap: the wrapped value only
         looked like "was full" while the cap and the damage were both
         compile-time, and a rules table can put any pair of numbers here. */
      if (sim->rules.shell_damage > before) {
        (*value)->item[count].armour = 0;
      } else {
        (*value)->item[count].armour =
            (BYTE) (before - sim->rules.shell_damage);
      }
      logAddEvent(log_BaseSetStock, count, (*value)->item[count].shells, (*value)->item[count].mines, (*value)->item[count].armour, 0, NULL);
      if (sim->callbacks.recordDamage) {
        sim->callbacks.recordDamage(sim->callbacks.ctx, owner, DMG_TARGET_BASE,
                                    count, DMG_SRC_SHELL,
                                    (uint16_t)(before - (*value)->item[count].armour), false,
                                    (*value)->item[count].x, (*value)->item[count].y);
      }
      if ((*value)->item[count].armour <= BASE_DISPLAY_X) {
        if (isServer == FALSE) {
          frontEndStatusBase(clientSimFromSim(sim), (BYTE) (count+1), baseDead);
        }
      }
      done = TRUE;
    }
    count++;
  }
}

/*********************************************************
*NAME:          basesCanHit
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
*  Returns whether the bases at the particular location
*  can be hit by a shell from a particular owner
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*  hitBy  - Person who fired the shell
*********************************************************/
bool basesCanHit(GameSim *sim, BYTE xValue, BYTE yValue, BYTE hitBy) {
  bases *value = &sim->bs;
  bool returnValue;         /* Value to return */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = FALSE;
  if (hitBy != NEUTRAL) {
    count = 0;
    done = FALSE;
    while (done == FALSE && count < ((*value)->numBases)) {
      if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
        if ((playersIsAllie(&sim->plyrs, ((*value)->item[count].owner), hitBy) == FALSE) && (*value)->item[count].owner != NEUTRAL && (*value)->item[count].armour > sim->rules.base_hit_armour) {
          returnValue = TRUE;
        }
        done = TRUE;
      }
      count++;
    }
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesCantDrive
*AUTHOR:        Minhiriath
*CREATION DATE: 29/12/2008
*LAST MODIFIED: 29/12/2008
*PURPOSE:
*  Returns whether the bases at the particular location
*  can be driven over by an object owned by the player lgm, or tank.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*  hitBy  - Person who fired the shell
*********************************************************/
bool basesCantDrive(GameSim *sim, BYTE xValue, BYTE yValue, BYTE hitBy) {
  bases *value = &sim->bs;
  bool returnValue;         /* Value to return */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = FALSE;
  if (hitBy != NEUTRAL) {
    count = 0;
    done = FALSE;
    while (done == FALSE && count < ((*value)->numBases)) {
      if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
        if ((playersIsAllie(&sim->plyrs, ((*value)->item[count].owner), hitBy) == FALSE) && (*value)->item[count].owner != NEUTRAL && (*value)->item[count].armour > sim->rules.base_capture_armour) {
          returnValue = TRUE;
        }
        done = TRUE;
      }
      count++;
    }
  }

  return returnValue;
}
/*********************************************************
*NAME:          basesArmourVisibleToPlayer
*PURPOSE:
*  Returns whether a base's true armour may be sent to a
*  player rather than the BASE_FULL_ARMOUR stand-in. See the
*  header for the three cases.
*
*ARGUMENTS:
*  sim     - Pointer to the game sim
*  baseIdx - Index of the base being considered
*  player  - Player the send is destined for
*********************************************************/
bool basesArmourVisibleToPlayer(GameSim *sim, BYTE baseIdx, BYTE player) {
  bases *value = &sim->bs;
  BYTE owner;
  int baseX, baseY, gapX, gapY;
  WORLD tankX, tankY;

  if (baseIdx >= (*value)->numBases || (*value)->active[baseIdx] == FALSE) {
    return FALSE;
  }

  /* Neutral, own and allied bases are never masked. */
  owner = (*value)->item[baseIdx].owner;
  if (owner == NEUTRAL || owner == player ||
      playersIsAllie(&sim->plyrs, owner, player) == TRUE) {
    return TRUE;
  }

  /* A dead enemy base reports its real armour so the capturable flip shows. */
  if ((*value)->item[baseIdx].armour <= sim->rules.base_capture_armour) {
    return TRUE;
  }

  /* Otherwise only while the player's tank is close enough that their client
     has to predict this square's solidity. A player with no living tank has
     nothing to predict with. */
  if (player >= MAX_TANKS || sim->tanks[player] == NULL) {
    return FALSE;
  }
  if (tankIsDestroyed(&sim->tanks[player])) {
    return FALSE;
  }
  tankGetWorld(&sim->tanks[player], &tankX, &tankY);

  baseX = ((int)(*value)->item[baseIdx].x << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE;
  baseY = ((int)(*value)->item[baseIdx].y << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE;
  gapX = abs((int)tankX - baseX);
  gapY = abs((int)tankY - baseY);
  /* Bound both axes before squaring so the multiply cannot overflow on a
     full-size map; never rejects a base that is genuinely in range. */
  if (gapX >= sim->rules.base_reveal_range ||
      gapY >= sim->rules.base_reveal_range) {
    return FALSE;
  }
  return (gapX * gapX + gapY * gapY) <
         ((int64_t) sim->rules.base_reveal_range *
          sim->rules.base_reveal_range);
}

/*********************************************************
*NAME:          basesGetBaseOwner
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* Returns the owner of a base 
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - The base number to check
*********************************************************/
BYTE basesGetBaseOwner(bases *value, BYTE baseNum) {
  BYTE returnValue;         /* Value to return */

  returnValue = BASE_NOT_FOUND;
  /* A removed base answers the same as one off the end of the list: there is
     no such base, so it has no owner. */
  if (baseNum > 0 && baseNum <= (*value)->numBases &&
      (*value)->active[baseNum - 1] != FALSE) {
    baseNum--;
    returnValue = (*value)->item[baseNum].owner;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesGetOwnerPos
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* Returns the owner of a base at the position.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE basesGetOwnerPos(bases *value, BYTE xValue, BYTE yValue) {
  BYTE returnValue;         /* Value to return */
  bool done;                /* Finished looping */
  BYTE count;               /* Looping Variable */

  returnValue = NEUTRAL;
  count = 0;
  done = FALSE;
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      returnValue = (*value)->item[count].owner;
      done = TRUE;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesGetNumNeutral
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the number of neutral bases
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
BYTE basesGetNumNeutral(bases *value) {
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping variable */
  
  returnValue = 0;
  for (count=0;count<(*value)->numBases;count++) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].owner == NEUTRAL) {
      returnValue++;
    }
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesValidate
*PURPOSE:
*  Clamps the base fields a map cannot be trusted on whatever
*  the rules are: the count, which indexes item[], and each
*  owner, which indexes the player list. basesSetBase applies
*  the owner clamp on the file-load path; the compressed path
*  memcpys the structs wholesale and reaches neither, so a
*  downloaded map can seat values no legitimate map holds.
*  Both are properties of the file, so this runs without a
*  sim and the map editor, the preview and the tile-test tool
*  get it as the game does; the gameplay caps are
*  basesClampToRules below. Idempotent, and pure clamping: no
*  logging or side effects, so it is safe to call on a
*  half-built map.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
void basesValidate(bases *value) {
  BYTE count;

  if (value == NULL || *value == NULL) {
    return;
  }
  if ((*value)->numBases > MAX_BASES) {
    (*value)->numBases = MAX_BASES;
  }
  for (count = 0; count < (*value)->numBases; count++) {
    base *item = &((*value)->item[count]);
    /* x and y are BYTE against a 256x256 map, so every value is in
     * range by type and needs no clamp. */
    if (item->owner > (MAX_TANKS - 1) && item->owner != NEUTRAL) {
      item->owner = NEUTRAL;
    }
  }
}

/*********************************************************
*NAME:          basesClampToRules
*PURPOSE:
*  Clamps every base's stocks to what this sim lets a base
*  hold. What a base may carry is a gameplay number, so it
*  is capped here, once a sim owns the records, rather than
*  on the load path, which the map editor and the preview
*  share and which has no sim to ask. Pure clamping, and
*  idempotent.
*
*ARGUMENTS:
*  sim   - Pointer to the game sim
*  value - Pointer to the bases structure
*********************************************************/
void basesClampToRules(GameSim *sim, bases *value) {
  BYTE count;

  if (sim == NULL || value == NULL || *value == NULL) {
    return;
  }
  for (count = 0; count < (*value)->numBases; count++) {
    base *item = &((*value)->item[count]);
    if (item->armour > sim->rules.base_full_armour) {
      item->armour = (BYTE) sim->rules.base_full_armour;
    }
    if (item->shells > sim->rules.base_full_shells) {
      item->shells = (BYTE) sim->rules.base_full_shells;
    }
    if (item->mines > sim->rules.base_full_mines) {
      item->mines = (BYTE) sim->rules.base_full_mines;
    }
  }
}

/*********************************************************
*NAME:          basesFillToRules
*PURPOSE:
*  Brings every base up to the stock caps this sim runs on.
*  The other direction from basesClampToRules above, for a
*  scenario that raises a cap and asks for the map to start
*  at it: a map file holds a number and has no way of saying
*  "full", so without this a raised cap leaves every base
*  where the file put it.
*
*  Idempotent, and a base already at a cap is left alone.
*
*ARGUMENTS:
*  sim   - Pointer to the game sim
*  value - Pointer to the bases structure
*********************************************************/
void basesFillToRules(GameSim *sim, bases *value) {
  BYTE count;

  if (sim == NULL || value == NULL || *value == NULL) {
    return;
  }
  for (count = 0; count < (*value)->numBases; count++) {
    base *item = &((*value)->item[count]);
    if (item->armour < sim->rules.base_full_armour) {
      item->armour = (BYTE) sim->rules.base_full_armour;
    }
    if (item->shells < sim->rules.base_full_shells) {
      item->shells = (BYTE) sim->rules.base_full_shells;
    }
    if (item->mines < sim->rules.base_full_mines) {
      item->mines = (BYTE) sim->rules.base_full_mines;
    }
  }
}

void basesSetBaseCompressData(bases *value, BYTE *buff, int dataLen) {
  memcpy(&(**value), buff, SIZEOF_BASES);
  /* The wire blob carries numBases in its trailing byte; a hostile map can
   * set it past MAX_BASES, driving out-of-bounds reads of item[] for the
   * life of the game. Clamp it here so the corrupt count never propagates. */
  if ((*value)->numBases > MAX_BASES) {
    (*value)->numBases = MAX_BASES;
  }
  /* The live flags sit past the wire format, so the copy above leaves them
     describing whatever list was here before. A blob is a map, and every base
     a map carries is on it: mark the count live and the slots above it
     removed. */
  memset((*value)->active, TRUE, (*value)->numBases);
  memset((*value)->active + (*value)->numBases, FALSE,
         (size_t)(MAX_BASES - (*value)->numBases));
}

/*********************************************************
*NAME:          basesSetBaseNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Gets a copy of the base data and copies it to buff.
* Returns the data length
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*  buff  - Buffer to hold copy of bases data
*********************************************************/
BYTE basesGetBaseNetData(bases *value, BYTE *buff) {
  int returnValue = 1; /* Value to return - Data length  */
  BYTE count = 0;
  unsigned short us;

  buff[0] = (*value)->numBases;
  while (count < (*value)->numBases) {
    buff[returnValue] = (*value)->item[count].x;
    returnValue++;
    buff[returnValue] = (*value)->item[count].y;
    returnValue++;
    buff[returnValue] = (*value)->item[count].owner;
    returnValue++;
    buff[returnValue] = (*value)->item[count].armour;
    returnValue++;
    buff[returnValue] = (*value)->item[count].shells;
    returnValue++;
    buff[returnValue] = (*value)->item[count].mines;
    returnValue++;
    buff[returnValue] = (*value)->item[count].refuelTime;
    returnValue++;
    us = (*value)->item[count].baseTime;
    us = htons(us);
    buff[returnValue] = (BYTE) (us >> 8);
    returnValue++;
    buff[returnValue] = (BYTE) (us & 0xFF);
    returnValue++;
    buff[returnValue] = (*value)->item[count].justStopped;
    returnValue++;
    count++;
  }

  return returnValue;

  /* Old code *
	memcpy(buff, &(**value), SIZEOF_BASES);
  return SIZEOF_BASES; */
}

/*********************************************************
*NAME:          basesNetGiveArmour
*AUTHOR:        John Morrison
*CREATION DATE: 9/3/99
*LAST MODIFIED: 9/3/99
*PURPOSE:
* A network call has been made that some one is refueling
* armour from a base. Remove it and update the screen here
*
*ARGUMENTS:
*  sim     - The game the base belongs to
*  value   - Pointer to the bases structure
*  baseNum - Basenum it is happening to
*********************************************************/
void basesNetGiveArmour(GameSim *sim, bases *value, BYTE baseNum) {
  if (((*value)->item[baseNum].armour - sim->rules.base_armour_give) >= sim->rules.base_min_armour) {
    (*value)->item[baseNum].armour -= sim->rules.base_armour_give;
    (*value)->item[baseNum].refuelTime = basesHalfTickCalulator(sim, BASES_HALFTICK_TYPE_ARMOUR);
    logAddEvent(log_BaseSetStock, baseNum, (*value)->item[baseNum].shells, (*value)->item[baseNum].mines, (*value)->item[baseNum].armour, 0, NULL);
  }
}

/*********************************************************
*NAME:          basesNetGiveShells
*AUTHOR:        John Morrison
*CREATION DATE: 9/3/99
*LAST MODIFIED: 9/3/99
*PURPOSE:
* A network call has been made that some one is refueling
* shells from a base. Remove it and update the screen here
*
*ARGUMENTS:
*  sim     - The game the base belongs to
*  value   - Pointer to the bases structure
*  baseNum - Basenum it is happening to
*********************************************************/
void basesNetGiveShells(GameSim *sim, bases *value, BYTE baseNum) {
  if (((*value)->item[baseNum].shells - sim->rules.base_shells_give) >= sim->rules.base_min_shells) {
    (*value)->item[baseNum].shells -= sim->rules.base_shells_give;
    (*value)->item[baseNum].refuelTime = basesHalfTickCalulator(sim, BASES_HALFTICK_TYPE_SHELL);
    logAddEvent(log_BaseSetStock, baseNum, (*value)->item[baseNum].shells, (*value)->item[baseNum].mines, (*value)->item[baseNum].armour, 0, NULL);
  }
}

/*********************************************************
*NAME:          basesNetGiveMines
*AUTHOR:        John Morrison
*CREATION DATE: 9/3/99
*LAST MODIFIED: 9/3/99
*PURPOSE:
* A network call has been made that some one is refueling
* mines from a base. Remove it and update the screen here
*
*ARGUMENTS:
*  sim     - The game the base belongs to
*  value   - Pointer to the bases structure
*  baseNum - Basenum it is happening to
*********************************************************/
void basesNetGiveMines(GameSim *sim, bases *value, BYTE baseNum) {
  if (((*value)->item[baseNum].mines - sim->rules.base_mines_give) >= sim->rules.base_min_mines) {
    (*value)->item[baseNum].mines -= sim->rules.base_mines_give;
    (*value)->item[baseNum].refuelTime = basesHalfTickCalulator(sim, BASES_HALFTICK_TYPE_MINE);
    logAddEvent(log_BaseSetStock, baseNum, (*value)->item[baseNum].shells, (*value)->item[baseNum].mines, (*value)->item[baseNum].armour, 0, NULL);
  }
}

/*********************************************************
*NAME:          basesSetNeutralOwner
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
*  Changes all bases of owner owner back to neutral
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*  owner - Owner to set back to neutral
*********************************************************/
void basesSetNeutralOwner(GameSim *sim, BYTE owner) {
  bases *value = &sim->bs;
  BYTE count;       /* Looping Variable */

  count = 0;
  while (count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].owner) == owner) {
      (*value)->item[count].owner = NEUTRAL;
      logAddEvent(log_BaseSetOwner, count, NEUTRAL, FALSE, 0, 0, NULL);
    }
    count++;
  }
}

/*********************************************************
*NAME:          basesMigrate
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
*  Causes all bases owned by owner  to migrate to a new 
* owner because its old owner left the game.
*
*ARGUMENTS:
*  value    - Pointer to the bases structure
*  oldOwner - Old Owner to remove
*  newOwner - Owner to replace with
*********************************************************/
void basesMigrate(GameSim *sim, BYTE oldOwner, BYTE newOwner) {
  bases *value = &sim->bs;
  BYTE count;       /* Looping Variable */

  count = 0;
  while (count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].owner) == oldOwner) {
      (*value)->item[count].owner = newOwner;
      logAddEvent(log_BaseSetOwner, count, newOwner, TRUE, 0, 0, NULL);
    }
    count++;
  }
}

/*********************************************************
*NAME:          basesServerRefuel
*AUTHOR:        John Morrison
*CREATION DATE: 9/11/99
*LAST MODIFIED: 9/11/99
*PURPOSE:
*  The server has issued a refuel command to a base
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  baseNum   - The base number to refuel
*  addAmount - Amount of items to add
*********************************************************/
void basesServerRefuel(GameSim *sim, BYTE baseNum, BYTE addAmount) {
  bases *value = &sim->bs;
  bool isServer = sim->isServer;
  BYTE oldArmour; /* Amount of old armour base had */

  if (baseNum < (*value)->numBases && (*value)->active[baseNum] != FALSE) {
    if ((*value)->item[baseNum].armour < sim->rules.base_full_armour) {
      oldArmour = (*value)->item[baseNum].armour;
      (*value)->item[baseNum].armour += addAmount;
      if ((*value)->item[baseNum].armour > sim->rules.base_full_armour) {
        (*value)->item[baseNum].armour = (BYTE) sim->rules.base_full_armour;
      }
      /* Update the frontend status as required: this tick is where the base
         came back off the capture threshold, so its icon changes. */
      if (oldArmour == sim->rules.base_capture_armour &&
          (*value)->item[baseNum].armour > sim->rules.base_capture_armour) {
        if (isServer == FALSE) {
          frontEndStatusBase(clientSimFromSim(sim), (BYTE) (baseNum+1), (basesGetStatusNum(sim, (BYTE) (baseNum+1))));
        }
      }
    }
    if ((*value)->item[baseNum].shells < sim->rules.base_full_shells) {
      (*value)->item[baseNum].shells += addAmount;
      if ((*value)->item[baseNum].shells > sim->rules.base_full_shells) {
        (*value)->item[baseNum].shells = (BYTE) sim->rules.base_full_shells;
      }
    }

    if ((*value)->item[baseNum].mines < sim->rules.base_full_mines) {
      (*value)->item[baseNum].mines += addAmount;
      if ((*value)->item[baseNum].mines > sim->rules.base_full_mines) {
        (*value)->item[baseNum].mines = (BYTE) sim->rules.base_full_mines;
      }
    }
  }
}

/*********************************************************
*NAME:          basesSetStock
*PURPOSE:
*  Writes what a base is holding. Where basesServerRefuel
*  above adds to each stock, this one says what each is to
*  be, capped at its full amount, and -1 leaves that stock
*  where it was.
*
*  No frontend status call: the one caller is the scenario
*  funnel, which runs on the server, and the new stock
*  reaches a client as the periodic update's does.
*
*ARGUMENTS:
*  sim     - Pointer to the game sim
*  baseNum - The base to write, counting from zero
*  armour  - Armour to hold, or -1 to leave it alone
*  shells  - Shells to hold, or -1 to leave it alone
*  mines   - Mines to hold, or -1 to leave it alone
*********************************************************/
void basesSetStock(GameSim *sim, BYTE baseNum, int16_t armour, int16_t shells, int16_t mines) {
  bases *value = &sim->bs;

  if (baseNum < (*value)->numBases && (*value)->active[baseNum] != FALSE) {
    if (armour >= 0) {
      if (armour > sim->rules.base_full_armour) {
        armour = (int16_t) sim->rules.base_full_armour;
      }
      (*value)->item[baseNum].armour = (BYTE) armour;
    }
    if (shells >= 0) {
      if (shells > sim->rules.base_full_shells) {
        shells = (int16_t) sim->rules.base_full_shells;
      }
      (*value)->item[baseNum].shells = (BYTE) shells;
    }
    if (mines >= 0) {
      if (mines > sim->rules.base_full_mines) {
        mines = (int16_t) sim->rules.base_full_mines;
      }
      (*value)->item[baseNum].mines = (BYTE) mines;
    }
    /* The same record the periodic stock update writes, in the same order. */
    logAddEvent(log_BaseSetStock, baseNum, (*value)->item[baseNum].shells, (*value)->item[baseNum].mines, (*value)->item[baseNum].armour, 0, NULL);
  }
}

/*********************************************************
*NAME:          baseIsCapturable
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a base can be captured at a specific
*  location. Returns FALSE if it doesn't exist at location
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool baseIsCapturable(GameSim *sim, bases *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  bool done;        /* Finised looping */
  BYTE count;       /* Looping Variable */  

  returnValue = FALSE;
  done = FALSE;
  count = 0;
  while (done == FALSE && count < ((*value)->numBases)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      done = TRUE;
      if ((*value)->item[count].owner == NEUTRAL ||
          (*value)->item[count].armour <= sim->rules.base_capture_armour) {
        returnValue = TRUE;
      }
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesGetBrainBaseItem
*AUTHOR:        John Morrison
*CREATION DATE: 25/11/99
*LAST MODIFIED: 25/11/99
*PURPOSE:
*  Returns the base info required by a brain for a 
*  specific base.
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  baseNum - Base Number to get info for
*  wx      - Bases world X Location
*  wy      - Bases world Y Location
*  info    - Bases alliance
*  shells  - Amount of shells in the base
*  mines   - Amount of mines in the base
*  armour  - Amount of armour in the base
*********************************************************/
void basesGetBrainBaseItem(GameSim *sim, BYTE baseNum, WORLD *wx, WORLD *wy, BYTE *info, BYTE *shell, BYTE *mines, BYTE *armour) {
  bases *value = &sim->bs;
  if (baseNum > 0 && baseNum <= (*value)->numBases) {
    baseNum--;
    *wx = (*value)->item[baseNum].x;
    *wx <<= TANK_SHIFT_MAPSIZE;
    *wy = (*value)->item[baseNum].y;
    *wy <<= TANK_SHIFT_MAPSIZE;
    if ((*value)->item[baseNum].owner == NEUTRAL) {
      *info = BASES_BRAIN_NEUTRAL;
    } else if (playersIsAllie(&sim->plyrs, sim->viewPlayer, (*value)->item[baseNum].owner) == TRUE) {
      *info = BASES_BRAIN_FRIENDLY;
    } else {
      *info = BASES_BRAIN_HOSTILE;
    }

    *shell = (*value)->item[baseNum].shells;
    *mines = (*value)->item[baseNum].mines;
    *armour = (*value)->item[baseNum].armour /5;
  }
}

/*********************************************************
*NAME:          basesGetBrainBaseInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 09/06/01
*PURPOSE:
*  Makes the brain base info for each base inside the
*  rectangle formed by the function parameters
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  leftPos   - Left position of rectangle
*  rightPos  - Right position of rectangle
*  topPos    - Top position of rectangle
*  bottomPos - Bottom position of rectangle
*********************************************************/
void basesGetBrainBaseInRect(ClientSim *cs, GameSim *sim, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos) {
  bases *value = &sim->bs;
  BYTE count;     /* Looping variable */
  WORLD wx;       /* X and Y Positions of the object */
  WORLD wy;
  BYTE owner;     /* Owner of the item */
  BYTE playerNum; /* Our player Number */
  bool isAllie;   /* Is this item our allie */
  BYTE armour;


  count = 0;
  playerNum = sim->viewPlayer;

/* typedef struct
	{
	OBJECT object; = 3
	WORLD_X x;
	WORLD_Y y;
	WORD idnum;
	BYTE direction;
	BYTE info;
	} ObjectInfo;
*/

  while (count < ((*value)->numBases)) {
    isAllie = FALSE;
    if ((*value)->item[count].owner != NEUTRAL) {
      isAllie = playersIsAllie(&sim->plyrs, playerNum, (*value)->item[count].owner);
    }
    if ((*value)->active[count] != FALSE && ((((*value)->item[count].x) >= leftPos && ((*value)->item[count].x) <= rightPos && ((*value)->item[count].y) >= topPos && ((*value)->item[count].y) <= bottomPos) || isAllie == TRUE)) {
      /* In the rectangle */
      wx = (*value)->item[count].x;
      wx <<= TANK_SHIFT_MAPSIZE;
      wy = (*value)->item[count].y;
      wy <<= TANK_SHIFT_MAPSIZE;
      wx += MAP_SQUARE_MIDDLE;
      wy += MAP_SQUARE_MIDDLE;
      if ((*value)->item[count].owner == NEUTRAL) {
        owner = BASES_BRAIN_NEUTRAL;
      } else if (isAllie == TRUE) {
        owner = BASES_BRAIN_FRIENDLY;
      } else {
        owner = BASES_BRAIN_HOSTILE;
      }
      if (isAllie == FALSE) {
        /* Fog of war: hostile bases report 1 (alive) or 0 (capturable).
         * A base is capturable at or below this sim's capture threshold. */
        armour = ((*value)->item[count].armour <=
                  sim->rules.base_capture_armour) ? 0 : 1;
      } else {
        armour = (BYTE) ((*value)->item[count].armour / 5);
      }
      brainDataAddObject(cs, BASES_BRAIN_OBJECT_TYPE, wx, wy, count, armour, owner, 0);
    }
    count++;
  }
}

/*********************************************************
*NAME:          basesGetMaxs
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Gets the maximum positions values of all the bases.
*  eg left most, right most etc.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  leftPos   - Pointer to hold the left most value
*  rightPos  - Pointer to hold the right most value
*  topPos    - Pointer to hold the top most value
*  bottomPos - Pointer to hold the bottom most value
*********************************************************/
void basesGetMaxs(bases *value, int *leftPos, int *rightPos, int *topPos, int *bottomPos) {
  BYTE count; /* Looping Variable */

  *topPos = MAP_ARRAY_SIZE;
  *bottomPos = -1;
  *leftPos = MAP_ARRAY_SIZE;
  *rightPos = -1;

  count = 0;
  while (count < ((*value)->numBases)) {
    if ((*value)->active[count] == FALSE) {
      count++;
      continue;
    }
    if ((*value)->item[count].x < *leftPos) {
      *leftPos = (*value)->item[count].x;
    }
    if ((*value)->item[count].x > *rightPos) {
      *rightPos = (*value)->item[count].x;
    }
    if ((*value)->item[count].y < *topPos) {
      *topPos = (*value)->item[count].y;
    }
    if ((*value)->item[count].y > *bottomPos) {
      *bottomPos = (*value)->item[count].y;
    }  
    count++;
  }
}

/*********************************************************
*NAME:          basesMoveAll
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Repositions the bases by moveX, moveY
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  moveX  - The X move amount 
*  moveY  - The Y move amount 
*********************************************************/
void basesMoveAll(bases *value, int moveX, int moveY) {
  BYTE count; /* Looping Variable */

  count = 0;
  while (count < ((*value)->numBases)) {
    (*value)->item[count].x = (BYTE) ((*value)->item[count].x + moveX);
    (*value)->item[count].y = (BYTE) ((*value)->item[count].y + moveY);
    count++;
  }
}

/*********************************************************
*NAME:          basesGetOwnerBitMask
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Returns the owner bitmask for all the bases own by a 
* player.
*
*ARGUMENTS:
*  value  - Pointer to the bases  structure
*  owner  - Owner to return bases owned for
*  moveY  - The Y move amount 
*********************************************************/
PlayerBitMap basesGetOwnerBitMask(bases *value, BYTE owner) {
  PlayerBitMap returnValue; /* Value to return */
  BYTE count;               /* Looping variable */

  count = 0;
  returnValue = 0;
  while (count < (*value)->numBases) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].owner == owner) {
      returnValue |= 1 << count;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesClearMines
*AUTHOR:        John Morrison
*CREATION DATE: 31/7/00
*LAST MODIFIED: 31/7/00
*PURPOSE:
* Removes mines from under bases.
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*  mp     - Pointer to the map structure
*********************************************************/
void basesClearMines(GameSim *sim) {
  bases *value = &sim->bs;
  map *mp = &sim->mp;
  BYTE count;       /* Looping Variable */
  BYTE terrain;     /* Terrain Value    */

  count = 0;
  while (count < ((*value)->numBases)) {
    if ((*value)->active[count] == FALSE) {
      count++;
      continue;
    }
    terrain = mapGetPos(mp, (*value)->item[count].x, (*value)->item[count].y);
    if (terrain >= MINE_START && terrain <= MINE_END) {
      mapSetPos(sim, mp, (*value)->item[count].x, (*value)->item[count].y, (BYTE) (terrain - MINE_SUBTRACT), FALSE, TRUE);
    }
    count++;
  }
}

/*********************************************************
*NAME:          basesGetNumberOwnedByPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 19/11/03
*LAST MODIFIED: 19/11/03
*PURPOSE:
* Returns the number of bases owned by this player
*
*ARGUMENTS:
*  value     - Pointer to the bases structure
*  playerNum - Player number for bases
*********************************************************/
BYTE basesGetNumberOwnedByPlayer(bases *value, BYTE playerNum) {
  BYTE returnValue; /* Value to return */
  BYTE count; /* Looping variable */

  count = 0;
  returnValue = 0;

  while (count < (*value)->numBases) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].owner == playerNum) {
      returnValue++;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          basesHalfTickCalulator
*AUTHOR:        Minhiriath
*CREATION DATE: 1/3/2009
*LAST MODIFIED: 3/3/2009
*PURPOSE:
* calculate what to return to average out half ticks
*
*ARGUMENTS:
*  sim          - The game the base belongs to
*  typeSelector - tells us what type of number to return.
*********************************************************/
int basesHalfTickCalulator(GameSim *sim, int typeSelector) {
	double tempShell = 0;
	double tempMine = 0;

	switch(typeSelector)
	{
	case BASES_HALFTICK_TYPE_SHELL:
	  if(floor(sim->rules.base_refuel_shells_ticks) != sim->rules.base_refuel_shells_ticks){
	  	if(sim->halfTickShell == 0){
			sim->halfTickShell = sim->rules.base_refuel_shells_ticks-floor(sim->rules.base_refuel_shells_ticks);
			tempShell = floor(sim->rules.base_refuel_shells_ticks);
			return (int) tempShell;
		} else {
			tempShell = sim->rules.base_refuel_shells_ticks+sim->halfTickShell;
			sim->halfTickShell = 0;
			return (int) floor(tempShell);
		}
	  } else {
	    return sim->rules.base_refuel_shells_ticks;
	  }
	  break;
	case BASES_HALFTICK_TYPE_MINE:
	  if(floor(sim->rules.base_refuel_mines_ticks) != sim->rules.base_refuel_mines_ticks){
	  	if(sim->halfTickMine == 0){
			sim->halfTickMine = sim->rules.base_refuel_mines_ticks-floor(sim->rules.base_refuel_mines_ticks);
			return (int) floor(sim->rules.base_refuel_mines_ticks);
		} else {
			tempMine = sim->rules.base_refuel_mines_ticks+sim->halfTickMine;
			sim->halfTickMine = 0;
			return (int) floor(tempMine);
		}
	  } else {
	    return sim->rules.base_refuel_mines_ticks;
	  }
	  break;
	case BASES_HALFTICK_TYPE_ARMOUR:
	  return sim->rules.base_refuel_armour_ticks;
	  break;
	default:
	  return 0;
	  break;
	}
}

/*********************************************************
*NAME:          basesGetNumActive
*PURPOSE:
*  Returns how many bases are on the map: the slots under
*  the count whose live flag is set.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
BYTE basesGetNumActive(bases *value) {
  BYTE count;
  BYTE live = 0;
  if (value == NULL || *value == NULL) {
    return 0;
  }
  for (count = 0; count < (*value)->numBases && count < MAX_BASES; count++) {
    if ((*value)->active[count] != FALSE) {
      live++;
    }
  }
  return live;
}

/*********************************************************
*NAME:          basesRemoveBorderBases
*PURPOSE:
*  Takes off the map every live base that sits in the mined
*  border round the edge, the same rule as
*  startsRemoveBorderStarts. No tank can reach a base out
*  there, so it could never be captured or used, yet it was
*  counted, drawn and handed to the bots. Nothing is changed
*  when no live base is inside the border, so such a map
*  still plays as it does today. Slot numbers do not change.
*  Returns how many bases were taken off.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
BYTE basesRemoveBorderBases(bases *value) {
  BYTE count;
  BYTE inside = 0;
  BYTE removed = 0;

  if (value == NULL || *value == NULL) {
    return 0;
  }
  for (count = 0; count < (*value)->numBases && count < MAX_BASES; count++) {
    if ((*value)->active[count] != FALSE &&
        mapPosInBounds((*value)->item[count].x, (*value)->item[count].y)) {
      inside++;
    }
  }
  if (inside == 0) {
    return 0;
  }
  for (count = 0; count < (*value)->numBases && count < MAX_BASES; count++) {
    if ((*value)->active[count] != FALSE &&
        !mapPosInBounds((*value)->item[count].x, (*value)->item[count].y)) {
      (*value)->active[count] = FALSE;
      removed++;
    }
  }
  return removed;
}
