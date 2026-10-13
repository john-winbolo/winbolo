/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The log viewer's text dump (src/logviewer/lv_dump.c).
 *
 * A short round is recorded through the replay harness and dumped around one
 * tick with the production decoder. The text must have the header, the
 * round's tank and the mark on the tick asked for. A tick the round does not
 * hold must be refused, and with no tick the dump is the event list alone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_dump.h"
#include "replay_harness.h"
#include "test_harness.h"

/* The whole dump file as one string, or NULL. The caller frees it. */
static char *lvdReadAll(const char *path) {
    FILE *f = fopen(path, "rb");
    long  sz;
    char *text;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    text = (char *) malloc((size_t) sz + 1);
    if (text != NULL) {
        size_t got = fread(text, 1, (size_t) sz, f);
        text[got] = '\0';
    }
    fclose(f);
    return text;
}

int run_lv_dump_recorded_round(void) {
    ReplayHarness h;
    LvDumpOptions o;
    char          outPath[300];
    char          mark[64];
    char         *text = NULL;
    uint32_t      endTick = 0;
    uint32_t      tick = 0;
    bool          recorded;
    int           atTick = -1;
    int           atStart = -1;
    char         *start = NULL;
    int           pastEnd = -1;
    int           eventsOnly = -1;
    char         *events = NULL;

    memset(&h, 0, sizeof(h));
    recorded = replayHarnessStartRecording(&h, "lv_dump", "Tester");
    if (recorded) {
        replayHarnessTick(&h, 600);
        endTick  = replayHarnessServerTick(&h);
        recorded = replayHarnessStopRecording(&h);
    }
    if (recorded) {
        snprintf(outPath, sizeof(outPath), "%s.dump.txt", h.path);
        remove(outPath);

        /* An even tick well inside the round: playback steps two server
           ticks at a time from the round's first, which is even here. */
        tick = (endTick / 2u) & ~1u;
        memset(&o, 0, sizeof(o));
        o.logPath  = h.path;
        o.haveTick = 1;
        o.tick     = tick;
        o.span     = 20;
        atTick     = lvDumpRun(&o);   /* outPath NULL: <log>.dump.txt */
        text       = lvdReadAll(outPath);

        /* The round's first steps, where the join and the first placing
           are: the middle of a quiet round holds no log records. */
        o.tick  = 2;
        o.span  = 2;
        atStart = lvDumpRun(&o);
        start   = lvdReadAll(outPath);
        o.span  = 20;

        o.tick  = endTick + 100000u;
        pastEnd = lvDumpRun(&o);

        o.haveTick = 0;
        eventsOnly = lvDumpRun(&o);
        events     = lvdReadAll(outPath);
        remove(outPath);
    }
    replayHarnessStop(&h);
    UT_ASSERT_MSG(recorded, "the round could not be recorded");

    UT_ASSERT_MSG(atTick == LV_DUMP_OK, "the dump at tick %u failed with %d",
                  (unsigned) tick, atTick);
    UT_ASSERT_MSG(text != NULL, "the dump wrote no file beside the log");
    UT_ASSERT_MSG(strstr(text, "\nlog format_version=") != NULL,
                  "the dump has no header line");
    UT_ASSERT_MSG(strstr(text, "log server_ticks=1 ") != NULL,
                  "the dump does not say the recording has server ticks");
    UT_ASSERT_MSG(strstr(text, "\ndump mode=window ") != NULL,
                  "the dump does not state its window");
    UT_ASSERT_MSG(strstr(text, "\ntank tick=") != NULL, "the dump has no tank line");
    UT_ASSERT_MSG(strstr(text, "name=\"Tester\"") != NULL,
                  "the dump does not name the round's player");
    snprintf(mark, sizeof(mark), "\n==== TICK %u ====", (unsigned) tick);
    UT_ASSERT_MSG(strstr(text, mark) != NULL, "the dump does not mark tick %u",
                  (unsigned) tick);
    snprintf(mark, sizeof(mark), "\nstep tick=%u ", (unsigned) tick);
    UT_ASSERT_MSG(strstr(text, mark) != NULL, "the dump has no step at tick %u",
                  (unsigned) tick);
    /* 20 ticks each side: the step 22 ticks before must not be there. */
    snprintf(mark, sizeof(mark), "\nstep tick=%u ", (unsigned) (tick - 22u));
    UT_ASSERT_MSG(strstr(text, mark) == NULL, "the dump goes past its span");
    free(text);

    UT_ASSERT_MSG(atStart == LV_DUMP_OK, "the dump at the round's start failed with %d",
                  atStart);
    UT_ASSERT_MSG(start != NULL, "the dump at the round's start wrote no file");
    UT_ASSERT_MSG(strstr(start, "\nrec tick=") != NULL,
                  "the dump at the round's start has no record line");
    free(start);

    UT_ASSERT_MSG(pastEnd == LV_DUMP_ERR_TICK,
                  "a tick past the round's end gave %d, not the tick error", pastEnd);

    UT_ASSERT_MSG(eventsOnly == LV_DUMP_OK, "the events dump failed with %d", eventsOnly);
    UT_ASSERT_MSG(events != NULL, "the events dump wrote no file");
    UT_ASSERT_MSG(strstr(events, "\ndump mode=events") != NULL,
                  "the events dump does not state its mode");
    UT_ASSERT_MSG(strstr(events, "\nstep tick=") == NULL,
                  "the events dump holds steps");
    UT_ASSERT_MSG(strstr(events, "\nend ms=") != NULL, "the events dump has no end line");
    free(events);

    /* A number that does not fit 32 bits is refused, not cut down to some
       other tick; and a path too long for its buffer is refused, not cut
       short to name some other file. Both stop before any log is opened. */
    {
        static char   longPath[FILENAME_MAX + 64];
        char         *tooBig[]   = { "viewer", "-dump", "none.wbv", "-tick", "4294967297" };
        char         *bigRound[] = { "viewer", "-dump", "none.wbv", "-round", "2147483648" };
        LvDumpOptions longOpts;

        UT_ASSERT_MSG(lvDumpMain(5, tooBig) == LV_DUMP_ERR_ARGS,
                      "a tick over 32 bits was taken");
        UT_ASSERT_MSG(lvDumpMain(5, bigRound) == LV_DUMP_ERR_ARGS,
                      "a round over 31 bits was taken");
        memset(longPath, 'a', sizeof(longPath) - 1);
        longPath[sizeof(longPath) - 1] = '\0';
        memset(&longOpts, 0, sizeof(longOpts));
        longOpts.logPath = longPath;
        UT_ASSERT_MSG(lvDumpRun(&longOpts) == LV_DUMP_ERR_ARGS,
                      "a log path too long for its buffer was taken");
    }
    return 0;
}
