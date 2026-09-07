/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
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
#include "bases.h"
#include "global.h"
#include "pillbox.h"
#include "starts.h"
#include "types.h"

#define LENGTH_ID 8 /* Number of charecters in the "BMAPBOLO" id tag */
#define CURRENT_MAP_VERSION 1 /* The current Version is */

/* Number of bytes in structures */
#define SIZEOFBMAP_PILL_INFO 5
#define SIZEOFBAMP_BASE_INFO 6
#define SIZEOFBMAP_START_INFO 3
#define SIZEOFBMAP_RUN_HEADER 4

/* MAP_MINE_EDGE_* moved to global.h (used by map-render UIs that
 * don't pull bolo_map.h). */

/* Maximums */
/* Not required
#define MAX_PILLS 16
#define MAX_BASES  16

*/

/* For identical terrain this is the first map "code" */
#define MAP_CODE_IDENTICAL_START 8
/* Last length */
#define MAP_CODE_IDENTICAL_END 15
/* We want to skip two items for identical map squres */
#define MAP_CODE_IDENTICAL_SKIP 2

#define MAP_CODE_DIFFERENT_END 7

#define MAP_ARRAY_SIZE 256 /* maps are 256x256 units square */
#define MAP_ARRAY_LAST 255 /* Last item in a map is 255 or 0xFF */

/* All map files should begin with BMAPBOLO */
#define MAP_HEADER "BMAPBOLO" 

/* Must shift 4 bits to get a nibble */
#define MAP_SHIFT_SIZE 4 

/* length of six for map to be same */
#define MAP_RUN_SAME 6 
#define MAP_RUN_DIFF 8

/* Speeds of tanks over various terrains */
#define MAP_SPEED_TDEEPSEA 3 /* Tank in deep sea kills tank */
#define MAP_SPEED_TBUILDING 0
#define MAP_SPEED_TRIVER 3
#define MAP_SPEED_TSWAMP 3
#define MAP_SPEED_TCRATER 3
#define MAP_SPEED_TROAD 16
#define MAP_SPEED_TFOREST 6
#define MAP_SPEED_TRUBBLE 3
#define MAP_SPEED_TGRASS 12
#define MAP_SPEED_THALFBUILDING 0
#define MAP_SPEED_TBOAT 16 /* Was 3 - check ?? */
#define MAP_SPEED_TREFBASE 16 /* Refueling base */
#define MAP_SPEED_TPILLBOX 0 /* Pillbox */


/* Speeds of man over various terrains */
#define MAP_MANSPEED_TDEEPSEA 0 /* Tank in deep sea kills tank */
#define MAP_MANSPEED_TBUILDING 0
#define MAP_MANSPEED_TRIVER 0
#define MAP_MANSPEED_TSWAMP 4
#define MAP_MANSPEED_TCRATER 4
#define MAP_MANSPEED_TROAD 16
#define MAP_MANSPEED_TFOREST 8
#define MAP_MANSPEED_TRUBBLE 4
#define MAP_MANSPEED_TGRASS 16
#define MAP_MANSPEED_THALFBUILDING 0
#define MAP_MANSPEED_TBOAT 16 /* Was 3 - check ?? */
#define MAP_MANSPEED_TREFBASE 16 /* Refueling base */
#define MAP_MANSPEED_TPILLBOX 0 /* Pillbox */


/* Speeds of tanks turn
Given in bradians - 256 bradians in a circle */
#define MAP_TURN_TDEEPSEA 0.5 /* Tank in deep sea kills tank */
#define MAP_TURN_TBUILDING 0
#define MAP_TURN_TRIVER 0.25
#define MAP_TURN_TSWAMP 0.25 
#define MAP_TURN_TCRATER 0.25 
#define MAP_TURN_TROAD 1 
#define MAP_TURN_TFOREST 0.5 
#define MAP_TURN_TRUBBLE 0.25 
#define MAP_TURN_TGRASS 1 
#define MAP_TURN_THALFBUILDING 0
#define MAP_TURN_TBOAT 1
#define MAP_TURN_TREFBASE  1 /* Refueling base */
#define MAP_TURN_TPILLBOX 0 /* Pillbox */

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
	BYTE datalen;	/* length of the data for this run, INCLUDING this 4 byte header */
	MAP_Y y;		  /* y co-ordinate of this run. */
	BYTE  startx;	/* first square of the run */
	BYTE endx;		/* last square of run + 1 */
	BYTE data[0xFF]; /* actual length of data is always much less than 0xFF */
} bmapRun;


/* Linked-list emptiness helpers used by message and other queue
 * structures throughout the sim. */
#define IsEmpty(list) ((list) ==NULL)
#define NonEmpty(list) (!IsEmpty(list))

/* Prototypes */

/*********************************************************
*NAME:          mapCreate
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
void mapCreate(map *value);

/*********************************************************
*NAME:          mapDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Destroys the map data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void mapDestroy(map *value);

/* The BMAP record/run readers (mapReadPills / mapReadBases /
 * mapReadStarts / mapProcessRun / mapReadRuns) are now file-static
 * inside bolo_map.c. They read through an internal MapReader that
 * backs either a FILE* (mapRead) or a memory buffer
 * (mapReadFromMemory), so they no longer take a FILE* and are not
 * part of the public surface. The logviewer keeps its own lv_*
 * copies. */

/*********************************************************
*NAME:          mapRead
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Reads a map in.
* Returns if the operation was successful or not
*
*ARGUMENTS:
*  fileNam - Pointer to a string containing the file name
*  value   - Pointer to the map data structure
*  ss      - Pointer to the starts structure
*  bs      - Pointer to the bases structure
*  pb      - Pointer to the pillbox structure
*********************************************************/
bool mapRead(char *fileName, map *value, pillboxes *pb, bases *bs, starts *ss);

/*********************************************************
*NAME:          mapReadFromMemory
*AUTHOR:        John Morrison
*PURPOSE:
*  Like mapRead, but parses a .map file image held in
*  memory instead of reading from disk. No temp file is
*  written. Returns if the operation was successful.
*
*ARGUMENTS:
*  data    - Pointer to the .map file bytes
*  len     - Number of bytes available at data
*  value   - Pointer to the map data structure
*  pb      - Pointer to the pillbox structure
*  bs      - Pointer to the bases structure
*  ss      - Pointer to the starts structure
*********************************************************/
bool mapReadFromMemory(const BYTE *data, int len, map *value, pillboxes *pb, bases *bs, starts *ss);

/*********************************************************
*NAME:          mapGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 30/12/2008
*PURPOSE:
* Returns The value of a square in a map. Return 
* DEEP_SEA if value out of range
*
*ARGUMENTS:
*  value  - Pointer to the map data structure
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate 
*********************************************************/
BYTE mapGetPos(map *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          mapGetSpeed
*AUTHOR:        John Morrison
*CREATION DATE:  7/11/98
*LAST MODIFIED: 15/01/02
*PURPOSE:
* Returns the speed of the tank for a given map square
*
*ARGUMENTS:
*  value     - Pointer to the map data structure
*  pb        - Pointer to the pillboxes structure
*  bs        - Pointer to the bases structures
*  xValue    - The x co-ordinate
*  yValue    - The y co-ordinate 
*  onBoat    - Is the tank on a boat or not?
*  playerNum - Player Number of this player 
*********************************************************/
BYTE mapGetSpeed(struct GameSim *sim, map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue, bool onBoat, BYTE playerNum);

/*********************************************************
*NAME:          mapGetManSpeed
*AUTHOR:        John Morrison
*CREATION DATE:  7/11/98
*LAST MODIFIED: 15/01/02
*PURPOSE:
* Returns The speed of the tank for a given map square
*
*ARGUMENTS:
*  value     - Pointer to the map data structure
*  pb        - Pointer to the pillboxes structure
*  bs        - Pointer to the bases structures
*  xValue    - The x co-ordinate
*  yValue    - The y co-ordinate 
*  playerNum - Player Number of this player
*********************************************************/
BYTE mapGetManSpeed(struct GameSim *sim, map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue, BYTE playerNum);

/*********************************************************
*NAME:          mapGetTurnRate
*AUTHOR:        John Morrison
*CREATION DATE:  7/11/98
*LAST MODIFIED: 15/1/02
*PURPOSE:
* Returns The turn rate of the tank for a given 
* map square
*
*ARGUMENTS:
*  value  - Pointer to the map data structure
*  pb     - Pointer to the pillboxes structure
*  bs     - Pointer to the bases structures
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate 
*  onBoat - Is the tank on a boat or not?
*  playerNum - Player Number who is requesting the turn
*********************************************************/
TURNTYPE mapGetTurnRate(struct GameSim *sim, map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue, bool onBoat, BYTE playerNum);

/*********************************************************
*NAME:          mapIsPassable
*AUTHOR:        John Morrison
*CREATION DATE:  29/12/98
*LAST MODIFIED:  29/12/98
*PURPOSE:
* Returns whether the map square is passable or not
*
*ARGUMENTS:
*  value  - Pointer to the map data structure
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate 
*  onBoat - Is the item on a boat 
*********************************************************/
bool mapIsPassable(map *value, BYTE xValue, BYTE yValue, bool onBoat);

/*********************************************************
*NAME:          mapSetPos
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED:  31/7/00
*PURPOSE:
* Sets a position on the map
*
*ARGUMENTS:
*  value    - Pointer to the map data structure
*  xValue   - The x co-ordinate
*  yValue   - The y co-ordinate 
*  terrain  - The new terrain type
*  needSend - Used in network. True if we should
*             request the server make this change
*             False indicates the server should make it
* mineClear - Set to TRUE if we should just set the map
*             to the terrain. This is to remove the mines
*             from under bases on start up
*********************************************************/
void mapSetPos(struct GameSim *sim, map *value, BYTE xValue, BYTE yValue, BYTE terrain, bool needSend, bool mineClear);

/* Callback invoked when mapSetPos modifies terrain on the server.
 * Used by ServerSim to emit EVENT_MAP_CHANGE events. */
typedef void (*MapChangeCallback)(BYTE x, BYTE y, BYTE terrain);
void mapSetChangeCallback(MapChangeCallback cb);

/*********************************************************
*NAME:          mapIsLand
*AUTHOR:        John Morrison
*CREATION DATE: 6/1/99
*LAST MODIFIED: 6/1/99
*PURPOSE:
* Returns whether a map square is land or not 
*
*ARGUMENTS:
*  value   - Pointer to the map data structure
*  pb      - Pointer to the pillboxs structure
*  bs      - Pointer to the bases structure
*  xValue  - The x co-ordinate
*  yValue  - The y co-ordinate 
*********************************************************/
bool mapIsLand(map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          mapIsMine
*AUTHOR:        John Morrison
*CREATION DATE: 22/1/99
*LAST MODIFIED: 22/1/99
*PURPOSE:
* Returns whether a map square is mines or not
*
*ARGUMENTS:
*  value   - Pointer to the map data structure
*  xValue  - The x co-ordinate
*  yValue  - The y co-ordinate 
*********************************************************/
bool mapIsMine(map *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          mapWrite
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
*********************************************************/
bool mapWrite(char *fileName, map *value, pillboxes *pb, bases *bs, starts *ss);

/*********************************************************
*NAME:          mapWritePills
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
*********************************************************/
bool mapWritePills(FILE *fp, pillboxes *pb, BYTE total);

/*********************************************************
*NAME:          mapWriteBases
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
*********************************************************/
bool mapWriteBases(FILE *fp, bases *bs, BYTE total);

/*********************************************************
*NAME:          mapWriteBases
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
bool mapWriteStarts(FILE *fp, starts *ss, BYTE total);

/*********************************************************
*NAME:          mapWriteRuns
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
bool mapWriteRuns(FILE *fp, map *value);

/*********************************************************
*NAME:          mapPrepareRun
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
int32_t mapPrepareRun(map *value, bmapRun *run, BYTE *xPos, BYTE *yPos);


/*********************************************************
*NAME:          mapLoadCompressedMap
*AUTHOR:        John Morrison
*CREATION DATE: 1/5/99
*LAST MODIFIED: 1/5/99
*PURPOSE:
*  Reads a map in via a compressed map structure. Returns 
*  if the operation was successful or not
*
*ARGUMENTS:
*  value    - Pointer to the map data structure
*  ss       - Pointer to the starts structure
*  bs       - Pointer to the bases structure
*  pb       - Pointer to the pillbox structure
*  input    - Pointer to the data buffer
*  inputLen - Size of the buffer
*********************************************************/
bool mapLoadCompressedMap(map *value, pillboxes *pb, bases *bs, starts *ss, BYTE *input, int inputLen);

/*********************************************************
*NAME:          mapSaveCompressedMap
*AUTHOR:        John Morrison
*CREATION DATE: 1/5/99
*LAST MODIFIED: 1/5/99
*PURPOSE:
*  Saves a map to a compressed map structure. Returns 
*  compressed data length
*
*ARGUMENTS:
*  value     - Pointer to the map data structure
*  ss        - Pointer to the starts structure
*  bs        - Pointer to the bases structure
*  pb        - Pointer to the pillbox structure
*  output    - Pointer to the data buffer
*  outputCap - Size of the output buffer in bytes. Nothing is written past
*              it; a map that does not fit returns 0 instead.
*********************************************************/
int mapSaveCompressedMap(map *value, pillboxes *pb, bases *bs, starts *ss, BYTE *output, int outputCap);

/*********************************************************
*NAME:          mapCenter
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Centers the map file and everything on it.
*
*ARGUMENTS:
*  value   - Pointer to the map data structure
*  ss      - Pointer to the starts structure
*  bs      - Pointer to the bases structure
*  pb      - Pointer to the pillbox structure
*********************************************************/
void mapCenter(map *value, pillboxes *pb, bases *bs, starts *ss);

/*********************************************************
*NAME:          mapCalcChecksum
*PURPOSE:
*  Computes a CRC-16 checksum over the map terrain array.
*
*ARGUMENTS:
*  value - Pointer to the map structure
*  bs    - Bases list, so tiles under bases are folded to ROAD (may be NULL)
*  pb    - Pillbox list, so tiles under pills are folded to ROAD (may be NULL)
*********************************************************/
uint16_t mapCalcChecksum(map *value, bases *bs, pillboxes *pb);

#endif /* MAP_H */
