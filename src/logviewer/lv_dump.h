/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Text dump of a recording, with no window.
 *
 *   "Log Viewer.exe" -dump <log.wbv> [-tick N | -ms M] [-span S] [-round R]
 *                    [-out <file>]
 *
 * Decodes the log with the viewer's own decoder and writes what the recording
 * says as plain text, one record per line, key=value. With -tick or -ms it
 * writes every step of playback from S ticks before to S ticks after: the
 * raw log records, the event lines, and the tanks, builders, shells,
 * pillboxes and bases. With neither it writes the whole log's event lines.
 *
 * The tick is the number the Controls panel shows: the server's game tick
 * when the recording states it, else playback ms / 20. The server's tick
 * starts again each round, so -round picks one (1 is the first); without it
 * the first round that holds the tick is used.
 *
 * Only plain C types here, so a test on either side of the sim / viewer
 * split can include it.
 */

#ifndef LV_DUMP_H
#define LV_DUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_DUMP_DEFAULT_SPAN 100u

/* What lvDumpRun and lvDumpMain return. */
#define LV_DUMP_OK         0
#define LV_DUMP_ERR_ARGS   1   /* the command line was not understood */
#define LV_DUMP_ERR_OPEN   2   /* the log could not be opened or decoded */
#define LV_DUMP_ERR_TICK   3   /* the log does not hold that tick, ms or round */
#define LV_DUMP_ERR_WRITE  4   /* the output file could not be written */

typedef struct {
  const char *logPath;   /* the .wbv to read */
  const char *outPath;   /* the text file to write; NULL is <logPath>.dump.txt */
  int         haveTick;  /* tick is set */
  uint32_t    tick;
  int         haveMs;    /* ms is set (ignored when haveTick is) */
  uint32_t    ms;
  uint32_t    span;      /* ticks before and after */
  int         round;     /* 1 is the first; 0 lets the dump choose */
  int         toStdout;  /* also copy the text to stdout */
} LvDumpOptions;

/* Write the dump. Returns LV_DUMP_OK or one of the errors; an error after the
 * output file was opened is also written into that file, as a line starting
 * "error". */
int lvDumpRun(const LvDumpOptions *options);

/* Whether the command line asks for a dump: its first argument is -dump. */
int lvDumpWanted(int argc, char **argv);

/* Read the command line, run the dump and return the process exit code. */
int lvDumpMain(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* LV_DUMP_H */
