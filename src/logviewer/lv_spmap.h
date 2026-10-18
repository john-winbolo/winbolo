/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The map the Log Viewer writes for "Open map at this time in single
 * player": the recording's world at the playhead, with the owners moved so
 * the single-player user stands where one recorded player stood.
 *
 * A single-player game puts the user in player slot 0. So with a player
 * selected, everything that player or an ally owns is written as owned by
 * slot 0. Every other owner is written as a slot that is not 0, so it is
 * hostile to the user, and two recorded owners never share a slot. Neutral
 * stays neutral. With no player selected nothing is written as slot 0.
 *
 * Only plain C types here, so a test on either side of the sim / viewer
 * split can include it.
 */

#ifndef LV_SPMAP_H
#define LV_SPMAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_SPMAP_SLOTS   16
#define LV_SPMAP_USER    0     /* the slot a single-player game gives the user */
#define LV_SPMAP_NEUTRAL 0xFF  /* the map file's "no owner" */

/* Build the owner table: table[recorded owner] is the owner to write.
 *
 *  self   - the selected player's slot, or -1 for none
 *  allied - [slot] not 0 when the slot is the selected player or an ally;
 *           not read when self is -1
 *  owns   - [slot] not 0 when the slot owns a pillbox or a base in the map;
 *           read only to find a free slot when self is -1
 *
 * With a player selected: allied slots go to slot 0, the owner recorded in
 * slot 0 (if it is not allied) takes the selected player's slot, which the
 * move left free, and every other slot stays.
 *
 * With none selected: every slot stays but slot 0, which goes to the highest
 * slot that owns nothing. If all 16 slots own something there is no free
 * slot, and slot 0's items are written neutral so they are not the user's. */
static inline void lvSpmapOwnerTable(int self, const unsigned char *allied,
                                     const unsigned char *owns,
                                     unsigned char table[LV_SPMAP_SLOTS]) {
  int i;

  for (i = 0; i < LV_SPMAP_SLOTS; i++) {
    table[i] = (unsigned char)i;
  }
  if (self >= 0 && self < LV_SPMAP_SLOTS) {
    int zeroAllied = (self == LV_SPMAP_USER) ||
                     (allied != NULL && allied[LV_SPMAP_USER] != 0);
    for (i = 0; i < LV_SPMAP_SLOTS; i++) {
      if (i == self || (allied != NULL && allied[i] != 0)) {
        table[i] = LV_SPMAP_USER;
      }
    }
    if (!zeroAllied) {
      table[LV_SPMAP_USER] = (unsigned char)self;
    }
    return;
  }
  table[LV_SPMAP_USER] = LV_SPMAP_NEUTRAL;
  for (i = LV_SPMAP_SLOTS - 1; i > 0; i--) {
    if (owns == NULL || owns[i] == 0) {
      table[LV_SPMAP_USER] = (unsigned char)i;
      break;
    }
  }
}

/* The owner to write for a recorded owner. */
static inline unsigned char lvSpmapOwner(const unsigned char table[LV_SPMAP_SLOTS],
                                         unsigned char owner) {
  return (owner < LV_SPMAP_SLOTS) ? table[owner] : (unsigned char)LV_SPMAP_NEUTRAL;
}

/* A recorded tank frame (0 is north, 4 east, 16 to the circle) as the
 * direction a map file's start holds (0 is east, 4 north): the inverse of
 * the game's startsConvertDir. */
static inline unsigned char lvSpmapStartDir(unsigned char frame) {
  return (unsigned char)((4 + 16 - (frame & 15)) & 15);
}

/* What a write did. The places are the recording's own map squares. */
typedef struct {
  int           self;          /* the slot written as the user's, or -1 */
  int           tankStart;     /* 1: the one start is the selected tank's place */
  unsigned char startX, startY, startDir;   /* that start, as written */
  int           pills;         /* pillboxes written */
  int           pillsCarried;  /* left out: in a tank at this time */
  int           bases;
  int           starts;
  int           havePill;      /* firstPillX/Y are set */
  unsigned char firstPillX, firstPillY;     /* the first pillbox written */
} LvSpmapInfo;

/* Write the current decoder's world to path as a .map. self is the selected
 * player's slot, or -1. The caller holds the client mutex. Returns 1 when
 * the file was written. */
int lvSpmapWrite(const char *path, int self, LvSpmapInfo *info);

/* lvSpmapWriteNew returns this when a file of that name is there already. */
#define LV_SPMAP_EXISTS (-1)

/* As lvSpmapWrite, but never writes over a file: the file is made by the
 * open itself, and when a file of that name is there the result is
 * LV_SPMAP_EXISTS and that file is not touched. */
int lvSpmapWriteNew(const char *path, int self, LvSpmapInfo *info);

/* The same for a log on disk, with no window: decode logPath up to ms of
 * playback and write the world there. A self that is not in the game at
 * that time counts as none. Returns 1 when the file was written. */
int lvSpmapWriteFromLog(const char *logPath, uint32_t ms, int self,
                        const char *mapPath, LvSpmapInfo *info);

#ifdef __cplusplus
}
#endif

#endif /* LV_SPMAP_H */
