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

#include <stdlib.h>
#include <string.h>
#include "lv_global.h"
#include "blocks.h"
#include "lv_attribution.h"
#include "unzip.h"
#include "ioapi.h"

/* Initial decompression buffer capacity (256 KB) */
#define LOG_INITIAL_CAPACITY (256 * 1024)

/* Chunk size to decompress at a time (64 KB) */
#define LOG_DECOMPRESS_CHUNK (64 * 1024)

/* Maximum decompressed log size (512 MB) */
#define LOG_MAX_SIZE (512 * 1024 * 1024)

static uint8_t *ownedZipData = NULL;  /* Zip buffer owned by us (from memory load) */

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
  if (logFile == NULL) return;  /* no-op in append-fed stream mode */
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

/* Locate and parse the optional attribution-track member into memory. The
 * member is optional: an old .wbv without it simply leaves the parsed track
 * absent. Reads the whole member into a heap buffer with the same chunked
 * grow-and-read loop used for log.dat, then hands it to the parser (which
 * takes its own copy). Any failure clears the track and returns without
 * disturbing the log load. Must be called with no zip current-file open, and
 * leaves none open. */
static void lv_blocksLoadAttributionTrack(unzFile zf) {
  lvAttributionClear();
  if (zf == NULL) return;
  if (unzLocateFile(zf, ATTRIBUTION_TRACK_MEMBER, 0) != UNZ_OK) return;  /* old .wbv: no track */
  if (unzOpenCurrentFile(zf) != UNZ_OK) return;

  uint8_t *buf = NULL;
  size_t   size = 0, cap = 0;
  bool     ok = TRUE;
  for (;;) {
    if (size + LOG_DECOMPRESS_CHUNK > cap) {
      size_t newCap = cap ? cap * 2 : LOG_DECOMPRESS_CHUNK;
      while (newCap < size + LOG_DECOMPRESS_CHUNK) newCap *= 2;
      uint8_t *nb = (uint8_t *)realloc(buf, newCap);
      if (nb == NULL) { ok = FALSE; break; }
      buf = nb;
      cap = newCap;
    }
    int got = unzReadCurrentFile(zf, buf + size, LOG_DECOMPRESS_CHUNK);
    if (got < 0) { ok = FALSE; break; }
    if (got == 0) break;
    size += (size_t)got;
  }
  unzCloseCurrentFile(zf);
  if (ok && buf != NULL && size > 0) {
    lvAttributionParseMember(buf, size);  /* parser takes its own copy */
  }
  free(buf);
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
  free(ownedZipData);
  ownedZipData = NULL;

  logFile = unzOpen(fileName);
  if (logFile == NULL) return FALSE;

  if (unzLocateFile(logFile, "log.dat", 0) != UNZ_OK) {
    unzClose(logFile);
    logFile = NULL;
    return FALSE;
  }

  /* Optional attribution-track member. Load it before opening log.dat's
   * stream: locating another member moves the zip cursor, so re-locate
   * log.dat afterward and open it last for the lazy streaming reader. */
  lv_blocksLoadAttributionTrack(logFile);

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

/* ---- In-memory zip I/O callbacks for minizip ---- */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} MemReader;

static voidpf ZCALLBACK mem_open(voidpf opaque, const char *filename, int mode) {
    (void)filename; (void)mode;
    MemReader *mr = (MemReader *)opaque;
    mr->pos = 0;
    return mr; /* return non-NULL to indicate success */
}

static uLong ZCALLBACK mem_read(voidpf opaque, voidpf stream, void *buf, uLong size) {
    (void)opaque;
    MemReader *mr = (MemReader *)stream;
    size_t avail = mr->size > mr->pos ? mr->size - mr->pos : 0;
    size_t toRead = (size_t)size < avail ? (size_t)size : avail;
    if (toRead > 0) {
        memcpy(buf, mr->data + mr->pos, toRead);
        mr->pos += toRead;
    }
    return (uLong)toRead;
}

static uLong ZCALLBACK mem_write(voidpf opaque, voidpf stream, const void *buf, uLong size) {
    (void)opaque; (void)stream; (void)buf; (void)size;
    return 0; /* read-only */
}

static long ZCALLBACK mem_tell(voidpf opaque, voidpf stream) {
    (void)opaque;
    MemReader *mr = (MemReader *)stream;
    return (long)mr->pos;
}

static long ZCALLBACK mem_seek(voidpf opaque, voidpf stream, uLong offset, int origin) {
    (void)opaque;
    MemReader *mr = (MemReader *)stream;
    size_t newPos;
    switch (origin) {
    case ZLIB_FILEFUNC_SEEK_SET: newPos = (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_CUR: newPos = mr->pos + (size_t)offset; break;
    case ZLIB_FILEFUNC_SEEK_END: newPos = mr->size + (size_t)offset; break;
    default: return -1;
    }
    if (newPos > mr->size) return -1;
    mr->pos = newPos;
    return 0;
}

static int ZCALLBACK mem_close(voidpf opaque, voidpf stream) {
    (void)opaque; (void)stream;
    return 0;
}

static int ZCALLBACK mem_error(voidpf opaque, voidpf stream) {
    (void)opaque; (void)stream;
    return 0;
}

static MemReader s_memReader;

bool lv_blocksCreateFromMemory(uint8_t *zipData, size_t zipLen) {
  blockKey    = 0;
  logData     = NULL;
  logSize     = 0;
  logCapacity = 0;
  logPosition = 0;
  logEOF      = FALSE;
  logFile     = NULL;

  /* Take ownership of the buffer */
  ownedZipData = zipData;

  /* Set up memory I/O for minizip */
  s_memReader.data = zipData;
  s_memReader.size = zipLen;
  s_memReader.pos  = 0;

  zlib_filefunc_def filefuncs;
  filefuncs.zopen_file  = mem_open;
  filefuncs.zread_file  = mem_read;
  filefuncs.zwrite_file = mem_write;
  filefuncs.ztell_file  = mem_tell;
  filefuncs.zseek_file  = mem_seek;
  filefuncs.zclose_file = mem_close;
  filefuncs.zerror_file = mem_error;
  filefuncs.opaque      = &s_memReader;

  logFile = unzOpen2(NULL, &filefuncs);
  if (logFile == NULL) return FALSE;

  if (unzLocateFile(logFile, "log.dat", 0) != UNZ_OK) {
    unzClose(logFile);
    logFile = NULL;
    return FALSE;
  }

  /* Optional attribution-track member. Load it before opening log.dat's
   * stream: locating another member moves the zip cursor, so re-locate
   * log.dat afterward and open it last for the lazy streaming reader. */
  lv_blocksLoadAttributionTrack(logFile);

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

/* ---- No-zip append-fed stream source ----
 *
 * Drives the same logData/logSize/logPosition/logEOF reader state as the zip
 * path, but with plaintext bytes pushed in via lv_blocksAppendBytes rather
 * than pulled from a zip. lv_blocksReadBytes / lv_logSetPosition work
 * unchanged: decompressUpTo is a no-op without a zip (logFile == NULL) and the
 * read de-XOR uses blockKey, which is 0 for the v2 stream. */

/* Reset the reader for a no-zip append-fed stream session. Defensive: the
 * caller need not have called lv_blocksDestroy first, so any owned heap
 * buffers are freed here. A live zip handle is assumed already closed, as with
 * the other create entry points. */
void lv_blocksBeginStream(void) {
  free(logData);
  logData     = NULL;
  free(ownedZipData);
  ownedZipData = NULL;
  logSize     = 0;
  logCapacity = 0;
  logPosition = 0;
  logEOF      = FALSE;
  blockKey    = 0;
  logFile     = NULL;
}

/* Append plaintext bytes to the stream buffer. No XOR on write; reads de-XOR
 * with blockKey (0 for the v2 stream). Returns FALSE only on a NULL buffer or
 * an allocation failure; a zero-length append is a no-op success. */
bool lv_blocksAppendBytes(const uint8_t *data, size_t len) {
  if (len == 0) return TRUE;
  if (data == NULL) return FALSE;
  if (!growBuffer(logSize + len)) return FALSE;
  memcpy(logData + logSize, data, len);
  logSize += len;
  return TRUE;
}

/* Mark the appended stream complete so lv_blocksIsEOF reports EOF once the
 * read cursor catches up to logSize. */
void lv_blocksSetStreamEOF(void) {
  logEOF = TRUE;
}

void lv_blocksDestroy() {
  lvAttributionClear();  /* drop this log's parsed track so a later load can't see it */
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
  free(ownedZipData);
  ownedZipData = NULL;
}

bool lv_blocksIsEOF() {
  if (logPosition < logSize) return FALSE;
  if (logEOF) return TRUE;
  if (logFile != NULL) return unzeof(logFile);
  return TRUE;
}

int lv_blocksReadBytes(BYTE *buff, int len) {
  if (len <= 0) return -1;

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
