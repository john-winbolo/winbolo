/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_symmetry.c
 * Purpose:
 *   Symmetry operations for the map editor. Transforms
 *   terrain and objects within a rectangular region.
 *********************************************************/

#include "mapeditor_symmetry.h"
#include <stdlib.h>
#include <string.h>

/* Transform a start direction for mirror horizontal (reflect across vertical axis). */
static BYTE dirMirrorH(BYTE dir) {
    return (BYTE)((16 - dir) % 16);
}

/* Transform a start direction for mirror vertical (reflect across horizontal axis). */
static BYTE dirMirrorV(BYTE dir) {
    return (BYTE)((8 - dir + 16) % 16);
}

/* Transform a start direction for 90 degrees CW rotation. */
static BYTE dirRotate90(BYTE dir) {
    return (BYTE)((dir + 4) % 16);
}

/* Transform a start direction for 180 degree rotation. */
static BYTE dirRotate180(BYTE dir) {
    return (BYTE)((dir + 8) % 16);
}

void symmetryMirrorH(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                     int x1, int y1, int x2, int y2) {
    /* Mirror terrain: swap columns symmetrically */
    for (int y = y1; y <= y2; y++) {
        for (int i = 0; i <= (x2 - x1) / 2; i++) {
            int lx = x1 + i;
            int rx = x2 - i;
            if (lx >= rx) break;
            BYTE tmp = mp->mapItem[lx][y];
            mp->mapItem[lx][y] = mp->mapItem[rx][y];
            mp->mapItem[rx][y] = tmp;
        }
    }

    /* Mirror bases */
    for (int i = 0; i < bs->numBases; i++) {
        base *b = &bs->item[i];
        if (b->x >= x1 && b->x <= x2 && b->y >= y1 && b->y <= y2) {
            b->x = (BYTE)(x1 + (x2 - b->x));
        }
    }

    /* Mirror pillboxes */
    for (int i = 0; i < pb->numPills; i++) {
        pillbox *p = &pb->item[i];
        if (p->x >= x1 && p->x <= x2 && p->y >= y1 && p->y <= y2) {
            p->x = (BYTE)(x1 + (x2 - p->x));
        }
    }

    /* Mirror starts (position + direction) */
    for (int i = 0; i < ss->numStarts; i++) {
        start *s = &ss->item[i];
        if (s->x >= x1 && s->x <= x2 && s->y >= y1 && s->y <= y2) {
            s->x = (BYTE)(x1 + (x2 - s->x));
            s->dir = dirMirrorH(s->dir);
        }
    }
}

void symmetryMirrorV(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                     int x1, int y1, int x2, int y2) {
    /* Mirror terrain: swap rows symmetrically */
    for (int x = x1; x <= x2; x++) {
        for (int j = 0; j <= (y2 - y1) / 2; j++) {
            int ty = y1 + j;
            int by = y2 - j;
            if (ty >= by) break;
            BYTE tmp = mp->mapItem[x][ty];
            mp->mapItem[x][ty] = mp->mapItem[x][by];
            mp->mapItem[x][by] = tmp;
        }
    }

    /* Mirror bases */
    for (int i = 0; i < bs->numBases; i++) {
        base *b = &bs->item[i];
        if (b->x >= x1 && b->x <= x2 && b->y >= y1 && b->y <= y2) {
            b->y = (BYTE)(y1 + (y2 - b->y));
        }
    }

    /* Mirror pillboxes */
    for (int i = 0; i < pb->numPills; i++) {
        pillbox *p = &pb->item[i];
        if (p->x >= x1 && p->x <= x2 && p->y >= y1 && p->y <= y2) {
            p->y = (BYTE)(y1 + (y2 - p->y));
        }
    }

    /* Mirror starts (position + direction) */
    for (int i = 0; i < ss->numStarts; i++) {
        start *s = &ss->item[i];
        if (s->x >= x1 && s->x <= x2 && s->y >= y1 && s->y <= y2) {
            s->y = (BYTE)(y1 + (y2 - s->y));
            s->dir = dirMirrorV(s->dir);
        }
    }
}

bool symmetryRotate90(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                      int x1, int y1, int x2, int y2) {
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;

    /* Check rotated region fits in map */
    int newX2 = x1 + h - 1;
    int newY2 = y1 + w - 1;
    if (newX2 > 255 || newY2 > 255) return false;

    /* Copy source region into temporary buffer */
    BYTE *temp = (BYTE *)malloc((size_t)w * (size_t)h);
    if (!temp) return false;

    for (int i = 0; i < w; i++)
        for (int j = 0; j < h; j++)
            temp[i * h + j] = mp->mapItem[x1 + i][y1 + j];

    /* Clear the union of old and new regions */
    int clearW = w > h ? w : h;
    int clearH = w > h ? w : h;
    for (int i = 0; i < clearW && (x1 + i) <= 255; i++)
        for (int j = 0; j < clearH && (y1 + j) <= 255; j++)
            mp->mapItem[x1 + i][y1 + j] = DEEP_SEA;

    /* Write rotated: 90° CW means src(sx,sy) -> dest(sy, w-1-sx)
       Dest region is (x1, y1) to (x1+h-1, y1+w-1) */
    for (int sx = 0; sx < w; sx++)
        for (int sy = 0; sy < h; sy++)
            mp->mapItem[x1 + sy][y1 + (w - 1 - sx)] = temp[sx * h + sy];

    free(temp);

    /* Rotate bases: (rx,ry) -> (ry, w-1-rx) */
    for (int i = 0; i < bs->numBases; i++) {
        base *b = &bs->item[i];
        if (b->x >= x1 && b->x <= x2 && b->y >= y1 && b->y <= y2) {
            int rx = b->x - x1;
            int ry = b->y - y1;
            b->x = (BYTE)(x1 + ry);
            b->y = (BYTE)(y1 + (w - 1 - rx));
        }
    }

    /* Rotate pillboxes */
    for (int i = 0; i < pb->numPills; i++) {
        pillbox *p = &pb->item[i];
        if (p->x >= x1 && p->x <= x2 && p->y >= y1 && p->y <= y2) {
            int rx = p->x - x1;
            int ry = p->y - y1;
            p->x = (BYTE)(x1 + ry);
            p->y = (BYTE)(y1 + (w - 1 - rx));
        }
    }

    /* Rotate starts (position + direction) */
    for (int i = 0; i < ss->numStarts; i++) {
        start *s = &ss->item[i];
        if (s->x >= x1 && s->x <= x2 && s->y >= y1 && s->y <= y2) {
            int rx = s->x - x1;
            int ry = s->y - y1;
            s->x = (BYTE)(x1 + ry);
            s->y = (BYTE)(y1 + (w - 1 - rx));
            s->dir = dirRotate90(s->dir);
        }
    }

    return true;
}

void symmetryRotate180(struct mapObj *mp, struct basesObj *bs, struct pillsObj *pb, struct startsObj *ss,
                       int x1, int y1, int x2, int y2) {
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int total = w * h;

    /* Swap terrain in pairs from opposite ends */
    for (int idx = 0; idx < total / 2; idx++) {
        int ix = idx % w;
        int iy = idx / w;
        int ax = x1 + ix;
        int ay = y1 + iy;
        int bx = x2 - ix;
        int by = y2 - iy;

        BYTE tmp = mp->mapItem[ax][ay];
        mp->mapItem[ax][ay] = mp->mapItem[bx][by];
        mp->mapItem[bx][by] = tmp;
    }

    /* Rotate bases */
    for (int i = 0; i < bs->numBases; i++) {
        base *b = &bs->item[i];
        if (b->x >= x1 && b->x <= x2 && b->y >= y1 && b->y <= y2) {
            b->x = (BYTE)(x1 + (x2 - b->x));
            b->y = (BYTE)(y1 + (y2 - b->y));
        }
    }

    /* Rotate pillboxes */
    for (int i = 0; i < pb->numPills; i++) {
        pillbox *p = &pb->item[i];
        if (p->x >= x1 && p->x <= x2 && p->y >= y1 && p->y <= y2) {
            p->x = (BYTE)(x1 + (x2 - p->x));
            p->y = (BYTE)(y1 + (y2 - p->y));
        }
    }

    /* Rotate starts (position + direction) */
    for (int i = 0; i < ss->numStarts; i++) {
        start *s = &ss->item[i];
        if (s->x >= x1 && s->x <= x2 && s->y >= y1 && s->y <= y2) {
            s->x = (BYTE)(x1 + (x2 - s->x));
            s->y = (BYTE)(y1 + (y2 - s->y));
            s->dir = dirRotate180(s->dir);
        }
    }
}
