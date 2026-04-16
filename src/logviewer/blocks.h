/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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

#ifndef __BLOCKS_H
#define __BLOCKS_H

#include "global.h"

#define logIsEOF() lv_blocksIsEOF()
#define logReadBytes(X, Y) lv_blocksReadBytes(X, Y)

/* size parameter is ignored (kept for API compatibility) */
bool lv_blocksCreate(char *fileName, int size);
void lv_blocksDestroy();
int lv_blocksReadBytes(BYTE *buff, int len);
bool lv_blocksIsEOF();
size_t lv_logGetCurrentPosition();
void lv_logSetPosition(size_t pos);
void lv_logDecompressAll();
size_t lv_logGetTotalSize();
BYTE lv_blocksGetKey();
void lv_blocksSetKey(BYTE key);

#endif /* __BLOCKS_H */
