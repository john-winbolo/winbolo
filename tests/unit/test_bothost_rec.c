/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Bot Host's recorder keeps its block files: an upload is a copy.
 *   - an ACK MERGED marks the block uploaded (it is not sent again), and
 *     every file stays;
 *   - an ACK FAILED leaves the block not marked, and it is sent again
 *     after the retry time;
 *   - only CLEAR (the server's cleardebug) deletes the files;
 *   - the free-disk floor stops recording and deletes nothing.
 * The server side is a mock: it takes every bulk piece and keeps the DONE
 * events.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "bothost_rec.h"
#include "control_event.h"
#include "test_harness.h"

static int  s_tbhDone;            /* DONE events seen */
static char s_tbhDoneBlock[BOT_HOST_REC_BLOCK_LEN];
static int  s_tbhCleared;         /* CLEARED events seen */
static int  s_tbhUpStates;        /* STATE UPLOADING events seen */
static bool s_tbhUpBytesBad;      /* one had no total, or sent above it */
static int  s_tbhPieces;          /* bulk pieces sent */
static int  s_tbhBigStarts;       /* pieces at offset 0 of TBH_BIG_FILE */
static uint64_t s_tbhBigBytes;    /* bytes of TBH_BIG_FILE sent */
#define TBH_BIG_FILE "bot1_g1.hbr"

static void tbhSendEvent(int srv, const ControlEvent *evt) {
    (void)srv;
    if (evt->type != CTRL_BOT_HOST_REC) return;
    if (evt->u.botHostRec.op == BOT_HOST_REC_OP_DONE) {
        s_tbhDone++;
        SDL_strlcpy(s_tbhDoneBlock, evt->u.botHostRec.block,
                    sizeof(s_tbhDoneBlock));
    } else if (evt->u.botHostRec.op == BOT_HOST_REC_OP_CLEARED) {
        s_tbhCleared++;
    } else if (evt->u.botHostRec.op == BOT_HOST_REC_OP_STATE &&
               evt->u.botHostRec.state == BOT_HOST_REC_STATE_UPLOADING) {
        s_tbhUpStates++;
        if (evt->u.botHostRec.b == 0 ||
            evt->u.botHostRec.a > evt->u.botHostRec.b) {
            s_tbhUpBytesBad = true;
        }
    }
}

static bool tbhBulkIdle(int srv) {
    (void)srv;
    return true;
}

static bool tbhBulkSend(int srv, const BulkStreamHeader *h,
                        const uint8_t *blob, uint32_t len) {
    (void)srv;
    (void)blob;
    s_tbhPieces++;
    if (strstr(h->path, "/" TBH_BIG_FILE "/") != NULL) {
        s_tbhBigBytes += len;
        /* The offsets have no leading zero: only offset 0 ends in "/0". */
        size_t n = strlen(h->path);
        if (n >= 2 && strcmp(h->path + n - 2, "/0") == 0) s_tbhBigStarts++;
    }
    return true;
}

static void tbhStart(const char *cache, double minFreeGb) {
    HostRecIo io;
    memset(&io, 0, sizeof(io));
    io.sendEvent = tbhSendEvent;
    io.bulkIdle = tbhBulkIdle;
    io.bulkSend = tbhBulkSend;
    s_tbhDone = 0;
    s_tbhDoneBlock[0] = '\0';
    s_tbhCleared = 0;
    s_tbhUpStates = 0;
    s_tbhUpBytesBad = false;
    s_tbhPieces = 0;
    s_tbhBigStarts = 0;
    s_tbhBigBytes = 0;
    hostRecInit(cache, "testhost", minFreeGb, &io);
    hostRecServerUp(0, "127.0.0.1:27680");
}

static void tbhEvent(uint8_t op, uint8_t state, uint8_t reason,
                     const char *block) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_REC;
    evt.u.botHostRec.op = op;
    evt.u.botHostRec.state = state;
    evt.u.botHostRec.percent = reason;
    if (block != NULL) {
        SDL_strlcpy(evt.u.botHostRec.block, block,
                    sizeof(evt.u.botHostRec.block));
    }
    hostRecOnEvent(0, &evt);
}

/* Run the recorder with the clock addMs ahead until DONE number wantDone
 * comes, or for about loops * 5 ms. */
static void tbhPump(uint32_t addMs, int wantDone, int loops) {
    int i;
    for (i = 0; i < loops && s_tbhDone < wantDone; i++) {
        hostRecService((uint32_t)SDL_GetTicks() + addMs);
        SDL_Delay(5);
    }
}

static bool tbhFile(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    bool ok;
    if (f == NULL) return false;
    ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

static bool tbhExists(const char *path) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi);
}

/* A finished block with one host file and one print2 log. */
static bool tbhBlock(const char *root, const char *block) {
    char dir[1100], p[1200];
    snprintf(dir, sizeof(dir), "%s/%s", root, block);
    if (!SDL_CreateDirectory(dir)) return false;
    snprintf(p, sizeof(p), "%s/bot0_g1.hbr", dir);
    if (!tbhFile(p, "not a real host recording, only bytes to send")) {
        return false;
    }
    snprintf(p, sizeof(p), "%s/print2_bot0.log", dir);
    return tbhFile(p, "===TICK 1===\nhello\n");
}

/* The block's host file and its print2 log (plain or compressed) are there. */
static bool tbhBlockKept(const char *root, const char *block) {
    char p[1200], q[1200], r[1200];
    snprintf(p, sizeof(p), "%s/%s/bot0_g1.hbr", root, block);
    if (!tbhExists(p)) return false;
    snprintf(p, sizeof(p), "%s/%s/print2_bot0.log", root, block);
    snprintf(q, sizeof(q), "%s/%s/print2_bot0.log.gz", root, block);
    snprintf(r, sizeof(r), "%s/%s/print2_bot0.log.zst", root, block);
    return tbhExists(p) || tbhExists(q) || tbhExists(r);
}

static bool tbhMarked(const char *root, const char *block) {
    char p[1200];
    snprintf(p, sizeof(p), "%s/%s/uploaded.marker", root, block);
    return tbhExists(p);
}

#define TBH_A "2026-11-04_100000"
#define TBH_B "2026-11-04_110000"
#define TBH_C "2026-11-04_120000"

int run_bothost_rec_upload_keeps_files(void) {
    char cache[1024], root[1100];

    UT_ASSERT(utScratchPath(cache, sizeof(cache), "bhcache"));
    UT_ASSERT(SDL_CreateDirectory(cache));
    tbhStart(cache, 0.0);
    snprintf(root, sizeof(root), "%s/recordings/127.0.0.1_27680", cache);
    UT_ASSERT(SDL_CreateDirectory(root));
    UT_ASSERT(tbhBlock(root, TBH_A));

    /* Upload A; the server merges it. */
    tbhEvent(BOT_HOST_REC_OP_UPLOAD, 0, 0, NULL);
    tbhPump(5000, 1, 400);
    UT_ASSERT_MSG(s_tbhDone == 1, "DONE count %d", s_tbhDone);
    UT_ASSERT(strcmp(s_tbhDoneBlock, TBH_A) == 0);
    /* Its STATE said the KiB sent and to send, for the lobby's time left. */
    UT_ASSERT_MSG(s_tbhUpStates > 0 && !s_tbhUpBytesBad,
                  "%d UPLOADING states, bad bytes %d", s_tbhUpStates,
                  (int)s_tbhUpBytesBad);
    tbhEvent(BOT_HOST_REC_OP_ACK, BOT_HOST_REC_ACK_MERGED,
             BOT_HOST_REC_FAIL_NONE, TBH_A);
    /* A copy, not a move: the files stay, marked uploaded. */
    UT_ASSERT(tbhBlockKept(root, TBH_A));
    UT_ASSERT(tbhMarked(root, TBH_A));

    /* A marked block is not sent again. */
    tbhEvent(BOT_HOST_REC_OP_UPLOAD, 0, 0, NULL);
    tbhPump(5000, 2, 60);
    UT_ASSERT_MSG(s_tbhDone == 1, "DONE count %d", s_tbhDone);
    UT_ASSERT(tbhBlockKept(root, TBH_A));

    /* Block B: the server cannot merge it. */
    UT_ASSERT(tbhBlock(root, TBH_B));
    tbhEvent(BOT_HOST_REC_OP_UPLOAD, 0, 0, NULL);
    tbhPump(5000, 2, 400);
    UT_ASSERT_MSG(s_tbhDone == 2, "DONE count %d", s_tbhDone);
    UT_ASSERT(strcmp(s_tbhDoneBlock, TBH_B) == 0);
    tbhEvent(BOT_HOST_REC_OP_ACK, BOT_HOST_REC_ACK_FAILED,
             BOT_HOST_REC_FAIL_NO_FILL, TBH_B);
    UT_ASSERT(tbhBlockKept(root, TBH_B));
    UT_ASSERT(!tbhMarked(root, TBH_B));
    /* Not again before the retry time ... */
    tbhPump(1000, 3, 40);
    UT_ASSERT_MSG(s_tbhDone == 2, "DONE count %d", s_tbhDone);
    /* ... and again after it. */
    tbhPump(20000, 3, 400);
    UT_ASSERT_MSG(s_tbhDone == 3, "DONE count %d", s_tbhDone);
    UT_ASSERT(strcmp(s_tbhDoneBlock, TBH_B) == 0);
    tbhEvent(BOT_HOST_REC_OP_ACK, BOT_HOST_REC_ACK_MERGED,
             BOT_HOST_REC_FAIL_NONE, TBH_B);
    UT_ASSERT(tbhBlockKept(root, TBH_B));
    UT_ASSERT(tbhMarked(root, TBH_B));

    /* An ACK of an earlier connection's upload marks; it deletes nothing. */
    UT_ASSERT(tbhBlock(root, TBH_C));
    tbhEvent(BOT_HOST_REC_OP_ACK, BOT_HOST_REC_ACK_MERGED,
             BOT_HOST_REC_FAIL_NONE, TBH_C);
    UT_ASSERT(tbhBlockKept(root, TBH_C));
    UT_ASSERT(tbhMarked(root, TBH_C));

    /* Only CLEAR deletes. */
    tbhEvent(BOT_HOST_REC_OP_CLEAR, 0, 0, NULL);
    UT_ASSERT(s_tbhCleared == 1);
    UT_ASSERT(!tbhBlockKept(root, TBH_A));
    UT_ASSERT(!tbhBlockKept(root, TBH_B));
    UT_ASSERT(!tbhBlockKept(root, TBH_C));
    hostRecShutdown();
    return 0;
}

int run_bothost_rec_lowdisk_keeps_files(void) {
    char cache[1024], root[1100], dir[1200];

    UT_ASSERT(utScratchPath(cache, sizeof(cache), "bhlow"));
    UT_ASSERT(SDL_CreateDirectory(cache));
    /* A floor no disk has: every block is low disk. */
    tbhStart(cache, 1e9);
    snprintf(root, sizeof(root), "%s/recordings/127.0.0.1_27680", cache);
    UT_ASSERT(SDL_CreateDirectory(root));
    UT_ASSERT(tbhBlock(root, TBH_A));

    /* The new block is not recorded, and the old block stays. */
    tbhEvent(BOT_HOST_REC_OP_OPEN, 0, 0, TBH_B);
    tbhPump(1000, 1, 20);
    snprintf(dir, sizeof(dir), "%s/%s", root, TBH_B);
    UT_ASSERT(!tbhExists(dir));
    UT_ASSERT(tbhBlockKept(root, TBH_A));
    tbhEvent(BOT_HOST_REC_OP_CLOSE, 0, 0, TBH_B);

    /* The old block can still go up, and stays after it. */
    tbhEvent(BOT_HOST_REC_OP_UPLOAD, 0, 0, NULL);
    tbhPump(5000, 1, 400);
    UT_ASSERT_MSG(s_tbhDone == 1, "DONE count %d", s_tbhDone);
    tbhEvent(BOT_HOST_REC_OP_ACK, BOT_HOST_REC_ACK_MERGED,
             BOT_HOST_REC_FAIL_NONE, TBH_A);
    UT_ASSERT(tbhBlockKept(root, TBH_A));
    UT_ASSERT(tbhMarked(root, TBH_A));
    hostRecShutdown();
    return 0;
}

/* The pool block follows the OPEN flags: JSON unless the server sets
 * BOT_HOST_REC_F_POOL_RAW (a reader that takes only JSON never does), and
 * the raw flag keeps clear of every other OPEN flag and of LOWDISK. */
int run_bothost_rec_pool_raw_flag(void) {
    const char *json = "return brain.get_pool_breakdown_json()";
    const char *e;
    UT_ASSERT(strcmp(hostRecPoolExpr(0), json) == 0);
    UT_ASSERT(strcmp(hostRecPoolExpr(BOT_HOST_REC_F_VIZ | BOT_HOST_REC_F_POOL |
                                     BOT_HOST_REC_F_PRINT2 |
                                     BOT_HOST_REC_F_JSONL), json) == 0);
    e = hostRecPoolExpr(BOT_HOST_REC_F_POOL | BOT_HOST_REC_F_POOL_RAW);
    UT_ASSERT(strstr(e, "get_pool_breakdown_raw") != NULL);
    UT_ASSERT(strstr(e, "or brain.get_pool_breakdown_json") != NULL);
    UT_ASSERT((BOT_HOST_REC_F_POOL_RAW &
               (BOT_HOST_REC_F_VIZ | BOT_HOST_REC_F_PRINT2 | BOT_HOST_REC_F_JSONL |
                BOT_HOST_REC_F_POOL | BOT_HOST_REC_F_LOWDISK)) == 0);
    return 0;
}

static void tbhUpload(uint8_t flags) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_REC;
    evt.u.botHostRec.op = BOT_HOST_REC_OP_UPLOAD;
    evt.u.botHostRec.flags = flags;
    hostRecOnEvent(0, &evt);
}

/* HOLD (a game starts) stops an upload under way where it is: no piece and
 * no DONE until the next UPLOAD, which goes on from the same place (the
 * big file's first piece goes once, and its bytes once). An older
 * server's UPLOAD with BOT_HOST_REC_F_PACED (an ask during a game) sends
 * nothing either. */
int run_bothost_rec_hold_keeps_place(void) {
    char cache[1024], root[1100], p[1300];
    const uint32_t bigSize = 20u * 64u * 1024u;   /* 20 pieces */
    int i, piecesAtHold, piecesHeld;
    uint8_t *junk;
    FILE *f;

    UT_ASSERT(utScratchPath(cache, sizeof(cache), "bhhold"));
    UT_ASSERT(SDL_CreateDirectory(cache));
    tbhStart(cache, 0.0);
    snprintf(root, sizeof(root), "%s/recordings/127.0.0.1_27680", cache);
    UT_ASSERT(SDL_CreateDirectory(root));
    UT_ASSERT(tbhBlock(root, TBH_A));
    snprintf(p, sizeof(p), "%s/%s/" TBH_BIG_FILE, root, TBH_A);
    junk = (uint8_t *)malloc(bigSize);
    UT_ASSERT(junk != NULL);
    memset(junk, 0x3C, bigSize);
    f = fopen(p, "wb");
    UT_ASSERT(f != NULL);
    UT_ASSERT(fwrite(junk, 1, bigSize, f) == bigSize);
    UT_ASSERT(fclose(f) == 0);
    free(junk);

    /* The upload starts; stop it after the big file's first pieces. */
    tbhUpload(0);
    for (i = 0; i < 400 && s_tbhBigBytes == 0; i++) {
        hostRecService((uint32_t)SDL_GetTicks());
        if (s_tbhBigBytes == 0) SDL_Delay(5);
    }
    UT_ASSERT_MSG(s_tbhBigBytes > 0 && s_tbhBigBytes < bigSize,
                  "big file bytes %llu before HOLD",
                  (unsigned long long)s_tbhBigBytes);
    UT_ASSERT(s_tbhDone == 0);

    /* A game starts. */
    tbhEvent(BOT_HOST_REC_OP_HOLD, 0, 0, NULL);
    piecesAtHold = s_tbhPieces;
    for (i = 0; i < 40; i++) {
        hostRecService((uint32_t)SDL_GetTicks() + 30000u);
        hostRecPumpUploads();
    }
    piecesHeld = s_tbhPieces - piecesAtHold;
    UT_ASSERT_MSG(piecesHeld == 0, "%d pieces sent while held", piecesHeld);
    UT_ASSERT(s_tbhDone == 0);

    /* The game is over: the upload goes on from where it stopped. */
    tbhUpload(0);
    tbhPump(0, 1, 400);
    UT_ASSERT_MSG(s_tbhDone == 1, "DONE count %d", s_tbhDone);
    UT_ASSERT(strcmp(s_tbhDoneBlock, TBH_A) == 0);
    UT_ASSERT_MSG(s_tbhBigStarts == 1, "big file started %d times",
                  s_tbhBigStarts);
    UT_ASSERT_MSG(s_tbhBigBytes == bigSize, "big file bytes %llu of %u",
                  (unsigned long long)s_tbhBigBytes, (unsigned)bigSize);
    tbhEvent(BOT_HOST_REC_OP_ACK, BOT_HOST_REC_ACK_MERGED,
             BOT_HOST_REC_FAIL_NONE, TBH_A);
    UT_ASSERT(tbhMarked(root, TBH_A));

    /* An older server's ask during a game (PACED): nothing goes ... */
    UT_ASSERT(tbhBlock(root, TBH_B));
    piecesAtHold = s_tbhPieces;
    tbhUpload(BOT_HOST_REC_F_PACED);
    tbhPump(5000, 2, 40);
    UT_ASSERT_MSG(s_tbhPieces == piecesAtHold, "%d pieces on a PACED ask",
                  s_tbhPieces - piecesAtHold);
    UT_ASSERT(s_tbhDone == 1);
    /* ... until its lobby asks without the flag. */
    tbhUpload(0);
    tbhPump(5000, 2, 400);
    UT_ASSERT_MSG(s_tbhDone == 2, "DONE count %d", s_tbhDone);
    UT_ASSERT(strcmp(s_tbhDoneBlock, TBH_B) == 0);

    tbhEvent(BOT_HOST_REC_OP_CLEAR, 0, 0, NULL);
    hostRecShutdown();
    return 0;
}
