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
*Name:          Snapshot
*Filename:      snapshot.h
*Author:        John Morrison
*Purpose:
*  Sets up the snapshots used for jumping forwards and
*  backwards when fast forward or rewind is pressed.
*********************************************************/

#ifndef __SNAPSHOT_H
#define __SNAPSHOT_H


/** One snapshot the decoder has passed */
typedef struct {
  size_t filePos; /* Position in the decompressed stream */
  uint32_t time; /* Time in ticks in the game */
  BYTE pTeams[MAX_TANKS]; /* Team IDs */
  BYTE key; /* Decryption XOR key */
  /* The kill and death tallies playback had counted on reaching it. No
     snapshot body carries them, so a restore puts these back. */
  uint16_t kills[MAX_TANKS];
  uint16_t deaths[MAX_TANKS];
  /* When each slot's tank last died, as playback had it on reaching
     it. The snapshot body says only that a tank is destroyed, not since
     when, and the death static over the game view plays from that time. */
  uint32_t deathTimeMs[MAX_TANKS];
} snapshotEntry;

/** Snapshot data structure: the entries in time order. A server writes one
 *  every few seconds, so a log that ran for days holds hundreds of thousands
 *  and every lookup is a binary search. NULL until the first add. */
typedef struct snapshotObj *snapshot;
struct snapshotObj {
  snapshotEntry *items;
  int count;
  int capacity;
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
* kills  - Kill tallies to copy
* deaths - Death tallies to copy
*********************************************************/
void lv_snapshotAdd(snapshot *value, size_t filePos, uint32_t time, BYTE key, BYTE *pTeams,
                    const uint16_t *kills, const uint16_t *deaths);


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

/*********************************************************
*NAME:          lv_snapshotGetTallies
*PURPOSE:
* Copies out the kill and death tallies stored with the
* snapshot at exactly this time. Returns FALSE, leaving the
* arrays alone, when there is none.
*
*ARGUMENTS:
* value  - Snapshot list
* time   - Time of the snapshot
* kills  - Returned kill tallies (MAX_TANKS)
* deaths - Returned death tallies (MAX_TANKS)
*********************************************************/
bool lv_snapshotGetTallies(snapshot *value, uint32_t time, uint16_t *kills, uint16_t *deaths);

/* The time each slot's tank last died, stored with the snapshot at
   exactly this time. A new snapshot's times are zero until set. Both take
   arrays of MAX_TANKS and return FALSE when no snapshot is stored at that
   time. */
bool lv_snapshotSetDeathTimes(snapshot *value, uint32_t time, const uint32_t *deathTimeMs);
bool lv_snapshotGetDeathTimes(snapshot *value, uint32_t time, uint32_t *deathTimeMs);

#endif /* __SNAPSHOT_H */
