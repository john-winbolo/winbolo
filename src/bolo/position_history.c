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
*Filename:      position_history.c
*Purpose:
*  Ring buffer of recent tank positions for server-side
*  lag compensation (rewound collision detection).
*********************************************************/

#include "position_history.h"
#include <string.h>

void posHistoryInit(PosHistory *h) {
    memset(h, 0, sizeof(PosHistory));
}

void posHistoryRecord(PosHistory *h, WORLD x, WORLD y, bool alive) {
    h->entries[h->head].x = x;
    h->entries[h->head].y = y;
    h->entries[h->head].alive = alive;
    h->head = (h->head + 1) % POSITION_HISTORY_SIZE;
    if (h->count < POSITION_HISTORY_SIZE) {
        h->count++;
    }
}

bool posHistoryGet(const PosHistory *h, uint8_t ticksAgo, WORLD *outX, WORLD *outY) {
    uint8_t idx;
    if (ticksAgo >= h->count) {
        return FALSE;
    }
    idx = (h->head - 1 - ticksAgo + POSITION_HISTORY_SIZE) % POSITION_HISTORY_SIZE;
    if (!h->entries[idx].alive) {
        return FALSE;
    }
    *outX = h->entries[idx].x;
    *outY = h->entries[idx].y;
    return TRUE;
}
