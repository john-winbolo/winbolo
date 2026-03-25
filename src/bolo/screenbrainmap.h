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
*Filename:      screenBrainMap.h
*Author:        John Morrison
*Creation Date: 27/11/99
*Last Modified: 27/11/99
*Purpose:
*  Responsible for storing a copy of the map in form used
*  by Bolo Brains because doing a manual copy is too slow
*  each time a brain request is made
*********************************************************/

#ifndef _SCREEN_BRAIN_MAP_H
#define _SCREEN_BRAIN_MAP_H

#include "global.h"
#include "bolo_map.h"
#include "mines.h"

/* Forward declaration — full definition in client_sim.h */
typedef struct ClientSim ClientSim;

void screenBrainMapCreate(ClientSim *cs);
void screenBrainMapDestroy(ClientSim *cs);
BYTE *screenBrainMapGetPointer(ClientSim *cs);

/*********************************************************
*NAME:          screenBrainMapSetPos
*PURPOSE:
*  Sets a position in the brain map. brainMap may be NULL
*  (e.g. on the server side), in which case this is a no-op.
*
*ARGUMENTS:
*  brainMap - The 256x256 brain map array (or NULL)
*  xValue   - The X Value of the position to set
*  yValue   - The Y Value of the position to set
*  terrain  - Terrain to set it to
*  isMine   - Is this square mined
*********************************************************/
void screenBrainMapSetPos(BYTE brainMap[][MAP_ARRAY_SIZE], BYTE xValue, BYTE yValue, BYTE terrain, bool isMine);

/*********************************************************
*NAME:          screenBrainMapFillFromMap
*PURPOSE:
*  Fills the entire 256x256 brain map with actual terrain.
*  Used for aiFull mode.
*
*ARGUMENTS:
*  cs - Pointer to the ClientSim owning the brain map
*  mp - Pointer to the game map
*  mn - Pointer to the client mines structure
*********************************************************/
void screenBrainMapFillFromMap(ClientSim *cs, map *mp, mines *mn);

#endif /* _SCREEN_BRAIN_MAP_H */
