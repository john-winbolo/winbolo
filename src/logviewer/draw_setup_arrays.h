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
 *Name:          draw_setup_arrays
 *Filename:      draw_setup_arrays.h
 *Author:        John Morrison
 *Creation Date: 28/5/00
 *Last Modified: 2026
 *Purpose:
 *  Header for drawing position arrays setup
 *********************************************************/

#ifndef DRAW_SETUP_ARRAYS_H
#define DRAW_SETUP_ARRAYS_H

#include "global.h"

/* Drawing position arrays - indexed by tile type */
extern int lv_drawPosX[255];
extern int lv_drawPosY[255];

/*********************************************************
 *NAME:          lv_drawSetupArrays
 *AUTHOR:        John Morrison
 *CREATION DATE: 28/5/00
 *LAST MODIFIED: 28/5/00
 *PURPOSE:
 *  Sets up the drawing arrays to improve efficiency and
 *  remove the giant switch statement every drawing loop
 *
 *ARGUMENTS:
 *  zoomFactor - The scaling factor
 *********************************************************/
void lv_drawSetupArrays(BYTE zoomFactor);

#endif /* DRAW_SETUP_ARRAYS_H */