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
#include <limits.h>
#include <SDL3/SDL.h>
#include "../common/wb_log.h"
#include "bolo_rand.h"
#include "global.h"
#include "players.h"
#include "starts.h"
#include "game_sim.h"
#include "pillbox.h"
#include "bases.h"
#include "bolo_map.h"
#include "util.h"
#include "gametype.h"
#include "start_sides.h"
#include "lobby_shared_starts.h"  /* lobbySharedStartsEnabled — duplicate reservations */

/* Distance thresholds in map squares */
#define START_TANK_RANGE 1
#define START_PILL_RANGE 9
#define START_BASE_RANGE 9
/* Minimum distance a scattered spawn keeps from another live tank */
#define START_SPAWN_SEPARATION 2
/* Maximum spiral search steps */
#define START_SCATTER_MAX 1000
/* Fraction of neutral bases before we treat neutral same as own */
#define START_NEUTRAL_THRESHOLD_PCT 20
/* Batch placement score penalties for a team with a chosen side. Both are
 * larger than the hostile-near penalty (MAP_ARRAY_SIZE * 2), so a start on
 * the team's own side always outranks one it shares with another team's
 * side, and both outrank a centre start. Set either to zero and those
 * starts rank by plain distance like any other. */
#define START_SIDE_SHARED_PENALTY (MAP_ARRAY_SIZE * 4)
#define START_SIDE_CENTRE_PENALTY (MAP_ARRAY_SIZE * 8)

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
    /* Clamp dir to [0, 15]. Downstream defenders (server_sim.c,
     * client_sim.c, startsConvertDir's default case) all handle
     * out-of-range dir today, but that's a maintenance burden on
     * every future caller. Clamp once at the entry point so the
     * rest of the codebase can trust the field. */
    if (item->dir > 15) item->dir = 0;
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

static int startsMapDistance(int x1, int y1, int x2, int y2);

/*********************************************************
*NAME:          startsIsClearOfTanks
*AUTHOR:        John Morrison
*CREATION DATE: 8/9/26
*LAST MODIFIED: 8/9/26
*PURPOSE:
*  Returns whether a map square is at least
*  START_SPAWN_SEPARATION squares from every live tank
*  other than the one being placed.
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  mx        - Map X coordinate
*  my        - Map Y coordinate
*  playerNum - Slot being placed, whose own tank is
*              skipped; MAX_TANKS if there is none
*********************************************************/
static bool startsIsClearOfTanks(GameSim *sim, BYTE mx, BYTE my, BYTE playerNum) {
  BYTE count;
  WORLD wx;
  WORLD wy;

  for (count = 0; count < MAX_TANKS; count++) {
    if (count == playerNum || sim->tanks[count] == NULL) {
      continue;
    }
    tankGetWorld(&sim->tanks[count], &wx, &wy);
    if (startsMapDistance(mx, my, wx >> M_W_SHIFT_SIZE, wy >> M_W_SHIFT_SIZE) < START_SPAWN_SEPARATION) {
      return FALSE;
    }
  }
  return TRUE;
}

/*********************************************************
*NAME:          startsScatterFind
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 8/9/26
*PURPOSE:
*  Spiral-searches outward from a centre position to find
*  a valid deep-sea square with no mine. Two passes over
*  the same spiral: the first also requires the square to
*  be at least START_SPAWN_SEPARATION squares from every
*  other live tank; if nothing within START_SCATTER_MAX
*  steps satisfies that, the second pass drops the
*  separation rule so a crowded map still places the tank.
*  Falls back to the centre itself if both passes fail.
*
*ARGUMENTS:
*  sim       - Pointer to the game simulation
*  centreX   - Centre map X coordinate
*  centreY   - Centre map Y coordinate
*  outX      - Pointer to receive result X
*  outY      - Pointer to receive result Y
*  playerNum - Slot being placed, whose own tank is
*              ignored by the separation test; MAX_TANKS
*              if there is no tank to skip
*********************************************************/
static void startsScatterFind(GameSim *sim, BYTE centreX, BYTE centreY, BYTE *outX, BYTE *outY, BYTE playerNum) {
  int pass;
  int step;
  int dx;
  int dy;
  int sx;
  int sy;
  bool keepClear;

  for (pass = 0; pass < 2; pass++) {
    keepClear = (pass == 0);
    for (step = 0; step < START_SCATTER_MAX; step++) {
      utilSpiralOffset(step, &dx, &dy);
      sx = (int)centreX + dx;
      sy = (int)centreY + dy;
      if (sx > 0 && sx < MAP_ARRAY_SIZE && sy > 0 && sy < MAP_ARRAY_SIZE) {
        if (startsIsValidSquare(sim, (BYTE)sx, (BYTE)sy) &&
            (keepClear == FALSE || startsIsClearOfTanks(sim, (BYTE)sx, (BYTE)sy, playerNum))) {
          *outX = (BYTE)sx;
          *outY = (BYTE)sy;
          return;
        }
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
*  start positions looking for one with no nearby tanks and
*  no enemy or neutral pillboxes (friendly pills are fine).
*  Falls back to a position near only friendly units, then
*  to any valid start. Uses spiral scatter to find a nearby
*  valid deep-sea square.
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
  int friendlyChoice; /* Tier 2: a start with only friendly units nearby */
  int fallbackChoice; /* Tier 3: a start with hostile units nearby (last resort) */
  int chosen;
  bool anyTankNearby;
  bool nonFriendlyPillNearby; /* enemy or neutral pill within range */
  bool hostileTankNearby;
  bool hostilePillNearby;
  BYTE tankCount;
  BYTE pillCount;
  WORLD tankWX;
  WORLD tankWY;
  int dist;
  BYTE pillOwner;
  BYTE bt;

  friendlyChoice = -1;
  fallbackChoice = -1;
  offset = (BYTE)bolo_rand_below((uint32_t)numStarts);

  for (count = 0; count < numStarts; count++) {
    idx = (BYTE)((offset + count) % numStarts);
    sx = (*value)->item[idx].x;
    sy = (*value)->item[idx].y;

    /* Must be deep sea with no mine */
    if (startsIsValidSquare(sim, sx, sy) == FALSE) {
      continue;
    }

    anyTankNearby = FALSE;
    nonFriendlyPillNearby = FALSE;
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
        pillOwner = sim->pb->item[pillCount].owner;
        if (startsIsOwnerFriendly(sim, pillOwner, playerNum) == FALSE) {
          /* Neutral or enemy: disqualifies "ideal" */
          nonFriendlyPillNearby = TRUE;
          if (pillOwner != NEUTRAL) {
            hostilePillNearby = TRUE;
          }
        }
      }
    }

    /* Ideal: no tanks nearby and no enemy/neutral pills (friendly pills ok) */
    if (anyTankNearby == FALSE && nonFriendlyPillNearby == FALSE) {
      startsScatterFind(sim, sx, sy, x, y, playerNum);
      bt = startsConvertDir((*value)->item[idx].dir);
      *dir = (TURNTYPE)(bt * START_TIMES_16);
      return;
    }

    /* Good: only friendly units nearby */
    if (hostileTankNearby == FALSE && hostilePillNearby == FALSE) {
      if (friendlyChoice == -1) {
        friendlyChoice = idx;
      }
      continue;
    }

    /* Fallback: at least it's a valid square */
    if (fallbackChoice == -1) {
      fallbackChoice = idx;
    }
  }

  /* Pick the best available tier: prefer friendly-only over hostile-near.
   * Tracking these separately matters when the random offset hits a hostile
   * start before a friendly one — without separate tiers, hostile would
   * win just because of iteration order. */
  if (friendlyChoice >= 0) chosen = friendlyChoice;
  else if (fallbackChoice >= 0) chosen = fallbackChoice;
  else chosen = 0;

  /* Phase 3: scatter search around chosen position */
  startsScatterFind(sim, (*value)->item[chosen].x, (*value)->item[chosen].y, x, y, playerNum);
  bt = startsConvertDir((*value)->item[chosen].dir);
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
      WB_LOG_DEBUG(WB_LOG_CAT_SIM, "[starts] start %d (%d,%d): HOSTILE (tank=%d pill=%d base=%d)", count, sx, sy,
              hostileTankNearby, hostilePillNearby, hostileBaseNearby);
      fallbackCandidates[numFallback++] = count;
      continue;
    }

    WB_LOG_DEBUG(WB_LOG_CAT_SIM, "[starts] start %d (%d,%d): safe (ownBase=%d neutralBase=%d anyTank=%d anyPill=%d)",
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
  WB_LOG_DEBUG(WB_LOG_CAT_SIM, "[starts] player %d: neutralPref=%d own=%d neutral=%d safe=%d fallback=%d (neutralBases=%d/%d)",
          playerNum, neutralPreferred, numOwn, numNeutral, numSafe, numFallback, neutralCount, numBases);
  idx = 0;
  if (neutralPreferred && (numOwn > 0 || numNeutral > 0)) {
    BYTE pool[MAX_STARTS * 2];
    BYTE poolSize = 0;
    BYTE i;
    for (i = 0; i < numOwn; i++) pool[poolSize++] = ownCandidates[i];
    for (i = 0; i < numNeutral; i++) pool[poolSize++] = neutralCandidates[i];
    idx = pool[bolo_rand_below((uint32_t)poolSize)];
  } else if (numOwn > 0) {
    idx = ownCandidates[bolo_rand_below((uint32_t)numOwn)];
  } else if (numNeutral > 0) {
    idx = neutralCandidates[bolo_rand_below((uint32_t)numNeutral)];
  } else if (numSafe > 0) {
    idx = safeCandidates[bolo_rand_below((uint32_t)numSafe)];
  } else if (numFallback > 0) {
    idx = fallbackCandidates[bolo_rand_below((uint32_t)numFallback)];
  }

  WB_LOG_DEBUG(WB_LOG_CAT_SIM, "[starts] chose start %d (%d,%d)", idx, (*value)->item[idx].x, (*value)->item[idx].y);
  startsScatterFind(sim, (*value)->item[idx].x, (*value)->item[idx].y, x, y, playerNum);
  bt = startsConvertDir((*value)->item[idx].dir);
  *dir = (TURNTYPE)(bt * START_TIMES_16);
}

/*********************************************************
*NAME:          startsHasHostileNearAtStart
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns whether start index startIdx has any hostile
*  pillbox or base within range, treating playerNum (and
*  any allies) as the "friendly" reference. Used by the
*  batch placement to penalise hostile-near candidates.
*********************************************************/
static bool startsHasHostileNearAtStart(GameSim *sim, starts *value, BYTE startIdx, BYTE playerNum) {
  BYTE numPills = pillsGetNumPills(&sim->pb);
  BYTE numBases = basesGetNumBases(&sim->bs);
  BYTE sx = (*value)->item[startIdx].x;
  BYTE sy = (*value)->item[startIdx].y;
  BYTE i;
  int dist;
  BYTE owner;

  for (i = 0; i < numPills; i++) {
    if (sim->pb->item[i].inTank == TRUE || sim->pb->item[i].armour == 0) {
      continue;
    }
    dist = startsMapDistance(sx, sy, sim->pb->item[i].x, sim->pb->item[i].y);
    if (dist > START_PILL_RANGE) continue;
    owner = sim->pb->item[i].owner;
    if (owner != NEUTRAL && startsIsOwnerFriendly(sim, owner, playerNum) == FALSE) {
      return TRUE;
    }
  }
  for (i = 0; i < numBases; i++) {
    if (sim->bs->item[i].armour <= MIN_ARMOUR_CAPTURE) continue;
    dist = startsMapDistance(sx, sy, sim->bs->item[i].x, sim->bs->item[i].y);
    if (dist > START_BASE_RANGE) continue;
    owner = sim->bs->item[i].owner;
    if (owner != NEUTRAL && startsIsOwnerFriendly(sim, owner, playerNum) == FALSE) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Internal grouping structure for batch placement. */
typedef struct {
  BYTE players[MAX_TANKS]; /* Player indices in this group */
  BYTE size;
  bool isSolo;             /* TRUE if this is a single-player solo group */
  bool anchored;           /* TRUE once anchorX/anchorY are populated */
  int  anchorX;
  int  anchorY;
} StartsBatchGroup;

/* Insertion-sort group indices in `order` by group size descending. */
static void startsBatchSortBySize(int *order, int n, const StartsBatchGroup *groups) {
  int i;
  int j;
  int key;
  for (i = 1; i < n; i++) {
    key = order[i];
    j = i - 1;
    while (j >= 0 && groups[order[j]].size < groups[key].size) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }
}

/* Side mask of every start, classified against the bounding box of all
 * the starts (see start_sides.h). sideMask holds numStarts entries. */
static void startsSideMasks(starts *value, BYTE *sideMask) {
  int leftPos;
  int rightPos;
  int topPos;
  int bottomPos;
  BYTE i;
  startsGetMaxs(value, &leftPos, &rightPos, &topPos, &bottomPos);
  for (i = 0; i < (*value)->numStarts; i++) {
    sideMask[i] = startSideMaskFor((*value)->item[i].x, (*value)->item[i].y,
                                   leftPos, topPos, rightPos, bottomPos);
  }
}

/* Distance from start idx to the nearest claimed start, or MAP_ARRAY_SIZE
 * when nothing is claimed so that every start ties. */
static int startsBatchMinDistToClaimed(starts *value, BYTE numStarts,
                                       const bool *startClaimed, BYTE idx) {
  int minD = INT_MAX;
  BYTE j;
  for (j = 0; j < numStarts; j++) {
    int d;
    if (!startClaimed[j]) continue;
    d = startsMapDistance((*value)->item[idx].x, (*value)->item[idx].y,
                          (*value)->item[j].x, (*value)->item[j].y);
    if (d < minD) minD = d;
  }
  if (minD == INT_MAX) minD = MAP_ARRAY_SIZE;
  return minD;
}

/*********************************************************
*NAME:          startsAssignBatch
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Computes start positions for every connected player in a
*  single pass, used at the lobby->game transition where the
*  per-player algorithm cannot see siblings being created in
*  the same batch.
*
*  Players sharing a non-zero teamNumber are grouped and
*  placed near each other; teams without map-encoded base
*  ownership are assigned a stripe along the map's long
*  axis (so 2 teams split top/bottom or left/right rather
*  than corner-vs-corner). Solo players (teamNumber == 0)
*  are slotted in afterwards via farthest-first to maximise
*  spacing from teams and other solos.
*
*  Existing per-position rules (avoid hostile pills/bases,
*  scatter to a valid deep-sea square) are layered on top
*  of the team assignment.
*
*  A team with a chosen side (teamStartSide) is anchored on
*  that side and takes only starts its side accepts: its own
*  side first, then starts shared with another team's side,
*  then the centre. A chosen side is closed to every team
*  that did not choose it, and to solos. A side with no valid
*  start falls back to no side for the pass.
*
*  Every connected slot the placement leaves without a start
*  of its own rides one that is already taken — a teammate's
*  where it has one — so two slots may share an index. The
*  tank-aware scatter in startsGetStart spreads the riders
*  out when their tanks are created.
*
*ARGUMENTS:
*  sim          - Game simulation
*  value        - Starts structure
*  connected    - [MAX_TANKS] which player slots are joining
*  teamNumber   - [MAX_TANKS] team for each slot (0 = solo)
*  outStartIdx  - [MAX_TANKS] receives the chosen start index
*                 per slot (0..MAX_STARTS-1), or MAX_STARTS
*                 only when no start is valid at all (caller
*                 should fall back to the per-player
*                 algorithm). Scatter and direction conversion
*                 happen later, when startsGetStart consumes
*                 the slot.
*  reservedStartIdx0 - [MAX_TANKS] optional pre-reserved start
*                 per slot, 0-based (MAX_STARTS = none), or NULL
*                 for no reservations. A reserved slot locks its
*                 exact start and is excluded from placement.
*                 Several slots may name the same start when
*                 LOBBY_SHARED_STARTS is on; they all come out
*                 on it.
*  teamStartSide - [MAX_TANKS + 1] START_SIDE_* per team number
*                 (entry 0 unused), or NULL for no sides anywhere;
*                 a NULL table treats every team as START_SIDE_ANY.
*********************************************************/
void startsAssignBatch(GameSim *sim, starts *value,
                       const bool *connected, const BYTE *teamNumber,
                       BYTE *outStartIdx, const BYTE *reservedStartIdx0,
                       const BYTE *teamStartSide) {
  StartsBatchGroup groups[MAX_TANKS];
  int teamToGroup[MAX_TANKS + 1]; /* teamNumber 1..16 -> group index, -1 if unseen */
  int slotGroup[MAX_TANKS];       /* slot -> group index, -1 if reserved or absent */
  int unanchored[MAX_TANKS];
  int teamOrder[MAX_TANKS];
  int stripeCentX[MAX_TANKS];
  int stripeCentY[MAX_TANKS];
  int stripeCount[MAX_TANKS];
  bool stripeUsed[MAX_TANKS];
  bool startClaimed[MAX_STARTS];
  BYTE startToPlayer[MAX_STARTS];
  int riders[MAX_STARTS];           /* slots riding a start on top of its claimant */
  bool slotReserved[MAX_TANKS];     /* slot holds an honored reservation */
  bool reservedLocked[MAX_STARTS];  /* 0-based start already locked by a reservation */
  int reservedSumX[MAX_TANKS];      /* per-group reserved-start centroid accumulator */
  int reservedSumY[MAX_TANKS];
  int reservedCnt[MAX_TANKS];
  BYTE sideMask[MAX_STARTS];        /* side bits per start */
  BYTE groupSide[MAX_TANKS];        /* per-group START_SIDE_*, after the no-valid-start fallback */
  BYTE groupOtherMask[MAX_TANKS];   /* per-group union of the other teams' chosen sides */
  BYTE closedMask;                  /* union of every present team's chosen side */
  int leftPos;
  int rightPos;
  int topPos;
  int bottomPos;
  int spanX;
  int spanY;
  int numGroups;
  int numUnanchored;
  int numTeams;
  BYTE numStarts;
  BYTE numBases;
  BYTE i;
  int g;
  int s;
  int t;
  int p;
  int b;
  int sumX;
  int sumY;
  int cnt;

  for (i = 0; i < MAX_TANKS; i++) {
    outStartIdx[i] = MAX_STARTS;
  }
  if (*value == NULL || (*value)->numStarts == 0) {
    return;
  }
  numStarts = (*value)->numStarts;

  /* Decide which reservations to honor. A connected slot with a valid
   * 0-based reservation is honored; an out-of-range (stale) reservation
   * falls through to ordinary placement. Duplicates depend on the shared
   * starts flag: with it on every slot naming a start keeps it and they
   * all spawn there (the tank-aware scatter spreads them out), with it
   * off the first claimant keeps it and the rest are placed normally. */
  for (i = 0; i < MAX_TANKS; i++) slotReserved[i] = FALSE;
  for (i = 0; i < MAX_STARTS; i++) reservedLocked[i] = FALSE;
  if (reservedStartIdx0 != NULL) {
    for (i = 0; i < MAX_TANKS; i++) {
      BYTE r;
      if (!connected[i]) continue;
      r = reservedStartIdx0[i];
      if (r >= numStarts) continue;       /* MAX_STARTS sentinel or stale index */
      if (reservedLocked[r] && !lobbySharedStartsEnabled()) {
        continue;                          /* duplicate: honor the first */
      }
      reservedLocked[r] = TRUE;
      slotReserved[i] = TRUE;
    }
  }

  /* Step 0: the side of every start, and the sides the teams in this batch
   * chose. A team closes its side only when it has a connected member. */
  startsSideMasks(value, sideMask);
  closedMask = 0;
  if (teamStartSide != NULL) {
    for (i = 0; i < MAX_TANKS; i++) {
      BYTE tn;
      if (!connected[i]) continue;
      tn = teamNumber[i];
      if (tn > 0 && tn <= MAX_TANKS) closedMask |= startSideBits(teamStartSide[tn]);
    }
  }

  /* Step 1: build groups (reserved slots are placed by the lock below, not
   * by the cluster passes, so they stay out of the groups). */
  numGroups = 0;
  for (i = 0; i <= MAX_TANKS; i++) teamToGroup[i] = -1;
  for (i = 0; i < MAX_TANKS; i++) slotGroup[i] = -1;
  for (i = 0; i < MAX_TANKS; i++) {
    BYTE tn;
    if (!connected[i]) continue;
    if (slotReserved[i]) continue;
    tn = teamNumber[i];
    if (tn > 0 && tn <= MAX_TANKS && teamToGroup[tn] >= 0) {
      g = teamToGroup[tn];
    } else {
      g = numGroups++;
      groups[g].size = 0;
      groups[g].anchored = FALSE;
      groups[g].isSolo = (tn == 0);
      groups[g].anchorX = 0;
      groups[g].anchorY = 0;
      groupSide[g] = START_SIDE_ANY;
      if (tn > 0 && tn <= MAX_TANKS) {
        teamToGroup[tn] = g;
        if (teamStartSide != NULL && startSideBits(teamStartSide[tn]) != 0) {
          groupSide[g] = teamStartSide[tn];
        }
      }
    }
    groups[g].players[groups[g].size++] = i;
    slotGroup[i] = g;
  }

  /* Per-group side rules. A team keeps its own side open and is closed off
   * the sides the other teams chose; a team with no side, and a solo, is
   * closed off every chosen side. Two fallbacks keep a team from being left
   * with nothing: a side that accepts no valid start drops the team to no
   * side for this pass, and a team with no side that is closed off every
   * unclaimed valid start has its closed set cleared instead. */
  for (g = 0; g < numGroups; g++) {
    bool found;
    groupOtherMask[g] = (BYTE)(closedMask & ~startSideBits(groupSide[g]));
    if (groups[g].isSolo) continue;
    if (groupSide[g] != START_SIDE_ANY) {
      found = FALSE;
      for (i = 0; i < numStarts && !found; i++) {
        if (startSideAccepts(sideMask[i], groupSide[g]) &&
            startsIsValidSquare(sim, (*value)->item[i].x, (*value)->item[i].y)) {
          found = TRUE;
        }
      }
      if (!found) {
        WB_LOG_DEBUG(WB_LOG_CAT_SIM, "[starts] team %d: side %d has no valid start, placing it on any side",
                     teamNumber[groups[g].players[0]], groupSide[g]);
        groupSide[g] = START_SIDE_ANY;
      }
    }
    if (groupSide[g] == START_SIDE_ANY && groupOtherMask[g] != 0) {
      found = FALSE;
      for (i = 0; i < numStarts && !found; i++) {
        if (reservedLocked[i]) continue;
        if (startSideEligible(sideMask[i], START_SIDE_ANY, groupOtherMask[g]) &&
            startsIsValidSquare(sim, (*value)->item[i].x, (*value)->item[i].y)) {
          found = TRUE;
        }
      }
      if (!found) {
        WB_LOG_DEBUG(WB_LOG_CAT_SIM, "[starts] team %d: every unclaimed start is on a chosen side, opening them all",
                     teamNumber[groups[g].players[0]]);
        groupOtherMask[g] = 0;
      }
    }
  }

  /* Accumulate each team group's reserved-start centroid so the anchor
   * override below can pull its unreserved members near their locked
   * teammates. Reserved solo slots (team 0) have no group and are skipped.
   * Slots sharing one start each add it, so a start two teammates picked
   * pulls the anchor toward it twice — which is where the team is. */
  for (g = 0; g < numGroups; g++) {
    reservedSumX[g] = 0;
    reservedSumY[g] = 0;
    reservedCnt[g] = 0;
  }
  for (i = 0; i < MAX_TANKS; i++) {
    BYTE tn;
    if (!connected[i] || !slotReserved[i]) continue;
    tn = teamNumber[i];
    if (tn == 0 || tn > MAX_TANKS || teamToGroup[tn] < 0) continue;
    g = teamToGroup[tn];
    reservedSumX[g] += (*value)->item[reservedStartIdx0[i]].x;
    reservedSumY[g] += (*value)->item[reservedStartIdx0[i]].y;
    reservedCnt[g]++;
  }

  /* Step 2: anchors from owned bases (teams only) */
  numBases = basesGetNumBases(&sim->bs);
  for (g = 0; g < numGroups; g++) {
    if (groups[g].isSolo) continue;
    sumX = 0; sumY = 0; cnt = 0;
    for (b = 0; b < numBases; b++) {
      BYTE owner = sim->bs->item[b].owner;
      if (sim->bs->item[b].armour <= MIN_ARMOUR_CAPTURE) continue;
      if (owner == NEUTRAL) continue;
      for (p = 0; p < groups[g].size; p++) {
        if (groups[g].players[p] == owner) {
          sumX += sim->bs->item[b].x;
          sumY += sim->bs->item[b].y;
          cnt++;
          break;
        }
      }
    }
    if (cnt > 0) {
      groups[g].anchored = TRUE;
      groups[g].anchorX = sumX / cnt;
      groups[g].anchorY = sumY / cnt;
    }
  }

  /* Step 2b: a team with a side is anchored at the centroid of that side's
   * valid starts, over any base anchor. When only centre starts accept the
   * side, their centroid is used instead. */
  for (g = 0; g < numGroups; g++) {
    BYTE bits;
    int centreSumX = 0;
    int centreSumY = 0;
    int centreCnt = 0;
    if (groups[g].isSolo || groupSide[g] == START_SIDE_ANY) continue;
    bits = startSideBits(groupSide[g]);
    sumX = 0; sumY = 0; cnt = 0;
    for (i = 0; i < numStarts; i++) {
      if (startsIsValidSquare(sim, (*value)->item[i].x, (*value)->item[i].y) == FALSE) continue;
      if ((sideMask[i] & bits) != 0) {
        sumX += (*value)->item[i].x;
        sumY += (*value)->item[i].y;
        cnt++;
      } else if (startSideIsCentre(sideMask[i])) {
        centreSumX += (*value)->item[i].x;
        centreSumY += (*value)->item[i].y;
        centreCnt++;
      }
    }
    if (cnt == 0) {
      sumX = centreSumX;
      sumY = centreSumY;
      cnt = centreCnt;
    }
    if (cnt > 0) {
      groups[g].anchored = TRUE;
      groups[g].anchorX = sumX / cnt;
      groups[g].anchorY = sumY / cnt;
    }
  }

  /* Step 3: stripe-place unanchored teams along the map's long axis. A team
   * with a side was anchored on it above and never takes a stripe. */
  startsGetMaxs(value, &leftPos, &rightPos, &topPos, &bottomPos);
  spanX = rightPos - leftPos;
  spanY = bottomPos - topPos;
  numUnanchored = 0;
  for (g = 0; g < numGroups; g++) {
    if (!groups[g].isSolo && !groups[g].anchored && groupSide[g] == START_SIDE_ANY) {
      unanchored[numUnanchored++] = g;
    }
  }
  if (numUnanchored > 0) {
    /* Partition the bbox into a roughly-square grid (divX * divY >= N).
     * One-dimensional stripes don't separate corner clusters: e.g. on a
     * square map with 4 teams and clusters in each corner, splitting only
     * along X gives 4 vertical stripes that each span both the top and
     * bottom rows, mixing teammates across opposite corners. A 2x2 grid
     * gives one corner per team. Orientation is biased toward the long
     * axis so wide maps still get more X-divisions than Y. */
    int divX;
    int divY;
    int numCells;
    startsBatchSortBySize(unanchored, numUnanchored, groups);
    if (numUnanchored == 1) {
      divX = 1;
      divY = 1;
    } else if (spanX >= spanY) {
      divX = 1;
      while (divX * divX < numUnanchored) divX++;
      divY = (numUnanchored + divX - 1) / divX;
    } else {
      divY = 1;
      while (divY * divY < numUnanchored) divY++;
      divX = (numUnanchored + divY - 1) / divY;
    }
    numCells = divX * divY;
    if (numCells > MAX_TANKS) numCells = MAX_TANKS;

    for (s = 0; s < numCells; s++) {
      stripeCentX[s] = 0;
      stripeCentY[s] = 0;
      stripeCount[s] = 0;
      stripeUsed[s] = FALSE;
    }
    for (i = 0; i < numStarts; i++) {
      int sx = (*value)->item[i].x;
      int sy = (*value)->item[i].y;
      int cellX;
      int cellY;
      int cellIdx;
      if (spanX <= 0) cellX = 0;
      else {
        cellX = (sx - leftPos) * divX / (spanX + 1);
        if (cellX >= divX) cellX = divX - 1;
        if (cellX < 0) cellX = 0;
      }
      if (spanY <= 0) cellY = 0;
      else {
        cellY = (sy - topPos) * divY / (spanY + 1);
        if (cellY >= divY) cellY = divY - 1;
        if (cellY < 0) cellY = 0;
      }
      cellIdx = cellY * divX + cellX;
      if (cellIdx >= numCells) continue;
      stripeCentX[cellIdx] += sx;
      stripeCentY[cellIdx] += sy;
      stripeCount[cellIdx]++;
    }
    for (s = 0; s < numCells; s++) {
      if (stripeCount[s] > 0) {
        stripeCentX[s] /= stripeCount[s];
        stripeCentY[s] /= stripeCount[s];
      } else {
        /* Empty cell — synthesise centroid at the cell midpoint */
        int cx = s % divX;
        int cy = s / divX;
        stripeCentX[s] = leftPos + (cx * 2 + 1) * spanX / (divX * 2);
        stripeCentY[s] = topPos + (cy * 2 + 1) * spanY / (divY * 2);
      }
    }
    /* Largest unanchored team picks the cell with most starts */
    for (t = 0; t < numUnanchored; t++) {
      int bestS = -1;
      int bestCount = -1;
      for (s = 0; s < numCells; s++) {
        if (!stripeUsed[s] && stripeCount[s] > bestCount) {
          bestS = s;
          bestCount = stripeCount[s];
        }
      }
      if (bestS >= 0) {
        int jitterX;
        int jitterY;
        stripeUsed[bestS] = TRUE;
        g = unanchored[t];
        groups[g].anchored = TRUE;
        groups[g].anchorX = stripeCentX[bestS];
        groups[g].anchorY = stripeCentY[bestS];
        /* Jitter the anchor by up to a quarter-cell so the cluster sits
         * somewhere different each game without leaving its region. Members
         * pick closest-to-anchor independently, so the anchor is the only
         * lever that moves the whole cluster intact; jittering a member
         * instead would just fling one teammate away from the group. */
        jitterX = spanX / (divX * 4);
        jitterY = spanY / (divY * 4);
        if (jitterX > 0) {
          groups[g].anchorX += (int)bolo_rand_below((uint32_t)(jitterX * 2 + 1)) - jitterX;
        }
        if (jitterY > 0) {
          groups[g].anchorY += (int)bolo_rand_below((uint32_t)(jitterY * 2 + 1)) - jitterY;
        }
      }
    }
  }

  /* Anchor override: a team with locked reservations seeds its group anchor
   * from the centroid of those reserved starts, so its last unreserved member
   * clusters with its already-placed teammates instead of scattering. A team
   * with a side keeps its side anchor, so one member's off-side reservation
   * does not pull the rest of the team after it. */
  for (g = 0; g < numGroups; g++) {
    if (groups[g].isSolo || reservedCnt[g] == 0) continue;
    if (groupSide[g] != START_SIDE_ANY) continue;
    groups[g].anchored = TRUE;
    groups[g].anchorX = reservedSumX[g] / reservedCnt[g];
    groups[g].anchorY = reservedSumY[g] / reservedCnt[g];
  }

  /* Step 4: assign starts to teams, largest first.
   * When starts are scarce (sum of team sizes > valid starts), apportion
   * via Hamilton's method: floor each team's quota and distribute leftover
   * starts by largest fractional remainder, ties broken by team size.
   * Without this, a greedy "largest team takes its full size first" would
   * starve smaller teams entirely (e.g. 8 starts vs two 8-player teams).
   * Each quota is then capped at the starts the team's side rules let it
   * take, and what the cap frees goes to the teams still short; otherwise
   * a large team on a small side would hold starts it can never use. */
  {
    int totalTeamPlayers = 0;
    int validStartCount = 0;   /* valid starts no reservation has locked */
    int totalDesired = 0;
    int teamClaim[MAX_TANKS];
    int eligible[MAX_TANKS];   /* unclaimed valid starts the team may take */
    int desired[MAX_TANKS];    /* min(team size, eligible) */
    for (g = 0; g < numGroups; g++) {
      teamClaim[g] = 0;
      eligible[g] = 0;
      desired[g] = 0;
      if (!groups[g].isSolo) totalTeamPlayers += groups[g].size;
    }

    for (i = 0; i < MAX_STARTS; i++) {
      startClaimed[i] = FALSE;
      startToPlayer[i] = MAX_TANKS;
    }
    /* Lock honored reservations: pre-claim each reserved start for its slot
     * so the placement passes below skip it; Step 6 emits the slot's
     * outStartIdx from startToPlayer for free. When several slots share a
     * start the lowest of them stands for it here — startToPlayer names one
     * slot per start, and the rider passes below read it for the team a
     * start belongs to; Step 6 emits the rest from their reservations. */
    for (i = 0; i < MAX_TANKS; i++) {
      BYTE r;
      if (!connected[i] || !slotReserved[i]) continue;
      r = reservedStartIdx0[i];
      startClaimed[r] = TRUE;
      if (startToPlayer[r] >= MAX_TANKS) startToPlayer[r] = i;
    }
    /* Count what is left to hand out, in total and per team. */
    for (i = 0; i < numStarts; i++) {
      if (startClaimed[i]) continue;
      if (startsIsValidSquare(sim, (*value)->item[i].x, (*value)->item[i].y) == FALSE) continue;
      validStartCount++;
      for (g = 0; g < numGroups; g++) {
        if (groups[g].isSolo) continue;
        if (startSideEligible(sideMask[i], groupSide[g], groupOtherMask[g])) eligible[g]++;
      }
    }
    for (g = 0; g < numGroups; g++) {
      if (groups[g].isSolo) continue;
      desired[g] = (groups[g].size < eligible[g]) ? groups[g].size : eligible[g];
      totalDesired += desired[g];
    }
    numTeams = 0;
    for (g = 0; g < numGroups; g++) {
      if (!groups[g].isSolo) teamOrder[numTeams++] = g;
    }
    startsBatchSortBySize(teamOrder, numTeams, groups);

    if (validStartCount >= totalDesired || totalDesired == 0) {
      /* Plenty of starts — every team claims all it may take */
      for (t = 0; t < numTeams; t++) {
        teamClaim[teamOrder[t]] = desired[teamOrder[t]];
      }
    } else {
      /* Scarce: Hamilton apportionment.
       * floor and remainder are kept as integers via the *T trick:
       *   exact   = size * S / T
       *   floor_g = (size * S) / T          (integer division)
       *   rem_g   = (size * S) - floor_g*T  (in [0, T-1])
       */
      int floors[MAX_TANKS];
      int rems[MAX_TANKS];
      int totalFloor = 0;
      int leftover;
      for (t = 0; t < numTeams; t++) {
        int idx = teamOrder[t];
        int prod = (int)groups[idx].size * validStartCount;
        floors[idx] = prod / totalTeamPlayers;
        rems[idx] = prod - floors[idx] * totalTeamPlayers;
        teamClaim[idx] = floors[idx];
        totalFloor += floors[idx];
      }
      leftover = validStartCount - totalFloor;
      while (leftover > 0) {
        int bestG = -1;
        int bestRem = -1;
        int bestSize = -1;
        for (t = 0; t < numTeams; t++) {
          int idx = teamOrder[t];
          if (teamClaim[idx] >= groups[idx].size) continue;
          if (rems[idx] > bestRem ||
              (rems[idx] == bestRem && groups[idx].size > bestSize)) {
            bestG = idx;
            bestRem = rems[idx];
            bestSize = groups[idx].size;
          }
        }
        if (bestG < 0) break;
        teamClaim[bestG]++;
        rems[bestG] = -1; /* don't pick the same team for the next leftover */
        leftover--;
      }

      /* Cap each quota at what the team may take, then hand the freed
       * starts one at a time to the teams still short, largest first,
       * until nothing is freed or nobody can take more. */
      {
        int freed = 0;
        bool gave = TRUE;
        for (t = 0; t < numTeams; t++) {
          int idx = teamOrder[t];
          if (teamClaim[idx] > desired[idx]) {
            freed += teamClaim[idx] - desired[idx];
            teamClaim[idx] = desired[idx];
          }
        }
        while (freed > 0 && gave) {
          gave = FALSE;
          for (t = 0; t < numTeams && freed > 0; t++) {
            int idx = teamOrder[t];
            if (teamClaim[idx] >= desired[idx]) continue;
            teamClaim[idx]++;
            freed--;
            gave = TRUE;
          }
        }
      }
    }

    for (t = 0; t < numTeams; t++) {
      g = teamOrder[t];
      for (p = 0; p < teamClaim[g]; p++) {
      int bestStart = -1;
      int bestScore = -1;
      BYTE rep = groups[g].players[0];
      for (i = 0; i < numStarts; i++) {
        int dist;
        int score;
        if (startClaimed[i]) continue;
        if (startsIsValidSquare(sim, (*value)->item[i].x, (*value)->item[i].y) == FALSE) continue;
        if (!startSideEligible(sideMask[i], groupSide[g], groupOtherMask[g])) continue;
        dist = startsMapDistance((*value)->item[i].x, (*value)->item[i].y,
                                 groups[g].anchorX, groups[g].anchorY);
        score = dist;
        if (startsHasHostileNearAtStart(sim, value, i, rep)) {
          /* Push hostile-near candidates well below distance ranking */
          score += MAP_ARRAY_SIZE * 2;
        }
        if (groupSide[g] != START_SIDE_ANY) {
          /* A side team takes its own side's starts first, then starts
           * another team's side also covers, then the centre. */
          if ((sideMask[i] & groupOtherMask[g]) != 0) score += START_SIDE_SHARED_PENALTY;
          if (startSideIsCentre(sideMask[i])) score += START_SIDE_CENTRE_PENALTY;
        }
        if (bestStart < 0 || score < bestScore) {
          bestStart = i;
          bestScore = score;
        }
      }
      if (bestStart >= 0) {
        startClaimed[bestStart] = TRUE;
        startToPlayer[bestStart] = groups[g].players[p];
      }
      }
    }
  }

  /* Step 5: solos via farthest-first from already-claimed starts.
   * Farthest-first is a chain: each pick is measured against what's already
   * claimed, so picks 2..N follow deterministically from the first. We keep
   * every candidate tied for the best min-distance and choose randomly among
   * them, which (a) breaks the degenerate "nothing claimed yet" case where
   * all valid starts tie at MAP_ARRAY_SIZE — that is the single-player game,
   * which otherwise always picked the lowest-index start — and (b) varies the
   * seed so the whole spread differs between games while staying maximal. */
  for (g = 0; g < numGroups; g++) {
    int bestMinDist = -1;
    BYTE bestCandidates[MAX_STARTS];
    BYTE numBest = 0;
    BYTE soloPlayer;
    if (!groups[g].isSolo) continue;
    soloPlayer = groups[g].players[0];
    for (i = 0; i < numStarts; i++) {
      int minD = INT_MAX;
      BYTE j;
      if (startClaimed[i]) continue;
      if (startsIsValidSquare(sim, (*value)->item[i].x, (*value)->item[i].y) == FALSE) continue;
      if (!startSideEligible(sideMask[i], START_SIDE_ANY, closedMask)) continue;
      for (j = 0; j < numStarts; j++) {
        int d;
        if (!startClaimed[j]) continue;
        d = startsMapDistance((*value)->item[i].x, (*value)->item[i].y,
                              (*value)->item[j].x, (*value)->item[j].y);
        if (d < minD) minD = d;
      }
      if (minD == INT_MAX) minD = MAP_ARRAY_SIZE; /* no claims yet — any start is "infinitely far" */
      if (numBest == 0 || minD > bestMinDist) {
        bestMinDist = minD;
        numBest = 0;
        bestCandidates[numBest++] = i;
      } else if (minD == bestMinDist) {
        bestCandidates[numBest++] = i;
      }
    }
    if (numBest > 0) {
      BYTE bestStart = bestCandidates[bolo_rand_below((uint32_t)numBest)];
      startClaimed[bestStart] = TRUE;
      startToPlayer[bestStart] = soloPlayer;
    }
  }

  /* Step 6: emit chosen start indices. Scatter and direction conversion
   * are deferred to startsGetStart so they account for sibling tanks that
   * are placed earlier in the same batch loop. */
  for (i = 0; i < numStarts; i++) {
    BYTE pl;
    if (!startClaimed[i]) continue;
    pl = startToPlayer[i];
    if (pl >= MAX_TANKS) continue;
    outStartIdx[pl] = i;
  }
  /* Every honored reservation emits its own start, not just the slot
   * startToPlayer remembers for it — that is how slots sharing a start all
   * come out on it. With one slot per start this repeats what the loop
   * above already wrote. */
  if (reservedStartIdx0 != NULL) {
    for (i = 0; i < MAX_TANKS; i++) {
      if (!connected[i] || !slotReserved[i]) continue;
      outStartIdx[i] = reservedStartIdx0[i];
    }
  }

  /* Step 7: every connected slot still without a start rides one. Free
   * starts go first: a slot with a free valid start its side rules allow
   * claims it outright, so nobody shares a start while one sits free. A
   * solo reaching this point has none (Step 5 looked already), so only
   * team members claim here, nearest their team anchor. */
  for (i = 0; i < MAX_TANKS; i++) {
    int best = -1;
    int bestDist = 0;
    if (!connected[i] || outStartIdx[i] != MAX_STARTS) continue;
    g = slotGroup[i];
    if (g < 0 || groups[g].isSolo) continue;
    for (s = 0; s < numStarts; s++) {
      int d;
      if (startClaimed[s]) continue;
      if (startsIsValidSquare(sim, (*value)->item[s].x, (*value)->item[s].y) == FALSE) continue;
      if (!startSideEligible(sideMask[s], groupSide[g], groupOtherMask[g])) continue;
      d = startsMapDistance((*value)->item[s].x, (*value)->item[s].y,
                            groups[g].anchorX, groups[g].anchorY);
      if (best < 0 || d < bestDist) {
        best = s;
        bestDist = d;
      }
    }
    if (best >= 0) {
      startClaimed[best] = TRUE;
      startToPlayer[best] = i;
      outStartIdx[i] = (BYTE)best;
    }
  }

  /* Then the riders. A team member rides the least-ridden of its team's
   * claimed starts, nearest the team anchor on a tie; a member of a team
   * that claimed nothing rides the start its side rules allow nearest the
   * team anchor. Anyone else — a solo, or a member with no start its rules
   * allow — rides the least-ridden valid start, farthest from the claimed
   * starts on a tie, staying off the chosen sides while any start outside
   * them exists. Riders spread out when their tanks are created. */
  for (i = 0; i < MAX_STARTS; i++) riders[i] = 0;
  for (i = 0; i < MAX_TANKS; i++) {
    int host = -1;
    int hostRiders = 0;
    int hostTie = 0;
    if (!connected[i] || outStartIdx[i] != MAX_STARTS) continue;
    g = slotGroup[i];
    if (g < 0) continue;
    if (!groups[g].isSolo) {
      for (s = 0; s < numStarts; s++) {
        int d;
        if (!startClaimed[s] || startToPlayer[s] >= MAX_TANKS) continue;
        if (teamNumber[startToPlayer[s]] != teamNumber[i]) continue;
        d = startsMapDistance((*value)->item[s].x, (*value)->item[s].y,
                              groups[g].anchorX, groups[g].anchorY);
        if (host < 0 || riders[s] < hostRiders ||
            (riders[s] == hostRiders && d < hostTie)) {
          host = s;
          hostRiders = riders[s];
          hostTie = d;
        }
      }
      if (host < 0) {
        for (s = 0; s < numStarts; s++) {
          int d;
          if (startsIsValidSquare(sim, (*value)->item[s].x, (*value)->item[s].y) == FALSE) continue;
          if (!startSideEligible(sideMask[s], groupSide[g], groupOtherMask[g])) continue;
          d = startsMapDistance((*value)->item[s].x, (*value)->item[s].y,
                                groups[g].anchorX, groups[g].anchorY);
          if (host < 0 || d < hostTie) {
            host = s;
            hostTie = d;
          }
        }
      }
    }
    if (host < 0) {
      int pass;
      for (pass = 0; pass < 2 && host < 0; pass++) {
        bool onlyAllowed = (pass == 0);
        for (s = 0; s < numStarts; s++) {
          int d;
          if (startsIsValidSquare(sim, (*value)->item[s].x, (*value)->item[s].y) == FALSE) continue;
          if (onlyAllowed && !startSideEligible(sideMask[s], groupSide[g], groupOtherMask[g])) continue;
          d = startsBatchMinDistToClaimed(value, numStarts, startClaimed, (BYTE)s);
          if (host < 0 || riders[s] < hostRiders ||
              (riders[s] == hostRiders && d > hostTie)) {
            host = s;
            hostRiders = riders[s];
            hostTie = d;
          }
        }
      }
    }
    if (host >= 0) {
      riders[host]++;
      outStartIdx[i] = (BYTE)host;
    }
  }
}

/*********************************************************
*NAME:          startsPickIncremental
*AUTHOR:        John Morrison
*CREATION DATE: 24/4/26
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Picks one free start for a single joiner, sharing the
*  distance and validity logic with startsAssignBatch.
*  taken[] is 0-based per start (TRUE = already reserved).
*  teammateStarts0[] lists the 0-based start indices reserved
*  by the joiner's teammates (teammateCount may be 0).
*
*  Only starts the joiner's side rules allow are candidates
*  (see startSideEligible): a team with a side takes what
*  its side accepts, a team with no side stays off every
*  side in closedMask. Candidates are ranked in three tiers
*  — starts on the joiner's side alone, starts on its side
*  that another team's side also covers, then the centre —
*  and the first tier holding a free start wins. With no
*  side every candidate is in the first tier.
*
*  Within a tier: with teammate reservations the side
*  accepts, returns the free valid start with the smallest
*  distance to the nearest of them (cluster); otherwise
*  returns the free valid start maximising the min distance
*  to every taken start (farthest-first). Returns MAX_STARTS
*  when no free valid start the rules allow exists.
*
*ARGUMENTS:
*  sim             - Pointer to the game simulation
*  value           - Pointer to the starts structure
*  taken           - [numStarts] reservation flags, 0-based
*  teammateStarts0 - 0-based teammate reservation indices
*  teammateCount   - Number of entries in teammateStarts0
*  side            - The joiner's team side (START_SIDE_*)
*  closedMask      - Union of the side bits the other teams
*                    chose (START_SIDE_BIT_*)
*********************************************************/
BYTE startsPickIncremental(struct GameSim *sim, starts *value,
                           const bool *taken,
                           const BYTE *teammateStarts0, int teammateCount,
                           BYTE side, BYTE closedMask) {
  BYTE numStarts;
  BYTE sideMask[MAX_STARTS];
  BYTE startTier[MAX_STARTS]; /* 1 own side only, 2 shared with another side, 3 centre; 0 not a candidate */
  BYTE ownBits;
  int usableRefs;
  int tier;
  int j;
  BYTE i;
  int bestStart = -1;

  if (value == NULL || *value == NULL || (*value)->numStarts == 0) {
    return MAX_STARTS;
  }
  numStarts = (*value)->numStarts;
  startsSideMasks(value, sideMask);
  ownBits = startSideBits(side);

  /* Tier every free valid start the side rules allow. */
  for (i = 0; i < numStarts; i++) {
    startTier[i] = 0;
    if (taken[i]) continue;
    if (startsIsValidSquare(sim, (*value)->item[i].x, (*value)->item[i].y) == FALSE) continue;
    if (!startSideEligible(sideMask[i], side, closedMask)) continue;
    if (ownBits == 0) {
      startTier[i] = 1;
    } else if (startSideIsCentre(sideMask[i])) {
      startTier[i] = 3;
    } else if ((sideMask[i] & closedMask & (BYTE)~ownBits) != 0) {
      startTier[i] = 2;
    } else {
      startTier[i] = 1;
    }
  }

  /* Only teammate reservations the side accepts pull the pick toward
   * them; an off-side reservation is ignored as a reference. */
  usableRefs = 0;
  for (j = 0; j < teammateCount; j++) {
    BYTE t0 = teammateStarts0[j];
    if (t0 < numStarts && startSideAccepts(sideMask[t0], side)) usableRefs++;
  }

  for (tier = 1; tier <= 3 && bestStart < 0; tier++) {
    if (usableRefs > 0) {
      /* Cluster: smallest distance to the nearest teammate reservation. */
      int bestDist = INT_MAX;
      for (i = 0; i < numStarts; i++) {
        int minD = INT_MAX;
        if (startTier[i] != tier) continue;
        for (j = 0; j < teammateCount; j++) {
          BYTE t0 = teammateStarts0[j];
          int d;
          if (t0 >= numStarts) continue;
          if (!startSideAccepts(sideMask[t0], side)) continue;
          d = startsMapDistance((*value)->item[i].x, (*value)->item[i].y,
                                (*value)->item[t0].x, (*value)->item[t0].y);
          if (d < minD) minD = d;
        }
        if (bestStart < 0 || minD < bestDist) {
          bestDist = minD;
          bestStart = i;
        }
      }
    } else {
      /* Farthest-first: maximise the min distance to all taken starts. */
      int bestMinDist = -1;
      for (i = 0; i < numStarts; i++) {
        int minD = INT_MAX;
        if (startTier[i] != tier) continue;
        for (j = 0; j < numStarts; j++) {
          int d;
          if (!taken[j]) continue;
          d = startsMapDistance((*value)->item[i].x, (*value)->item[i].y,
                                (*value)->item[j].x, (*value)->item[j].y);
          if (d < minD) minD = d;
        }
        if (minD == INT_MAX) minD = MAP_ARRAY_SIZE; /* nothing taken — any start qualifies */
        if (minD > bestMinDist) {
          bestMinDist = minD;
          bestStart = i;
        }
      }
    }
  }

  if (bestStart < 0) {
    return MAX_STARTS;
  }
  return (BYTE)bestStart;
}

/*********************************************************
*NAME:          startsGetStart
*AUTHOR:        John Morrison
*CREATION DATE: 7/1/99
*LAST MODIFIED: 24/4/26
*PURPOSE:
*  Returns a start position. If a pre-computed batch
*  position has been stashed for this player (lobby->game
*  transition), consume it. Otherwise dispatch to the
*  per-player algorithm based on game type.
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

  /* Tutorial: deterministic start gated on player progress. Unlike
     pendingStartIdx this is not consumed — every tutorial respawn uses it. */
  if (sim->isTutorial) {
    BYTE idx = sim->tutorialStartIdx;
    BYTE rx;
    BYTE ry;
    BYTE bt;
    if (idx >= (*value)->numStarts) idx = 0;   /* clamp to a valid start */
    startsScatterFind(sim, (*value)->item[idx].x, (*value)->item[idx].y, &rx, &ry, playerNum);
    bt = startsConvertDir((*value)->item[idx].dir);
    *x = rx;
    *y = ry;
    *dir = (TURNTYPE)(bt * START_TIMES_16);
    return;
  }

  if (playerNum < MAX_TANKS && sim->pendingStartIdx[playerNum] < (*value)->numStarts) {
    BYTE idx = sim->pendingStartIdx[playerNum];
    BYTE rx;
    BYTE ry;
    BYTE bt;
    sim->pendingStartIdx[playerNum] = MAX_STARTS;
    startsScatterFind(sim, (*value)->item[idx].x, (*value)->item[idx].y, &rx, &ry, playerNum);
    bt = startsConvertDir((*value)->item[idx].dir);
    *x = rx;
    *y = ry;
    *dir = (TURNTYPE)(bt * START_TIMES_16);
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
    rnd = (int)bolo_rand_below((uint32_t)(*value)->numStarts);
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
  /* Clamp the wire-supplied count so a hostile map cannot drive out-of-bounds
   * reads of item[] past MAX_STARTS. */
  if ((*value)->numStarts > MAX_STARTS) {
    (*value)->numStarts = MAX_STARTS;
  }
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
