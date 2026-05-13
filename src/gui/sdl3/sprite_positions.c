/*
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
 * Name:          sprite_positions.c
 * Purpose:
 *   Definitions for mapViewPosX/Y and mapViewInit().
 *   Split out of mapview.c so modules that only need the
 *   tile-number-to-atlas lookup (e.g. the log viewer) can
 *   link this small file alone, without dragging in
 *   mapview's draw helpers and their bolo screen.c
 *   dependencies.
 *********************************************************/

#include "sprite_positions.h"
#include "../tiles.h"
#include "global.h"   /* SWAMP, FOREST, GRASS, RUBBLE, CRATER, HALFBUILDING */
#include "tilenum.h"

#include <string.h>

/* Tile-number -> atlas-coordinate lookup tables */
int mapViewPosX[256];
int mapViewPosY[256];

/*********************************************************
 * mapViewInit — populate lookup tables. Always stores base
 * atlas coords (scale 1); callers multiply by their atlas
 * sheet scale at blit time.
 *********************************************************/
void mapViewInit(void) {
  memset(mapViewPosX, 0, sizeof(mapViewPosX));
  memset(mapViewPosY, 0, sizeof(mapViewPosY));

  mapViewPosX[DEEP_SEA_SOLID] = DEEP_SEA_SOLID_X;
  mapViewPosY[DEEP_SEA_SOLID] = DEEP_SEA_SOLID_Y;
  mapViewPosX[DEEP_SEA_CORN1] = DEEP_SEA_CORN1_X;
  mapViewPosY[DEEP_SEA_CORN1] = DEEP_SEA_CORN1_Y;
  mapViewPosX[DEEP_SEA_CORN2] = DEEP_SEA_CORN2_X;
  mapViewPosY[DEEP_SEA_CORN2] = DEEP_SEA_CORN2_Y;
  mapViewPosX[DEEP_SEA_CORN3] = DEEP_SEA_CORN3_X;
  mapViewPosY[DEEP_SEA_CORN3] = DEEP_SEA_CORN3_Y;
  mapViewPosX[DEEP_SEA_CORN4] = DEEP_SEA_CORN4_X;
  mapViewPosY[DEEP_SEA_CORN4] = DEEP_SEA_CORN4_Y;
  mapViewPosX[DEEP_SEA_SIDE1] = DEEP_SEA_SIDE1_X;
  mapViewPosY[DEEP_SEA_SIDE1] = DEEP_SEA_SIDE1_Y;
  mapViewPosX[DEEP_SEA_SIDE2] = DEEP_SEA_SIDE2_X;
  mapViewPosY[DEEP_SEA_SIDE2] = DEEP_SEA_SIDE2_Y;
  mapViewPosX[DEEP_SEA_SIDE3] = DEEP_SEA_SIDE3_X;
  mapViewPosY[DEEP_SEA_SIDE3] = DEEP_SEA_SIDE3_Y;
  mapViewPosX[DEEP_SEA_SIDE4] = DEEP_SEA_SIDE4_X;
  mapViewPosY[DEEP_SEA_SIDE4] = DEEP_SEA_SIDE4_Y;

  mapViewPosX[BUILD_SINGLE] = BUILD_SINGLE_X;
  mapViewPosY[BUILD_SINGLE] = BUILD_SINGLE_Y;
  mapViewPosX[BUILD_SOLID] = BUILD_SOLID_X;
  mapViewPosY[BUILD_SOLID] = BUILD_SOLID_Y;
  mapViewPosX[BUILD_CORNER1] = BUILD_CORNER1_X;
  mapViewPosY[BUILD_CORNER1] = BUILD_CORNER1_Y;
  mapViewPosX[BUILD_CORNER2] = BUILD_CORNER2_X;
  mapViewPosY[BUILD_CORNER2] = BUILD_CORNER2_Y;
  mapViewPosX[BUILD_CORNER3] = BUILD_CORNER3_X;
  mapViewPosY[BUILD_CORNER3] = BUILD_CORNER3_Y;
  mapViewPosX[BUILD_CORNER4] = BUILD_CORNER4_X;
  mapViewPosY[BUILD_CORNER4] = BUILD_CORNER4_Y;
  mapViewPosX[BUILD_L1] = BUILD_L1_X;
  mapViewPosY[BUILD_L1] = BUILD_L1_Y;
  mapViewPosX[BUILD_L2] = BUILD_L2_X;
  mapViewPosY[BUILD_L2] = BUILD_L2_Y;
  mapViewPosX[BUILD_L3] = BUILD_L3_X;
  mapViewPosY[BUILD_L3] = BUILD_L3_Y;
  mapViewPosX[BUILD_L4] = BUILD_L4_X;
  mapViewPosY[BUILD_L4] = BUILD_L4_Y;
  mapViewPosX[BUILD_T1] = BUILD_T1_X;
  mapViewPosY[BUILD_T1] = BUILD_T1_Y;
  mapViewPosX[BUILD_T2] = BUILD_T2_X;
  mapViewPosY[BUILD_T2] = BUILD_T2_Y;
  mapViewPosX[BUILD_T3] = BUILD_T3_X;
  mapViewPosY[BUILD_T3] = BUILD_T3_Y;
  mapViewPosX[BUILD_T4] = BUILD_T4_X;
  mapViewPosY[BUILD_T4] = BUILD_T4_Y;
  mapViewPosX[BUILD_HORZ] = BUILD_HORZ_X;
  mapViewPosY[BUILD_HORZ] = BUILD_HORZ_Y;
  mapViewPosX[BUILD_VERT] = BUILD_VERT_X;
  mapViewPosY[BUILD_VERT] = BUILD_VERT_Y;
  mapViewPosX[BUILD_VERTEND1] = BUILD_VERTEND1_X;
  mapViewPosY[BUILD_VERTEND1] = BUILD_VERTEND1_Y;
  mapViewPosX[BUILD_VERTEND2] = BUILD_VERTEND2_X;
  mapViewPosY[BUILD_VERTEND2] = BUILD_VERTEND2_Y;
  mapViewPosX[BUILD_HORZEND1] = BUILD_HORZEND1_X;
  mapViewPosY[BUILD_HORZEND1] = BUILD_HORZEND1_Y;
  mapViewPosX[BUILD_HORZEND2] = BUILD_HORZEND2_X;
  mapViewPosY[BUILD_HORZEND2] = BUILD_HORZEND2_Y;
  mapViewPosX[BUILD_CROSS] = BUILD_CROSS_X;
  mapViewPosY[BUILD_CROSS] = BUILD_CROSS_Y;
  mapViewPosX[BUILD_SIDE1] = BUILD_SIDE1_X;
  mapViewPosY[BUILD_SIDE1] = BUILD_SIDE1_Y;
  mapViewPosX[BUILD_SIDE2] = BUILD_SIDE2_X;
  mapViewPosY[BUILD_SIDE2] = BUILD_SIDE2_Y;
  mapViewPosX[BUILD_SIDE3] = BUILD_SIDE3_X;
  mapViewPosY[BUILD_SIDE3] = BUILD_SIDE3_Y;
  mapViewPosX[BUILD_SIDE4] = BUILD_SIDE4_X;
  mapViewPosY[BUILD_SIDE4] = BUILD_SIDE4_Y;
  mapViewPosX[BUILD_SIDECORN1] = BUILD_SIDECORN1_X;
  mapViewPosY[BUILD_SIDECORN1] = BUILD_SIDECORN1_Y;
  mapViewPosX[BUILD_SIDECORN2] = BUILD_SIDECORN2_X;
  mapViewPosY[BUILD_SIDECORN2] = BUILD_SIDECORN2_Y;
  mapViewPosX[BUILD_SIDECORN3] = BUILD_SIDECORN3_X;
  mapViewPosY[BUILD_SIDECORN3] = BUILD_SIDECORN3_Y;
  mapViewPosX[BUILD_SIDECORN4] = BUILD_SIDECORN4_X;
  mapViewPosY[BUILD_SIDECORN4] = BUILD_SIDECORN4_Y;
  mapViewPosX[BUILD_SIDECORN5] = BUILD_SIDECORN5_X;
  mapViewPosY[BUILD_SIDECORN5] = BUILD_SIDECORN5_Y;
  mapViewPosX[BUILD_SIDECORN6] = BUILD_SIDECORN6_X;
  mapViewPosY[BUILD_SIDECORN6] = BUILD_SIDECORN6_Y;
  mapViewPosX[BUILD_SIDECORN7] = BUILD_SIDECORN7_X;
  mapViewPosY[BUILD_SIDECORN7] = BUILD_SIDECORN7_Y;
  mapViewPosX[BUILD_SIDECORN8] = BUILD_SIDECORN8_X;
  mapViewPosY[BUILD_SIDECORN8] = BUILD_SIDECORN8_Y;
  mapViewPosX[BUILD_SIDECORN9] = BUILD_SIDECORN9_X;
  mapViewPosY[BUILD_SIDECORN9] = BUILD_SIDECORN9_Y;
  mapViewPosX[BUILD_SIDECORN10] = BUILD_SIDECORN10_X;
  mapViewPosY[BUILD_SIDECORN10] = BUILD_SIDECORN10_Y;
  mapViewPosX[BUILD_SIDECORN11] = BUILD_SIDECORN11_X;
  mapViewPosY[BUILD_SIDECORN11] = BUILD_SIDECORN11_Y;
  mapViewPosX[BUILD_SIDECORN12] = BUILD_SIDECORN12_X;
  mapViewPosY[BUILD_SIDECORN12] = BUILD_SIDECORN12_Y;
  mapViewPosX[BUILD_SIDECORN13] = BUILD_SIDECORN13_X;
  mapViewPosY[BUILD_SIDECORN13] = BUILD_SIDECORN13_Y;
  mapViewPosX[BUILD_SIDECORN14] = BUILD_SIDECORN14_X;
  mapViewPosY[BUILD_SIDECORN14] = BUILD_SIDECORN14_Y;
  mapViewPosX[BUILD_SIDECORN15] = BUILD_SIDECORN15_X;
  mapViewPosY[BUILD_SIDECORN15] = BUILD_SIDECORN15_Y;
  mapViewPosX[BUILD_SIDECORN16] = BUILD_SIDECORN16_X;
  mapViewPosY[BUILD_SIDECORN16] = BUILD_SIDECORN16_Y;
  mapViewPosX[BUILD_TWIST1] = BUILD_TWIST1_X;
  mapViewPosY[BUILD_TWIST1] = BUILD_TWIST1_Y;
  mapViewPosX[BUILD_TWIST2] = BUILD_TWIST2_X;
  mapViewPosY[BUILD_TWIST2] = BUILD_TWIST2_Y;
  mapViewPosX[BUILD_MOST1] = BUILD_MOST1_X;
  mapViewPosY[BUILD_MOST1] = BUILD_MOST1_Y;
  mapViewPosX[BUILD_MOST2] = BUILD_MOST2_X;
  mapViewPosY[BUILD_MOST2] = BUILD_MOST2_Y;
  mapViewPosX[BUILD_MOST3] = BUILD_MOST3_X;
  mapViewPosY[BUILD_MOST3] = BUILD_MOST3_Y;
  mapViewPosX[BUILD_MOST4] = BUILD_MOST4_X;
  mapViewPosY[BUILD_MOST4] = BUILD_MOST4_Y;

  mapViewPosX[RIVER_END1] = RIVER_END1_X;
  mapViewPosY[RIVER_END1] = RIVER_END1_Y;
  mapViewPosX[RIVER_END2] = RIVER_END2_X;
  mapViewPosY[RIVER_END2] = RIVER_END2_Y;
  mapViewPosX[RIVER_END3] = RIVER_END3_X;
  mapViewPosY[RIVER_END3] = RIVER_END3_Y;
  mapViewPosX[RIVER_END4] = RIVER_END4_X;
  mapViewPosY[RIVER_END4] = RIVER_END4_Y;
  mapViewPosX[RIVER_SOLID] = RIVER_SOLID_X;
  mapViewPosY[RIVER_SOLID] = RIVER_SOLID_Y;
  mapViewPosX[RIVER_SURROUND] = RIVER_SURROUND_X;
  mapViewPosY[RIVER_SURROUND] = RIVER_SURROUND_Y;
  mapViewPosX[RIVER_SIDE1] = RIVER_SIDE1_X;
  mapViewPosY[RIVER_SIDE1] = RIVER_SIDE1_Y;
  mapViewPosX[RIVER_SIDE2] = RIVER_SIDE2_X;
  mapViewPosY[RIVER_SIDE2] = RIVER_SIDE2_Y;
  mapViewPosX[RIVER_ONESIDE1] = RIVER_ONESIDE1_X;
  mapViewPosY[RIVER_ONESIDE1] = RIVER_ONESIDE1_Y;
  mapViewPosX[RIVER_ONESIDE2] = RIVER_ONESIDE2_X;
  mapViewPosY[RIVER_ONESIDE2] = RIVER_ONESIDE2_Y;
  mapViewPosX[RIVER_ONESIDE3] = RIVER_ONESIDE3_X;
  mapViewPosY[RIVER_ONESIDE3] = RIVER_ONESIDE3_Y;
  mapViewPosX[RIVER_ONESIDE4] = RIVER_ONESIDE4_X;
  mapViewPosY[RIVER_ONESIDE4] = RIVER_ONESIDE4_Y;
  mapViewPosX[RIVER_CORN1] = RIVER_CORN1_X;
  mapViewPosY[RIVER_CORN1] = RIVER_CORN1_Y;
  mapViewPosX[RIVER_CORN2] = RIVER_CORN2_X;
  mapViewPosY[RIVER_CORN2] = RIVER_CORN2_Y;
  mapViewPosX[RIVER_CORN3] = RIVER_CORN3_X;
  mapViewPosY[RIVER_CORN3] = RIVER_CORN3_Y;
  mapViewPosX[RIVER_CORN4] = RIVER_CORN4_X;
  mapViewPosY[RIVER_CORN4] = RIVER_CORN4_Y;

  mapViewPosX[SWAMP] = SWAMP_X;
  mapViewPosY[SWAMP] = SWAMP_Y;

  mapViewPosX[ROAD_CORNER1] = ROAD_CORNER1_X;
  mapViewPosY[ROAD_CORNER1] = ROAD_CORNER1_Y;
  mapViewPosX[ROAD_CORNER2] = ROAD_CORNER2_X;
  mapViewPosY[ROAD_CORNER2] = ROAD_CORNER2_Y;
  mapViewPosX[ROAD_CORNER3] = ROAD_CORNER3_X;
  mapViewPosY[ROAD_CORNER3] = ROAD_CORNER3_Y;
  mapViewPosX[ROAD_CORNER4] = ROAD_CORNER4_X;
  mapViewPosY[ROAD_CORNER4] = ROAD_CORNER4_Y;
  mapViewPosX[ROAD_CORNER5] = ROAD_CORNER5_X;
  mapViewPosY[ROAD_CORNER5] = ROAD_CORNER5_Y;
  mapViewPosX[ROAD_CORNER6] = ROAD_CORNER6_X;
  mapViewPosY[ROAD_CORNER6] = ROAD_CORNER6_Y;
  mapViewPosX[ROAD_CORNER7] = ROAD_CORNER7_X;
  mapViewPosY[ROAD_CORNER7] = ROAD_CORNER7_Y;
  mapViewPosX[ROAD_CORNER8] = ROAD_CORNER8_X;
  mapViewPosY[ROAD_CORNER8] = ROAD_CORNER8_Y;
  mapViewPosX[ROAD_SIDE1] = ROAD_SIDE1_X;
  mapViewPosY[ROAD_SIDE1] = ROAD_SIDE1_Y;
  mapViewPosX[ROAD_SIDE2] = ROAD_SIDE2_X;
  mapViewPosY[ROAD_SIDE2] = ROAD_SIDE2_Y;
  mapViewPosX[ROAD_SIDE3] = ROAD_SIDE3_X;
  mapViewPosY[ROAD_SIDE3] = ROAD_SIDE3_Y;
  mapViewPosX[ROAD_SIDE4] = ROAD_SIDE4_X;
  mapViewPosY[ROAD_SIDE4] = ROAD_SIDE4_Y;
  mapViewPosX[ROAD_SOLID] = ROAD_SOLID_X;
  mapViewPosY[ROAD_SOLID] = ROAD_SOLID_Y;
  mapViewPosX[ROAD_CROSS] = ROAD_CROSS_X;
  mapViewPosY[ROAD_CROSS] = ROAD_CROSS_Y;
  mapViewPosX[ROAD_T1] = ROAD_T1_X;
  mapViewPosY[ROAD_T1] = ROAD_T1_Y;
  mapViewPosX[ROAD_T2] = ROAD_T2_X;
  mapViewPosY[ROAD_T2] = ROAD_T2_Y;
  mapViewPosX[ROAD_T3] = ROAD_T3_X;
  mapViewPosY[ROAD_T3] = ROAD_T3_Y;
  mapViewPosX[ROAD_T4] = ROAD_T4_X;
  mapViewPosY[ROAD_T4] = ROAD_T4_Y;
  mapViewPosX[ROAD_HORZ] = ROAD_HORZ_X;
  mapViewPosY[ROAD_HORZ] = ROAD_HORZ_Y;
  mapViewPosX[ROAD_VERT] = ROAD_VERT_X;
  mapViewPosY[ROAD_VERT] = ROAD_VERT_Y;
  mapViewPosX[ROAD_WATER1] = ROAD_WATER1_X;
  mapViewPosY[ROAD_WATER1] = ROAD_WATER1_Y;
  mapViewPosX[ROAD_WATER2] = ROAD_WATER2_X;
  mapViewPosY[ROAD_WATER2] = ROAD_WATER2_Y;
  mapViewPosX[ROAD_WATER3] = ROAD_WATER3_X;
  mapViewPosY[ROAD_WATER3] = ROAD_WATER3_Y;
  mapViewPosX[ROAD_WATER4] = ROAD_WATER4_X;
  mapViewPosY[ROAD_WATER4] = ROAD_WATER4_Y;
  mapViewPosX[ROAD_WATER5] = ROAD_WATER5_X;
  mapViewPosY[ROAD_WATER5] = ROAD_WATER5_Y;
  mapViewPosX[ROAD_WATER6] = ROAD_WATER6_X;
  mapViewPosY[ROAD_WATER6] = ROAD_WATER6_Y;
  mapViewPosX[ROAD_WATER7] = ROAD_WATER7_X;
  mapViewPosY[ROAD_WATER7] = ROAD_WATER7_Y;
  mapViewPosX[ROAD_WATER8] = ROAD_WATER8_X;
  mapViewPosY[ROAD_WATER8] = ROAD_WATER8_Y;
  mapViewPosX[ROAD_WATER9] = ROAD_WATER9_X;
  mapViewPosY[ROAD_WATER9] = ROAD_WATER9_Y;
  mapViewPosX[ROAD_WATER10] = ROAD_WATER10_X;
  mapViewPosY[ROAD_WATER10] = ROAD_WATER10_Y;
  mapViewPosX[ROAD_WATER11] = ROAD_WATER11_X;
  mapViewPosY[ROAD_WATER11] = ROAD_WATER11_Y;

  mapViewPosX[PILL_EVIL_15] = PILL_EVIL15_X;
  mapViewPosY[PILL_EVIL_15] = PILL_EVIL15_Y;
  mapViewPosX[PILL_EVIL_14] = PILL_EVIL14_X;
  mapViewPosY[PILL_EVIL_14] = PILL_EVIL14_Y;
  mapViewPosX[PILL_EVIL_13] = PILL_EVIL13_X;
  mapViewPosY[PILL_EVIL_13] = PILL_EVIL13_Y;
  mapViewPosX[PILL_EVIL_12] = PILL_EVIL12_X;
  mapViewPosY[PILL_EVIL_12] = PILL_EVIL12_Y;
  mapViewPosX[PILL_EVIL_11] = PILL_EVIL11_X;
  mapViewPosY[PILL_EVIL_11] = PILL_EVIL11_Y;
  mapViewPosX[PILL_EVIL_10] = PILL_EVIL10_X;
  mapViewPosY[PILL_EVIL_10] = PILL_EVIL10_Y;
  mapViewPosX[PILL_EVIL_9] = PILL_EVIL9_X;
  mapViewPosY[PILL_EVIL_9] = PILL_EVIL9_Y;
  mapViewPosX[PILL_EVIL_8] = PILL_EVIL8_X;
  mapViewPosY[PILL_EVIL_8] = PILL_EVIL8_Y;
  mapViewPosX[PILL_EVIL_7] = PILL_EVIL7_X;
  mapViewPosY[PILL_EVIL_7] = PILL_EVIL7_Y;
  mapViewPosX[PILL_EVIL_6] = PILL_EVIL6_X;
  mapViewPosY[PILL_EVIL_6] = PILL_EVIL6_Y;
  mapViewPosX[PILL_EVIL_5] = PILL_EVIL5_X;
  mapViewPosY[PILL_EVIL_5] = PILL_EVIL5_Y;
  mapViewPosX[PILL_EVIL_4] = PILL_EVIL4_X;
  mapViewPosY[PILL_EVIL_4] = PILL_EVIL4_Y;
  mapViewPosX[PILL_EVIL_3] = PILL_EVIL3_X;
  mapViewPosY[PILL_EVIL_3] = PILL_EVIL3_Y;
  mapViewPosX[PILL_EVIL_2] = PILL_EVIL2_X;
  mapViewPosY[PILL_EVIL_2] = PILL_EVIL2_Y;
  mapViewPosX[PILL_EVIL_1] = PILL_EVIL1_X;
  mapViewPosY[PILL_EVIL_1] = PILL_EVIL1_Y;
  mapViewPosX[PILL_EVIL_0] = PILL_EVIL0_X;
  mapViewPosY[PILL_EVIL_0] = PILL_EVIL0_Y;

  mapViewPosX[PILL_GOOD_15] = PILL_GOOD15_X;
  mapViewPosY[PILL_GOOD_15] = PILL_GOOD15_Y;
  mapViewPosX[PILL_GOOD_14] = PILL_GOOD14_X;
  mapViewPosY[PILL_GOOD_14] = PILL_GOOD14_Y;
  mapViewPosX[PILL_GOOD_13] = PILL_GOOD13_X;
  mapViewPosY[PILL_GOOD_13] = PILL_GOOD13_Y;
  mapViewPosX[PILL_GOOD_12] = PILL_GOOD12_X;
  mapViewPosY[PILL_GOOD_12] = PILL_GOOD12_Y;
  mapViewPosX[PILL_GOOD_11] = PILL_GOOD11_X;
  mapViewPosY[PILL_GOOD_11] = PILL_GOOD11_Y;
  mapViewPosX[PILL_GOOD_10] = PILL_GOOD10_X;
  mapViewPosY[PILL_GOOD_10] = PILL_GOOD10_Y;
  mapViewPosX[PILL_GOOD_9] = PILL_GOOD9_X;
  mapViewPosY[PILL_GOOD_9] = PILL_GOOD9_Y;
  mapViewPosX[PILL_GOOD_8] = PILL_GOOD8_X;
  mapViewPosY[PILL_GOOD_8] = PILL_GOOD8_Y;
  mapViewPosX[PILL_GOOD_7] = PILL_GOOD7_X;
  mapViewPosY[PILL_GOOD_7] = PILL_GOOD7_Y;
  mapViewPosX[PILL_GOOD_6] = PILL_GOOD6_X;
  mapViewPosY[PILL_GOOD_6] = PILL_GOOD6_Y;
  mapViewPosX[PILL_GOOD_5] = PILL_GOOD5_X;
  mapViewPosY[PILL_GOOD_5] = PILL_GOOD5_Y;
  mapViewPosX[PILL_GOOD_4] = PILL_GOOD4_X;
  mapViewPosY[PILL_GOOD_4] = PILL_GOOD4_Y;
  mapViewPosX[PILL_GOOD_3] = PILL_GOOD3_X;
  mapViewPosY[PILL_GOOD_3] = PILL_GOOD3_Y;
  mapViewPosX[PILL_GOOD_2] = PILL_GOOD2_X;
  mapViewPosY[PILL_GOOD_2] = PILL_GOOD2_Y;
  mapViewPosX[PILL_GOOD_1] = PILL_GOOD1_X;
  mapViewPosY[PILL_GOOD_1] = PILL_GOOD1_Y;
  mapViewPosX[PILL_GOOD_0] = PILL_GOOD0_X;
  mapViewPosY[PILL_GOOD_0] = PILL_GOOD0_Y;

  mapViewPosX[BASE_GOOD] = BASE_GOOD_X;
  mapViewPosY[BASE_GOOD] = BASE_GOOD_Y;
  mapViewPosX[BASE_NEUTRAL] = BASE_NEUTRAL_X;
  mapViewPosY[BASE_NEUTRAL] = BASE_NEUTRAL_Y;
  mapViewPosX[BASE_EVIL] = BASE_EVIL_X;
  mapViewPosY[BASE_EVIL] = BASE_EVIL_Y;

  mapViewPosX[FOREST] = FOREST_X;
  mapViewPosY[FOREST] = FOREST_Y;
  mapViewPosX[FOREST_SINGLE] = FOREST_SINGLE_X;
  mapViewPosY[FOREST_SINGLE] = FOREST_SINGLE_Y;
  mapViewPosX[FOREST_BR] = FOREST_BR_X;
  mapViewPosY[FOREST_BR] = FOREST_BR_Y;
  mapViewPosX[FOREST_BL] = FOREST_BL_X;
  mapViewPosY[FOREST_BL] = FOREST_BL_Y;
  mapViewPosX[FOREST_AR] = FOREST_AR_X;
  mapViewPosY[FOREST_AR] = FOREST_AR_Y;
  mapViewPosX[FOREST_AL] = FOREST_AL_X;
  mapViewPosY[FOREST_AL] = FOREST_AL_Y;
  mapViewPosX[FOREST_ABOVE] = FOREST_ABOVE_X;
  mapViewPosY[FOREST_ABOVE] = FOREST_ABOVE_Y;
  mapViewPosX[FOREST_BELOW] = FOREST_BELOW_X;
  mapViewPosY[FOREST_BELOW] = FOREST_BELOW_Y;
  mapViewPosX[FOREST_LEFT] = FOREST_LEFT_X;
  mapViewPosY[FOREST_LEFT] = FOREST_LEFT_Y;
  mapViewPosX[FOREST_RIGHT] = FOREST_RIGHT_X;
  mapViewPosY[FOREST_RIGHT] = FOREST_RIGHT_Y;

  mapViewPosX[CRATER] = CRATER_X;
  mapViewPosY[CRATER] = CRATER_Y;
  mapViewPosX[CRATER_SINGLE] = CRATER_SINGLE_X;
  mapViewPosY[CRATER_SINGLE] = CRATER_SINGLE_Y;
  mapViewPosX[CRATER_BR] = CRATER_BR_X;
  mapViewPosY[CRATER_BR] = CRATER_BR_Y;
  mapViewPosX[CRATER_BL] = CRATER_BL_X;
  mapViewPosY[CRATER_BL] = CRATER_BL_Y;
  mapViewPosX[CRATER_AR] = CRATER_AR_X;
  mapViewPosY[CRATER_AR] = CRATER_AR_Y;
  mapViewPosX[CRATER_AL] = CRATER_AL_X;
  mapViewPosY[CRATER_AL] = CRATER_AL_Y;
  mapViewPosX[CRATER_ABOVE] = CRATER_ABOVE_X;
  mapViewPosY[CRATER_ABOVE] = CRATER_ABOVE_Y;
  mapViewPosX[CRATER_BELOW] = CRATER_BELOW_X;
  mapViewPosY[CRATER_BELOW] = CRATER_BELOW_Y;
  mapViewPosX[CRATER_LEFT] = CRATER_LEFT_X;
  mapViewPosY[CRATER_LEFT] = CRATER_LEFT_Y;
  mapViewPosX[CRATER_RIGHT] = CRATER_RIGHT_X;
  mapViewPosY[CRATER_RIGHT] = CRATER_RIGHT_Y;

  mapViewPosX[RUBBLE] = RUBBLE_X;
  mapViewPosY[RUBBLE] = RUBBLE_Y;
  mapViewPosX[GRASS] = GRASS_X;
  mapViewPosY[GRASS] = GRASS_Y;
  mapViewPosX[HALFBUILDING] = SHOT_BUILDING_X;
  mapViewPosY[HALFBUILDING] = SHOT_BUILDING_Y;

  mapViewPosX[BOAT_0] = BOAT0_X;
  mapViewPosY[BOAT_0] = BOAT0_Y;
  mapViewPosX[BOAT_1] = BOAT1_X;
  mapViewPosY[BOAT_1] = BOAT1_Y;
  mapViewPosX[BOAT_2] = BOAT2_X;
  mapViewPosY[BOAT_2] = BOAT2_Y;
  mapViewPosX[BOAT_3] = BOAT3_X;
  mapViewPosY[BOAT_3] = BOAT3_Y;
  mapViewPosX[BOAT_4] = BOAT4_X;
  mapViewPosY[BOAT_4] = BOAT4_Y;
  mapViewPosX[BOAT_5] = BOAT5_X;
  mapViewPosY[BOAT_5] = BOAT5_Y;
  mapViewPosX[BOAT_6] = BOAT6_X;
  mapViewPosY[BOAT_6] = BOAT6_Y;
  mapViewPosX[BOAT_7] = BOAT7_X;
  mapViewPosY[BOAT_7] = BOAT7_Y;
}
