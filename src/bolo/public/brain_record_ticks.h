/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_ticks.h
 *PURPOSE:
 *  The tick-range note written beside a brainrec.btr. BrainTest's
 *  session list shows each recording's game length, which is the
 *  first and last frame tick of the .btr. Those ticks are inside a
 *  gzip stream, and the last one can only be found by reading the
 *  whole stream (about 5 s for a 450 MB part). So whoever closes a
 *  .btr (the recorder, brainrec_split) writes the two ticks here, as
 *  a few lines of text:
 *
 *    first_tick=80206
 *    last_tick=90204
 *    frames=5000
 *    btr_bytes=480720368
 *
 *  btr_bytes is the size of the .btr when the note was written. A
 *  reader uses the note only when the .btr still has that size, so a
 *  note left over from another file, or a .btr that grew after the
 *  note, is ignored and the reader falls back to reading the .btr.
 *
 *  Header-only (stdio only) so the recorder in bolo_static, the
 *  stand-alone splitter and BrainTest all share one copy.
 *********************************************************/
#ifndef BRAIN_RECORD_TICKS_H
#define BRAIN_RECORD_TICKS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRAINREC_TICKS_FILENAME "brainrec_ticks.txt"

/* Engine ticks per second of game clock (GAME_TICK_LENGTH is 10 ms). */
#define BRAINREC_TICKS_PER_SEC 100u

/* Size of a file in bytes, or -1 if it cannot be read. */
static inline int64_t brainRecTicksFileSize(const char *path) {
#ifdef _WIN32
    struct _stat64 st;
    if (!path || _stat64(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
#else
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
#endif
}

/* Write <dir>/brainrec_ticks.txt for the .btr at <dir>/brainrec.btr, which
 * must already be closed (its size goes into the note). Returns false if the
 * .btr is missing or the note cannot be written. */
static inline bool brainRecTicksWrite(const char *dir, uint32_t firstTick,
                                      uint32_t lastTick, uint64_t frames) {
    char btr[1100], note[1100];
    if (!dir || !dir[0]) return false;
    snprintf(btr,  sizeof btr,  "%s/brainrec.btr", dir);
    snprintf(note, sizeof note, "%s/%s", dir, BRAINREC_TICKS_FILENAME);
    int64_t size = brainRecTicksFileSize(btr);
    if (size < 0) return false;
    FILE *f = fopen(note, "w");
    if (!f) return false;
    int n = fprintf(f, "first_tick=%lu\nlast_tick=%lu\nframes=%llu\nbtr_bytes=%lld\n",
                    (unsigned long)firstTick, (unsigned long)lastTick,
                    (unsigned long long)frames, (long long)size);
    bool ok = (n > 0);
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Read <dir>/brainrec_ticks.txt. Returns true only when every field is there
 * and btr_bytes equals btrBytes (the .btr's size now). */
static inline bool brainRecTicksRead(const char *dir, int64_t btrBytes,
                                     uint32_t *firstTick, uint32_t *lastTick) {
    char note[1100], line[128];
    unsigned long first = 0, last = 0;
    long long bytes = -1;
    int have = 0;
    if (!dir || !dir[0] || btrBytes < 0) return false;
    snprintf(note, sizeof note, "%s/%s", dir, BRAINREC_TICKS_FILENAME);
    FILE *f = fopen(note, "r");
    if (!f) return false;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "first_tick=%lu", &first) == 1)      have |= 1;
        else if (sscanf(line, "last_tick=%lu", &last) == 1)   have |= 2;
        else if (sscanf(line, "btr_bytes=%lld", &bytes) == 1) have |= 4;
    }
    fclose(f);
    if (have != 7 || bytes != (long long)btrBytes || last < first) return false;
    if (firstTick) *firstTick = (uint32_t)first;
    if (lastTick)  *lastTick  = (uint32_t)last;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_RECORD_TICKS_H */
