/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_io.h
 *PURPOSE:
 *  The container a brainrec.btr byte stream is stored in. The frame
 *  layout (brain_record.h) is not changed by this file; only the
 *  compression around it.
 *
 *  New files are zstd:
 *    1. A skippable frame (magic 0x184D2A50) whose 8-byte payload is
 *       BRAINREC_ZSTD_MARKER. It tells a reader that this is the zstd
 *       .btr format. Old builds open the file with gzopen, read these
 *       bytes as the header, see the wrong magic and refuse the file.
 *    2. One zstd frame for the header + legend, then one independent
 *       zstd frame per "block". The writer starts a new block at each
 *       periodic map keyframe, so a reader can start decoding at any
 *       block. Inside a block the long window (windowLog 25, 32 MiB)
 *       reaches back over the previous recording frames.
 *    3. At a clean close, a seek table in the zstd seekable format
 *       (skippable frame, magic 0x184D2A5E): one {compressed size,
 *       decompressed size} pair per zstd frame, then the footer
 *       {numFrames, descriptor, 0x8F92EAB1}. A file cut off by a hard
 *       kill has no table; it still reads in order up to the last flush.
 *
 *  Old files are gzip. The reader detects the format from the first
 *  bytes, so every reader reads both.
 *
 *  This is the only place that knows the container. The recorder,
 *  BrainTest, the splitter and the unit tests use these functions.
 *********************************************************/
#ifndef BRAIN_RECORD_IO_H
#define BRAIN_RECORD_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The payload of the leading skippable frame. */
#define BRAINREC_ZSTD_MARKER      "WBBTRZS1"
#define BRAINREC_ZSTD_MARKER_LEN  8

/* zstd settings, chosen from the measurement on two 300 MB samples of a
 * 4-bot recording (level 3 + windowLog 25: ratio 158-216 against 4.7-5.0
 * for gzip level 1, and about 2600 MB/s against about 145 MB/s). */
#define BRAINREC_ZSTD_LEVEL       3
#define BRAINREC_ZSTD_WINDOW_LOG  25

/* A block never grows past this many decompressed bytes, so each seek
 * table entry fits its 32-bit fields. A new block starts early if needed. */
#define BRAINREC_ZSTD_BLOCK_MAX   (1u << 30)

typedef enum {
    BRAINREC_FMT_NONE = 0,
    BRAINREC_FMT_GZIP = 1,   /* the old format (also reads plain bytes) */
    BRAINREC_FMT_ZSTD = 2,
} BrainRecFormat;

/* ---- writer --------------------------------------------------------- */
typedef struct BrainRecOut BrainRecOut;

/* Create (truncate) path and write the format marker. NULL on error. */
BrainRecOut *brainRecOutOpen(const char *path);

/* Append n decompressed bytes. Returns false after any write error. */
bool brainRecOutWrite(BrainRecOut *o, const void *p, size_t n);

/* End the current zstd frame. The next byte written starts a new,
 * independent frame (a seek point). A no-op when nothing was written
 * since the last block start. */
bool brainRecOutNewBlock(BrainRecOut *o);

/* Push all bytes written so far to the file (zstd flush + fflush), so a
 * hard kill after this keeps them. */
bool brainRecOutFlush(BrainRecOut *o);

/* End the last frame, write the seek table and close. Frees o. Returns
 * false if any write failed. NULL is allowed. */
bool brainRecOutClose(BrainRecOut *o);

/* Totals so far: bytes given to Write and bytes put in the file. */
uint64_t brainRecOutBytesIn(const BrainRecOut *o);
uint64_t brainRecOutBytesOut(const BrainRecOut *o);

/* ---- reader --------------------------------------------------------- */
typedef struct BrainRecIn BrainRecIn;

/* Open path and detect the format from its first bytes. NULL on error. */
BrainRecIn *brainRecInOpen(const char *path);

/* Read up to n decompressed bytes. Returns the count read; less than n
 * only at the end of the data or on an error (see brainRecInError). */
size_t brainRecInRead(BrainRecIn *in, void *dst, size_t n);

/* Move forward n decompressed bytes. With a seek table this jumps over
 * whole blocks without decoding them. Returns false if the data ends
 * first. */
bool brainRecInSkip(BrainRecIn *in, uint64_t n);

/* Decompressed bytes consumed so far. */
uint64_t brainRecInTell(const BrainRecIn *in);

/* Compressed bytes read from the file so far (for a progress bar), and
 * the file size. */
uint64_t brainRecInCompressedPos(const BrainRecIn *in);
uint64_t brainRecInFileSize(const BrainRecIn *in);

/* True after a decode or read error. */
bool brainRecInError(const BrainRecIn *in);

/* True if the data ended inside a compressed frame (a file cut off by a
 * hard kill). Valid after a read came back short. */
bool brainRecInTruncated(const BrainRecIn *in);

BrainRecFormat brainRecInFormat(const BrainRecIn *in);

/* Number of zstd frames in the seek table, 0 if the file has none. */
uint32_t brainRecInSeekEntries(const BrainRecIn *in);

void brainRecInClose(BrainRecIn *in);

/* Detect the format of the first bytes of a file (at least 4 bytes). */
BrainRecFormat brainRecDetectFormat(const unsigned char *head, size_t n);

/* ---- text logs (print2_bot*.log) ------------------------------------ */
/* A finished print2 log is stored as <name>.zst: one plain zstd stream
 * (the zstd tool reads it), with long-distance matching and a 128 MiB
 * window, because print2 repeats lines across the whole game. Older
 * builds wrote <name>.gz. brainRecInOpen reads all three forms (plain,
 * gzip, zstd), so it is also the log reader; a reader that knows only
 * the plain name looks for <name>.zst and <name>.gz too.
 *
 * The settings come from a measurement on a 150 MB print2 log of a real
 * 2v2 game, on one core of a Ryzen 5 5600 (gzip -6 made 10.6 MB):
 *   level 15                          5.67 MB   3.9 s
 *   level 17                          5.16 MB  16.5 s
 *   level 19                          4.10 MB  60.0 s
 *   level 19, btopt, searchLog 1,
 *     minMatch 6 (these)              4.37 MB   7.8 s
 * Reading it back takes about 0.1 s. */
#define BRAINLOG_ZSTD_LEVEL       19
#define BRAINLOG_ZSTD_SEARCH_LOG  1
#define BRAINLOG_ZSTD_MIN_MATCH   6
#define BRAINLOG_ZSTD_WINDOW_LOG  27
#define BRAINLOG_ZSTD_EXT         ".zst"
#define BRAINLOG_GZIP_EXT         ".gz"

typedef enum {
    BRAINLOG_PLAIN = 0,
    BRAINLOG_GZIP  = 1,
    BRAINLOG_ZSTD  = 2,
} BrainLogFormat;

/* The form a log file name says: ".zst", ".gz" or anything else. */
BrainLogFormat brainLogFormatOfName(const char *name);

typedef struct BrainLogOut BrainLogOut;

/* Create (truncate) path for a log of the given form. srcSize is the
 * size of the text when known (0 when not): zstd then uses no larger a
 * window than the text needs. NULL on error. */
BrainLogOut *brainLogOutOpen(const char *path, BrainLogFormat fmt,
                             uint64_t srcSize);
bool brainLogOutWrite(BrainLogOut *o, const void *p, size_t n);
/* End the stream and close. Frees o. False if any write failed. */
bool brainLogOutClose(BrainLogOut *o);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_RECORD_IO_H */
