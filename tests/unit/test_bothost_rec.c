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
#include <string.h>
#include <SDL3/SDL.h>

#include "bothost_rec.h"
#include "control_event.h"
#include "test_harness.h"

static int  s_tbhDone;            /* DONE events seen */
static char s_tbhDoneBlock[BOT_HOST_REC_BLOCK_LEN];
static int  s_tbhCleared;         /* CLEARED events seen */

static void tbhSendEvent(int srv, const ControlEvent *evt) {
    (void)srv;
    if (evt->type != CTRL_BOT_HOST_REC) return;
    if (evt->u.botHostRec.op == BOT_HOST_REC_OP_DONE) {
        s_tbhDone++;
        SDL_strlcpy(s_tbhDoneBlock, evt->u.botHostRec.block,
                    sizeof(s_tbhDoneBlock));
    } else if (evt->u.botHostRec.op == BOT_HOST_REC_OP_CLEARED) {
        s_tbhCleared++;
    }
}

static bool tbhBulkIdle(int srv) {
    (void)srv;
    return true;
}

static bool tbhBulkSend(int srv, const BulkStreamHeader *h,
                        const uint8_t *blob, uint32_t len) {
    (void)srv;
    (void)h;
    (void)blob;
    (void)len;
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

/* The block's host file and its print2 log (plain or gzipped) are there. */
static bool tbhBlockKept(const char *root, const char *block) {
    char p[1200], q[1200];
    snprintf(p, sizeof(p), "%s/%s/bot0_g1.hbr", root, block);
    if (!tbhExists(p)) return false;
    snprintf(p, sizeof(p), "%s/%s/print2_bot0.log", root, block);
    snprintf(q, sizeof(q), "%s/%s/print2_bot0.log.gz", root, block);
    return tbhExists(p) || tbhExists(q);
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
