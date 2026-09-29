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
*Name:          Pillbox
*Filename:      pillbox.c
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 11/02/09
*Purpose:
*  Provides operations on pillbox and pillboxes
*********************************************************/

/* Includes */
#include <math.h>
#include <memory.h>
#include "global.h"
#include "tank.h"
#include "tilenum.h"
#include "frontend.h"
#include "../gui/lang.h"
#include "sounddist.h"
#include "brain_data.h"
#include "messages.h"
#include "players.h"
#include "log.h"
#include "server_sim.h"
#include "pillbox.h"
#include "bolo_map.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "../winbolonet/winbolonet_core.h"

/*********************************************************
*NAME:          pillsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the pillboxes structure.
*  Sets number of pillboxes to zero
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void pillsCreate(pillboxes *value) {
  BYTE count; /* Looping variable */

  New(*value);
  memset(*value, 0, sizeof(**value));
  ((*value)->numPills) = 0;

  for(count=0;count<MAX_PILLS;count++) {
    (*value)->item[count].speed = PILLBOX_ATTACK_NORMAL;
    (*value)->item[count].reload = PILLBOX_ATTACK_NORMAL;
    (*value)->item[count].coolDown = 0;
    (*value)->item[count].inTank = FALSE;
    (*value)->item[count].justSeen = FALSE;
  }
}


/*********************************************************
*NAME:          pillsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 11/12/98
*PURPOSE:
*  Destroys the pills data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*********************************************************/
void pillsDestroy(pillboxes *value) {
  if (*value != NULL) {
    Dispose(*value);
  }
}

/*********************************************************
*NAME:          pillsSetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific pill with its item data
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  numPills - The number of pills
*********************************************************/
void pillsSetNumPills(pillboxes *value, BYTE numPills) {
  BYTE count; /* Looping variable */

  if (numPills > 0 && numPills <= MAX_PILLS) {
    (*value)->numPills = numPills;
    /* Every pill a map brings in is live. Removal happens after the list is
       loaded, so the count and the live set agree here. */
    for (count = 0; count < numPills; count++) {
      (*value)->active[count] = TRUE;
    }
  }
}

/*********************************************************
*NAME:          pillsGetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of pills in the structure
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*********************************************************/
BYTE pillsGetNumPills(pillboxes *value) {
  if ((*value) != NULL) {
    return (*value)->numPills;
  }
  return 0;
}

/*********************************************************
*NAME:          pillsSetPill
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED:  29/4/00
*PURPOSE:
*  Sets a specific pill with its item data
*
*ARGUMENTS:
*  sim     - Pointer to the game sim
*  value   - Pointer to the pillbox structure
*  item    - Pointer to a pillbox
*  pillNum - The pillbox number
*********************************************************/
void pillsSetPill(GameSim *sim, pillboxes *value, pillbox *item, BYTE pillNum) {
  if (pillNum > 0 && pillNum  <= (*value)->numPills) {
    pillNum--;
    (((*value)->item[pillNum]).x) = item->x;
    (((*value)->item[pillNum]).y) = item->y;
    if (item->owner > (MAX_TANKS-1) && item->owner != NEUTRAL) {
      item->owner = NEUTRAL;
    }
    (((*value)->item[pillNum]).owner) = item->owner;
    /* Clamp armour to the sim's cap. The damage path's guard at
     * pillsGetDamagePos already catches overflow during gameplay,
     * but a caller can hand this a record holding anything — the
     * repair path only caps on the way up, so an out-of-range
     * value would persist until first repair. */
    if (item->armour > sim->rules.pill_max_armour) {
      item->armour = (BYTE) sim->rules.pill_max_armour;
    }
    (((*value)->item[pillNum]).armour) = item->armour;


    /* Clamp speed into the attack interval the sim allows. The damage
     * path floors speed at the minimum when the pill gets hit and the
     * cooldown tick growth ceilings it at the normal interval — values
     * outside that window are unreachable through normal play but
     * survive on the wire (speed=0 fires every tick; speed=255 is
     * a passive pillbox). The "starts angry" feature still works
     * because any value inside the pair is legitimate. */
    if (item->speed < sim->rules.pill_attack_min_ticks) {
      item->speed = (BYTE) sim->rules.pill_attack_min_ticks;
    } else if (item->speed > sim->rules.pill_attack_ticks) {
      item->speed = (BYTE) sim->rules.pill_attack_ticks;
    }
    if (item->speed != sim->rules.pill_attack_ticks) {
      (*value)->item[pillNum].coolDown = (BYTE) sim->rules.pill_cooldown_ticks;
    }
    (((*value)->item[pillNum]).speed) = item->speed;
    (((*value)->item[pillNum]).inTank) = item->inTank;
    (((*value)->item[pillNum]).justSeen) = item->justSeen;
    logAddEvent(log_PillSetOwner, pillNum, item->owner, TRUE, 0, 0, NULL);
    logAddEvent(log_PillSetHealth, pillNum, item->armour, 0, 0, 0, NULL);
    logAddEvent(log_PillSetInTank, utilPutNibble(pillNum, FALSE), 0, 0, 0, 0, NULL);
    logAddEvent(log_PillSetPlace, pillNum, item->x, item->y, 0, 0, NULL);
  }
}


/*********************************************************
*NAME:          pillsGetPill
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
*  Gets a specific pill.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - Pointer to a pillbox
*  pillNum - The pillbox number
*********************************************************/
void pillsGetPill(pillboxes *value, pillbox *item, BYTE pillNum) {
  /* Whole-struct copy. This used to assign six fields by hand and leave
     reload, coolDown and justSeen as whatever the caller's local held,
     which a reader of those three read back as undefined. */
  if (pillNum > 0 && pillNum  <= (*value)->numPills) {
    pillNum--;
    *item = (*value)->item[pillNum];
  }
}

/*********************************************************
*NAME:          pillsAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Puts a pillbox into the list and returns its number in
*  outPillNum. The lowest removed slot is reused; when
*  every slot in the count is live the list is extended and
*  the count raised. Returns FALSE with outPillNum
*  untouched when all MAX_PILLS pills are live.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  item       - The pillbox to store
*  outPillNum - Receives the pillbox number, 1 based
*********************************************************/
bool pillsAddItem(pillboxes *value, const pillbox *item, BYTE *outPillNum) {
  BYTE count; /* Looping variable */

  for (count = 0; count < (*value)->numPills; count++) {
    if ((*value)->active[count] == FALSE) {
      (*value)->item[count] = *item;
      (*value)->active[count] = TRUE;
      (*value)->posStale[count] = PILL_SQUARE_CONFIRMED;
      *outPillNum = (BYTE) (count + 1);
      return TRUE;
    }
  }
  if ((*value)->numPills >= MAX_PILLS) {
    return FALSE;
  }
  count = (*value)->numPills;
  (*value)->item[count] = *item;
  (*value)->active[count] = TRUE;
  (*value)->posStale[count] = PILL_SQUARE_CONFIRMED;
  (*value)->numPills++;
  *outPillNum = (BYTE) (count + 1);
  return TRUE;
}

/*********************************************************
*NAME:          pillsInstallItem
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Writes a pillbox at the number it is given and marks that
*  slot live, whatever the slot held before. A number past
*  the count raises the count to cover it and leaves every
*  slot the gap opens up removed: a number arrives from a
*  list that has already filled it, so the gap is the set of
*  pillboxes this list has not been told about. Returns
*  FALSE for number 0 or a number past MAX_PILLS.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  item    - The pillbox to store
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool pillsInstallItem(pillboxes *value, const pillbox *item, BYTE pillNum) {
  BYTE slot;  /* The array index the number names */
  BYTE count; /* Looping variable */

  if (pillNum == 0 || pillNum > MAX_PILLS) {
    return FALSE;
  }
  slot = (BYTE) (pillNum - 1);
  for (count = (*value)->numPills; count < slot; count++) {
    (*value)->active[count] = FALSE;
  }
  if (pillNum > (*value)->numPills) {
    (*value)->numPills = pillNum;
  }
  (*value)->item[slot] = *item;
  (*value)->active[slot] = TRUE;
  (*value)->posStale[slot] = PILL_SQUARE_CONFIRMED;
  return TRUE;
}

/*********************************************************
*NAME:          pillsRemoveItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Clears a pillbox's live flag. The slot, the count and
*  every pillbox number above it are left alone, so the
*  numbers the wire and the recordings use keep meaning the
*  same pillbox. Returns FALSE for a number out of range or
*  one already removed.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool pillsRemoveItem(pillboxes *value, BYTE pillNum) {
  if (pillNum == 0 || pillNum > (*value)->numPills) {
    return FALSE;
  }
  pillNum--;
  if ((*value)->active[pillNum] == FALSE) {
    return FALSE;
  }
  (*value)->active[pillNum] = FALSE;
  return TRUE;
}

/*********************************************************
*NAME:          pillsIsActive
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Returns whether a pillbox number names a pillbox that is
*  on the map. A removed pillbox keeps its slot and its
*  number, so a number in range is not on its own enough.
*  A number out of range returns FALSE.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*********************************************************/
bool pillsIsActive(pillboxes *value, BYTE pillNum) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (pillNum == 0 || pillNum > (*value)->numPills) {
    return FALSE;
  }
  return ((*value)->active[pillNum - 1] != FALSE);
}

/*********************************************************
*NAME:          pillsSetActive
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Puts a pillbox on the map or takes it off it, leaving its
*  record alone either way. The flag is all that moves, so a
*  pillbox put back is the one the slot already held. Returns
*  FALSE for a number out of range.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number, 1 based
*  onMap   - TRUE for on the map, FALSE for off it
*********************************************************/
bool pillsSetActive(pillboxes *value, BYTE pillNum, bool onMap) {
  if (value == NULL || *value == NULL) {
    return FALSE;
  }
  if (pillNum == 0 || pillNum > (*value)->numPills) {
    return FALSE;
  }
  (*value)->active[pillNum - 1] = onMap ? TRUE : FALSE;
  return TRUE;
}


/* The scan both pillsExistPos and pillsViewExistPos run. skipStale drops a pill
 * whose square this client has not been told is current, which is the
 * difference between the two. A pill this client watched leave its square is
 * dropped either way — there is nothing to draw at a square we know the pill
 * is not on. */
static bool pillsScanExistPos(pillboxes *value, BYTE xValue, BYTE yValue, bool skipStale) {
	bool returnValue; /* Value to return */
	BYTE count;       /* Looping Variable */

	returnValue = FALSE;
	count = 0;
	while (returnValue == FALSE && count < ((*value)->numPills)) {
		/* Does the pill's map coords match what was passed and is it not in a tank? */
		if ((((*value)->active[count]) != FALSE)
		&& (((*value)->item[count].x) == xValue)
		&& (((*value)->item[count].y) == yValue)
		&& (((*value)->item[count].inTank) == FALSE)
		&& (((*value)->posStale[count]) != PILL_SQUARE_MOVED)
		&& (skipStale == FALSE || ((*value)->posStale[count]) == PILL_SQUARE_CONFIRMED)) {
			returnValue = TRUE;
		}
		count++;
	}
	return returnValue;
}

/*********************************************************
*NAME:          pillsExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Returns whether a pillbox exist at a specific location
*  A pill whose square this client has not been told is
*  current does not count as being there, so movement, turn
*  rate and shell collision never meet a pill that is only
*  remembered. pillsViewExistPos is the variant the view
*  builder draws from.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool pillsExistPos(pillboxes *value, BYTE xValue, BYTE yValue) {
	return pillsScanExistPos(value, xValue, yValue, TRUE);
}

/*********************************************************
*NAME:          pillsViewExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  pillsExistPos without the position-current filter: is
*  there a pill here as far as this client last saw. The
*  view builder draws from this, so a pill you have lost
*  sight of stays on screen at the square you last saw it
*  on, rather than the ground underneath showing through.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool pillsViewExistPos(pillboxes *value, BYTE xValue, BYTE yValue) {
	return pillsScanExistPos(value, xValue, yValue, FALSE);
}

/*********************************************************
*NAME:          pillsGetAllianceNum
*AUTHOR:        John Morrison
*CREATION DATE: 21/12/98
*LAST MODIFIED: 16/2/99
*PURPOSE:
*  Returns the alliance type of a pillbox for drawing
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox Number to get 
*********************************************************/
pillAlliance pillsGetAllianceNum(GameSim *sim, pillboxes *value, BYTE pillNum) {
  pillAlliance returnValue; /* Value to return */

  returnValue = pillNeutral;
  pillNum--;
  if ((*value) != NULL) {
    /* A pillbox off the map has no alliance to draw; it reads as neutral,
       which is what the panel shows for a slot the map does not use. */
    if ((pillNum) < ((*value)->numPills) && (*value)->active[pillNum] != FALSE) {
      if ((*value)->item[pillNum].armour == 0 && (*value)->item[pillNum].inTank == FALSE) {
        returnValue = pillDead;
      } else if ((*value)->item[pillNum].owner == sim->viewPlayer) {
        if ((*value)->item[pillNum].inTank == TRUE) {
          returnValue = pillTankGood;
        } else {
          returnValue = pillGood;
        }
      } else if (playersIsAllie(&sim->plyrs, (*value)->item[pillNum].owner, sim->viewPlayer) == TRUE) {
        if ((*value)->item[pillNum].inTank == TRUE) {
          returnValue = pillTankAllie;
        } else {
          returnValue = pillAllie;
        }
      } else if ((*value)->item[pillNum].owner != NEUTRAL) {
        if ((*value)->item[pillNum].inTank == TRUE) {
          returnValue = pillTankEvil;
        } else {
          returnValue = pillEvil;
        }
      }
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          pillsUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 30/10/99
*PURPOSE:
*  Updates each pillboxs reload time and fires if the
*  tank is in range
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  mp    - Pointer to the map structure
*  bs    - Pointer to the bases structure
*  tnk   - Tank Structure
*  shs   - Shells Structure
*********************************************************/
void pillsUpdate(GameSim *sim, tank tanks[], bool *connected, BYTE numTanks) {
  pillboxes *value = &sim->pb;
  map *mp = &sim->mp;
  bases *bs = &sim->bs;
  players *plyrs = &sim->plyrs;
  shells *shs = &sim->shs;
  WORLD x;         /* X and Y co-ords of the pillbox */
  WORLD y;
  WORLD tankX;     /* X and Y co-ords of the tank */
  WORLD tankY;
  WORLD diffX;     /* X and Y differences in distances */
  WORLD diffY;
  TURNTYPE dir;    /* Direction Pillbox should fire */
  BYTE count;      /* Looping variable */
  BYTE t;          /* Tank looping variable */
  double amount;   /* Distance of player away from pill */

  /* Best target tracking */
  double bestDist;
  WORLD bestTankX, bestTankY;
  BYTE bestTankDir, bestTankSpeed;
  tank *bestTank;
  BYTE bestTankNum;
  bool foundTarget;

  /* pill_shell_cap: the pill shells already in the air at each tank,
   * counted once here rather than kept as a running total, so no path that
   * ends a shell can leave a count behind. A shell marked dead is gone for
   * this purpose even though the list still holds it until the next update.
   * Every shell fired below adds to the count, so the pills that fire in this
   * pass count against each other. */
  uint16_t shellsAtTank[MAX_TANKS];
  const bool shellCapOn = sim->rules.pill_shell_cap != 0;
  bool heldByCap;

  if (shellCapOn) {
    shells q;

    memset(shellsAtTank, 0, sizeof(shellsAtTank));
    for (q = *shs; q != NULL; q = q->next) {
      if (q->shellDead == FALSE && q->target < MAX_TANKS) {
        shellsAtTank[q->target]++;
      }
    }
  }

  for (count=0;count<(*value)->numPills;count++) {
    if ((*value)->active[count] == FALSE) {
      continue;
    }
    /* Set world Co-ords for the Pillbox */
    x = (*value)->item[count].x;
    x <<= TANK_SHIFT_MAPSIZE;
    x += MAP_SQUARE_MIDDLE;
    y = (*value)->item[count].y;
    y <<= TANK_SHIFT_MAPSIZE;
    y += MAP_SQUARE_MIDDLE;

    /* Update the reload time */
    if ((*value)->item[count].reload < (*value)->item[count].speed) {
      (*value)->item[count].reload++;
    }
    /* Adjust the cool down time */
    if ((*value)->item[count].coolDown > 0) {
      (*value)->item[count].coolDown--;
      if ((*value)->item[count].coolDown ==0) {
        (*value)->item[count].speed++;
        if ((*value)->item[count].speed < sim->rules.pill_attack_ticks) {
          (*value)->item[count].coolDown = (BYTE) sim->rules.pill_cooldown_ticks;
        }
      }
    }
    /* Check to see if it should fire ie Is alive, not reloading, not in tank */
    if ((*value)->item[count].armour > 0 && (*value)->item[count].reload >= (*value)->item[count].speed && (*value)->item[count].inTank == FALSE) {
      /* Find the closest non-allied tank in range */
      bestDist = 999999.0;
      bestTank = NULL;
      bestTankNum = NEUTRAL;
      foundTarget = FALSE;
      heldByCap = FALSE;
      bestTankX = 0;
      bestTankY = 0;
      bestTankDir = 0;
      bestTankSpeed = 0;

      for (t = 0; t < numTanks; t++) {
        if (!connected[t] || tanks[t] == NULL) continue;
        /* Skip allied tanks */
        if (playersIsAllie(plyrs, (*value)->item[count].owner, t) == TRUE || (*value)->item[count].owner == t) continue;

        tankGetWorld(&tanks[t], &tankX, &tankY);
        /* Skip dead tanks */
        if (tankIsDestroyed(&tanks[t])) continue;

        if (tankX > x) {
          diffX = tankX - x;
        } else {
          diffX = x - tankX;
        }
        if (tankY > y) {
          diffY = tankY - y;
        } else {
          diffY = y - tankY;
        }

        /* Check visibility: not hidden in trees (unless very close or just fired) */
        if ((utilIsTankInTrees(mp, value, bs, tankX, tankY)) == TRUE && (diffX >= sim->rules.tree_hide_distance ||
             diffY >= sim->rules.tree_hide_distance) && tankJustFired(&tanks[t]) == FALSE) {
          continue;
        }

        if ((utilIsItemInRange(x, y, tankX, tankY, (WORLD) sim->rules.pill_range, &amount)) == TRUE) {
          /* A tank with as many pill shells coming as the rule allows is
           * passed over for the next nearest. Checked only once the tank
           * has passed every other test, so heldByCap means a target the
           * pill could otherwise shoot: one that leaves range, hides or
           * dies clears justSeen below the same as it always has. */
          if (shellCapOn && t < MAX_TANKS &&
              shellsAtTank[t] >= sim->rules.pill_max_shells_at_tank) {
            heldByCap = TRUE;
            continue;
          }
          if (amount < bestDist) {
            bestDist = amount;
            bestTankX = tankX;
            bestTankY = tankY;
            bestTankDir = tankGetTravelAngel(&tanks[t]);
            bestTankSpeed = tankGetSpeed(&tanks[t]);
            bestTank = &tanks[t];
            bestTankNum = t;
            foundTarget = TRUE;
          }
        }
      }

      if (foundTarget) {
        /* Fire at closest enemy tank */
        if ((*value)->item[count].justSeen == TRUE) {
          dir = pillsTargetTank(sim, mp, value, bs, x, y, bestTankX, bestTankY, (TURNTYPE) bestTankDir, bestTankSpeed, (tankIsOnBoat(bestTank)), tankBoatExitSpeed(sim, *bestTank), tankIsObstructed(bestTank));
          shellsAddItem(sim, shs, x, y, dir, sim->rules.pill_fire_length, NEUTRAL, bestTankNum, count, FALSE);
          if (shellCapOn && bestTankNum < MAX_TANKS) {
            shellsAtTank[bestTankNum]++;
          }
          (*value)->item[count].reload = 0;
          sim->callbacks.soundDist(sim->callbacks.ctx, shootNear, (*value)->item[count].x, (*value)->item[count].y);
        } else {
          /* They were just seen */
          (*value)->item[count].justSeen = TRUE;
          (*value)->item[count].reload = 0;
        }
      } else if (heldByCap == FALSE) {
        (*value)->item[count].justSeen = FALSE;
      }
      /* Held only by the cap: justSeen and the full reload are kept, so the
       * pill fires on the first update a shell at its target comes free. */
    }
  }
}

/*********************************************************
*NAME:          pillsIsPillHit
*AUTHOR:        John Morrison
*CREATION DATE: 30/10/98
*LAST MODIFIED: 19/3/98
*PURPOSE:
*  Returns whether a pillbox is hit at a specific location
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
bool pillsIsPillHit(pillboxes *value, BYTE xValue, BYTE yValue) {
  return pillsHitSlot(value, xValue, yValue) != DMG_NO_PILL;
}

/*********************************************************
*NAME:          pillsHitSlot
*PURPOSE:
*  The pill index of the pillbox a shell at this square
*  would hit — a live one on the map — or DMG_NO_PILL for a
*  square with none.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE pillsHitSlot(pillboxes *value, BYTE xValue, BYTE yValue) {
  BYTE count;       /* Looping Variable */

  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && ((*value)->item[count].armour >0) && (*value)->item[count].inTank == FALSE) {
      /* Pillbox has been Hit */
      return count;
    }
    count++;
  }

  return DMG_NO_PILL;
}

/* What a blow takes off a pill once the host has priced it, as a percent of
   the classic amount. One rounding, half up, so a hundred leaves the classic
   number exactly; capped at a byte, which is more than any pill can hold. */
static BYTE pillsScaledDamage(GameSim *sim, int base, BYTE attacker, BYTE index,
                              BYTE cause, BYTE pill) {
  int64_t amount;

  amount = ((int64_t) base *
            gameSimPillDamageScale(sim, attacker, index, cause, pill) + 50) /
           100;
  if (amount > 255) {
    amount = 255;
  }
  return (BYTE) amount;
}

/*********************************************************
*NAME:          pillsDamagePos
*AUTHOR:        John Morrison
*CREATION DATE: 18/3/98
*LAST MODIFIED: 24/4/01
*PURPOSE:
*  Does damage to a pillbox at xValue, yValue.
*  Returns if the pill is dead
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  xValue     - X Location
*  yValue     - Y Location
*  wantDamage - TRUE if we just want to do damage to it
*  wantAngry  - TRUE if we just want to make it angry
*  owner      - Who fired the shell, NEUTRAL for a pillbox
*  pill       - The pill index of the pillbox that fired
*               it, DMG_NO_PILL for a tank's shell
*********************************************************/
bool pillsDamagePos(GameSim *sim, BYTE xValue, BYTE yValue, bool wantDamage, bool wantAngry, BYTE owner, BYTE pill) {
  pillboxes *value = &sim->pb;
  bool isServer = sim->isServer;
  bool returnValue;  /* Value to return */
  bool done;         /* Loop guard */
  BYTE count;        /* Looping Variable */

  returnValue = FALSE;
  done = FALSE;
  count = 0;
  while (done == FALSE && count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && ((*value)->item[count].armour >0) && (*value)->item[count].inTank == FALSE) {
      /* Pillbox has been Hit */
      done = TRUE;
      BYTE before = (*value)->item[count].armour;  /* > 0 here */
      if (wantDamage == TRUE && (*value)->item[count].armour > 0) {
        /* What the shell takes, as the host prices it. A price of nothing
           still stops the shell and still angers the pill below: only the
           armour lost is the host's to scale. */
        BYTE damage = pillsScaledDamage(sim, sim->rules.pill_shell_damage,
                                        owner, count, DMG_SRC_SHELL, pill);
        /* Ask whether the shell takes more than is left rather than
           subtracting first and reading the wrap: what a shell takes off a
           pill and what a pill may hold are two rules now, and a table may
           put any pair of numbers here. */
        if (damage > before) {
          (*value)->item[count].armour = 0;
        } else {
          (*value)->item[count].armour = (BYTE) (before - damage);
        }
        /* The blow that would finish the pill is the host's to refuse, and a
           refusal holds it at one armour, where it goes on firing. Taken back
           before the damage is recorded, so the record says what the pill
           actually lost. */
        if ((*value)->item[count].armour == 0 &&
            gameSimCanDie(sim, DIE_KIND_PILL, count, owner,
                          DMG_SRC_SHELL, pill) == FALSE) {
          (*value)->item[count].armour = 1;
        }
      }
      if (wantDamage == TRUE && sim->callbacks.recordDamage) {
        BYTE after = (*value)->item[count].armour;
        bool destroyed = (after == 0);
        sim->callbacks.recordDamage(sim->callbacks.ctx, owner, DMG_TARGET_PILL,
                                    count, DMG_SRC_SHELL,
                                    (uint16_t)(before - after), destroyed,
                                    (*value)->item[count].x, (*value)->item[count].y);
      }
      logAddEvent(log_PillSetHealth, count, (*value)->item[count].armour, 0, 0, 0, NULL);
      if ((*value)->item[count].armour == 0) {
        returnValue = TRUE;
        /* The entry test above required armour > 0, so reaching zero here is
           always this blow's doing. count is the 0-based item[] slot. */
        if (sim->isServer && sim->callbacks.pillKilled) {
          sim->callbacks.pillKilled(sim->callbacks.ctx, count, owner);
        }
        if (isServer == FALSE) {
          frontEndStatusPillbox(clientSimFromSim(sim), (BYTE) (count+1), pillDead);
        }
      } else if (wantDamage == TRUE) {
        (*value)->item[count].coolDown = (BYTE) sim->rules.pill_cooldown_ticks;
        if ((*value)->item[count].speed > sim->rules.pill_attack_min_ticks) {
          (*value)->item[count].speed /= (BYTE) sim->rules.pill_angry_divisor;
          if ((*value)->item[count].speed < sim->rules.pill_attack_min_ticks) {
            (*value)->item[count].speed = (BYTE) sim->rules.pill_attack_min_ticks;
          }
        }
      }
    }
    count++;
  }

  return returnValue;
}

/* The sixteen pictures a pillbox is drawn with, worst first: entry 0 is the
 * empty pill and entry 15 the intact one. Two arrays because a pill an ally
 * owns is drawn differently from one an enemy does, and each is written out
 * rather than computed because PILL_EVIL_15 sits away from the other fifteen
 * evil tiles in the tile numbering. */
static const BYTE pillEvilTiles[PILLS_MAX_ARMOUR + 1] = {
  PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
  PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
  PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
  PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
};
static const BYTE pillGoodTiles[PILLS_MAX_ARMOUR + 1] = {
  PILL_GOOD_0,  PILL_GOOD_1,  PILL_GOOD_2,  PILL_GOOD_3,
  PILL_GOOD_4,  PILL_GOOD_5,  PILL_GOOD_6,  PILL_GOOD_7,
  PILL_GOOD_8,  PILL_GOOD_9,  PILL_GOOD_10, PILL_GOOD_11,
  PILL_GOOD_12, PILL_GOOD_13, PILL_GOOD_14, PILL_GOOD_15
};

/*********************************************************
*NAME:          pillsArmourTile
*PURPOSE:
*  Which of the sixteen pillbox pictures an armour value is
*  drawn as.
*
*  There are sixteen pictures and pill_max_armour is a rule a
*  scenario can set as high as 255, so the armour is scaled
*  onto the pictures rather than used as an index into them.
*  Armour 0 draws the empty pill, armour at the cap draws the
*  intact one, and everything between lands in proportion.
*  Before this, anything above 15 fell through to the empty
*  tile, so on a sim with a raised cap every pill on the map
*  drew as a wreck until somebody shot it down to 15.
*
*  Integer arithmetic, rounded down. Rounding down keeps the
*  classic cap exact — armour times 15 over 15 is the armour
*  itself, so each of the sixteen armour values still has its
*  own picture — and means a pill that has taken any damage at
*  all stops drawing as the intact one however high the cap
*  goes.
*
*ARGUMENTS:
*  sim    - The sim whose rules table holds the cap
*  armour - The pillbox's armour
*  allied - Whether the viewer is allied to the pill's owner
*********************************************************/
static BYTE pillsArmourTile(GameSim *sim, BYTE armour, bool allied) {
  int32_t maxArmour = (sim != NULL) ? sim->rules.pill_max_armour
                                    : PILLS_MAX_ARMOUR;
  int32_t level;

  /* The rules check holds the cap at 1 or above; a table that never went
     through it would divide by zero here. */
  if (maxArmour < 1) {
    maxArmour = PILLS_MAX_ARMOUR;
  }
  if ((int32_t)armour >= maxArmour) {
    level = PILLS_MAX_ARMOUR;
  } else {
    level = ((int32_t)armour * PILLS_MAX_ARMOUR) / maxArmour;
  }
  return allied ? pillGoodTiles[level] : pillEvilTiles[level];
}

/*********************************************************
*NAME:          pillsGetScreenHealth
*AUTHOR:        John Morrison
*CREATION DATE: 30/10/98
*LAST MODIFIED: 30/10/98
*PURPOSE:
*  Returns the health of a pillbox as a screen define.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location
*  yValue - Y Location
*********************************************************/
BYTE pillsGetScreenHealth(GameSim *sim, pillboxes *value, BYTE xValue, BYTE yValue, BYTE viewPlayer) {
  bool done;        /* Finished searching */
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  done = FALSE;
  count = 0;
  returnValue = PILL_EVIL_15;

  while (done == FALSE && count < ((*value)->numPills)) {
    /* The same skip pillsViewExistPos makes, which is asked first: two pills
       on one square must not answer differently about which of them is there. */
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && (*value)->item[count].inTank == FALSE
        && ((*value)->posStale[count]) != PILL_SQUARE_MOVED) {
      /* Pillbox has been Hit */
      done = TRUE;

      returnValue = pillsArmourTile(
          sim, (*value)->item[count].armour,
          playersIsAllie(&sim->plyrs, (*value)->item[count].owner,
                         viewPlayer) != FALSE);

    }
    count++;
  }

  return returnValue;
}

/* Mac Bolo 0.99.7's quarter-wave table: trunc(128*sin(b*pi/128)),
 * clipped to 127. Used by both its lead vector and its integer aim search.
 * Reference: mac_bolo/goport/{trig/trig.go,sim/pills.go,sim/math.go,
 * sim/shells.go}, verified there against the original disassembly. */
static const BYTE pillMacSine[65] = {
  0, 3, 6, 9, 12, 15, 18, 21, 24, 28, 31, 34, 37, 40, 43, 46,
  48, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
  90, 92, 94, 96, 98, 100, 102, 104, 106, 108, 109, 111, 112, 114,
  115, 117, 118, 119, 120, 121, 122, 123, 124, 124, 125, 126,
  126, 127, 127, 127, 127, 127, 127
};

static int pillsMacSin(int angle) {
  int half = angle & 127;
  int value = pillMacSine[half > 64 ? 128 - half : half];
  return (angle & 128) ? -value : value;
}

/* Arithmetic right shift, including floor rounding for negative products. */
static int32_t pillsMacShift(int32_t value, int bits) {
  int32_t divisor = (int32_t)1 << bits;
  return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

static int32_t pillsMacPixelDelta(WORLD a, WORLD b) {
  int32_t delta = (uint16_t)(a - b);
  if (delta >= 32768) delta -= 65536;
  return pillsMacShift(delta, 4);
}

static int pillsMacRange(WORLD px, WORLD py, WORLD tx, WORLD ty) {
  int32_t dx = pillsMacPixelDelta(px, tx);
  int32_t dy = pillsMacPixelDelta(py, ty);
  int32_t squared = dx * dx + dy * dy;
  /* Equivalent to Mac's floor(sqrt(256*i)) lookup table and its three
   * buckets. The input to sqrt is an exact integer no larger than 65280. */
  if (squared > 0xFFFFF) return 0x7FFF;
  if (squared > 0xFFFF) return (int)sqrt((double)((squared >> 12) * 256)) * 4;
  if (squared > 0xFFF) return (int)sqrt((double)((squared >> 8) * 256));
  return (int)sqrt((double)((squared >> 4) * 256)) / 4;
}

static TURNTYPE pillsMacAim(int32_t x, int32_t y) {
  bool negativeX = x < 0, negativeY = y < 0;
  int angle, delta;
  if (negativeX) x = -x;
  if (negativeY) y = -y;
  angle = x < y ? 16 : 48;
  for (delta = 8; delta != 0; delta >>= 1) {
    if (y * pillMacSine[angle] - x * pillMacSine[64 - angle] < 0) {
      angle += delta;
    } else {
      angle -= delta;
    }
  }
  if (!negativeY) angle = -128 - angle;
  if (negativeX) angle = -angle;
  return (TURNTYPE)(uint8_t)angle;
}

static TURNTYPE pillsTargetTankMac(WORLD px, WORLD py, WORLD tx, WORLD ty,
                                  TURNTYPE heading, BYTE speed, bool obstructed) {
  int distance = pillsMacRange(px, py, tx, ty);
  int direction = ((int)(heading + 8) & 255) & 240;
  /* WinBolo moves speed WU per 20 ms; Mac moves speed*127/256 WU
   * per 40 ms. Its road speed 64 corresponds to WinBolo's 16. Preserve
   * the Mac byte's range for scenarios with unusually high speeds. */
  int macSpeed = speed > 63 ? 255 : speed * 4;
  uint16_t lead = obstructed ? 0 : (uint16_t)((distance - 16) * macSpeed) >> 2;
  /* Keep these signed and wider than WORLD: wrapping the distant aim point
   * at a map edge would reverse shots. The original only wraps the lead. */
  int32_t x = (int32_t)tx - pillsMacShift(lead * -pillsMacSin(direction), 8) - px;
  int32_t y = (int32_t)ty - pillsMacShift(lead * pillsMacSin(direction + 64), 8) - py;
  return pillsMacAim(x, y);
}

/*********************************************************
*NAME:          pillsTargetTank
*AUTHOR:        John Morrison
*CREATION DATE:  1/12/98
*LAST MODIFIED: 17/1/99
*PURPOSE:
*  Returns the angle need to fire to hit a tank
*
*ARGUMENTS:
*  mp     - Pointer to the map structure
*  pb     - Pointer to the pills structure
*  bs     - Pointer to the bases structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*  tankX  - X Location of tank
*  tankY  - Y Location of tank
*  angle  - Angle of the tank
*  speed  - The speed of the tank
*  onBoat - Is the tank on a boat
*  boatExitSpeed - Speed at which that tank leaves a boat
*  obstructed - Whether the target's last move was blocked
*********************************************************/
TURNTYPE pillsTargetTank(GameSim *sim, map *mp, pillboxes *pb, bases *bs, WORLD xValue, WORLD yValue, WORLD tankX, WORLD tankY, TURNTYPE angle, BYTE speed, bool onBoat, BYTE boatExitSpeed, bool obstructed) {
  TURNTYPE returnValue; /* Value to return */

  if (sim->rules.pill_aim_mac != 0) {
    return pillsTargetTankMac(xValue, yValue, tankX, tankY, angle, speed, obstructed);
  }
  if (speed == 0) {
    returnValue = utilCalcAngle(xValue, yValue, tankX, tankY);
  } else if (sim->rules.pill_massage_range <= 0) {
    returnValue = pillsTargetTankMove(sim, mp, pb, bs, xValue, yValue, tankX, tankY, angle, speed, onBoat, boatExitSpeed);
  } else {
    /* The "pillmassage" aim. Inside pill_massage_range the forward
       prediction the original formula makes (tank_steps = speed *
       (dist-16) >> 2) is wide enough of the tank that it can circle the
       pillbox without being hit. It only applies to a tank sliding past:
       one driving at or away from the pillbox is led by the solver as
       usual. Beyond that range every tank is. */
    long diffX = (long)tankX - (long)xValue;
    long diffY = (long)tankY - (long)yValue;
    double dist = sqrt((double)(diffX * diffX + diffY * diffY));

    if (dist < (double)sim->rules.pill_massage_range) {
      int tankDirX, tankDirY;
      utilCalcDistance(&tankDirX, &tankDirY, angle, speed);

      /* Dot product of tank velocity with tank-to-pill vector.
         If the tank is driving straight at (or away from) the pill
         the dot product magnitude is large. */
      double dot = (double)tankDirX * (-diffX) + (double)tankDirY * (-diffY);
      double dirMag = sqrt((double)(tankDirX * tankDirX + tankDirY * tankDirY));
      double cosAngle = (dirMag > 0.0 && dist > 0.0) ? dot / (dirMag * dist) : 1.0;

      if (fabs(cosAngle) > (double)sim->rules.pill_massage_cosine) {
        returnValue = pillsTargetTankMove(sim, mp, pb, bs, xValue, yValue, tankX, tankY, angle, speed, onBoat, boatExitSpeed);
      } else {
        long tank_steps = ((long)speed * ((long)(dist + 0.5) - 16)) >> 2;
        long predictedX = (long)tankX + tank_steps * (long)tankDirX;
        long predictedY = (long)tankY + tank_steps * (long)tankDirY;
        returnValue = utilCalcAngle(xValue, yValue, (WORLD)predictedX, (WORLD)predictedY);
      }
    } else {
      returnValue = pillsTargetTankMove(sim, mp, pb, bs, xValue, yValue, tankX, tankY, angle, speed, onBoat, boatExitSpeed);
    }
  }

  return returnValue;
}

/*********************************************************
*NAME:          pillsTargetTankMove
*AUTHOR:        John Morrison
*CREATION DATE: 31/12/98
*LAST MODIFIED: 11/02/09
*PURPOSE:
*  Returns the angle need to fire to hit a moving tank
*
*ARGUMENTS:
*  mp     - Pointer to the map structyre
*  pb     - Pointer to the pillbox structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*  tankX  - X Location of tank
*  tankY  - Y Location of tank
*  angle  - Angle of the tank
*  speed  - The speed of the tank
*  onBoat - Is the tank on a boat
*  boatExitSpeed - Speed at which that tank leaves a boat
*********************************************************/
TURNTYPE pillsTargetTankMove(GameSim *sim, map *mp, pillboxes *pb, bases *bs, WORLD xValue, WORLD yValue, WORLD tankX, WORLD tankY, TURNTYPE angle, BYTE speed, bool onBoat, BYTE boatExitSpeed) {
  TURNTYPE returnValue; /* Value to return */
  TURNTYPE estimate;    /* Current Estimate */
  BYTE count;           /* Looping Variable */
  bool found;           /* Finished looping */
  int tankAddX;         /* Tank add X */
  int tankAddY;         /* Tank add Y */
  WORLD tankTestAddX;   /* Check for tank colisions */
  WORLD tankTestAddY;
  BYTE bmx;
  BYTE bmy;
  BYTE newbmx;
  BYTE newbmy;
  int shellAddX;        /* Tank add X */
  int shellAddY;        /* Tank add Y */
  WORLD shellX;         /* Shell X location */
  WORLD shellY;         /* Shell Y locatiopn */
  bool isLand;          /* Is this square land */

  found = FALSE;
  count = 1;
  shellX = xValue;
  shellY = yValue;

  returnValue = 0;

  /* Get tank Add amounts */
  utilCalcDistance(&tankAddX, &tankAddY, angle, speed);
    
  /* Get initial estimate */
  estimate = utilCalcAngle(xValue, yValue, tankX,tankY);
  /* Calculate distance */
  utilCalcDistance(&shellAddX, &shellAddY, estimate, sim->rules.shell_speed);
  shellX = (WORLD) (xValue + shellAddX);
  shellY = (WORLD) (yValue + shellAddY);
  
  while (found == FALSE && count < sim->rules.pill_aim_iterations) {
    if ((utilIsTankHit(tankX, tankY, angle, shellX, shellY, estimate,
                       (WORLD) sim->rules.tank_hit_radius)) == TRUE  ) {
      found = TRUE;
      returnValue = estimate;
    }
    count++;
    
    tankTestAddX = (WORLD) (tankX + tankAddX);
    tankTestAddY = (WORLD) (tankY + tankAddY);
    if (tankAddX >= 0) {
      tankTestAddX += TANK_SUBTRACT;
    } else {
      tankTestAddX -= TANK_SUBTRACT;
    }

    if (tankAddY >= 0) {
      tankTestAddY += TANK_SUBTRACT;
    } else {
      tankTestAddY -= TANK_SUBTRACT;
    }

    tankTestAddX >>= TANK_SHIFT_MAPSIZE;
    tankTestAddY >>= TANK_SHIFT_MAPSIZE;
    newbmx = (BYTE) tankTestAddX;
    newbmy = (BYTE) tankTestAddY;

    tankTestAddX = tankX;
    tankTestAddY = tankY;
    tankTestAddX >>= TANK_SHIFT_MAPSIZE;
    tankTestAddY >>= TANK_SHIFT_MAPSIZE;
    bmx = (BYTE) tankTestAddX;
    bmy = (BYTE) tankTestAddY;

    isLand = mapIsLand(mp, pb,bs, bmx, newbmy);
    if (mapGetSpeed(sim,mp,pb,bs,bmx,newbmy, onBoat, sim->viewPlayer) > 0 && (onBoat == FALSE || (onBoat == TRUE && isLand == FALSE) || (onBoat == TRUE && isLand == TRUE && speed >= boatExitSpeed))) {
      tankY = (WORLD) (tankY + tankAddY);
    }

    isLand = mapIsLand(mp, pb,bs, newbmx, bmy);
    if (mapGetSpeed(sim,mp,pb,bs,newbmx,bmy, onBoat, sim->viewPlayer) > 0 && (onBoat == FALSE || (onBoat == TRUE && isLand == FALSE) || (onBoat == TRUE && isLand == TRUE && speed >= boatExitSpeed))) {
      tankX = (WORLD) (tankX + tankAddX);
    }
    
    estimate = utilCalcAngle(xValue, yValue, tankX,tankY);
    utilCalcDistance(&shellAddX, &shellAddY, estimate, sim->rules.shell_speed);    
    shellX = (WORLD) (xValue + ((count+  5) * shellAddX));
    shellY = (WORLD) (yValue + ((count +  5) * shellAddY));
    /* Set it anyway */
    returnValue = estimate;
  }

  return returnValue;
} 

/*********************************************************
*NAME:          pillsDeadPos
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Returns whether a pillbox a specific location is dead
*  or not. A pill whose square this client has not been
*  told is current does not count as being there.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*********************************************************/
bool pillsDeadPos(pillboxes *value, BYTE xValue, BYTE yValue) {
	bool returnValue; /* Value to return */
	BYTE count;       /* Looping Variable */

	returnValue = FALSE;
	count = 0;
	while (returnValue == FALSE && count < ((*value)->numPills)) {
		/* Do the pill's map coords match what was passed and does it have zero armour and is it not in a tank?
		   A pill whose square we have not been told is current answers no, the same as in pillsExistPos —
		   every caller of this one is gameplay, so there is no view variant to pair with it. */
		if ((((*value)->active[count]) != FALSE)
		&& (((*value)->item[count].x) == xValue)
		&& (((*value)->item[count].y) == yValue)
		&& (((*value)->item[count].armour == 0))
		&& (((*value)->item[count].inTank == FALSE))
		&& (((*value)->posStale[count]) == 0))
		{
			returnValue = TRUE;
		}
		count++;
	}
	return returnValue;
}

/* The scan both pillsGetPillNum and pillsGetViewPillNum run. skipStale drops a
 * pill whose square this client has not been told is current, which is the
 * difference between the two. */
static BYTE pillsScanPillNum(pillboxes *value, BYTE xValue, BYTE yValue, bool careInTank, bool inTank, bool skipStale) {
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  returnValue = PILL_NOT_FOUND-1;
  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue
        && (skipStale == FALSE || ((*value)->posStale[count]) == 0)) {
      if (careInTank == FALSE || ((*value)->item[count].inTank == inTank)) {
        returnValue = (BYTE) (count+1);
        count = (*value)->numPills;
      }
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          pillsGetPillNum
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 14/9/00
*PURPOSE:
*  Returns the pill number of a pillbox at that location
*  If not found returns PILL_NOT_FOUND
*  A pill whose square this client has not been told is
*  current is passed over. pillsGetViewPillNum is the
*  variant the camera and the displays use.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  xValue     - X Location of pillbox
*  yValue     - Y Location of pillbox
*  careInTank - Whether we are about the in tank state
*  inTank     - The intank state to check if we care
*********************************************************/
BYTE pillsGetPillNum(pillboxes *value, BYTE xValue, BYTE yValue, bool careInTank, bool inTank) {
  return pillsScanPillNum(value, xValue, yValue, careInTank, inTank, TRUE);
}

/*********************************************************
*NAME:          pillsGetViewPillNum
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  pillsGetPillNum without the position-current filter:
*  which pill this client last saw at that square, whether
*  or not the server has confirmed it is still there. The
*  camera and the displays use this so a pill you have lost
*  sight of stays selectable and stays drawn where you last
*  saw it.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  xValue     - X Location of pillbox
*  yValue     - Y Location of pillbox
*  careInTank - Whether we are about the in tank state
*  inTank     - The intank state to check if we care
*********************************************************/
BYTE pillsGetViewPillNum(pillboxes *value, BYTE xValue, BYTE yValue, bool careInTank, bool inTank) {
  return pillsScanPillNum(value, xValue, yValue, careInTank, inTank, FALSE);
}

/*********************************************************
*NAME:          pillsSetPosState
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Records what this client knows about a pill's square:
*  one of PILL_SQUARE_CONFIRMED, PILL_SQUARE_REMEMBERED or
*  PILL_SQUARE_MOVED.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox index, 0 based
*  state   - One of the PILL_SQUARE_ values
*********************************************************/
void pillsSetPosState(pillboxes *value, BYTE pillNum, BYTE state) {
  if (pillNum < MAX_PILLS) {
    (*value)->posStale[pillNum] = state;
  }
}

/*********************************************************
*NAME:          pillsGetPosState
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Returns what this client knows about a pill's square, as
*  one of the PILL_SQUARE_ values.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox index, 0 based
*********************************************************/
BYTE pillsGetPosState(pillboxes *value, BYTE pillNum) {
  if (pillNum < MAX_PILLS) {
    return (*value)->posStale[pillNum];
  }
  return PILL_SQUARE_CONFIRMED;
}

/*********************************************************
*NAME:          pillsUpdatePosState
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Folds one arriving pill update into that pill's square
*  state. Call it before writing the new inTank, so the
*  in-tank flag it reads is the one this client held.
*
*ARGUMENTS:
*  value      - Pointer to the pillbox structure
*  pillNum    - Pillbox index, 0 based
*  posCurrent - The position-current bit off the wire
*  nowInTank  - The in-tank flag that arrived
*********************************************************/
void pillsUpdatePosState(pillboxes *value, BYTE pillNum, bool posCurrent,
                         bool nowInTank) {
  bool wasInTank; /* The in-tank flag this client held */

  if (pillNum >= MAX_PILLS) {
    return;
  }

  wasInTank = (*value)->item[pillNum].inTank;
  if (posCurrent == TRUE) {
    /* The square arrived with the update, so it is where the pill is —
       whatever we believed about it before. */
    (*value)->posStale[pillNum] = PILL_SQUARE_CONFIRMED;
  } else if (wasInTank == TRUE && nowInTank == FALSE) {
    /* It has been put down, and we were not told where. The square we hold is
       the one it was picked up from, which it is no longer on. */
    (*value)->posStale[pillNum] = PILL_SQUARE_MOVED;
  } else if ((*value)->posStale[pillNum] != PILL_SQUARE_MOVED) {
    /* Nothing new about the square. Only a confirmed one clears the moved
       state, so an unconfirmed update cannot talk us back into drawing it. */
    (*value)->posStale[pillNum] = PILL_SQUARE_REMEMBERED;
  }
}

/*********************************************************
*NAME:          pillsIsPosStale
*AUTHOR:        John Morrison
*CREATION DATE: 5/9/26
*LAST MODIFIED: 5/9/26
*PURPOSE:
*  Returns whether a pill's square is one this client has
*  not been told is current — remembered or moved, as
*  against confirmed.
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - Pillbox index, 0 based
*********************************************************/
bool pillsIsPosStale(pillboxes *value, BYTE pillNum) {
  if (pillNum < MAX_PILLS) {
    return (*value)->posStale[pillNum] != PILL_SQUARE_CONFIRMED;
  }
  return FALSE;
}

/*********************************************************
*NAME:          pillsSetPillInTank
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Returns the pill number of a pillbox at that location
*  If not found returns PILL_NOT_FOUND
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number to apply it to
*  inTank  - Is it in tank or not
*********************************************************/
void pillsSetPillInTank(pillboxes *value, BYTE pillNum, bool inTank) {
  if (pillNum > 0 && pillNum<= (*value)->numPills) {
    pillNum--;
    (*value)->item[pillNum].inTank = inTank;
    logAddEvent(log_PillSetInTank, utilPutNibble(pillNum, inTank), 0, 0, 0, 0, NULL);

  }
}

/*********************************************************
*NAME:          pillsGetPillOwner
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Returns the owner of the pill.
*  If not found returns PILL_NOT_FOUND
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number to apply it to
*********************************************************/
BYTE pillsGetPillOwner(pillboxes *value, BYTE pillNum) {
  BYTE returnValue; /* Value to return */

  returnValue = PILL_NOT_FOUND;
  /* A removed pill answers the same as one off the end of the list: there is
     no such pill, so it has no owner. */
  if (pillNum > 0 && pillNum<= (*value)->numPills &&
      (*value)->active[pillNum - 1] != FALSE) {
    pillNum--;
    returnValue = (*value)->item[pillNum].owner;
  }
  return returnValue;
}

/*********************************************************
*NAME:          pillsSetPillOwner
*AUTHOR:        John Morrison
*CREATION DATE: 15/01/99
*LAST MODIFIED: 04/04/02
*PURPOSE:
* Sets the pillbox pillNum to owner. Returns the previous
* owner. If migrate is set to TRUE then it has migrated
* from a alliance when a player left and nothing is
* reported at all
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  pillNum - The pillbox number to apply it to
*  owner   - The new owner
*  migrate - TRUE if it is migrating.
*********************************************************/
BYTE pillsSetPillOwner(GameSim *sim, pillboxes *value, BYTE pillNum, BYTE owner, bool migrate) {
  BYTE returnValue; /* Value to return */

  returnValue = NEUTRAL;
  if (pillNum > 0 && pillNum<= (*value)->numPills &&
      (*value)->active[pillNum - 1] != FALSE) {
    pillNum--;
    returnValue = (*value)->item[pillNum].owner;
    (*value)->item[pillNum].owner = owner;
    /* Report the change. This is the only thing said about it: no newswire
       line is written here, so every client — the hosting one included —
       draws its line from the event, which carries the announce policy's
       answer. A pill going neutral is reported the same way, and the client
       draws no line for one. */
    if (migrate == FALSE && sim->isServer) {
      if (sim->callbacks.pillOwnerChanged) {
        BYTE captureClass;
        captureClass = (returnValue == NEUTRAL)                                ? CAPTURE_CLASS_NEUTRAL
                     : (playersIsAllie(&sim->plyrs, owner, returnValue) == FALSE) ? CAPTURE_CLASS_ENEMY
                     :                                                           CAPTURE_CLASS_ALLY;
        sim->callbacks.pillOwnerChanged(sim->callbacks.ctx, pillNum,
                                        returnValue, owner, captureClass,
                                        (*value)->item[pillNum].x,
                                        (*value)->item[pillNum].y);
      }
    }
    (*value)->item[pillNum].owner = owner;
    logAddEvent(log_PillSetOwner, pillNum, owner, migrate, 0, 0, NULL);
    pillNum++;
    /* Winbolo.Net stuff — allied steals are not stat-tracked. */
    if (migrate == FALSE && owner != NEUTRAL) {
      if (returnValue == NEUTRAL) {
        winbolonetAddEvent(WINBOLO_NET_EVENT_PILL_CAPTURE, sim->isServer, owner, WINBOLO_NET_NO_PLAYER,
                           playersIsBot(&sim->plyrs, owner), FALSE);
      } else if (playersIsAllie(&sim->plyrs, owner, returnValue) == FALSE) {
        winbolonetAddEvent(WINBOLO_NET_EVENT_PILL_STEAL, sim->isServer, owner, returnValue,
                           playersIsBot(&sim->plyrs, owner), playersIsBot(&sim->plyrs, returnValue));
      }
    }
    if (sim->isServer == FALSE) {
      frontEndStatusPillbox(clientSimFromSim(sim), pillNum, (pillsGetAllianceNum(sim, value, pillNum)));
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          pillsGetDamagePos
*AUTHOR:        John Morrison
*CREATION DATE: 15/1/99
*LAST MODIFIED: 15/1/99
*PURPOSE:
*  Does the amount of damage to the pillbox at the give
*  location. Usually caused by explosions
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*  amount - Amount of damage done to the pillbox
*  owner  - The slot whose dying tank the blast came
*           from, for the policy questions alone: the
*           kill is still credited to nobody
*********************************************************/
void pillsGetDamagePos(GameSim *sim, pillboxes *value, BYTE xValue, BYTE yValue, BYTE amount, BYTE owner) {
  BYTE count;       /* Looping Variable */

  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue) {
      BYTE before = (*value)->item[count].armour;
      /* The blast's damage as the host prices it. */
      amount = pillsScaledDamage(sim, amount, owner, count, DMG_SRC_EXPLOSION,
                                 DMG_NO_PILL);
      /* Ask whether the blow takes more than is left rather than subtracting
         first and reading the wrap: a wrapped value only looks like "was
         full" while the cap and the damage are both compile-time, and a rules
         table can put any pair of numbers here. */
      if (amount > before) {
        (*value)->item[count].armour = 0;
      } else {
        (*value)->item[count].armour = (BYTE) (before - amount);
      }
      /* The blow that would finish the pill is the host's to refuse, and a
         refusal holds it at one armour, where it goes on firing. Asked only
         where this blow is what emptied it, so a pill already dead is left
         dead rather than raised by an explosion landing on it. The question
         names the tank whose blast it was. */
      if ((*value)->item[count].armour == 0 && before > 0 &&
          gameSimCanDie(sim, DIE_KIND_PILL, count, owner,
                        DMG_SRC_EXPLOSION, DMG_NO_PILL) == FALSE) {
        (*value)->item[count].armour = 1;
      }
      if ((*value)->item[count].armour == 0) {
        /* Only when this blow is what emptied it — an explosion landing on a
           pill already dead kills nothing. Nobody is credited with splash, so
           the attacker is NEUTRAL here even though the death question above
           was told whose blast it was. */
        if (before > 0 && sim->isServer && sim->callbacks.pillKilled) {
          sim->callbacks.pillKilled(sim->callbacks.ctx, count, NEUTRAL);
        }
        if (sim->isServer == FALSE) {
          frontEndStatusPillbox(clientSimFromSim(sim), (BYTE) (count+1), pillDead);
        }
      }
      logAddEvent(log_PillSetHealth, count, (*value)->item[count].armour, 0, 0, 0, NULL);

      count = (*value)->numPills;
    }
    count++;
  }
}

/*********************************************************
*NAME:          pillsNumInRect
*AUTHOR:        John Morrison
*CREATION DATE: 17/1/99
*LAST MODIFIED: 27/3/99
*PURPOSE:
*  Returns the number of hostile pillboxs inside the
*  given rectangle
*
*ARGUMENTS:
*  value   - Pointer to the pillbox structure
*  leftPos   - Left of the rectangle
*  rightPos  - Right of the rectangle
*  top    - Top of the rectangle
*  bottom - Bottom of the rectangle
*********************************************************/
BYTE pillsNumInRect(GameSim *sim, pillboxes *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  returnValue = 0;
  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].x >= leftPos && (*value)->item[count].x <= rightPos && (*value)->item[count].y >= top && (*value)->item[count].y <= bottom && (playersIsAllie(&sim->plyrs, (*value)->item[count].owner, sim->viewPlayer) == FALSE)) {
      if ((*value)->item[count].armour > 0) {
        returnValue++;
      }
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          pillsRepairPos
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 18/1/99
*PURPOSE:
*  Repairs a pill at the specific location
*  given rectangle
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Map position
*  yValue - Y Map position
*  treeAmount - Trees the man is carrying
*
*  Returns the trees that weren't needed, for the man to carry home.
*********************************************************/
BYTE pillsRepairPos(GameSim *sim, pillboxes *value, BYTE xValue, BYTE yValue, BYTE treeAmount) {
  BYTE count;   /* Looping variable */
  BYTE armour;  /* Armour the pill has right now */
  BYTE needed;  /* Trees it takes to reach full armour from there */
  BYTE used;    /* Trees actually spent */

  used = 0;
  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && ((*value)->item[count].inTank) == FALSE) {
      /* Repair against the armour the pill has on arrival, not the armour it
         had when the order was given — it may have taken more hits since, or
         been patched up by someone else. */
      armour = (*value)->item[count].armour;
      needed = 0;
      if (armour < sim->rules.pill_max_armour) {
        needed = (BYTE) (((sim->rules.pill_max_armour - armour) +
                          sim->rules.pill_repair_amount - 1) /
                         sim->rules.pill_repair_amount);
      }
      used = treeAmount;
      if (used > needed) {
        used = needed;
      }
      if (used > 0) {
        armour = (BYTE) (armour + (used * sim->rules.pill_repair_amount));
        if (armour > sim->rules.pill_max_armour) {
          armour = (BYTE) sim->rules.pill_max_armour;
        }
        (*value)->item[count].armour = armour;
        if (sim->isServer == FALSE) {
          frontEndStatusPillbox(clientSimFromSim(sim), (BYTE) (count+1), (pillsGetAllianceNum(sim, value, (BYTE) (count+1))));
        }
        logAddEvent(log_PillSetHealth, count, (*value)->item[count].armour, 0, 0, 0, NULL);
      }
      count = (*value)->numPills;

    }
    count++;
  }
  return (BYTE) (treeAmount - used);
}

/*********************************************************
*NAME:          pillsGetArmourPos
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 18/1/99
*PURPOSE:
*  Returns the amount of armour a pillbox has at a specific
*  location. Returns PILL_NOT_FOUND if no pills exist at
*  that location
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Map position
*  yValue - Y Map position
*********************************************************/
BYTE pillsGetArmourPos(pillboxes *value, BYTE mx, BYTE my) {
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping variable */
  
  returnValue = PILL_NOT_FOUND;
  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == mx && ((*value)->item[count].y) == my && ((*value)->item[count].inTank) == FALSE) {
      returnValue = (*value)->item[count].armour;
      count = (*value)->numPills;
    }
    count++;
  }
  return returnValue;
}


/*********************************************************
*NAME:          pillsMoveView
*AUTHOR:        John Morrison
*CREATION DATE: 21/1/01
*LAST MODIFIED: 21/1/01
*PURPOSE:
* Allows players to scroll through their pills. Returns
* whether a pill was found in that direction. 
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  eligible - Bit per pill index: that pill may be selected
*  mx       - Pointer to hold X Map position (and prev)
*  my       - Pointer to hold Y Map position (and prev)
*  xMove    - -1 for moving left, 1 for right, 0 for neither
*  yMove    - -1 for moving up, 1 for down, 0 for neither
*********************************************************/
bool pillsMoveView(GameSim *sim, pillboxes *value, PlayerBitMap eligible, BYTE *mx, BYTE *my, int xMove, int yMove) {
  bool returnValue; /* Value to return */
  double nearest;   /* Nearest */
  BYTE count;       /* Looping variable */
  BYTE myPlayerNum; /* Out player number */
  BYTE found;       /* Have we found the item */
  double dist;
  BYTE oldPill;

  nearest = 65000;
  returnValue = FALSE;
  found = 0;
  count = 0;
  oldPill = pillsGetViewPillNum(value, *mx, *my, FALSE, FALSE);
  oldPill--;
  myPlayerNum = sim->viewPlayer;
  while (count < (*value)->numPills) {
    if (count != oldPill && (*value)->active[count] != FALSE && (eligible & ((PlayerBitMap)1 << count)) != 0 && (playersIsAllie(&sim->plyrs, myPlayerNum, (*value)->item[count].owner) == TRUE) && ((*value)->item[count].armour) > 0 && ((*value)->item[count].inTank) == FALSE) {
      if (((yMove == 0 && (xMove < 0 && (*value)->item[count].x < *mx)) || (xMove > 0 && (*value)->item[count].x > *mx)) || ((xMove == 0 && (yMove < 0 && (*value)->item[count].y < *my)) || (yMove > 0 && (*value)->item[count].y > *my))) {
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
*NAME:          pillsGetNextView
*AUTHOR:        John Morrison
*CREATION DATE: 3/2/99
*LAST MODIFIED: 3/2/99
*PURPOSE:
* Returns the next allied (and alive) pill exists. If so
* then it puts its map co-ordinates into the parameters
* passed. If a previous pill is being used the the 
* parameter 'prev' is true and mx & my are set to the last
* pills location
*
*ARGUMENTS:
*  value    - Pointer to the pillbox structure
*  eligible - Bit per pill index: that pill may be selected
*  mx       - Pointer to hold X Map position (and prev)
*  my       - Pointer to hold Y Map position (and prev)
*  prev     - Whether a previous pill is being passed
*********************************************************/
bool pillsGetNextView(GameSim *sim, pillboxes *value, PlayerBitMap eligible, BYTE *mx, BYTE *my, bool prev) {
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
    count = pillsGetViewPillNum(value, *mx, *my, FALSE, FALSE);
    if (count == PILL_NOT_FOUND) {
      count = 0;
    } else {
      count--;
      if ((*value)->item[count].armour > 0 && ((*value)->item[count].inTank) == FALSE) {
        okLoop = TRUE;
        count++;
      } else {
        count = 0;
      }
    }
  }

  /* Find the next item */
  while (done == FALSE && count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && (eligible & ((PlayerBitMap)1 << count)) != 0 && (playersIsAllie(&sim->plyrs, playNumber, (*value)->item[count].owner) == TRUE) && ((*value)->item[count].armour) > 0 && ((*value)->item[count].inTank) == FALSE) {
      done = TRUE;
      *mx = (*value)->item[count].x;
      *my = (*value)->item[count].y;
    }
    count++;
  }
 
  /* If not found still and we are loop do it here */
  if (done == FALSE && okLoop == TRUE) {
    count = 0;
    while (done == FALSE && count < ((*value)->numPills)) {
      if ((*value)->active[count] != FALSE && (eligible & ((PlayerBitMap)1 << count)) != 0 && (playersIsAllie(&sim->plyrs, playNumber, (*value)->item[count].owner) == TRUE) && ((*value)->item[count].armour) > 0 && ((*value)->item[count].inTank) == FALSE) {
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

/* The one pill-view predicate — see internal/pillbox.h. */
bool pillsCanView(GameSim *sim, pillboxes *value, BYTE pillIdx, BYTE viewPlayer) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if (pillIdx < (*value)->numPills) {
    if ((*value)->active[pillIdx] != FALSE && (playersIsAllie(&sim->plyrs, viewPlayer, (*value)->item[pillIdx].owner) == TRUE) && ((*value)->item[pillIdx].armour) != 0 && ((*value)->item[pillIdx].inTank) == FALSE) {
      returnValue = TRUE;
    }
  }

  return returnValue;
}

/*********************************************************
*NAME:          pillsCheckView
*AUTHOR:        John Morrison
*CREATION DATE: 3/2/99
*LAST MODIFIED: 3/2/99
*PURPOSE:
* We are currently in pillview with the pill at position
* mx and my. This function checks that it is still alive
* so we can continue viewing through it.
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  mx    - X Map position
*  my    - Y Map position
*********************************************************/
bool pillsCheckView(GameSim *sim, pillboxes *value, BYTE mx, BYTE my) {
  BYTE pillNum; /* The pillbox number */

  pillNum = pillsGetViewPillNum(value, mx, my, FALSE, FALSE);
  if (pillNum == PILL_NOT_FOUND || pillNum == (PILL_NOT_FOUND-1)) {
    return FALSE;
  }
  pillNum--;

  return pillsCanView(sim, value, pillNum, sim->viewPlayer);
}

/*********************************************************
*NAME:          pillsBaseHit
*AUTHOR:        John Morrison
*CREATION DATE: 16/2/99
*LAST MODIFIED: 16/2/99
*PURPOSE:
* A base has been hit. Check to see if any pillboxes are 
* in range and are allied to the base to see if they should
* get angry
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  mx    - X location of the base that was hit
*  my    - Y location of the base that was hit
*  baseOwner - Owner of the base that was hit
*********************************************************/
void pillsBaseHit(GameSim *sim, pillboxes *value, BYTE mx, BYTE my, BYTE baseOwner) {
  BYTE count; /* Looping variable */
  int xDist;  /* x distance from the base */
  int yDist;  /* y distance from the base */
  int range;  /* pill_base_defend_range */
  bool inRange;

  range = sim->rules.pill_base_defend_range;
  for (count=0;count<(*value)->numPills;count++) {
    xDist = ((*value)->item[count].x) - mx;
    yDist = ((*value)->item[count].y) - my;
    if (sim->rules.pill_base_defend_shape == PILL_BASE_HIT_CIRCLE) {
      inRange = (xDist * xDist + yDist * yDist < range * range);
    } else {
      inRange = (xDist >= -range && xDist <= range && yDist >= -range && yDist <= range);
    }
    if ((*value)->active[count] != FALSE && inRange &&
        (*value)->item[count].owner != NEUTRAL && (playersIsAllie(&sim->plyrs, baseOwner, (*value)->item[count].owner) == TRUE) && (*value)->item[count].armour > 0) {
      /* It is in range make it angry */
      (*value)->item[count].coolDown = (BYTE) sim->rules.pill_cooldown_ticks;
      if ((*value)->item[count].speed > sim->rules.pill_attack_min_ticks) {
        (*value)->item[count].speed /= (BYTE) sim->rules.pill_angry_divisor;
        if ((*value)->item[count].speed < sim->rules.pill_attack_min_ticks) {
          (*value)->item[count].speed = (BYTE) sim->rules.pill_attack_min_ticks;
        }
      }
    }
  }
}

/*********************************************************
*NAME:          pillsGetNumNeutral
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Returns the number of neutral pills
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*********************************************************/
BYTE pillsGetNumNeutral(pillboxes *value) {
  BYTE returnValue; /* Value to return */
  BYTE count;       /* Looping variable */
  
  returnValue = 0;
  for (count=0;count<(*value)->numPills;count++) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].owner == NEUTRAL) {
      returnValue++;
    }
  }

  return returnValue;
}

/*********************************************************
*NAME:          pillsValidate
*PURPOSE:
*  Clamps the pillbox fields a map cannot be trusted on
*  whatever the rules are: the count, which indexes item[],
*  and each owner, which indexes the player list. Both are
*  properties of the file, so this runs without a sim and
*  the map editor, the preview and the tile-test tool get it
*  as the game does. The gameplay caps are pillsClampToRules
*  below. Pure clamping, and idempotent.
*
*ARGUMENTS:
*  value - Pointer to the pillboxes structure
*********************************************************/
void pillsValidate(pillboxes *value) {
  BYTE count;

  if (value == NULL || *value == NULL) {
    return;
  }
  if ((*value)->numPills > MAX_PILLS) {
    (*value)->numPills = MAX_PILLS;
  }
  for (count = 0; count < (*value)->numPills; count++) {
    pillbox *item = &((*value)->item[count]);
    /* x and y are BYTE against a 256x256 map: in range by type. */
    if (item->owner > (MAX_TANKS - 1) && item->owner != NEUTRAL) {
      item->owner = NEUTRAL;
    }
  }
}

/*********************************************************
*NAME:          pillsClampToRules
*PURPOSE:
*  Clamps every pillbox to the gameplay caps this sim runs
*  on: armour to the pill armour cap, and speed into the
*  attack interval. The same clamps pillsSetPill applies to
*  one record, over the whole list, for the load paths that
*  do not go through it, and arms the cooldown alongside
*  speed as that function does. Idempotent.
*
*ARGUMENTS:
*  sim   - Pointer to the game sim
*  value - Pointer to the pillboxes structure
*********************************************************/
void pillsClampToRules(GameSim *sim, pillboxes *value) {
  BYTE count;

  if (sim == NULL || value == NULL || *value == NULL) {
    return;
  }
  for (count = 0; count < (*value)->numPills; count++) {
    pillbox *item = &((*value)->item[count]);
    if (item->armour > sim->rules.pill_max_armour) {
      item->armour = (BYTE) sim->rules.pill_max_armour;
    }
    if (item->speed < sim->rules.pill_attack_min_ticks) {
      item->speed = (BYTE) sim->rules.pill_attack_min_ticks;
    } else if (item->speed > sim->rules.pill_attack_ticks) {
      item->speed = (BYTE) sim->rules.pill_attack_ticks;
    }
    /* A pill firing faster than the normal interval is warming back up, and
       the countdown is what walks it there. A record can arrive without one —
       the .map format has no field for it — so arm it rather than leave the
       pill angry for the rest of the game. One already counting down keeps
       its countdown, so a map installed mid-game does not restart it. */
    if (item->speed != sim->rules.pill_attack_ticks && item->coolDown == 0) {
      item->coolDown = (BYTE) sim->rules.pill_cooldown_ticks;
    }
  }
}

/*********************************************************
*NAME:          pillsFillToRules
*PURPOSE:
*  Brings every pillbox up to the armour cap this sim runs
*  on. The other direction from pillsClampToRules above, for
*  a scenario that raises the cap and asks for the map to
*  start at it: a map file holds a number and has no way of
*  saying "full", so without this a raised cap leaves every
*  pillbox where the file put it.
*
*  Armour only. Speed is an attack interval rather than a
*  stock, so filling it would leave every pillbox firing at
*  the slowest rate the table allows. Idempotent, and a pill
*  already at the cap is left alone.
*
*ARGUMENTS:
*  sim   - Pointer to the game sim
*  value - Pointer to the pillboxes structure
*********************************************************/
void pillsFillToRules(GameSim *sim, pillboxes *value) {
  BYTE count;

  if (sim == NULL || value == NULL || *value == NULL) {
    return;
  }
  for (count = 0; count < (*value)->numPills; count++) {
    pillbox *item = &((*value)->item[count]);
    if (item->armour < sim->rules.pill_max_armour) {
      item->armour = (BYTE) sim->rules.pill_max_armour;
    }
  }
}

void pillsSetPillCompressData(pillboxes *value, BYTE *buff, int dataLen) {
  memcpy(&(**value), buff, SIZEOF_PILLS);
  /* Clamp the wire-supplied count so a hostile map cannot drive out-of-bounds
   * reads of item[] past MAX_PILLS. */
  if ((*value)->numPills > MAX_PILLS) {
    (*value)->numPills = MAX_PILLS;
  }
  /* The copy above stops at the wire format, so the position-current flags
   * still describe whatever list was here before. Every square in the blob is
   * one this client was given, so start them all current and let the next
   * snapshot say which of them the pill has moved off. That does mean a pill
   * that really has moved counts as solid for up to a full-sync interval after
   * a mid-game resync; the other default would make every pill on the map
   * non-solid for the same window, including the one you are driving at. */
  memset((*value)->posStale, 0, sizeof((*value)->posStale));
  /* The live flags sit past the wire format too, so they also still describe
     whatever list was here before. A blob is a map, and every pill a map
     carries is on it: mark the count live and the slots above it removed. */
  memset((*value)->active, TRUE, (*value)->numPills);
  memset((*value)->active + (*value)->numPills, FALSE,
         (size_t)(MAX_PILLS - (*value)->numPills));
}

/*********************************************************
*NAME:          pillsGetPillNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Gets a copy of the pills data and copies it to buff.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*  buff  - Buffer to hold copy of pills data
*********************************************************/
BYTE pillsGetPillNetData(pillboxes *value, BYTE *buff) {
  BYTE count = 0;
  BYTE len = 1;

  buff[0] = (*value)->numPills;
  while (count < (*value)->numPills) {
    buff[len] = (*value)->item[count].x;
    len++;
    buff[len] = (*value)->item[count].y;
    len++;
    buff[len] = (*value)->item[count].armour;
    len++;
    buff[len] = (*value)->item[count].owner;
    len++;
    buff[len] = (*value)->item[count].speed;
    len++;
    buff[len] = (*value)->item[count].inTank;
    len++;
    buff[len] = (*value)->item[count].reload;
    len++;
    buff[len] = (*value)->item[count].justSeen;
    len++;
    buff[len] = (*value)->item[count].coolDown;
    len++;
    count++;
  }
  return len;
}


/*********************************************************
*NAME:          pillsDropSetNeutralOwner
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
*  Changes all pills of owner owner back to neutral. Also
*  if they are inside a tank it drops them.
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  owner - Owner to set back to neutral
*********************************************************/
void pillsDropSetNeutralOwner(GameSim *sim, BYTE owner) {
	pillboxes *value = &sim->pb;
	BYTE count;       /* Looping Variable */

	count = 0;
	while (count < ((*value)->numPills)) {
		/* Pills owner is the same owner that has quit the game */
		if ((*value)->active[count] != FALSE && ((*value)->item[count].owner) == owner) {
			(*value)->item[count].owner = NEUTRAL;
			/* A 'neutral' tank captures the pill instantly */
			/* The tank was carrying pills */
			if (((*value)->item[count].inTank) == TRUE) {
				(*value)->item[count].inTank = FALSE;
				/* Send a message that all the pills in the tank are now dead and belong to a 'neutral' player */
        logAddEvent(log_PillSetInTank, utilPutNibble(count, FALSE), 0, 0, 0, 0, NULL);
        logAddEvent(log_PillSetOwner, count, NEUTRAL, FALSE, 0, 0, NULL);
			}
		}
		count++;
	}
}

/*********************************************************
*NAME:          pillsMigrate
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED: 1/11/99
*PURPOSE:
*  Causes all bases owned by owner to migrate to a new 
*  owner because its old owner left the game.
*
*ARGUMENTS:
*  value    - Pointer to the bases structure
*  oldOwner - Old Owner to remove
*  newOwner - Owner to replace with
*********************************************************/
void pillsMigrate(GameSim *sim, BYTE oldOwner, BYTE newOwner) {
	pillboxes *value = &sim->pb;
	bool isServer = sim->isServer;
	BYTE count;    /* Looping Variable */

	count = 0;
	while (count < ((*value)->numPills)) {
		if ((*value)->active[count] != FALSE && ((*value)->item[count].owner) == oldOwner) {
			(*value)->item[count].owner = newOwner;
				if (((*value)->item[count].inTank) == TRUE && isServer == TRUE) {
				(*value)->item[count].inTank = FALSE;
        logAddEvent(log_PillSetOwner, count, newOwner, FALSE, 0, 0, NULL);
        logAddEvent(log_PillSetInTank, utilPutNibble(count, FALSE), 0, 0, 0, 0, NULL);
        logAddEvent(log_PillSetPlace, count, (*value)->item[count].x, (*value)->item[count].y, 0, 0, NULL);
			}
		}
		count++;
	}
}

/*********************************************************
*NAME:          pillsMigratePlanted
*AUTHOR:        Minhiriath
*CREATION DATE: 14/03/2009
*LAST MODIFIED: 14/03/2009
*PURPOSE:
*  Causes all pills owned by owner  to migrate to a new 
* owner becuase its owner left alliance
*
*ARGUMENTS:
*  value    - Pointer to the bases structure
*  oldOwner - Old Owner to remove
*  newOwner - Owner to replace with
*********************************************************/
void pillsMigratePlanted(GameSim *sim, BYTE oldOwner, BYTE newOwner) {
  pillboxes *value = &sim->pb;
  BYTE count;    /* Looping Variable */
  count = 0;
  while (count < ((*value)->numPills)) {
	if ((*value)->active[count] != FALSE && ((*value)->item[count].owner) == oldOwner) {
 	  if((*value)->item[count].inTank == FALSE){
	    (*value)->item[count].owner = newOwner;
	  } else {
	    (*value)->item[count].inTank = TRUE;
		(*value)->item[count].owner = oldOwner;
	  }
	} 
    count++;
  }
}

/*********************************************************
*NAME:          pillsIsCapturable
*AUTHOR:        John Morrison
*CREATION DATE: 10/11/99
*LAST MODIFIED: 10/11/99
*PURPOSE:
*  Returns whether a pillbox a specific location is dead
*  or not. A pill whose square this client has not been
*  told is current does not count as being there.
*
*ARGUMENTS:
*  value  - Pointer to the pillbox structure
*  xValue - X Location of pillbox
*  yValue - Y Location of pillbox
*********************************************************/
bool pillsIsCapturable(pillboxes *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping Variable */

  returnValue = FALSE;
  count = 0;
  while (returnValue == FALSE && count < ((*value)->numPills)) {
    /* The same position-current test pillsGetPillNum makes: the pickup loop
       asks this first and then asks pillsGetPillNum which pill it was, so a
       pill only one of the two can see would leave the tank carrying a pill
       number that does not exist. */
    if ((*value)->active[count] != FALSE && ((*value)->item[count].x) == xValue && ((*value)->item[count].y) == yValue && ((*value)->item[count].armour == 0)  && ((*value)->item[count].inTank == FALSE) && ((*value)->posStale[count]) == 0) {
      returnValue = TRUE;
      count = (*value)->numPills;
    }
    count++;
  }
  return returnValue;
}


/*********************************************************
*NAME:          pillsExplicitDrop
*AUTHOR:        John Morrison
*CREATION DATE: 10/11/99
*LAST MODIFIED: 10/11/99
*PURPOSE:
*  Drops all pills of owner owner
*
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*  owner - Owner to set back to neutral
*********************************************************/
void pillsExplicitDrop(GameSim *sim, BYTE owner) {
  pillboxes *value = &sim->pb;
  BYTE count;       /* Looping Variable */

  count = 0;
  while (count < ((*value)->numPills)) {
    if ((*value)->active[count] != FALSE && ((*value)->item[count].owner) == owner) {
      if (((*value)->item[count].inTank) == TRUE) {
        (*value)->item[count].inTank = FALSE;
        logAddEvent(log_PillSetInTank, utilPutNibble(count, FALSE), 0, 0, 0, 0, NULL);
        logAddEvent(log_PillSetPlace, count, (*value)->item[count].x, (*value)->item[count].y, 0, 0, NULL);
      }
    }
    count++;
  }
}

/*********************************************************
*NAME:          pillsGetBrainPillsInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 09/06/01
*PURPOSE:
*  Makes the brain pill info for each pill inside the
*  rectangle formed by the function parameters
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  leftPos   - Left position of rectangle
*  rightPos  - Right position of rectangle
*  top    - Top position of rectangle
*  bottom - Bottom position of rectangle
*********************************************************/
void pillsGetBrainPillsInRect(ClientSim *cs, GameSim *sim, pillboxes *value, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  WORLD wx;        /* X and Y Position of the object */
  WORLD wy;
  BYTE owner;      /* Owner of the object */
  BYTE playerNum;  /* Our player Number */
  bool isAllie;    /* Is this item our allie */
  BYTE count;      /* Looping variable */

  count = 0;
  playerNum = sim->viewPlayer;

/* typedef struct
	{
	OBJECT object; = 2
	WORLD_X x;
	WORLD_Y y;
	WORD idnum;
	BYTE direction;
	BYTE info;
	} ObjectInfo;
*/

  while (count < ((*value)->numPills)) {
    isAllie = FALSE;
    if ((*value)->item[count].owner != NEUTRAL) {
      isAllie = playersIsAllie(&sim->plyrs, playerNum, (*value)->item[count].owner);
    }
    if ((*value)->active[count] != FALSE && ((((*value)->item[count].x) >= leftPos && ((*value)->item[count].x) <= rightPos && ((*value)->item[count].y) >= top && ((*value)->item[count].y) <= bottom) || isAllie == TRUE) && ((*value)->item[count].inTank == FALSE)) {
      /* In the rectangle */
      wx = (*value)->item[count].x;
      wx <<= TANK_SHIFT_MAPSIZE;
      wy = (*value)->item[count].y;
      wy <<= TANK_SHIFT_MAPSIZE;
      wx += MAP_SQUARE_MIDDLE;
      wy += MAP_SQUARE_MIDDLE;
      if ((*value)->item[count].owner == NEUTRAL) {
        owner = PILLS_BRAIN_NEUTRAL;
      } else if (isAllie == TRUE) {
        owner = PILLS_BRAIN_FRIENDLY;
      } else {
        owner = PILLS_BRAIN_HOSTILE;
      }
      brainDataAddObject(cs, PILLS_BRAIN_OBJECT_TYPE, wx, wy, count, (*value)->item[count].armour, owner, 0);
    }
    count++;
  }
}

/*********************************************************
*NAME:          pillsSetBrainView
*AUTHOR:        John Morrison
*CREATION DATE: 13/12/99
*LAST MODIFIED: 13/12/99
*PURPOSE:
*  Returns if we are allowed to view from this pillbox.
*  By allowed the pill must not be in a tank, must not be
*  dead and must be on the same alliance as us.
*
*ARGUMENTS:
*  value     - Pointer to the pills structure
*  pillNum   - The pill number we are requesting the view
*  playerNum - Our player number
*********************************************************/
bool pillsSetView(GameSim *sim, pillboxes *value, BYTE pillNum, BYTE playerNum) {
  bool returnValue; /* Value to return */

  returnValue = FALSE;
  if (pillNum < ((*value)->numPills)) {
    if ((*value)->active[pillNum] != FALSE && (*value)->item[pillNum].inTank == FALSE && (*value)->item[pillNum].armour > 0 && playersIsAllie(&sim->plyrs, playerNum, (*value)->item[pillNum].owner) == TRUE) {
      returnValue = TRUE;
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          pillsGetMaxs
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Gets the maximum positions values of all the pillboxs.
*  eg left most, right most etc.
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  leftPos   - Pointer to hold the left most value
*  rightPos  - Pointer to hold the right most value
*  top    - Pointer to hold the top most value
*  bottom - Pointer to hold the bottom most value
*********************************************************/
void pillsGetMaxs(pillboxes *value, int *leftPos, int *rightPos, int *top, int *bottom) {
  BYTE count; /* Looping Variable */

  *top = MAP_ARRAY_SIZE;
  *bottom = -1;
  *leftPos = MAP_ARRAY_SIZE;
  *rightPos = -1;

  count = 0;
  while (count < ((*value)->numPills)) {
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
    if ((*value)->item[count].y < *top) {
      *top = (*value)->item[count].y;
    }
    if ((*value)->item[count].y > *bottom) {
      *bottom = (*value)->item[count].y;
    }  
    count++;
  }
}

/*********************************************************
*NAME:          pillsMoveAll
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Repositions the pillboxes by moveX, moveY
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  moveX  - The X move amount 
*  moveY  - The Y move amount 
*********************************************************/
void pillsMoveAll(pillboxes *value, int moveX, int moveY) {
  BYTE count; /* Looping Variable */

  count = 0;
  while (count < ((*value)->numPills)) {
    (*value)->item[count].x = (BYTE) ((*value)->item[count].x + moveX);
    (*value)->item[count].y = (BYTE) ((*value)->item[count].y + moveY);
    count++;
  }
}

/*********************************************************
*NAME:          pillsGetOwnerBitMask
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Returns the owner bitmask for all the pills own by a 
* player.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*  owner - Owner to return pills owned for
*  moveY  - The Y move amount 
*********************************************************/
PlayerBitMap pillsGetOwnerBitMask(pillboxes *value, BYTE owner) {
  PlayerBitMap returnValue; /* Value to return */
  BYTE count;               /* Looping variable */

  count = 0;
  returnValue = 0;
  while (count < (*value)->numPills) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].owner == owner) {
      returnValue |= 1 << count;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          pillsGetAttackSpeed
*AUTHOR:        John Morrison
*CREATION DATE: 29/7/00
*LAST MODIFIED: 29/7/00
*PURPOSE:
* Returns the pillboxes attacking speed. Returns 
* PILLBOX_ATTACK_NORMAL if out of range.
*
*ARGUMENTS:
*  value - Pointer to the pills structure
*  pillNum - Pillbox number to get
*********************************************************/
BYTE pillsGetAttackSpeed(pillboxes *value, BYTE pillNum) {
  BYTE returnValue; /* Value to return */

  returnValue = PILLBOX_ATTACK_NORMAL;
  if (pillNum > 0 && pillNum  <= (*value)->numPills) {
    pillNum--;
    returnValue = (*value)->item[pillNum].speed;
  }

  return returnValue;
}

/*********************************************************
*NAME:          pillsIsInView
*AUTHOR:        John Morrison
*CREATION DATE: 29/7/00
*LAST MODIFIED: 29/7/00
*PURPOSE:
* Returns if a location is in view (range +/- 10 squares
* of a pill allied to playerNum
*
*ARGUMENTS:
*  value     - Pointer to the pills structure
*  playerNum - Player number for pill alliance
*  mx        - X Map location
*  my        - Y Map location
*********************************************************/
bool pillsIsInView(GameSim *sim, pillboxes *value, BYTE playerNum, BYTE mx, BYTE my) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping variable */
  int gapX;
  int gapY;

  count = 0;
  returnValue = FALSE;
  while (count < (*value)->numPills && returnValue == FALSE) {
    if ((*value)->active[count] != FALSE && playersIsAllie(&sim->plyrs, (*value)->item[count].owner, playerNum) == TRUE) {
      gapX = (*value)->item[count].x - mx;
      gapY = (*value)->item[count].y - my;
      if (gapX >= -10 && gapX <= 10 && gapY >= -10 && gapY <=  10) {
        returnValue = TRUE;
      }
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          pillsGetNumberOwnedByPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 19/11/03
*LAST MODIFIED: 19/11/03
*PURPOSE:
* Returns the number of pillboxes owned by this player
*
*ARGUMENTS:
*  value     - Pointer to the pills structure
*  playerNum - Player number for pills
*********************************************************/
BYTE pillsGetNumberOwnedByPlayer(pillboxes *value, BYTE playerNum) {
  BYTE returnValue; /* Value to return */
  BYTE count; /* Looping variable */

  returnValue = 0;
  count = 0;

  while (count < (*value)->numPills) {
    if ((*value)->active[count] != FALSE && (*value)->item[count].owner == playerNum) {
      returnValue++;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          pillsGetNumActive
*PURPOSE:
*  Returns how many pillboxes are on the map: the slots
*  under the count whose live flag is set.
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*********************************************************/
BYTE pillsGetNumActive(pillboxes *value) {
  BYTE count;
  BYTE live = 0;
  if (value == NULL || *value == NULL) {
    return 0;
  }
  for (count = 0; count < (*value)->numPills && count < MAX_PILLS; count++) {
    if ((*value)->active[count] != FALSE) {
      live++;
    }
  }
  return live;
}

/*********************************************************
*NAME:          pillsRemoveBorderPills
*PURPOSE:
*  Takes off the map every live pillbox that sits in the
*  mined border round the edge, the same rule as
*  startsRemoveBorderStarts. No tank can reach a pillbox
*  out there, so it could never be shot or picked up, yet it
*  was counted, drawn and handed to the bots. A pillbox in a
*  tank is not on a square and is left alone: its square is
*  only where it was picked up. Nothing is changed when no
*  live pillbox on the ground is inside the border, so such
*  a map still plays as it does today. Slot numbers do not
*  change. Returns how many pillboxes were taken off.
*
*ARGUMENTS:
*  value - Pointer to the pillbox structure
*********************************************************/
BYTE pillsRemoveBorderPills(pillboxes *value) {
  BYTE count;
  BYTE inside = 0;
  BYTE removed = 0;

  if (value == NULL || *value == NULL) {
    return 0;
  }
  for (count = 0; count < (*value)->numPills && count < MAX_PILLS; count++) {
    if ((*value)->active[count] != FALSE &&
        (*value)->item[count].inTank == FALSE &&
        mapPosInBounds((*value)->item[count].x, (*value)->item[count].y)) {
      inside++;
    }
  }
  if (inside == 0) {
    return 0;
  }
  for (count = 0; count < (*value)->numPills && count < MAX_PILLS; count++) {
    if ((*value)->active[count] != FALSE &&
        (*value)->item[count].inTank == FALSE &&
        !mapPosInBounds((*value)->item[count].x, (*value)->item[count].y)) {
      (*value)->active[count] = FALSE;
      removed++;
    }
  }
  return removed;
}
