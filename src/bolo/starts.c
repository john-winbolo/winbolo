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
*Name:          Starts 
*Filename:      starts.c 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified:   9/4/00
*Purpose:
*  Provides operations on player starts 
*********************************************************/

/* Includes */
#include <stdlib.h>
#include <memory.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "players.h"
#include "starts.h"
#include "game_sim.h"
#include "pillbox.h"
#include "bases.h"
#include "bolo_map.h"
#include "util.h"
#include "gametype.h"

/* Distance thresholds in map squares */
#define START_TANK_RANGE 1
#define START_PILL_RANGE 9
#define START_BASE_RANGE 9
/* Maximum spiral search steps */
#define START_SCATTER_MAX 1000
/* Fraction of neutral bases before we treat neutral same as own */
#define START_NEUTRAL_THRESHOLD_PCT 20

/*********************************************************
*NAME:          startsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the player starts structure.
*  Sets number of starts to zero
*
*ARGUMENTS:
*  value - Pointer to the starts structure 
*********************************************************/
void startsCreate(starts *value) {
  New(*value);
  memset(*value, 0, sizeof(**value));
  ((*value)->numStarts) = 0;
}


/*********************************************************
*NAME:          startsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the starts data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void startsDestroy(starts *value) {
  if (*value != NULL) {
    Dispose(*value);
  }
}

/*********************************************************
*NAME:          startsSetNumStarts
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of starts in the structure 
*
*ARGUMENTS:
*  value     - Pointer to the starts structure
*  numStarts - The number of starts 
*********************************************************/
void startsSetNumStarts(starts *value, BYTE numStarts) {
  if (numStarts <= MAX_STARTS) {
    (*value)->numStarts = numStarts;
  }
}


/*********************************************************
*NAME:          startsGetNumStarts
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of starts in the structure
*
*ARGUMENTS:
*  value  - Pointer to the starts structure
*********************************************************/
BYTE startsGetNumStarts(starts *value) {
  return (*value)->numStarts;
}

/*********************************************************
*NAME:          startsSetStart
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific start with its item data
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - Pointer to a player start 
*  startNum - The start number
*********************************************************/
void startsSetStart(starts *value, start *item, BYTE startNum) {
  if (startNum > 0 && startNum  <= (*value)->numStarts) {
    startNum--;
    (((*value)->item[startNum]).x) = item->x;
    (((*value)->item[startNum]).y) = item->y;
    (((*value)->item[startNum]).dir) = item->dir;
  }
}

/*********************************************************
*NAME:          startsGetStartStruct
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/98
*LAST MODIFIED: 9/2/98
*PURPOSE:
*  Gets a specific start.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - Pointer to a player start 
*  startNum - The start number
*********************************************************/
void startsGetStartStruct(starts *value, start *item, BYTE startNum) {
  if (startNum > 0 && startNum  <= (*value)->numStarts) {
    startNum--;
    item->x = ((*value)->item[startNum]).x;
    item->y = ((*value)->item[startNum]).y;
    item->dir = ((*value)->item[startNum]).dir;
  }
}

/*********************************************************
*NAME:          startsExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 2/7/00
*LAST MODIFIED: 2/7/00
*PURPOSE:
*  Returns if a start exists at a specific location
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  xValue   - X Location to test
*  yValue   - Y Location to test
*********************************************************/
bool startsExistPos(starts *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return  */
  BYTE count;       /* Looping variable */

  returnValue = FALSE;
  count = 0;
  while (count < (*value)->numStarts && returnValue == FALSE) {
    if ((*value)->item[count].x == xValue && (*value)->item[count].y == yValue) {
      returnValue = TRUE;
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          startsIsValidSquare
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns whether a map square is deep sea with no mine.
*
*ARGUMENTS:
*  sim - Pointer to the game simulation
*  mx  - Map X coordinate
*  my  - Map Y coordinate
*********************************************************/
static bool startsIsValidSquare(GameSim *sim, BYTE mx, BYTE my) {
  return (mapGetPos(&sim->mp, mx, my) == DEEP_SEA && mapIsMine(&sim->mp, mx, my) == FALSE);
}

/*********************************************************
*NAME:          startsScatterFind
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Spiral-searches outward from a centre position to find
*  a valid deep-sea square with no mine.
*
*ARGUMENTS:
*  sim    - Pointer to the game simulation
*  centreX - Centre map X coordinate
*  centreY - Centre map Y coordinate
*  outX   - Pointer to receive result X
*  outY   - Pointer to receive result Y
*********************************************************/
static void startsScatterFind(GameSim *sim, BYTE centreX, BYTE centreY, BYTE *outX, BYTE *outY) {
  int step;
  int dx;
  int dy;
  int sx;
  int sy;

  for (step = 0; step < START_SCATTER_MAX; step++) {
    utilSpiralOffset(step, &dx, &dy);
    sx = (int)centreX + dx;
    sy = (int)centreY + dy;
    if (sx > 0 && sx < MAP_ARRAY_SIZE && sy > 0 && sy < MAP_ARRAY_SIZE) {
      if (startsIsValidSquare(sim, (BYTE)sx, (BYTE)sy)) {
        *outX = (BYTE)sx;
        *outY = (BYTE)sy;
        return;
      }
    }
  }
  /* Fallback: use centre even if not ideal */
  *outX = centreX;
  *outY = centreY;
}

/*********************************************************
*NAME:          startsMapDistance
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns the distance in map squares between two points.
*
*ARGUMENTS:
*  x1 - First X coordinate
*  y1 - First Y coordinate
*  x2 - Second X coordinate
*  y2 - Second Y coordinate
*********************************************************/
static int startsMapDistance(int x1, int y1, int x2, int y2) {
  int dx = x1 - x2;
  int dy = y1 - y2;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  return (dx > dy) ? dx : dy;
}

/*********************************************************
*NAME:          startsIsOwnerFriendly
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns whether owner is the same as playerNum or an
*  ally. NEUTRAL is treated as not friendly.
*********************************************************/
static bool startsIsOwnerFriendly(GameSim *sim, BYTE owner, BYTE playerNum) {
  if (owner == NEUTRAL) {
    return FALSE;
  }
  if (owner == playerNum) {
    return TRUE;
  }
  return playersIsAllie(&sim->plyrs, owner, playerNum);
}

/*********************************************************
*NAME:          startsGetStartOpen
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns a start position for open games. Iterates all
*  start positions looking for one with no nearby tanks or
*  pillboxes. Falls back to a position near only friendly
*  units, then to any valid start. Uses spiral scatter to
*  find a nearby valid deep-sea square.
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  value     - Pointer to the starts structure
*  x         - Pointer to receive X map coordinate
*  y         - Pointer to receive Y map coordinate
*  dir       - Pointer to receive direction
*  playerNum - Player number requesting the start
*********************************************************/
static void startsGetStartOpen(GameSim *sim, starts *value, BYTE *x, BYTE *y, TURNTYPE *dir, BYTE playerNum) {
  BYTE numStarts = (*value)->numStarts;
  BYTE numPills = pillsGetNumPills(&sim->pb);
  BYTE offset;
  BYTE idx;
  BYTE count;
  BYTE sx;
  BYTE sy;
  int secondChoice;
  bool anyTankNearby;
  bool anyPillNearby;
  bool hostileTankNearby;
  bool hostilePillNearby;
  BYTE tankCount;
  BYTE pillCount;
  WORLD tankWX;
  WORLD tankWY;
  int dist;
  BYTE pillOwner;
  BYTE bt;

  secondChoice = -1;
  offset = (BYTE)(rand() % numStarts);

  for (count = 0; count < numStarts; count++) {
    idx = (BYTE)((offset + count) % numStarts);
    sx = (*value)->item[idx].x;
    sy = (*value)->item[idx].y;

    /* Must be deep sea with no mine */
    if (startsIsValidSquare(sim, sx, sy) == FALSE) {
      continue;
    }

    anyTankNearby = FALSE;
    anyPillNearby = FALSE;
    hostileTankNearby = FALSE;
    hostilePillNearby = FALSE;

    /* Check all tanks */
    for (tankCount = 0; tankCount < MAX_TANKS; tankCount++) {
      if (tankCount == playerNum || sim->tanks[tankCount] == NULL) {
        continue;
      }
      tankGetWorld(&sim->tanks[tankCount], &tankWX, &tankWY);
      dist = startsMapDistance(sx, sy, tankWX >> M_W_SHIFT_SIZE, tankWY >> M_W_SHIFT_SIZE);
      if (dist <= START_TANK_RANGE) {
        anyTankNearby = TRUE;
        if (playersIsAllie(&sim->plyrs, playerNum, tankCount) == FALSE) {
          hostileTankNearby = TRUE;
        }
      }
    }

    /* Check all pillboxes (alive, not in a tank) */
    for (pillCount = 0; pillCount < numPills; pillCount++) {
      if (sim->pb->item[pillCount].inTank == TRUE || sim->pb->item[pillCount].armour == 0) {
        continue;
      }
      dist = startsMapDistance(sx, sy, sim->pb->item[pillCount].x, sim->pb->item[pillCount].y);
      if (dist <= START_PILL_RANGE) {
        anyPillNearby = TRUE;
        pillOwner = sim->pb->item[pillCount].owner;
        if (pillOwner != NEUTRAL && startsIsOwnerFriendly(sim, pillOwner, playerNum) == FALSE) {
          hostilePillNearby = TRUE;
        }
      }
    }

    /* Ideal: no units nearby at all */
    if (anyTankNearby == FALSE && anyPillNearby == FALSE) {
      startsScatterFind(sim, sx, sy, x, y);
      bt = startsConvertDir((*value)->item[idx].dir);
      *dir = (TURNTYPE)(bt * START_TIMES_16);
      return;
    }

    /* Good: only friendly units nearby */
    if (hostileTankNearby == FALSE && hostilePillNearby == FALSE) {
      if (secondChoice == -1) {
        secondChoice = idx;
      }
      continue;
    }

    /* Fallback: at least it's a valid square */
    if (secondChoice == -1) {
      secondChoice = idx;
    }
  }

  /* Phase 2: use best available */
  if (secondChoice == -1) {
    secondChoice = 0;
  }

  /* Phase 3: scatter search around chosen position */
  startsScatterFind(sim, (*value)->item[secondChoice].x, (*value)->item[secondChoice].y, x, y);
  bt = startsConvertDir((*value)->item[secondChoice].dir);
  *dir = (TURNTYPE)(bt * START_TIMES_16);
}

/*********************************************************
*NAME:          startsGetStartTournament
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns a start position for tournament/strict games.
*  Builds on the open algorithm but biases towards starts
*  near the player's own bases. If more than 20% of bases
*  are still neutral, neutral bases are weighted equally
*  with own bases.
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  value     - Pointer to the starts structure
*  x         - Pointer to receive X map coordinate
*  y         - Pointer to receive Y map coordinate
*  dir       - Pointer to receive direction
*  playerNum - Player number requesting the start
*********************************************************/
static void startsGetStartTournament(GameSim *sim, starts *value, BYTE *x, BYTE *y, TURNTYPE *dir, BYTE playerNum) {
  BYTE numStarts = (*value)->numStarts;
  BYTE numBases = basesGetNumBases(&sim->bs);
  BYTE numPills = pillsGetNumPills(&sim->pb);
  BYTE idx;
  BYTE count;
  BYTE sx;
  BYTE sy;
  BYTE baseCount;
  BYTE pillCount;
  BYTE tankCount;
  int dist;
  int neutralCount;
  bool neutralPreferred;
  bool hasOwnBase;
  bool hasNeutralBase;
  bool hostileTankNearby;
  bool hostilePillNearby;
  bool hostileBaseNearby;
  bool anyTankNearby;
  bool anyPillNearby;
  BYTE ownCandidates[MAX_STARTS];
  BYTE neutralCandidates[MAX_STARTS];
  BYTE safeCandidates[MAX_STARTS];
  BYTE fallbackCandidates[MAX_STARTS];
  BYTE numOwn = 0;
  BYTE numNeutral = 0;
  BYTE numSafe = 0;
  BYTE numFallback = 0;
  WORLD tankWX;
  WORLD tankWY;
  BYTE pillOwner;
  BYTE baseOwner;
  BYTE bt;

  /* Count neutral bases to decide if neutral is preferred */
  neutralCount = 0;
  for (baseCount = 0; baseCount < numBases; baseCount++) {
    if (sim->bs->item[baseCount].owner == NEUTRAL) {
      neutralCount++;
    }
  }
  neutralPreferred = (numBases > 0 && (neutralCount * 100 / numBases) > START_NEUTRAL_THRESHOLD_PCT);

  for (count = 0; count < numStarts; count++) {
    sx = (*value)->item[count].x;
    sy = (*value)->item[count].y;

    /* Must be deep sea with no mine */
    if (startsIsValidSquare(sim, sx, sy) == FALSE) {
      continue;
    }

    hostileTankNearby = FALSE;
    hostilePillNearby = FALSE;
    hostileBaseNearby = FALSE;
    anyTankNearby = FALSE;
    anyPillNearby = FALSE;

    for (tankCount = 0; tankCount < MAX_TANKS; tankCount++) {
      if (tankCount == playerNum || sim->tanks[tankCount] == NULL) {
        continue;
      }
      tankGetWorld(&sim->tanks[tankCount], &tankWX, &tankWY);
      dist = startsMapDistance(sx, sy, tankWX >> M_W_SHIFT_SIZE, tankWY >> M_W_SHIFT_SIZE);
      if (dist <= START_TANK_RANGE) {
        anyTankNearby = TRUE;
        if (playersIsAllie(&sim->plyrs, playerNum, tankCount) == FALSE) {
          hostileTankNearby = TRUE;
        }
      }
    }

    for (pillCount = 0; pillCount < numPills; pillCount++) {
      if (sim->pb->item[pillCount].inTank == TRUE || sim->pb->item[pillCount].armour == 0) {
        continue;
      }
      dist = startsMapDistance(sx, sy, sim->pb->item[pillCount].x, sim->pb->item[pillCount].y);
      if (dist <= START_PILL_RANGE) {
        anyPillNearby = TRUE;
        pillOwner = sim->pb->item[pillCount].owner;
        if (pillOwner != NEUTRAL && startsIsOwnerFriendly(sim, pillOwner, playerNum) == FALSE) {
          hostilePillNearby = TRUE;
        }
      }
    }

    /* Check for nearby bases - both for hostile detection and own/neutral */
    hasOwnBase = FALSE;
    hasNeutralBase = FALSE;
    for (baseCount = 0; baseCount < numBases; baseCount++) {
      if (sim->bs->item[baseCount].armour <= MIN_ARMOUR_CAPTURE) {
        continue;
      }
      dist = startsMapDistance(sx, sy, sim->bs->item[baseCount].x, sim->bs->item[baseCount].y);
      if (dist <= START_BASE_RANGE) {
        baseOwner = sim->bs->item[baseCount].owner;
        if (baseOwner == NEUTRAL) {
          hasNeutralBase = TRUE;
        } else if (startsIsOwnerFriendly(sim, baseOwner, playerNum)) {
          hasOwnBase = TRUE;
        } else {
          hostileBaseNearby = TRUE;
        }
      }
    }

    /* Skip positions near hostile units (tank, pill, or base) */
    if (hostileTankNearby == TRUE || hostilePillNearby == TRUE || hostileBaseNearby == TRUE) {
      SDL_Log("[starts] start %d (%d,%d): HOSTILE (tank=%d pill=%d base=%d)", count, sx, sy,
              hostileTankNearby, hostilePillNearby, hostileBaseNearby);
      fallbackCandidates[numFallback++] = count;
      continue;
    }

    SDL_Log("[starts] start %d (%d,%d): safe (ownBase=%d neutralBase=%d anyTank=%d anyPill=%d)",
            count, sx, sy, hasOwnBase, hasNeutralBase, anyTankNearby, anyPillNearby);

    if (hasOwnBase == TRUE) {
      ownCandidates[numOwn++] = count;
    }
    if (hasNeutralBase == TRUE) {
      neutralCandidates[numNeutral++] = count;
    }
    if (anyTankNearby == FALSE && anyPillNearby == FALSE) {
      safeCandidates[numSafe++] = count;
    }
    fallbackCandidates[numFallback++] = count;
  }

  /* Pick the best available option, randomly within the chosen tier.
   * When neutral bases are still plentiful (>20%), own and neutral
   * are pooled and treated equally. Otherwise own is preferred. */
  SDL_Log("[starts] player %d: neutralPref=%d own=%d neutral=%d safe=%d fallback=%d (neutralBases=%d/%d)",
          playerNum, neutralPreferred, numOwn, numNeutral, numSafe, numFallback, neutralCount, numBases);
  idx = 0;
  if (neutralPreferred && (numOwn > 0 || numNeutral > 0)) {
    BYTE pool[MAX_STARTS * 2];
    BYTE poolSize = 0;
    BYTE i;
    for (i = 0; i < numOwn; i++) pool[poolSize++] = ownCandidates[i];
    for (i = 0; i < numNeutral; i++) pool[poolSize++] = neutralCandidates[i];
    idx = pool[rand() % poolSize];
  } else if (numOwn > 0) {
    idx = ownCandidates[rand() % numOwn];
  } else if (numNeutral > 0) {
    idx = neutralCandidates[rand() % numNeutral];
  } else if (numSafe > 0) {
    idx = safeCandidates[rand() % numSafe];
  } else if (numFallback > 0) {
    idx = fallbackCandidates[rand() % numFallback];
  }

  SDL_Log("[starts] chose start %d (%d,%d)", idx, (*value)->item[idx].x, (*value)->item[idx].y);
  startsScatterFind(sim, (*value)->item[idx].x, (*value)->item[idx].y, x, y);
  bt = startsConvertDir((*value)->item[idx].dir);
  *dir = (TURNTYPE)(bt * START_TIMES_16);
}

/*********************************************************
*NAME:          startsGetStart
*AUTHOR:        John Morrison
*CREATION DATE: 7/1/99
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns a start position. Dispatches to the appropriate
*  algorithm based on game type.
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  value     - Pointer to the starts structure
*  x         - Pointer to receive X map coordinate
*  y         - Pointer to receive Y map coordinate
*  dir       - Pointer to receive direction
*  playerNum - Player number requesting the start
*********************************************************/
void startsGetStart(GameSim *sim, starts *value, BYTE *x, BYTE *y, TURNTYPE *dir, BYTE playerNum) {
  if (*value == NULL || (*value)->numStarts == 0) {
    return;
  }

  if (sim->game == gameOpen) {
    startsGetStartOpen(sim, value, x, y, dir, playerNum);
  } else {
    startsGetStartTournament(sim, value, x, y, dir, playerNum);
  }
}

/*********************************************************
*NAME:          startsGetRandStart
*AUTHOR:        John Morrison
*CREATION DATE: 7/1/99
*LAST MODIFIED: 9/4/01
*PURPOSE:
*  Returns a random start position. Was the original
*  startsGetStart() code. Now simplified and only
*  used for LGM's parachuting in. Does not collision
*  checks
*
*ARGUMENTS:
*  value     - Pointer to the starts structure
*  x         - X Value of the start
*  y         - Y Value of the start
*  dir       - Direction it is facing
*********************************************************/ 
void startsGetRandStart(GameSim *sim, starts *value, BYTE *x, BYTE *y, TURNTYPE *dir) {
  int rnd;      /* Random number */
  BYTE bt;      /* Used to convert the BMAP starts (0 = east) to my starts */

  if ((*value)->numStarts > 0) {
    rnd = rand() % (*value)->numStarts;
    *x = (*value)->item[rnd].x;
    *y = (*value)->item[rnd].y;
    bt = startsConvertDir((*value)->item[rnd].dir);
    *dir = (TURNTYPE) (bt * START_TIMES_16);
  } else {
    *x = BRADIANS_GAP;
    *y = BRADIANS_GAP;
    *dir = BRADIANS_SOUTH;
  }
}

/*********************************************************
*NAME:          startsConvertDir
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 16/1/99
*PURPOSE:
*  Converts a Bolo Map start (0 =East, 4 = North) to my
*  starts 0 = North, 4 = East etc.
*
*ARGUMENTS:
*  dir   - Direction it is facing
*********************************************************/
BYTE startsConvertDir(BYTE dir) {
  BYTE returnValue; /* Value to return */

  switch (dir) {
  case START0:
    returnValue = START4;
    break;
    case START1:
    returnValue = START3;
    break;
  case START2:
    returnValue = START2;
    break;
  case START3:
    returnValue = START1;
    break;
  case START4:
    returnValue = START0;
    break;
  case START5:
    returnValue = START15;
    break;
  case START6:
    returnValue = START14;
    break;
  case START7:
    returnValue = START13;
    break;
  case START8:
    returnValue = START12;
    break;
  case START9:
    returnValue = START11;
    break;
  case START10:
    returnValue = START10;
    break;
  case START11:
    returnValue = START9;
    break;
  case START12:
    returnValue = START8;
    break;
  case START13:
    returnValue = START7;
    break;
  case START14:
    returnValue = START6;
    break;
  default:
    /* START15 */
    returnValue = START5;
    break;
  }

  return returnValue;
}

void startsSetStartCompressData(starts *value, BYTE *buff, int dataLen) {
  memcpy(&(**value), buff, SIZEOF_STARTS);
}

/*********************************************************
*NAME:          startsSetStartNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Sets the starts data to buff from the network.
*
*ARGUMENTS:
*  value   - Pointer to the starts structure
*  buff    - Buffer of data to set starts structure to
*  dataLen - Length of the data
*********************************************************/
void startsSetStartNetData(starts *value, BYTE *buff, BYTE dataLen) {
  BYTE count = 0;
  BYTE len = 1;

  (*value)->numStarts = buff[0];
  while (count < (*value)->numStarts) {
    (*value)->item[count].x = buff[len];
    len++;
    (*value)->item[count].y = buff[len];
    len++;
    (*value)->item[count].dir = buff[len];
    len++;
    count++;
  }
}

/*********************************************************
*NAME:          startsGetStartNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Prepares the starts data for sending across the network
* Returns data length
*ARGUMENTS:
*  value - Pointer to the starts structure
*  buff  - Buffer to hold copy of starts data
*********************************************************/
BYTE startsGetStartNetData(starts *value, BYTE *buff) {
  BYTE len = 1;
  BYTE count = 0;

  buff[0] = (*value)->numStarts;
  while (count < (*value)->numStarts) {
    buff[len] = (*value)->item[count].x;
    len++;
    buff[len] = (*value)->item[count].y;
    len++;
    buff[len] = (*value)->item[count].dir;
    len++;
    count++;
  }
  return len;
}

/*********************************************************
*NAME:          startsGetMaxs
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Gets the maximum positions values of all the starts.
*  eg left most, right most etc.
*
*ARGUMENTS:
*  valuePos  - Pointer to the starts structure
*  leftPos   - Pointer to hold the left most value
*  rightPos  - Pointer to hold the right most value
*  topPos    - Pointer to hold the top most value
*  bottomPos - Pointer to hold the bottom most value
*********************************************************/
void startsGetMaxs(starts *value, int *leftPos, int *rightPos, int *topPos, int *bottomPos) {
  BYTE count; /* Looping Variable */

  *topPos = MAP_ARRAY_SIZE;
  *bottomPos = -1;
  *leftPos = MAP_ARRAY_SIZE;
  *rightPos = -1;

  count = 0;
  while (count < ((*value)->numStarts)) {
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
*NAME:          startsMoveAll
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Repositions the starts by moveX, moveY
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  moveX  - The X move amount 
*  moveY  - The Y move amount 
*********************************************************/
void startsMoveAll(starts *value, int moveX, int moveY) {
  BYTE count; /* Looping Variable */

  count = 0;
  while (count < ((*value)->numStarts)) {
    (*value)->item[count].x = (BYTE) ((*value)->item[count].x + moveX);
    (*value)->item[count].y = (BYTE) ((*value)->item[count].y + moveY);
    count++;
  }
}
