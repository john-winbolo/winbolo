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
 *Name:          Client Map Preview
 *Filename:      client_mappreview.h
 *Purpose:
 *  Opaque MapPreview handle that owns a transient
 *  map/pillboxes/bases/starts quartet loaded from a
 *  .map file or compressed buffer. Used by the map
 *  chooser preview popup, the minimap renderer's entry
 *  points, and other UI surfaces that need to inspect
 *  a map without spinning up a full sim.
 *********************************************************/

#ifndef CLIENT_MAPPREVIEW_H
#define CLIENT_MAPPREVIEW_H

#include "global.h"
#include "alliance_enums.h" /* not strictly required but kept for API parity */

/* Opaque handle. Definition lives in client_mappreview.c. */
typedef struct MapPreview MapPreview;

/* Forward decls — the struct accessors return pointers into types.h
 * structs without forcing every caller to include bolo_map.h etc. */
struct mapObj;
struct pillsObj;
struct basesObj;
struct startsObj;

/*********************************************************
 *NAME:          clientMapPreviewLoadFromBuffer
 *PURPOSE:
 *  Allocates a MapPreview and decompresses a network
 *  map buffer into it. Returns NULL on failure (the
 *  partial allocation is freed before returning).
 *********************************************************/
MapPreview *clientMapPreviewLoadFromBuffer(const BYTE *data, int len);

/*********************************************************
 *NAME:          clientMapPreviewLoadFromFile
 *PURPOSE:
 *  Allocates a MapPreview and reads a .map file into it.
 *  Returns NULL on failure.
 *********************************************************/
MapPreview *clientMapPreviewLoadFromFile(const char *path);

/*********************************************************
 *NAME:          clientMapPreviewLoadFromMapBytes
 *PURPOSE:
 *  Allocates a MapPreview and parses an on-disk .map file
 *  image held in memory (e.g. a WinBolo.net download kept
 *  in RAM) into it. No temp file is written. Returns NULL
 *  on failure (the partial allocation is freed first).
 *********************************************************/
MapPreview *clientMapPreviewLoadFromMapBytes(const BYTE *data, int len);

/*********************************************************
 *NAME:          clientMapConvertFileToCompressed
 *PURPOSE:
 *  Parses an on-disk .map file image held in memory and
 *  re-serializes it to the runtime compressed map format
 *  (the format clientMapPreviewLoadFromBuffer and the map
 *  preview popup consume). Returns a malloc'd buffer the
 *  caller must free, writing its length to *outLen, or
 *  NULL on parse failure. No temp file is written.
 *********************************************************/
BYTE *clientMapConvertFileToCompressed(const BYTE *fileData, int fileLen, int *outLen);

/*********************************************************
 *NAME:          clientMapPreviewWrap
 *PURPOSE:
 *  Allocates a MapPreview that wraps caller-owned
 *  T-internal substructs without taking ownership of the
 *  inner data. The returned handle should be released with
 *  clientMapPreviewDestroy, which detects the wrap and
 *  frees only the wrapper itself, not the wrapped pointers.
 *********************************************************/
MapPreview *clientMapPreviewWrap(struct mapObj   *mp,
                                 struct pillsObj *pb,
                                 struct basesObj *bs,
                                 struct startsObj *ss);

/*********************************************************
 *NAME:          clientMapPreviewDestroy
 *PURPOSE:
 *  Tears down the four owned substructs and frees the
 *  MapPreview. NULL-safe.
 *********************************************************/
void clientMapPreviewDestroy(MapPreview *mp);

/* --- Per-position queries ----------------------------- */
BYTE         clientMapPreviewGetTerrain(const MapPreview *mp, BYTE x, BYTE y);
bool         clientMapPreviewIsMine(const MapPreview *mp, BYTE x, BYTE y);
bool         clientMapPreviewIsPill(const MapPreview *mp, BYTE x, BYTE y);
BYTE         clientMapPreviewGetPillArmourAt(const MapPreview *mp, BYTE x, BYTE y);
bool         clientMapPreviewIsBase(const MapPreview *mp, BYTE x, BYTE y);
bool         clientMapPreviewIsStart(const MapPreview *mp, BYTE x, BYTE y);

/* --- Counts ------------------------------------------- */
BYTE         clientMapPreviewGetPillCount(const MapPreview *mp);
BYTE         clientMapPreviewGetBaseCount(const MapPreview *mp);
BYTE         clientMapPreviewGetStartCount(const MapPreview *mp);

/* --- Per-index getters (un-bundled out-params) -------- */
/* Each returns false if i is out of range. */
bool clientMapPreviewGetPill(const MapPreview *mp, BYTE i,
                             BYTE *x, BYTE *y, BYTE *owner, BYTE *armour);
bool clientMapPreviewGetBase(const MapPreview *mp, BYTE i,
                             BYTE *x, BYTE *y, BYTE *owner);
bool clientMapPreviewGetStart(const MapPreview *mp, BYTE i,
                              BYTE *x, BYTE *y, BYTE *dir);

/* --- Substruct accessors -----------------------------
 * Const pointers into the four owned T4 structs. Used by
 * minimap_render.c's entry points to feed the rendering
 * helpers without re-walking the data through query
 * wrappers. Lifetime: tied to the MapPreview handle. */
const struct mapObj   *clientMapPreviewMap(const MapPreview *mp);
const struct pillsObj *clientMapPreviewPills(const MapPreview *mp);
const struct basesObj *clientMapPreviewBases(const MapPreview *mp);
const struct startsObj *clientMapPreviewStarts(const MapPreview *mp);

#endif /* CLIENT_MAPPREVIEW_H */
