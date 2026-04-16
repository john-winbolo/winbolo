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

#include <stdlib.h>
#include "global.h"
#include "blocks.h"
#include "unzip.h"

/* Initial decompression buffer capacity (256 KB) */
#define LOG_INITIAL_CAPACITY (256 * 1024)

/* Chunk size to decompress at a time (64 KB) */
#define LOG_DECOMPRESS_CHUNK (64 * 1024)

/* Maximum decompressed log size (512 MB) */
#define LOG_MAX_SIZE (512 * 1024 * 1024)

static uint8_t *logData     = NULL;  /* Decompressed log data */
static size_t   logSize     = 0;     /* Bytes decompressed so far */
static size_t   logCapacity = 0;     /* Allocated capacity */
static size_t   logPosition = 0;     /* Current read position */
static bool     logEOF      = FALSE; /* No more data to decompress */
static BYTE     blockKey    = 0;     /* XOR decryption key */

static unzFile  logFile = NULL;

/* Grow logData to at least 'needed' bytes. Returns TRUE on success. */
static bool growBuffer(size_t needed) {
  if (needed > LOG_MAX_SIZE) return FALSE;
  if (needed <= logCapacity) return TRUE;
  size_t newCap = logCapacity ? logCapacity : LOG_INITIAL_CAPACITY;
  while (newCap < needed) newCap *= 2;
  if (newCap > LOG_MAX_SIZE) newCap = LOG_MAX_SIZE;
  uint8_t *newData = realloc(logData, newCap);
  if (newData == NULL) return FALSE;
  logData = newData;
  logCapacity = newCap;
  return TRUE;
}

/* Decompress from the zip stream until logSize >= target or EOF. */
static void decompressUpTo(size_t target) {
  while (logSize < target && !logEOF) {
    if (!growBuffer(logSize + LOG_DECOMPRESS_CHUNK)) break;
    int got = unzReadCurrentFile(logFile, logData + logSize, LOG_DECOMPRESS_CHUNK);
    if (got <= 0) {
      logEOF = TRUE;
      break;
    }
    logSize += (size_t)got;
  }
}

/* Size parameter is ignored -- we buffer the whole file. */
bool lv_blocksCreate(char *fileName, int size) {
  (void)size;
  blockKey    = 0;
  logData     = NULL;
  logSize     = 0;
  logCapacity = 0;
  logPosition = 0;
  logEOF      = FALSE;
  logFile     = NULL;

  logFile = unzOpen(fileName);
  if (logFile == NULL) return FALSE;

  if (unzLocateFile(logFile, "log.dat", 0) != UNZ_OK) {
    unzClose(logFile);
    logFile = NULL;
    return FALSE;
  }

  if (unzOpenCurrentFile(logFile) != UNZ_OK) {
    unzClose(logFile);
    logFile = NULL;
    return FALSE;
  }

  return TRUE;
}

void lv_blocksDestroy() {
  if (logFile != NULL) {
    unzCloseCurrentFile(logFile);
    unzClose(logFile);
    logFile = NULL;
  }
  free(logData);
  logData     = NULL;
  logSize     = 0;
  logCapacity = 0;
  logPosition = 0;
  logEOF      = FALSE;
}

bool lv_blocksIsEOF() {
  if (logPosition < logSize) return FALSE;
  if (logEOF) return TRUE;
  if (logFile != NULL) return unzeof(logFile);
  return TRUE;
}

int lv_blocksReadBytes(BYTE *buff, int len) {
  if (logFile == NULL || len <= 0) return -1;

  decompressUpTo(logPosition + (size_t)len);

  size_t available = logSize > logPosition ? logSize - logPosition : 0;
  size_t toRead = (size_t)len < available ? (size_t)len : available;
  if (toRead == 0) return 0;

  memcpy(buff, logData + logPosition, toRead);

  for (size_t i = 0; i < toRead; i++) {
    buff[i] ^= blockKey;
  }

  logPosition += toRead;
  return (int)toRead;
}

void lv_logDecompressAll(void) {
  if (logFile == NULL) return;
  while (!logEOF && logSize < LOG_MAX_SIZE) {
    decompressUpTo(logSize + LOG_DECOMPRESS_CHUNK);
  }
}

size_t lv_logGetTotalSize(void) {
  return logSize;
}

void lv_logSetPosition(size_t pos) {
  if (pos > logSize) {
    decompressUpTo(pos);
  }
  logPosition = pos <= logSize ? pos : logSize;
}

size_t lv_logGetCurrentPosition() {
  return logPosition;
}

BYTE lv_blocksGetKey() {
  return blockKey;
}

void lv_blocksSetKey(BYTE key) {
  blockKey = key;
}
