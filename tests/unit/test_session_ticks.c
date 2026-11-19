/*
 * test_session_ticks.c - the game length BrainTest's Load Session list shows.
 *
 * The list used to show wall-clock time (the .btr mtime minus the time in
 * the dir name). That is wrong for an -asap server, and for every
 * brainrec_split part, whose names all keep the game's start time. It now
 * shows the game time between the first and last recorded tick.
 *
 * session_ticks_recorder_note: the recorder writes brainrec_ticks.txt at
 * close with the same range a full walk of the .btr finds; a note whose
 * btr_bytes does not match the .btr is ignored; BrainTest's lookup takes the
 * note at once, reads the .btr in the background when there is no note, and
 * keeps that result in its cache file for the next run.
 *
 * session_ticks_real_dirs: a check against real recordings, run by hand.
 * Set WINBOLO_SESSION_TICKS_DIRS to session dirs separated by ';' and it
 * prints each one's range as the list shows it. With the variable unset it
 * does nothing and passes.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "server_sim.h"
#include "brain_record.h"
#include "brain_record_ticks.h"
#include "brain_record_walk.h"
#include "braintest_session_ticks.h"
#include "test_harness.h"

/* Look up a dir's range through the BrainTest lookup, waiting up to
 * timeoutMs for a background read. */
static SessionTicksState stLookupWait(const char *dir, uint32_t *first, uint32_t *last,
                                      Uint32 timeoutMs, Uint64 *msOut) {
    char btr[700];
    SDL_PathInfo pi;
    SessionTicksState st = SESSION_TICKS_FAILED;
    Uint64 t0 = SDL_GetTicks();

    snprintf(btr, sizeof btr, "%s/%s", dir, BRAINREC_FILENAME);
    if (!SDL_GetPathInfo(btr, &pi)) return SESSION_TICKS_FAILED;
    for (;;) {
        st = sessionTicksLookup(dir, btr, (int64_t)pi.size, (int64_t)pi.modify_time,
                                first, last);
        if (st != SESSION_TICKS_PENDING) break;
        if (SDL_GetTicks() - t0 > timeoutMs) break;
        SDL_Delay(5);
    }
    if (msOut) *msOut = SDL_GetTicks() - t0;
    return st;
}

int run_session_ticks_recorder_note(void) {
    char dir[512], btr[700], note[700], cache[512];
    uint32_t first = 0, last = 0, wFirst = 0, wLast = 0;
    uint64_t wFrames = 0;
    int64_t size;
    ServerSim *sim;
    int n;

    UT_ASSERT_MSG(utScratchPath(dir, sizeof dir, NULL), "no scratch dir");
    UT_ASSERT(utScratchPath(cache, sizeof cache, "ticks.cache"));
    snprintf(btr,  sizeof btr,  "%s/%s", dir, BRAINREC_FILENAME);
    snprintf(note, sizeof note, "%s/%s", dir, BRAINREC_TICKS_FILENAME);

    /* Record a short game. */
    sim = ut_make_running_sim("Recorded");
    UT_ASSERT(sim != NULL);
    for (n = 0; n < 5; n++) serverSimTick(sim);   /* a first tick above 0 */
    brainRecordSetSessionDir(dir);
    brainRecordSetEnabled(true);
    for (n = 0; n < 20; n++) {
        serverSimTick(sim);
        brainRecordTick(sim);
    }
    brainRecordEndGame();
    brainRecordSetEnabled(false);
    brainRecordSetSessionDir(NULL);
    serverSimDestroy(sim);

    /* The full walk is the reference. */
    UT_ASSERT_MSG(brainRecWalkFileTicks(btr, &wFirst, &wLast, &wFrames, NULL, NULL),
                  "could not walk %s", btr);
    UT_ASSERT_MSG(wFrames == 20, "the walk found %llu frames, wanted 20",
                  (unsigned long long)wFrames);
    UT_ASSERT_MSG(wLast > wFirst && wFirst > 0, "walk range %lu..%lu",
                  (unsigned long)wFirst, (unsigned long)wLast);

    /* The recorder's note says the same, and only for this .btr size. */
    size = brainRecTicksFileSize(btr);
    UT_ASSERT(size > 0);
    UT_ASSERT_MSG(brainRecTicksRead(dir, size, &first, &last), "no valid %s", note);
    UT_ASSERT_MSG(first == wFirst && last == wLast,
                  "note %lu..%lu, walk %lu..%lu", (unsigned long)first,
                  (unsigned long)last, (unsigned long)wFirst, (unsigned long)wLast);
    UT_ASSERT_MSG(!brainRecTicksRead(dir, size + 1, &first, &last),
                  "a note for another .btr size was taken");

    /* BrainTest's lookup takes the note at once. */
    sessionTicksInit(cache);
    first = last = 0;
    UT_ASSERT(stLookupWait(dir, &first, &last, 0, NULL) == SESSION_TICKS_HAVE);
    UT_ASSERT(first == wFirst && last == wLast);
    sessionTicksShutdown();

    /* No note (a recording made before the note existed): read in the
     * background, then kept in the cache file. */
    UT_ASSERT_MSG(remove(note) == 0, "could not delete %s", note);
    sessionTicksInit(cache);
    first = last = 0;
    UT_ASSERT_MSG(stLookupWait(dir, &first, &last, 20000, NULL) == SESSION_TICKS_HAVE,
                  "the background read did not finish");
    UT_ASSERT(first == wFirst && last == wLast);
    sessionTicksShutdown();

    sessionTicksInit(cache);
    first = last = 0;
    UT_ASSERT_MSG(stLookupWait(dir, &first, &last, 0, NULL) == SESSION_TICKS_HAVE,
                  "the range was not in the cache file on the next run");
    UT_ASSERT(first == wFirst && last == wLast);
    sessionTicksShutdown();

    /* The seconds the list shows: 100 engine ticks per second. */
    {
        int s0 = -1, s1 = -1, len = -1;
        sessionTicksSeconds(80206u, 90204u, &s0, &s1, &len);
        UT_ASSERT_MSG(s0 == 13 * 60 + 22 && s1 == 15 * 60 + 2 && len == 99,
                      "80206..90204 gave %d, %d, %d", s0, s1, len);
    }
    return 0;
}

int run_session_ticks_real_dirs(void) {
    const char *env = getenv("WINBOLO_SESSION_TICKS_DIRS");
    char cache[512], list[8192];
    char *save = NULL;

    if (!env || !env[0]) {
        printf("  (WINBOLO_SESSION_TICKS_DIRS unset: nothing to check)\n");
        return 0;
    }
    UT_ASSERT(utScratchPath(cache, sizeof cache, "ticks.cache"));
    sessionTicksInit(cache);
    SDL_strlcpy(list, env, sizeof list);
    for (char *dir = SDL_strtok_r(list, ";", &save); dir; dir = SDL_strtok_r(NULL, ";", &save)) {
        uint32_t first = 0, last = 0;
        Uint64 ms = 0;
        int s0, s1, len;
        SessionTicksState st = stLookupWait(dir, &first, &last, 120000, &ms);
        if (st != SESSION_TICKS_HAVE) {
            printf("  %s: no range (state %d)\n", dir, (int)st);
            continue;
        }
        sessionTicksSeconds(first, last, &s0, &s1, &len);
        printf("  %s: ticks %lu..%lu  length %d:%02d  clock %d:%02d-%d:%02d  (%llu ms)\n",
               dir, (unsigned long)first, (unsigned long)last, len / 60, len % 60,
               s0 / 60, s0 % 60, s1 / 60, s1 % 60, (unsigned long long)ms);
    }
    sessionTicksShutdown();
    return 0;
}
