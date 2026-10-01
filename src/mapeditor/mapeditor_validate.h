/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_validate.h
 * Purpose:
 *   Map validation engine for the map editor. Checks
 *   object counts, terrain placement, border zones, and
 *   stacked objects.
 *********************************************************/

#ifndef MAPEDITOR_VALIDATE_H
#define MAPEDITOR_VALIDATE_H

#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VALIDATE_ERROR,
    VALIDATE_WARNING
} ValidateSeverity;

typedef enum {
    VALIDATE_LOC_NONE,
    VALIDATE_LOC_TILE,
    VALIDATE_LOC_BASE,
    VALIDATE_LOC_PILL,
    VALIDATE_LOC_START
} ValidateLocType;

typedef struct {
    ValidateSeverity severity;
    ValidateLocType locType;
    int locIndex;
    BYTE locX, locY;
    char message[128];
} ValidateIssue;

typedef struct {
    ValidateIssue *issues;
    int count;
    int capacity;
    int errorCount;
    int warningCount;
} ValidateResult;

/* Run all validation checks. Caller must call validateResultFree() when done. */
ValidateResult mapEditorValidate(map mp, bases bs, pillboxes pb, starts ss);

/* Free the issues array. */
void validateResultFree(ValidateResult *result);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_VALIDATE_H */
