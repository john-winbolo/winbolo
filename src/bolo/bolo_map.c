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


/*********************************************************
*Name:          bolo_map
*Filename:      bolo_map.c
*Author:        John Morrison
*Creation Date: 21/10/98
*Last Modified: 15/01/02
*Purpose:
*  Provides operations for read and writing of
*  Bolo map files
*********************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "global.h"
#include "bolo_map.h"
#include "crc.h"
#include "pillbox.h"
#include "starts.h"
#include "bases.h"
#include "game_sim.h"
#include "client_enums.h"  /* updateType */
#include "viewport_types.h"  /* screen */
#include "players.h"
#include "messages.h"
#include "mines.h"
#include "sounddist.h"
#include "floodfill.h"
#include "log.h"
#include "util.h"
#include "../common/wb_log.h"
#include "screenbrainmap.h"
#include "bolo_map_validate.h"

/* Map change callback — set by ServerSim during tick */
static THREAD_LOCAL MapChangeCallback mapChangeCb = NULL;

void mapSetChangeCallback(MapChangeCallback cb) {
    mapChangeCb = cb;
}

int lzwdecoding(unsigned char *src, unsigned char *dest, int len, int destCap);
int lzwencoding(unsigned char *src, unsigned char *dest, int len, int destCap);

/*********************************************************
*NAME:          mapCreate
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Creates and initilises the map structure. Sets all 
*  map squares to be deep sea
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void mapCreate(map *value) {
  int count;  /* Looping variable */
  int count2; /* Looping variable */

  New(*value);
  for (count=0;count<MAP_ARRAY_SIZE ;count++) {
    for (count2=0;count2<MAP_ARRAY_SIZE;count2++) {
      ((*value)->mapItem[count][count2]) = DEEP_SEA;
    }
  }
}

/*********************************************************
*NAME:          mapDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 23/2/99
*PURPOSE:
*  Destroys the map data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the map file
*********************************************************/
void mapDestroy(map *value) {
  if (*value != NULL) {
    Dispose(*value);
  }
  *value = NULL;
}

/*********************************************************
*NAME:          MapReader
*PURPOSE:
*  Backs the BMAP file parser with either a stdio FILE*
*  (mapRead, reading an on-disk .map) or an in-memory
*  byte buffer (mapReadFromMemory, e.g. a WBN map held
*  in RAM after download). The mapRead* helpers below
*  take a MapReader* so a single parser serves both and
*  the file path stays byte-for-byte identical.
*********************************************************/
typedef struct {
  FILE       *fp;        /* non-NULL: file-backed */
  const BYTE *buf;       /* non-NULL: memory-backed */
  size_t      len;       /* memory: total bytes available */
  size_t      pos;       /* memory: read cursor */
  int         pushback;  /* memory: one-byte ungetc slot, -1 if empty */
  bool        eof;       /* memory: a read ran past the buffer end */
} MapReader;

static void mrInitFile(MapReader *r, FILE *fp) {
  r->fp = fp; r->buf = NULL; r->len = 0; r->pos = 0;
  r->pushback = -1; r->eof = false;
}

static void mrInitMem(MapReader *r, const BYTE *buf, size_t len) {
  r->fp = NULL; r->buf = buf; r->len = len; r->pos = 0;
  r->pushback = -1; r->eof = false;
}

/* Mirrors fgetc: returns the next byte (0..255) or EOF. */
static int mrGetc(MapReader *r) {
  if (r->fp != NULL) return fgetc(r->fp);
  if (r->pushback >= 0) { int c = r->pushback; r->pushback = -1; return c; }
  if (r->pos >= r->len) { r->eof = true; return EOF; }
  return r->buf[r->pos++];
}

/* Mirrors ungetc: pushes one byte back so the next read returns it.
 * A successful push clears the end-of-input state, as stdio does. */
static int mrUngetc(MapReader *r, int c) {
  if (r->fp != NULL) return ungetc(c, r->fp);
  if (c == EOF || r->pushback >= 0) return EOF;
  r->pushback = c & 0xff;
  r->eof = false;
  return c & 0xff;
}

/* Mirrors fread: returns the number of whole `size`-byte elements
 * read. A pending ungetc byte is consumed first, exactly as stdio. */
static size_t mrRead(MapReader *r, void *ptr, size_t size, size_t n) {
  if (r->fp != NULL) return fread(ptr, size, n, r->fp);
  size_t want = size * n;
  if (want == 0) return 0;
  BYTE  *out = (BYTE *)ptr;
  size_t got = 0;
  if (r->pushback >= 0) { out[got++] = (BYTE)r->pushback; r->pushback = -1; }
  size_t avail = (r->pos < r->len) ? (r->len - r->pos) : 0;
  size_t need  = want - got;
  size_t take  = (need < avail) ? need : avail;
  if (take > 0) {
    memcpy(out + got, r->buf + r->pos, take);
    r->pos += take;
    got += take;
  }
  if (got < want) r->eof = true;
  return (size > 0) ? (got / size) : 0;
}

/* Mirrors fgets: reads up to size-1 bytes, stopping after a newline,
 * NUL-terminates, and returns dest (or NULL if nothing was read). */
static char *mrGets(MapReader *r, char *dest, int size) {
  if (r->fp != NULL) return fgets(dest, size, r->fp);
  if (size <= 0) return NULL;
  int i = 0;
  while (i < size - 1) {
    int c = mrGetc(r);
    if (c == EOF) break;
    dest[i++] = (char)c;
    if (c == '\n') break;
  }
  if (i == 0) return NULL;
  dest[i] = '\0';
  return dest;
}

/* Mirrors feof / ferror. Memory reads never hard-error. */
static bool mrEof(MapReader *r) {
  if (r->fp != NULL) return feof(r->fp) != 0;
  return r->eof;
}
static bool mrError(MapReader *r) {
  if (r->fp != NULL) return ferror(r->fp) != 0;
  return false;
}

/*********************************************************
*NAME:          mapReadPills
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Reads the pill information from a map file
*  into the pill structure. Returns if the 
*  operation was successful or not
*
*  Stores each record as it is read. Clamping armour and the
*  attack interval needs the sim's rules and this path has no
*  sim — the map editor, the preview and the tile-test tool
*  load maps through here. mapClampToRules caps the list once
*  a sim owns it; pillsValidate covers the count and the
*  owners, which are the file's own business.
*
*ARGUMENTS:
*  fp    - Pointer to the file being read from
*  value - Pointer to the pillbox structure
*********************************************************/
static bool mapReadPills(MapReader *fp, pillboxes *value) {
  int total;        /* Total number of pills to be read in */
  int count;        /* Looping variable */
  size_t ret;          /* Number of bytes read */
  bool returnValue; /* Value to return */
  pillbox readInto; /* The pillbox being read into */

  count = 1;
  total = pillsGetNumPills(value);
  returnValue = TRUE;
  readInto.inTank = FALSE;

  while (count <= total && returnValue == TRUE && !mrEof(fp)) {
    ret = mrRead(fp, &readInto, SIZEOFBMAP_PILL_INFO, 1);
    if (ret != 1) {
      returnValue = FALSE;
    } else {
      readInto.justSeen = FALSE;
      if (readInto.owner > (MAX_TANKS - 1) && readInto.owner != NEUTRAL) {
        readInto.owner = NEUTRAL;
      }
      if (count <= (int)(*value)->numPills) {
        pillbox *slot = &((*value)->item[count - 1]);
        /* The five fields the record carries, plus the two the loop sets.
           reload is left where it is, as the setter this replaced left it:
           it is the slot's, not the file's. coolDown starts at zero and
           pillsClampToRules arms it if the pill loaded angry, which it can
           only work out once a sim says what angry is. */
        slot->x        = readInto.x;
        slot->y        = readInto.y;
        slot->owner    = readInto.owner;
        slot->armour   = readInto.armour;
        slot->speed    = readInto.speed;
        slot->inTank   = readInto.inTank;
        slot->justSeen = readInto.justSeen;
        slot->coolDown = 0;
        logAddEvent(log_PillSetOwner, (BYTE) (count - 1), readInto.owner, TRUE, 0, 0, NULL);
        logAddEvent(log_PillSetHealth, (BYTE) (count - 1), readInto.armour, 0, 0, 0, NULL);
        logAddEvent(log_PillSetInTank, utilPutNibble((BYTE) (count - 1), FALSE), 0, 0, 0, 0, NULL);
        logAddEvent(log_PillSetPlace, (BYTE) (count - 1), readInto.x, readInto.y, 0, 0, NULL);
      }
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          mapReadBases
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Reads the base information from a map file
*  into the base structure. Returns if the 
*  operation was successful or not
*
*ARGUMENTS:
*  fp    - Pointer to the file being read from
*  value - Pointer to the pillbox structure
*********************************************************/
static bool mapReadBases(MapReader *fp, bases *value) {
  int count;        /* Looping variable */
  int total;        /* Total number of bases to read */
  size_t ret;          /* Number of bytes read */
  bool returnValue; /* Value to return */
  base readInto;    /* The base structure to read into */

  count = 1;
  total = basesGetNumBases(value);
  returnValue = TRUE;

  while (count <= total && returnValue == TRUE && !mrEof(fp)) {
    ret = mrRead(fp, &readInto, SIZEOFBAMP_BASE_INFO, 1);
    if (ret != 1) {
      returnValue = FALSE;
    } else {
      readInto.baseTime = 0;     /* Time between stock updates */
      basesSetBase(value,&readInto,(BYTE) count);
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          mapReadStarts
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Reads the player start information from a map file
*  into the player starts structure. Returns if the 
*  operation was successful or not
*
*ARGUMENTS:
*  fp    - Pointer to the file being read from
*  value - Pointer to the pillbox structure
*********************************************************/
static bool mapReadStarts(MapReader *fp, starts *value) {
  int total;        /* Total number of entries to read */
  int count;        /* Looping variable */
  size_t ret;          /* Number of bytes read */
  bool returnValue; /* Value to return */
  start readInto;   /* Item to read into */

  count = 1;
  total = startsGetNumStarts(value);
  returnValue = TRUE;
  while (count <= total && returnValue == TRUE && !mrEof(fp)) {
    ret = mrRead(fp, &readInto, SIZEOFBMAP_START_INFO, 1);
    if (ret != 1) {
      returnValue = FALSE;
    } else {
      startsSetStart(value, &readInto, (BYTE) count);
    }
    count++;
  }
  return returnValue;
}

/*********************************************************
*NAME:          mapProcessRun
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Process a single map run and puts values into map
*  data structure.
*  Returns whether operation was successful or not
*
*ARGUMENTS:
*  fp     - Pointer to the file being read from
*  value  - Pointer to the map data structure
*  elems  - Number of elements in the run
*  yValue - The y Map co-ordinate
*  startX - The start x co-ordinate
*  endX   - The end x co-ordinate
*********************************************************/
static bool mapProcessRun(MapReader *fp,map *value,BYTE elems, MAP_Y yValue, BYTE startX, BYTE endX) {
  bool returnValue;  /* Value to return */
  bool needRead;     /* State variable - Do we need to read the next byte */
  mapRunState state; /* Current run state */  
  BYTE item;         /* Item being worked on */
  BYTE highNibble;   /* The low nibble of a byte */
  BYTE lowNibble;    /* The low nibble of a byte */
  BYTE len;          /* Length of the row */
  BYTE mapPos;       /* Possision on map */
  int count;         /* Looping variables */
  int count2;
  
  count = 0;
  returnValue = TRUE;
  state = highLen;
  mapPos = startX;
  len = 0;

  item = (BYTE) mrGetc(fp);
  while (count < elems && !mrError(fp)) {
    needRead = FALSE;
    highNibble = lowNibble = item;
    /* Extract Nibbles */
    highNibble >>= MAP_SHIFT_SIZE;
    lowNibble <<= MAP_SHIFT_SIZE;
    lowNibble >>= MAP_SHIFT_SIZE;

    while (needRead == FALSE) {
      switch (state) {
      
      case highLen:
        len = highNibble;
        if (len < MAP_RUN_DIFF) {
          state = lowDiff;
          len++;
        } else {
          state = lowSame;
        }
        break;
      
      case lowLen:
        len = lowNibble;
        if (len < MAP_RUN_DIFF) {
          state = highDiff;
          len++;
        } else {
          state = highSame;
        }
        needRead = TRUE;
        break;
      
      case lowDiff:
        ((*value)->mapItem[mapPos][yValue]) = lowNibble;
        mapPos++;
        len--;
        if (len == 0) {
          state = highLen;
        } else {
          state = highDiff;
        }
        needRead = TRUE;
        break;

      case highDiff:
        ((*value)->mapItem[mapPos][yValue]) = highNibble;
        mapPos++;
        len--;
        if (len == 0) {
          state = lowLen;
        } else {
          state = lowDiff;
        }
        break;

      case lowSame:
        count2 = 0;
        while (count2 < (len-MAP_RUN_SAME) && (mapPos+count2) < MAP_ARRAY_SIZE) {
          ((*value)->mapItem[mapPos+count2][yValue]) = lowNibble;
          count2++;
        }
        mapPos = (BYTE) (mapPos + count2);
        state = highLen;
        needRead = TRUE;
        break;

      case highSame:
        count2 = 0;
        while (count2 < (len-MAP_RUN_SAME) && (mapPos+count2) < MAP_ARRAY_SIZE) {
          ((*value)->mapItem[mapPos+count2][yValue]) = highNibble;
          count2++;
        }
        mapPos = (BYTE) (mapPos + count2);
        state = lowLen;
        break;
      }
    }
    count++;
    item = (BYTE) mrGetc(fp);
  }
  count2 = mrUngetc(fp, item);
  /* Check all read correctly */
  if (count != elems || mapPos != endX || count2 == EOF)  { /*  || mapPos != endX */ 
    returnValue = FALSE;
  }
  return returnValue;
}


/*********************************************************
*NAME:          mapReadRuns
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED: 21/10/98
*PURPOSE:
*  Reads the map runs into the map data structure
*  Returns if the operation was successful or not
*
*ARGUMENTS:
*  fp    - Pointer to the file being read from
*  value - Pointer to the map data structure
*********************************************************/
static bool mapReadRuns(MapReader *fp, map *value) {
  bmapRunHeader runHead; /* The header of each run */
  size_t bytesRead;         /* The number of bytes read from the header */
  bool returnValue;      /* Value to return */
  bool done;             /* Is all the runs read */
  bool ret;              /* Function return Value */

  returnValue = TRUE;
  done = FALSE;

  bytesRead = mrRead(fp, &runHead, SIZEOFBMAP_RUN_HEADER, 1);

  while (!mrEof(fp) && done == FALSE) {
    if (bytesRead != 1) {
    /* Something bad happened reading */
      done = TRUE;
      returnValue = FALSE;
    } else if (runHead.datalen == 4  &&  runHead.y == MAP_ARRAY_LAST && runHead.startx == MAP_ARRAY_LAST && runHead.endx == MAP_ARRAY_LAST) {
    /* Finished reading */
      done = TRUE;
      returnValue = TRUE;
    } else if (runHead.datalen < SIZEOFBMAP_RUN_HEADER || runHead.startx > runHead.endx) {
      /* Invalid run header — reject malformed map */
      done = TRUE;
      returnValue = FALSE;
    } else {
      ret = mapProcessRun(fp,value,(BYTE) ((runHead.datalen)- SIZEOFBMAP_RUN_HEADER),runHead.y, runHead.startx, runHead.endx);
      if (ret == FALSE) {
        /* Function return failed */
        done = TRUE;
        returnValue = FALSE;
      }
    }
    bytesRead = mrRead(fp, &runHead, SIZEOFBMAP_RUN_HEADER, 1);
  }

  return returnValue;
}

/*********************************************************
*NAME:          mapRead
*AUTHOR:        John Morrison
*CREATION DATE: 21/10/98
*LAST MODIFIED:  13/6/00
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
/* Shared BMAP parse core. Reads from `r` (file- or memory-backed)
 * into the supplied structures and applies the same post-load
 * fix-ups (centre, base/pill terrain) as the historical mapRead. */
static bool mapReadStream(MapReader *r, map *value, pillboxes *pb, bases *bs, starts *ss) {
  bool returnValue;   /* Value to return */
  char id[LENGTH_ID+1]; /* The map ID Should read "BMAPBOLO" */
  BYTE mapVersion;    /* Version of the map file */
  BYTE current;       /* Item being read */
  char *str;          /* return value of mrGets */

  returnValue = TRUE;
  str = mrGets(r, id, (LENGTH_ID+1));
  if (str == NULL || strcmp(id,MAP_HEADER) != 0) {
    returnValue = FALSE;
  }
  if (returnValue == TRUE) {
    mapVersion = (BYTE) mrGetc(r);
    if (mapVersion != CURRENT_MAP_VERSION) {
      returnValue = FALSE;
    }
  }
  if (returnValue == TRUE) {
    current = (BYTE) mrGetc(r);
    pillsSetNumPills(pb,current);
    current = (BYTE) mrGetc(r);
    basesSetNumBases(bs,current);
    current = (BYTE) mrGetc(r);
    startsSetNumStarts(ss,current);
    if (pillsGetNumPills(pb) > MAX_PILLS || basesGetNumBases(bs) > MAX_BASES || startsGetNumStarts(ss) > MAX_STARTS) {
      returnValue = FALSE;
    }
  }
  if (returnValue == TRUE) {
    returnValue = mapReadPills(r,pb);
  }
  if (returnValue == TRUE) {
    returnValue = mapReadBases(r,bs);
  }
  if (returnValue == TRUE) {
    returnValue = mapReadStarts(r,ss);
  }
  if (returnValue == TRUE) {
    returnValue = mapReadRuns(r,value);
  }

  if (returnValue == TRUE) {
    mapCenter(value, pb, bs, ss);
  }

  /* Ensure terrain under bases is ROAD */
  if (returnValue == TRUE) {
    BYTE numBases = basesGetNumBases(bs);
    BYTE bi;
    for (bi = 0; bi < numBases; bi++) {
      if (basesIsActive(bs, (BYTE)(bi + 1)) == FALSE) continue;
      (*value)->mapItem[(*bs)->item[bi].x][(*bs)->item[bi].y] = ROAD;
    }
  }

  /* Fix terrain under pillboxes — replace impassable terrain with ROAD */
  if (returnValue == TRUE) {
    BYTE numPills = pillsGetNumPills(pb);
    BYTE pi;
    for (pi = 0; pi < numPills; pi++) {
      BYTE t;
      if (pillsIsActive(pb, (BYTE)(pi + 1)) == FALSE) continue;
      t = (*value)->mapItem[(*pb)->item[pi].x][(*pb)->item[pi].y];
      if (t == RIVER || t == DEEP_SEA || t == BUILDING || t == HALFBUILDING) {
        (*value)->mapItem[(*pb)->item[pi].x][(*pb)->item[pi].y] = ROAD;
      }
    }
  }

  return returnValue;
}

bool mapRead(char *fileName, map *value, pillboxes *pb, bases *bs, starts *ss) {
  FILE *fp;           /* File pointer */
  MapReader r;        /* File-backed reader */
  bool returnValue;   /* Value to return */

  fp = fopen(fileName,"rb");
  if (!fp) {
    return FALSE;
  }
  mrInitFile(&r, fp);
  returnValue = mapReadStream(&r, value, pb, bs, ss);
  fclose(fp);
  return returnValue;
}

/*********************************************************
*NAME:          mapReadFromMemory
*AUTHOR:        John Morrison
*PURPOSE:
*  Identical to mapRead but parses an on-disk .map file
*  image held in memory (e.g. a WinBolo.net map fetched
*  over HTTP) rather than reading from disk. No temp file
*  is created. Returns whether the operation succeeded.
*
*ARGUMENTS:
*  data    - Pointer to the .map file bytes
*  len     - Number of bytes available at data
*  value   - Pointer to the map data structure
*  pb      - Pointer to the pillbox structure
*  bs      - Pointer to the bases structure
*  ss      - Pointer to the starts structure
*********************************************************/
bool mapReadFromMemory(const BYTE *data, int len, map *value, pillboxes *pb, bases *bs, starts *ss) {
  MapReader r;        /* Memory-backed reader */

  if (data == NULL || len <= 0) {
    return FALSE;
  }
  mrInitMem(&r, data, (size_t)len);
  return mapReadStream(&r, value, pb, bs, ss);
}


/*********************************************************
*NAME:          mapGetPos
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 30/12/2008
*PURPOSE:
* Returns The value of a square in a map. Return 
* RIVER if value out of range
*
*ARGUMENTS:
*  value  - Pointer to the map data structure
*  xValue - The x co-ordinate
*  yValue - The y co-ordinate 
*********************************************************/
BYTE mapGetPos(map *value, BYTE xValue, BYTE yValue) {
  BYTE returnValue = DEEP_SEA; /* Value to return */

  if (xValue > MAP_MINE_EDGE_LEFT && xValue < MAP_MINE_EDGE_RIGHT && yValue > MAP_MINE_EDGE_TOP && yValue < MAP_MINE_EDGE_BOTTOM) {
    returnValue = (*value)->mapItem[xValue][yValue];
  }

  return returnValue;
}

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
BYTE mapGetSpeed(GameSim *sim, map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue, bool onBoat, BYTE playerNum) {
  BYTE returnValue; /* Value to return */
  BYTE terrain;     /* The current Terrain */
  bool done;        /* Are we done ? */

  returnValue = (BYTE) sim->rules.speed_deep_sea;
  done = FALSE;
  if ((pillsExistPos(pb,xValue,yValue)) == TRUE) {
    /* Check for PB */
    if (pillsDeadPos(pb, xValue, yValue) == FALSE) {
      /* A live pillbox is impassable because tankBuildingCollision tests the
         terrain, not because its speed is zero, so it keeps its constant. */
      returnValue = MAP_SPEED_TPILLBOX;
      done = TRUE;
    }
  } else if ((basesExistPos(bs,xValue,yValue)) == TRUE) {
    /* Check for owned base */
    if (basesCantDrive(sim, xValue, yValue, playerNum) == FALSE) {
      returnValue = (BYTE) sim->rules.speed_refuel_base;
    } else {
      returnValue = 0;
    }
	done = TRUE;
  }
  if (done == FALSE) {
    terrain = (*value)->mapItem[xValue][yValue];
    if (terrain >= MINE_START && terrain <= MINE_END) {
      terrain = terrain - MINE_SUBTRACT;
    }
    /* On boat: use boat speed for all terrain (handles LeavingBoat on land) */
    if (onBoat == TRUE) {
      returnValue = (BYTE) sim->rules.speed_boat;
    } else
    switch (terrain) {
    case DEEP_SEA:
      returnValue = (BYTE) sim->rules.speed_deep_sea;
      break;
    case BUILDING:
      /* Building and half-building keep their constants for the same reason
         a live pillbox does: the collision test is what stops a tank. */
      returnValue = MAP_SPEED_TBUILDING;
      break;
    case RIVER:
      returnValue = (BYTE) sim->rules.speed_river;
      break;
    case SWAMP:
      returnValue = (BYTE) sim->rules.speed_swamp;
      break;
    case CRATER:
      returnValue = (BYTE) sim->rules.speed_crater;
      break;
    case ROAD:
      returnValue = (BYTE) sim->rules.speed_road;
      break;
    case FOREST:
      returnValue = (BYTE) sim->rules.speed_forest;
      break;
    case RUBBLE:
      returnValue = (BYTE) sim->rules.speed_rubble;
      break;
    case GRASS:
      returnValue = (BYTE) sim->rules.speed_grass;
      break;
    case HALFBUILDING:
      returnValue = MAP_SPEED_THALFBUILDING;
      break;
    case BOAT:
      returnValue = (BYTE) sim->rules.speed_boat;
      break;
    default:
      /* Fall through */
      break;
    }
  }
  return returnValue;
}

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
BYTE mapGetManSpeed(GameSim *sim, map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue, BYTE playerNum) {
  BYTE returnValue; /* Value to return */
  BYTE terrain;     /* The current Terrain */
  bool done;        /* Are we done ? */

  returnValue = MAP_MANSPEED_TDEEPSEA;
  done = FALSE;
  if ((pillsExistPos(pb,xValue,yValue)) == TRUE) {
    /* Check for PB */
    if (pillsDeadPos(pb, xValue, yValue) == FALSE) {
      returnValue = MAP_MANSPEED_TPILLBOX;
      done = TRUE;
    }
  } else if ((basesExistPos(bs,xValue,yValue)) == TRUE) {
    /* Check for owned base */
    if (basesCantDrive(sim, xValue, yValue, playerNum) == FALSE) {
      returnValue = MAP_MANSPEED_TREFBASE;
    } else {
      returnValue = 0;
    }
    done = TRUE;
  } 
  if (done == FALSE) {
    terrain = (*value)->mapItem[xValue][yValue];
    if (terrain >= MINE_START && terrain <= MINE_END) {
      terrain = terrain - MINE_SUBTRACT;
    }
    switch (terrain) {
    case DEEP_SEA:
      returnValue = MAP_MANSPEED_TDEEPSEA;
      break;
    case BUILDING:
      returnValue = MAP_MANSPEED_TBUILDING;
      break;
    case RIVER:
      returnValue = MAP_MANSPEED_TRIVER;
      break;
    case SWAMP:
      returnValue = MAP_MANSPEED_TSWAMP;
      break;
    case CRATER:
      returnValue = MAP_MANSPEED_TCRATER;
      break;
    case ROAD:
      returnValue = MAP_MANSPEED_TROAD;
      break;
    case FOREST:
      returnValue = MAP_MANSPEED_TFOREST;
      break;
    case RUBBLE:
      returnValue = MAP_MANSPEED_TRUBBLE;
      break;
    case GRASS:
      returnValue = MAP_MANSPEED_TGRASS;
      break;
    case HALFBUILDING:
      returnValue = MAP_MANSPEED_THALFBUILDING;
      break;
    case BOAT:
      returnValue = MAP_MANSPEED_TBOAT;
      break;
    }
  }
  return returnValue;
}

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
TURNTYPE mapGetTurnRate(GameSim *sim, map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue, bool onBoat, BYTE playerNum) {
  TURNTYPE returnValue; /* Value to return */
  BYTE terrain;         /* The current Terrain */
  bool done;            /* Are we done ? */

  returnValue = (TURNTYPE) sim->rules.turn_deep_sea;
  done = FALSE;
  if ((pillsExistPos(pb,xValue,yValue)) == TRUE) {
    /* Check for PB */
    if ((pillsDeadPos(pb,xValue, yValue)) == FALSE) {
      /* No rule, for the same reason the speed above has none. */
      returnValue = MAP_TURN_TPILLBOX;
      done = TRUE;
    }
  } else if ((basesExistPos(bs,xValue,yValue)) == TRUE) {
    /* Check for owned base */
    if (basesCantDrive(sim, xValue, yValue, playerNum) == FALSE) {
      returnValue = (TURNTYPE) sim->rules.turn_refuel_base;
    } else {
      returnValue = 0;
    }
    done = TRUE;
  }
  if (done == FALSE) {
    terrain = (*value)->mapItem[xValue][yValue];
    if (terrain >= MINE_START && terrain <= MINE_END) {
      terrain = terrain - MINE_SUBTRACT;
    }
    switch (terrain) {
    case DEEP_SEA:
      returnValue = (TURNTYPE) sim->rules.turn_deep_sea;
      if (onBoat == TRUE) {
        returnValue = (TURNTYPE) sim->rules.turn_boat;
      }
      break;
    case BUILDING:
      returnValue = MAP_TURN_TBUILDING;
      break;
    case RIVER:
      returnValue = (TURNTYPE) sim->rules.turn_river;
      if (onBoat == TRUE) {
        returnValue = (TURNTYPE) sim->rules.turn_boat;
      }
      break;
    case SWAMP:
      returnValue = (TURNTYPE) sim->rules.turn_swamp;
      break;
    case CRATER:
      returnValue = (TURNTYPE) sim->rules.turn_crater;
      break;
    case ROAD:
      returnValue = (TURNTYPE) sim->rules.turn_road;
      break;
    case FOREST:
      returnValue = (TURNTYPE) sim->rules.turn_forest;
      break;
    case RUBBLE:
      returnValue = (TURNTYPE) sim->rules.turn_rubble;
      break;
    case GRASS:
      returnValue = (TURNTYPE) sim->rules.turn_grass;
      break;
    case HALFBUILDING:
      returnValue = MAP_TURN_THALFBUILDING;
      break;
    case BOAT:
      returnValue = (TURNTYPE) sim->rules.turn_boat;
      break;
    }
  }
  return returnValue;
}

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
bool mapIsPassable(map *value, BYTE xValue, BYTE yValue, bool onBoat) {
  bool returnValue; /* Value to return */
  BYTE pos;         /* The current position */

  returnValue = FALSE;

  pos = (*value)->mapItem[xValue][yValue];
  if (pos >= MINE_START && pos <= MINE_END) {
    pos -= MINE_SUBTRACT;
  }
  switch (pos) {
  case DEEP_SEA:
    returnValue = TRUE;
    break;
  case BUILDING:
    returnValue = FALSE;
    break;
  case RIVER:
    returnValue = TRUE;
    break;
  case SWAMP:
    if (onBoat == FALSE) {
      returnValue = TRUE;
    }
    break;
  case CRATER:
    returnValue = TRUE;
    break;
  case ROAD:
    if (onBoat == FALSE) {
      returnValue = TRUE;
    }
    break;
  case FOREST:
    returnValue = FALSE;
    break;
  case RUBBLE:
    if (onBoat == FALSE) {
      returnValue = TRUE;
    }
    break;
  case GRASS:
    if (onBoat == FALSE) {
      returnValue = TRUE;
    }
    break;
  case HALFBUILDING:
    returnValue = FALSE;
    break;
  case BOAT:
    returnValue = FALSE;
    break;
  }
  return returnValue;
}

/*********************************************************
*NAME:          mapSetPos
*AUTHOR:        John Morrison
*CREATION DATE: 30/12/98
*LAST MODIFIED: 05/05/01
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
void mapSetPos(GameSim *sim, map *value, BYTE xValue, BYTE yValue, BYTE terrain, bool needSend, bool mineClear) {
  (void)needSend;
  if (sim->isServer == TRUE || mineClear == TRUE) {
    /* Server-authoritative: directly set terrain.  This covers both
     * single-player (netSingle) and the server sim. */
      (*value)->mapItem[xValue][yValue] = terrain;
      if (mapChangeCb != NULL && sim->isServer == TRUE) {
        mapChangeCb(xValue, yValue, terrain);
      }
      screenBrainMapSetPos(sim->brainMap, xValue, yValue, terrain, minesExistPos(&sim->mns, &sim->mp, xValue, yValue));
  }
  /* Client-side path: terrain mutations are server-authoritative under
   * the ServerSim model — the client receives the change via
   * EVENT_MAP_CHANGE rather than queueing a request, so there is
   * nothing to do here. */
  logAddEvent(log_MapChange, xValue, yValue, terrain, 0, 0, NULL);
}

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
bool mapIsLand(map *value, pillboxes *pb, bases *bs, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  
  returnValue = TRUE;
  
  if ((((*value)->mapItem[xValue][yValue]) == RIVER) || (*value)->mapItem[xValue][yValue] == DEEP_SEA) {
    if ((basesExistPos(bs, xValue, yValue) == FALSE)) { /* (pillsExistPos(pb, xValue, yValue) == FALSE) &&  */
      returnValue = FALSE;
    }
  }
  return returnValue;
}

/*********************************************************
*NAME:          mapIsMine
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
bool mapIsMine(map *value, BYTE xValue, BYTE yValue) {
  bool returnValue; /* Value to return */
  
  returnValue = FALSE;
  
  if (xValue <= MAP_MINE_EDGE_LEFT || xValue >= MAP_MINE_EDGE_RIGHT || yValue <= MAP_MINE_EDGE_TOP || yValue >= MAP_MINE_EDGE_BOTTOM) {
    returnValue = TRUE;
  } else if ((*value)->mapItem[xValue][yValue] >= MINE_START && (*value)->mapItem[xValue][yValue] <= MINE_END) {
    returnValue = TRUE;
  }
  /* In server-authoritative architecture, mine state is set directly
   * on the map by the server.  No network queue check needed. */
  return returnValue;
}

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
bool mapWrite(char *fileName, map *value, pillboxes *pb, bases *bs, starts *ss) {
  FILE *fp;           /* File pointer */
  bool returnValue;   /* Value to return */
  int ret;            /* Function return value */
  BYTE numPills;      /* Number of pills on the map */
  BYTE numBases;      /* Number of bases on the map */
  BYTE numStarts;     /* Number of starts on the map */

  returnValue = TRUE;
  /* The file holds what is on the map. A slot a removal has emptied is sim
     state, not map data, so the header counts the live items and the writers
     below skip the rest; a map saved mid-scenario reloads without them. */
  numPills = pillsGetNumActive(pb);
  numBases = basesGetNumActive(bs);
  numStarts = startsGetNumActive(ss);

  fp = fopen(fileName,"wb");
  if (fp == NULL) {
    returnValue = FALSE;
  }
  /* Write header */
  if (returnValue == TRUE && fp) {
    ret = fputs(MAP_HEADER, fp);
    if (ret == EOF) {
      returnValue = FALSE;
    }
  }
  
  /* Write map version */
  if (returnValue == TRUE && fp) {
    ret = fputc(CURRENT_MAP_VERSION, fp);
    if (ret == EOF) {
      returnValue = FALSE;
    }
  }

  /* Put number of pills */
  if (returnValue == TRUE && fp) {
    ret = fputc(numPills, fp);
    if (ret == EOF) {
      returnValue = FALSE;
    }
  }

  /* Put number of bases */
  if (returnValue == TRUE && fp) {
    ret = fputc(numBases, fp);
    if (ret == EOF) {
      returnValue = FALSE;
    }
  }

  /* Put number of starts */
  if (returnValue == TRUE && fp) {
    ret = fputc(numStarts, fp);
    if (ret == EOF) {
      returnValue = FALSE;
    }
  }

  /* Write pill locations */
  if (returnValue == TRUE && fp) {
    returnValue = mapWritePills(fp, pb, numPills);
  }
  /* Write bases locations */
  if (returnValue == TRUE && fp) {
    returnValue = mapWriteBases(fp, bs, numBases);
  }
  /* Write starts locations */
  if (returnValue == TRUE && fp) {
    returnValue = mapWriteStarts(fp, ss, numStarts);
  }

  if (returnValue == TRUE && fp) {
    returnValue = mapWriteRuns(fp,value);
  }
  if (fp) {
    fclose(fp);
  }
  return returnValue;
   
}

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
*  fp    - File pointer
*  pb    - Pointer to the pillbox structure
*  total - Total number of pills to write
*********************************************************/
bool mapWritePills(FILE *fp, pillboxes *pb, BYTE total) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping variable */
  pillbox item;     /* Pillbox information */
  int ret;          /* Function return Value */
  
  returnValue = TRUE;
  count = 1;
  (void)total;
  /* Every slot, writing the live ones: the header already says how many. */
  while (count <= pillsGetNumPills(pb) && returnValue == TRUE) {
    if (pillsIsActive(pb, count) == FALSE) {
      count++;
      continue;
    }
    pillsGetPill(pb, &item, count);
    /* Write each pill out */
    ret = fputc(item.x, fp);
    if (ret != item.x) {
      returnValue = FALSE;
    }
    if (returnValue == TRUE) {
      ret = fputc(item.y, fp);
      if (ret != item.y) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.owner, fp);
      if (ret != item.owner) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.armour, fp);
      if (ret != item.armour) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.speed, fp);
      if (ret != item.speed) {
        returnValue = FALSE;
      }
    }
    count++;
  }

  return returnValue;
}

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
bool mapWriteBases(FILE *fp, bases *bs, BYTE total) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping variable */
  base item;        /* The base item being saved */
  int ret;          /* Function return */

  returnValue = TRUE;
  count = 1;
  (void)total;
  while (count <= basesGetNumBases(bs) && returnValue == TRUE) {
    if (basesIsActive(bs, count) == FALSE) {
      count++;
      continue;
    }
    basesGetBase(bs, &item, count);
    /* Write each base out */
    ret = fputc(item.x, fp);
    if (ret != item.x) {
      returnValue = FALSE;
    }
    if (returnValue == TRUE) {
      ret = fputc(item.y, fp);
      if (ret != item.y) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.owner, fp);
      if (ret != item.owner) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.armour, fp);
      if (ret != item.armour) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.shells, fp);
      if (ret != item.shells) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.mines, fp);
      if (ret != item.mines) {
        returnValue = FALSE;
      }
    }
    count++;
  }

  return returnValue;
}

/*********************************************************
*NAME:          mapWriteStarts
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
bool mapWriteStarts(FILE *fp, starts *ss, BYTE total) {
  bool returnValue; /* Value to return */
  BYTE count;       /* Looping variable */
  start item;       /* Item being saved */
  int ret;          /* Function return */

  returnValue = TRUE;
  count = 1;
  (void)total;
  while (count <= startsGetNumStarts(ss) && returnValue == TRUE) {
    if (startsIsActive(ss, count) == FALSE) {
      count++;
      continue;
    }
    startsGetStartStruct(ss, &item, count);
    /* Write each start out */
    ret = fputc(item.x, fp);
    if (ret != item.x) {
      returnValue = FALSE;
    }
    if (returnValue == TRUE) {
      ret = fputc(item.y, fp);
      if (ret != item.y) {
        returnValue = FALSE;
      }
    }
    if (returnValue == TRUE) {
      ret = fputc(item.dir, fp);
      if (ret != item.dir) {
        returnValue = FALSE;
      }
    }
    count++;
  } 

  return returnValue;
}


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
bool mapWriteRuns(FILE *fp, map *value) {
  bool returnValue;         /* Value to return */
  bmapRun run;
  BYTE xPos;                /* Current position on the map */
  BYTE yPos;
  int32_t len;              /* Length of the run to write */
  size_t ret;                  /* Function return */

  returnValue = TRUE;
  xPos = 0;
  yPos = 0;
  while (yPos < 0xFF && returnValue == TRUE) {
    /* Process runs */
    len = mapPrepareRun(value, &run, &xPos, &yPos);
    /* Write the run out */
    ret = fwrite(&run, (size_t) len, 1, fp);
    if (ret !=  1) {
      returnValue = FALSE;
    }
  }

  return returnValue;
}

#define put_nibble(X) (!nibble_flag ? (nibble_flag = TRUE,  *nibble_data = (X)<<4) : (nibble_flag = FALSE, *nibble_data++ |= (X) & 0xF))

/*********************************************************
*NAME:          mapPrepareRun
*AUTHOR:        John Morrison
*CREATION DATE:  9/2/99
*LAST MODIFIED: 10/2/99
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
int32_t mapPrepareRun(map *value, bmapRun *run, BYTE *xPos, BYTE *yPos) {
	BYTE terrain;     /* Terrain under current Position */
	BYTE code;        /* Map code (ie identical/differnt etc) */
  BYTE x;           /* Temp variables to hold xPos and yPos */
	BYTE y;
	bool nibble_flag; /* High/Low nibble filter */
	BYTE *nibble_data; /* Holds run->data for nibble calculations */
  
  nibble_flag = FALSE;
  x = *xPos;
  y = *yPos;
  nibble_data = run->data;
	
	/* Search for non-DEEPSEA terrain */
  while (mapGetPos(value, x, y) == DEEP_SEA) {
    if (x < 0xFF) {
      x++; 
    } else if (y < MAP_ARRAY_LAST) { 
      x = 0; 
      y++; 
    } else {
      break;
    }
  }
  run->startx = x;
	if (y < MAP_ARRAY_LAST) {
		terrain = mapGetPos(value, x, y);
		while(terrain != DEEP_SEA) {
			if (terrain == mapGetPos(value, (BYTE) (x+1), y)){
			        /* Two squares are the same */
				code = MAP_CODE_IDENTICAL_START;
				x += MAP_CODE_IDENTICAL_SKIP;			/* skip over the two squares we have found */
				while (code < MAP_CODE_IDENTICAL_END && mapGetPos(value, x, y) == terrain) {
				          code++;
				          x++;
			        }
				put_nibble(code);
				put_nibble(terrain);
			}	else {
				BYTE code = 0;	/* code 0 means 1 individual square */
				MAP_X ds = x++;	/* record where the difference run starts */
				while (code < MAP_CODE_DIFFERENT_END  && mapGetPos(value, x, y) != DEEP_SEA && mapGetPos(value, x, y) != mapGetPos(value, (BYTE) (x+1), y)) { 
          code++; 
          x++;
        }
				put_nibble(code);
        while (ds<x) {
          put_nibble(mapGetPos(value, ds++, y)); 
        }
      }
      terrain = mapGetPos(value, x, y);
		}
    if (nibble_flag == TRUE) {
      put_nibble(0);	/* round it up to whole number of bytes */
    }
  }
  

  *xPos = run->endx = x;
	*yPos = run->y    = y;
	return (run->datalen = (BYTE) (nibble_data - (BYTE*)run));
}

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
int mapSaveCompressedMap(map *value, pillboxes *pb, bases *bs, starts *ss, BYTE *output, int outputCap) {
  int returnValue; /* Value to return */
  int headerLen;   /* Fixed-size part written before the terrain */
  int mapLen;      /* Encoded terrain length, or -1 if it did not fit */
  BYTE *ptr;       /* Data pointer    */
  BYTE *ptr2;

  returnValue = 0;

  /* Refuse before the first memcpy rather than after: the three struct copies
   * below are fixed size and write regardless of how small output is. */
  headerLen = SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS;
  if (output == NULL || outputCap < headerLen) {
    return 0;
  }

  /* Bases — raw struct copy to match basesSetBaseCompressData on load side */
  ptr = output;
  memcpy(ptr, &(**bs), SIZEOF_BASES);
  returnValue += SIZEOF_BASES;
  ptr += SIZEOF_BASES;

  /* Pillboxes — raw struct copy to match pillsSetPillCompressData on load side */
  memcpy(ptr, &(**pb), SIZEOF_PILLS);
  returnValue += SIZEOF_PILLS;
  ptr += SIZEOF_PILLS;

  /* Starts — raw struct copy to match startsSetStartCompressData on load side */
  memcpy(ptr, &(**ss), SIZEOF_STARTS);
  returnValue += SIZEOF_STARTS;
  ptr += SIZEOF_STARTS;

  /* Map. An incompressible map encodes larger than its input, so the encoder
   * is given what is left of the buffer and reports rather than overruns. */
  ptr2 = (BYTE *) (*value)->mapItem;
  mapLen = lzwencoding(ptr2, ptr, sizeof((*value)->mapItem),
                       outputCap - headerLen);
  if (mapLen < 0) {
    return 0;
  }
  returnValue += mapLen;
  return returnValue;
}


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
bool mapLoadCompressedMap(map *value, pillboxes *pb, bases *bs, starts *ss, BYTE *input, int inputLen) {
  bool returnValue; /* Value to return */
  BYTE *ptr;       /* Data pointer    */
  int mapSize;
  BYTE *ptr2;

  returnValue = TRUE;

  /* Each of the four world structures arrives as a pointer to its handle, so
   * either level can be NULL when the game is torn down under a caller that is
   * still holding it. Nothing below checks: the three compress setters and the
   * (*value)->mapItem reads all dereference straight away. Refuse instead, so
   * the caller gets its FALSE rather than a crash inside the loader. */
  if (value == NULL || *value == NULL ||
      pb == NULL || *pb == NULL ||
      bs == NULL || *bs == NULL ||
      ss == NULL || *ss == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_MAP, "mapLoadCompressedMap: null world handle");
    return FALSE;
  }

  /* Reject input too short to hold the fixed header before any struct read:
   * basesSetBaseCompressData/pillsSetPillCompressData/startsSetStartCompressData
   * each memcpy their full SIZEOF_* below regardless of inputLen, so a truncated
   * or malformed blob would over-read past the buffer. Every legitimate caller
   * passes a full compressed map; only short/garbage input is rejected here. */
  if (input == NULL ||
      inputLen < (int)(SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS)) {
    return FALSE;
  }

  /* Bases */
  ptr = input;
  basesSetBaseCompressData(bs, ptr, SIZEOF_BASES);
  inputLen -= SIZEOF_BASES;
  ptr += SIZEOF_BASES;

  /* Pillboxes */
  pillsSetPillCompressData(pb, ptr, SIZEOF_PILLS);
  inputLen -= SIZEOF_PILLS;
  ptr += SIZEOF_PILLS;

  /* Starts */
  startsSetStartCompressData(ss, ptr, SIZEOF_STARTS);
  inputLen -= SIZEOF_STARTS;
  ptr += SIZEOF_STARTS;

  /* The three setters above memcpy the wire structs wholesale, so none of the
   * per-field clamps that basesSetBase/pillsSetPill/startsSetStart apply on
   * the file-load path have run. Everything downstream — the terrain fixups
   * below included — reads these values, and a map arrives from whatever
   * server the player joined. Clamp here, once, so the rest of the codebase
   * can trust the fields rather than each consumer having to re-check.
   *
   * What a pill or a base may hold is not settled here: that is the sim's to
   * say, and this loader has none — the map editor and the preview call it
   * too. A caller that owns a sim calls mapClampToRules afterwards. */
  basesValidate(bs);
  pillsValidate(pb);
  startsValidate(ss);

  /* Map */
  ptr2 = (BYTE *) (*value)->mapItem;
  mapSize = lzwdecoding(ptr, ptr2, inputLen, (int)sizeof((*value)->mapItem));
  if (mapSize != (int)sizeof((*value)->mapItem)) {
    returnValue = FALSE;
  }

  /* Ensure terrain under bases is ROAD */
  if (returnValue == TRUE) {
    BYTE numBases = basesGetNumBases(bs);
    BYTE bi;
    for (bi = 0; bi < numBases; bi++) {
      if (basesIsActive(bs, (BYTE)(bi + 1)) == FALSE) continue;
      (*value)->mapItem[(*bs)->item[bi].x][(*bs)->item[bi].y] = ROAD;
    }
  }

  /* Fix terrain under pillboxes — replace impassable terrain with ROAD */
  if (returnValue == TRUE) {
    BYTE numPills = pillsGetNumPills(pb);
    BYTE pi;
    for (pi = 0; pi < numPills; pi++) {
      BYTE t;
      if (pillsIsActive(pb, (BYTE)(pi + 1)) == FALSE) continue;
      t = (*value)->mapItem[(*pb)->item[pi].x][(*pb)->item[pi].y];
      if (t == RIVER || t == DEEP_SEA || t == BUILDING || t == HALFBUILDING) {
        (*value)->mapItem[(*pb)->item[pi].x][(*pb)->item[pi].y] = ROAD;
      }
    }
  }

  return returnValue;
}

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
void mapCenter(map *value, pillboxes *pb, bases *bs, starts *ss) {
  int bestTop;    /* Our best gesses for centering */
  int bestBottom;
  int bestLeft;
  int bestRight;
  int guessTop;    /* Our best gesses for centering */
  int guessBottom;
  int guessLeft;
  int guessRight;
  int addX;        /* X and Y add amounts */
  int addY;
  int count1;     /* Looping variable */
  int count2;    /* Looping variable */
  map map2;       /* The new map to create */

  bestLeft = MAP_ARRAY_SIZE;
  bestRight = -1;
  bestTop = MAP_ARRAY_SIZE;
  bestBottom = -1;

  /* Pills */
  pillsGetMaxs(pb, &guessLeft, &guessRight, &guessTop, &guessBottom);
  if (guessLeft < bestLeft) {
    bestLeft = guessLeft;
  }
  if (guessRight < bestRight) {
    bestRight = guessRight;
  }
  if (guessTop < bestTop) {
    bestTop = guessTop;
  }
  if (guessBottom < bestBottom) {
    bestBottom = guessBottom;
  }
  /* Bases */
  basesGetMaxs(bs, &guessLeft, &guessRight, &guessTop, &guessBottom);
  if (guessLeft < bestLeft) {
    bestLeft = guessLeft;
  }
  if (guessRight < bestRight) {
    bestRight = guessRight;
  }
  if (guessTop < bestTop) {
    bestTop = guessTop;
  }
  if (guessBottom < bestBottom) {
    bestBottom = guessBottom;
  }
  /* Starts */
  startsGetMaxs(ss, &guessLeft, &guessRight, &guessTop, &guessBottom);
  if (guessLeft < bestLeft) {
    bestLeft = guessLeft;
  }
  if (guessRight < bestRight) {
    bestRight = guessRight;
  }
  if (guessTop < bestTop) {
    bestTop = guessTop;
  }
  if (guessBottom < bestBottom) {
    bestBottom = guessBottom;
  }
  for (count1=0; count1 <= MAP_ARRAY_LAST; count1++) {
    for (count2=0; count2 <= MAP_ARRAY_LAST; count2++) {
      if ((*value)->mapItem[count1][count2] != DEEP_SEA) {
        /* Center test */
        if (count1 < bestLeft) {
          bestLeft = count1;
        }
        if (count1 > bestRight) {
          bestRight = count1;
        }
        if (count2 < bestTop) {
          bestTop = count2;
        }
        if (count2 > bestBottom) {
          bestBottom = count2;
        }
      }
    }
  }

  /* Do the recentering */
  if (bestTop <= bestBottom && bestLeft <= bestRight) {
    addX = (255/2) - ((bestLeft + bestRight) /2) - 1;
    addY = (255/2) - ((bestTop + bestBottom) /2) - 1;
    if (addX != 0 && addY != 0) {
      /* It needs centering */
      mapCreate(&map2);
      for (count1=bestLeft; count1 <= (BYTE) bestRight; count1++) {
        for (count2=bestTop; count2 <= (BYTE) bestBottom; count2++) {
          map2->mapItem[(BYTE) (count1+addX)][(BYTE) (count2+addY)] = (*value)->mapItem[count1][count2];
        }
      }
      pillsMoveAll(pb, addX, addY);
      basesMoveAll(bs, addX, addY);
      startsMoveAll(ss, addX, addY);
      mapDestroy(value);
      *value = map2;
    }
  }
}

uint16_t mapCalcChecksum(map *value, bases *bs, pillboxes *pb) {
  const BYTE *src = (const BYTE *)(*value)->mapItem;
  size_t n = (size_t)MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
  BYTE *masked;
  uint16_t crc;
  size_t i;

  /* Mine state is stored in mapItem as terrain values in [MINE_START,
   * MINE_END] but is synced separately (the mns overlay + EVENT_MINE_VISIBLE),
   * so under hidden mines the client and server legitimately hold different
   * mine bits. CRCing the raw mapItem therefore mismatches by a fixed amount
   * forever, which drives an endless resync loop. Strip each mined tile back
   * to its base terrain (value - MINE_SUBTRACT) before CRCing so the checksum
   * covers only terrain both ends agree on. The buffer is per-call heap: this
   * runs on both the sim and recv threads, so a static buffer would race and a
   * 64 KB stack array is too large. */
  masked = (BYTE *)malloc(n);
  if (masked == NULL) {
    /* Degraded but safe: the raw (mine-inclusive) CRC rather than a crash. */
    return (uint16_t)CRCCalc((BYTE *)(*value)->mapItem, (int)n);
  }
  for (i = 0; i < n; i++) {
    BYTE t = src[i];
    masked[i] = (t >= MINE_START && t <= MINE_END) ? (BYTE)(t - MINE_SUBTRACT) : t;
  }

  /* Terrain under bases and pillboxes is not authoritative in a resync blob:
   * mapLoadCompressedMap forces every base tile to ROAD and every pill tile on
   * impassable terrain (RIVER/DEEP_SEA/BUILDING/HALFBUILDING) to ROAD after
   * decode. The live map can legitimately hold CRATER or RIVER under a base —
   * a tank exploding on the tile craters it, and the flood can then turn that
   * crater into RIVER. CRCing the raw live terrain there would never match any
   * round-tripped copy, driving an endless resync loop. Fold those tiles to
   * ROAD here so the checksum covers only terrain both ends can agree on,
   * exactly mirroring the decode fixup. mapItem is [x][y] row-major, so index
   * the flat buffer as x*MAP_ARRAY_SIZE + y. bs/pb may be NULL (skip). */
  if (bs != NULL) {
    BYTE numBases = basesGetNumBases(bs);
    BYTE bi;
    for (bi = 0; bi < numBases; bi++) {
      if (basesIsActive(bs, (BYTE)(bi + 1)) == FALSE) continue;
      masked[(size_t)(*bs)->item[bi].x * MAP_ARRAY_SIZE + (*bs)->item[bi].y] = ROAD;
    }
  }
  if (pb != NULL) {
    BYTE numPills = pillsGetNumPills(pb);
    BYTE pi;
    for (pi = 0; pi < numPills; pi++) {
      size_t idx;
      BYTE t;
      if (pillsIsActive(pb, (BYTE)(pi + 1)) == FALSE) continue;
      idx = (size_t)(*pb)->item[pi].x * MAP_ARRAY_SIZE + (*pb)->item[pi].y;
      t = masked[idx];
      if (t == RIVER || t == DEEP_SEA || t == BUILDING || t == HALFBUILDING) {
        masked[idx] = ROAD;
      }
    }
  }

  crc = (uint16_t)CRCCalc(masked, (int)n);
  free(masked);
  return crc;
}

/*********************************************************
*NAME:          mapClampToRules
*PURPOSE:
*  Clamps the pills and bases a map just put into this sim
*  against the gameplay caps the sim runs on. mapRead and
*  mapLoadCompressedMap settle what the file may say — the
*  counts and the owners — and leave the caps alone, because
*  both are shared with the map editor, the preview and the
*  server's scratch validation, none of which has a sim. A
*  caller that does have one calls this straight after the
*  load, so the records the game goes on to read are inside
*  what this sim allows.
*
*  Idempotent, so a caller that loads twice may call it
*  twice.
*
*ARGUMENTS:
*  sim - The sim that has just taken the map on
*********************************************************/
void mapClampToRules(GameSim *sim) {
  if (sim == NULL) {
    return;
  }
  pillsClampToRules(sim, &sim->pb);
  basesClampToRules(sim, &sim->bs);
}

bool boloMapValidate(const char *path, char *outMapName, size_t outMapNameSize) {
  map        scratchMap;
  pillboxes  scratchPills;
  bases      scratchBases;
  starts     scratchStarts;
  bool       ok;

  if (path == NULL) {
    if (outMapName != NULL && outMapNameSize > 0) {
      outMapName[0] = '\0';
    }
    return false;
  }

  mapCreate(&scratchMap);
  pillsCreate(&scratchPills);
  basesCreate(&scratchBases);
  startsCreate(&scratchStarts);

  ok = (mapRead((char *)path, &scratchMap, &scratchPills, &scratchBases,
                &scratchStarts) == TRUE);

  if (ok && outMapName != NULL && outMapNameSize > 0) {
    const char *base = path;
    const char *p;
    size_t      len;
    for (p = path; *p; p++) {
      if (*p == '/' || *p == '\\') {
        base = p + 1;
      }
    }
    strncpy(outMapName, base, outMapNameSize - 1);
    outMapName[outMapNameSize - 1] = '\0';
    len = strlen(outMapName);
    if (len >= 4 && strcmp(outMapName + len - 4, ".map") == 0) {
      outMapName[len - 4] = '\0';
    }
  } else if (outMapName != NULL && outMapNameSize > 0) {
    outMapName[0] = '\0';
  }

  startsDestroy(&scratchStarts);
  basesDestroy(&scratchBases);
  pillsDestroy(&scratchPills);
  mapDestroy(&scratchMap);

  return ok;
}

