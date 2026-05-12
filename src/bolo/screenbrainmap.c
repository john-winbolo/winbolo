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
*Name:          Screen Brain Map
*Filename:      screenBrainMap.c
*Author:        John Morrison
*Creation Date: 27/11/99
*Last Modified:  1/12/99
*Purpose:
*  Responsible for storing a copy of the map in form used
*  by Bolo Brains because doing a manual copy is too slow
*  each time a brain request is made
*********************************************************/

#include <memory.h>
#include "global.h"
#include "bolo_map.h"
#include "brain.h"
#include "mines.h"
#include "client_sim.h"
#include "screenbrainmap.h"

void screenBrainMapCreate(ClientSim *cs) {
  memset(clientSimGetBrainMap(cs), TERRAIN_UNKNOWN, (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE));
  clientSimGetGameSim(cs)->brainMap = clientSimGetBrainMap(cs);
}

void screenBrainMapDestroy(ClientSim *cs) {
  clientSimGetGameSim(cs)->brainMap = NULL;
}

BYTE *screenBrainMapGetPointer(ClientSim *cs) {
  return clientSimGetBrainMap(cs);
}

void screenBrainMapFillFromMap(ClientSim *cs, map *mp, mines *mn) {
  BYTE x, y;
  BYTE (*brainMap)[MAP_ARRAY_SIZE] = (BYTE (*)[MAP_ARRAY_SIZE])clientSimGetBrainMap(cs);
  for (y = 0; ; y++) {
    for (x = 0; ; x++) {
      screenBrainMapSetPos(brainMap, x, y, mapGetPos(mp, x, y), minesExistPos(mn, mp, x, y));
      if (x == 255) break;
    }
    if (y == 255) break;
  }
}

void screenBrainMapSetPos(BYTE brainMap[][MAP_ARRAY_SIZE], BYTE xValue, BYTE yValue, BYTE terrain, bool isMine) {
  if (brainMap == NULL) {
    return;
  }
  brainMap[yValue][xValue] = terrain;
  if (terrain >= MINE_START && terrain <= MINE_END) {
    brainMap[yValue][xValue] -= MINE_SUBTRACT;
  } else if (terrain == DEEP_SEA) {
    brainMap[yValue][xValue] = BDEEPSEA;
  }

  if (isMine == TRUE) {
    brainMap[yValue][xValue] |= TERRAIN_MINE;
  }
}
