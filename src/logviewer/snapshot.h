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
*Name:          Snapshot
*Filename:      snapshot.h
*Author:        John Morrison
*Purpose:
*  Sets up the snapshots used for jumping forwards and
*  backwards when fast forward or rewind is pressed.
*********************************************************/

#ifndef __SNAPSHOT_H
#define __SNAPSHOT_H


/** Snapshot data structure */
typedef struct snapshotObj *snapshot;
struct snapshotObj {
  snapshot next; /* Next snapshot to goto */
  size_t filePos; /* Position in the decompressed stream */
  uint32_t time; /* Time in ticks in the game */
  BYTE pTeams[MAX_TANKS]; /* Team IDs */
  BYTE key; /* Decryption XOR key */
};


/*********************************************************
*NAME:          lv_snapshotCreate
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Creates the snapshot list. Returns empty snapshot list
*
*ARGUMENTS:
*
*********************************************************/
snapshot lv_snapshotCreate();

/*********************************************************
*NAME:          lv_snapshotDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Destroys the snapshots we have created
*
*ARGUMENTS:
* value - Snapshot to destroy
*********************************************************/
void lv_snapshotDestroy(snapshot *value);


/*********************************************************
*NAME:          snapshotExists
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Returns whether a snapshot exists at this time
*
*ARGUMENTS:
* value - Snapshot to check
* filePos - Unuesd variable
* time    - Time in game ticks
*********************************************************/
bool lv_snapshotExist(snapshot *value, size_t filePos, uint32_t time);

/*********************************************************
*NAME:          lv_snapshotAdd
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Adds a new snapshot to the snapshot list
*
*ARGUMENTS:
* value - Snapshot list to add to
* filePos - File position this snapshot is at
* time - Time in ticks this snapshot is at
* key - Key at this snapshot
* pteams - Player Teams to copy
*********************************************************/
void lv_snapshotAdd(snapshot *value, size_t filePos, uint32_t time, BYTE key, BYTE *pTeams);


/*********************************************************
*NAME:          snapshotFoward
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Moves forward to the next snapshot from the given time
* Returns whether a snapshot exists forward of this point.
*
*ARGUMENTS:
* value - Snapshot list to add to
* filePos - File position that the foward snapshot is
* time - Time in ticks that the foward (to check then set)
* key - Key at this snapshot to set to
* pteams - Player Teams to copy to set to
*********************************************************/
bool lv_snapshotForward(snapshot *value, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams);

/*********************************************************
*NAME:          lv_snapshotBackwards
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Moves backwards to the previous snapshot from the given time
* Returns whether a snapshot exists backwards of this point.
*
*ARGUMENTS:
* value - Snapshot list to add to
* filePos - File position that the foward snapshot is
* time - Time in ticks that the foward (to check then set)
* key - Key at this snapshot to set to
* pteams - Player Teams to copy to set to
*********************************************************/
bool lv_snapshotBackwards(snapshot *value, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams);

/*********************************************************
*NAME:          lv_snapshotCount
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Returns the number of snapshots we have.
*
*ARGUMENTS:
*
*********************************************************/
int lv_snapshotCount(snapshot *value);

/*********************************************************
*NAME:          lv_snapshotFindByPosition
*PURPOSE:
* Finds the latest snapshot at or before the given file
* position. Returns TRUE if found.
*
*ARGUMENTS:
* value   - Snapshot list
* targetPos - Target byte position in the log
* filePos - Returned file position of the snapshot
* time    - Returned time of the snapshot
* key     - Returned decryption key
* pTeams  - Returned pointer to team data
*********************************************************/
bool lv_snapshotFindByPosition(snapshot *value, size_t targetPos, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams);

/*********************************************************
*NAME:          lv_snapshotFindByTime
*PURPOSE:
* Finds the latest snapshot at or before the given game
* time (ms). Returns TRUE if found.
*
*ARGUMENTS:
* value      - Snapshot list
* targetTime - Target game time in ms
* filePos    - Returned file position of the snapshot
* time       - Returned time of the snapshot
* key        - Returned decryption key
* pTeams     - Returned pointer to team data
*********************************************************/
bool lv_snapshotFindByTime(snapshot *value, uint32_t targetTime, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams);

#endif /* __SNAPSHOT_H */
