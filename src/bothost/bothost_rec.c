/*
 * WinBoloBotHost - brain recordings of the bots this host runs.
 * See bothost_rec.h.
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bothost_rec.h"
#include "global.h"            /* MAX_TANKS */
#include "client_sim.h"        /* BOT_HOST_REC_STATE_* */
#include "server_sim.h"        /* BrainGoalInfo */
#include "bot_manager.h"       /* botBrainGetGoalInfo, botBrainEvalLuaString */
#include "brain_record.h"
#include "brain_record_host.h"
#include "brain_lua_glue.h"     /* brainCoreReleaseLogFiles */

/* Files up to 4 GiB are sent, and long is 32 bits on Windows. */
#if defined(_WIN32)
#define rec_fseek _fseeki64
#else
#include <sys/types.h>
#define rec_fseek fseeko
#endif

#define REC_MAX_SERVERS    16
#define REC_MAX_SEATS      MAX_TANKS
/* Records kept per seat before the server's OPEN arrives: the first thinks
 * of a game come before it. */
#define REC_RING           32
#define REC_PIECE          (64u * 1024u)
#define REC_DISK_CHECK_MS  5000u
#define REC_ACK_WAIT_MS    120000u
#define REC_RETRY_MS       10000u
#define REC_PACED_MS       200u
#define REC_STATE_MS       250u
#define REC_MAX_FAILS      3
/* Written into a block folder the server merged: it is not sent again.
 * The files stay; only CLEAR deletes them. */
#define REC_UPLOADED_MARKER "uploaded.marker"
/* The same, from hosts that deleted a block after it was sent. */
#define REC_SENT_MARKER    "sent.marker"
/* Written into a block that stopped early for low disk. */
#define REC_LOWDISK_MARKER "lowdisk.marker"

typedef struct {
    uint32_t    seq;
    uint32_t    estTick;
    BrainRecBuf body;
} RecRing;

typedef struct {
    bool              used;
    int               srv;
    uint8_t           slot;
    uint16_t          placeGen;
    struct lua_State *L;
    HostRecWriter     w;
    bool              fileOpen;
    char              fileBlock[BOT_HOST_REC_BLOCK_LEN];
    bool              skip[256];
    bool              haveSkip;
    RecRing           ring[REC_RING];
    int               ringHead;   /* the oldest record */
    int               ringCount;
} RecSeat;

typedef struct {
    char     name[96];
    uint32_t size;
} RecUpFile;

typedef struct {
    bool     up;
    char     key[96];
    char     root[700];        /* <cache>/recordings/<key> */

    /* The block the server records now. */
    bool     open;
    char     block[BOT_HOST_REC_BLOCK_LEN];
    uint8_t  flags;
    uint8_t  poolMode;
    uint32_t startTick;
    bool     lowDisk;          /* this block stopped for low disk */

    /* Uploads. */
    bool       want;
    bool       paced;
    uint32_t   retryAtMs;
    bool       active;
    char       upBlock[BOT_HOST_REC_BLOCK_LEN];
    RecUpFile *files;
    int        fileCount;
    int        fileIdx;
    uint32_t   fileOff;
    FILE      *fp;
    uint64_t   total;
    uint64_t   sent;
    int        bots;
    int        botsDone;
    bool       upLowDisk;
    bool       waitAck;
    uint32_t   ackDeadline;
    uint32_t   lastPieceMs;
    int        fails;
    char       skipBlocks[8][BOT_HOST_REC_BLOCK_LEN];
    int        skipCount;

    /* The last state told to the server. */
    uint8_t  lastState;
    uint8_t  lastPct;
    uint8_t  lastBotsDone;
    uint32_t lastStateMs;
    bool     stateSent;
} RecSrv;

static char      s_cache[512];
static char      s_recRoot[600];      /* <cache>/recordings */
static char      s_hostName[HOSTREC_HOST_LEN];
static uint64_t  s_minFree;
static HostRecIo s_io;
static bool      s_init;
static RecSrv    s_srv[REC_MAX_SERVERS];
static RecSeat   s_seat[REC_MAX_SEATS];
static uint32_t  s_lastDiskMs;
static bool      s_diskLow;
static bool      s_diskChecked;
static BrainRecBuf s_body;

/* ---- Helpers ------------------------------------------------------------- */

bool hostRecFreeBytes(const char *path, uint64_t *out) {
#ifdef _WIN32
    ULARGE_INTEGER freeAvail;
    if (GetDiskFreeSpaceExA(path, &freeAvail, NULL, NULL)) {
        *out = (uint64_t)freeAvail.QuadPart;
        return true;
    }
#else
    struct statvfs vfs;
    if (statvfs(path, &vfs) == 0) {
        *out = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        return true;
    }
#endif
    return false;
}

static RecSrv *recSrv(int srv) {
    if (srv < 0 || srv >= REC_MAX_SERVERS) return NULL;
    return &s_srv[srv];
}

static void recSendEvt(int srv, uint8_t op, uint8_t flags, uint8_t state,
                       uint8_t pct, uint8_t done, uint8_t total, uint32_t a,
                       uint32_t b, const char *block) {
    ControlEvent evt;
    if (s_io.sendEvent == NULL) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_REC;
    evt.u.botHostRec.op = op;
    evt.u.botHostRec.flags = flags;
    evt.u.botHostRec.state = state;
    evt.u.botHostRec.percent = pct;
    evt.u.botHostRec.botsDone = done;
    evt.u.botHostRec.botsTotal = total;
    evt.u.botHostRec.a = a;
    evt.u.botHostRec.b = b;
    if (block != NULL) {
        SDL_strlcpy(evt.u.botHostRec.block, block,
                    sizeof(evt.u.botHostRec.block));
    }
    s_io.sendEvent(srv, &evt);
}

static bool recIsDir(const char *p) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}

static bool recIsFile(const char *p) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_FILE;
}

static void recTouch(const char *dir, const char *name) {
    char path[900];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f != NULL) fclose(f);
}

/* A Lua string literal of s (no quotes added). */
static void recLuaQuote(const char *s, char *out, size_t sz) {
    size_t n = 0;
    for (; *s != '\0' && n + 3 < sz; s++) {
        if (*s == '\\' || *s == '"') out[n++] = '\\';
        out[n++] = *s;
    }
    out[n] = '\0';
}

/* ---- Seats ----------------------------------------------------------------- */

static void ringClear(RecSeat *rs) {
    rs->ringHead = 0;
    rs->ringCount = 0;
}

static void ringPush(RecSeat *rs, uint32_t seq, uint32_t estTick,
                     const BrainRecBuf *body) {
    RecRing *r;
    if (rs->ringCount < REC_RING) {
        r = &rs->ring[(rs->ringHead + rs->ringCount) % REC_RING];
        rs->ringCount++;
    } else {
        r = &rs->ring[rs->ringHead];
        rs->ringHead = (rs->ringHead + 1) % REC_RING;
    }
    r->seq = seq;
    r->estTick = estTick;
    brainRecBufReset(&r->body);
    brainRecBufPut(&r->body, body->data, body->len);
}

static void seatCloseFile(RecSeat *rs) {
    if (rs->fileOpen) {
        hostRecWriterClose(&rs->w);
        rs->fileOpen = false;
    }
    rs->fileBlock[0] = '\0';
}

/* print2 of a seat: into the open block of its server, or off. */
static void seatApplyPrint2(RecSeat *rs) {
    RecSrv *sv = recSrv(rs->srv);
    char src[1400];
    if (rs->L == NULL || sv == NULL) return;
    if (sv->open && !sv->lowDisk && (sv->flags & BOT_HOST_REC_F_PRINT2)) {
        char dir[900];
        char q[1000];
        snprintf(dir, sizeof(dir), "%s/%s", sv->root, sv->block);
        recLuaQuote(dir, q, sizeof(q));
        snprintf(src, sizeof(src),
                 "_G.DEBUG_SESSION_DIR=\"%s\"; _G._PRINT2_ENABLED=true; "
                 "local ok,p=pcall(require,'print2'); "
                 "if ok and p.reset_log then p.reset_log() end", q);
    } else {
        /* Flush what the old block has, then stop: a log with no block
         * folder has nowhere it may be written. */
        snprintf(src, sizeof(src),
                 "local ok,p=pcall(require,'print2'); "
                 "if ok and p.reset_log then p.reset_log() end; "
                 "_G._PRINT2_ENABLED=false; "
                 "if gh_opt_log and gh_opt_log.flush then "
                 "pcall(gh_opt_log.flush) end");
    }
    (void)botBrainExecLua(rs->L, src);
}

/* Open the seat's file in its server's open block. */
static bool seatOpenFile(RecSeat *rs, RecSrv *sv) {
    char dir[900];
    char name[64];
    char path[1000];
    HostRecHeader hdr;
    char *legend = NULL;
    char *csv;
    int i;
    unsigned part;

    snprintf(dir, sizeof(dir), "%s/%s", sv->root, sv->block);
    if (!recIsDir(dir) && !SDL_CreateDirectory(dir)) {
        fprintf(stderr, "[bothost] cannot make %s (%s); bot %d is not "
                        "recorded\n", dir, SDL_GetError(), (int)rs->slot);
        return false;
    }
    /* A file of this seat can be in the block already: the brain was built
     * again (a new brain owed, a reload) and the file was closed. Opening
     * that name again would empty it, so the next part takes a new name. */
    for (part = 0; part < 1000u; part++) {
        hostRecPartFileName(name, sizeof(name), rs->slot, rs->placeGen, part);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (!SDL_GetPathInfo(path, NULL)) break;
    }
    memset(&hdr, 0, sizeof(hdr));
    SDL_strlcpy(hdr.hostName, s_hostName, sizeof(hdr.hostName));
    SDL_strlcpy(hdr.block, sv->block, sizeof(hdr.block));
    hdr.slot = rs->slot;
    hdr.placeGen = rs->placeGen;
    if (rs->L != NULL) {
        legend = botBrainEvalLuaString(rs->L, "return brain.viz_legend_json()");
    }
    memset(rs->skip, 0, sizeof(rs->skip));
    rs->haveSkip = false;
    if (rs->L != NULL) {
        csv = botBrainEvalLuaString(rs->L, "return brain.viz_record_skip_csv()");
        if (csv != NULL) {
            const char *p = csv;
            while (*p) {
                int v = atoi(p);
                if (v >= 0 && v < 256) {
                    rs->skip[v] = true;
                    rs->haveSkip = true;
                }
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            free(csv);
        }
    }
    if (!hostRecWriterOpen(&rs->w, path, &hdr,
                           (legend != NULL && legend[0] == '{') ? legend : "")) {
        fprintf(stderr, "[bothost] cannot write %s; bot %d is not recorded\n",
                path, (int)rs->slot);
        free(legend);
        return false;
    }
    free(legend);
    rs->fileOpen = true;
    SDL_strlcpy(rs->fileBlock, sv->block, sizeof(rs->fileBlock));
    /* The thinks before the OPEN came. */
    for (i = 0; i < rs->ringCount; i++) {
        const RecRing *r = &rs->ring[(rs->ringHead + i) % REC_RING];
        (void)hostRecWriterAppend(&rs->w, r->seq, r->estTick, r->body.data,
                                  r->body.len);
    }
    ringClear(rs);
    return true;
}

void hostRecSeatBuilt(int idx, int srv, struct lua_State *L) {
    RecSeat *rs;
    if (!s_init || idx < 0 || idx >= REC_MAX_SEATS) return;
    rs = &s_seat[idx];
    if (rs->used && rs->srv != srv) hostRecSeatGone(idx);
    rs->used = true;
    rs->srv = srv;
    rs->L = L;
    /* A new brain may number its overlay categories anew: the file takes
     * the new legend from the next think on. */
    seatCloseFile(rs);
    ringClear(rs);
    seatApplyPrint2(rs);
}

void hostRecSeatGone(int idx) {
    RecSeat *rs;
    int i;
    if (!s_init || idx < 0 || idx >= REC_MAX_SEATS) return;
    rs = &s_seat[idx];
    seatCloseFile(rs);
    for (i = 0; i < REC_RING; i++) brainRecBufFree(&rs->ring[i].body);
    memset(rs, 0, sizeof(*rs));
}

/* What srv's recording of a seat takes this tick: false when nothing
 * records. recording: a block is open (else the -brain-debug ring before
 * the OPEN). */
static bool srvRecPlan(const RecSrv *sv, bool debug, bool *recording,
                       uint8_t *flags, uint8_t *poolMode) {
    *recording = sv->up && sv->open && !sv->lowDisk;
    if (!*recording && !(sv->up && debug && !s_diskLow)) return false;
    if (*recording) {
        *flags = sv->flags;
        *poolMode = sv->poolMode;
    } else {
        /* Before the OPEN: what -brain-debug records by default. */
        *flags = BOT_HOST_REC_F_VIZ | BOT_HOST_REC_F_POOL;
        *poolMode = 1;
    }
    return true;
}

/* The same block the server records for its own bots: the raw capture by
 * default, the panel JSON when the OPEN asks. String literals, so a caller
 * can compare the pointers. */
static const char *const kPoolExprJson =
    "return brain.get_pool_breakdown_json()";
static const char *const kPoolExprRaw =
    "return (brain.get_pool_breakdown_raw"
    " or brain.get_pool_breakdown_json)()";

static const char *poolExprFor(uint8_t flags) {
    return (flags & BOT_HOST_REC_F_POOL_JSON) ? kPoolExprJson : kPoolExprRaw;
}

const char *hostRecSeatPoolPlan(int srv, bool debug, uint8_t *poolMode) {
    const RecSrv *sv = recSrv(srv);
    bool recording;
    uint8_t flags = 0, mode = 0;
    *poolMode = 0;
    if (!s_init || sv == NULL) return NULL;
    if (!srvRecPlan(sv, debug, &recording, &flags, &mode)) return NULL;
    if (!(flags & BOT_HOST_REC_F_POOL) || mode == 0) return NULL;
    *poolMode = mode;
    return poolExprFor(flags);
}

bool hostRecPoolWanted(uint8_t poolMode, uint32_t seq, int rrIndex,
                       int rrCount) {
    if (poolMode == 0) return false;
    return poolMode == 2 ||
           (rrCount > 0 && (int)((seq / 2u) % (uint32_t)rrCount) == rrIndex);
}

void hostRecSeatThink(int idx, int srv, uint8_t slot, uint16_t placeGen,
                      uint32_t seq, uint32_t estTick, float thinkMs,
                      struct lua_State *L, const OverlayCmdBuffer *ovl,
                      bool debug, int rrIndex, int rrCount,
                      char *poolPrefetched, const char *poolPrefetchExpr) {
    RecSeat *rs;
    RecSrv *sv = recSrv(srv);
    BrainGoalInfo gi;
    bool haveGoal;
    bool recording;
    char *pj = NULL;
    uint8_t flags = 0;
    uint8_t poolMode = 0;

    if (!s_init || sv == NULL || idx < 0 || idx >= REC_MAX_SEATS || L == NULL) {
        free(poolPrefetched);
        return;
    }
    rs = &s_seat[idx];
    if (!rs->used || rs->srv != srv) {
        hostRecSeatBuilt(idx, srv, L);
    }
    rs->L = L;
    if (rs->slot != slot || rs->placeGen != placeGen) {
        /* Placed again: a new file. */
        seatCloseFile(rs);
        ringClear(rs);
        rs->slot = slot;
        rs->placeGen = placeGen;
    }
    if (!srvRecPlan(sv, debug, &recording, &flags, &poolMode)) {
        free(poolPrefetched);
        if (rs->ringCount > 0) ringClear(rs);
        return;
    }
    if (recording && rs->fileOpen && strcmp(rs->fileBlock, sv->block) != 0) {
        seatCloseFile(rs);
    }

    /* The pool capture normally arrives from the seat's own worker, made
     * right after its think (bothost_main.c hostThinkJob), so this serial
     * section only writes the bytes. The eval here is the fallback for a
     * seat whose worker did not make it, or made it with another
     * expression. Never two evals in one tick: a worker eval that gave
     * NULL counts as done. The capture comes before the goal info, as on
     * the server, in both paths. */
    if ((flags & BOT_HOST_REC_F_POOL) &&
        hostRecPoolWanted(poolMode, seq, rrIndex, rrCount)) {
        const char *expr = poolExprFor(flags);
        if (poolPrefetchExpr == expr) {
            pj = poolPrefetched;
        } else {
            free(poolPrefetched);
            pj = botBrainEvalLuaString(L, expr);
        }
    } else {
        free(poolPrefetched);
    }
    haveGoal = botBrainGetGoalInfo(L, &gi);
    brainRecBufReset(&s_body);
    (void)brainRecBodyEncode(&s_body, thinkMs, haveGoal ? &gi : NULL,
                             (flags & BOT_HOST_REC_F_VIZ) ? ovl : NULL,
                             rs->haveSkip ? rs->skip : NULL, pj);
    free(pj);
    if (s_body.failed) return;

    if (!recording) {
        ringPush(rs, seq, estTick, &s_body);
        return;
    }
    if (!rs->fileOpen && !seatOpenFile(rs, sv)) return;
    if (!hostRecWriterAppend(&rs->w, seq, estTick, s_body.data, s_body.len)) {
        fprintf(stderr, "[bothost] write to the recording of bot %d failed; "
                        "it stops for this block\n", (int)slot);
        seatCloseFile(rs);
        /* Keep it closed for this block. */
        SDL_strlcpy(rs->fileBlock, sv->block, sizeof(rs->fileBlock));
    }
}

/* ---- Blocks ---------------------------------------------------------------- */

static void srvCloseSeats(int srv) {
    int i;
    for (i = 0; i < REC_MAX_SEATS; i++) {
        if (s_seat[i].used && s_seat[i].srv == srv) seatCloseFile(&s_seat[i]);
    }
}

static void srvApplyPrint2(int srv) {
    int i;
    for (i = 0; i < REC_MAX_SEATS; i++) {
        if (s_seat[i].used && s_seat[i].srv == srv) seatApplyPrint2(&s_seat[i]);
    }
}

/* Stop recording srv's block now (low disk). */
static void srvStopLowDisk(int srv) {
    RecSrv *sv = &s_srv[srv];
    char dir[900];
    if (!sv->open || sv->lowDisk) return;
    sv->lowDisk = true;
    srvCloseSeats(srv);
    srvApplyPrint2(srv);
    snprintf(dir, sizeof(dir), "%s/%s", sv->root, sv->block);
    if (recIsDir(dir)) recTouch(dir, REC_LOWDISK_MARKER);
}

static void srvOpen(int srv, const ControlEvent *evt) {
    RecSrv *sv = &s_srv[srv];
    const char *block = evt->u.botHostRec.block;
    char dir[900];
    uint64_t freeB = 0;

    if (!brainRecNameOk(block, false)) return;
    if (sv->open && strcmp(sv->block, block) == 0) {
        /* The same block again (a reconnect): the flags may be new. */
        sv->flags = evt->u.botHostRec.flags;
        sv->poolMode = evt->u.botHostRec.state;
        return;
    }
    if (sv->open) srvCloseSeats(srv);
    sv->open = true;
    SDL_strlcpy(sv->block, block, sizeof(sv->block));
    sv->flags = evt->u.botHostRec.flags;
    sv->poolMode = evt->u.botHostRec.state;
    if (sv->poolMode > 2) sv->poolMode = 1;
    sv->startTick = evt->u.botHostRec.a;
    sv->lowDisk = false;
    /* The disk check at the start of each block. */
    s_diskLow = hostRecFreeBytes(s_cache, &freeB) && freeB < s_minFree;
    s_lastDiskMs = (uint32_t)SDL_GetTicks();
    s_diskChecked = true;
    (void)SDL_CreateDirectory(sv->root);
    snprintf(dir, sizeof(dir), "%s/%s", sv->root, sv->block);
    if (s_diskLow) {
        sv->lowDisk = true;
        fprintf(stderr, "[bothost] not recording %s for %s: low disk (%.1f GB "
                        "free, %.1f GB wanted)\n", block, sv->key,
                (double)freeB / 1e9, (double)s_minFree / 1e9);
    } else if (!SDL_CreateDirectory(dir)) {
        fprintf(stderr, "[bothost] cannot make %s (%s); this block is not "
                        "recorded\n", dir, SDL_GetError());
        sv->lowDisk = true;
    } else {
        fprintf(stderr, "[bothost] recording the bots of %s into %s\n",
                sv->key, dir);
    }
    srvApplyPrint2(srv);
}

static void srvClose(int srv) {
    RecSrv *sv = &s_srv[srv];
    if (!sv->open) return;
    srvCloseSeats(srv);
    sv->open = false;
    srvApplyPrint2(srv);
    fprintf(stderr, "[bothost] block %s of %s finished\n", sv->block, sv->key);
}

/* ---- Uploads ----------------------------------------------------------------- */

static void upReset(RecSrv *sv) {
    if (sv->fp != NULL) fclose(sv->fp);
    sv->fp = NULL;
    free(sv->files);
    sv->files = NULL;
    sv->fileCount = 0;
    sv->fileIdx = 0;
    sv->fileOff = 0;
    sv->total = 0;
    sv->sent = 0;
    sv->bots = 0;
    sv->botsDone = 0;
    sv->upLowDisk = false;
    sv->active = false;
    sv->waitAck = false;
    sv->upBlock[0] = '\0';
}

static bool upSkipped(const RecSrv *sv, const char *block) {
    int i;
    for (i = 0; i < sv->skipCount; i++) {
        if (strcmp(sv->skipBlocks[i], block) == 0) return true;
    }
    return false;
}

typedef struct {
    RecSrv *sv;
    char    found[BOT_HOST_REC_BLOCK_LEN];
} PickCtx;

static SDL_EnumerationResult SDLCALL upPickEntry(void *userdata,
                                                 const char *dirname,
                                                 const char *fname) {
    PickCtx *c = (PickCtx *)userdata;
    char path[900];
    char marker[1000];
    (void)dirname;
    if (!brainRecNameOk(fname, false)) return SDL_ENUM_CONTINUE;
    if (c->sv->open && strcmp(fname, c->sv->block) == 0) return SDL_ENUM_CONTINUE;
    if (upSkipped(c->sv, fname)) return SDL_ENUM_CONTINUE;
    snprintf(path, sizeof(path), "%s/%s", c->sv->root, fname);
    if (!recIsDir(path)) return SDL_ENUM_CONTINUE;
    /* Uploaded already: kept here, not sent again. */
    snprintf(marker, sizeof(marker), "%s/%s", path, REC_UPLOADED_MARKER);
    if (recIsFile(marker)) return SDL_ENUM_CONTINUE;
    snprintf(marker, sizeof(marker), "%s/%s", path, REC_SENT_MARKER);
    if (recIsFile(marker)) return SDL_ENUM_CONTINUE;
    /* The oldest name first: the names start with the date and time. */
    if (c->found[0] == '\0' || strcmp(fname, c->found) < 0) {
        SDL_strlcpy(c->found, fname, sizeof(c->found));
    }
    return SDL_ENUM_CONTINUE;
}

typedef struct {
    RecSrv *sv;
    char    dir[900];
    bool    failed;
} ListCtx;

static SDL_EnumerationResult SDLCALL upListEntry(void *userdata,
                                                 const char *dirname,
                                                 const char *fname) {
    ListCtx *c = (ListCtx *)userdata;
    RecSrv *sv = c->sv;
    char path[1000];
    SDL_PathInfo pi;
    RecUpFile *nf;
    size_t n;
    (void)dirname;
    if (strcmp(fname, REC_LOWDISK_MARKER) == 0) {
        sv->upLowDisk = true;
        return SDL_ENUM_CONTINUE;
    }
    if (strcmp(fname, REC_SENT_MARKER) == 0 ||
        strcmp(fname, REC_UPLOADED_MARKER) == 0) {
        return SDL_ENUM_CONTINUE;
    }
    if (!brainRecNameOk(fname, true)) return SDL_ENUM_CONTINUE;
    snprintf(path, sizeof(path), "%s/%s", c->dir, fname);
    if (!SDL_GetPathInfo(path, &pi) || pi.type != SDL_PATHTYPE_FILE) {
        return SDL_ENUM_CONTINUE;
    }
    if (pi.size > 0xFFFFFFFFull) {
        fprintf(stderr, "[bothost] %s is too big to send; it stays here\n",
                path);
        return SDL_ENUM_CONTINUE;
    }
    nf = (RecUpFile *)realloc(sv->files, sizeof(RecUpFile) *
                                         (size_t)(sv->fileCount + 1));
    if (nf == NULL) {
        c->failed = true;
        return SDL_ENUM_FAILURE;
    }
    sv->files = nf;
    SDL_strlcpy(sv->files[sv->fileCount].name, fname,
                sizeof(sv->files[sv->fileCount].name));
    sv->files[sv->fileCount].size = (uint32_t)pi.size;
    sv->fileCount++;
    sv->total += pi.size;
    n = strlen(fname);
    if (n > 4 && strcmp(fname + n - 4, HOSTREC_EXT) == 0) sv->bots++;
    return SDL_ENUM_CONTINUE;
}

/* ---- Log compression ------------------------------------------------------ */
/* A finished block's logs (print2_bot*.log) are gzipped before they are
 * sent, by the same rule the server uses for its own blocks. The work runs
 * on a thread, so the bots of the other servers keep their think rate. */
static SDL_Thread   *s_logzThread = NULL;
static SDL_AtomicInt s_logzDone;
static int           s_logzCount = 0;
static char          s_logzDir[900];      /* the folder the thread works on */
static char          s_logzDoneDir[900];  /* the last folder it finished */

static int SDLCALL logzThreadFn(void *arg) {
    (void)arg;
    s_logzCount = brainRecordCompressLogsNow(s_logzDir);
    SDL_SetAtomicInt(&s_logzDone, 1);
    return 0;
}

static void logzJoin(void) {
    if (s_logzThread == NULL) return;
    SDL_WaitThread(s_logzThread, NULL);
    s_logzThread = NULL;
    SDL_strlcpy(s_logzDoneDir, s_logzDir, sizeof(s_logzDoneDir));
    if (s_logzCount > 0) {
        fprintf(stderr, "[bothost] gzipped %d log file(s) in %s\n",
                s_logzCount, s_logzDir);
    }
}

/* True when the logs of dir are gzipped (or that was tried). False while
 * the thread works; ask again later. */
static bool upLogsReady(const char *dir) {
    if (s_logzThread != NULL) {
        if (SDL_GetAtomicInt(&s_logzDone) == 0) return false;
        logzJoin();
    }
    if (strcmp(s_logzDoneDir, dir) == 0) return true;
    /* The brains' log writer keeps its files open: close them first. It
     * opens them again at its next line. */
    brainCoreReleaseLogFiles();
    SDL_strlcpy(s_logzDir, dir, sizeof(s_logzDir));
    SDL_SetAtomicInt(&s_logzDone, 0);
    s_logzThread = SDL_CreateThread(logzThreadFn, "bothost_logz", NULL);
    if (s_logzThread == NULL) {
        /* No thread: send the logs as they are. */
        SDL_strlcpy(s_logzDoneDir, dir, sizeof(s_logzDoneDir));
        return true;
    }
    return false;
}

/* Start sending the next finished block; false when none is left. A block
 * whose logs are being gzipped counts as started: the call comes again. */
static bool upStart(int srv) {
    RecSrv *sv = &s_srv[srv];
    PickCtx pc;
    ListCtx lc;
    upReset(sv);
    if (!recIsDir(sv->root)) return false;
    memset(&pc, 0, sizeof(pc));
    pc.sv = sv;
    (void)SDL_EnumerateDirectory(sv->root, upPickEntry, &pc);
    if (pc.found[0] == '\0') return false;
    memset(&lc, 0, sizeof(lc));
    lc.sv = sv;
    snprintf(lc.dir, sizeof(lc.dir), "%s/%s", sv->root, pc.found);
    if (!upLogsReady(lc.dir)) {
        sv->retryAtMs = (uint32_t)SDL_GetTicks() + REC_STATE_MS;
        return true;
    }
    (void)SDL_EnumerateDirectory(lc.dir, upListEntry, &lc);
    if (lc.failed) {
        upReset(sv);
        return false;
    }
    SDL_strlcpy(sv->upBlock, pc.found, sizeof(sv->upBlock));
    sv->active = true;
    fprintf(stderr, "[bothost] sending block %s to %s: %d files, %.1f MB\n",
            sv->upBlock, sv->key, sv->fileCount,
            (double)sv->total / (1024.0 * 1024.0));
    return true;
}

/* The server merged a block: mark it uploaded so it is not sent again.
 * Its files stay here. */
static void upMarkUploaded(RecSrv *sv, const char *block, uint64_t bytes) {
    char dir[900];
    snprintf(dir, sizeof(dir), "%s/%s", sv->root, block);
    if (!recIsDir(dir)) return;
    recTouch(dir, REC_UPLOADED_MARKER);
    if (bytes > 0) {
        fprintf(stderr, "[bothost] block %s copied to %s and kept here (%.1f "
                        "MB in %s)\n", block, sv->key,
                (double)bytes / (1024.0 * 1024.0), dir);
    } else {
        fprintf(stderr, "[bothost] block %s copied to %s and kept here (%s)\n",
                block, sv->key, dir);
    }
}

/* Send the next piece. */
static void upPiece(int srv) {
    RecSrv *sv = &s_srv[srv];
    const RecUpFile *f;
    BulkStreamHeader h;
    uint8_t *buf;
    uint32_t want;
    size_t got;

    if (sv->fileIdx >= sv->fileCount) {
        /* Every piece is queued: say so once the last one left. */
        if (s_io.bulkIdle != NULL && !s_io.bulkIdle(srv)) return;
        recSendEvt(srv, BOT_HOST_REC_OP_DONE,
                   sv->upLowDisk ? BOT_HOST_REC_F_LOWDISK : 0, 0, 100,
                   (uint8_t)sv->bots, (uint8_t)sv->bots,
                   (uint32_t)sv->fileCount, (uint32_t)sv->total, sv->upBlock);
        sv->waitAck = true;
        sv->ackDeadline = (uint32_t)SDL_GetTicks() + REC_ACK_WAIT_MS;
        return;
    }
    f = &sv->files[sv->fileIdx];
    if (sv->fp == NULL) {
        char path[1000];
        snprintf(path, sizeof(path), "%s/%s/%s", sv->root, sv->upBlock, f->name);
        sv->fp = fopen(path, "rb");
        sv->fileOff = 0;
        if (sv->fp == NULL) {
            fprintf(stderr, "[bothost] cannot read %s; the block is sent "
                            "later\n", path);
            upReset(sv);
            sv->retryAtMs = (uint32_t)SDL_GetTicks() + REC_RETRY_MS;
            return;
        }
    }
    want = f->size - sv->fileOff;
    if (want > REC_PIECE) want = REC_PIECE;
    buf = (uint8_t *)malloc(want > 0 ? want : 1);
    if (buf == NULL) return;
    got = want > 0 ? fread(buf, 1, want, sv->fp) : 0;
    if (got != want) {
        /* The file got shorter: send it again from the start later. */
        free(buf);
        upReset(sv);
        sv->retryAtMs = (uint32_t)SDL_GetTicks() + REC_RETRY_MS;
        return;
    }
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_REC_CHUNK;
    h.gen = f->size;
    h.totalSize = want;
    snprintf(h.path, sizeof(h.path), "%s/%s/%u", sv->upBlock, f->name,
             (unsigned)sv->fileOff);
    h.pathLen = (uint8_t)strlen(h.path);
    if (s_io.bulkSend == NULL || !s_io.bulkSend(srv, &h, buf, want)) {
        /* Not taken now: the same piece goes next time. */
        free(buf);
        if (rec_fseek(sv->fp, (int64_t)sv->fileOff, SEEK_SET) != 0) {
            upReset(sv);
        }
        return;
    }
    free(buf);
    sv->fileOff += want;
    sv->sent += want;
    sv->lastPieceMs = (uint32_t)SDL_GetTicks();
    if (sv->fileOff >= f->size) {
        size_t n = strlen(f->name);
        fclose(sv->fp);
        sv->fp = NULL;
        sv->fileOff = 0;
        if (n > 4 && strcmp(f->name + n - 4, HOSTREC_EXT) == 0) sv->botsDone++;
        sv->fileIdx++;
    }
}

static void srvAck(int srv, const ControlEvent *evt) {
    RecSrv *sv = &s_srv[srv];
    const char *block = evt->u.botHostRec.block;
    uint8_t res = evt->u.botHostRec.state;
    if (!brainRecNameOk(block, false)) return;
    if (!sv->active || strcmp(block, sv->upBlock) != 0) {
        /* An answer to an upload of an earlier connection. */
        if ((res == BOT_HOST_REC_ACK_MERGED || res == BOT_HOST_REC_ACK_SAVED) &&
            !(sv->open && strcmp(block, sv->block) == 0)) {
            upMarkUploaded(sv, block, 0);
        }
        return;
    }
    if (res == BOT_HOST_REC_ACK_MERGED || res == BOT_HOST_REC_ACK_SAVED) {
        uint64_t bytes = sv->total;
        fprintf(stderr, "[bothost] %s %s block %s\n", sv->key,
                res == BOT_HOST_REC_ACK_MERGED ? "merged" : "kept", block);
        upReset(sv);
        upMarkUploaded(sv, block, bytes);
        sv->fails = 0;
        return;
    }
    fprintf(stderr, "[bothost] %s did not merge block %s: %s (%u host files "
                    "could not be read, %u filled no entry); it stays here, "
                    "not marked uploaded, and is sent again later\n", sv->key,
            block, brainRecMergeFailText(evt->u.botHostRec.percent),
            (unsigned)evt->u.botHostRec.botsDone,
            (unsigned)evt->u.botHostRec.botsTotal);
    if (++sv->fails >= REC_MAX_FAILS) {
        if (sv->skipCount < 8) {
            SDL_strlcpy(sv->skipBlocks[sv->skipCount++], block,
                        sizeof(sv->skipBlocks[0]));
        }
        sv->fails = 0;
        fprintf(stderr, "[bothost] block %s failed %d times; it waits for the "
                        "next connection\n", block, REC_MAX_FAILS);
    }
    upReset(sv);
    sv->retryAtMs = (uint32_t)SDL_GetTicks() + REC_RETRY_MS;
}

typedef struct {
    RecSrv  *sv;
    uint64_t bytes;
    uint32_t files;
} ClearCtx;

/* One entry of the server's folder: delete it unless it is the open block. */
static SDL_EnumerationResult SDLCALL clearEntry(void *userdata,
                                                const char *dirname,
                                                const char *fname) {
    ClearCtx *c = (ClearCtx *)userdata;
    char path[900];
    uint64_t b = 0;
    uint32_t f = 0;
    (void)dirname;
    if (c->sv->open && strcmp(fname, c->sv->block) == 0) {
        return SDL_ENUM_CONTINUE;
    }
    snprintf(path, sizeof(path), "%s/%s", c->sv->root, fname);
    (void)brainRecRemoveTree(s_recRoot, path, &b, &f);
    c->bytes += b;
    c->files += f;
    return SDL_ENUM_CONTINUE;
}

/* CLEAR: delete this server's finished blocks. */
static void srvClear(int srv) {
    RecSrv *sv = &s_srv[srv];
    uint64_t total = 0;
    uint32_t files = 0;
    if (sv->active) upReset(sv);
    if (recIsDir(sv->root)) {
        if (!sv->open) {
            (void)brainRecRemoveTree(s_recRoot, sv->root, &total, &files);
        } else {
            /* Keep the block open now. */
            ClearCtx cc;
            memset(&cc, 0, sizeof(cc));
            cc.sv = sv;
            (void)SDL_EnumerateDirectory(sv->root, clearEntry, &cc);
            total = cc.bytes;
            files = cc.files;
        }
    }
    sv->skipCount = 0;
    fprintf(stderr, "[bothost] %s asked to clear its recordings: %.1f MB in "
                    "%u files deleted\n", sv->key,
            (double)total / (1024.0 * 1024.0), (unsigned)files);
    recSendEvt(srv, BOT_HOST_REC_OP_CLEARED, 0, 0, 0, 0, 0,
               (uint32_t)(total & 0xFFFFFFFFu), (uint32_t)(total >> 32), NULL);
}

/* ---- Public ------------------------------------------------------------------ */

void hostRecInit(const char *cacheDir, const char *hostName,
                 double minFreeGb, const HostRecIo *io) {
    memset(s_srv, 0, sizeof(s_srv));
    memset(s_seat, 0, sizeof(s_seat));
    SDL_strlcpy(s_cache, cacheDir, sizeof(s_cache));
    snprintf(s_recRoot, sizeof(s_recRoot), "%s/recordings", s_cache);
    SDL_strlcpy(s_hostName, hostName, sizeof(s_hostName));
    s_minFree = minFreeGb > 0.0 ? (uint64_t)(minFreeGb * 1e9) : 0u;
    s_io = *io;
    brainRecBufInit(&s_body);
    (void)SDL_CreateDirectory(s_cache);
    (void)SDL_CreateDirectory(s_recRoot);
    s_init = true;
}

void hostRecServerUp(int srv, const char *label) {
    RecSrv *sv = recSrv(srv);
    if (!s_init || sv == NULL) return;
    upReset(sv);
    sv->up = true;
    sv->open = false;
    sv->lowDisk = false;
    sv->want = false;
    sv->paced = true;
    sv->fails = 0;
    sv->skipCount = 0;
    sv->stateSent = false;
    /* The folder is named after the address the host knows the server by,
     * so a block left from before goes back to the same server. */
    brainRecSafeName(label, sv->key, sizeof(sv->key), true);
    snprintf(sv->root, sizeof(sv->root), "%s/%s", s_recRoot, sv->key);
}

void hostRecServerDown(int srv) {
    RecSrv *sv = recSrv(srv);
    if (!s_init || sv == NULL) return;
    srvCloseSeats(srv);
    upReset(sv);
    sv->up = false;
    sv->open = false;
    sv->want = false;
}

void hostRecOnEvent(int srv, const ControlEvent *evt) {
    RecSrv *sv = recSrv(srv);
    if (!s_init || sv == NULL || !sv->up) return;
    switch (evt->u.botHostRec.op) {
        case BOT_HOST_REC_OP_OPEN:  srvOpen(srv, evt); break;
        case BOT_HOST_REC_OP_CLOSE: srvClose(srv); break;
        case BOT_HOST_REC_OP_UPLOAD:
            sv->want = true;
            sv->paced = (evt->u.botHostRec.flags & BOT_HOST_REC_F_PACED) != 0;
            sv->retryAtMs = 0;
            break;
        case BOT_HOST_REC_OP_HOLD:
            sv->paced = true;
            break;
        case BOT_HOST_REC_OP_CLEAR: srvClear(srv); break;
        case BOT_HOST_REC_OP_ACK:   srvAck(srv, evt); break;
        default: break;
    }
}

static void srvReportState(int srv, uint32_t now) {
    RecSrv *sv = &s_srv[srv];
    uint8_t st, pct = 0;
    if (sv->active) {
        st = BOT_HOST_REC_STATE_UPLOADING;
        pct = sv->total > 0 ? (uint8_t)(sv->sent * 100u / sv->total) : 100u;
        if (pct > 100) pct = 100;
    } else if (sv->open && sv->lowDisk) {
        st = BOT_HOST_REC_STATE_LOWDISK;
    } else if (sv->open) {
        st = BOT_HOST_REC_STATE_RECORDING;
    } else {
        st = BOT_HOST_REC_STATE_NONE;
    }
    if (sv->stateSent && st == sv->lastState && pct == sv->lastPct &&
        (uint8_t)sv->botsDone == sv->lastBotsDone) {
        return;
    }
    if (sv->stateSent && st == sv->lastState && now - sv->lastStateMs < REC_STATE_MS) {
        return;
    }
    recSendEvt(srv, BOT_HOST_REC_OP_STATE, 0, st, pct, (uint8_t)sv->botsDone,
               (uint8_t)sv->bots, 0, 0, sv->active ? sv->upBlock : sv->block);
    sv->stateSent = true;
    sv->lastState = st;
    sv->lastPct = pct;
    sv->lastBotsDone = (uint8_t)sv->botsDone;
    sv->lastStateMs = now;
}

void hostRecService(uint32_t nowMs) {
    int srv;
    if (!s_init) return;

    if (!s_diskChecked || nowMs - s_lastDiskMs >= REC_DISK_CHECK_MS) {
        uint64_t freeB = 0;
        bool known = hostRecFreeBytes(s_cache, &freeB);
        bool low = known && freeB < s_minFree;
        s_lastDiskMs = nowMs;
        s_diskChecked = true;
        if (low && !s_diskLow) {
            fprintf(stderr, "[bothost] the brain cache disk has %.1f GB free "
                            "(under %.1f GB): recording stops\n",
                    (double)freeB / 1e9, (double)s_minFree / 1e9);
            for (srv = 0; srv < REC_MAX_SERVERS; srv++) {
                if (s_srv[srv].up && s_srv[srv].open) srvStopLowDisk(srv);
            }
        }
        /* A block stopped for low disk stays stopped; the next one checks
         * again when it opens. */
        s_diskLow = low;
    }

    for (srv = 0; srv < REC_MAX_SERVERS; srv++) {
        RecSrv *sv = &s_srv[srv];
        if (!sv->up) continue;
        if (sv->want && !sv->active &&
            (sv->retryAtMs == 0 || (int32_t)(nowMs - sv->retryAtMs) >= 0)) {
            if (!upStart(srv)) sv->want = false;
        }
        if (sv->active && sv->waitAck) {
            if ((int32_t)(nowMs - sv->ackDeadline) >= 0) {
                fprintf(stderr, "[bothost] %s did not answer for block %s; it "
                                "is sent again later\n", sv->key, sv->upBlock);
                upReset(sv);
                sv->retryAtMs = nowMs + REC_RETRY_MS;
            }
        } else if (sv->active) {
            if (!sv->paced) {
                /* As fast as the bulk channel takes it. */
                int n;
                for (n = 0; n < 4 && sv->active && !sv->waitAck; n++) {
                    if (s_io.bulkIdle != NULL && !s_io.bulkIdle(srv)) break;
                    upPiece(srv);
                }
            } else if (nowMs - sv->lastPieceMs >= REC_PACED_MS) {
                if (s_io.bulkIdle == NULL || s_io.bulkIdle(srv)) upPiece(srv);
            }
        }
        srvReportState(srv, nowMs);
    }
}

void hostRecShutdown(void) {
    int i;
    if (!s_init) return;
    for (i = 0; i < REC_MAX_SEATS; i++) hostRecSeatGone(i);
    for (i = 0; i < REC_MAX_SERVERS; i++) upReset(&s_srv[i]);
    logzJoin();
    brainRecBufFree(&s_body);
    s_init = false;
}
