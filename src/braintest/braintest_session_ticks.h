/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          braintest_session_ticks.h
 *PURPOSE:
 *  First and last recorded tick of each session in the Load Session
 *  list, so the list shows GAME time (not wall-clock time, which an
 *  -asap server and brainrec_split parts both get wrong).
 *
 *  Where the ticks come from, cheapest first:
 *    1. this process's table (keyed on .btr size + mtime);
 *    2. brainrec_ticks.txt beside the .btr (the recorder and
 *       brainrec_split write it at close; one small text read);
 *    3. the cache file, which keeps every range found by step 4;
 *    4. a walk of the whole .btr on one background thread (about 5 s
 *       for a 450 MB part). The row shows "..." until it is done.
 *  So each recording is read in full at most once, and never on the
 *  main thread.
 *********************************************************/
#ifndef BRAINTEST_SESSION_TICKS_H
#define BRAINTEST_SESSION_TICKS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SESSION_TICKS_PENDING = 0,   /* the background walk has not finished */
    SESSION_TICKS_HAVE,          /* *first and *last are set */
    SESSION_TICKS_FAILED         /* the file has no readable frames */
} SessionTicksState;

/* Set the cache file (BrainTest uses "braintest_session_ticks.cache" in its
 * working dir) and read it. Call once before the first lookup. A missing
 * file is an empty cache. */
void sessionTicksInit(const char *cachePath);

/* Look up the tick range of the .btr at btrPath (inside dir), whose size
 * and mtime the caller already has from a stat. Never blocks on a full
 * read: a range not known yet is queued for the background walk and this
 * returns SESSION_TICKS_PENDING. Main thread only. */
SessionTicksState sessionTicksLookup(const char *dir, const char *btrPath,
                                     int64_t size, int64_t mtime,
                                     uint32_t *first, uint32_t *last);

/* Game clock seconds for a tick range: the clock at the first and last
 * tick, and the length between them (engine ticks run at 100 per second;
 * each value rounds down to a whole second). */
void sessionTicksSeconds(uint32_t first, uint32_t last,
                         int *startSec, int *endSec, int *lengthSec);

/* Stop the background walk (a walk in progress stops at its next read)
 * and wait for the thread. Safe to call when nothing was started. */
void sessionTicksShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_SESSION_TICKS_H */
