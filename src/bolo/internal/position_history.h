/*
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

void posHistoryInit(PosHistory *h);
void posHistoryRecord(PosHistory *h, WORLD x, WORLD y, bool alive);
bool posHistoryGet(const PosHistory *h, uint8_t ticksAgo, WORLD *outX, WORLD *outY);

#endif /* POSITION_HISTORY_H */
