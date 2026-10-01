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
*Name:          ScreenCalc
*Filename:      screenCalc.h 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 26/3/99
*Purpose:
*  Translates a map square into a tile. 
*  eg road -> T-Junction
*********************************************************/

#ifndef SCREEN_CALC_H
#define SCREEN_CALC_H


/* Includes */
#include "lv_global.h"
#include "lv_tilenum.h"

/* Prototypes */

/*********************************************************
*NAME:          lv_screenCalcRoad
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Returns the road type of a square
*
*ARGUMENTS:
*  aboveLeft  - The square above left 
*  above      - The square above 
*  aboveRight - The square above right
*  left       - The square left
*  right      - The square right
*  belowLeft  - The square below left
*  belowRight - The square below right
*********************************************************/
BYTE lv_screenCalcRoad(BYTE aboveLeft, BYTE above, BYTE aboveRight, BYTE left, BYTE right, BYTE belowLeft, BYTE below, BYTE belowRight);

/*********************************************************
*NAME:          lv_screenCalcBoat
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Returns the boat type of a square
*
*ARGUMENTS:
*  aboveLeft  - The square above left 
*  above      - The square above 
*  aboveRight - The square above right
*  left       - The square left
*  right      - The square right
*  belowLeft  - The square below left
*  belowRight - The square below right
*********************************************************/
BYTE lv_screenCalcBoat(BYTE aboveLeft, BYTE above, BYTE aboveRight, BYTE left, BYTE right, BYTE belowLeft, BYTE below, BYTE belowRight);

/*********************************************************
*NAME:          lv_screenCalcBuilding
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Returns the building type
*
*ARGUMENTS:
*  aboveLeft  - The square above left 
*  above      - The square above 
*  aboveRight - The square above right
*  left       - The square left
*  right      - The square right
*  belowLeft  - The square below left
*  belowRight - The square below right
*********************************************************/
BYTE lv_screenCalcBuilding(BYTE aboveLeft, BYTE above, BYTE aboveRight, BYTE left, BYTE right, BYTE belowLeft, BYTE below, BYTE belowRight);

/*********************************************************
*NAME:          lv_screenCalcRiver
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Returns the River type
*
*ARGUMENTS:
*  aboveLeft  - The square above left 
*  above      - The square above 
*  aboveRight - The square above right
*  left       - The square left
*  right      - The square right
*  belowLeft  - The square below left
*  belowRight - The square below right
*********************************************************/
BYTE lv_screenCalcRiver(BYTE aboveLeft, BYTE above, BYTE aboveRight, BYTE left, BYTE right, BYTE belowLeft, BYTE below, BYTE belowRight);

/*********************************************************
*NAME:          lv_screenCalcDeepSea
*AUTHOR:        John Morrison
*CREATION DATE: 29/10/98
*LAST MODIFIED: 29/10/98
*PURPOSE:
*  Returns the Deep Sea type
*
*ARGUMENTS:
*  aboveLeft  - The square above left 
*  above      - The square above 
*  aboveRight - The square above right
*  left       - The square left
*  right      - The square right
*  belowLeft  - The square below left
*  belowRight - The square below right
*********************************************************/
BYTE lv_screenCalcDeepSea(BYTE aboveLeft, BYTE above, BYTE aboveRight, BYTE left, BYTE right, BYTE belowLeft, BYTE below, BYTE belowRight);

/*********************************************************
*NAME:          lv_screenCalcForest
*AUTHOR:        John Morrison
*CREATION DATE: 26/3/98
*LAST MODIFIED: 26/3/98
*PURPOSE:
*  Returns the forest type of a square
*
*ARGUMENTS:
*  aboveLeft  - The square above left 
*  above      - The square above 
*  aboveRight - The square above right
*  left       - The square left
*  right      - The square right
*  belowLeft  - The square below left
*  belowRight - The square below right
*********************************************************/
BYTE lv_screenCalcForest(BYTE aboveLeft, BYTE above, BYTE aboveRight, BYTE left, BYTE right, BYTE belowLeft, BYTE below, BYTE belowRight);

#endif /* SCREEN_CALC_H */
