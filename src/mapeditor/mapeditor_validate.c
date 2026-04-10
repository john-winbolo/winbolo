/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_validate.c
 * Purpose:
 *   Map validation engine. Pure logic — no SDL or ImGui
 *   dependencies. Checks object counts, terrain placement,
 *   border zones, and stacked objects.
 *********************************************************/

#include "mapeditor_validate.h"
#include "../bolo/bolo_map.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Initial capacity for the issues array */
#define VALIDATE_INIT_CAP 16

static void addIssue(ValidateResult *r, ValidateSeverity sev,
                     ValidateLocType loc, int idx, BYTE x, BYTE y,
                     const char *fmt, ...) {
    if (r->count >= r->capacity) {
        int newCap = r->capacity ? r->capacity * 2 : VALIDATE_INIT_CAP;
        ValidateIssue *tmp = (ValidateIssue *)realloc(r->issues,
                                                       (size_t)newCap * sizeof(ValidateIssue));
        if (!tmp) return;
        r->issues = tmp;
        r->capacity = newCap;
    }
    ValidateIssue *iss = &r->issues[r->count++];
    iss->severity = sev;
    iss->locType = loc;
    iss->locIndex = idx;
    iss->locX = x;
    iss->locY = y;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(iss->message, sizeof(iss->message), fmt, ap);
    va_end(ap);

    if (sev == VALIDATE_ERROR) r->errorCount++;
    else r->warningCount++;
}

static bool isTraversable(BYTE terrain) {
    switch (terrain) {
    case BUILDING: case SWAMP: case CRATER: case ROAD:
    case FOREST: case RUBBLE: case GRASS: case HALFBUILDING:
        return true;
    default:
        return false;
    }
}

static bool isInPlayableArea(BYTE x, BYTE y) {
    return x > MAP_MINE_EDGE_LEFT && x < MAP_MINE_EDGE_RIGHT &&
           y > MAP_MINE_EDGE_TOP  && y < MAP_MINE_EDGE_BOTTOM;
}

static BYTE stripMine(BYTE terrain) {
    if (terrain >= MINE_START && terrain <= MINE_END)
        return (BYTE)(terrain - MINE_SUBTRACT);
    return terrain;
}

ValidateResult mapEditorValidate(map mp, bases bs, pillboxes pb, starts ss) {
    ValidateResult r;
    memset(&r, 0, sizeof(r));

    /* 1-3: Object count limits */
    if (bs->numBases > MAX_BASES) {
        addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_NONE, -1, 0, 0,
                 "Too many bases: %d (max %d)", bs->numBases, MAX_BASES);
    }
    if (pb->numPills > MAX_PILLS) {
        addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_NONE, -1, 0, 0,
                 "Too many pillboxes: %d (max %d)", pb->numPills, MAX_PILLS);
    }
    if (ss->numStarts > MAX_STARTS) {
        addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_NONE, -1, 0, 0,
                 "Too many starts: %d (max %d)", ss->numStarts, MAX_STARTS);
    }

    /* 4: Base on non-traversable terrain */
    for (int i = 0; i < bs->numBases && i < MAX_BASES; i++) {
        BYTE x = bs->item[i].x, y = bs->item[i].y;
        BYTE t = stripMine(mp->mapItem[x][y]);
        if (!isTraversable(t)) {
            addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_BASE, i, x, y,
                     "Base #%d at (%d,%d): on non-traversable terrain", i, x, y);
        }
        /* 7: Base in border zone */
        if (!isInPlayableArea(x, y)) {
            addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_BASE, i, x, y,
                     "Base #%d at (%d,%d): in mine border zone", i, x, y);
        }
    }

    /* 5: Pillbox on non-traversable terrain */
    for (int i = 0; i < pb->numPills && i < MAX_PILLS; i++) {
        BYTE x = pb->item[i].x, y = pb->item[i].y;
        BYTE t = stripMine(mp->mapItem[x][y]);
        if (!isTraversable(t)) {
            addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_PILL, i, x, y,
                     "Pillbox #%d at (%d,%d): on non-traversable terrain", i, x, y);
        }
        /* 7: Pillbox in border zone */
        if (!isInPlayableArea(x, y)) {
            addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_PILL, i, x, y,
                     "Pillbox #%d at (%d,%d): in mine border zone", i, x, y);
        }
    }

    /* 6: Start must be on deep sea + 7: Start in border zone */
    for (int i = 0; i < ss->numStarts && i < MAX_STARTS; i++) {
        BYTE x = ss->item[i].x, y = ss->item[i].y;
        BYTE t = stripMine(mp->mapItem[x][y]);
        if (t != DEEP_SEA) {
            addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_START, i, x, y,
                     "Start #%d at (%d,%d): must be on deep sea", i, x, y);
        }
        if (!isInPlayableArea(x, y)) {
            addIssue(&r, VALIDATE_ERROR, VALIDATE_LOC_START, i, x, y,
                     "Start #%d at (%d,%d): in mine border zone", i, x, y);
        }
    }

    /* 8-9: Start count warnings */
    if (ss->numStarts == 0) {
        addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_NONE, -1, 0, 0,
                 "No start positions placed — map is unplayable");
    } else if (ss->numStarts == 1) {
        addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_NONE, -1, 0, 0,
                 "Only 1 start position — single player only");
    }

    /* 10: Stacked objects */
    /* Base-Base overlaps */
    for (int i = 0; i < bs->numBases && i < MAX_BASES; i++) {
        for (int j = i + 1; j < bs->numBases && j < MAX_BASES; j++) {
            if (bs->item[i].x == bs->item[j].x && bs->item[i].y == bs->item[j].y) {
                addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_TILE, -1,
                         bs->item[i].x, bs->item[i].y,
                         "Base #%d and Base #%d overlap at (%d,%d)",
                         i, j, bs->item[i].x, bs->item[i].y);
            }
        }
    }
    /* Pill-Pill overlaps */
    for (int i = 0; i < pb->numPills && i < MAX_PILLS; i++) {
        for (int j = i + 1; j < pb->numPills && j < MAX_PILLS; j++) {
            if (pb->item[i].x == pb->item[j].x && pb->item[i].y == pb->item[j].y) {
                addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_TILE, -1,
                         pb->item[i].x, pb->item[i].y,
                         "Pillbox #%d and Pillbox #%d overlap at (%d,%d)",
                         i, j, pb->item[i].x, pb->item[i].y);
            }
        }
    }
    /* Base-Pill overlaps */
    for (int i = 0; i < bs->numBases && i < MAX_BASES; i++) {
        for (int j = 0; j < pb->numPills && j < MAX_PILLS; j++) {
            if (bs->item[i].x == pb->item[j].x && bs->item[i].y == pb->item[j].y) {
                addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_TILE, -1,
                         bs->item[i].x, bs->item[i].y,
                         "Base #%d and Pillbox #%d overlap at (%d,%d)",
                         i, j, bs->item[i].x, bs->item[i].y);
            }
        }
    }
    /* Start-Start overlaps */
    for (int i = 0; i < ss->numStarts && i < MAX_STARTS; i++) {
        for (int j = i + 1; j < ss->numStarts && j < MAX_STARTS; j++) {
            if (ss->item[i].x == ss->item[j].x && ss->item[i].y == ss->item[j].y) {
                addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_TILE, -1,
                         ss->item[i].x, ss->item[i].y,
                         "Start #%d and Start #%d overlap at (%d,%d)",
                         i, j, ss->item[i].x, ss->item[i].y);
            }
        }
    }
    /* Start-Base overlaps */
    for (int i = 0; i < ss->numStarts && i < MAX_STARTS; i++) {
        for (int j = 0; j < bs->numBases && j < MAX_BASES; j++) {
            if (ss->item[i].x == bs->item[j].x && ss->item[i].y == bs->item[j].y) {
                addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_TILE, -1,
                         ss->item[i].x, ss->item[i].y,
                         "Start #%d and Base #%d overlap at (%d,%d)",
                         i, j, ss->item[i].x, ss->item[i].y);
            }
        }
    }
    /* Start-Pill overlaps */
    for (int i = 0; i < ss->numStarts && i < MAX_STARTS; i++) {
        for (int j = 0; j < pb->numPills && j < MAX_PILLS; j++) {
            if (ss->item[i].x == pb->item[j].x && ss->item[i].y == pb->item[j].y) {
                addIssue(&r, VALIDATE_WARNING, VALIDATE_LOC_TILE, -1,
                         ss->item[i].x, ss->item[i].y,
                         "Start #%d and Pillbox #%d overlap at (%d,%d)",
                         i, j, ss->item[i].x, ss->item[i].y);
            }
        }
    }

    return r;
}

void validateResultFree(ValidateResult *result) {
    free(result->issues);
    result->issues = NULL;
    result->count = 0;
    result->capacity = 0;
    result->errorCount = 0;
    result->warningCount = 0;
}
