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
*Filename:      snapshot.c
*Author:        John Morrison
*Purpose:
*  Sets up the snapshots used for jumping forwards and
*  backwards when fast forward or rewind is pressed.
*********************************************************/

#include "lv_global.h"
#include "snapshot.h"

/* Index of the first entry whose time is greater than time, or count if
   there is none. Entries before it are at or before time. */
static int lv_snapshotUpperBound(snapshot s, uint32_t time) {
  int lo = 0;
  int hi = s->count;

  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (s->items[mid].time <= time) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* Index of the first entry whose time is at or after time, or count. */
static int lv_snapshotLowerBound(snapshot s, uint32_t time) {
  int lo = 0;
  int hi = s->count;

  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (s->items[mid].time < time) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static void lv_snapshotGet(snapshot s, int i, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams) {
  *filePos = s->items[i].filePos;
  *time = s->items[i].time;
  *key = s->items[i].key;
  *pTeams = s->items[i].pTeams;
}

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
snapshot lv_snapshotCreate() {
  return NULL;
}

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
int lv_snapshotCount(snapshot *value) {
  return (*value == NULL) ? 0 : (*value)->count;
}

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
void lv_snapshotDestroy(snapshot *value) {
  if (*value != NULL) {
    free((*value)->items);
    free(*value);
    *value = NULL;
  }
}

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
bool lv_snapshotExist(snapshot *value, size_t filePos, uint32_t time) {
  int i;

  (void)filePos;
  if (*value == NULL) {
    return FALSE;
  }
  i = lv_snapshotLowerBound(*value, time);
  return (i < (*value)->count && (*value)->items[i].time == time) ? TRUE : FALSE;
}

/*********************************************************
*NAME:          lv_snapshotAdd
*AUTHOR:        John Morrison
*CREATION DATE: 1/11/99
*LAST MODIFIED:  4/7/00
*PURPOSE:
* Adds a new snapshot to the snapshot list, in time order.
* A snapshot at a time already held is not added again.
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
                    const uint16_t *kills, const uint16_t *deaths) {
  snapshot s = *value;
  snapshotEntry *e;
  int i;

  if (s == NULL) {
    s = (snapshot) calloc(1, sizeof(*s));
    if (s == NULL) {
      return;
    }
    *value = s;
  }

  /* Playback reaches snapshots in time order, so the new one almost always
     goes on the end. */
  if (s->count == 0 || s->items[s->count - 1].time < time) {
    i = s->count;
  } else {
    i = lv_snapshotLowerBound(s, time);
    if (i < s->count && s->items[i].time == time) {
      return;
    }
  }

  if (s->count == s->capacity) {
    int newCap = (s->capacity == 0) ? 64 : s->capacity * 2;
    snapshotEntry *grown = (snapshotEntry *) realloc(s->items, (size_t)newCap * sizeof(*grown));
    if (grown == NULL) {
      return;
    }
    s->items = grown;
    s->capacity = newCap;
  }

  if (i < s->count) {
    memmove(&s->items[i + 1], &s->items[i], (size_t)(s->count - i) * sizeof(*s->items));
  }
  e = &s->items[i];
  e->filePos = filePos;
  e->time = time;
  e->key = key;
  memcpy(e->pTeams, pTeams, MAX_TANKS);
  memcpy(e->kills, kills, sizeof(e->kills));
  memcpy(e->deaths, deaths, sizeof(e->deaths));
  s->count++;
}

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
bool lv_snapshotForward(snapshot *value, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams) {
  int i;

  if (*value == NULL) {
    return FALSE;
  }
  i = lv_snapshotUpperBound(*value, *time);
  if (i >= (*value)->count) {
    return FALSE;
  }
  lv_snapshotGet(*value, i, filePos, time, key, pTeams);
  return TRUE;
}

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
bool lv_snapshotFindByPosition(snapshot *value, size_t targetPos, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams) {
  snapshot s = *value;
  int i = 0;

  if (s == NULL || s->count == 0) {
    return FALSE;
  }
  while (i < s->count && s->items[i].filePos <= targetPos) {
    i++;
  }

  /* If targetPos is before the first snapshot, use the first snapshot */
  lv_snapshotGet(s, (i > 0) ? i - 1 : 0, filePos, time, key, pTeams);
  return TRUE;
}

bool lv_snapshotFindByTime(snapshot *value, uint32_t targetTime, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams) {
  snapshot s = *value;
  int i;

  if (s == NULL || s->count == 0) {
    return FALSE;
  }
  i = lv_snapshotUpperBound(s, targetTime);

  /* If targetTime is before the first snapshot, use the first snapshot */
  lv_snapshotGet(s, (i > 0) ? i - 1 : 0, filePos, time, key, pTeams);
  return TRUE;
}

bool lv_snapshotBackwards(snapshot *value, size_t *filePos, uint32_t *time, BYTE *key, BYTE **pTeams) {
  snapshot s = *value;
  int i;

  if (s == NULL || s->count == 0 || *time == 0) {
    return FALSE;
  } else if (s->items[0].time > *time) {
    return FALSE;
  }

  /* The newest snapshot strictly before the given time, or the first one
     when the time is the first snapshot's own. */
  i = lv_snapshotLowerBound(s, *time);
  lv_snapshotGet(s, (i > 0) ? i - 1 : 0, filePos, time, key, pTeams);
  return TRUE;
}

bool lv_snapshotGetTallies(snapshot *value, uint32_t time, uint16_t *kills, uint16_t *deaths) {
  snapshot s = *value;
  int i;

  if (s == NULL) {
    return FALSE;
  }
  i = lv_snapshotLowerBound(s, time);
  if (i >= s->count || s->items[i].time != time) {
    return FALSE;
  }
  memcpy(kills, s->items[i].kills, sizeof(s->items[i].kills));
  memcpy(deaths, s->items[i].deaths, sizeof(s->items[i].deaths));
  return TRUE;
}
