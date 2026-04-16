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
 *Filename:      draw_setup_arrays.c
 *Author:        John Morrison
 *Creation Date: 28/5/00
 *Last Modified: 2026
 *Purpose:
 *  Sets up the drawing arrays to improve efficiency and
 *  remove the giant switch statement every drawing loop
 *********************************************************/

#include "tiles.h"
#include "tilenum.h"
#include "draw_setup_arrays.h"

/* Drawing position arrays - indexed by tile type */
int lv_drawPosX[255];
int lv_drawPosY[255];

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
void lv_drawSetupArrays(BYTE zoomFactor) {
  lv_drawPosX[DEEP_SEA_SOLID] = zoomFactor * DEEP_SEA_SOLID_X;
  lv_drawPosY[DEEP_SEA_SOLID] = zoomFactor * DEEP_SEA_SOLID_Y;
  lv_drawPosX[DEEP_SEA_CORN1] = zoomFactor * DEEP_SEA_CORN1_X;
  lv_drawPosY[DEEP_SEA_CORN1] = zoomFactor * DEEP_SEA_CORN1_Y;
  lv_drawPosX[DEEP_SEA_CORN2] = zoomFactor * DEEP_SEA_CORN2_X;
  lv_drawPosY[DEEP_SEA_CORN2] = zoomFactor * DEEP_SEA_CORN2_Y;
  lv_drawPosX[DEEP_SEA_CORN3] = zoomFactor * DEEP_SEA_CORN3_X;
  lv_drawPosY[DEEP_SEA_CORN3] = zoomFactor * DEEP_SEA_CORN3_Y;
  lv_drawPosX[DEEP_SEA_CORN4] = zoomFactor * DEEP_SEA_CORN4_X;
  lv_drawPosY[DEEP_SEA_CORN4] = zoomFactor * DEEP_SEA_CORN4_Y;
  lv_drawPosX[DEEP_SEA_SIDE1] = zoomFactor * DEEP_SEA_SIDE1_X;
  lv_drawPosY[DEEP_SEA_SIDE1] = zoomFactor * DEEP_SEA_SIDE1_Y;
  lv_drawPosX[DEEP_SEA_SIDE2] = zoomFactor * DEEP_SEA_SIDE2_X;
  lv_drawPosY[DEEP_SEA_SIDE2] = zoomFactor * DEEP_SEA_SIDE2_Y;
  lv_drawPosX[DEEP_SEA_SIDE3] = zoomFactor * DEEP_SEA_SIDE3_X;
  lv_drawPosY[DEEP_SEA_SIDE3] = zoomFactor * DEEP_SEA_SIDE3_Y;
  lv_drawPosX[DEEP_SEA_SIDE4] = zoomFactor * DEEP_SEA_SIDE4_X;
  lv_drawPosY[DEEP_SEA_SIDE4] = zoomFactor * DEEP_SEA_SIDE4_Y;

  lv_drawPosX[BUILD_SINGLE] = zoomFactor * BUILD_SINGLE_X;
  lv_drawPosY[BUILD_SINGLE] = zoomFactor * BUILD_SINGLE_Y;
  lv_drawPosX[BUILD_SOLID] = zoomFactor * BUILD_SOLID_X;
  lv_drawPosY[BUILD_SOLID] = zoomFactor * BUILD_SOLID_Y;
  lv_drawPosX[BUILD_CORNER1] = zoomFactor * BUILD_CORNER1_X;
  lv_drawPosY[BUILD_CORNER1] = zoomFactor * BUILD_CORNER1_Y;
  lv_drawPosX[BUILD_CORNER2] = zoomFactor * BUILD_CORNER2_X;
  lv_drawPosY[BUILD_CORNER2] = zoomFactor * BUILD_CORNER2_Y;
  lv_drawPosX[BUILD_CORNER3] = zoomFactor * BUILD_CORNER3_X;
  lv_drawPosY[BUILD_CORNER3] = zoomFactor * BUILD_CORNER3_Y;
  lv_drawPosX[BUILD_CORNER4] = zoomFactor * BUILD_CORNER4_X;
  lv_drawPosY[BUILD_CORNER4] = zoomFactor * BUILD_CORNER4_Y;
  lv_drawPosX[BUILD_L1] = zoomFactor * BUILD_L1_X;
  lv_drawPosY[BUILD_L1] = zoomFactor * BUILD_L1_Y;
  lv_drawPosX[BUILD_L2] = zoomFactor * BUILD_L2_X;
  lv_drawPosY[BUILD_L2] = zoomFactor * BUILD_L2_Y;
  lv_drawPosX[BUILD_L3] = zoomFactor * BUILD_L3_X;
  lv_drawPosY[BUILD_L3] = zoomFactor * BUILD_L3_Y;
  lv_drawPosX[BUILD_L4] = zoomFactor * BUILD_L4_X;
  lv_drawPosY[BUILD_L4] = zoomFactor * BUILD_L4_Y;
  lv_drawPosX[BUILD_T1] = zoomFactor * BUILD_T1_X;
  lv_drawPosY[BUILD_T1] = zoomFactor * BUILD_T1_Y;
  lv_drawPosX[BUILD_T2] = zoomFactor * BUILD_T2_X;
  lv_drawPosY[BUILD_T2] = zoomFactor * BUILD_T2_Y;
  lv_drawPosX[BUILD_T3] = zoomFactor * BUILD_T3_X;
  lv_drawPosY[BUILD_T3] = zoomFactor * BUILD_T3_Y;
  lv_drawPosX[BUILD_T4] = zoomFactor * BUILD_T4_X;
  lv_drawPosY[BUILD_T4] = zoomFactor * BUILD_T4_Y;
  lv_drawPosX[BUILD_HORZ] = zoomFactor * BUILD_HORZ_X;
  lv_drawPosY[BUILD_HORZ] = zoomFactor * BUILD_HORZ_Y;
  lv_drawPosX[BUILD_VERT] = zoomFactor * BUILD_VERT_X;
  lv_drawPosY[BUILD_VERT] = zoomFactor * BUILD_VERT_Y;
  lv_drawPosX[BUILD_VERTEND1] = zoomFactor * BUILD_VERTEND1_X;
  lv_drawPosY[BUILD_VERTEND1] = zoomFactor * BUILD_VERTEND1_Y;
  lv_drawPosX[BUILD_VERTEND2] = zoomFactor * BUILD_VERTEND2_X;
  lv_drawPosY[BUILD_VERTEND2] = zoomFactor * BUILD_VERTEND2_Y;
  lv_drawPosX[BUILD_HORZEND1] = zoomFactor * BUILD_HORZEND1_X;
  lv_drawPosY[BUILD_HORZEND1] = zoomFactor * BUILD_HORZEND1_Y;
  lv_drawPosX[BUILD_HORZEND2] = zoomFactor * BUILD_HORZEND2_X;
  lv_drawPosY[BUILD_HORZEND2] = zoomFactor * BUILD_HORZEND2_Y;
  lv_drawPosX[BUILD_CROSS] = zoomFactor * BUILD_CROSS_X;
  lv_drawPosY[BUILD_CROSS] = zoomFactor * BUILD_CROSS_Y;
  lv_drawPosX[BUILD_SIDE1] = zoomFactor * BUILD_SIDE1_X;
  lv_drawPosY[BUILD_SIDE1] = zoomFactor * BUILD_SIDE1_Y;
  lv_drawPosX[BUILD_SIDE2] = zoomFactor * BUILD_SIDE2_X;
  lv_drawPosY[BUILD_SIDE2] = zoomFactor * BUILD_SIDE2_Y;
  lv_drawPosX[BUILD_SIDE3] = zoomFactor * BUILD_SIDE3_X;
  lv_drawPosY[BUILD_SIDE3] = zoomFactor * BUILD_SIDE3_Y;
  lv_drawPosX[BUILD_SIDE4] = zoomFactor * BUILD_SIDE4_X;
  lv_drawPosY[BUILD_SIDE4] = zoomFactor * BUILD_SIDE4_Y;
  lv_drawPosX[BUILD_SIDECORN1] = zoomFactor * BUILD_SIDECORN1_X;
  lv_drawPosY[BUILD_SIDECORN1] = zoomFactor * BUILD_SIDECORN1_Y;
  lv_drawPosX[BUILD_SIDECORN2] = zoomFactor * BUILD_SIDECORN2_X;
  lv_drawPosY[BUILD_SIDECORN2] = zoomFactor * BUILD_SIDECORN2_Y;
  lv_drawPosX[BUILD_SIDECORN3] = zoomFactor * BUILD_SIDECORN3_X;
  lv_drawPosY[BUILD_SIDECORN3] = zoomFactor * BUILD_SIDECORN3_Y;
  lv_drawPosX[BUILD_SIDECORN4] = zoomFactor * BUILD_SIDECORN4_X;
  lv_drawPosY[BUILD_SIDECORN4] = zoomFactor * BUILD_SIDECORN4_Y;
  lv_drawPosX[BUILD_SIDECORN5] = zoomFactor * BUILD_SIDECORN5_X;
  lv_drawPosY[BUILD_SIDECORN5] = zoomFactor * BUILD_SIDECORN5_Y;
  lv_drawPosX[BUILD_SIDECORN6] = zoomFactor * BUILD_SIDECORN6_X;
  lv_drawPosY[BUILD_SIDECORN6] = zoomFactor * BUILD_SIDECORN6_Y;
  lv_drawPosX[BUILD_SIDECORN7] = zoomFactor * BUILD_SIDECORN7_X;
  lv_drawPosY[BUILD_SIDECORN7] = zoomFactor * BUILD_SIDECORN7_Y;
  lv_drawPosX[BUILD_SIDECORN8] = zoomFactor * BUILD_SIDECORN8_X;
  lv_drawPosY[BUILD_SIDECORN8] = zoomFactor * BUILD_SIDECORN8_Y;
  lv_drawPosX[BUILD_SIDECORN9] = zoomFactor * BUILD_SIDECORN9_X;
  lv_drawPosY[BUILD_SIDECORN9] = zoomFactor * BUILD_SIDECORN9_Y;
  lv_drawPosX[BUILD_SIDECORN10] = zoomFactor * BUILD_SIDECORN10_X;
  lv_drawPosY[BUILD_SIDECORN10] = zoomFactor * BUILD_SIDECORN10_Y;
  lv_drawPosX[BUILD_SIDECORN11] = zoomFactor * BUILD_SIDECORN11_X;
  lv_drawPosY[BUILD_SIDECORN11] = zoomFactor * BUILD_SIDECORN11_Y;
  lv_drawPosX[BUILD_SIDECORN12] = zoomFactor * BUILD_SIDECORN12_X;
  lv_drawPosY[BUILD_SIDECORN12] = zoomFactor * BUILD_SIDECORN12_Y;
  lv_drawPosX[BUILD_SIDECORN13] = zoomFactor * BUILD_SIDECORN13_X;
  lv_drawPosY[BUILD_SIDECORN13] = zoomFactor * BUILD_SIDECORN13_Y;
  lv_drawPosX[BUILD_SIDECORN14] = zoomFactor * BUILD_SIDECORN14_X;
  lv_drawPosY[BUILD_SIDECORN14] = zoomFactor * BUILD_SIDECORN14_Y;
  lv_drawPosX[BUILD_SIDECORN15] = zoomFactor * BUILD_SIDECORN15_X;
  lv_drawPosY[BUILD_SIDECORN15] = zoomFactor * BUILD_SIDECORN15_Y;
  lv_drawPosX[BUILD_SIDECORN16] = zoomFactor * BUILD_SIDECORN16_X;
  lv_drawPosY[BUILD_SIDECORN16] = zoomFactor * BUILD_SIDECORN16_Y;
  lv_drawPosX[BUILD_TWIST1] = zoomFactor * BUILD_TWIST1_X;
  lv_drawPosY[BUILD_TWIST1] = zoomFactor * BUILD_TWIST1_Y;
  lv_drawPosX[BUILD_TWIST2] = zoomFactor * BUILD_TWIST2_X;
  lv_drawPosY[BUILD_TWIST2] = zoomFactor * BUILD_TWIST2_Y;
  lv_drawPosX[BUILD_MOST1] = zoomFactor * BUILD_MOST1_X;
  lv_drawPosY[BUILD_MOST1] = zoomFactor * BUILD_MOST1_Y;
  lv_drawPosX[BUILD_MOST2] = zoomFactor * BUILD_MOST2_X;
  lv_drawPosY[BUILD_MOST2] = zoomFactor * BUILD_MOST2_Y;
  lv_drawPosX[BUILD_MOST3] = zoomFactor * BUILD_MOST3_X;
  lv_drawPosY[BUILD_MOST3] = zoomFactor * BUILD_MOST3_Y;
  lv_drawPosX[BUILD_MOST4] = zoomFactor * BUILD_MOST4_X;
  lv_drawPosY[BUILD_MOST4] = zoomFactor * BUILD_MOST4_Y;



  lv_drawPosX[RIVER_END1] = zoomFactor * RIVER_END1_X;
  lv_drawPosY[RIVER_END1] = zoomFactor * RIVER_END1_Y;
  lv_drawPosX[RIVER_END2] = zoomFactor * RIVER_END2_X;
  lv_drawPosY[RIVER_END2] = zoomFactor * RIVER_END2_Y;
  lv_drawPosX[RIVER_END3] = zoomFactor * RIVER_END3_X;
  lv_drawPosY[RIVER_END3] = zoomFactor * RIVER_END3_Y;
  lv_drawPosX[RIVER_END4] = zoomFactor * RIVER_END4_X;
  lv_drawPosY[RIVER_END4] = zoomFactor * RIVER_END4_Y;
  lv_drawPosX[RIVER_SOLID] = zoomFactor * RIVER_SOLID_X;
  lv_drawPosY[RIVER_SOLID] = zoomFactor * RIVER_SOLID_Y;
  lv_drawPosX[RIVER_SURROUND] = zoomFactor * RIVER_SURROUND_X;
  lv_drawPosY[RIVER_SURROUND] = zoomFactor * RIVER_SURROUND_Y;
  lv_drawPosX[RIVER_SIDE1] = zoomFactor * RIVER_SIDE1_X;
  lv_drawPosY[RIVER_SIDE1] = zoomFactor * RIVER_SIDE1_Y;
  lv_drawPosX[RIVER_SIDE2] = zoomFactor * RIVER_SIDE2_X;
  lv_drawPosY[RIVER_SIDE2] = zoomFactor * RIVER_SIDE2_Y;
  lv_drawPosX[RIVER_ONESIDE1] = zoomFactor * RIVER_ONESIDE1_X;
  lv_drawPosY[RIVER_ONESIDE1] = zoomFactor * RIVER_ONESIDE1_Y;
  lv_drawPosX[RIVER_ONESIDE2] = zoomFactor * RIVER_ONESIDE2_X;
  lv_drawPosY[RIVER_ONESIDE2] = zoomFactor * RIVER_ONESIDE2_Y;
  lv_drawPosX[RIVER_ONESIDE3] = zoomFactor * RIVER_ONESIDE3_X;
  lv_drawPosY[RIVER_ONESIDE3] = zoomFactor * RIVER_ONESIDE3_Y;
  lv_drawPosX[RIVER_ONESIDE4] = zoomFactor * RIVER_ONESIDE4_X;
  lv_drawPosY[RIVER_ONESIDE4] = zoomFactor * RIVER_ONESIDE4_Y;
  lv_drawPosX[RIVER_CORN1] = zoomFactor * RIVER_CORN1_X;
  lv_drawPosY[RIVER_CORN1] = zoomFactor * RIVER_CORN1_Y;
  lv_drawPosX[RIVER_CORN2] = zoomFactor * RIVER_CORN2_X;
  lv_drawPosY[RIVER_CORN2] = zoomFactor * RIVER_CORN2_Y;
  lv_drawPosX[RIVER_CORN3] = zoomFactor * RIVER_CORN3_X;
  lv_drawPosY[RIVER_CORN3] = zoomFactor * RIVER_CORN3_Y;
  lv_drawPosX[RIVER_CORN4] = zoomFactor * RIVER_CORN4_X;
  lv_drawPosY[RIVER_CORN4] = zoomFactor * RIVER_CORN4_Y;

  lv_drawPosX[SWAMP] = zoomFactor * SWAMP_X;
  lv_drawPosY[SWAMP] = zoomFactor * SWAMP_Y;
  lv_drawPosX[CRATER] = zoomFactor * CRATER_X;
  lv_drawPosY[CRATER] = zoomFactor * CRATER_Y;


  lv_drawPosX[ROAD_CORNER1] = zoomFactor * ROAD_CORNER1_X;
  lv_drawPosY[ROAD_CORNER1] = zoomFactor * ROAD_CORNER1_Y;
  lv_drawPosX[ROAD_CORNER2] = zoomFactor * ROAD_CORNER2_X;
  lv_drawPosY[ROAD_CORNER2] = zoomFactor * ROAD_CORNER2_Y;
  lv_drawPosX[ROAD_CORNER3] = zoomFactor * ROAD_CORNER3_X;
  lv_drawPosY[ROAD_CORNER3] = zoomFactor * ROAD_CORNER3_Y;
  lv_drawPosX[ROAD_CORNER4] = zoomFactor * ROAD_CORNER4_X;
  lv_drawPosY[ROAD_CORNER4] = zoomFactor * ROAD_CORNER4_Y;
  lv_drawPosX[ROAD_CORNER5] = zoomFactor * ROAD_CORNER5_X;
  lv_drawPosY[ROAD_CORNER5] = zoomFactor * ROAD_CORNER5_Y;
  lv_drawPosX[ROAD_CORNER6] = zoomFactor * ROAD_CORNER6_X;
  lv_drawPosY[ROAD_CORNER6] = zoomFactor * ROAD_CORNER6_Y;
  lv_drawPosX[ROAD_CORNER7] = zoomFactor * ROAD_CORNER7_X;
  lv_drawPosY[ROAD_CORNER7] = zoomFactor * ROAD_CORNER7_Y;
  lv_drawPosX[ROAD_CORNER8] = zoomFactor * ROAD_CORNER8_X;
  lv_drawPosY[ROAD_CORNER8] = zoomFactor * ROAD_CORNER8_Y;

  lv_drawPosX[ROAD_SIDE1] = zoomFactor * ROAD_SIDE1_X;
  lv_drawPosY[ROAD_SIDE1] = zoomFactor * ROAD_SIDE1_Y;
  lv_drawPosX[ROAD_SIDE2] = zoomFactor * ROAD_SIDE2_X;
  lv_drawPosY[ROAD_SIDE2] = zoomFactor * ROAD_SIDE2_Y;
  lv_drawPosX[ROAD_SIDE3] = zoomFactor * ROAD_SIDE3_X;
  lv_drawPosY[ROAD_SIDE3] = zoomFactor * ROAD_SIDE3_Y;
  lv_drawPosX[ROAD_SIDE4] = zoomFactor * ROAD_SIDE4_X;
  lv_drawPosY[ROAD_SIDE4] = zoomFactor * ROAD_SIDE4_Y;
  lv_drawPosX[ROAD_SOLID] = zoomFactor * ROAD_SOLID_X;
  lv_drawPosY[ROAD_SOLID] = zoomFactor * ROAD_SOLID_Y;
  lv_drawPosX[ROAD_CROSS] = zoomFactor * ROAD_CROSS_X;
  lv_drawPosY[ROAD_CROSS] = zoomFactor * ROAD_CROSS_Y;
  lv_drawPosX[ROAD_T1] = zoomFactor * ROAD_T1_X;
  lv_drawPosY[ROAD_T1] = zoomFactor * ROAD_T1_Y;
  lv_drawPosX[ROAD_T2] = zoomFactor * ROAD_T2_X;
  lv_drawPosY[ROAD_T2] = zoomFactor * ROAD_T2_Y;
  lv_drawPosX[ROAD_T3] = zoomFactor * ROAD_T3_X;
  lv_drawPosY[ROAD_T3] = zoomFactor * ROAD_T3_Y;
  lv_drawPosX[ROAD_T4] = zoomFactor * ROAD_T4_X;
  lv_drawPosY[ROAD_T4] = zoomFactor * ROAD_T4_Y;
  lv_drawPosX[ROAD_HORZ] = zoomFactor * ROAD_HORZ_X;
  lv_drawPosY[ROAD_HORZ] = zoomFactor * ROAD_HORZ_Y;
  lv_drawPosX[ROAD_VERT] = zoomFactor * ROAD_VERT_X;
  lv_drawPosY[ROAD_VERT] = zoomFactor * ROAD_VERT_Y;
  lv_drawPosX[ROAD_WATER1] = zoomFactor * ROAD_WATER1_X;
  lv_drawPosY[ROAD_WATER1] = zoomFactor * ROAD_WATER1_Y;
  lv_drawPosX[ROAD_WATER2] = zoomFactor * ROAD_WATER2_X;
  lv_drawPosY[ROAD_WATER2] = zoomFactor * ROAD_WATER2_Y;
  lv_drawPosX[ROAD_WATER3] = zoomFactor * ROAD_WATER3_X;
  lv_drawPosY[ROAD_WATER3] = zoomFactor * ROAD_WATER3_Y;
  lv_drawPosX[ROAD_WATER4] = zoomFactor * ROAD_WATER4_X;
  lv_drawPosY[ROAD_WATER4] = zoomFactor * ROAD_WATER4_Y;
  lv_drawPosX[ROAD_WATER5] = zoomFactor * ROAD_WATER5_X;
  lv_drawPosY[ROAD_WATER5] = zoomFactor * ROAD_WATER5_Y;
  lv_drawPosX[ROAD_WATER6] = zoomFactor * ROAD_WATER6_X;
  lv_drawPosY[ROAD_WATER6] = zoomFactor * ROAD_WATER6_Y;
  lv_drawPosX[ROAD_WATER7] = zoomFactor * ROAD_WATER7_X;
  lv_drawPosY[ROAD_WATER7] = zoomFactor * ROAD_WATER7_Y;
  lv_drawPosX[ROAD_WATER8] = zoomFactor * ROAD_WATER8_X;
  lv_drawPosY[ROAD_WATER8] = zoomFactor * ROAD_WATER8_Y;
  lv_drawPosX[ROAD_WATER9] = zoomFactor * ROAD_WATER9_X;
  lv_drawPosY[ROAD_WATER9] = zoomFactor * ROAD_WATER9_Y;
  lv_drawPosX[ROAD_WATER10] = zoomFactor * ROAD_WATER10_X;
  lv_drawPosY[ROAD_WATER10] = zoomFactor * ROAD_WATER10_Y;
  lv_drawPosX[ROAD_WATER11] = zoomFactor * ROAD_WATER11_X;
  lv_drawPosY[ROAD_WATER11] = zoomFactor * ROAD_WATER11_Y;

  lv_drawPosX[PILL_EVIL_15] = zoomFactor * PILL_EVIL15_X;
  lv_drawPosY[PILL_EVIL_15] = zoomFactor * PILL_EVIL15_Y;
  lv_drawPosX[PILL_EVIL_14] = zoomFactor * PILL_EVIL14_X;
  lv_drawPosY[PILL_EVIL_14] = zoomFactor * PILL_EVIL14_Y;
  lv_drawPosX[PILL_EVIL_13] = zoomFactor * PILL_EVIL13_X;
  lv_drawPosY[PILL_EVIL_13] = zoomFactor * PILL_EVIL13_Y;
  lv_drawPosX[PILL_EVIL_12] = zoomFactor * PILL_EVIL12_X;
  lv_drawPosY[PILL_EVIL_12] = zoomFactor * PILL_EVIL12_Y;
  lv_drawPosX[PILL_EVIL_11] = zoomFactor * PILL_EVIL11_X;
  lv_drawPosY[PILL_EVIL_11] = zoomFactor * PILL_EVIL11_Y;
  lv_drawPosX[PILL_EVIL_10] = zoomFactor * PILL_EVIL10_X;
  lv_drawPosY[PILL_EVIL_10] = zoomFactor * PILL_EVIL10_Y;
  lv_drawPosX[PILL_EVIL_9] = zoomFactor * PILL_EVIL9_X;
  lv_drawPosY[PILL_EVIL_9] = zoomFactor * PILL_EVIL9_Y;
  lv_drawPosX[PILL_EVIL_8] = zoomFactor * PILL_EVIL8_X;
  lv_drawPosY[PILL_EVIL_8] = zoomFactor * PILL_EVIL8_Y;
  lv_drawPosX[PILL_EVIL_7] = zoomFactor * PILL_EVIL7_X;
  lv_drawPosY[PILL_EVIL_7] = zoomFactor * PILL_EVIL7_Y;
  lv_drawPosX[PILL_EVIL_6] = zoomFactor * PILL_EVIL6_X;
  lv_drawPosY[PILL_EVIL_6] = zoomFactor * PILL_EVIL6_Y;
  lv_drawPosX[PILL_EVIL_5] = zoomFactor * PILL_EVIL5_X;
  lv_drawPosY[PILL_EVIL_5] = zoomFactor * PILL_EVIL5_Y;
  lv_drawPosX[PILL_EVIL_4] = zoomFactor * PILL_EVIL4_X;
  lv_drawPosY[PILL_EVIL_4] = zoomFactor * PILL_EVIL4_Y;
  lv_drawPosX[PILL_EVIL_3] = zoomFactor * PILL_EVIL3_X;
  lv_drawPosY[PILL_EVIL_3] = zoomFactor * PILL_EVIL3_Y;
  lv_drawPosX[PILL_EVIL_2] = zoomFactor * PILL_EVIL2_X;
  lv_drawPosY[PILL_EVIL_2] = zoomFactor * PILL_EVIL2_Y;
  lv_drawPosX[PILL_EVIL_1] = zoomFactor * PILL_EVIL1_X;
  lv_drawPosY[PILL_EVIL_1] = zoomFactor * PILL_EVIL1_Y;
  lv_drawPosX[PILL_EVIL_0] = zoomFactor * PILL_EVIL0_X;
  lv_drawPosY[PILL_EVIL_0] = zoomFactor * PILL_EVIL0_Y;

  lv_drawPosX[PILL_GOOD_15] = zoomFactor * PILL_GOOD15_X;
  lv_drawPosY[PILL_GOOD_15] = zoomFactor * PILL_GOOD15_Y;
  lv_drawPosX[PILL_GOOD_14] = zoomFactor * PILL_GOOD14_X;
  lv_drawPosY[PILL_GOOD_14] = zoomFactor * PILL_GOOD14_Y;
  lv_drawPosX[PILL_GOOD_13] = zoomFactor * PILL_GOOD13_X;
  lv_drawPosY[PILL_GOOD_13] = zoomFactor * PILL_GOOD13_Y;
  lv_drawPosX[PILL_GOOD_12] = zoomFactor * PILL_GOOD12_X;
  lv_drawPosY[PILL_GOOD_12] = zoomFactor * PILL_GOOD12_Y;
  lv_drawPosX[PILL_GOOD_11] = zoomFactor * PILL_GOOD11_X;
  lv_drawPosY[PILL_GOOD_11] = zoomFactor * PILL_GOOD11_Y;
  lv_drawPosX[PILL_GOOD_10] = zoomFactor * PILL_GOOD10_X;
  lv_drawPosY[PILL_GOOD_10] = zoomFactor * PILL_GOOD10_Y;
  lv_drawPosX[PILL_GOOD_9] = zoomFactor * PILL_GOOD9_X;
  lv_drawPosY[PILL_GOOD_9] = zoomFactor * PILL_GOOD9_Y;
  lv_drawPosX[PILL_GOOD_8] = zoomFactor * PILL_GOOD8_X;
  lv_drawPosY[PILL_GOOD_8] = zoomFactor * PILL_GOOD8_Y;
  lv_drawPosX[PILL_GOOD_7] = zoomFactor * PILL_GOOD7_X;
  lv_drawPosY[PILL_GOOD_7] = zoomFactor * PILL_GOOD7_Y;
  lv_drawPosX[PILL_GOOD_6] = zoomFactor * PILL_GOOD6_X;
  lv_drawPosY[PILL_GOOD_6] = zoomFactor * PILL_GOOD6_Y;
  lv_drawPosX[PILL_GOOD_5] = zoomFactor * PILL_GOOD5_X;
  lv_drawPosY[PILL_GOOD_5] = zoomFactor * PILL_GOOD5_Y;
  lv_drawPosX[PILL_GOOD_4] = zoomFactor * PILL_GOOD4_X;
  lv_drawPosY[PILL_GOOD_4] = zoomFactor * PILL_GOOD4_Y;
  lv_drawPosX[PILL_GOOD_3] = zoomFactor * PILL_GOOD3_X;
  lv_drawPosY[PILL_GOOD_3] = zoomFactor * PILL_GOOD3_Y;
  lv_drawPosX[PILL_GOOD_2] = zoomFactor * PILL_GOOD2_X;
  lv_drawPosY[PILL_GOOD_2] = zoomFactor * PILL_GOOD2_Y;
  lv_drawPosX[PILL_GOOD_1] = zoomFactor * PILL_GOOD1_X;
  lv_drawPosY[PILL_GOOD_1] = zoomFactor * PILL_GOOD1_Y;
  lv_drawPosX[PILL_GOOD_0] = zoomFactor * PILL_GOOD0_X;
  lv_drawPosY[PILL_GOOD_0] = zoomFactor * PILL_GOOD0_Y;

  lv_drawPosX[BASE_GOOD] = zoomFactor * BASE_GOOD_X;
  lv_drawPosY[BASE_GOOD] = zoomFactor * BASE_GOOD_Y;
  lv_drawPosX[BASE_NEUTRAL] = zoomFactor * BASE_NEUTRAL_X;
  lv_drawPosY[BASE_NEUTRAL] = zoomFactor * BASE_NEUTRAL_Y;
  lv_drawPosX[BASE_EVIL] = zoomFactor * BASE_EVIL_X;
  lv_drawPosY[BASE_EVIL] = zoomFactor * BASE_EVIL_Y;


  lv_drawPosX[FOREST] = zoomFactor * FOREST_X;
  lv_drawPosY[FOREST] = zoomFactor * FOREST_Y;
  lv_drawPosX[FOREST_SINGLE] = zoomFactor * FOREST_SINGLE_X;
  lv_drawPosY[FOREST_SINGLE] = zoomFactor * FOREST_SINGLE_Y;
  lv_drawPosX[FOREST_BR] = zoomFactor * FOREST_BR_X;
  lv_drawPosY[FOREST_BR] = zoomFactor * FOREST_BR_Y;
  lv_drawPosX[FOREST_BL] = zoomFactor * FOREST_BL_X;
  lv_drawPosY[FOREST_BL] = zoomFactor * FOREST_BL_Y;
  lv_drawPosX[FOREST_AR] = zoomFactor * FOREST_AR_X;
  lv_drawPosY[FOREST_AR] = zoomFactor * FOREST_AR_Y;
  lv_drawPosX[FOREST_AL] = zoomFactor * FOREST_AL_X;
  lv_drawPosY[FOREST_AL] = zoomFactor * FOREST_AL_Y;
  lv_drawPosX[FOREST_ABOVE] = zoomFactor * FOREST_ABOVE_X;
  lv_drawPosY[FOREST_ABOVE] = zoomFactor * FOREST_ABOVE_Y;
  lv_drawPosX[FOREST_BELOW] = zoomFactor * FOREST_BELOW_X;
  lv_drawPosY[FOREST_BELOW] = zoomFactor * FOREST_BELOW_Y;
  lv_drawPosX[FOREST_LEFT] = zoomFactor * FOREST_LEFT_X;
  lv_drawPosY[FOREST_LEFT] = zoomFactor * FOREST_LEFT_Y;
  lv_drawPosX[FOREST_RIGHT] = zoomFactor * FOREST_RIGHT_X;
  lv_drawPosY[FOREST_RIGHT] = zoomFactor * FOREST_RIGHT_Y;

  lv_drawPosX[RUBBLE] = zoomFactor * RUBBLE_X;
  lv_drawPosY[RUBBLE] = zoomFactor * RUBBLE_Y;
  lv_drawPosX[GRASS] = zoomFactor * GRASS_X;
  lv_drawPosY[GRASS] = zoomFactor * GRASS_Y;
  lv_drawPosX[HALFBUILDING] = zoomFactor * SHOT_BUILDING_X;
  lv_drawPosY[HALFBUILDING] = zoomFactor * SHOT_BUILDING_Y;

  lv_drawPosX[BOAT_0] = zoomFactor * BOAT0_X;
  lv_drawPosY[BOAT_0] = zoomFactor * BOAT0_Y;
  lv_drawPosX[BOAT_1] = zoomFactor * BOAT1_X;
  lv_drawPosY[BOAT_1] = zoomFactor * BOAT1_Y;
  lv_drawPosX[BOAT_2] = zoomFactor * BOAT2_X;
  lv_drawPosY[BOAT_2] = zoomFactor * BOAT2_Y;
  lv_drawPosX[BOAT_3] = zoomFactor * BOAT3_X;
  lv_drawPosY[BOAT_3] = zoomFactor * BOAT3_Y;
  lv_drawPosX[BOAT_4] = zoomFactor * BOAT4_X;
  lv_drawPosY[BOAT_4] = zoomFactor * BOAT4_Y;
  lv_drawPosX[BOAT_5] = zoomFactor * BOAT5_X;
  lv_drawPosY[BOAT_5] = zoomFactor * BOAT5_Y;
  lv_drawPosX[BOAT_6] = zoomFactor * BOAT6_X;
  lv_drawPosY[BOAT_6] = zoomFactor * BOAT6_Y;
  lv_drawPosX[BOAT_7] = zoomFactor * BOAT7_X;
  lv_drawPosY[BOAT_7] = zoomFactor * BOAT7_Y;


  /* Draw Tank frames */

  /* Do I want to do this?
  drawTankPosX[TANK_SELF_0] = zoomFactor * TANK_SELF_0_X;
  drawTankPosY[TANK_SELF_0] = zoomFactor * TANK_SELF_0_Y;
  drawTankPosX[TANK_SELF_1] = zoomFactor * TANK_SELF_1_X;
  drawTankPosY[TANK_SELF_1] = zoomFactor * TANK_SELF_1_Y;
  drawTankPosX[TANK_SELF_2] = zoomFactor * TANK_SELF_2_X;
  drawTankPosY[TANK_SELF_2] = zoomFactor * TANK_SELF_2_Y;
  drawTankPosX[TANK_SELF_3] = zoomFactor * TANK_SELF_3_X;
  drawTankPosY[TANK_SELF_3] = zoomFactor * TANK_SELF_3_Y;
  drawTankPosX[TANK_SELF_4] = zoomFactor * TANK_SELF_4_X;
  drawTankPosY[TANK_SELF_4] = zoomFactor * TANK_SELF_4_Y;
  drawTankPosX[TANK_SELF_5] = zoomFactor * TANK_SELF_5_X;
  drawTankPosY[TANK_SELF_5] = zoomFactor * TANK_SELF_5_Y;
  drawTankPosX[TANK_SELF_6] = zoomFactor * TANK_SELF_6_X;
  drawTankPosY[TANK_SELF_6] = zoomFactor * TANK_SELF_6_Y;
  drawTankPosX[TANK_SELF_7] = zoomFactor * TANK_SELF_7_X;
  drawTankPosY[TANK_SELF_7] = zoomFactor * TANK_SELF_7_Y;
  drawTankPosX[TANK_SELF_8] = zoomFactor * TANK_SELF_8_X;
  drawTankPosY[TANK_SELF_8] = zoomFactor * TANK_SELF_8_Y;
  drawTankPosX[TANK_SELF_9] = zoomFactor * TANK_SELF_9_X;
  drawTankPosY[TANK_SELF_9] = zoomFactor * TANK_SELF_9_Y;
  drawTankPosX[TANK_SELF_10] = zoomFactor * TANK_SELF_10_X;
  drawTankPosY[TANK_SELF_10] = zoomFactor * TANK_SELF_10_Y;
  drawTankPosX[TANK_SELF_11] = zoomFactor * TANK_SELF_11_X;
  drawTankPosY[TANK_SELF_11] = zoomFactor * TANK_SELF_11_Y;
  drawTankPosX[TANK_SELF_12] = zoomFactor * TANK_SELF_12_X;
  drawTankPosY[TANK_SELF_12] = zoomFactor * TANK_SELF_12_Y;
  drawTankPosX[TANK_SELF_13] = zoomFactor * TANK_SELF_13_X;
  drawTankPosY[TANK_SELF_13] = zoomFactor * TANK_SELF_13_Y;
  drawTankPosX[TANK_SELF_14] = zoomFactor * TANK_SELF_14_X;
  drawTankPosY[TANK_SELF_14] = zoomFactor * TANK_SELF_14_Y;
  drawTankPosX[TANK_SELF_15] = zoomFactor * TANK_SELF_15_X;
  drawTankPosY[TANK_SELF_15] = zoomFactor * TANK_SELF_15_Y;

  drawTankPosX[TANK_SELFBOAT_0] = zoomFactor * TANK_SELFBOAT_0_X;
  drawTankPosY[TANK_SELFBOAT_0] = zoomFactor * TANK_SELFBOAT_0_Y;
  drawTankPosX[TANK_SELFBOAT_1] = zoomFactor * TANK_SELFBOAT_1_X;
  drawTankPosY[TANK_SELFBOAT_1] = zoomFactor * TANK_SELFBOAT_1_Y;
  drawTankPosX[TANK_SELFBOAT_2] = zoomFactor * TANK_SELFBOAT_2_X;
  drawTankPosY[TANK_SELFBOAT_2] = zoomFactor * TANK_SELFBOAT_2_Y;
  drawTankPosX[TANK_SELFBOAT_3] = zoomFactor * TANK_SELFBOAT_3_X;
  drawTankPosY[TANK_SELFBOAT_3] = zoomFactor * TANK_SELFBOAT_3_Y;
  drawTankPosX[TANK_SELFBOAT_4] = zoomFactor * TANK_SELFBOAT_4_X;
  drawTankPosY[TANK_SELFBOAT_4] = zoomFactor * TANK_SELFBOAT_4_Y;
  drawTankPosX[TANK_SELFBOAT_5] = zoomFactor * TANK_SELFBOAT_5_X;
  drawTankPosY[TANK_SELFBOAT_5] = zoomFactor * TANK_SELFBOAT_5_Y;
  drawTankPosX[TANK_SELFBOAT_6] = zoomFactor * TANK_SELFBOAT_6_X;
  drawTankPosY[TANK_SELFBOAT_6] = zoomFactor * TANK_SELFBOAT_6_Y;
  drawTankPosX[TANK_SELFBOAT_7] = zoomFactor * TANK_SELFBOAT_7_X;
  drawTankPosY[TANK_SELFBOAT_7] = zoomFactor * TANK_SELFBOAT_7_Y;
  drawTankPosX[TANK_SELFBOAT_8] = zoomFactor * TANK_SELFBOAT_8_X;
  drawTankPosY[TANK_SELFBOAT_8] = zoomFactor * TANK_SELFBOAT_8_Y;
  drawTankPosX[TANK_SELFBOAT_9] = zoomFactor * TANK_SELFBOAT_9_X;
  drawTankPosY[TANK_SELFBOAT_9] = zoomFactor * TANK_SELFBOAT_9_Y;
  drawTankPosX[TANK_SELFBOAT_10] = zoomFactor * TANK_SELFBOAT_10_X;
  drawTankPosY[TANK_SELFBOAT_10] = zoomFactor * TANK_SELFBOAT_10_Y;
  drawTankPosX[TANK_SELFBOAT_11] = zoomFactor * TANK_SELFBOAT_11_X;
  drawTankPosY[TANK_SELFBOAT_11] = zoomFactor * TANK_SELFBOAT_11_Y;
  drawTankPosX[TANK_SELFBOAT_12] = zoomFactor * TANK_SELFBOAT_12_X;
  drawTankPosY[TANK_SELFBOAT_12] = zoomFactor * TANK_SELFBOAT_12_Y;
  drawTankPosX[TANK_SELFBOAT_13] = zoomFactor * TANK_SELFBOAT_13_X;
  drawTankPosY[TANK_SELFBOAT_13] = zoomFactor * TANK_SELFBOAT_13_Y;
  drawTankPosX[TANK_SELFBOAT_14] = zoomFactor * TANK_SELFBOAT_14_X;
  drawTankPosY[TANK_SELFBOAT_14] = zoomFactor * TANK_SELFBOAT_14_Y;
  drawTankPosX[TANK_SELFBOAT_15] = zoomFactor * TANK_SELFBOAT_15_X;
  drawTankPosY[TANK_SELFBOAT_15] = zoomFactor * TANK_SELFBOAT_15_Y; */
}