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
*Filename:      starts.h 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 11/11/00
*Purpose:
*  Provides operations on player starts 
*********************************************************/

#ifndef STARTS_H
#define STARTS_H


/* Includes */
#include "global.h"
#include "bolo/types.h"

/* Defines */
#define MAX_STARTS 16


/* Typedefs */

typedef struct startsObj *starts;

/* Prototypes */

/*********************************************************
*NAME:          lv_startsCreate
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
void lv_startsCreate(starts *value);

/*********************************************************
*NAME:          lv_startsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the starts data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void lv_startsDestroy(starts *value);

/*********************************************************
*NAME:          lv_startsSetNumStarts
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
void lv_startsSetNumStarts(starts *value, BYTE numStarts);

/*********************************************************
*NAME:          startsGetNumPills
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of starts in the structure
*
*ARGUMENTS:
*  value  - Pointer to the starts structure
*********************************************************/
BYTE lv_startsGetNumStarts(starts *value);

/*********************************************************
*NAME:          lv_startsSetStart
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
void lv_startsSetStart(starts *value, start *item, BYTE startNum);

/*********************************************************
*NAME:          lv_startsGetStartStruct
*AUTHOR:        John Morrison
*CREATION DATE:   9/2/98
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Gets a specific start.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - Pointer to a player start 
*  startNum - The start number
*********************************************************/
void lv_startsGetStartStruct(starts *value, start *item, BYTE startNum);

void lv_startsDeleteStart(starts *value, BYTE x, BYTE y);


bool lv_startsExistPos(starts *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_startsSetStartNetData
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
void lv_startsSetStartNetData(starts *value, BYTE *buff, BYTE dataLen);

#endif /* STARTS_H */
