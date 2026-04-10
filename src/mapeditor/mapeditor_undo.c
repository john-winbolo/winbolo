/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_undo.c
 * Purpose:
 *   Undo/redo command stack for the map editor.
 *********************************************************/

#include "mapeditor_undo.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_TILE_CAP 64
#define INITIAL_OBJ_CAP 8

/* Free a single command's dynamic arrays. */
static void freeCommand(EditCommand *cmd) {
    free(cmd->tileChanges);
    free(cmd->objChanges);
    memset(cmd, 0, sizeof(*cmd));
}

void undoStackInit(UndoStack *stack) {
    memset(stack, 0, sizeof(*stack));
}

void undoStackClear(UndoStack *stack) {
    for (int i = 0; i < stack->count + stack->redoCount; i++) {
        freeCommand(&stack->commands[i]);
    }
    stack->count = 0;
    stack->redoCount = 0;
    stack->savedCommandIndex = 0;
    stack->building = false;
}

void undoBeginCommand(UndoStack *stack) {
    /* If already building (caller forgot undoEndCommand), end it first */
    if (stack->building) {
        undoEndCommand(stack);
    }

    /* Clear redo history */
    for (int i = stack->count; i < stack->count + stack->redoCount; i++) {
        freeCommand(&stack->commands[i]);
    }
    stack->redoCount = 0;

    /* If stack is full, drop oldest command */
    if (stack->count >= UNDO_STACK_MAX) {
        freeCommand(&stack->commands[0]);
        memmove(&stack->commands[0], &stack->commands[1],
                (UNDO_STACK_MAX - 1) * sizeof(EditCommand));
        stack->count = UNDO_STACK_MAX - 1;
        if (stack->savedCommandIndex > 0) {
            stack->savedCommandIndex--;
        } else {
            stack->savedCommandIndex = -1;
        }
    }

    /* Initialize the new command at commands[count] */
    EditCommand *cmd = &stack->commands[stack->count];
    memset(cmd, 0, sizeof(*cmd));
    stack->building = true;
}

void undoRecordTile(UndoStack *stack, map mp, BYTE x, BYTE y, BYTE newTerrain) {
    if (!stack->building) return;

    EditCommand *cmd = &stack->commands[stack->count];

    /* Deduplicate: check if this (x,y) is already recorded */
    for (int i = 0; i < cmd->numTileChanges; i++) {
        if (cmd->tileChanges[i].x == x && cmd->tileChanges[i].y == y) {
            cmd->tileChanges[i].newTerrain = newTerrain;
            return;
        }
    }

    /* Grow array if needed */
    if (cmd->numTileChanges >= cmd->capTileChanges) {
        int newCap = cmd->capTileChanges == 0 ? INITIAL_TILE_CAP : cmd->capTileChanges * 2;
        TileChange *newArr = (TileChange *)realloc(cmd->tileChanges, newCap * sizeof(TileChange));
        if (!newArr) return;
        cmd->tileChanges = newArr;
        cmd->capTileChanges = newCap;
    }

    TileChange *tc = &cmd->tileChanges[cmd->numTileChanges++];
    tc->x = x;
    tc->y = y;
    tc->oldTerrain = mp->mapItem[x][y];
    tc->newTerrain = newTerrain;
}

static void ensureObjCapacity(EditCommand *cmd) {
    if (cmd->numObjChanges >= cmd->capObjChanges) {
        int newCap = cmd->capObjChanges == 0 ? INITIAL_OBJ_CAP : cmd->capObjChanges * 2;
        ObjChange *newArr = (ObjChange *)realloc(cmd->objChanges, newCap * sizeof(ObjChange));
        if (!newArr) return;
        cmd->objChanges = newArr;
        cmd->capObjChanges = newCap;
    }
}

static void copyObjData(ObjKind kind, const void *src, void *dstUnion) {
    switch (kind) {
    case OBJ_BASE:
        memcpy(dstUnion, src, sizeof(base));
        break;
    case OBJ_PILL:
        memcpy(dstUnion, src, sizeof(pillbox));
        break;
    case OBJ_START:
        memcpy(dstUnion, src, sizeof(start));
        break;
    }
}

void undoRecordObjAdd(UndoStack *stack, ObjKind kind, int index, const void *data) {
    if (!stack->building) return;
    EditCommand *cmd = &stack->commands[stack->count];
    ensureObjCapacity(cmd);

    ObjChange *oc = &cmd->objChanges[cmd->numObjChanges++];
    memset(oc, 0, sizeof(*oc));
    oc->changeType = OBJ_ADD;
    oc->kind = kind;
    oc->index = index;
    copyObjData(kind, data, &oc->newData);
}

void undoRecordObjRemove(UndoStack *stack, ObjKind kind, int index, const void *data) {
    if (!stack->building) return;
    EditCommand *cmd = &stack->commands[stack->count];
    ensureObjCapacity(cmd);

    ObjChange *oc = &cmd->objChanges[cmd->numObjChanges++];
    memset(oc, 0, sizeof(*oc));
    oc->changeType = OBJ_REMOVE;
    oc->kind = kind;
    oc->index = index;
    copyObjData(kind, data, &oc->oldData);
}

void undoRecordObjModify(UndoStack *stack, ObjKind kind, int index,
                         const void *oldData, const void *newData) {
    if (!stack->building) return;
    EditCommand *cmd = &stack->commands[stack->count];
    ensureObjCapacity(cmd);

    ObjChange *oc = &cmd->objChanges[cmd->numObjChanges++];
    memset(oc, 0, sizeof(*oc));
    oc->changeType = OBJ_MODIFY;
    oc->kind = kind;
    oc->index = index;
    copyObjData(kind, oldData, &oc->oldData);
    copyObjData(kind, newData, &oc->newData);
}

void undoEndCommand(UndoStack *stack) {
    if (!stack->building) return;
    stack->building = false;

    EditCommand *cmd = &stack->commands[stack->count];
    if (cmd->numTileChanges == 0 && cmd->numObjChanges == 0) {
        /* Empty command — discard */
        freeCommand(cmd);
        return;
    }

    stack->count++;
}

/* Undo a single object change (reverse direction). */
static void undoObjChange(const ObjChange *oc, pillboxes pb, bases bs, starts ss) {
    switch (oc->changeType) {
    case OBJ_ADD:
        /* Undo an add = remove the object at index */
        switch (oc->kind) {
        case OBJ_PILL:
            if (oc->index < pb->numPills) {
                for (int j = oc->index; j < pb->numPills - 1; j++)
                    pb->item[j] = pb->item[j + 1];
                pb->numPills--;
            }
            break;
        case OBJ_BASE:
            if (oc->index < bs->numBases) {
                for (int j = oc->index; j < bs->numBases - 1; j++)
                    bs->item[j] = bs->item[j + 1];
                bs->numBases--;
            }
            break;
        case OBJ_START:
            if (oc->index < ss->numStarts) {
                for (int j = oc->index; j < ss->numStarts - 1; j++)
                    ss->item[j] = ss->item[j + 1];
                ss->numStarts--;
            }
            break;
        }
        break;

    case OBJ_REMOVE:
        /* Undo a remove = re-insert the object at index */
        switch (oc->kind) {
        case OBJ_PILL:
            if (pb->numPills < MAX_PILLS) {
                for (int j = pb->numPills; j > oc->index; j--)
                    pb->item[j] = pb->item[j - 1];
                pb->item[oc->index] = oc->oldData.pillData;
                pb->numPills++;
            }
            break;
        case OBJ_BASE:
            if (bs->numBases < MAX_BASES) {
                for (int j = bs->numBases; j > oc->index; j--)
                    bs->item[j] = bs->item[j - 1];
                bs->item[oc->index] = oc->oldData.baseData;
                bs->numBases++;
            }
            break;
        case OBJ_START:
            if (ss->numStarts < MAX_STARTS) {
                for (int j = ss->numStarts; j > oc->index; j--)
                    ss->item[j] = ss->item[j - 1];
                ss->item[oc->index] = oc->oldData.startData;
                ss->numStarts++;
            }
            break;
        }
        break;

    case OBJ_MODIFY:
        /* Undo a modify = restore old data */
        switch (oc->kind) {
        case OBJ_PILL:
            if (oc->index < pb->numPills)
                pb->item[oc->index] = oc->oldData.pillData;
            break;
        case OBJ_BASE:
            if (oc->index < bs->numBases)
                bs->item[oc->index] = oc->oldData.baseData;
            break;
        case OBJ_START:
            if (oc->index < ss->numStarts)
                ss->item[oc->index] = oc->oldData.startData;
            break;
        }
        break;
    }
}

/* Redo a single object change (forward direction). */
static void redoObjChange(const ObjChange *oc, pillboxes pb, bases bs, starts ss) {
    switch (oc->changeType) {
    case OBJ_ADD:
        /* Redo an add = insert the object at index */
        switch (oc->kind) {
        case OBJ_PILL:
            if (pb->numPills < MAX_PILLS) {
                for (int j = pb->numPills; j > oc->index; j--)
                    pb->item[j] = pb->item[j - 1];
                pb->item[oc->index] = oc->newData.pillData;
                pb->numPills++;
            }
            break;
        case OBJ_BASE:
            if (bs->numBases < MAX_BASES) {
                for (int j = bs->numBases; j > oc->index; j--)
                    bs->item[j] = bs->item[j - 1];
                bs->item[oc->index] = oc->newData.baseData;
                bs->numBases++;
            }
            break;
        case OBJ_START:
            if (ss->numStarts < MAX_STARTS) {
                for (int j = ss->numStarts; j > oc->index; j--)
                    ss->item[j] = ss->item[j - 1];
                ss->item[oc->index] = oc->newData.startData;
                ss->numStarts++;
            }
            break;
        }
        break;

    case OBJ_REMOVE:
        /* Redo a remove = remove the object at index */
        switch (oc->kind) {
        case OBJ_PILL:
            if (oc->index < pb->numPills) {
                for (int j = oc->index; j < pb->numPills - 1; j++)
                    pb->item[j] = pb->item[j + 1];
                pb->numPills--;
            }
            break;
        case OBJ_BASE:
            if (oc->index < bs->numBases) {
                for (int j = oc->index; j < bs->numBases - 1; j++)
                    bs->item[j] = bs->item[j + 1];
                bs->numBases--;
            }
            break;
        case OBJ_START:
            if (oc->index < ss->numStarts) {
                for (int j = oc->index; j < ss->numStarts - 1; j++)
                    ss->item[j] = ss->item[j + 1];
                ss->numStarts--;
            }
            break;
        }
        break;

    case OBJ_MODIFY:
        /* Redo a modify = apply new data */
        switch (oc->kind) {
        case OBJ_PILL:
            if (oc->index < pb->numPills)
                pb->item[oc->index] = oc->newData.pillData;
            break;
        case OBJ_BASE:
            if (oc->index < bs->numBases)
                bs->item[oc->index] = oc->newData.baseData;
            break;
        case OBJ_START:
            if (oc->index < ss->numStarts)
                ss->item[oc->index] = oc->newData.startData;
            break;
        }
        break;
    }
}

bool undoApply(UndoStack *stack, map mp, pillboxes pb, bases bs, starts ss) {
    if (stack->count <= 0) return false;

    stack->count--;
    stack->redoCount++;

    EditCommand *cmd = &stack->commands[stack->count];

    /* Restore tiles in reverse order */
    for (int i = cmd->numTileChanges - 1; i >= 0; i--) {
        TileChange *tc = &cmd->tileChanges[i];
        mp->mapItem[tc->x][tc->y] = tc->oldTerrain;
    }

    /* Restore objects in reverse order */
    for (int i = cmd->numObjChanges - 1; i >= 0; i--) {
        undoObjChange(&cmd->objChanges[i], pb, bs, ss);
    }

    return true;
}

bool redoApply(UndoStack *stack, map mp, pillboxes pb, bases bs, starts ss) {
    if (stack->redoCount <= 0) return false;

    EditCommand *cmd = &stack->commands[stack->count];

    /* Apply tiles forward */
    for (int i = 0; i < cmd->numTileChanges; i++) {
        TileChange *tc = &cmd->tileChanges[i];
        mp->mapItem[tc->x][tc->y] = tc->newTerrain;
    }

    /* Apply objects forward */
    for (int i = 0; i < cmd->numObjChanges; i++) {
        redoObjChange(&cmd->objChanges[i], pb, bs, ss);
    }

    stack->count++;
    stack->redoCount--;

    return true;
}

bool undoCanUndo(const UndoStack *stack) {
    return stack->count > 0;
}

bool undoCanRedo(const UndoStack *stack) {
    return stack->redoCount > 0;
}
