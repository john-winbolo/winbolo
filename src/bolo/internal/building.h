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
*Name:          Building
*Filename:      building.h
*Author:        John Morrison
*Creation Date: 30/12/98
*Last Modified: 04/10/03
*Purpose:
*  Responsable for tracking lifetime of Buildings.
*  buildings can be shot 5 times before being destroyed
*********************************************************/

#ifndef BUILDING_H
#define BUILDING_H

#include "global.h"

struct GameSim;

/* Empty / Non Empty / Head / Tail Macros */
#define IsEmpty(list) ((list) ==NULL)
#define NonEmpty(list) (!IsEmpty(list))
#define BuildingHeadX(list) ((list)->x);
#define BuildingHeadY(list) ((list)->y);
#define BuildingHeadLength(list) ((list)->life);
#define BuildingTail(list) ((list)->next);

/* How many shots it takes to destroy a half building. A building takes
   one more: its first shot makes it a half building. */
#define BUILDING_LIFE 4

/* Shells die when there length equals */
#define BUILDING_DEATH 0

/* Type structure */

typedef struct buildingObj *building;
struct buildingObj {
  building next; /* Next item */ 
  BYTE x;      /* X and Y positions */
  BYTE y;
  BYTE life;   /* Life time of the rubble */
};

/* Prototypes */

/*********************************************************
*NAME:          buildingCreate
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED: 30/12/98
*PURPOSE:
*  Sets up the building data structure
*
*ARGUMENTS:
* bld - Pointer to the buildings object
*********************************************************/
void buildingCreate(building *bld);

/*********************************************************
*NAME:          buildingDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED: 30/12/98
*PURPOSE:
*  Destroys and frees memory for the building data structure
*
*ARGUMENTS:
* bld - Pointer to the buildings object
*********************************************************/
void buildingDestroy(building *bld);

/*********************************************************
*NAME:          buildingeAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED: 25/04/01
*PURPOSE:
*  Adds an item to the building data structure.
*  If it already exists returns the terrain type of the
*  item and decrements its lifetime.
*
*  A building's first hit makes it a half building and
*  starts its record with the whole life. A half building
*  with no record was not made by a hit: it was on the map
*  from the start, or a scenario wrote it. It stands where
*  a building stands after its first hit, so its record
*  starts one hit down, and it takes one hit fewer than a
*  building, as in Mac Bolo.
*
*ARGUMENTS:
*  sim     - The game the building belongs to
*  bld     - Pointer to the buildings object
*  x       - X co-ord
*  y       - Y co-ord
*  terrain - The square's terrain as the shell found it:
*            BUILDING or HALFBUILDING
*********************************************************/
BYTE buildingAddItem(struct GameSim *sim, building *bld, BYTE x, BYTE y, BYTE terrain);

/*********************************************************
*NAME:          buildingDeleteItem
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED: 30/12/98
*PURPOSE:
*  Deletes the item for the given number
*
*ARGUMENTS:
*  bld     - Pointer to the buildings object
*  itemNum - The item number to get
*********************************************************/
void buildingDeleteItem(building *bld, int itemNum);

/*********************************************************
*NAME:          buildingRemovePos
*AUTHOR:        John Morrison
*CREATION DATE: 18/1/99
*LAST MODIFIED: 18/1/99
*PURPOSE:
*  Removes an item from the building data structure if it 
*  exists at a specific loaction. Otherwise the function
*  does nothing
*
*ARGUMENTS:
*  bld   - Pointer to the buildings object
*  x     - X co-ord
*  y     - Y co-ord
*********************************************************/
void buildingRemovePos(building *bld, BYTE x, BYTE y);

#endif /* BUILDING_H */

