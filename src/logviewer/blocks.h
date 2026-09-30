/*
 * $Id$
 *
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

#ifndef __BLOCKS_H
#define __BLOCKS_H

#include "lv_global.h"

#define logIsEOF() lv_blocksIsEOF()
#define logReadBytes(X, Y) lv_blocksReadBytes(X, Y)

/* size parameter is ignored (kept for API compatibility) */
bool lv_blocksCreate(char *fileName, int size);

/* Create from an in-memory zip buffer. Takes ownership of zipData
 * (freed in lv_blocksDestroy). */
bool lv_blocksCreateFromMemory(uint8_t *zipData, size_t zipLen);

/* No-zip append-fed stream source: BeginStream resets the reader, AppendBytes
 * feeds plaintext bytes, SetStreamEOF marks completion. Reads go through the
 * existing lv_blocksReadBytes/logReadBytes path (decompressUpTo no-ops without
 * a zip). AppendBytes returns FALSE on a NULL buffer or allocation failure. */
void lv_blocksBeginStream(void);
bool lv_blocksAppendBytes(const uint8_t *data, size_t len);
void lv_blocksSetStreamEOF(void);

void lv_blocksDestroy();
int lv_blocksReadBytes(BYTE *buff, int len);
bool lv_blocksIsEOF();
size_t lv_logGetCurrentPosition();
void lv_logSetPosition(size_t pos);
void lv_logDecompressAll();
size_t lv_logGetTotalSize();
BYTE lv_blocksGetKey();
void lv_blocksSetKey(BYTE key);

/* The open recording's scripts.json member, read when the zip was opened,
 * NUL-terminated, with its length in *len (the terminator not counted). NULL
 * with *len = 0 when the member is absent, over SCN_RECORD_TEXT_MAX or could
 * not be read, and always for a stream, which has no zip. Valid until the
 * next create, stream begin or destroy. */
const char *lv_blocksGetScriptsJson(size_t *len);

#endif /* __BLOCKS_H */
