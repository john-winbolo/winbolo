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
 * Name:          tilemap.h
 * Purpose:
 *   Static table mapping every sprite name to its position
 *   on the 496x176 sprite sheet.  Names match the filenames
 *   in data/svg/ (without extension).  Coordinates come from
 *   tiles.h.
 *********************************************************/

#ifndef TILEMAP_H
#define TILEMAP_H

#include "../tiles.h"

typedef struct {
    const char *name;
    int sheetX, sheetY;
    int width, height;
} TileMapEntry;

static const TileMapEntry gTileMap[] = {
    /* ---- Row 0: Roads (y=0) ---- */
    { "road_horizontal",       ROAD_HORZ_X,       ROAD_HORZ_Y,       16, 16 },
    { "road_vertical",         ROAD_VERT_X,       ROAD_VERT_Y,       16, 16 },
    { "road_corner1",          ROAD_CORNER1_X,    ROAD_CORNER1_Y,    16, 16 },
    { "road_corner2",          ROAD_CORNER2_X,    ROAD_CORNER2_Y,    16, 16 },
    { "road_corner3",          ROAD_CORNER3_X,    ROAD_CORNER3_Y,    16, 16 },
    { "road_corner4",          ROAD_CORNER4_X,    ROAD_CORNER4_Y,    16, 16 },
    { "road_corner5_solid",    ROAD_CORNER5_X,    ROAD_CORNER5_Y,    16, 16 },
    { "road_corner6_solid",    ROAD_CORNER6_X,    ROAD_CORNER6_Y,    16, 16 },
    { "road_corner7_solid",    ROAD_CORNER7_X,    ROAD_CORNER7_Y,    16, 16 },
    { "road_corner8_solid",    ROAD_CORNER8_X,    ROAD_CORNER8_Y,    16, 16 },
    { "road_solid",            ROAD_SOLID_X,      ROAD_SOLID_Y,      16, 16 },
    { "road_crossroads",       ROAD_CROSS_X,      ROAD_CROSS_Y,      16, 16 },
    { "road_t1",               ROAD_T1_X,         ROAD_T1_Y,         16, 16 },
    { "road_t2",               ROAD_T2_X,         ROAD_T2_Y,         16, 16 },
    { "road_t3",               ROAD_T3_X,         ROAD_T3_Y,         16, 16 },
    { "road_t4",               ROAD_T4_X,         ROAD_T4_Y,         16, 16 },
    { "road_water1",           ROAD_WATER1_X,     ROAD_WATER1_Y,     16, 16 },
    { "road_water2",           ROAD_WATER2_X,     ROAD_WATER2_Y,     16, 16 },
    { "road_water3",           ROAD_WATER3_X,     ROAD_WATER3_Y,     16, 16 },
    { "road_water4",           ROAD_WATER4_X,     ROAD_WATER4_Y,     16, 16 },
    { "road_water5_corner",    ROAD_WATER5_X,     ROAD_WATER5_Y,     16, 16 },
    { "road_water6_corner",    ROAD_WATER6_X,     ROAD_WATER6_Y,     16, 16 },
    { "road_water7_corner",    ROAD_WATER7_X,     ROAD_WATER7_Y,     16, 16 },
    { "road_water8_corner",    ROAD_WATER8_X,     ROAD_WATER8_Y,     16, 16 },
    { "road_water_horizontal", ROAD_WATER9_X,     ROAD_WATER9_Y,     16, 16 },
    { "road_water_vertical",   ROAD_WATER10_X,    ROAD_WATER10_Y,    16, 16 },
    { "road_water_lone",       ROAD_WATER11_X,    ROAD_WATER11_Y,    16, 16 },
    { "road_side1",            ROAD_SIDE1_X,      ROAD_SIDE1_Y,      16, 16 },
    { "road_side2",            ROAD_SIDE2_X,      ROAD_SIDE2_Y,      16, 16 },
    { "road_side3",            ROAD_SIDE3_X,      ROAD_SIDE3_Y,      16, 16 },
    { "road_side4",            ROAD_SIDE4_X,      ROAD_SIDE4_Y,      16, 16 },

    /* ---- Row 1: Terrain + buildings start (y=16) ---- */
    { "deep_sea",              DEEP_SEA_SOLID_X,  DEEP_SEA_SOLID_Y,  16, 16 },
    { "river_solid",           RIVER_SOLID_X,     RIVER_SOLID_Y,     16, 16 },
    { "grass",                 GRASS_X,           GRASS_Y,           16, 16 },
    { "forest",                FOREST_X,          FOREST_Y,          16, 16 },
    { "rubble",                RUBBLE_X,          RUBBLE_Y,          16, 16 },
    { "crater",                CRATER_X,          CRATER_Y,          16, 16 },
    { "building_single",       BUILD_SINGLE_X,    BUILD_SINGLE_Y,    16, 16 },
    { "swamp",                 SWAMP_X,           SWAMP_Y,           16, 16 },
    { "shot_building",         SHOT_BUILDING_X,   SHOT_BUILDING_Y,   16, 16 },
    { "base_good",             BASE_GOOD_X,       BASE_GOOD_Y,       16, 16 },
    { "building_horizontal",   BUILD_HORZ_X,      BUILD_HORZ_Y,      16, 16 },
    { "building_vertical",     BUILD_VERT_X,      BUILD_VERT_Y,      16, 16 },
    { "building_horzend1",     BUILD_HORZEND1_X,  BUILD_HORZEND1_Y,  16, 16 },
    { "building_horzend2",     BUILD_HORZEND2_X,  BUILD_HORZEND2_Y,  16, 16 },
    { "building_vertend1",     BUILD_VERTEND1_X,  BUILD_VERTEND1_Y,  16, 16 },
    { "building_vertend2",     BUILD_VERTEND2_X,  BUILD_VERTEND2_Y,  16, 16 },
    { "building_solid",        BUILD_SOLID_X,     BUILD_SOLID_Y,     16, 16 },
    { "building_corner1",      BUILD_CORNER1_X,   BUILD_CORNER1_Y,   16, 16 },
    { "building_corner2",      BUILD_CORNER2_X,   BUILD_CORNER2_Y,   16, 16 },
    { "building_corner3",      BUILD_CORNER3_X,   BUILD_CORNER3_Y,   16, 16 },
    { "building_corner4",      BUILD_CORNER4_X,   BUILD_CORNER4_Y,   16, 16 },
    { "building_l1",           BUILD_L1_X,        BUILD_L1_Y,        16, 16 },
    { "building_l2",           BUILD_L2_X,        BUILD_L2_Y,        16, 16 },
    { "building_l3",           BUILD_L3_X,        BUILD_L3_Y,        16, 16 },
    { "building_l4",           BUILD_L4_X,        BUILD_L4_Y,        16, 16 },
    { "building_t1",           BUILD_T1_X,        BUILD_T1_Y,        16, 16 },
    { "building_t2",           BUILD_T2_X,        BUILD_T2_Y,        16, 16 },
    { "building_t3",           BUILD_T3_X,        BUILD_T3_Y,        16, 16 },
    { "building_t4",           BUILD_T4_X,        BUILD_T4_Y,        16, 16 },
    { "building_cross",        BUILD_CROSS_X,     BUILD_CROSS_Y,     16, 16 },

    /* ---- Row 2: Building sides + river ends (y=32) ---- */
    { "building_side1",        BUILD_SIDE1_X,     BUILD_SIDE1_Y,     16, 16 },
    { "building_side2",        BUILD_SIDE2_X,     BUILD_SIDE2_Y,     16, 16 },
    { "building_side3",        BUILD_SIDE3_X,     BUILD_SIDE3_Y,     16, 16 },
    { "building_side4",        BUILD_SIDE4_X,     BUILD_SIDE4_Y,     16, 16 },
    { "building_sidecorn1",    BUILD_SIDECORN1_X, BUILD_SIDECORN1_Y, 16, 16 },
    { "building_sidecorn2",    BUILD_SIDECORN2_X, BUILD_SIDECORN2_Y, 16, 16 },
    { "building_sidecorn3",    BUILD_SIDECORN3_X, BUILD_SIDECORN3_Y, 16, 16 },
    { "building_sidecorn4",    BUILD_SIDECORN4_X, BUILD_SIDECORN4_Y, 16, 16 },
    { "building_sidecorn5",    BUILD_SIDECORN5_X, BUILD_SIDECORN5_Y, 16, 16 },
    { "building_sidecorn6",    BUILD_SIDECORN6_X, BUILD_SIDECORN6_Y, 16, 16 },
    { "building_sidecorn7",    BUILD_SIDECORN7_X, BUILD_SIDECORN7_Y, 16, 16 },
    { "building_sidecorn8",    BUILD_SIDECORN8_X, BUILD_SIDECORN8_Y, 16, 16 },
    { "building_sidecorn9",    BUILD_SIDECORN9_X, BUILD_SIDECORN9_Y, 16, 16 },
    { "building_sidecorn10",   BUILD_SIDECORN10_X, BUILD_SIDECORN10_Y, 16, 16 },
    { "building_sidecorn11",   BUILD_SIDECORN11_X, BUILD_SIDECORN11_Y, 16, 16 },
    { "building_sidecorn12",   BUILD_SIDECORN12_X, BUILD_SIDECORN12_Y, 16, 16 },
    { "building_sidecorn13",   BUILD_SIDECORN13_X, BUILD_SIDECORN13_Y, 16, 16 },
    { "building_sidecorn14",   BUILD_SIDECORN14_X, BUILD_SIDECORN14_Y, 16, 16 },
    { "building_sidecorn15",   BUILD_SIDECORN15_X, BUILD_SIDECORN15_Y, 16, 16 },
    { "building_sidecorn16",   BUILD_SIDECORN16_X, BUILD_SIDECORN16_Y, 16, 16 },
    { "building_twist1",       BUILD_TWIST1_X,    BUILD_TWIST1_Y,    16, 16 },
    { "building_twist2",       BUILD_TWIST2_X,    BUILD_TWIST2_Y,    16, 16 },
    { "building_most1",        BUILD_MOST1_X,     BUILD_MOST1_Y,     16, 16 },
    { "building_most2",        BUILD_MOST2_X,     BUILD_MOST2_Y,     16, 16 },
    { "building_most3",        BUILD_MOST3_X,     BUILD_MOST3_Y,     16, 16 },
    { "building_most4",        BUILD_MOST4_X,     BUILD_MOST4_Y,     16, 16 },
    { "river_end1",            RIVER_END1_X,      RIVER_END1_Y,      16, 16 },
    { "river_end2",            RIVER_END2_X,      RIVER_END2_Y,      16, 16 },
    { "river_end3",            RIVER_END3_X,      RIVER_END3_Y,      16, 16 },
    { "river_end4",            RIVER_END4_X,      RIVER_END4_Y,      16, 16 },
    { "river_surround",        RIVER_SURROUND_X,  RIVER_SURROUND_Y,  16, 16 },

    /* ---- Row 3: Rivers + deep sea + boats + mine + tank + explosions (y=48) ---- */
    { "river_side1",           RIVER_SIDE1_X,     RIVER_SIDE1_Y,     16, 16 },
    { "river_side2",           RIVER_SIDE2_X,     RIVER_SIDE2_Y,     16, 16 },
    { "river_oneside1",        RIVER_ONESIDE1_X,  RIVER_ONESIDE1_Y,  16, 16 },
    { "river_oneside2",        RIVER_ONESIDE2_X,  RIVER_ONESIDE2_Y,  16, 16 },
    { "river_oneside3",        RIVER_ONESIDE3_X,  RIVER_ONESIDE3_Y,  16, 16 },
    { "river_oneside4",        RIVER_ONESIDE4_X,  RIVER_ONESIDE4_Y,  16, 16 },
    { "river_corner1",         RIVER_CORN1_X,     RIVER_CORN1_Y,     16, 16 },
    { "river_corner2",         RIVER_CORN2_X,     RIVER_CORN2_Y,     16, 16 },
    { "river_corner3",         RIVER_CORN3_X,     RIVER_CORN3_Y,     16, 16 },
    { "river_corner4",         RIVER_CORN4_X,     RIVER_CORN4_Y,     16, 16 },
    { "deep_sea_corner1",      DEEP_SEA_CORN1_X,  DEEP_SEA_CORN1_Y,  16, 16 },
    { "deep_sea_corner2",      DEEP_SEA_CORN2_X,  DEEP_SEA_CORN2_Y,  16, 16 },
    { "deep_sea_corner3",      DEEP_SEA_CORN3_X,  DEEP_SEA_CORN3_Y,  16, 16 },
    { "deep_sea_corner4",      DEEP_SEA_CORN4_X,  DEEP_SEA_CORN4_Y,  16, 16 },
    { "deep_sea_side1",        DEEP_SEA_SIDE1_X,  DEEP_SEA_SIDE1_Y,  16, 16 },
    { "deep_sea_side2",        DEEP_SEA_SIDE2_X,  DEEP_SEA_SIDE2_Y,  16, 16 },
    { "deep_sea_side3",        DEEP_SEA_SIDE3_X,  DEEP_SEA_SIDE3_Y,  16, 16 },
    { "deep_sea_side4",        DEEP_SEA_SIDE4_X,  DEEP_SEA_SIDE4_Y,  16, 16 },
    { "boat0",                 BOAT0_X,           BOAT0_Y,           16, 16 },
    { "mine",                  MINE_X,            MINE_Y,            16, 16 },
    { "tank_icon",             TANK_X,            TANK_Y,            16, 16 },
    { "explosion1",            EXPLOSION1_X,      EXPLOSION1_Y,      16, 16 },
    { "explosion2",            EXPLOSION2_X,      EXPLOSION2_Y,      16, 16 },

    /* ---- Row 4: Tank self + gunsight + base neutral + explosions (y=64) ---- */
    { "tank_self_00",          TANK_SELF_0_X,     TANK_SELF_0_Y,     16, 16 },
    { "tank_self_01",          TANK_SELF_1_X,     TANK_SELF_1_Y,     16, 16 },
    { "tank_self_02",          TANK_SELF_2_X,     TANK_SELF_2_Y,     16, 16 },
    { "tank_self_03",          TANK_SELF_3_X,     TANK_SELF_3_Y,     16, 16 },
    { "tank_self_04",          TANK_SELF_4_X,     TANK_SELF_4_Y,     16, 16 },
    { "tank_self_05",          TANK_SELF_5_X,     TANK_SELF_5_Y,     16, 16 },
    { "tank_self_06",          TANK_SELF_6_X,     TANK_SELF_6_Y,     16, 16 },
    { "tank_self_07",          TANK_SELF_7_X,     TANK_SELF_7_Y,     16, 16 },
    { "tank_self_08",          TANK_SELF_8_X,     TANK_SELF_8_Y,     16, 16 },
    { "tank_self_09",          TANK_SELF_9_X,     TANK_SELF_9_Y,     16, 16 },
    { "tank_self_10",          TANK_SELF_10_X,    TANK_SELF_10_Y,    16, 16 },
    { "tank_self_11",          TANK_SELF_11_X,    TANK_SELF_11_Y,    16, 16 },
    { "tank_self_12",          TANK_SELF_12_X,    TANK_SELF_12_Y,    16, 16 },
    { "tank_self_13",          TANK_SELF_13_X,    TANK_SELF_13_Y,    16, 16 },
    { "tank_self_14",          TANK_SELF_14_X,    TANK_SELF_14_Y,    16, 16 },
    { "tank_self_15",          TANK_SELF_15_X,    TANK_SELF_15_Y,    16, 16 },
    { "base_neutral",          BASE_NEUTRAL_X,    BASE_NEUTRAL_Y,    16, 16 },
    { "gunsight",              GUNSIGHT_X,        GUNSIGHT_Y,        16, 16 },
    { "explosion7",            EXPLOSION7_X,      EXPLOSION7_Y,      16, 16 },
    { "explosion8",            EXPLOSION8_X,      EXPLOSION8_Y,      16, 16 },
    { "explosion3",            EXPLOSION3_X,      EXPLOSION3_Y,      16, 16 },
    { "explosion4",            EXPLOSION4_X,      EXPLOSION4_Y,      16, 16 },

    /* ---- Row 5: Pillbox evil + tank selfboat start + explosions (y=80) ---- */
    { "pillbox_evil_00",       PILL_EVIL0_X,      PILL_EVIL0_Y,      16, 16 },
    { "pillbox_evil_01",       PILL_EVIL1_X,      PILL_EVIL1_Y,      16, 16 },
    { "pillbox_evil_02",       PILL_EVIL2_X,      PILL_EVIL2_Y,      16, 16 },
    { "pillbox_evil_03",       PILL_EVIL3_X,      PILL_EVIL3_Y,      16, 16 },
    { "pillbox_evil_04",       PILL_EVIL4_X,      PILL_EVIL4_Y,      16, 16 },
    { "pillbox_evil_05",       PILL_EVIL5_X,      PILL_EVIL5_Y,      16, 16 },
    { "pillbox_evil_06",       PILL_EVIL6_X,      PILL_EVIL6_Y,      16, 16 },
    { "pillbox_evil_07",       PILL_EVIL7_X,      PILL_EVIL7_Y,      16, 16 },
    { "pillbox_evil_08",       PILL_EVIL8_X,      PILL_EVIL8_Y,      16, 16 },
    { "pillbox_evil_09",       PILL_EVIL9_X,      PILL_EVIL9_Y,      16, 16 },
    { "pillbox_evil_10",       PILL_EVIL10_X,     PILL_EVIL10_Y,     16, 16 },
    { "pillbox_evil_11",       PILL_EVIL11_X,     PILL_EVIL11_Y,     16, 16 },
    { "pillbox_evil_12",       PILL_EVIL12_X,     PILL_EVIL12_Y,     16, 16 },
    { "pillbox_evil_13",       PILL_EVIL13_X,     PILL_EVIL13_Y,     16, 16 },
    { "pillbox_evil_14",       PILL_EVIL14_X,     PILL_EVIL14_Y,     16, 16 },
    { "pillbox_evil_15",       PILL_EVIL15_X,     PILL_EVIL15_Y,     16, 16 },
    { "tank_selfboat_00",      TANK_SELFBOAT_0_X, TANK_SELFBOAT_0_Y, 16, 16 },
    { "tank_selfboat_01",      TANK_SELFBOAT_1_X, TANK_SELFBOAT_1_Y, 16, 16 },
    { "tank_selfboat_02",      TANK_SELFBOAT_2_X, TANK_SELFBOAT_2_Y, 16, 16 },
    { "tank_selfboat_03",      TANK_SELFBOAT_3_X, TANK_SELFBOAT_3_Y, 16, 16 },
    { "tank_selfboat_04",      TANK_SELFBOAT_4_X, TANK_SELFBOAT_4_Y, 16, 16 },
    { "explosion5",            EXPLOSION5_X,      EXPLOSION5_Y,      16, 16 },
    { "explosion6",            EXPLOSION6_X,      EXPLOSION6_Y,      16, 16 },

    /* ---- Row 6: Tank selfboat cont + boats + mouse square (y=96) ---- */
    { "tank_selfboat_05",      TANK_SELFBOAT_5_X, TANK_SELFBOAT_5_Y, 16, 16 },
    { "tank_selfboat_06",      TANK_SELFBOAT_6_X, TANK_SELFBOAT_6_Y, 16, 16 },
    { "tank_selfboat_07",      TANK_SELFBOAT_7_X, TANK_SELFBOAT_7_Y, 16, 16 },
    { "tank_selfboat_08",      TANK_SELFBOAT_8_X, TANK_SELFBOAT_8_Y, 16, 16 },
    { "tank_selfboat_09",      TANK_SELFBOAT_9_X, TANK_SELFBOAT_9_Y, 16, 16 },
    { "tank_selfboat_10",      TANK_SELFBOAT_10_X, TANK_SELFBOAT_10_Y, 16, 16 },
    { "tank_selfboat_11",      TANK_SELFBOAT_11_X, TANK_SELFBOAT_11_Y, 16, 16 },
    { "tank_selfboat_12",      TANK_SELFBOAT_12_X, TANK_SELFBOAT_12_Y, 16, 16 },
    { "tank_selfboat_13",      TANK_SELFBOAT_13_X, TANK_SELFBOAT_13_Y, 16, 16 },
    { "tank_selfboat_14",      TANK_SELFBOAT_14_X, TANK_SELFBOAT_14_Y, 16, 16 },
    { "tank_selfboat_15",      TANK_SELFBOAT_15_X, TANK_SELFBOAT_15_Y, 16, 16 },
    { "boat1",                 BOAT1_X,           BOAT1_Y,           16, 16 },
    { "boat2",                 BOAT2_X,           BOAT2_Y,           16, 16 },
    { "boat3",                 BOAT3_X,           BOAT3_Y,           16, 16 },
    { "boat4",                 BOAT4_X,           BOAT4_Y,           16, 16 },
    { "boat5",                 BOAT5_X,           BOAT5_Y,           16, 16 },
    { "boat6",                 BOAT6_X,           BOAT6_Y,           16, 16 },
    { "boat7",                 BOAT7_X,           BOAT7_Y,           16, 16 },
    { "mouse_square",          MOUSE_SQUARE_X,    MOUSE_SQUARE_Y,    16, 16 },

    /* ---- Row 7: Pillbox good + LGM helicopter (y=112) ---- */
    { "pillbox_good_00",       PILL_GOOD0_X,      PILL_GOOD0_Y,      16, 16 },
    { "pillbox_good_01",       PILL_GOOD1_X,      PILL_GOOD1_Y,      16, 16 },
    { "pillbox_good_02",       PILL_GOOD2_X,      PILL_GOOD2_Y,      16, 16 },
    { "pillbox_good_03",       PILL_GOOD3_X,      PILL_GOOD3_Y,      16, 16 },
    { "pillbox_good_04",       PILL_GOOD4_X,      PILL_GOOD4_Y,      16, 16 },
    { "pillbox_good_05",       PILL_GOOD5_X,      PILL_GOOD5_Y,      16, 16 },
    { "pillbox_good_06",       PILL_GOOD6_X,      PILL_GOOD6_Y,      16, 16 },
    { "pillbox_good_07",       PILL_GOOD7_X,      PILL_GOOD7_Y,      16, 16 },
    { "pillbox_good_08",       PILL_GOOD8_X,      PILL_GOOD8_Y,      16, 16 },
    { "pillbox_good_09",       PILL_GOOD9_X,      PILL_GOOD9_Y,      16, 16 },
    { "pillbox_good_10",       PILL_GOOD10_X,     PILL_GOOD10_Y,     16, 16 },
    { "pillbox_good_11",       PILL_GOOD11_X,     PILL_GOOD11_Y,     16, 16 },
    { "pillbox_good_12",       PILL_GOOD12_X,     PILL_GOOD12_Y,     16, 16 },
    { "pillbox_good_13",       PILL_GOOD13_X,     PILL_GOOD13_Y,     16, 16 },
    { "pillbox_good_14",       PILL_GOOD14_X,     PILL_GOOD14_Y,     16, 16 },
    { "pillbox_good_15",       PILL_GOOD15_X,     PILL_GOOD15_Y,     16, 16 },
    { "lgm_helicopter",        LGM_HELICOPTER_X,  LGM_HELICOPTER_Y,  16, 16 },

    /* ---- Row 8: Tank goodboat end + tank evil (y=128) ---- */
    { "tank_goodboat_11",      TANK_GOODBOAT_11_X, TANK_GOODBOAT_11_Y, 16, 16 },
    { "tank_goodboat_12",      TANK_GOODBOAT_12_X, TANK_GOODBOAT_12_Y, 16, 16 },
    { "tank_goodboat_13",      TANK_GOODBOAT_13_X, TANK_GOODBOAT_13_Y, 16, 16 },
    { "tank_goodboat_14",      TANK_GOODBOAT_14_X, TANK_GOODBOAT_14_Y, 16, 16 },
    { "tank_goodboat_15",      TANK_GOODBOAT_15_X, TANK_GOODBOAT_15_Y, 16, 16 },
    { "tank_evil_00",          TANK_EVIL_0_X,     TANK_EVIL_0_Y,     16, 16 },
    { "tank_evil_01",          TANK_EVIL_1_X,     TANK_EVIL_1_Y,     16, 16 },
    { "tank_evil_02",          TANK_EVIL_2_X,     TANK_EVIL_2_Y,     16, 16 },
    { "tank_evil_03",          TANK_EVIL_3_X,     TANK_EVIL_3_Y,     16, 16 },
    { "tank_evil_04",          TANK_EVIL_4_X,     TANK_EVIL_4_Y,     16, 16 },
    { "tank_evil_05",          TANK_EVIL_5_X,     TANK_EVIL_5_Y,     16, 16 },
    { "tank_evil_06",          TANK_EVIL_6_X,     TANK_EVIL_6_Y,     16, 16 },
    { "tank_evil_07",          TANK_EVIL_7_X,     TANK_EVIL_7_Y,     16, 16 },
    { "tank_evil_08",          TANK_EVIL_8_X,     TANK_EVIL_8_Y,     16, 16 },
    { "tank_evil_09",          TANK_EVIL_9_X,     TANK_EVIL_9_Y,     16, 16 },
    { "tank_evil_10",          TANK_EVIL_10_X,    TANK_EVIL_10_Y,    16, 16 },
    { "tank_evil_11",          TANK_EVIL_11_X,    TANK_EVIL_11_Y,    16, 16 },

    /* ---- Row 9: Tank evilboat end (y=144) ---- */
    { "tank_evilboat_10",      TANK_EVILBOAT_10_X, TANK_EVILBOAT_10_Y, 16, 16 },
    { "tank_evilboat_11",      TANK_EVILBOAT_11_X, TANK_EVILBOAT_11_Y, 16, 16 },
    { "tank_evilboat_12",      TANK_EVILBOAT_12_X, TANK_EVILBOAT_12_Y, 16, 16 },
    { "tank_evilboat_13",      TANK_EVILBOAT_13_X, TANK_EVILBOAT_13_Y, 16, 16 },
    { "tank_evilboat_14",      TANK_EVILBOAT_14_X, TANK_EVILBOAT_14_Y, 16, 16 },
    { "tank_evilboat_15",      TANK_EVILBOAT_15_X, TANK_EVILBOAT_15_Y, 16, 16 },

    /* ---- Row 10: Static (y=160) ---- */
    { "static",                STATIC_X,          STATIC_Y,          16, 16 },

    /* ---- Tank good (irregular positions) ---- */
    { "tank_good_00",          TANK_GOOD_0_X,     TANK_GOOD_0_Y,     16, 16 },
    { "tank_good_01",          TANK_GOOD_1_X,     TANK_GOOD_1_Y,     16, 16 },
    { "tank_good_02",          TANK_GOOD_2_X,     TANK_GOOD_2_Y,     16, 16 },
    { "tank_good_03",          TANK_GOOD_3_X,     TANK_GOOD_3_Y,     16, 16 },
    { "tank_good_04",          TANK_GOOD_4_X,     TANK_GOOD_4_Y,     16, 16 },
    { "tank_good_05",          TANK_GOOD_5_X,     TANK_GOOD_5_Y,     16, 16 },
    { "tank_good_06",          TANK_GOOD_6_X,     TANK_GOOD_6_Y,     16, 16 },
    { "tank_good_07",          TANK_GOOD_7_X,     TANK_GOOD_7_Y,     16, 16 },
    { "tank_good_08",          TANK_GOOD_8_X,     TANK_GOOD_8_Y,     16, 16 },
    { "tank_good_09",          TANK_GOOD_9_X,     TANK_GOOD_9_Y,     16, 16 },
    { "tank_good_10",          TANK_GOOD_10_X,    TANK_GOOD_10_Y,    16, 16 },
    { "tank_good_11",          TANK_GOOD_11_X,    TANK_GOOD_11_Y,    16, 16 },
    { "tank_good_12",          TANK_GOOD_12_X,    TANK_GOOD_12_Y,    16, 16 },
    { "tank_good_13",          TANK_GOOD_13_X,    TANK_GOOD_13_Y,    16, 16 },
    { "tank_good_14",          TANK_GOOD_14_X,    TANK_GOOD_14_Y,    16, 16 },
    { "tank_good_15",          TANK_GOOD_15_X,    TANK_GOOD_15_Y,    16, 16 },

    /* ---- Tank goodboat (irregular positions) ---- */
    { "tank_goodboat_00",      TANK_GOODBOAT_0_X, TANK_GOODBOAT_0_Y, 16, 16 },
    { "tank_goodboat_01",      TANK_GOODBOAT_1_X, TANK_GOODBOAT_1_Y, 16, 16 },
    { "tank_goodboat_02",      TANK_GOODBOAT_2_X, TANK_GOODBOAT_2_Y, 16, 16 },
    { "tank_goodboat_03",      TANK_GOODBOAT_3_X, TANK_GOODBOAT_3_Y, 16, 16 },
    { "tank_goodboat_04",      TANK_GOODBOAT_4_X, TANK_GOODBOAT_4_Y, 16, 16 },
    { "tank_goodboat_05",      TANK_GOODBOAT_5_X, TANK_GOODBOAT_5_Y, 16, 16 },
    { "tank_goodboat_06",      TANK_GOODBOAT_6_X, TANK_GOODBOAT_6_Y, 16, 16 },
    { "tank_goodboat_07",      TANK_GOODBOAT_7_X, TANK_GOODBOAT_7_Y, 16, 16 },
    { "tank_goodboat_08",      TANK_GOODBOAT_8_X, TANK_GOODBOAT_8_Y, 16, 16 },
    { "tank_goodboat_09",      TANK_GOODBOAT_9_X, TANK_GOODBOAT_9_Y, 16, 16 },
    { "tank_goodboat_10",      TANK_GOODBOAT_10_X, TANK_GOODBOAT_10_Y, 16, 16 },

    /* ---- Tank evil 12-15 (irregular positions) ---- */
    { "tank_evil_12",          TANK_EVIL_12_X,    TANK_EVIL_12_Y,    16, 16 },
    { "tank_evil_13",          TANK_EVIL_13_X,    TANK_EVIL_13_Y,    16, 16 },
    { "tank_evil_14",          TANK_EVIL_14_X,    TANK_EVIL_14_Y,    16, 16 },
    { "tank_evil_15",          TANK_EVIL_15_X,    TANK_EVIL_15_Y,    16, 16 },

    /* ---- Tank evilboat 0-9 (irregular positions) ---- */
    { "tank_evilboat_00",      TANK_EVILBOAT_0_X, TANK_EVILBOAT_0_Y, 16, 16 },
    { "tank_evilboat_01",      TANK_EVILBOAT_1_X, TANK_EVILBOAT_1_Y, 16, 16 },
    { "tank_evilboat_02",      TANK_EVILBOAT_2_X, TANK_EVILBOAT_2_Y, 16, 16 },
    { "tank_evilboat_03",      TANK_EVILBOAT_3_X, TANK_EVILBOAT_3_Y, 16, 16 },
    { "tank_evilboat_04",      TANK_EVILBOAT_4_X, TANK_EVILBOAT_4_Y, 16, 16 },
    { "tank_evilboat_05",      TANK_EVILBOAT_5_X, TANK_EVILBOAT_5_Y, 16, 16 },
    { "tank_evilboat_06",      TANK_EVILBOAT_6_X, TANK_EVILBOAT_6_Y, 16, 16 },
    { "tank_evilboat_07",      TANK_EVILBOAT_7_X, TANK_EVILBOAT_7_Y, 16, 16 },
    { "tank_evilboat_08",      TANK_EVILBOAT_8_X, TANK_EVILBOAT_8_Y, 16, 16 },
    { "tank_evilboat_09",      TANK_EVILBOAT_9_X, TANK_EVILBOAT_9_Y, 16, 16 },

    /* ---- Tank transparent ---- */
    { "tank_transparent",      TANK_TRANSPARENT_X, TANK_TRANSPARENT_Y, 16, 16 },

    /* ---- Indents (non-standard sizes) ---- */
    { "indent_on",             INDENT_ON_X,       INDENT_ON_Y,       54, 54 },
    { "indent_off",            INDENT_OFF_X,      INDENT_OFF_Y,      54, 54 },
    { "indent_dot_on",         INDENT_DOT_ON_X,   INDENT_DOT_ON_Y,   6,  6  },
    { "indent_dot_off",        INDENT_DOT_OFF_X,  INDENT_DOT_OFF_Y,  6,  6  },

    /* ---- Forest/crater variants (y=144 area) ---- */
    { "base_evil",             BASE_EVIL_X,       BASE_EVIL_Y,       16, 16 },
    { "forest_single",         FOREST_SINGLE_X,   FOREST_SINGLE_Y,   16, 16 },
    { "forest_bottomright",    FOREST_BR_X,       FOREST_BR_Y,       16, 16 },
    { "forest_bottomleft",     FOREST_BL_X,       FOREST_BL_Y,       16, 16 },
    { "forest_aboveleft",      FOREST_AL_X,       FOREST_AL_Y,       16, 16 },
    { "forest_aboveright",     FOREST_AR_X,       FOREST_AR_Y,       16, 16 },
    { "forest_right",          FOREST_RIGHT_X,    FOREST_RIGHT_Y,    16, 16 },
    { "forest_left",           FOREST_LEFT_X,     FOREST_LEFT_Y,     16, 16 },
    { "forest_below",          FOREST_BELOW_X,    FOREST_BELOW_Y,    16, 16 },
    { "forest_above",          FOREST_ABOVE_X,    FOREST_ABOVE_Y,    16, 16 },

    /* ---- Crater variants (y=160 area) ----
     * crater_above/below/left/right/_single all live here on the
     * atlas but rely on the skin.bmp fallback in the builder; no
     * tilemap entry means no SVG/PNG override is searched for. */

    /* ---- Shells (irregular sizes, packed region) ---- */
    { "shell_00",              SHELL_0_X,         SHELL_0_Y,         SHELL_0_WIDTH,  SHELL_0_HEIGHT  },
    { "shell_01",              SHELL_1_X,         SHELL_1_Y,         SHELL_1_WIDTH,  SHELL_1_HEIGHT  },
    { "shell_02",              SHELL_2_X,         SHELL_2_Y,         SHELL_2_WIDTH,  SHELL_2_HEIGHT  },
    { "shell_03",              SHELL_3_X,         SHELL_3_Y,         SHELL_3_WIDTH,  SHELL_3_HEIGHT  },
    { "shell_04",              SHELL_4_X,         SHELL_4_Y,         SHELL_4_WIDTH,  SHELL_4_HEIGHT  },
    { "shell_05",              SHELL_5_X,         SHELL_5_Y,         SHELL_5_WIDTH,  SHELL_5_HEIGHT  },
    { "shell_06",              SHELL_6_X,         SHELL_6_Y,         SHELL_6_WIDTH,  SHELL_6_HEIGHT  },
    { "shell_07",              SHELL_7_X,         SHELL_7_Y,         SHELL_7_WIDTH,  SHELL_7_HEIGHT  },
    { "shell_08",              SHELL_8_X,         SHELL_8_Y,         SHELL_8_WIDTH,  SHELL_8_HEIGHT  },
    { "shell_09",              SHELL_9_X,         SHELL_9_Y,         SHELL_9_WIDTH,  SHELL_9_HEIGHT  },
    { "shell_10",              SHELL_10_X,        SHELL_10_Y,        SHELL_10_WIDTH, SHELL_10_HEIGHT },
    { "shell_11",              SHELL_11_X,        SHELL_11_Y,        SHELL_11_WIDTH, SHELL_11_HEIGHT },
    { "shell_12",              SHELL_12_X,        SHELL_12_Y,        SHELL_12_WIDTH, SHELL_12_HEIGHT },
    { "shell_13",              SHELL_13_X,        SHELL_13_Y,        SHELL_13_WIDTH, SHELL_13_HEIGHT },
    { "shell_14",              SHELL_14_X,        SHELL_14_Y,        SHELL_14_WIDTH, SHELL_14_HEIGHT },
    { "shell_15",              SHELL_15_X,        SHELL_15_Y,        SHELL_15_WIDTH, SHELL_15_HEIGHT },

    /* ---- LGM frames (3x4 each) ---- */
    { "lgm_frame0",            LGM0_X,            LGM0_Y,            LGM_WIDTH, LGM_HEIGHT },
    { "lgm_frame1",            LGM1_X,            LGM1_Y,            LGM_WIDTH, LGM_HEIGHT },
    { "lgm_frame2",            LGM2_X,            LGM2_Y,            LGM_WIDTH, LGM_HEIGHT },

    /* ---- Status items (12x12 each) ---- */
    { "status_dead",               STATUS_ITEM_DEAD_X,          STATUS_ITEM_DEAD_Y,          STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_base_neutral",       STATUS_BASE_NEUTRAL_X,       STATUS_BASE_NEUTRAL_Y,       STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_base_alliegood",     STATUS_BASE_ALLIEGOOD_X,     STATUS_BASE_ALLIEGOOD_Y,     STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_base_good",          STATUS_BASE_GOOD_X,          STATUS_BASE_GOOD_Y,          STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_base_evil",          STATUS_BASE_EVIL_X,          STATUS_BASE_EVIL_Y,          STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_pill_neutral",       STATUS_PILLBOX_NEUTRAL_X,    STATUS_PILLBOX_NEUTRAL_Y,    STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_pill_evil",          STATUS_PILLBOX_EVIL_X,       STATUS_PILLBOX_EVIL_Y,       STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_pill_tankgood",      STATUS_PILLBOX_TANKGOOD_X,   STATUS_PILLBOX_TANKGOOD_Y,   STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_pill_tankallie",     STATUS_PILLBOX_TANKALLIE_X,  STATUS_PILLBOX_TANKALLIE_Y,  STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },
    { "status_pill_tankevil",      STATUS_PILLBOX_TANKEVIL_X,   STATUS_PILLBOX_TANKEVIL_Y,   STATUS_ITEM_SIZE_X, STATUS_ITEM_SIZE_Y },

    /* Sentinel */
    { NULL, 0, 0, 0, 0 }
};

#define TILE_MAP_COUNT (sizeof(gTileMap) / sizeof(gTileMap[0]) - 1)

#endif /* TILEMAP_H */
