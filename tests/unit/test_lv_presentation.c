/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The log viewer's scenario panels, scores, announcement and markers at the
 * playhead.
 *
 * A recording holds a scenario's presentation as log_ScnPanel, log_ScnScore,
 * log_ScnAnnounce and log_ScnMarker records, each replacing the state for its
 * own key. The viewer applies them as playback passes them, and after a seek
 * rebuilds them from an index the load walk made, since its snapshots carry
 * none of it.
 *
 * The hand-built cases write the log.dat bytes out as hex, so the bytes say
 * what the format is rather than what the writer happens to emit. The
 * scripted case records a real round through the scenario host
 * (lv_scripts_fixture.c) and reads it back through the production reader.
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
#include "lv_scripts_fixture.h"
#include "test_harness.h"

/* ── Building a log.dat ────────────────────────────────────────────── */

typedef struct {
    uint8_t buf[65536];   /* static instances only: it is too big for a stack */
    size_t  len;
} LvpLog;

static void lvpPut(LvpLog *b, const void *bytes, size_t n) {
    memcpy(b->buf + b->len, bytes, n);
    b->len += n;
}

/* The v2 file header: WBOLOMOV, version 2, map name "Test", game type 1, no
 * hidden mines, ai or password, 16 players, game version 2.0.2, then a zero
 * ip, port, create time and WBN key. */
static const uint8_t kLvpHeader[] = {
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

/* A running round's opening snapshot: no start delay or game length, no
 * items, one map run of grass (which marks it as carrying a world, so no
 * lobby is in front of the stream) and the terminator. The sixteen empty
 * player blocks follow it. */
static const uint8_t kLvpSnapshot[] = {
    0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x05, 0x00, 0x00, 0x02, 0x87,
    0x04, 0xFF, 0xFF, 0xFF,
};
static const uint8_t kLvpEmptyPlayer[] = {0x02, 0x00, 0x00};

static void lvpPutOpening(LvpLog *b) {
    int i;

    b->len = 0;
    lvpPut(b, kLvpHeader, sizeof(kLvpHeader));
    lvpPut(b, kLvpSnapshot, sizeof(kLvpSnapshot));
    for (i = 0; i < MAX_TANKS; i++) {
        lvpPut(b, kLvpEmptyPlayer, sizeof(kLvpEmptyPlayer));
    }
}

/* One LOG_EVENT frame holding one event, framed by its own length. */
static void lvpPutEvent(LvpLog *b, uint8_t code, const uint8_t *payload,
                        size_t n) {
    const uint8_t frame[5] = {0x03, 0x01, code, (uint8_t)(n >> 8),
                              (uint8_t)(n & 0xFF)};
    lvpPut(b, frame, sizeof(frame));
    lvpPut(b, payload, n);
}

/* The display lists the cases store: A and B for everyone, C for team 2.
 *   A: rect x=1 y=2 w=3 h=4 colour=5 fill=0
 *   B: line 0,0 to 127,127 colour=2
 *   C: rect x=10 y=10 w=20 h=20 colour=6 fill=1 */
static const uint8_t kLvpListA[] = {0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00};
static const uint8_t kLvpListB[] = {0x02, 0x00, 0x00, 0x7F, 0x7F, 0x02};
static const uint8_t kLvpListC[] = {0x01, 0x0A, 0x0A, 0x14, 0x14, 0x06, 0x01};

/* The event stream after the opening snapshot. A record the walk counts as
 * tick N lands at (N + 1) * 20 ms, the time playback reaches it.
 *
 *   ticks 0-3   NOEVENTS 3
 *   tick 4      marker 3, a square at (10, 12), colour 5         100 ms
 *   tick 5      everyone's panel, list A                         120 ms
 *   tick 6      "Go!" for 250 ticks                              140 ms
 *   tick 7      team 2's panel, list C; slot 0 scores -5
 *               "kills"; team 1 scores 1234 "flags"; marker 5
 *               follows slot 1 in colour 6                       160 ms
 *   ticks 8-9   NOEVENTS 1
 *   tick 10     everyone's panel, list B                         220 ms
 *   tick 11     marker 3 cleared                                 240 ms
 *   tick 12     the line cleared                                 260 ms
 *   ticks 13-16 NOEVENTS 3
 *   tick 17     LOG_QUIT                                         360 ms */
static const uint8_t kLvpStream[] = {
    0x01, 0x03,
    0x03, 0x01, 0x41, 0x00, 0x09,
        0x03, 0x00, 0x00, 0xFF, 0x04, 0x0A, 0x0C, 0x00, 0x05,
    0x03, 0x01, 0x3E, 0x00, 0x0C,
        0x00, 0x00, 0xFF, 0x00, 0x07,
        0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00,
    0x03, 0x01, 0x40, 0x00, 0x08,
        0x00, 0xFF, 0x00, 0xFA, 0x03, 0x47, 0x6F, 0x21,
    0x03, 0x04,
        0x3E, 0x00, 0x0C,
            0x00, 0x02, 0xFF, 0x00, 0x07,
            0x01, 0x0A, 0x0A, 0x14, 0x14, 0x06, 0x01,
        0x3F, 0x00, 0x0C,
            0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFB,
            0x05, 0x6B, 0x69, 0x6C, 0x6C, 0x73,
        0x3F, 0x00, 0x0C,
            0x01, 0x01, 0x00, 0x00, 0x04, 0xD2,
            0x05, 0x66, 0x6C, 0x61, 0x67, 0x73,
        0x41, 0x00, 0x09,
            0x05, 0x01, 0x00, 0xFF, 0x04, 0x00, 0x00, 0x01, 0x06,
    0x01, 0x01,
    0x03, 0x01, 0x3E, 0x00, 0x0B,
        0x00, 0x00, 0xFF, 0x00, 0x06,
        0x02, 0x00, 0x00, 0x7F, 0x7F, 0x02,
    0x03, 0x01, 0x41, 0x00, 0x09,
        0x03, 0x02, 0x00, 0xFF, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x01, 0x40, 0x00, 0x05,
        0x00, 0xFF, 0x00, 0x00, 0x00,
    0x01, 0x03,
    0x00,
};

#define LVP_END_MS 360u

/* The log.dat bytes as a .wbv on disk, read back into a heap buffer for
 * lv_screenLoadMapFromMemory, which takes ownership of it. */
static uint8_t *lvpZipToMemory(const LvpLog *b, const char *path,
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

/* A decoder with b loaded from memory, or NULL. */
static LogViewerState *lvpOpen(const LvpLog *b, const char *path) {
    LogViewerState *lv;
    uint8_t        *zipData;
    size_t          zipLen = 0;

    zipData = lvpZipToMemory(b, path, &zipLen);
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

/* Tick until playback ends; FALSE if it never does. */
static bool lvpPlayToEnd(void) {
    int steps = 0;
    while (lv_screenIsPlaying() == TRUE && steps < 4096) {
        lv_screenLogTick();
        steps++;
    }
    return lv_screenIsPlaying() != TRUE;
}

/* Is the row set and holding exactly list? */
static bool lvpRowHolds(const LvPresPanelRow *row, const uint8_t *list,
                        size_t n) {
    return row != NULL && row->set && row->len == n &&
           memcmp(row->bytes, list, n) == 0;
}

static bool lvpRowEmpty(const LvPresPanelRow *row) {
    return row != NULL && !row->set;
}

/* ── 1. Stored at the playhead, then after seeks both ways ─────────── */

/* What the stores hold after the whole stream, whether playback passed each
 * record or the rebuild applied them. A message naming the first thing that
 * is wrong, or NULL. */
static const char *lvpCheckEnd(void) {
    const LvPresScore  *s;
    const LvPresMarker *m;

    if (!lvpRowHolds(lv_screenGetPanelRow(0, 0xFF), kLvpListB,
                     sizeof(kLvpListB))) {
        return "everyone's panel is not list B";
    }
    if (!lvpRowHolds(lv_screenGetPanelRow(2, 0xFF), kLvpListC,
                     sizeof(kLvpListC))) {
        return "team 2's panel is not list C";
    }
    if (!lvpRowEmpty(lv_screenGetPanelRow(1, 0xFF)) ||
        !lvpRowEmpty(lv_screenGetPanelRow(0, 0))) {
        return "a panel row no record named is set";
    }
    s = lv_screenGetScore(SCN_SCORE_KIND_PLAYER, 0);
    if (s == NULL || !s->valid || s->score != -5 ||
        strcmp(s->label, "kills") != 0) {
        return "slot 0's score is not -5 \"kills\"";
    }
    s = lv_screenGetScore(SCN_SCORE_KIND_TEAM, 1);
    if (s == NULL || !s->valid || s->score != 1234 ||
        strcmp(s->label, "flags") != 0) {
        return "team 1's score is not 1234 \"flags\"";
    }
    s = lv_screenGetScore(SCN_SCORE_KIND_PLAYER, 1);
    if (s == NULL || s->valid) {
        return "slot 1 has a score";
    }
    s = lv_screenGetScore(SCN_SCORE_KIND_TEAM, 2);
    if (s == NULL || s->valid) {
        return "team 2 has a score";
    }
    if (lv_screenGetAnnounce() == NULL || lv_screenGetAnnounce()->set) {
        return "the line is still up after its clear";
    }
    m = lv_screenGetMarker(3);
    if (m == NULL || m->set) {
        return "marker 3 is still set after its clear";
    }
    m = lv_screenGetMarker(5);
    if (m == NULL || !m->set || m->kind != SCN_MARKER_KIND_FOLLOW ||
        m->slot != 1 || m->colour != 6 || m->destTeam != 0 ||
        m->destPlayer != 0xFF) {
        return "marker 5 is not following slot 1 in colour 6";
    }
    return NULL;
}

int run_lv_presentation_seek(void) {
    const char           *path = "lv_presentation_seek.wbv";
    static LvpLog         b;
    LogViewerState       *lv;
    const LvPresAnnounce *a;
    const LvPresMarker   *m;
    const char           *why;

    /* The record codes the stream spells in hex. */
    UT_ASSERT(log_ScnPanel == 0x3E && log_ScnScore == 0x3F &&
              log_ScnAnnounce == 0x40 && log_ScnMarker == 0x41);
    UT_ASSERT(LOG_QUIT == 0x00 && LOG_NOEVENTS == 0x01 &&
              LOG_EVENT == 0x03 && LOG_SNAPSHOT == 0x05);

    lvpPutOpening(&b);
    lvpPut(&b, kLvpStream, sizeof(kLvpStream));
    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }

    /* Nothing is up before playback reaches a record. */
    if (!lvpRowEmpty(lv_screenGetPanelRow(0, 0xFF)) ||
        lv_screenGetMarker(3) == NULL || lv_screenGetMarker(3)->set) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("the stores are not empty at the start of the log");
    }

    /* Keys out of range have no store. */
    if (lv_screenGetPanelRow(16, 0xFF) != NULL ||
        lv_screenGetPanelRow(0, 16) != NULL ||
        lv_screenGetScore(2, 0) != NULL ||
        lv_screenGetScore(SCN_SCORE_KIND_PLAYER, 16) != NULL ||
        lv_screenGetScore(SCN_SCORE_KIND_TEAM, 0) != NULL ||
        lv_screenGetScore(SCN_SCORE_KIND_TEAM, 16) != NULL ||
        lv_screenGetMarker(16) != NULL) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("an accessor answered a key out of range");
    }

    /* Played through: every record applied as playback passed it. */
    if (!lvpPlayToEnd()) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("playback did not reach end-of-log");
    }
    why = lvpCheckEnd();
    if (why != NULL) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("at end-of-log: %s", why);
    }

    /* Back to tick 7: list A, the line and marker 3 are all up again. */
    lv_screenSeekToTimeMs(160);
    why = NULL;
    a   = lv_screenGetAnnounce();
    m   = lv_screenGetMarker(3);
    if (lv_screenGetTimeRunning() != 160) {
        why = "the seek did not land at 160 ms";
    } else if (!lvpRowHolds(lv_screenGetPanelRow(0, 0xFF), kLvpListA,
                            sizeof(kLvpListA))) {
        why = "everyone's panel is not list A";
    } else if (!lvpRowHolds(lv_screenGetPanelRow(2, 0xFF), kLvpListC,
                            sizeof(kLvpListC))) {
        why = "team 2's panel is not list C";
    } else if (a == NULL || !a->set || a->ms != 140 || a->ticks != 250 ||
               a->destTeam != 0 || a->destPlayer != 0xFF ||
               strcmp(a->text, "Go!") != 0) {
        why = "the line is not \"Go!\" from 140 ms for 250 ticks";
    } else if (m == NULL || !m->set || m->kind != SCN_MARKER_KIND_SQUARE ||
               m->x != 10 || m->y != 12 || m->colour != 5) {
        why = "marker 3 is not the square at (10, 12) in colour 5";
    } else if (lv_screenGetScore(SCN_SCORE_KIND_TEAM, 1) == NULL ||
               !lv_screenGetScore(SCN_SCORE_KIND_TEAM, 1)->valid) {
        why = "team 1's score is not there";
    }
    if (why != NULL) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("after seeking back to 160 ms: %s", why);
    }

    /* Further back, to tick 4: only marker 3 has landed. */
    lv_screenSeekToTimeMs(100);
    why = NULL;
    m   = lv_screenGetMarker(3);
    if (!lvpRowEmpty(lv_screenGetPanelRow(0, 0xFF)) ||
        !lvpRowEmpty(lv_screenGetPanelRow(2, 0xFF))) {
        why = "a panel is up before its record";
    } else if (lv_screenGetAnnounce()->set) {
        why = "the line is up before its record";
    } else if (lv_screenGetScore(SCN_SCORE_KIND_PLAYER, 0)->valid ||
               lv_screenGetScore(SCN_SCORE_KIND_TEAM, 1)->valid) {
        why = "a score is there before its record";
    } else if (m == NULL || !m->set) {
        why = "marker 3 is not set";
    } else if (lv_screenGetMarker(5)->set) {
        why = "marker 5 is set before its record";
    }
    if (why != NULL) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("after seeking back to 100 ms: %s", why);
    }

    /* Forward to the end: replaced and cleared again. */
    lv_screenSeekToTimeMs(LVP_END_MS);
    why = lvpCheckEnd();
    lv_decoderDestroy(lv);
    remove(path);
    UT_ASSERT_MSG(why == NULL, "after seeking forward to the end: %s", why);
    return 0;
}

/* ── 2. Hostile records ────────────────────────────────────────────── */

/* The records the stores hold before anything hostile arrives, in one frame
 * at 20 ms: everyone's panel list A, slot 0's score of 1 "ok", the line "Go!"
 * and marker 3 on the square (10, 12) in colour 5. */
static const uint8_t kLvpBaseline[] = {
    0x03, 0x04,
        0x3E, 0x00, 0x0C,
            0x00, 0x00, 0xFF, 0x00, 0x07,
            0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00,
        0x3F, 0x00, 0x09,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x6F, 0x6B,
        0x40, 0x00, 0x08,
            0x00, 0xFF, 0x00, 0xFA, 0x03, 0x47, 0x6F, 0x21,
        0x41, 0x00, 0x09,
            0x03, 0x00, 0x00, 0xFF, 0x04, 0x0A, 0x0C, 0x00, 0x05,
};

/* One hostile payload: the record's code and its bytes. */
typedef struct {
    uint8_t     code;
    uint8_t     len;
    uint8_t     bytes[24];
    const char *what;
} LvpHostile;

/* Each would change a store a well-formed record of its kind can reach. */
static const LvpHostile kLvpHostile[] = {
    /* Panels. List C to everyone would replace list A. */
    {0x3E, 12, {0x01, 0x00, 0xFF, 0x00, 0x07,
                0x01, 0x0A, 0x0A, 0x14, 0x14, 0x06, 0x01}, "panel id 1"},
    {0x3E, 12, {0x00, 0x10, 0xFF, 0x00, 0x07,
                0x01, 0x0A, 0x0A, 0x14, 0x14, 0x06, 0x01}, "panel team 16"},
    {0x3E, 12, {0x00, 0x00, 0x10, 0x00, 0x07,
                0x01, 0x0A, 0x0A, 0x14, 0x14, 0x06, 0x01}, "panel slot 16"},
    {0x3E, 6,  {0x00, 0x00, 0xFF, 0x00, 0x01, 0x00},
     "panel list with opcode 0"},
    {0x3E, 12, {0x00, 0x00, 0xFF, 0x00, 0x07,
                0x01, 0x0A, 0x0A, 0x14, 0x14, 0x06, 0x02},
     "panel rect with fill 2"},
    {0x3E, 5,  {0x00, 0x10, 0xFF, 0x00, 0x00}, "panel clear to team 16"},
    /* Scores. */
    {0x3F, 7,  {0x02, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00}, "score kind 2"},
    {0x3F, 7,  {0x00, 0x10, 0x00, 0x00, 0x00, 0x09, 0x00},
     "score for slot 16"},
    {0x3F, 7,  {0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00},
     "score for team 0"},
    {0x3F, 7,  {0x01, 0x10, 0x00, 0x00, 0x00, 0x09, 0x00},
     "score for team 16"},
    {0x3F, 23, {0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x10,
                0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
                0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41},
     "score label of 16 bytes"},
    /* Announcements. */
    {0x40, 7,  {0x10, 0xFF, 0x00, 0x0A, 0x02, 0x68, 0x69},
     "line to team 16"},
    {0x40, 7,  {0x00, 0x10, 0x00, 0x0A, 0x02, 0x68, 0x69},
     "line to slot 16"},
    {0x40, 5,  {0x10, 0xFF, 0x00, 0x00, 0x00}, "line clear to team 16"},
    /* Markers, all on id 3, so one taken would move or clear it. */
    {0x41, 9,  {0x10, 0x00, 0x00, 0xFF, 0x04, 0x14, 0x15, 0x00, 0x05},
     "marker id 16"},
    {0x41, 9,  {0x03, 0x03, 0x00, 0xFF, 0x04, 0x14, 0x15, 0x00, 0x05},
     "marker kind 3"},
    {0x41, 8,  {0x03, 0x00, 0x00, 0xFF, 0x03, 0x14, 0x15, 0x00},
     "marker placement of 3 bytes"},
    {0x41, 10, {0x03, 0x00, 0x00, 0xFF, 0x05, 0x14, 0x15, 0x00, 0x05, 0x00},
     "marker placement of 5 bytes"},
    {0x41, 9,  {0x03, 0x00, 0x00, 0xFF, 0x04, 0x14, 0x15, 0x00, 0x10},
     "marker colour 16"},
    {0x41, 9,  {0x03, 0x01, 0x00, 0xFF, 0x04, 0x00, 0x00, 0x10, 0x05},
     "marker following slot 16"},
    {0x41, 9,  {0x03, 0x02, 0x00, 0xFF, 0x04, 0x00, 0x00, 0x00, 0x10},
     "marker clear in colour 16"},
    {0x41, 9,  {0x03, 0x00, 0x11, 0xFF, 0x04, 0x14, 0x15, 0x00, 0x05},
     "marker to team 17"},
};

#define LVP_HOSTILE_COUNT (sizeof(kLvpHostile) / sizeof(kLvpHostile[0]))

/* NOEVENTS 5, then LOG_QUIT. */
static const uint8_t kLvpEnd[] = {0x01, 0x05, 0x00};

/* The stores as they stood after the baseline, for comparing against. */
static LvPresentation s_lvpBaseline;

static void lvpBuildHostile(LvpLog *b) {
    uint8_t payload[1 + 5 + 1018];
    size_t  i;

    lvpPutOpening(b);
    lvpPut(b, kLvpBaseline, sizeof(kLvpBaseline));
    for (i = 0; i < LVP_HOSTILE_COUNT; i++) {
        lvpPutEvent(b, kLvpHostile[i].code, kLvpHostile[i].bytes,
                    kLvpHostile[i].len);
    }

    /* A list one byte past SCN_PANEL_MAX, every byte of it present. */
    payload[0] = 0x00; payload[1] = 0x00; payload[2] = 0xFF;
    payload[3] = (uint8_t)((SCN_PANEL_MAX + 1) >> 8);
    payload[4] = (uint8_t)((SCN_PANEL_MAX + 1) & 0xFF);
    memset(payload + 5, 0x01, SCN_PANEL_MAX + 1);
    lvpPutEvent(b, 0x3E, payload, 5 + SCN_PANEL_MAX + 1);

    /* A line of 129 bytes. */
    payload[0] = 0x00; payload[1] = 0xFF; payload[2] = 0x00; payload[3] = 0x0A;
    payload[4] = 129;
    memset(payload + 5, 'x', 129);
    lvpPutEvent(b, 0x40, payload, 5 + 129);

    lvpPut(b, kLvpEnd, sizeof(kLvpEnd));
}

int run_lv_presentation_hostile(void) {
    const char     *path = "lv_presentation_hostile.wbv";
    const char     *tail = "lv_presentation_tail.wbv";
    static LvpLog   b;
    LogViewerState *lv;
    bool            baselineRight;
    bool            ended;
    bool            unchanged;
    bool            unchangedAfterSeek;
    bool            tailUnchanged;
    const char     *tookFirst;
    uint32_t        endMs;
    int             i;

    lvpBuildHostile(&b);
    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }

    /* The baseline frame is the first tick. */
    lv_screenLogTick();
    baselineRight =
        lvpRowHolds(lv_screenGetPanelRow(0, 0xFF), kLvpListA,
                    sizeof(kLvpListA)) &&
        lv_screenGetScore(SCN_SCORE_KIND_PLAYER, 0)->valid &&
        lv_screenGetScore(SCN_SCORE_KIND_PLAYER, 0)->score == 1 &&
        strcmp(lv_screenGetAnnounce()->text, "Go!") == 0 &&
        lv_screenGetMarker(3)->set && lv_screenGetMarker(3)->x == 10;
    memcpy(&s_lvpBaseline, &lv->pres, sizeof(s_lvpBaseline));

    /* One hostile record a tick, then the two built above. */
    tookFirst = NULL;
    for (i = 0; i < (int)LVP_HOSTILE_COUNT + 2 &&
                lv_screenIsPlaying() == TRUE; i++) {
        lv_screenLogTick();
        if (tookFirst == NULL &&
            memcmp(&s_lvpBaseline, &lv->pres, sizeof(s_lvpBaseline)) != 0) {
            tookFirst = (i < (int)LVP_HOSTILE_COUNT)
                            ? kLvpHostile[i].what
                            : (i == (int)LVP_HOSTILE_COUNT)
                                  ? "panel list past SCN_PANEL_MAX"
                                  : "line of 129 bytes";
        }
    }
    ended     = lvpPlayToEnd();
    unchanged = memcmp(&s_lvpBaseline, &lv->pres, sizeof(s_lvpBaseline)) == 0;
    endMs     = lv_screenGetTimeRunning();

    /* A rebuild reads the hostile records again, and must turn them down
       the same way. */
    lv_screenSeekToTimeMs(20);
    lv_screenSeekToTimeMs(endMs);
    unchangedAfterSeek =
        memcmp(&s_lvpBaseline, &lv->pres, sizeof(s_lvpBaseline)) == 0;
    lv_decoderDestroy(lv);
    remove(path);

    /* A panel whose list the file ends inside. Playback is stopped on that
       record, since there is nothing after it to read. */
    lvpPutOpening(&b);
    lvpPut(&b, kLvpBaseline, sizeof(kLvpBaseline));
    {
        static const uint8_t cut[] = {0x03, 0x01, 0x3E, 0x00, 0x0C,
                                      0x00, 0x00, 0xFF, 0x00, 0x07,
                                      0x01, 0x0A};
        lvpPut(&b, cut, sizeof(cut));
    }
    lv = lvpOpen(&b, tail);
    tailUnchanged = FALSE;
    if (lv != NULL) {
        for (i = 0; i < 2; i++) {
            lv_screenLogTick();
        }
        tailUnchanged =
            memcmp(&s_lvpBaseline, &lv->pres, sizeof(s_lvpBaseline)) == 0;
        lv_decoderDestroy(lv);
    }
    remove(tail);

    UT_ASSERT_MSG(baselineRight, "the baseline records were not stored");
    UT_ASSERT_MSG(ended, "playback did not reach end-of-log past the %d "
                  "hostile records", (int)LVP_HOSTILE_COUNT + 2);
    UT_ASSERT_MSG(tookFirst == NULL, "the stores took the %s record",
                  tookFirst);
    UT_ASSERT_MSG(unchanged, "a hostile record changed the stores");
    UT_ASSERT_MSG(unchangedAfterSeek,
                  "a hostile record changed the stores in a rebuild");
    UT_ASSERT_MSG(tailUnchanged,
                  "a panel with its list cut off changed the stores");
    return 0;
}

/* ── 3. A recorded scripted round ──────────────────────────────────── */

typedef struct {
    bool     panelSet;
    int      panelParse;      /* scnPanelParse's answer on the stored row */
    int      panelCount;
    int      panelOp;
    bool     scoreValid;
    int32_t  score;
    char     label[LV_PRES_LABEL_LEN];
    bool     lineSet;
    char     line[LV_PRES_ANNOUNCE_MAX + 1];
    uint32_t lineMs;
    bool     panelBefore;     /* after a seek to a tick before the records */
    bool     lineBefore;
    bool     panelAt;         /* after a seek forward onto them */
} LvpScripted;

static void lvpReadScripted(void *ctx) {
    LvpScripted           *seen = (LvpScripted *)ctx;
    static ScnPanelList    list;
    const LvPresPanelRow  *row = lv_screenGetPanelRow(0, 0xFF);
    const LvPresScore     *s   = lv_screenGetScore(SCN_SCORE_KIND_TEAM,
                                                   LVSF_PRES_TEAM);
    const LvPresAnnounce  *a   = lv_screenGetAnnounce();

    seen->panelSet = row != NULL && row->set;
    if (seen->panelSet) {
        memset(&list, 0, sizeof(list));
        seen->panelParse = (int)scnPanelParse(row->bytes, row->len, &list);
        seen->panelCount = list.count;
        seen->panelOp    = (list.count > 0) ? list.items[0].op : -1;
    }
    if (s != NULL) {
        seen->scoreValid = s->valid;
        seen->score      = s->score;
        memcpy(seen->label, s->label, sizeof(seen->label));
    }
    if (a != NULL && a->set) {
        seen->lineSet = TRUE;
        seen->lineMs  = a->ms;
        memcpy(seen->line, a->text, sizeof(seen->line));
    }
    if (!seen->lineSet || seen->lineMs < 40) {
        return;
    }

    /* The three records went down in one tick. */
    lv_screenSeekToTimeMs(seen->lineMs - 20);
    seen->panelBefore = lv_screenGetPanelRow(0, 0xFF)->set;
    seen->lineBefore  = lv_screenGetAnnounce()->set;
    lv_screenSeekToTimeMs(seen->lineMs);
    seen->panelAt     = lv_screenGetPanelRow(0, 0xFF)->set;
}

/* With WB_KEEP_WBV set to a path, a copy of the recording at from is written
 * there. tests/fuzz/seed_replay.sh uses it to seed fuzz_replay with a round
 * that carries scripts.json and presentation records. Unset, it does
 * nothing, and a failed copy does not fail the case. */
static void lvpKeepCopy(const char *from) {
    const char *to = getenv("WB_KEEP_WBV");
    FILE       *in;
    FILE       *out;
    char        buf[4096];
    size_t      n;

    if (to == NULL || to[0] == '\0') return;
    in = fopen(from, "rb");
    if (in == NULL) return;
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) break;
    }
    fclose(out);
    fclose(in);
}

int run_lv_presentation_scripted_round(void) {
    LvpScripted seen;
    char        path[512];
    bool        recorded;
    bool        played = false;

    memset(&seen, 0, sizeof(seen));
    seen.panelParse = -1;
    recorded = lvScriptsRecordPresentationRound("presentation", path,
                                                sizeof(path));
    if (recorded) {
        played = replayHarnessDecodeFileThen(path, lvpReadScripted, &seen);
        lvpKeepCopy(path);
    }
    if (path[0] != '\0') remove(path);
    UT_ASSERT_MSG(recorded, "the presentation round could not be recorded");
    UT_ASSERT_MSG(played, "the presentation round did not play to end-of-log");

    UT_ASSERT_MSG(seen.panelSet, "everyone's panel row is not set");
    UT_ASSERT_MSG(seen.panelParse == SCN_PANEL_OK,
                  "the stored list does not parse (%d)", seen.panelParse);
    UT_ASSERT_MSG(seen.panelCount == 1 && seen.panelOp == SCN_PANEL_OP_RECT,
                  "the stored list holds %d primitives, the first op %d "
                  "(want one rect)", seen.panelCount, seen.panelOp);
    UT_ASSERT_MSG(seen.scoreValid, "team %d's score is not valid",
                  LVSF_PRES_TEAM);
    UT_ASSERT_MSG(seen.score == LVSF_PRES_SCORE &&
                  strcmp(seen.label, LVSF_PRES_LABEL) == 0,
                  "team %d's score is %d \"%s\" (want %d \"%s\")",
                  LVSF_PRES_TEAM, (int)seen.score, seen.label,
                  LVSF_PRES_SCORE, LVSF_PRES_LABEL);
    UT_ASSERT_MSG(seen.lineSet && strcmp(seen.line, LVSF_PRES_LINE) == 0,
                  "the line is \"%s\" (want \"%s\")", seen.line,
                  LVSF_PRES_LINE);
    UT_ASSERT_MSG(seen.lineMs >= 40,
                  "the line landed at %u ms (want 40 ms or later, so there "
                  "is a frame before it to seek to)", (unsigned)seen.lineMs);
    UT_ASSERT_MSG(!seen.panelBefore && !seen.lineBefore,
                  "a seek to before the records left the panel (%d) or the "
                  "line (%d) up", (int)seen.panelBefore, (int)seen.lineBefore);
    UT_ASSERT_MSG(seen.panelAt,
                  "a seek forward onto the records left the panel empty");
    return 0;
}

/* ── 4. Rebuilding from many records ───────────────────────────────── */

/* One frame of everyone's panel holding a single bar:
 *   bar x=16 y=32 w=64 h=8 colour=5 value=0x0000 max=0xFFFF
 * Record i sets the value, bytes 16 and 17 of the frame, to i, so no two
 * records hold the same list. */
static const uint8_t kLvpBarFrame[] = {
    0x03, 0x01, 0x3E, 0x00, 0x0F,
        0x00, 0x00, 0xFF, 0x00, 0x0A,
        0x06, 0x10, 0x20, 0x40, 0x08, 0x05, 0x00, 0x00, 0xFF, 0xFF,
};
#define LVP_BAR_VALUE_AT 16

/* Everyone's panel with a list scnPanelParse refuses: opcode 0. */
static const uint8_t kLvpBadListFrame[] = {
    0x03, 0x01, 0x3E, 0x00, 0x06,
        0x00, 0x00, 0xFF, 0x00, 0x01,
        0x00,
};

/* NOEVENTS 2, then LOG_QUIT. */
static const uint8_t kLvpManyEnd[] = {0x01, 0x02, 0x00};

/* Records 0..LVP_MANY-1 land on ticks 0..LVP_MANY-1, record i at
 * (i + 1) * 20 ms. The refused one lands on tick LVP_MANY, and LOG_QUIT
 * on tick LVP_MANY + 3. */
#define LVP_MANY        2000
#define LVP_MANY_MID    1000
#define LVP_MANY_END_MS ((uint32_t)(LVP_MANY + 4 + 1) * 20u)

/* Does everyone's panel hold record i's list? */
static bool lvpHoldsBar(int i) {
    uint8_t list[10];

    memcpy(list, kLvpBarFrame + 10, sizeof(list));
    list[LVP_BAR_VALUE_AT - 10]     = (uint8_t)(i >> 8);
    list[LVP_BAR_VALUE_AT - 10 + 1] = (uint8_t)(i & 0xFF);
    return lvpRowHolds(lv_screenGetPanelRow(0, 0xFF), list, sizeof(list));
}

int run_lv_presentation_rebuild_many(void) {
    const char     *path = "lv_presentation_many.wbv";
    static LvpLog   b;
    uint8_t         frame[sizeof(kLvpBarFrame)];
    LogViewerState *lv;
    bool            atEnd;
    bool            atMid;
    bool            before;
    uint32_t        midMs = (uint32_t)(LVP_MANY_MID + 1) * 20u;
    int             i;

    lvpPutOpening(&b);
    memcpy(frame, kLvpBarFrame, sizeof(frame));
    for (i = 0; i < LVP_MANY; i++) {
        frame[LVP_BAR_VALUE_AT]     = (uint8_t)(i >> 8);
        frame[LVP_BAR_VALUE_AT + 1] = (uint8_t)(i & 0xFF);
        lvpPut(&b, frame, sizeof(frame));
    }
    lvpPut(&b, kLvpBadListFrame, sizeof(kLvpBadListFrame));
    lvpPut(&b, kLvpManyEnd, sizeof(kLvpManyEnd));

    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }

    /* Forward to the end: the last good list, the refused one ignored. */
    lv_screenSeekToTimeMs(LVP_MANY_END_MS);
    atEnd = lvpHoldsBar(LVP_MANY - 1);

    /* Back to the middle: that tick's list. */
    lv_screenSeekToTimeMs(midMs);
    atMid = lv_screenGetTimeRunning() == midMs && lvpHoldsBar(LVP_MANY_MID);

    /* Back to before the first record: nothing. */
    lv_screenSeekToTimeMs(0);
    before = lvpRowEmpty(lv_screenGetPanelRow(0, 0xFF));

    lv_decoderDestroy(lv);
    remove(path);
    UT_ASSERT_MSG(atEnd, "after seeking to the end, everyone's panel is not "
                  "record %d's list", LVP_MANY - 1);
    UT_ASSERT_MSG(atMid, "after seeking to %u ms, everyone's panel is not "
                  "record %d's list", (unsigned)midMs, LVP_MANY_MID);
    UT_ASSERT_MSG(before, "after seeking to before the first record, "
                  "everyone's panel is set");
    return 0;
}

/* ── 5. A plain round ──────────────────────────────────────────────── */

typedef struct {
    int filled;   /* stores that hold something */
} LvpPlain;

static void lvpReadPlain(void *ctx) {
    LvpPlain *plain = (LvpPlain *)ctx;
    int       i;

    for (i = 0; i < LV_PRES_TEAMS; i++) {
        if (lv_screenGetPanelRow((BYTE)i, 0xFF)->set) plain->filled++;
        if (i > 0 && lv_screenGetScore(SCN_SCORE_KIND_TEAM, (BYTE)i)->valid) {
            plain->filled++;
        }
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if (lv_screenGetPanelRow(0, (BYTE)i)->set) plain->filled++;
        if (lv_screenGetScore(SCN_SCORE_KIND_PLAYER, (BYTE)i)->valid) {
            plain->filled++;
        }
    }
    for (i = 0; i < SCN_MARKERS_MAX; i++) {
        if (lv_screenGetMarker((BYTE)i)->set) plain->filled++;
    }
    if (lv_screenGetAnnounce()->set) plain->filled++;
}

int run_lv_presentation_plain_round(void) {
    ReplayHarness h;
    LvpPlain      plain;
    bool          recorded;
    bool          played = false;

    memset(&plain, 0, sizeof(plain));
    memset(&h, 0, sizeof(h));
    recorded = replayHarnessStartRecording(&h, "lv_presentation_plain",
                                           "Tester");
    if (recorded) {
        replayHarnessTick(&h, 6);
        recorded = replayHarnessStopRecording(&h);
    }
    if (recorded) {
        played = replayHarnessDecodeFileThen(h.path, lvpReadPlain, &plain);
    }
    replayHarnessStop(&h);
    UT_ASSERT_MSG(recorded, "the plain round could not be recorded");
    UT_ASSERT_MSG(played, "the plain round did not play to end-of-log");
    UT_ASSERT_MSG(plain.filled == 0,
                  "%d stores hold something in a plain round", plain.filled);
    return 0;
}

/* ── 6. Which row the panel draws ──────────────────────────────────── */

/* Set a row as a record would have left it at ms: a list when set is TRUE,
 * a clear when it is FALSE. */
static void lvpWriteRow(LvPresPanelRow *row, bool set, uint32_t ms) {
    memset(row, 0, sizeof(*row));
    row->written = TRUE;
    row->set     = set;
    row->ms      = ms;
    if (set) {
        row->len = (uint16_t)sizeof(kLvpListA);
        memcpy(row->bytes, kLvpListA, sizeof(kLvpListA));
    }
}

int run_lv_presentation_panel_choice(void) {
    static LvPresPanelRow e, t, s;

    /* Nothing written: nothing to draw. */
    memset(&e, 0, sizeof(e));
    memset(&t, 0, sizeof(t));
    memset(&s, 0, sizeof(s));
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == NULL,
                  "three unwritten rows drew something");
    UT_ASSERT_MSG(lv_screenChoosePanelRow(NULL, NULL, NULL) == NULL,
                  "no candidates drew something");

    /* The newest wins, whichever row it is. */
    lvpWriteRow(&e, TRUE, 10);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == &e,
                  "the only written row was not drawn");
    lvpWriteRow(&s, TRUE, 30);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == &s,
                  "a newer slot row lost to an older everyone row");
    lvpWriteRow(&t, TRUE, 40);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == &t,
                  "a newer team row lost to an older slot row");
    lvpWriteRow(&e, TRUE, 50);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == &e,
                  "a newer everyone row lost to an older team row");

    /* A newer clear draws nothing, even over an older list. */
    lvpWriteRow(&s, FALSE, 60);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == NULL,
                  "a newer clear on the slot row still drew a list");

    /* Equal ms: slot, then team, then everyone. */
    lvpWriteRow(&e, TRUE, 70);
    lvpWriteRow(&t, TRUE, 70);
    lvpWriteRow(&s, TRUE, 70);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == &s,
                  "on a tie the slot row did not win");
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, NULL) == &t,
                  "on a tie the team row did not beat the everyone row");
    lvpWriteRow(&s, FALSE, 70);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == NULL,
                  "a clear on the slot row at the same ms did not win");

    /* A row no record has written is ignored, whatever else it holds. */
    lvpWriteRow(&e, TRUE, 0);
    memset(&s, 0, sizeof(s));
    s.ms  = 100;
    s.set = TRUE;
    memset(&t, 0, sizeof(t));
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, &t, &s) == &e,
                  "an unwritten row beat a written one");

    /* A team that is not known is passed as no candidate: the newest team
       list is not drawn, and the everyone and slot rows compete alone. */
    lvpWriteRow(&e, TRUE, 10);
    lvpWriteRow(&t, TRUE, 90);
    lvpWriteRow(&s, TRUE, 20);
    UT_ASSERT_MSG(lv_screenChoosePanelRow(&e, NULL, &s) == &s,
                  "with no team candidate the slot row was not drawn");
    return 0;
}

/* ── 7. A slot's team from log_TeamSet ─────────────────────────────── */

/* Every line lv_messageAdd posts, counted by test_logviewer_stubs.c. */
extern int g_lvStubEventsAdded;

/* After the opening:
 *   tick 0   team 2's panel, list C                              20 ms
 *   tick 1   log_TeamSet: slot 1 onto team 2                     40 ms
 *   ticks 2-5 NOEVENTS 3
 *   tick 6   LOG_QUIT                                           140 ms */
static const uint8_t kLvpTeamStream[] = {
    0x03, 0x01, 0x3E, 0x00, 0x0C,
        0x00, 0x02, 0xFF, 0x00, 0x07,
        0x01, 0x0A, 0x0A, 0x14, 0x14, 0x06, 0x01,
    0x03, 0x01, 0x29, 0x00, 0x02,
        0x01, 0x02,
    0x01, 0x03,
    0x00,
};

/* Inside the NOEVENTS run: past the log_TeamSet, before LOG_QUIT. */
#define LVP_TEAM_MID_MS 100u

/* Slot 1's team, or -1 while it is not known. */
static int lvpSlot1Team(void) {
    BYTE team = 0xEE;
    return lv_screenGetSlotTeam(1, &team) ? (int)team : -1;
}

/* The row the panel draws for slot 1, following it as the overview does. */
static const LvPresPanelRow *lvpFollowSlot1(void) {
    lv_playersSetSelf(1);
    return lv_screenFollowedPanelRow();
}

int run_lv_presentation_slot_team(void) {
    const char     *path = "lv_presentation_team.wbv";
    static LvpLog   b;
    LogViewerState *lv;
    int             linesPlayed, linesBack, linesForward;
    int             teamEnd, teamAt20, teamAt0, teamForward, teamOther;
    bool            drawnEnd, drawnAt20, drawnAt0;
    bool            rowAt20;
    int             before;

    UT_ASSERT(log_TeamSet == 0x29 && log_ScnPanel == 0x3E);

    lvpPutOpening(&b);
    lvpPut(&b, kLvpTeamStream, sizeof(kLvpTeamStream));
    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }

    /* Played past the log_TeamSet, short of LOG_QUIT, which posts a line of
       its own: the team's line is the only one. Then on to the end. */
    before = g_lvStubEventsAdded;
    {
        int steps = 0;
        while (lv_screenIsPlaying() == TRUE &&
               lv_screenGetTimeRunning() < LVP_TEAM_MID_MS && steps < 64) {
            lv_screenLogTick();
            steps++;
        }
    }
    linesPlayed = g_lvStubEventsAdded - before;
    if (!lvpPlayToEnd()) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("playback did not reach end-of-log");
    }
    teamEnd     = lvpSlot1Team();
    teamOther   = lv_screenGetSlotTeam(0, NULL) ? 1 : 0;
    drawnEnd    = lvpRowHolds(lvpFollowSlot1(), kLvpListC, sizeof(kLvpListC));

    /* Back to the panel's tick, before the log_TeamSet: the team-2 row is
       there, but slot 1 is on no team anyone knows of, so it is not drawn. */
    before    = g_lvStubEventsAdded;
    lv_screenSeekToTimeMs(20);
    linesBack = g_lvStubEventsAdded - before;
    teamAt20  = lvpSlot1Team();
    rowAt20   = lvpRowHolds(lv_screenGetPanelRow(2, 0xFF), kLvpListC,
                            sizeof(kLvpListC));
    drawnAt20 = lvpFollowSlot1() != NULL;

    /* Forward past it again: playback passes the log_TeamSet and posts its
       line once; the rebuild after it posts none. */
    before       = g_lvStubEventsAdded;
    lv_screenSeekToTimeMs(LVP_TEAM_MID_MS);
    linesForward = g_lvStubEventsAdded - before;
    teamForward  = lvpSlot1Team();

    /* Back to the start: nothing known, nothing drawn. */
    lv_screenSeekToTimeMs(0);
    teamAt0  = lvpSlot1Team();
    drawnAt0 = lvpFollowSlot1() != NULL;

    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(linesPlayed == 1, "playback posted %d lines (want the "
                  "log_TeamSet's one)", linesPlayed);
    UT_ASSERT_MSG(teamEnd == 2, "at end-of-log slot 1's team is %d (want 2)",
                  teamEnd);
    UT_ASSERT_MSG(teamOther == 0, "slot 0's team is known with no record "
                  "naming it");
    UT_ASSERT_MSG(drawnEnd, "at end-of-log slot 1 is not drawn team 2's "
                  "panel");
    UT_ASSERT_MSG(linesBack == 0, "the seek back to 20 ms posted %d lines",
                  linesBack);
    UT_ASSERT_MSG(teamAt20 == -1, "before the log_TeamSet slot 1's team is "
                  "%d (want unknown)", teamAt20);
    UT_ASSERT_MSG(rowAt20, "at 20 ms team 2's panel is not list C");
    UT_ASSERT_MSG(!drawnAt20, "slot 1 was drawn team 2's panel before it "
                  "joined the team");
    UT_ASSERT_MSG(linesForward == 1, "the seek forward posted %d lines (want "
                  "playback's one; the rebuild posts none)", linesForward);
    UT_ASSERT_MSG(teamForward == 2, "after the seek forward slot 1's team is "
                  "%d (want 2)", teamForward);
    UT_ASSERT_MSG(teamAt0 == -1, "at the start slot 1's team is %d (want "
                  "unknown)", teamAt0);
    UT_ASSERT_MSG(!drawnAt0, "a panel is drawn before any record");
    return 0;
}

/* ── 8. Announcements on the newswire ──────────────────────────────── */

/* After the opening:
 *   tick 0   "Go!" to team 2 for 250 ticks                        20 ms
 *   tick 1   "Hi" to everyone for 100 ticks                       40 ms
 *   tick 2   the line cleared                                     60 ms
 *   ticks 3-6 NOEVENTS 3
 *   tick 7   LOG_QUIT                                            160 ms
 * The unit binary's lang table answers "?" for every string, so what is
 * counted is the lines, not their words. */
static const uint8_t kLvpAnnounceStream[] = {
    0x03, 0x01, 0x40, 0x00, 0x08,
        0x02, 0xFF, 0x00, 0xFA, 0x03, 0x47, 0x6F, 0x21,
    0x03, 0x01, 0x40, 0x00, 0x07,
        0x00, 0xFF, 0x00, 0x64, 0x02, 0x48, 0x69,
    0x03, 0x01, 0x40, 0x00, 0x05,
        0x00, 0xFF, 0x00, 0x00, 0x00,
    0x01, 0x03,
    0x00,
};

/* Past the clear, before LOG_QUIT, which posts a line of its own. */
#define LVP_ANNOUNCE_MID_MS 120u

int run_lv_presentation_announce_posts(void) {
    const char     *path = "lv_presentation_announce.wbv";
    static LvpLog   b;
    LogViewerState *lv;
    int             linesPlayed, linesBack, linesForward;
    int             before;
    bool            clearedAtMid;

    UT_ASSERT(log_ScnAnnounce == 0x40);

    lvpPutOpening(&b);
    lvpPut(&b, kLvpAnnounceStream, sizeof(kLvpAnnounceStream));
    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }

    /* Played past both lines and the clear: one line each, none for the
       clear. */
    before = g_lvStubEventsAdded;
    {
        int steps = 0;
        while (lv_screenIsPlaying() == TRUE &&
               lv_screenGetTimeRunning() < LVP_ANNOUNCE_MID_MS && steps < 64) {
            lv_screenLogTick();
            steps++;
        }
    }
    linesPlayed  = g_lvStubEventsAdded - before;
    clearedAtMid = !lv_screenGetAnnounce()->set;

    /* Back before the first line: the rebuild stores nothing and posts
       nothing. */
    before    = g_lvStubEventsAdded;
    lv_screenSeekToTimeMs(0);
    linesBack = g_lvStubEventsAdded - before;

    /* Forward past them again: playback posts the two lines once more, and
       the rebuild after it posts none of its own. */
    before       = g_lvStubEventsAdded;
    lv_screenSeekToTimeMs(LVP_ANNOUNCE_MID_MS);
    linesForward = g_lvStubEventsAdded - before;

    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(linesPlayed == 2, "playback posted %d lines (want one for "
                  "each announcement and none for the clear)", linesPlayed);
    UT_ASSERT_MSG(clearedAtMid, "the line is still stored after its clear");
    UT_ASSERT_MSG(linesBack == 0, "the seek back posted %d lines", linesBack);
    UT_ASSERT_MSG(linesForward == 2, "the seek forward posted %d lines (want "
                  "playback's two; the rebuild posts none)", linesForward);
    return 0;
}

/* ── 8b. The status line, and an announcement with a position ─────── */

/* After the opening:
 *   tick 0   status "Wave 1/5" to everyone, counting to tick 4096   20 ms
 *   tick 1   "Hi" to everyone for 100 ticks at the top: the two
 *            position bytes, 127 and 0, after the text              40 ms
 *   tick 2   the status line cleared                                60 ms
 *   ticks 3-6 NOEVENTS 3
 *   tick 7   LOG_QUIT                                              160 ms */
static const uint8_t kLvpStatusStream[] = {
    0x03, 0x01, 0x44, 0x00, 0x0F,
        0x00, 0xFF, 0x00, 0x00, 0x10, 0x00,
        0x08, 0x57, 0x61, 0x76, 0x65, 0x20, 0x31, 0x2F, 0x35,
    0x03, 0x01, 0x40, 0x00, 0x09,
        0x00, 0xFF, 0x00, 0x64, 0x02, 0x48, 0x69, 0x7F, 0x00,
    0x03, 0x01, 0x44, 0x00, 0x07,
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    0x01, 0x03,
    0x00,
};

int run_lv_presentation_status_line(void) {
    const char          *path = "lv_presentation_status.wbv";
    static LvpLog        b;
    LogViewerState      *lv;
    const LvPresStatus  *st;
    bool                 upAt40, heldText, heldEnds, annAt40, goneAt120;
    bool                 backAt40, rowSet;

    UT_ASSERT(log_ScnStatus == 0x44);

    lvpPutOpening(&b);
    lvpPut(&b, kLvpStatusStream, sizeof(kLvpStatusStream));
    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }

    lv_screenSeekToTimeMs(40);
    st       = lv_screenFollowedStatus();
    upAt40   = (st != NULL);
    heldText = (st != NULL && strcmp(st->text, "Wave 1/5") == 0);
    heldEnds = (st != NULL && st->endsAt == 4096u);
    rowSet   = (lv_screenGetStatusRow(0, 0xFF) != NULL &&
                lv_screenGetStatusRow(0, 0xFF)->set);
    /* The positioned announcement is read with its position, and the
       status record after it from the right byte. */
    annAt40  = (lv_screenGetAnnounce()->set &&
                strcmp(lv_screenGetAnnounce()->text, "Hi") == 0 &&
                lv_screenGetAnnounce()->hasPos &&
                lv_screenGetAnnounce()->posX == 127 &&
                lv_screenGetAnnounce()->posY == 0);

    lv_screenSeekToTimeMs(120);
    goneAt120 = (lv_screenFollowedStatus() == NULL);

    lv_screenSeekToTimeMs(40);
    backAt40 = (lv_screenFollowedStatus() != NULL);

    /* Played through to the end from the start: nothing refused. */
    lv_screenSeekToTimeMs(0);
    UT_ASSERT_MSG(lvpPlayToEnd(), "playback did not reach the end");

    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(upAt40, "no status line at 40 ms");
    UT_ASSERT_MSG(rowSet, "everyone's status row is not set at 40 ms");
    UT_ASSERT_MSG(heldText, "the status line holds the wrong text");
    UT_ASSERT_MSG(heldEnds, "the status line counts to the wrong tick");
    UT_ASSERT_MSG(annAt40, "the positioned announcement was not read");
    UT_ASSERT_MSG(goneAt120, "the status line is still up after its clear");
    UT_ASSERT_MSG(backAt40, "a seek back did not rebuild the status line");
    return 0;
}

/* ── 8c. Where a recorded announcement goes ────────────────────────── */

/* After the opening:
 *   tick 0   "Go!" with no position                                  20 ms
 *   tick 1   "Hi" at (30, 240)                                       40 ms
 *   tick 2   "Up" at (255, 16) and one byte more a later build
 *            might add: 255 reads as 254, the extra byte is skipped  60 ms
 *   tick 3   "No" and one stray byte: too few for a position         80 ms
 *   ticks 4-7 NOEVENTS 3
 *   tick 8   LOG_QUIT                                               180 ms */
static const uint8_t kLvpAnnouncePosStream[] = {
    0x03, 0x01, 0x40, 0x00, 0x08,
        0x00, 0xFF, 0x00, 0x64, 0x03, 0x47, 0x6F, 0x21,
    0x03, 0x01, 0x40, 0x00, 0x09,
        0x00, 0xFF, 0x00, 0x64, 0x02, 0x48, 0x69, 0x1E, 0xF0,
    0x03, 0x01, 0x40, 0x00, 0x0A,
        0x00, 0xFF, 0x00, 0x64, 0x02, 0x55, 0x70, 0xFF, 0x10, 0x55,
    0x03, 0x01, 0x40, 0x00, 0x08,
        0x00, 0xFF, 0x00, 0x64, 0x02, 0x4E, 0x6F, 0x33,
    0x01, 0x03,
    0x00,
};

/* The announcement stored at ms, as text and position: hasPos is -1 when
   nothing is stored. */
typedef struct {
    char text[8];
    int  hasPos;
    int  x;
    int  y;
} LvpAnnSeen;

static LvpAnnSeen lvpAnnAt(uint32_t ms) {
    LvpAnnSeen           seen;
    const LvPresAnnounce *a;

    memset(&seen, 0, sizeof(seen));
    lv_screenSeekToTimeMs(ms);
    a = lv_screenGetAnnounce();
    if (!a->set) {
        seen.hasPos = -1;
        return seen;
    }
    snprintf(seen.text, sizeof(seen.text), "%s", a->text);
    seen.hasPos = a->hasPos ? 1 : 0;
    seen.x      = a->posX;
    seen.y      = a->posY;
    return seen;
}

int run_lv_presentation_announce_position(void) {
    const char     *path = "lv_presentation_announce_pos.wbv";
    static LvpLog   b;
    LogViewerState *lv;
    LvpAnnSeen      at20, at40, at60, at80, back40, back20;
    bool            playedPos = false;

    lvpPutOpening(&b);
    lvpPut(&b, kLvpAnnouncePosStream, sizeof(kLvpAnnouncePosStream));
    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }

    /* Played through from the start: the line at 40 ms has its position
       as playback reads it, not only as a rebuild does. */
    {
        int steps = 0;
        while (lv_screenIsPlaying() == TRUE && steps < 64 &&
               !(lv_screenGetAnnounce()->set &&
                 strcmp(lv_screenGetAnnounce()->text, "Hi") == 0)) {
            lv_screenLogTick();
            steps++;
        }
        playedPos = lv_screenGetAnnounce()->set &&
                    lv_screenGetAnnounce()->hasPos &&
                    lv_screenGetAnnounce()->posX == 30 &&
                    lv_screenGetAnnounce()->posY == 240;
    }

    /* Seeks forward, then back: each rebuild reads the position from the
       record's own framed length. */
    at20   = lvpAnnAt(20);
    at40   = lvpAnnAt(40);
    at60   = lvpAnnAt(60);
    at80   = lvpAnnAt(80);
    back40 = lvpAnnAt(40);
    back20 = lvpAnnAt(20);

    UT_ASSERT_MSG(lvpPlayToEnd(), "playback did not reach the end");
    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(playedPos, "playback lost the position of the line at 40 ms");
    UT_ASSERT_MSG(strcmp(at20.text, "Go!") == 0 && at20.hasPos == 0,
                  "at 20 ms: '%s' hasPos %d", at20.text, at20.hasPos);
    UT_ASSERT_MSG(strcmp(at40.text, "Hi") == 0 && at40.hasPos == 1 &&
                      at40.x == 30 && at40.y == 240,
                  "at 40 ms: '%s' hasPos %d at (%d, %d)", at40.text,
                  at40.hasPos, at40.x, at40.y);
    UT_ASSERT_MSG(strcmp(at60.text, "Up") == 0 && at60.hasPos == 1 &&
                      at60.x == (int)SCN_ANNOUNCE_POS_MAX && at60.y == 16,
                  "at 60 ms: '%s' hasPos %d at (%d, %d)", at60.text,
                  at60.hasPos, at60.x, at60.y);
    UT_ASSERT_MSG(strcmp(at80.text, "No") == 0 && at80.hasPos == 0,
                  "at 80 ms: '%s' hasPos %d", at80.text, at80.hasPos);
    UT_ASSERT_MSG(back40.hasPos == 1 && back40.x == 30 && back40.y == 240,
                  "a seek back to 40 ms rebuilt hasPos %d at (%d, %d)",
                  back40.hasPos, back40.x, back40.y);
    UT_ASSERT_MSG(back20.hasPos == 0 && strcmp(back20.text, "Go!") == 0,
                  "a seek back to 20 ms kept a position");
    return 0;
}

/* ── 9. Which markers the followed player sees ─────────────────────── */

/* A marker as a record would have left it. */
static LvPresMarker lvpMarker(BYTE destTeam, BYTE destPlayer) {
    LvPresMarker m;

    memset(&m, 0, sizeof(m));
    m.set        = TRUE;
    m.kind       = SCN_MARKER_KIND_SQUARE;
    m.destTeam   = destTeam;
    m.destPlayer = destPlayer;
    m.x          = 10;
    m.y          = 12;
    m.colour     = 5;
    return m;
}

int run_lv_presentation_marker_visible(void) {
    const char     *path = "lv_presentation_marker.wbv";
    static LvpLog   b;
    LogViewerState *lv;
    LvPresMarker    everyone = lvpMarker(0, 0xFF);
    LvPresMarker    team2    = lvpMarker(2, 0xFF);
    LvPresMarker    team3    = lvpMarker(3, 0xFF);
    LvPresMarker    slot1    = lvpMarker(0, 1);
    LvPresMarker    unset    = lvpMarker(0, 0xFF);
    bool            everyoneShown, everyoneNobody, unsetHidden;
    bool            team2Slot1, team2Slot0, team3Slot1, team2Unknown;
    bool            slot1Slot1, slot1Slot0, slot1Nobody;

    unset.set = FALSE;

    /* kLvpTeamStream puts slot 1 on team 2 at 40 ms. */
    lvpPutOpening(&b);
    lvpPut(&b, kLvpTeamStream, sizeof(kLvpTeamStream));
    lv = lvpOpen(&b, path);
    if (lv == NULL) {
        remove(path);
        UT_FAIL("the hand-built log could not be loaded");
    }
    if (!lvpPlayToEnd()) {
        lv_decoderDestroy(lv);
        remove(path);
        UT_FAIL("playback did not reach end-of-log");
    }

    everyoneShown  = lv_screenMarkerVisible(&everyone, 1);
    everyoneNobody = lv_screenMarkerVisible(&everyone, NEUTRAL);
    unsetHidden    = !lv_screenMarkerVisible(&unset, 1) &&
                     !lv_screenMarkerVisible(NULL, 1);
    team2Slot1     = lv_screenMarkerVisible(&team2, 1);
    team2Slot0     = lv_screenMarkerVisible(&team2, 0);
    team3Slot1     = lv_screenMarkerVisible(&team3, 1);
    slot1Slot1     = lv_screenMarkerVisible(&slot1, 1);
    slot1Slot0     = lv_screenMarkerVisible(&slot1, 0);
    slot1Nobody    = lv_screenMarkerVisible(&slot1, NEUTRAL);

    /* Before the log_TeamSet slot 1's team is not known. */
    lv_screenSeekToTimeMs(20);
    team2Unknown = lv_screenMarkerVisible(&team2, 1);

    lv_decoderDestroy(lv);
    remove(path);

    UT_ASSERT_MSG(everyoneShown && everyoneNobody,
                  "a marker to everyone was hidden");
    UT_ASSERT_MSG(unsetHidden, "an unset marker was shown");
    UT_ASSERT_MSG(team2Slot1, "team 2's marker was hidden from slot 1 on "
                  "team 2");
    UT_ASSERT_MSG(!team2Slot0, "team 2's marker was shown to slot 0, whose "
                  "team is not known");
    UT_ASSERT_MSG(!team3Slot1, "team 3's marker was shown to slot 1 on "
                  "team 2");
    UT_ASSERT_MSG(!team2Unknown, "team 2's marker was shown to slot 1 before "
                  "it joined the team");
    UT_ASSERT_MSG(slot1Slot1, "slot 1's marker was hidden from slot 1");
    UT_ASSERT_MSG(!slot1Slot0 && !slot1Nobody,
                  "slot 1's marker was shown to another slot or to nobody");
    return 0;
}

/* ── A live feed ───────────────────────────────────────────────────── */

/* kLvpStream as a live feed that has caught up with its head: the stream
   without its LOG_QUIT, loaded as a stream, in live mode and played to the
   last byte. The feed state is module state in screen.c, so each case turns
   live mode off again before it destroys the decoder. */
static LogViewerState *lvpOpenLive(LvpLog *b) {
    LogViewerState *lv;

    lvpPutOpening(b);
    lvpPut(b, kLvpStream, sizeof(kLvpStream) - 1);
    lv = lv_decoderCreate(false);
    if (lv == NULL) return NULL;
    lv_screenSetSizeX(30);
    lv_screenSetSizeY(30);
    if (lv_screenLoadFromStream(b->buf, b->len) != TRUE) {
        lv_decoderDestroy(lv);
        return NULL;
    }
    lv_screenSpecSetLiveMode(TRUE);
    lv_screenSpecJumpToLive();
    return lv;
}

static void lvpCloseLive(LogViewerState *lv) {
    lv_screenSpecSetLiveMode(FALSE);
    lv_decoderDestroy(lv);
}

/* Seeking back on a live feed puts the stores back to what they held at the
 * earlier time. A feed has no load walk, so playback indexes each record as
 * it reads it and the rebuild after the seek works from that index. */
int run_lv_presentation_live_seek(void) {
    static LvpLog         b;
    LogViewerState       *lv;
    const LvPresMarker   *m;
    const char           *why = NULL;

    lv = lvpOpenLive(&b);
    UT_ASSERT_MSG(lv != NULL, "the feed could not be loaded");

    /* At the head: marker 3 was placed at 100 ms and cleared at 240 ms, and
       marker 5 placed at 160 ms. */
    m = lv_screenGetMarker(5);
    if (m == NULL || !m->set) {
        why = "marker 5 is not set at the head of the feed";
    }

    /* Back before every record. */
    if (why == NULL) {
        lv_screenSeekToTimeMs(20);
        if (lv_screenGetTimeRunning() > 100) {
            why = "the seek back did not land before the first record";
        } else if (lv_screenGetMarker(5)->set || lv_screenGetMarker(3)->set) {
            why = "a marker is still set before its record";
        } else if (!lvpRowEmpty(lv_screenGetPanelRow(0, 0xFF)) ||
                   !lvpRowEmpty(lv_screenGetPanelRow(2, 0xFF))) {
            why = "a panel is still up before its record";
        } else if (lv_screenGetScore(SCN_SCORE_KIND_TEAM, 1)->valid) {
            why = "team 1's score is still there before its record";
        } else if (lv_screenGetAnnounce()->set) {
            why = "the line is still up before its record";
        }
    }

    /* Forward to 160 ms: replayed, and indexed only once. */
    if (why == NULL) {
        lv_screenSeekToTimeMs(160);
        m = lv_screenGetMarker(3);
        if (m == NULL || !m->set || m->x != 10 || m->y != 12) {
            why = "marker 3 is not back at (10, 12) at 160 ms";
        } else if (!lvpRowHolds(lv_screenGetPanelRow(0, 0xFF), kLvpListA,
                                sizeof(kLvpListA))) {
            why = "everyone's panel is not list A at 160 ms";
        }
    }

    /* And back again, which a doubled index entry would not survive. */
    if (why == NULL) {
        lv_screenSeekToTimeMs(20);
        if (lv_screenGetMarker(3)->set ||
            !lvpRowEmpty(lv_screenGetPanelRow(0, 0xFF))) {
            why = "the second seek back left a record's store set";
        }
    }

    lvpCloseLive(lv);
    UT_ASSERT_MSG(why == NULL, "live feed: %s", why);
    return 0;
}

/* Fast Forward on a live feed that has caught up with its head returns: the
 * tick finds nothing to read, and there is no snapshot or end-of-log ahead
 * for the loop to stop at until the host appends more. */
int run_lv_presentation_live_fast_forward(void) {
    static LvpLog   b;
    LogViewerState *lv;
    uint32_t        before;
    uint32_t        after;
    bool            playing;

    lv = lvpOpenLive(&b);
    UT_ASSERT_MSG(lv != NULL, "the feed could not be loaded");
    before = lv_screenGetTimeRunning();
    lv_screenFastForward();
    after   = lv_screenGetTimeRunning();
    playing = lv_screenIsPlaying() == TRUE;
    lvpCloseLive(lv);

    UT_ASSERT_MSG(playing, "fast forward ended playback on a live feed");
    UT_ASSERT_MSG(after >= before && after - before <= 200,
                  "fast forward at the head moved playback from %u to %u ms",
                  (unsigned)before, (unsigned)after);
    return 0;
}
