/*
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
*Name:          Position History
*Filename:      position_history.h
*Purpose:
*  Ring buffer of recent tank positions for server-side
*  lag compensation (rewound collision detection).
*********************************************************/

#ifndef POSITION_HISTORY_H
#define POSITION_HISTORY_H

#include "global.h"

#define POSITION_HISTORY_SIZE 16  /* ~320ms at 50 Hz, covers up to 300ms one-way */

typedef struct {
    WORLD x, y;
    bool alive;
} PosHistoryEntry;

typedef struct {
    PosHistoryEntry entries[POSITION_HISTORY_SIZE];
    uint8_t head;    /* next write slot */
    uint8_t count;   /* how many valid entries (up to POSITION_HISTORY_SIZE) */
} PosHistory;

/* What the history holds for one frame in the past. */
typedef enum {
    POS_HISTORY_NO_RECORD = 0,  /* nothing that old has been recorded yet */
    POS_HISTORY_ABSENT,         /* a record that old exists and says "not alive":
                                   a tank that was dead, a builder that was dead
                                   or inside his tank */
    POS_HISTORY_PRESENT         /* a record that old exists and has a position */
} PosHistoryLookup;

void posHistoryInit(PosHistory *h);
void posHistoryRecord(PosHistory *h, WORLD x, WORLD y, bool alive);
/* The record made ticksAgo frames before the newest one (0 is the newest).
 * outX and outY are written only for POS_HISTORY_PRESENT. */
PosHistoryLookup posHistoryLookup(const PosHistory *h, uint8_t ticksAgo, WORLD *outX, WORLD *outY);
/* TRUE only for POS_HISTORY_PRESENT. FALSE does not say which of the other
 * two it was; a caller that must tell them apart uses posHistoryLookup. */
bool posHistoryGet(const PosHistory *h, uint8_t ticksAgo, WORLD *outX, WORLD *outY);

#endif /* POSITION_HISTORY_H */
