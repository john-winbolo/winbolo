/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The server's game tick at the playhead.
 *
 * A recording states the server's tick in log_ServerTick records, at the
 * round's first entry, at every entry that carries a snapshot and every
 * FULL_SYNC_INTERVAL ticks. The viewer
 * keeps each as an anchor and answers the tick at any playback time from the
 * last anchor at or before it, counting the server's log entries in between
 * rather than milliseconds, since a LOG_NOEVENTS run, a snapshot and the
 * entity-mask block each cost playback 20 ms with no entry behind them.
 *
 * The hand-built cases write the log.dat bytes out as hex, so the bytes say
 * what the format is rather than what the writer happens to emit. The
 * recorded case writes a real round through the replay harness and reads it
 * back through the production reader.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_log.h"
#include "zip.h"
#include "replay_harness.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

/* ── Building a log.dat ────────────────────────────────────────────── */

typedef struct {
    uint8_t buf[65536];   /* static instances only: it is too big for a stack */
    size_t  len;
} LvstLog;

static void lvstPut(LvstLog *b, const void *bytes, size_t n) {
    memcpy(b->buf + b->len, bytes, n);
    b->len += n;
}

/* The v2 file header: WBOLOMOV, version 2, map name "Test", game type 1, no
 * hidden mines, ai or password, 16 players, game version 2.0.2, then a zero
 * ip, port, create time and WBN key. */
static const uint8_t kLvstHeader[] = {
    0x57, 0x42, 0x4F, 0x4C, 0x4F, 0x4D, 0x4F, 0x56,
    0x02,
    0x04, 0x54, 0x65, 0x73, 0x74,
    0x01, 0x00, 0x00, 0x00, 0x10,
    0x02, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* A running round's snapshot: the marker, no start delay or game length, no
 * items, one map run of grass and the terminator. The sixteen empty player
 * blocks follow it. The same bytes open every log here and stand for the
 * snapshots written mid-stream. */
static const uint8_t kLvstSnapshot[] = {
    0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x05, 0x00, 0x00, 0x02, 0x87,
    0x04, 0xFF, 0xFF, 0xFF,
};
static const uint8_t kLvstEmptyPlayer[] = {0x02, 0x00, 0x00};

static void lvstPutSnapshot(LvstLog *b) {
    int i;

    lvstPut(b, kLvstSnapshot, sizeof(kLvstSnapshot));
    for (i = 0; i < MAX_TANKS; i++) {
        lvstPut(b, kLvstEmptyPlayer, sizeof(kLvstEmptyPlayer));
    }
}

static void lvstPutOpening(LvstLog *b) {
    b->len = 0;
    lvstPut(b, kLvstHeader, sizeof(kLvstHeader));
    lvstPutSnapshot(b);
}

/* One LOG_EVENT block holding one log_ServerTick of tick. */
static void lvstPutAnchor(LvstLog *b, uint32_t tick) {
    const uint8_t frame[9] = {
        0x03, 0x01, 0x43, 0x00, 0x04,
        (uint8_t)(tick >> 24), (uint8_t)(tick >> 16),
        (uint8_t)(tick >> 8), (uint8_t)tick,
    };
    lvstPut(b, frame, sizeof(frame));
}

/* One LOG_EVENT block holding a log_ScnHint for slot 0 with the verb "a",
 * which the viewer consumes and shows nothing for: a tick that had
 * something in it. */
static const uint8_t kLvstFiller[] = {
    0x03, 0x01, 0x42, 0x00, 0x03, 0x00, 0x01, 0x61,
};

/* The entity-mask block the writer puts straight after a snapshot: every
 * mask empty, which is all an empty snapshot's lists can hold. */
static const uint8_t kLvstMasks[] = {
    0x03, 0x01, 0x3A, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void lvstPutNoEvents(LvstLog *b, uint8_t n) {
    const uint8_t rec[2] = {0x01, n};
    lvstPut(b, rec, sizeof(rec));
}

static void lvstPutQuit(LvstLog *b) {
    const uint8_t quit = 0x00;
    lvstPut(b, &quit, 1);
}

/* The log.dat bytes as a .wbv on disk, read back into a heap buffer for
 * lv_screenLoadMapFromMemory, which takes ownership of it. */
static uint8_t *lvstZipToMemory(const LvstLog *b, const char *path,
                                size_t *outLen) {
    zipFile      zf;
    zip_fileinfo zi;
    FILE        *f;
    long         sz;
    uint8_t     *out;

    remove(path);
    memset(&zi, 0, sizeof(zi));
    zf = zipOpen(path, 0);
    if (zf == NULL) return NULL;
    if (zipOpenNewFileInZip(zf, "log.dat", &zi, NULL, 0, NULL, 0, "",
                            Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK ||
        zipWriteInFileInZip(zf, b->buf, (unsigned)b->len) != ZIP_OK ||
        zipCloseFileInZip(zf) != ZIP_OK) {
        zipClose(zf, NULL);
        return NULL;
    }
    if (zipClose(zf, NULL) != ZIP_OK) return NULL;

    f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out = (uint8_t *)malloc((size_t)sz);
    if (out == NULL || fread(out, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(out);
        return NULL;
    }
    fclose(f);
    *outLen = (size_t)sz;
    return out;
}

/* A decoder with the .wbv bytes loaded from memory, or NULL. Takes ownership
 * of zipData. */
static LogViewerState *lvstOpenZip(uint8_t *zipData, size_t zipLen) {
    LogViewerState *lv;

    if (zipData == NULL) return NULL;
    lv = lv_decoderCreate(false);
    if (lv == NULL) {
        free(zipData);
        return NULL;
    }
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);
    if (lv_screenLoadMapFromMemory(zipData, zipLen) != TRUE) {
        lv_decoderDestroy(lv);
        return NULL;
    }
    return lv;
}

/* A decoder with b loaded as a .wbv, or NULL. */
static LogViewerState *lvstOpen(const LvstLog *b, const char *path) {
    size_t   zipLen  = 0;
    uint8_t *zipData = lvstZipToMemory(b, path, &zipLen);
    return lvstOpenZip(zipData, zipLen);
}

/* Tick until playback ends; FALSE if it never does. */
static bool lvstPlayToEnd(void) {
    int steps = 0;
    while (lv_screenIsPlaying() == TRUE && steps < 4096) {
        lv_screenLogTick();
        steps++;
    }
    return lv_screenIsPlaying() != TRUE;
}

/* One expected answer: the server tick at a playback time. */
typedef struct {
    uint32_t ms;
    uint32_t tick;
} LvstWant;

/* The first row the query answers wrongly, or -1. */
static int lvstFirstWrong(const LvstWant *rows, size_t n, uint32_t *got) {
    size_t i;
    for (i = 0; i < n; i++) {
        *got = lv_screenServerTickAt(rows[i].ms);
        if (*got != rows[i].tick) {
            return (int)i;
        }
    }
    return -1;
}

/* ── 1. A recorded round ───────────────────────────────────────────── */

typedef struct {
    bool     has;
    uint32_t endMs;
    uint32_t tickAtEnd;
} LvstEnd;

static void lvstReadEnd(void *ctx) {
    LvstEnd *seen = (LvstEnd *)ctx;
    seen->has       = lv_screenHasServerTick();
    seen->endMs     = lv_screenGetTimeRunning();
    seen->tickAtEnd = lv_screenServerTickAt(seen->endMs);
}

int run_lv_server_tick_recorded(void) {
    ReplayHarness h;
    LvstEnd       seen;
    uint32_t      endTick = 0;
    uint32_t      diff;
    bool          recorded;
    bool          played = false;

    memset(&h, 0, sizeof(h));
    memset(&seen, 0, sizeof(seen));
    recorded = replayHarnessStartRecording(&h, "lv_server_tick", "Tester");
    if (recorded) {
        /* Past two snapshot intervals, so the round's first entry is not the
           only place a record could be. */
        replayHarnessTick(&h, 600);
        endTick  = replayHarnessServerTick(&h);
        recorded = replayHarnessStopRecording(&h);
    }
    if (recorded) {
        played = replayHarnessDecodeFileThen(h.path, lvstReadEnd, &seen);
    }
    replayHarnessStop(&h);
    UT_ASSERT_MSG(recorded, "the round could not be recorded");
    UT_ASSERT_MSG(played, "the round did not play to end-of-log");

    UT_ASSERT_MSG(seen.has, "the recording carries no log_ServerTick");
    /* The sim's tick is one or two past the last one it wrote an entry for,
       as the round stops on a game tick or a keys tick. */
    diff = (seen.tickAtEnd > endTick) ? seen.tickAtEnd - endTick
                                      : endTick - seen.tickAtEnd;
    UT_ASSERT_MSG(diff <= 2,
                  "the viewer answered tick %u at end-of-log (%u ms); the "
                  "server stopped at %u", (unsigned)seen.tickAtEnd,
                  (unsigned)seen.endMs, (unsigned)endTick);
    return 0;
}

/* ── 2. Anchors, a snapshot between them and a new round's run ────── */

/* The event stream after the opening snapshot. A record the walk counts as
 * step N lands at (N + 1) * 20 ms; the writer ticks after it are what the
 * server counted.
 *
 *   step 0     anchor, tick 100                      20 ms   1
 *   steps 1-4  NOEVENTS 3                            40 ms   1, 2, 3, 4
 *   step 5     a tick with something in it          120 ms   5
 *   step 6     the snapshot's flush                 140 ms   5
 *   step 7     snapshot                             160 ms   5
 *   step 8     the entity-mask block                180 ms   5
 *   step 9     anchor, tick secondTick              200 ms   6
 *   steps 10-12 NOEVENTS 2                          220 ms   6, 7, 8
 *   step 13    anchor, tick 20: a new round's run   280 ms   9
 *   steps 14-18 NOEVENTS 4                          300 ms   9 .. 13
 *   step 19    LOG_QUIT                             400 ms
 *
 * With secondTick 110 the block at 140 ms is the snapshot's own flush. With
 * 112 it was the previous tick's own block — the snapshot's tick had queued
 * nothing before it — and the anchor puts that tick back. */
static void lvstBuildAnchors(LvstLog *b, uint32_t secondTick) {
    lvstPutOpening(b);
    lvstPutAnchor(b, 100);
    lvstPutNoEvents(b, 3);
    lvstPut(b, kLvstFiller, sizeof(kLvstFiller));
    lvstPut(b, kLvstFiller, sizeof(kLvstFiller));
    lvstPutSnapshot(b);
    lvstPut(b, kLvstMasks, sizeof(kLvstMasks));
    lvstPutAnchor(b, secondTick);
    lvstPutNoEvents(b, 2);
    lvstPutAnchor(b, 20);
    lvstPutNoEvents(b, 4);
    lvstPutQuit(b);
}

static const LvstWant kLvstFlushed[] = {
    {0, 98}, {10, 98},                       /* counted back from the first */
    {20, 100}, {40, 100}, {60, 102}, {100, 106},
    {120, 108}, {140, 108}, {160, 108}, {180, 108},
    {200, 110}, {240, 112}, {260, 114},
    {280, 20}, {300, 20}, {340, 24}, {380, 28}, {400, 28}, {5000, 28},
};

static const LvstWant kLvstSettled[] = {
    {0, 98}, {60, 102}, {120, 108},
    {140, 110}, {160, 110}, {180, 110},
    {200, 112}, {240, 114}, {260, 116},
    {280, 20}, {400, 28},
};

/* Load the anchors log with secondTick, play it through, seek back, and
   check the query against want each time. A message or NULL. */
static const char *lvstCheckAnchors(uint32_t secondTick, const LvstWant *want,
                                    size_t n, const char *path) {
    static LvstLog  b;
    static char     why[160];
    LogViewerState *lv;
    uint32_t        got = 0;
    int             row;

    lvstBuildAnchors(&b, secondTick);
    lv = lvstOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        return "the hand-built log could not be loaded";
    }

    /* The walk collects every anchor at load, before playback moves. */
    row = lv_screenHasServerTick() ? lvstFirstWrong(want, n, &got) : -2;
    if (row == -1 && !lvstPlayToEnd()) {
        row = -3;
    }
    if (row == -1) {
        row = lvstFirstWrong(want, n, &got);
    }
    if (row == -1) {
        lv_screenSeekToTimeMs(120);
        if (lv_screenGetTimeRunning() != 120 || !lv_screenHasServerTick()) {
            row = -4;
        } else {
            row = lvstFirstWrong(want, n, &got);
        }
    }
    lv_decoderDestroy(lv);
    remove(path);

    if (row == -1) {
        return NULL;
    }
    if (row == -2) {
        snprintf(why, sizeof(why), "no anchors after loading (second %u)",
                 (unsigned)secondTick);
    } else if (row == -3) {
        snprintf(why, sizeof(why), "playback did not reach end-of-log");
    } else if (row == -4) {
        snprintf(why, sizeof(why), "the seek back did not land at 120 ms");
    } else {
        snprintf(why, sizeof(why),
                 "second anchor %u: tick at %u ms is %u (want %u)",
                 (unsigned)secondTick, (unsigned)want[row].ms, (unsigned)got,
                 (unsigned)want[row].tick);
    }
    return why;
}

int run_lv_server_tick_anchors(void) {
    const char *why;

    /* The record codes the stream spells in hex. */
    UT_ASSERT(log_ServerTick == 0x43 && log_ScnHint == 0x42 &&
              log_EntityMasks == 0x3A);
    UT_ASSERT(LOG_QUIT == 0x00 && LOG_NOEVENTS == 0x01 &&
              LOG_EVENT == 0x03 && LOG_SNAPSHOT == 0x05);

    why = lvstCheckAnchors(110, kLvstFlushed,
                           sizeof(kLvstFlushed) / sizeof(kLvstFlushed[0]),
                           "lv_server_tick_flushed.wbv");
    UT_ASSERT_MSG(why == NULL, "%s", why);

    why = lvstCheckAnchors(112, kLvstSettled,
                           sizeof(kLvstSettled) / sizeof(kLvstSettled[0]),
                           "lv_server_tick_settled.wbv");
    UT_ASSERT_MSG(why == NULL, "%s", why);
    return 0;
}

/* ── 3. Records of the wrong length ────────────────────────────────── */

int run_lv_server_tick_hostile(void) {
    static LvstLog  b;
    const char     *path = "lv_server_tick_hostile.wbv";
    LogViewerState *lv;
    bool            has;
    bool            ended;
    uint32_t        at20, at40, at120;

    /*   step 0     log_ServerTick three bytes long            20 ms   1
     *   step 1     one five bytes long, then a good one of
     *              tick 500, in one block                     40 ms   2
     *   steps 2-4  NOEVENTS 2                                 60 ms   2, 3, 4
     *   step 5     LOG_QUIT                                  120 ms */
    static const uint8_t kStream[] = {
        0x03, 0x01, 0x43, 0x00, 0x03, 0x01, 0x02, 0x03,
        0x03, 0x02,
            0x43, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05,
            0x43, 0x00, 0x04, 0x00, 0x00, 0x01, 0xF4,
        0x01, 0x02,
        0x00,
    };
    static const uint8_t kOnlyBad[] = {
        0x03, 0x01, 0x43, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x01, 0x02,
        0x00,
    };

    lvstPutOpening(&b);
    lvstPut(&b, kStream, sizeof(kStream));
    lv = lvstOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }
    ended = lvstPlayToEnd();
    has   = lv_screenHasServerTick();
    at20  = lv_screenServerTickAt(20);
    at40  = lv_screenServerTickAt(40);
    at120 = lv_screenServerTickAt(120);
    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(ended, "playback did not reach end-of-log");
    UT_ASSERT_MSG(has, "the good record was not kept");
    UT_ASSERT_MSG(at40 == 500, "tick at 40 ms is %u (want 500)",
                  (unsigned)at40);
    UT_ASSERT_MSG(at20 == 498, "tick at 20 ms is %u (want 498, counted back "
                  "from the good record)", (unsigned)at20);
    UT_ASSERT_MSG(at120 == 504, "tick at 120 ms is %u (want 504)",
                  (unsigned)at120);

    /* A log whose only record is of the wrong length has no anchors. */
    lvstPutOpening(&b);
    lvstPut(&b, kOnlyBad, sizeof(kOnlyBad));
    lv = lvstOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the second hand-built log could not be loaded");
    }
    ended = lvstPlayToEnd();
    has   = lv_screenHasServerTick();
    at40  = lv_screenServerTickAt(40);
    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(ended, "playback did not reach end-of-log with only a bad "
                  "record");
    UT_ASSERT_MSG(!has, "a record of the wrong length made an anchor");
    UT_ASSERT_MSG(at40 == 4, "tick at 40 ms is %u with no anchors (want 4)",
                  (unsigned)at40);
    return 0;
}

/* ── 4. A recording from before the record ─────────────────────────── */

int run_lv_server_tick_old_recording(void) {
    const char     *dir = getenv("WB_WBV_FIXTURE_DIR");
    char            path[512];
    FILE           *f;
    long            sz;
    uint8_t        *zipData;
    LogViewerState *lv;
    bool            has;
    uint32_t        at0, at20, at1234;

    if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
    snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);

    f = fopen(path, "rb");
    UT_ASSERT_MSG(f != NULL, "cannot open the v2 fixture: %s", path);
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    zipData = (sz > 0) ? (uint8_t *)malloc((size_t)sz) : NULL;
    if (zipData == NULL || fread(zipData, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(zipData);
        UT_FAIL("cannot read the v2 fixture: %s", path);
    }
    fclose(f);

    /* The walk runs at load, so the load is all this needs. */
    lv = lvstOpenZip(zipData, (size_t)sz);
    UT_ASSERT_MSG(lv != NULL, "the v2 fixture did not load: %s", path);
    has    = lv_screenHasServerTick();
    at0    = lv_screenServerTickAt(0);
    at20   = lv_screenServerTickAt(20);
    at1234 = lv_screenServerTickAt(1234);
    lv_decoderDestroy(lv);

    UT_ASSERT_MSG(!has, "an old recording has anchors");
    UT_ASSERT_MSG(at0 == 0 && at20 == 2 && at1234 == 123,
                  "the fallback answered %u, %u, %u (want 0, 2, 123)",
                  (unsigned)at0, (unsigned)at20, (unsigned)at1234);
    return 0;
}

/* ── 5. Events and empty ticks alternating ─────────────────────────── */

/* After an anchor of tick 1000 at 20 ms, LVST_PAIRS pairs of an event block
 * and a LOG_NOEVENTS 1: two writer ticks for every 60 ms of playback, where
 * playback time alone would say three. */
#define LVST_PAIRS 150u

int run_lv_server_tick_alternating(void) {
    static LvstLog  b;
    const char     *path = "lv_server_tick_alternating.wbv";
    LogViewerState *lv;
    uint32_t        i;
    uint32_t        endMs, atEnd, atMid;
    bool            ended;

    lvstPutOpening(&b);
    lvstPutAnchor(&b, 1000);
    for (i = 0; i < LVST_PAIRS; i++) {
        lvstPut(&b, kLvstFiller, sizeof(kLvstFiller));
        lvstPutNoEvents(&b, 1);
    }
    lvstPutQuit(&b);

    lv = lvstOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }
    ended = lvstPlayToEnd();
    endMs = lv_screenGetTimeRunning();
    atEnd = lv_screenServerTickAt(endMs);
    /* Pair 100's event block is step 1 + 3 * 100, at (302 * 20) ms, and
       leaves 1 + 2 * 100 + 1 writer ticks counted. */
    atMid = lv_screenServerTickAt(302u * 20u);
    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(ended, "playback did not reach end-of-log");
    UT_ASSERT_MSG(endMs == (2u + 3u * LVST_PAIRS) * 20u,
                  "end-of-log at %u ms (want %u)", (unsigned)endMs,
                  (unsigned)((2u + 3u * LVST_PAIRS) * 20u));
    UT_ASSERT_MSG(atEnd == 1000u + 2u * (2u * LVST_PAIRS),
                  "tick at end-of-log is %u (want %u)", (unsigned)atEnd,
                  (unsigned)(1000u + 2u * (2u * LVST_PAIRS)));
    UT_ASSERT_MSG(atMid == 1000u + 2u * 201u,
                  "tick at pair 100 is %u (want %u)", (unsigned)atMid,
                  (unsigned)(1000u + 2u * 201u));
    return 0;
}

/* ── 6. A live feed ────────────────────────────────────────────────── */

int run_lv_server_tick_live_feed(void) {
    static LvstLog  b;
    LogViewerState *lv;
    bool            loaded;
    bool            atLoad;
    bool            ended;
    uint32_t        got = 0;
    int             row;

    /* A feed is the spectator ring's translation: one record per writer
     * tick, a keyframe snapshot included.
     *
     *   step 0     anchor, tick 200                      20 ms   1
     *   steps 1-2  NOEVENTS 1                            40 ms   1, 2
     *   step 3     a tick with something in it           80 ms   3
     *   step 4     keyframe snapshot                    100 ms   4
     *   step 5     anchor, tick 208                     120 ms   5
     *   steps 6-7  NOEVENTS 1                           140 ms   5, 6
     *   step 8     LOG_QUIT                             180 ms */
    static const LvstWant want[] = {
        {20, 200}, {40, 200}, {60, 202}, {80, 204}, {100, 206},
        {120, 208}, {140, 208}, {160, 210}, {180, 210},
    };

    lvstPutOpening(&b);
    lvstPutAnchor(&b, 200);
    lvstPutNoEvents(&b, 1);
    lvstPut(&b, kLvstFiller, sizeof(kLvstFiller));
    lvstPutSnapshot(&b);
    lvstPutAnchor(&b, 208);
    lvstPutNoEvents(&b, 1);
    lvstPutQuit(&b);

    lv = lv_decoderCreate(false);
    UT_ASSERT_MSG(lv != NULL, "lv_decoderCreate returned NULL");
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);

    /* A stream has no load walk: the anchors arrive as playback reaches
       them. */
    loaded = lv_screenLoadFromStream(b.buf, b.len) == TRUE;
    atLoad = lv_screenHasServerTick();
    ended  = loaded && lvstPlayToEnd();
    row    = lvstFirstWrong(want, sizeof(want) / sizeof(want[0]), &got);
    lv_decoderDestroy(lv);

    UT_ASSERT_MSG(loaded, "the stream failed to load");
    UT_ASSERT_MSG(!atLoad, "a feed had anchors before playback");
    UT_ASSERT_MSG(ended, "playback did not reach end-of-log on the feed");
    UT_ASSERT_MSG(row < 0, "tick at %u ms on the feed is %u (want %u)",
                  (unsigned)want[row < 0 ? 0 : row].ms, (unsigned)got,
                  (unsigned)want[row < 0 ? 0 : row].tick);
    return 0;
}
