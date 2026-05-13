/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_symmetry.h
 * Purpose:
 *   Symmetry operations for the map editor. Pure logic
 *   functions that transform terrain and objects within
 *   a rectangular region.
 *********************************************************/

#ifndef MAPEDITOR_SYMMETRY_H
#define MAPEDITOR_SYMMETRY_H

#include <stdbool.h>
#include "global.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mirror terrain and objects horizontally (left<->right) within the given rect. */
void symmetryMirrorH(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                     int x1, int y1, int x2, int y2);

/* Mirror terrain and objects vertically (top<->bottom) within the given rect. */
void symmetryMirrorV(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                     int x1, int y1, int x2, int y2);

/* Rotate terrain and objects 90 degrees clockwise within the given rect.
 * A W×H region becomes H×W. Returns false if the rotated region would exceed map bounds. */
bool symmetryRotate90(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                      int x1, int y1, int x2, int y2);

/* Rotate terrain and objects 180 degrees within the given rect. */
void symmetryRotate180(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                       int x1, int y1, int x2, int y2);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_SYMMETRY_H */
