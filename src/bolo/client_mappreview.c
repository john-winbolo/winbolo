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
 *Filename:      client_mappreview.c
 *********************************************************/

#include <stdlib.h>

#include "client_mappreview.h"
#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"

struct MapPreview {
  map         mp;
  pillboxes   pb;
  bases       bs;
  starts      ss;
  bool        owns;  /* true = destroy frees inner; false = wrapper only */
};

static MapPreview *clientMapPreviewAlloc(void) {
  MapPreview *mp = (MapPreview *)malloc(sizeof(MapPreview));
  if (mp == NULL) return NULL;
  mp->mp = NULL;
  mp->pb = NULL;
  mp->bs = NULL;
  mp->ss = NULL;
  mp->owns = true;
  mapCreate(&mp->mp);
  pillsCreate(&mp->pb);
  basesCreate(&mp->bs);
  startsCreate(&mp->ss);
  return mp;
}

MapPreview *clientMapPreviewWrap(struct mapObj   *mp,
                                 struct pillsObj *pb,
                                 struct basesObj *bs,
                                 struct startsObj *ss) {
  MapPreview *view = (MapPreview *)malloc(sizeof(MapPreview));
  if (view == NULL) return NULL;
  view->mp   = (map)mp;
  view->pb   = (pillboxes)pb;
  view->bs   = (bases)bs;
  view->ss   = (starts)ss;
  view->owns = false;
  return view;
}

MapPreview *clientMapPreviewLoadFromBuffer(const BYTE *data, int len) {
  MapPreview *mp;
  if (data == NULL || len <= 0) return NULL;
  mp = clientMapPreviewAlloc();
  if (mp == NULL) return NULL;
  if (!mapLoadCompressedMap(&mp->mp, &mp->pb, &mp->bs, &mp->ss, (BYTE *)data, len)) {
    clientMapPreviewDestroy(mp);
    return NULL;
  }
  return mp;
}

MapPreview *clientMapPreviewLoadFromFile(const char *path) {
  MapPreview *mp;
  if (path == NULL || path[0] == '\0') return NULL;
  mp = clientMapPreviewAlloc();
  if (mp == NULL) return NULL;
  if (mapRead((char *)path, &mp->mp, &mp->pb, &mp->bs, &mp->ss) != TRUE) {
    clientMapPreviewDestroy(mp);
    return NULL;
  }
  return mp;
}

MapPreview *clientMapPreviewLoadFromMapBytes(const BYTE *data, int len) {
  MapPreview *mp;
  if (data == NULL || len <= 0) return NULL;
  mp = clientMapPreviewAlloc();
  if (mp == NULL) return NULL;
  if (!mapReadFromMemory(data, len, &mp->mp, &mp->pb, &mp->bs, &mp->ss)) {
    clientMapPreviewDestroy(mp);
    return NULL;
  }
  return mp;
}

/* Worst-case serialized size of a runtime compressed map. Mirrors the
 * 256 KiB scratch the random-map generator hands mapSaveCompressedMap. */
#define CLIENT_MAP_COMPRESSED_MAX (256 * 1024)

BYTE *clientMapConvertFileToCompressed(const BYTE *fileData, int fileLen, int *outLen) {
  MapPreview *mp;
  BYTE *out;
  int len;

  if (outLen != NULL) *outLen = 0;
  if (fileData == NULL || fileLen <= 0) return NULL;

  mp = clientMapPreviewLoadFromMapBytes(fileData, fileLen);
  if (mp == NULL) return NULL;

  out = (BYTE *)malloc(CLIENT_MAP_COMPRESSED_MAX);
  if (out == NULL) {
    clientMapPreviewDestroy(mp);
    return NULL;
  }
  len = mapSaveCompressedMap(&mp->mp, &mp->pb, &mp->bs, &mp->ss, out,
                             CLIENT_MAP_COMPRESSED_MAX);
  clientMapPreviewDestroy(mp);
  if (len <= 0) {
    free(out);
    return NULL;
  }
  if (outLen != NULL) *outLen = len;
  return out;
}

void clientMapPreviewDestroy(MapPreview *mp) {
  if (mp == NULL) return;
  if (mp->owns) {
    mapDestroy(&mp->mp);
    pillsDestroy(&mp->pb);
    basesDestroy(&mp->bs);
    startsDestroy(&mp->ss);
  }
  free(mp);
}

BYTE clientMapPreviewGetTerrain(const MapPreview *mp, BYTE x, BYTE y) {
  return mapGetPos(&((MapPreview *)mp)->mp, x, y);
}

bool clientMapPreviewIsMine(const MapPreview *mp, BYTE x, BYTE y) {
  return mapIsMine(&((MapPreview *)mp)->mp, x, y);
}

bool clientMapPreviewIsPill(const MapPreview *mp, BYTE x, BYTE y) {
  return pillsExistPos(&((MapPreview *)mp)->pb, x, y);
}

BYTE clientMapPreviewGetPillArmourAt(const MapPreview *mp, BYTE x, BYTE y) {
  return pillsGetArmourPos(&((MapPreview *)mp)->pb, x, y);
}

bool clientMapPreviewIsBase(const MapPreview *mp, BYTE x, BYTE y) {
  return basesExistPos(&((MapPreview *)mp)->bs, x, y);
}

bool clientMapPreviewIsStart(const MapPreview *mp, BYTE x, BYTE y) {
  return startsExistPos(&((MapPreview *)mp)->ss, x, y);
}

BYTE clientMapPreviewGetPillCount(const MapPreview *mp) {
  return pillsGetNumPills(&((MapPreview *)mp)->pb);
}

BYTE clientMapPreviewGetBaseCount(const MapPreview *mp) {
  return basesGetNumBases(&((MapPreview *)mp)->bs);
}

BYTE clientMapPreviewGetStartCount(const MapPreview *mp) {
  return startsGetNumStarts(&((MapPreview *)mp)->ss);
}

bool clientMapPreviewGetPill(const MapPreview *mp, BYTE i,
                             BYTE *x, BYTE *y, BYTE *owner, BYTE *armour) {
  pillbox p;
  BYTE n = pillsGetNumPills(&((MapPreview *)mp)->pb);
  if (i == 0 || i > n) return false;
  pillsGetPill(&((MapPreview *)mp)->pb, &p, i);
  if (x)      *x      = p.x;
  if (y)      *y      = p.y;
  if (owner)  *owner  = p.owner;
  if (armour) *armour = p.armour;
  return true;
}

bool clientMapPreviewGetBase(const MapPreview *mp, BYTE i,
                             BYTE *x, BYTE *y, BYTE *owner) {
  base b;
  BYTE n = basesGetNumBases(&((MapPreview *)mp)->bs);
  if (i == 0 || i > n) return false;
  basesGetBase(&((MapPreview *)mp)->bs, &b, i);
  if (x)     *x     = b.x;
  if (y)     *y     = b.y;
  if (owner) *owner = b.owner;
  return true;
}

bool clientMapPreviewGetStart(const MapPreview *mp, BYTE i,
                              BYTE *x, BYTE *y, BYTE *dir) {
  start s;
  BYTE n = startsGetNumStarts(&((MapPreview *)mp)->ss);
  if (i == 0 || i > n) return false;
  startsGetStartStruct(&((MapPreview *)mp)->ss, &s, i);
  if (x)   *x   = s.x;
  if (y)   *y   = s.y;
  if (dir) *dir = startsConvertDir((BYTE)((s.dir < 16) ? s.dir : 0));
  return true;
}

const struct mapObj *clientMapPreviewMap(const MapPreview *mp) {
  return mp->mp;
}

const struct pillsObj *clientMapPreviewPills(const MapPreview *mp) {
  return mp->pb;
}

const struct basesObj *clientMapPreviewBases(const MapPreview *mp) {
  return mp->bs;
}

const struct startsObj *clientMapPreviewStarts(const MapPreview *mp) {
  return mp->ss;
}
