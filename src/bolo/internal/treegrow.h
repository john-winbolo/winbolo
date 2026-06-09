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
*Name:          Tree Growth
*Filename:      treegrow.h
*Author:        John Morrison
*Creation Date: 22/1/99
*Last Modified: 24/12/99
*Purpose:
*  Responsible for storing and calculating tree growth
*********************************************************/

#ifndef TREEGROW_H
#define TREEGROW_H

#include "global.h"
#include "types.h"

struct GameSim;

/* The time a tree exist before growing unless we find a better spot */
#define TREEGROW_TIME 3000 /* 3700 */

/* Initial time delay before we build a tree. This will be overwritten
   before it gets called */
#define TREEGROW_INITIAL_TIME 30000
/* Initial scores to beat */
#define TREEGROW_INITIAL_SCORE -10000


/* Weights of the items */
#define TREE_GROW_FOREST 100 /* 67  100 */
#define TREE_GROW_GRASS 25 /* 10 17  25 */
#define TREE_GROW_RIVER 2 /* 7  10 */
#define TREE_GROW_BOAT 1 /* 3  5 */
#define TREE_GROW_DEEP_SEA  0 /* 1 */
#define TREE_GROW_DEEP_SWAMP 2 /* 1 */
#define TREE_GROW_DEEP_RUBBLE -2 /* -2  -1 */
#define TREE_GROW_BUILDING -20 /* -7  -5 */
#define TREE_GROW_HALF_BUILDING -15 /* -7  -5 */
#define TREE_GROW_CRATER -2 /* -2  -1  */
#define TREE_GROW_ROAD -100 /* -18 * -14  -10 */
#define TREE_GROW_MINE -7 /* -7 -5 */

/* Score must be greater then 0 for it to grow */
#define TREE_NO_GROW 0

/* Prototypes */

/*********************************************************
*NAME:          treeGrowCreate
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
*  Sets up the treeGrow data structure
*
*ARGUMENTS:
*
*********************************************************/
void treeGrowCreate(struct GameSim *sim);

/*********************************************************
*NAME:          treeGrowDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
*  Destroys and frees memory for the treeGrow data 
*  structure
*
*ARGUMENTS:
*
*********************************************************/
void treeGrowDestroy(struct GameSim *sim);

/*********************************************************
*NAME:          treeGrowReset
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
*  Clears the pending grow target for a new round without
*  re-seeding (draws nothing from bolo_rand).
*
*ARGUMENTS:
*  sim - Pointer to the game simulation
*********************************************************/
void treeGrowReset(struct GameSim *sim);

/*********************************************************
*NAME:          treeGrowAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
*  Adds or updates an item in the treeGrow data structure.
*  Replaces the score if the score is greater then the
*  one that is already there and resets the timer to zero
*
*ARGUMENTS:
*  x     - X co-ord
*  y     - Y co-ord
*  score - The score of the item
*********************************************************/
void treeGrowAddItem(struct GameSim *sim, BYTE x, BYTE y, int score);

/*********************************************************
*NAME:          treeGrowUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
*  Game tick has happened. Calculate the growth scores 
*  Build new trees etc.
*
*ARGUMENTS:
*  sim - Pointer to the game simulation
*********************************************************/
void treeGrowUpdate(struct GameSim *sim);

/*********************************************************
*NAME:          treeGrowCalcScore
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
* Calculates the tree growth score for the a specific 
* map location.
*
*ARGUMENTS:
*  sim - Pointer to the game simulation
*  mx - Map X Position
*  my - Map Y Position
*********************************************************/
int treeGrowCalcScore(struct GameSim *sim, BYTE mx, BYTE my);

/*********************************************************
*NAME:          treeGrowCheckGrowTree
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
* Called once every tick to see if a tree should be grown
* and removed from the data structure.
*
*ARGUMENTS:
*  sim - Pointer to the game simulation
*********************************************************/
void treeGrowCheckGrowTree(struct GameSim *sim);

/*********************************************************
*NAME:          treeGrowCalcSquare
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
* Calculates the tree growth score for a specific square
*
*ARGUMENTS:
*  sim - Pointer to the game simulation
*  mx - Map X position
*  my - Map Y position
*********************************************************/
int treeGrowCalcSquare(struct GameSim *sim, BYTE mx, BYTE my);

#endif /* TREEGROW_H */
