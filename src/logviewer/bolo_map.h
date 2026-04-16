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


#ifndef MAP_H
#define MAP_H

/* Defines */
#include "global.h"
#include "pillbox.h"
#include "starts.h"
#include "bases.h"

#define LENGTH_ID 8 /* Number of charecters in the "BMAPBOLO" id tag */
#define CURRENT_MAP_VERSION 1 /* The current Version is */

/* Number of bytes in structures */
#define SIZEOFBMAP_PILL_INFO 5
#define SIZEOFBAMP_BASE_INFO 6
#define SIZEOFBMAP_START_INFO 3
#define SIZEOFBMAP_RUN_HEADER 4


/* For identical terrain this is the first map "code" */
#define MAP_CODE_IDENTICAL_START 8
/* Last length */
#define MAP_CODE_IDENTICAL_END 15
/* We want to skip two items for identical map squres */
#define MAP_CODE_IDENTICAL_SKIP 2

#define MAP_CODE_DIFFERENT_END 7


/* Maximums */
#define MAX_PILLS 16
#define MAX_BASES  16


#define MAP_ARRAY_SIZE 256 /* maps are 256x256 units square */
#define MAP_ARRAY_LAST 255 /* Last item in a map is 255 or 0xFF */

/* All map files should begin with BMAPBOLO */
#define MAP_HEADER "BMAPBOLO" 

/* Must shift 4 bits to get a nibble */
#define MAP_SHIFT_SIZE 4 

/* length of six for map to be same */
#define MAP_RUN_SAME 6 
#define MAP_RUN_DIFF 8

/* Map Edges for mines */
#define MAP_MINE_EDGE_LEFT  20
#define MAP_MINE_EDGE_RIGHT 236
#define MAP_MINE_EDGE_TOP  20
#define MAP_MINE_EDGE_BOTTOM 236


/* Type definitions */

/* Defines the state of map reading */
typedef enum {
  highLen,  /* The next high nibble is a length */
  lowLen,   /* The next low nibble is a length */
  highDiff, /* The next high nibble is a item and the length indicates differnt items */
  lowDiff,  /* The next low nibble is a item and the length indicates differnt items */
  highSame, /* The next high nibble is a item and the same items for the length */
  lowSame   /* The next low nibble is a item and the same items for the length */
} mapRunState;


typedef struct {
  BYTE datalen;	/* length of the data for this run INCLUDING this 4 byte header */
  MAP_Y y;		/* y co-ordinate of this run. */
  MAP_X startx;	/* first square of the run */
  MAP_X endx;	/* last square of run + 1 (ie first deep sea square after run) */
} bmapRunHeader;

typedef struct {
	BYTE datalen;	/* length of the data for this run */
	      				/* INCLUDING this 4 byte header */
	MAP_Y y;		  /* y co-ordinate of this run. */
	BYTE  startx;	/* first square of the run */
	BYTE endx;		/* last square of run + 1 */
	BYTE data[0xFF]; /* actual length of data is always much less than 0xFF */
} bmapRun;

typedef struct mapObj *map;



/* Prototypes */

/*********************************************************
*NAME:          lv_mapCreate
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Creates and initilises the map structure. Sets all 
*  map squares to be deep ocean
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void lv_mapCreate(map *value);

/*********************************************************
*NAME:          lv_mapDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Destroys the map data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void lv_mapDestroy(map *value);

/*********************************************************
*NAME:          lv_mapGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
* Returns The value of a square in a map. Return 
* DEEP_SEA if value out of range
*
*ARGUMENTS:
*  value  - Pointer to the map data structure
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate 
*********************************************************/
BYTE lv_mapGetPos(map *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_mapSetPos
*AUTHOR:        John Morrison
*CREATION DATE: 11/11/00
*LAST MODIFIED: 11/11/00
*PURPOSE:
* Returns The value of a square in a map. Return 
* DEEP_SEA if value out of range
*
*ARGUMENTS:
*  value   - Pointer to the map data structure
*  xValue  - The x co-ordinate
*  yValue  - The y co-ordinate 
*  terrain - Terrain to set to
*********************************************************/
void lv_mapSetPos(map *value, BYTE xValue, BYTE yValue, BYTE terrain);

/*********************************************************
*NAME:          lv_mapIsMine
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 13/3/99
*PURPOSE:
* Returns whether a map square is mines or not
*
*ARGUMENTS:
*  value   - Pointer to the map data structure
*  xValue  - The x co-ordinate
*  yValue  - The y co-ordinate 
*********************************************************/
bool lv_mapIsMine(map *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          lv_mapReadRuns
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Reads the map runs into the map data structure
*  Returns if the operation was successful or not
*
*ARGUMENTS:
*  value - Pointer to the map data structure
*********************************************************/
bool lv_mapReadRuns(map *value);

/*********************************************************
*NAME:          lv_mapProcessRun
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Process a single map run and puts values into map
*  data structure.
*  Returns whether operation was successful or not
*
*ARGUMENTS:
*  value  - Pointer to the map data structure
*  elems  - Number of elements in the run
*  yValue - The y Map co-ordinate
*  startX - The start x co-ordinate
*  endX   - The end x co-ordinate
*********************************************************/
bool lv_mapProcessRun(map *value,BYTE elems, MAP_Y yValue, BYTE startX, BYTE endX);

/*********************************************************
*NAME:          lv_mapWrite
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
* Writes a map to the filename given
* Returns if the operation was successful or not
*
*ARGUMENTS:
*  fileName - Pointer to a string containing the file name
*  value    - Pointer to the map data structure
*  ss       - Pointer to the starts structure
*  bs       - Pointer to the bases structure
*  pb       - Pointer to the pillbox structure
*  saveOwnerships - Save ownerships or not
*********************************************************/
bool lv_mapWrite(char *fileName, map *value, pillboxes *pb, bases *bs, starts *ss, bool saveOwnerships);

/*********************************************************
*NAME:          lv_mapWritePills
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
* Writes the pillbox locations out
* Returns if the operation was successful or not
*
*ARGUMENTS:
*  fileNam - Pointer to a string containing the file name
*  pb      - Pointer to the pillbox structure
*  total   - Total number of pills to write
*  saveOwnerships - Save ownerships or not
*********************************************************/
bool lv_mapWritePills(FILE *fp, pillboxes *pb, BYTE total, bool saveOwnerships);

/*********************************************************
*NAME:          lv_mapWriteStarts
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
* Writes the bases locations out
* Returns if the operation was successful or not
*
*ARGUMENTS:
*  fp    - File pointer
*  bs    - Pointer to the pillbox structure
*  total - Total number of bases to write
*  saveOwnerships - Save ownerships or not
*********************************************************/
bool lv_mapWriteBases(FILE *fp, bases *bs, BYTE total, bool saveOwnerships);

/*********************************************************
*NAME:          lv_mapWriteBases
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
* Writes the starts locations out
* Returns if the operation was successful or not
*
*ARGUMENTS:
*  fp    - File pointer
*  ss    - Pointer to the starts structure
*  total - Total number of starts to write
*********************************************************/
bool lv_mapWriteStarts(FILE *fp, starts *ss, BYTE total);

/*********************************************************
*NAME:          lv_mapWriteRuns
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
* Writes out the series of map runs
* Returns if the operation was successful or not
*
*ARGUMENTS:
*  fp    - File pointer
*  value - Pointer to the map structure
*********************************************************/
bool lv_mapWriteRuns(FILE *fp, map *value);

/*********************************************************
*NAME:          lv_mapPrepareRun
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/99
*LAST MODIFIED: 9/2/99
*PURPOSE:
* Prepares a map run to write out. Returns the length of
* the run
*
*ARGUMENTS:
*  value - Pointer to the map structure
*  run   - Pointer to the map run structure
*  xPos  - Pointer to our current X position
*  yPos  - Pointer to our current Y position
*********************************************************/
int32_t lv_mapPrepareRun(map *value, bmapRun *run, BYTE *xPos, BYTE *yPos);

#endif /* MAP_H */
