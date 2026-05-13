/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_undo.h
 * Purpose:
 *   Undo/redo system for the map editor. Tracks tile and
 *   object changes grouped into commands.
 *********************************************************/

#ifndef MAPEDITOR_UNDO_H
#define MAPEDITOR_UNDO_H

#include <stdbool.h>
#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A single tile change within a command. */
typedef struct {
    BYTE x;
    BYTE y;
    BYTE oldTerrain;
    BYTE newTerrain;
} TileChange;

/* Object change types. */
typedef enum {
    OBJ_ADD,
    OBJ_REMOVE,
    OBJ_MODIFY
} ObjChangeType;

typedef enum {
    OBJ_BASE,
    OBJ_PILL,
    OBJ_START
} ObjKind;

/* A single object change within a command. */
typedef struct {
    ObjChangeType changeType;
    ObjKind kind;
    int index;
    union {
        base baseData;
        pillbox pillData;
        start startData;
    } oldData;
    union {
        base baseData;
        pillbox pillData;
        start startData;
    } newData;
} ObjChange;

/* A command groups all changes from a single user action. */
typedef struct {
    TileChange *tileChanges;
    int numTileChanges;
    int capTileChanges;
    ObjChange *objChanges;
    int numObjChanges;
    int capObjChanges;
} EditCommand;

#define UNDO_STACK_MAX 100

typedef struct {
    EditCommand commands[UNDO_STACK_MAX];
    int count;      /* Number of commands in the undo stack */
    int redoCount;  /* Number of commands in the redo portion */
    int savedCommandIndex; /* Stack count at last save (-1 = lost) */
    bool building;  /* True while a command is being recorded */
} UndoStack;

/* Call before starting any edit operation. */
void undoBeginCommand(UndoStack *stack);

/* Record a tile change. Call BEFORE writing to mapItem.
 * Deduplicates: if (x,y) already recorded, updates newTerrain only. */
void undoRecordTile(UndoStack *stack, map mp, BYTE x, BYTE y, BYTE newTerrain);

/* Record object changes. */
void undoRecordObjAdd(UndoStack *stack, ObjKind kind, int index, const void *data);
void undoRecordObjRemove(UndoStack *stack, ObjKind kind, int index, const void *data);
void undoRecordObjModify(UndoStack *stack, ObjKind kind, int index,
                         const void *oldData, const void *newData);

/* Finalize the current command and push it onto the stack.
 * Discards empty commands. Clears redo history. */
void undoEndCommand(UndoStack *stack);

/* Apply undo. Returns false if nothing to undo. */
bool undoApply(UndoStack *stack, map mp, pillboxes pb, bases bs, starts ss);

/* Apply redo. Returns false if nothing to redo. */
bool redoApply(UndoStack *stack, map mp, pillboxes pb, bases bs, starts ss);

/* Free all commands in the stack. */
void undoStackClear(UndoStack *stack);

/* Initialize an undo stack (zero everything). */
void undoStackInit(UndoStack *stack);

/* Query: can undo/redo? */
bool undoCanUndo(const UndoStack *stack);
bool undoCanRedo(const UndoStack *stack);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_UNDO_H */
