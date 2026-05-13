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
*Name:          Bases 
*Filename:      bases.h 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 11/11/00
*Purpose:
*  Provides operations on bases
*********************************************************/

#ifndef BASES_H
#define BASES_H


/* Includes */
#include "lv_global.h"
#include "types.h"
/* Defines */
#define MAX_BASES 16


/* Typedefs — bases is defined in bolo/types.h */

/* Determines the base type, good, neutral or evil */

#ifndef _BACKEND_H
typedef enum {
  baseDead,
  baseOwnGood,
  baseAllieGood,
  baseNeutral,
  baseEvil
} baseAlliance;
#endif /* _BACKEND_H */

/* Prototypes */

/*********************************************************
*NAME:         lv_basesCreate 
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
void lv_basesCreate(bases *value);

/*********************************************************
*NAME:          lv_basesDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the bases data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*********************************************************/
void lv_basesDestroy(bases *value);

/*********************************************************
*NAME:          lv_basesSetNumBases
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
void lv_basesSetNumBases(bases *value, BYTE numBases);

/*********************************************************
*NAME:          lv_basesGetNumBases 
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of bases in the structure
*
*ARGUMENTS:
*  value  - Pointer to the bases structure
*********************************************************/
BYTE lv_basesGetNumBases(bases *value);

/*********************************************************
*NAME:          lv_basesSetBase
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific base with its item data
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void lv_basesSetBase(bases *value, base *item, BYTE bsaeNum);

/*********************************************************
*NAME:          lv_basesExistPos
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
bool lv_basesExistPos(bases *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_basesGetBase
*AUTHOR:        John Morrison
*CREATION DATE:   9/2/98
*LAST MODIFIED: 11/11/00
*PURPOSE:
*  Gets a specific base
*
*ARGUMENTS:
*  value   - Pointer to the bases structure
*  item    - Pointer to a base item 
*  baseNum - The base number
*********************************************************/
void lv_basesGetBase(bases *value, base *item, BYTE baseNum);


void lv_basesDeleteBase(bases *value, BYTE x, BYTE y);


/*********************************************************
*NAME:          lv_basesAmOwner
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
bool lv_basesAmOwner(bases *value, BYTE owner, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_basesGetAlliancePos
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
baseAlliance lv_basesGetAlliancePos(bases *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_basesSetOwner
*AUTHOR:        John Morrison
*CREATION DATE: 10/1/99
*LAST MODIFIED: 2/11/99
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
*  baseNum - Base Number
*  owner   - Who owns it
*  migrate - TRUE if it has migrated from an alliance
*********************************************************/
BYTE lv_basesSetOwner(bases *value, BYTE baseNum, BYTE owner, BYTE migrate);

void lv_basesSetStock(bases *value, BYTE baseNum, BYTE s, BYTE m, BYTE a);

bool lv_basesChooseView(bases *value, int x, int y);

/*********************************************************
*NAME:          lv_basesSetBaseNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/2/99
*LAST MODIFIED: 27/2/99
*PURPOSE:
* Sets the base data to buff.
*
*ARGUMENTS:
*  value - Pointer to the bases structure
*  buff  - Buffer of data to set base structure to
*  len   - Length of the data
*********************************************************/
void lv_basesSetBaseNetData(bases *value, BYTE *buff, int len);

// Assumes that the base exists otherwise returns 0
BYTE lv_basesItemNumAt(bases *value, BYTE xValue, BYTE yValue);

#endif /* BASES_H */
