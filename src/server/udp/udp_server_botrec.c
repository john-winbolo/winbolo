/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Bot Host recordings
 *Filename:      udp_server_botrec.c
 *Purpose:
 *  The server side of the Bot Hosts' brain recordings
 *  (-brain-debug). A bot whose brain runs on a Bot Host has
 *  no brain here, so brainrec.btr holds only a hosted marker
 *  for it (brain_record_host.h). The host records the bot
 *  itself, into a folder named like the server's session
 *  block, and sends the finished blocks back:
 *
 *    - each session block that opens or closes is told to
 *      every connected host (CTRL_BOT_HOST_REC OPEN / CLOSE);
 *    - back in the lobby the hosts are asked to UPLOAD; while
 *      a game runs (a map rotation) they send slowly;
 *    - a host sends each file of a block in
 *      BULK_KIND_REC_CHUNK pieces and then DONE;
 *    - the server stages the files in
 *      debug_sessions/<block>/bothost_<host>_<n>.upload/ (n: the
 *      host's connection number, as two hosts can have one name)
 *      and merges
 *      them into the block (brainRecMergeDir) on a thread, one
 *      merge at a time, then ACKs. MERGED only when every
 *      host file was read and filled entries; else FAILED with
 *      the reason, and the host sends the block again later.
 *      The host keeps its copy either way, marked uploaded on
 *      MERGED. The uploaded files are kept as they came in
 *      debug_sessions/<block>/bothost_raw/. The pieces of a
 *      block the server does not have (or still records) are
 *      not written: the block is FAILED with a log line. An
 *      upload larger than REC_UPLOAD_MAX_BYTES or
 *      REC_UPLOAD_MAX_FILES is refused.
 *
 *  cleardebug deletes debug_sessions/ except the open block
 *  and has every host delete its recordings of this server:
 *  the hosts connected now at once, the others when they
 *  next connect.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "transport_udp_internal.h"
#include "transport_udp_server_internal.h"
#include "transport_udp.h"
#include "server_sim.h"
#include "control_event.h"
#include "client_sim.h"        /* BOT_HOST_REC_STATE_* */
#include "channel_mux.h"
#include "bulk_transfer.h"
#include "brain_record.h"
#include "brain_record_host.h"
#include "brain_lua_glue.h"     /* brainCoreReleaseLogFiles */
#include "../../gui/sdl3/luabrainshandler.h" /* luaBrainsGetDebugParts */
#include "../../common/wb_log.h"
#include "../threads.h"          /* threadsWaitForMutex, threadsReleaseMutex */
#include "bot_host_guard.h"      /* BotHostRecAsked: the blocks asked for */

#define REC_ROOT            "debug_sessions"
/* The largest recording piece a host may send. */
#define REC_PIECE_MAX       (64u * 1024u)
/* Ticks the bytes of a block may come after its DONE (30 s). */
#define REC_DONE_WAIT_TICKS 1500u
/* The most one upload (one host, one block) may write. DONE carries the
 * byte count in 32 bits, so a larger upload could never be whole. */
#define REC_UPLOAD_MAX_BYTES 0xFFFFFFFFull
#define REC_UPLOAD_MAX_FILES 4096u

/* The block open now: its name, its folder and the tick it opened. s_seq
 * rises with each open or close, so each host is told each change once. */
static bool     s_open;
static char     s_block[BOT_HOST_REC_BLOCK_LEN];
static char     s_dir[FILENAME_MAX];
static uint32_t s_startTick;
static uint32_t s_seq;
/* cleardebug runs; each host is told once. */
static uint32_t s_clearGen;
/* The blocks opened to each host (by name and kind): a host may upload
 * only those. */
static BotHostRecAsked s_asked;

void serverBotRecNoteAsked(const char *block, const char *host,
                           bool playerHost) {
    botHostRecAskedNote(&s_asked, block, host, playerHost);
}

/* The merge thread: one merge at a time. */
typedef struct {
    SDL_Thread  *thread;
    SDL_AtomicInt done;
    int          h;
    uint64_t     connId;
    char         hostName[JOIN_BOT_HOST_NAME_LEN];
    char         block[BOT_HOST_REC_BLOCK_LEN];
    char         sessionDir[FILENAME_MAX];
    char         stage[600];
    bool         ok;
    bool         gzipLogs;   /* the thread compresses the plain logs after */
    BrainRecMergeStats st;
} RecMergeJob;
static RecMergeJob s_job;

/* ---- Small helpers ------------------------------------------------------- */

static const char *recBase(const char *dir) {
    const char *a = strrchr(dir, '/');
    const char *b = strrchr(dir, '\\');
    if (b != NULL && (a == NULL || b > a)) a = b;
    return a != NULL ? a + 1 : dir;
}

static bool recIsDir(const char *p) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}

static void recHostSafe(const BotHostConn *hc, char *out, size_t sz) {
    char name[JOIN_BOT_HOST_NAME_LEN];
    if (hc->name[0] == '\0' && hc->playerHost) {
        /* A player with no name: the folder takes the player's number. */
        snprintf(name, sizeof(name), "player%u", (unsigned)hc->ownerSlot);
        brainRecSafeName(name, out, sz, false);
        return;
    }
    brainRecSafeName(hc->name, out, sz, false);
}

static void recSend(int h, uint8_t op, uint8_t flags, uint8_t state,
                    uint32_t a, const char *block) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_REC;
    evt.u.botHostRec.op = op;
    evt.u.botHostRec.flags = flags;
    evt.u.botHostRec.state = state;
    evt.u.botHostRec.a = a;
    if (block != NULL) {
        SDL_strlcpy(evt.u.botHostRec.block, block,
                    sizeof(evt.u.botHostRec.block));
    }
    (void)serverBotHostSendEvent(h, &evt);
}

/* ACK a block. reason: BOT_HOST_REC_FAIL_*; skipped, unused: the host
 * files that could not be read, and that filled no entry. */
static void recSendAck(int h, uint8_t reason, uint32_t skipped,
                       uint32_t unused, const char *block) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_REC;
    evt.u.botHostRec.op = BOT_HOST_REC_OP_ACK;
    evt.u.botHostRec.state = reason == BOT_HOST_REC_FAIL_NONE
                                 ? BOT_HOST_REC_ACK_MERGED
                                 : BOT_HOST_REC_ACK_FAILED;
    evt.u.botHostRec.percent = reason;
    evt.u.botHostRec.botsDone = (uint8_t)(skipped > 255u ? 255u : skipped);
    evt.u.botHostRec.botsTotal = (uint8_t)(unused > 255u ? 255u : unused);
    if (block != NULL) {
        SDL_strlcpy(evt.u.botHostRec.block, block,
                    sizeof(evt.u.botHostRec.block));
    }
    (void)serverBotHostSendEvent(h, &evt);
}

static void recCloseFile(BotHostConn *hc) {
    if (hc->recFile != NULL) {
        fclose(hc->recFile);
        hc->recFile = NULL;
    }
    hc->recFileName[0] = '\0';
    hc->recFileOffset = 0;
}

/* Forget the block being received. */
static void recResetUpload(BotHostConn *hc) {
    recCloseFile(hc);
    hc->recBlock[0] = '\0';
    hc->recStage[0] = '\0';
    hc->recNoSession = false;
    hc->recBad = false;
    hc->recFiles = 0;
    hc->recFilesOpened = 0;
    hc->recBytes = 0;
    hc->recDoneOwed = false;
    hc->recDoneFiles = 0;
    hc->recDoneBytes = 0;
    hc->recMergeWant = false;
}

/* ---- Lifecycle ------------------------------------------------------------ */

void transportUdpServerRecOpen(const char *dir, uint32_t tick) {
    if (dir == NULL) return;
    SDL_strlcpy(s_dir, dir, sizeof(s_dir));
    SDL_strlcpy(s_block, recBase(dir), sizeof(s_block));
    if (!brainRecNameOk(s_block, false)) {
        fprintf(stderr, "[bothost] the session folder name \"%s\" cannot be "
                        "sent to Bot Hosts; their bots are not recorded\n",
                s_block);
        s_open = false;
        s_block[0] = '\0';
        s_seq++;
        return;
    }
    s_open = true;
    s_startTick = tick;
    s_seq++;
}

void transportUdpServerRecClose(void) {
    int h;
    if (!s_open) return;
    s_open = false;
    s_seq++;
    /* The block is finished: the hosts send it now, at full speed, also
     * while a game runs. */
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        hc->recUploadOwed = true;
    }
}

/* True when this server recorded, or has session folders from before:
 * only then is a host asked for its recordings. */
static bool recHasSessions(void) {
    return s_seq != 0 || recIsDir(REC_ROOT);
}

void serverBotRecOnLobby(void) {
    int h;
    if (!recHasSessions()) return;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;
        hc->recUploadOwed = true;
    }
}

void serverBotRecOnGameStart(void) {
    /* Nothing: an upload goes on at full speed while a game runs. An older
     * server sent BOT_HOST_REC_OP_HOLD here, which made an older host slow
     * its upload down. */
}

void serverBotRecHostConnected(int h, bool inLobby) {
    BotHostConn *hc = &udpServer.botHosts[h];
    recResetUpload(hc);
    bulkReceiverInit(&hc->recRecv);
    hc->recSentSeq = 0;
    hc->recClearSent = 0;
    /* A host may hold blocks from before (the server or the host was
     * stopped): it sends them now, when this server has sessions. */
    hc->recUploadOwed = recHasSessions();
    (void)inLobby;
}

void serverBotRecHostGone(int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    recCloseFile(hc);
    hc->recMergeWant = false;
    /* A piece cut off mid-way holds its buffer. */
    if (hc->recRecv.state == BULK_RECV_BODY && hc->recRecv.dst != NULL) {
        free(hc->recRecv.dst);
    }
    bulkReceiverInit(&hc->recRecv);
}

bool serverBotRecBusy(int h) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    if (hc->recBlock[0] != '\0' || hc->recMergeWant) return true;
    if (hc->recState == BOT_HOST_REC_STATE_UPLOADING) return true;
    return s_job.thread != NULL && s_job.h == h;
}

void serverBotRecRow(int h, uint8_t *state, uint8_t *percent,
                     uint8_t *botsDone, uint8_t *bots) {
    const BotHostConn *hc = &udpServer.botHosts[h];
    bool up = hc->recState == BOT_HOST_REC_STATE_UPLOADING;
    *state = hc->recState;
    *percent = up ? hc->recPercent : 0;
    *bots = up ? hc->recBots : 0;
    *botsDone = up ? hc->recBotsDone : 0;
    if (hc->recMergeWant || (s_job.thread != NULL && s_job.h == h)) {
        *state = BOT_HOST_REC_STATE_MERGING;
        *percent = 0;
        *bots = 0;
        *botsDone = 0;
    }
    if (*state > BOT_HOST_REC_STATE_MERGING) *state = BOT_HOST_REC_STATE_NONE;
    if (*percent > 100) *percent = 100;
    if (*bots > MAX_TANKS) *bots = MAX_TANKS;
    if (*botsDone > *bots) *botsDone = *bots;
}

/* ---- Control from the host ------------------------------------------------ */

void serverBotRecOnControl(int h, const ControlEvent *evt) {
    BotHostConn *hc = &udpServer.botHosts[h];
    const char *block = evt->u.botHostRec.block;

    switch (evt->u.botHostRec.op) {
        case BOT_HOST_REC_OP_STATE: {
            uint8_t st = evt->u.botHostRec.state;
            if (st > BOT_HOST_REC_STATE_MERGING) return;
            if (st == BOT_HOST_REC_STATE_LOWDISK && !hc->recLowDiskLogged) {
                hc->recLowDiskLogged = true;
                fprintf(stderr, "[bothost] Bot Host \"%s\" not recording: low "
                                "disk; its bots have no host data in %s\n",
                        hc->name, s_open ? s_block : "the next block");
                WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] Bot Host \"%s\" not "
                            "recording: low disk", hc->name);
            } else if (st == BOT_HOST_REC_STATE_RECORDING) {
                hc->recLowDiskLogged = false;
            }
            if (st == BOT_HOST_REC_STATE_UPLOADING &&
                hc->recState != BOT_HOST_REC_STATE_UPLOADING) {
                fprintf(stderr, "[bothost] Bot Host \"%s\" sends its "
                                "recordings (%u bots)\n", hc->name,
                        (unsigned)evt->u.botHostRec.botsTotal);
            }
            /* The lobby row text, each 25%, so the log shows the upload
             * move on. */
            if (st == BOT_HOST_REC_STATE_UPLOADING &&
                (hc->recState != BOT_HOST_REC_STATE_UPLOADING ||
                 evt->u.botHostRec.percent / 25 != hc->recPercent / 25)) {
                fprintf(stderr, "[bothost] %s: uploading recordings %u%%, %u "
                                "of %u bots\n", hc->name,
                        (unsigned)evt->u.botHostRec.percent,
                        (unsigned)evt->u.botHostRec.botsDone,
                        (unsigned)evt->u.botHostRec.botsTotal);
            }
            hc->recState = st;
            hc->recPercent = evt->u.botHostRec.percent;
            hc->recBots = evt->u.botHostRec.botsTotal;
            hc->recBotsDone = evt->u.botHostRec.botsDone;
            return;
        }
        case BOT_HOST_REC_OP_DONE:
            if (!brainRecNameOk(block, false)) return;
            if (strcmp(block, hc->recBlock) != 0) {
                /* Nothing of it came (a block of empty files, or every piece
                 * was lost): an empty upload is still a whole one when it
                 * has no bytes. */
                recResetUpload(hc);
                if (evt->u.botHostRec.b != 0) {
                    recSendAck(h, BOT_HOST_REC_FAIL_BROKEN, 0, 0, block);
                    return;
                }
                /* No bytes: nothing to merge, nothing to send again. */
                recSendAck(h, BOT_HOST_REC_FAIL_NONE, 0, 0, block);
                return;
            }
            hc->recDoneOwed = true;
            hc->recDoneFiles = evt->u.botHostRec.a;
            hc->recDoneBytes = evt->u.botHostRec.b;
            hc->recDoneTick = udpServer.tickCount;
            if (evt->u.botHostRec.flags & BOT_HOST_REC_F_LOWDISK) {
                fprintf(stderr, "[bothost] Bot Host \"%s\" stopped recording "
                                "%s early: low disk\n", hc->name, block);
            }
            return;
        case BOT_HOST_REC_OP_CLEARED: {
            uint64_t bytes = ((uint64_t)evt->u.botHostRec.b << 32) |
                             evt->u.botHostRec.a;
            fprintf(stderr, "[bothost] Bot Host \"%s\" deleted its recordings "
                            "of this server: %.1f MB\n", hc->name,
                    (double)bytes / (1024.0 * 1024.0));
            WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] Bot Host \"%s\" cleared "
                        "%llu bytes", hc->name, (unsigned long long)bytes);
            return;
        }
        default:
            return;
    }
}

/* ---- Pieces --------------------------------------------------------------- */

/* "<block>/<file>/<offset>". */
static bool recParsePath(const char *p, char *block, size_t blockSz,
                         char *file, size_t fileSz, uint32_t *offset) {
    const char *a = strchr(p, '/');
    const char *b;
    size_t n;
    char *end = NULL;
    unsigned long v;
    if (a == NULL) return false;
    b = strchr(a + 1, '/');
    if (b == NULL) return false;
    n = (size_t)(a - p);
    if (n == 0 || n >= blockSz) return false;
    memcpy(block, p, n);
    block[n] = '\0';
    n = (size_t)(b - a - 1);
    if (n == 0 || n >= fileSz) return false;
    memcpy(file, a + 1, n);
    file[n] = '\0';
    if (b[1] < '0' || b[1] > '9') return false;
    v = strtoul(b + 1, &end, 10);
    if (end == NULL || *end != '\0') return false;
    *offset = (uint32_t)v;
    return brainRecNameOk(block, false) && brainRecNameOk(file, true);
}

/* The first piece of a block: make its staging folder in the session
 * folder. A block with no finished session folder here writes nothing. */
static void recStartBlock(BotHostConn *hc, const char *block) {
    char safe[JOIN_BOT_HOST_NAME_LEN];
    char session[FILENAME_MAX];
    int n;
    recResetUpload(hc);
    SDL_strlcpy(hc->recBlock, block, sizeof(hc->recBlock));
    recHostSafe(hc, safe, sizeof(safe));
    n = snprintf(session, sizeof(session), REC_ROOT "/%s", block);
    if (n < 0 || (size_t)n >= sizeof(session) || !recIsDir(session) ||
        (s_open && strcmp(block, s_block) == 0)) {
        hc->recNoSession = true;
        return;
    }
    if (!botHostRecAskedHas(&s_asked, block, hc->name, hc->playerHost)) {
        /* This server never opened that block to a host of this name. */
        fprintf(stderr, "[bothost] Bot Host \"%s\" sent recordings for %s, "
                        "a block it was not asked to record; refused\n",
                hc->name, block);
        hc->recNoSession = true;
        return;
    }
    /* The connection number keeps two hosts with one name (each named
     * "Bot Host" when no --name is given) out of one staging folder. */
    n = snprintf(hc->recStage, sizeof(hc->recStage), "%s/bothost_%s_%d.upload",
                 session, safe, (int)(hc - udpServer.botHosts));
    if (n < 0 || (size_t)n >= sizeof(hc->recStage)) {
        hc->recStage[0] = '\0';
        hc->recBad = true;
        return;
    }
    if (!SDL_CreateDirectory(hc->recStage)) {
        fprintf(stderr, "[bothost] cannot make %s (%s); the recordings of Bot "
                        "Host \"%s\" are refused\n", hc->recStage,
                SDL_GetError(), hc->name);
        hc->recBad = true;
    }
}

static void recPiece(BotHostConn *hc, const BulkStreamHeader *hd,
                     const uint8_t *data) {
    char block[BOT_HOST_REC_BLOCK_LEN];
    char file[96];
    uint32_t offset = 0;
    uint32_t size = hd->gen;   /* the whole file's size */

    if (!recParsePath(hd->path, block, sizeof(block), file, sizeof(file),
                      &offset)) {
        hc->recBad = true;
        return;
    }
    if (strcmp(block, hc->recBlock) != 0) recStartBlock(hc, block);
    if (hc->recBad || hc->recNoSession) return;
    /* size is 32 bits, so the one file alone never passes the cap. */
    if (hc->recBytes + hd->totalSize > REC_UPLOAD_MAX_BYTES) {
        fprintf(stderr, "[bothost] the recordings of Bot Host \"%s\" for %s "
                        "are larger than %llu bytes; refused\n", hc->name,
                hc->recBlock, (unsigned long long)REC_UPLOAD_MAX_BYTES);
        hc->recBad = true;
        recCloseFile(hc);
        return;
    }
    if (offset == 0 || strcmp(file, hc->recFileName) != 0) {
        char path[800];
        int n;
        recCloseFile(hc);
        if (offset != 0) {   /* a file must start at its first byte */
            hc->recBad = true;
            return;
        }
        /* Count each file when it opens: a file that never completes
         * still made a file in the staging folder. */
        if (hc->recFilesOpened >= REC_UPLOAD_MAX_FILES) {
            fprintf(stderr, "[bothost] the recordings of Bot Host \"%s\" for "
                            "%s have more than %u files; refused\n", hc->name,
                    hc->recBlock, (unsigned)REC_UPLOAD_MAX_FILES);
            hc->recBad = true;
            return;
        }
        n = snprintf(path, sizeof(path), "%s/%s", hc->recStage, file);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            hc->recBad = true;
            return;
        }
        hc->recFile = fopen(path, "wb");
        if (hc->recFile == NULL) {
            fprintf(stderr, "[bothost] cannot write %s\n", path);
            hc->recBad = true;
            return;
        }
        hc->recFilesOpened++;
        SDL_strlcpy(hc->recFileName, file, sizeof(hc->recFileName));
        hc->recFileOffset = 0;
    }
    if (offset != hc->recFileOffset ||
        (uint64_t)offset + hd->totalSize > size) {
        hc->recBad = true;
        recCloseFile(hc);
        return;
    }
    if (hd->totalSize > 0 &&
        fwrite(data, 1, hd->totalSize, hc->recFile) != hd->totalSize) {
        hc->recBad = true;
        recCloseFile(hc);
        return;
    }
    hc->recFileOffset += hd->totalSize;
    hc->recBytes += hd->totalSize;
    if (hc->recFileOffset == size) {
        recCloseFile(hc);
        hc->recFiles++;
    }
}

void serverBotRecPiece(int h, const BulkStreamHeader *hd,
                       const uint8_t *data) {
    if (h < 0 || h >= MAX_BOT_HOSTS || hd == NULL) return;
    recPiece(&udpServer.botHosts[h], hd, data);
}

typedef struct {
    int h;
} RecSinkCtx;

static uint8_t *recBulkBegin(void *ctx, const BulkStreamHeader *hd) {
    (void)ctx;
    if (hd->kind != BULK_KIND_REC_CHUNK) return NULL;
    if (hd->totalSize > REC_PIECE_MAX) return NULL;
    return (uint8_t *)malloc(hd->totalSize > 0 ? hd->totalSize : 1);
}

static void recBulkComplete(void *ctx, const BulkStreamHeader *hd,
                            uint8_t *buf) {
    RecSinkCtx *c = (RecSinkCtx *)ctx;
    serverBotRecPiece(c->h, hd, buf);
    free(buf);
}

void serverBotRecDrainBulk(int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    BulkRecvSink sink;
    RecSinkCtx ctx;
    uint8_t frag[CHANNEL_MAX_SEG];
    uint16_t fragLen = sizeof(frag);
    ctx.h = h;
    sink.onBegin = recBulkBegin;
    sink.onComplete = recBulkComplete;
    sink.ctx = &ctx;
    while (channelReceive(&hc->channelMux, CHANNEL_BULK, frag, &fragLen)) {
        bulkReceiverFeed(&hc->recRecv, frag, fragLen, &sink);
        fragLen = sizeof(frag);
    }
}

/* ---- Merge ---------------------------------------------------------------- */

/* A merge line goes to the console, to the WINBOLO_LOG file, and to
 * bothost_merge.log in the session folder (the folder BrainTest loads),
 * so a block with empty host entries says why. */
static void recMergeNote(const char *sessionDir, const char *line) {
    char path[1100];
    FILE *f;
    fprintf(stderr, "%s\n", line);
    WB_LOG_INFO(WB_LOG_CAT_NET, "%s", line);
    if (sessionDir == NULL || sessionDir[0] == '\0') return;
    snprintf(path, sizeof(path), "%s/bothost_merge.log", sessionDir);
    f = fopen(path, "ab");
    if (f == NULL) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void recMergeLog(void *ctx, const char *line) {
    recMergeNote((const char *)ctx, line);
}

static int SDLCALL recMergeThread(void *data) {
    RecMergeJob *j = (RecMergeJob *)data;
    j->ok = brainRecMergeDir(j->sessionDir, j->stage, j->hostName, &j->st,
                             recMergeLog, j->sessionDir);
    /* The compression of the merged logs runs here too, so the tick thread never
     * waits for it. brainRecordCompressLogsNow takes the folder's lock. */
    if (j->gzipLogs) {
        brainRecordCompressLogsNow(j->sessionDir);
    }
    SDL_SetAtomicInt(&j->done, 1);
    return 0;
}

/* Start the merge of host h's finished upload. */
static void recMergeStart(int h) {
    BotHostConn *hc = &udpServer.botHosts[h];
    char safe[JOIN_BOT_HOST_NAME_LEN];
    memset(&s_job, 0, sizeof(s_job));
    s_job.h = h;
    s_job.connId = hc->connId;
    recHostSafe(hc, safe, sizeof(safe));
    SDL_strlcpy(s_job.hostName, safe, sizeof(s_job.hostName));
    SDL_strlcpy(s_job.block, hc->recBlock, sizeof(s_job.block));
    snprintf(s_job.sessionDir, sizeof(s_job.sessionDir), REC_ROOT "/%s",
             hc->recBlock);
    SDL_strlcpy(s_job.stage, hc->recStage, sizeof(s_job.stage));
    SDL_SetAtomicInt(&s_job.done, 0);
    /* The brains' log writer keeps the session's print2 logs open, and an
     * open file cannot be replaced by its merged copy. Close them; the
     * writer opens them again at its next line. */
    brainCoreReleaseLogFiles();
    /* The merge's output follows the compression rule: a folder the recorder still
     * compresses later is left to it; any other gets its plain logs compressed
     * on the merge thread. Asked here, on the tick thread, as the recorder's
     * own state is the tick thread's. */
    s_job.gzipLogs = !brainRecordLogCompressPending(s_job.sessionDir);
    {
        char line[1400];
        snprintf(line, sizeof(line), "[bothost] merging the recordings of Bot "
                 "Host \"%s\" into %s (%u files, %.1f MB)", hc->name,
                 s_job.sessionDir, (unsigned)hc->recFiles,
                 (double)hc->recBytes / (1024.0 * 1024.0));
        recMergeNote(s_job.sessionDir, line);
    }
    s_job.thread = SDL_CreateThread(recMergeThread, "brainrec_merge", &s_job);
    if (s_job.thread == NULL) {
        fprintf(stderr, "[bothost] cannot start the merge thread (%s); the "
                        "files stay in %s\n", SDL_GetError(), s_job.stage);
        recSendAck(h, BOT_HOST_REC_FAIL_NO_THREAD, 0, 0, hc->recBlock);
        recResetUpload(hc);
        return;
    }
    /* The upload state goes; the job holds what the merge needs. */
    recResetUpload(hc);
}

static void recMergeFinish(void) {
    int h = s_job.h;
    BotHostConn *hc = &udpServer.botHosts[h];
    uint8_t reason;
    SDL_WaitThread(s_job.thread, NULL);
    s_job.thread = NULL;
    reason = brainRecMergeVerdict(s_job.ok, &s_job.st);
    {
        char line[1400];
        snprintf(line, sizeof(line), "[bothost] merge of Bot Host \"%s\" into "
                 "%s %s: %u host files used (%u filled no entry), %u "
                 "skipped, %u frames, %u hosted entries (%u filled, %u a few "
                 "thinks old, %u with no host data); answer: %s",
                 s_job.hostName, s_job.sessionDir,
                 s_job.ok ? "done" : "done in part",
                 (unsigned)s_job.st.hostFiles,
                 (unsigned)s_job.st.hostFilesUnused,
                 (unsigned)s_job.st.hostFilesSkipped,
                 (unsigned)s_job.st.frames, (unsigned)s_job.st.markers,
                 (unsigned)s_job.st.filled, (unsigned)s_job.st.stale,
                 (unsigned)s_job.st.missing,
                 reason == BOT_HOST_REC_FAIL_NONE
                     ? "merged" : brainRecMergeFailText(reason));
        recMergeNote(s_job.sessionDir, line);
    }
    /* The uploaded files stay in the session's bothost_raw/ either way.
     * MERGED marks the block uploaded on the host; FAILED has it send the
     * block again. The host keeps its copy in both cases. */
    if (hc->connected && hc->connId == s_job.connId) {
        recSendAck(h, reason, s_job.st.hostFilesSkipped,
                   s_job.st.hostFilesUnused, s_job.block);
    }
}

/* ---- Per tick --------------------------------------------------------------- */

static uint8_t recOpenFlags(int *poolMode) {
    int p2 = 1, pool = 1, viz = 1, jsonl = 1;
    bool recViz = true;
    uint8_t f = 0;
    luaBrainsGetDebugParts(&p2, &pool, &viz, &jsonl);
    brainRecordGetParts(&recViz, poolMode);
    if (recViz && viz) f |= BOT_HOST_REC_F_VIZ;
    if (p2) f |= BOT_HOST_REC_F_PRINT2;
    if (jsonl) f |= BOT_HOST_REC_F_JSONL;
    if (pool && *poolMode != 0) f |= BOT_HOST_REC_F_POOL;
    /* No BOT_HOST_REC_F_POOL_RAW: this server records the pool as JSON and
     * its reader takes only JSON, so the hosts record JSON too. */
    return f;
}

void serverBotRecService(ServerSim *sim, bool inLobby) {
    int h;
    (void)sim;
    (void)inLobby;

    if (s_job.thread != NULL && SDL_GetAtomicInt(&s_job.done) != 0) {
        recMergeFinish();
    }

    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        BotHostConn *hc = &udpServer.botHosts[h];
        if (!hc->connected) continue;

        if (s_clearGen != 0 && hc->recClearSent != s_clearGen) {
            hc->recClearSent = s_clearGen;
            recSend(h, BOT_HOST_REC_OP_CLEAR, 0, 0, 0, NULL);
        }
        if (s_seq != 0 && hc->recSentSeq != s_seq) {
            hc->recSentSeq = s_seq;
            if (s_open) {
                int poolMode = 0;
                uint8_t f = recOpenFlags(&poolMode);
                recSend(h, BOT_HOST_REC_OP_OPEN, f, (uint8_t)poolMode,
                        s_startTick, s_block);
                botHostRecAskedNote(&s_asked, s_block, hc->name,
                                    hc->playerHost);
            } else {
                recSend(h, BOT_HOST_REC_OP_CLOSE, 0, 0, 0, NULL);
            }
        }
        if (hc->recUploadOwed) {
            hc->recUploadOwed = false;
            /* Never BOT_HOST_REC_F_PACED now. a: this server's wide bulk
             * ring, which a newer host sends into; an older host leaves a
             * alone and keeps the built-in ring. */
            recSend(h, BOT_HOST_REC_OP_UPLOAD, 0, 0,
                    serverBotHostBulkWide(h), NULL);
        }
        if (hc->recDoneOwed && hc->recNoSession) {
            /* Nothing of it was written. */
            fprintf(stderr, "[bothost] the recordings of Bot Host \"%s\" for "
                            "%s are refused: the server has no such finished "
                            "session folder\n", hc->name, hc->recBlock);
            recSendAck(h, BOT_HOST_REC_FAIL_NO_SESSION, 0, 0, hc->recBlock);
            recResetUpload(hc);
        } else if (hc->recDoneOwed) {
            bool whole = hc->recBytes == hc->recDoneBytes &&
                         hc->recFiles == hc->recDoneFiles &&
                         hc->recFile == NULL;
            if (hc->recBad || (!whole && udpServer.tickCount - hc->recDoneTick >
                                             REC_DONE_WAIT_TICKS)) {
                fprintf(stderr, "[bothost] the recordings of Bot Host \"%s\" "
                                "for %s came in broken (%u of %u files, %llu "
                                "of %u bytes); it sends them again later\n",
                        hc->name, hc->recBlock, (unsigned)hc->recFiles,
                        (unsigned)hc->recDoneFiles,
                        (unsigned long long)hc->recBytes,
                        (unsigned)hc->recDoneBytes);
                recSendAck(h, BOT_HOST_REC_FAIL_BROKEN, 0, 0, hc->recBlock);
                recResetUpload(hc);
            } else if (whole) {
                hc->recDoneOwed = false;
                hc->recMergeWant = true;
            }
        }
    }

    if (s_job.thread == NULL) {
        for (h = 0; h < MAX_BOT_HOSTS; h++) {
            BotHostConn *hc = &udpServer.botHosts[h];
            if (hc->connected && hc->recMergeWant) {
                recMergeStart(h);
                break;
            }
        }
    }
}

/* ---- Console ------------------------------------------------------------------ */

typedef struct {
    uint64_t bytes;
    uint32_t files;
    bool     ok;
} ClearCtx;

static SDL_EnumerationResult SDLCALL recClearEntry(void *userdata,
                                                   const char *dirname,
                                                   const char *fname) {
    ClearCtx *c = (ClearCtx *)userdata;
    char path[FILENAME_MAX];
    uint64_t b = 0;
    uint32_t f = 0;
    (void)dirname;
    if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    if (s_open && strcmp(fname, s_block) == 0) return SDL_ENUM_CONTINUE;
    snprintf(path, sizeof(path), REC_ROOT "/%s", fname);
    if (!brainRecRemoveTree(REC_ROOT, path, &b, &f)) c->ok = false;
    c->bytes += b;
    c->files += f;
    return SDL_ENUM_CONTINUE;
}

bool transportUdpServerClearDebug(char *msg, size_t msgLen) {
    ClearCtx c;
    int h, hosts = 0;
    if (s_job.thread != NULL) {
        snprintf(msg, msgLen, "cleardebug refused: a Bot Host merge runs now");
        return false;
    }
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        const BotHostConn *hc = &udpServer.botHosts[h];
        if (hc->connected && (hc->recBlock[0] != '\0' || hc->recMergeWant)) {
            snprintf(msg, msgLen, "cleardebug refused: Bot Host \"%s\" sends "
                     "its recordings now", hc->name);
            return false;
        }
    }
    memset(&c, 0, sizeof(c));
    c.ok = true;
    if (recIsDir(REC_ROOT)) {
        (void)SDL_EnumerateDirectory(REC_ROOT, recClearEntry, &c);
    }
    s_clearGen++;
    for (h = 0; h < MAX_BOT_HOSTS; h++) {
        if (udpServer.botHosts[h].connected) hosts++;
    }
    snprintf(msg, msgLen, "cleardebug: deleted %.1f MB in %u files from "
             REC_ROOT "/%s%s%s; %d connected Bot Hosts delete theirs now, "
             "others when they next connect",
             (double)c.bytes / (1024.0 * 1024.0), (unsigned)c.files,
             s_open ? " (kept the open block " : "", s_open ? s_block : "",
             s_open ? ")" : "", hosts);
    if (!c.ok) {
        SDL_strlcat(msg, "; some files could not be deleted", msgLen);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] %s", msg);
    return c.ok;
}

bool transportUdpServerBrainMerge(const char *sessionDir, const char *hostDir,
                                  char *msg, size_t msgLen) {
    BrainRecMergeStats st;
    bool ok;
    bool busy;
    bool pending;
    if (sessionDir == NULL || hostDir == NULL || sessionDir[0] == '\0' ||
        hostDir[0] == '\0') {
        snprintf(msg, msgLen, "Usage: brainmerge <session dir> <host dir>");
        return false;
    }
    if (!recIsDir(sessionDir) || !recIsDir(hostDir)) {
        snprintf(msg, msgLen, "brainmerge: %s or %s is not a folder",
                 sessionDir, hostDir);
        return false;
    }
    /* This runs on the console's thread. s_open, s_block and the
     * recorder's state belong to the tick thread, so read them under the
     * server mutex. The merge and the compression only work on files (the folder
     * lock keeps them apart from the recorder's own compression), so they run
     * without the mutex and the game goes on. */
    threadsWaitForMutex();
    busy = s_open && strcmp(recBase(sessionDir), s_block) == 0;
    threadsReleaseMutex();
    if (busy) {
        snprintf(msg, msgLen, "brainmerge refused: %s is recording now",
                 sessionDir);
        return false;
    }
    ok = brainRecMergeDir(sessionDir, hostDir, recBase(hostDir), &st,
                          recMergeLog, NULL);
    /* The compression rule, as for an upload. The console's thread waits for the
     * merge anyway, so the compression runs here too, in line, and the recorder's
     * table of compression threads is not changed from this thread. */
    threadsWaitForMutex();
    pending = brainRecordLogCompressPending(sessionDir);
    if (!pending) brainCoreReleaseLogFiles();
    threadsReleaseMutex();
    if (!pending) brainRecordCompressLogsNow(sessionDir);
    snprintf(msg, msgLen, "brainmerge %s: %u frames, %u hosted entries (%u "
             "filled, %u a few thinks old, %u with no host data)%s",
             sessionDir, (unsigned)st.frames, (unsigned)st.markers,
             (unsigned)st.filled, (unsigned)st.stale, (unsigned)st.missing,
             ok ? "" : "; some files were kept beside it");
    return ok;
}
