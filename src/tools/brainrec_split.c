/*********************************************************
 *NAME:          brainrec_split.c
 *PURPOSE:
 *  Splits one -braindebug session (or a bare brainrec.btr) into several
 *  smaller, self-contained sessions that BrainTest can load one at a
 *  time. A full-pool 16-bot recording is gigabytes on disk and BrainTest
 *  materialises every frame when it loads one, so a long game costs more
 *  RAM than the machine has. Halving the frame count halves that cost.
 *
 *  Why a splitter and not a byte-chopper: brainrec.btr is a gzip stream
 *  of variable-length frames, and its map track is delta-encoded — only
 *  every BRAINREC_MAP_KEYFRAME_INTERVAL'th frame carries the whole map.
 *  A part that starts anywhere else replays with the terrain of whatever
 *  keyframe it never saw. So the tool walks real frames, cuts only on a
 *  frame that carries a full map, and gives each part its own copy of
 *  the file header and legend block.
 *
 *  Memory: the tool never holds a frame. It walks with the shared
 *  brain_record_walk.c walker, which hands every variable-length block
 *  to a reader that copies it through a fixed 256 KB buffer straight
 *  into the output gzip stream. Peak working set is a few MB regardless
 *  of how big the recording is; the tool prints it when it finishes.
 *
 *  Companion logs are sliced to match. Careful: .btr frame ticks are ENGINE
 *  ticks, while print2/jsonl carry the brain's own state.tick — half the
 *  rate (a brain thinks every other engine tick) AND restarted from 1 every
 *  time the brain is recreated, which a respawning bot does several times a
 *  game. See the "Brain-tick runs" block below for how the two are lined up.
 *
 *  Usage:
 *    brainrec_split <session-dir | brainrec.btr> [-parts N]
 *                   [-frames-per-part N] [-out <dir>] [-dry-run]
 *                   [-companions-only]
 *
 *  The input is opened read-only and never modified; the tool refuses to
 *  start if a target directory already exists.
 *********************************************************/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
/* SDL_SetMainReady() lives here, and SDL3's SDL.h does not pull SDL_main.h
 * in the way SDL2's did -- so with SDL_MAIN_HANDLED set and this header
 * missing, the call below was an implicit declaration. That is an error on
 * clang 15+ and gcc 14+, which is every current macOS and most current
 * Linux toolchains. */
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "brain_record_walk.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#define SPLIT_PATH_MAX      1024
#define SPLIT_MAX_PARTS     64
#define SPLIT_MAX_BOTS      64
#define SPLIT_MAX_RUNS      512
#define SPLIT_COPY_BYTES    (256 * 1024)   /* frame-copy window */
#define SPLIT_TEXT_BYTES    (1024 * 1024)  /* companion-log window */
#define SPLIT_LINE_PREFIX   256            /* bytes of a line needed to read its tick */

/* ---- Byte source for the shared walker ---------------------------------- */

/* One gzip input, optionally teed into one gzip output. `out` NULL means
 * "consume and discard", which is what the index and verify passes do. */
typedef struct {
    gzFile         in;
    gzFile         out;
    bool           writeFailed;
    unsigned char *buf;         /* SPLIT_COPY_BYTES scratch for skip() */
} GzTee;

static size_t gzTeeRead(void *ctx, void *dst, size_t len) {
    GzTee *t = (GzTee *)ctx;
    int got = gzread(t->in, dst, (unsigned)len);
    if (got <= 0) return 0;
    if (t->out && gzwrite(t->out, dst, (unsigned)got) != got) t->writeFailed = true;
    return (size_t)got;
}

/* Consume len bytes. Deliberately a bounded read loop rather than gzseek:
 * gzseek costs the same inflate on a read stream, and this version is the
 * copy path too, so both directions go through one piece of code. */
static bool gzTeeSkip(void *ctx, size_t len) {
    GzTee *t = (GzTee *)ctx;
    while (len > 0) {
        unsigned chunk = (unsigned)(len < SPLIT_COPY_BYTES ? len : SPLIT_COPY_BYTES);
        int got = gzread(t->in, t->buf, chunk);
        if (got != (int)chunk) return false;
        if (t->out && gzwrite(t->out, t->buf, (unsigned)got) != got) t->writeFailed = true;
        len -= (size_t)got;
    }
    return true;
}

static void gzTeeBind(BrainRecReader *r, GzTee *t) {
    r->ctx     = t;
    r->read    = gzTeeRead;
    r->skip    = gzTeeSkip;
    r->version = 0;   /* the preamble walk sets the file's own */
}

/* ---- Frame index -------------------------------------------------------- */

typedef struct {
    uint32_t tick;
    uint8_t  keyframe;
} FrameIx;

typedef struct {
    FrameIx *v;
    size_t   count;
    size_t   cap;
} FrameIxList;

static bool frameIxPush(FrameIxList *l, uint32_t tick, bool keyframe) {
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 4096;
        FrameIx *nv = (FrameIx *)realloc(l->v, nc * sizeof *nv);
        if (!nv) return false;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->count].tick     = tick;
    l->v[l->count].keyframe = keyframe ? 1 : 0;
    l->count++;
    return true;
}

/* ---- Companion-log classification --------------------------------------- */

/* Which tick counter a companion log is stamped with. The .btr is the only
 * file in engine ticks apart from the two server-side logs; everything the
 * brains write is in brain ticks. */
typedef enum {
    TICK_NONE = 0,   /* no tick markers — copied whole into every part */
    TICK_PRINT2,     /* "===TICK <n>==="            brain ticks */
    TICK_JSONL,      /* {"type":"tick","t":<n>,     brain ticks */
    TICK_KILLBOT,    /* "... tick=<n> bot=<n> ..."  engine ticks */
    TICK_PERF,       /* "PERF t=<n> ..."            engine ticks */
    TICK_PERFTICKS   /* {"tick":<n>,"bot":<n>,      engine ticks */
} TickKind;

static bool hasPrefix(const char *s, const char *pre) {
    size_t n = strlen(pre);
    return strncmp(s, pre, n) == 0;
}

static bool hasSuffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

static TickKind classifyCompanion(const char *name) {
    if (hasPrefix(name, "print2_bot") && hasSuffix(name, ".log")) return TICK_PRINT2;
    if (hasPrefix(name, "player")     && hasSuffix(name, ".jsonl")) return TICK_JSONL;
    if (strcmp(name, "killbot.log") == 0)          return TICK_KILLBOT;
    if (strcmp(name, "braindbg_perf.log") == 0)    return TICK_PERF;
    if (strcmp(name, "performance.ticks.log") == 0) return TICK_PERFTICKS;
    return TICK_NONE;
}

static bool tickKindIsEngine(TickKind k) {
    return k == TICK_KILLBOT || k == TICK_PERF || k == TICK_PERFTICKS;
}

/* ---- Line tick parsing (bounded — never runs off the read window) -------- */

static long long scanNumber(const char *p, size_t avail) {
    long long v = 0;
    size_t i = 0;
    while (i < avail && p[i] >= '0' && p[i] <= '9') {
        if (v > 100000000000LL) return -1;   /* not a tick; ignore */
        v = v * 10 + (p[i] - '0');
        i++;
    }
    return i ? v : -1;
}

static bool windowStartsWith(const char *p, size_t avail, const char *lit) {
    size_t n = strlen(lit);
    return avail >= n && memcmp(p, lit, n) == 0;
}

/* Tick of the line starting at p, or -1 if this line carries none. */
static long long parseLineTick(TickKind kind, const char *p, size_t avail) {
    switch (kind) {
        case TICK_PRINT2:
            if (!windowStartsWith(p, avail, "===TICK ")) return -1;
            return scanNumber(p + 8, avail - 8);
        case TICK_JSONL:
            if (!windowStartsWith(p, avail, "{\"type\":\"tick\",\"t\":")) return -1;
            return scanNumber(p + 19, avail - 19);
        case TICK_PERFTICKS:
            if (!windowStartsWith(p, avail, "{\"tick\":")) return -1;
            return scanNumber(p + 8, avail - 8);
        case TICK_PERF:
            if (!windowStartsWith(p, avail, "PERF t=")) return -1;
            return scanNumber(p + 7, avail - 7);
        case TICK_KILLBOT: {
            /* "[<timestamp>] tick=<n> bot=<n> ..." — the stamp is fixed
             * width but searching is cheaper to keep correct. */
            size_t lim = avail < SPLIT_LINE_PREFIX ? avail : SPLIT_LINE_PREFIX;
            for (size_t i = 0; i + 5 <= lim; i++) {
                if (p[i] == '\n') break;
                if (memcmp(p + i, "tick=", 5) == 0) return scanNumber(p + i + 5, lim - i - 5);
            }
            return -1;
        }
        default:
            return -1;
    }
}

/* ---- Small filesystem helpers ------------------------------------------- */

static bool pathExists(const char *path) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi);
}

static long long fileSize(const char *path) {
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(path, &pi) || pi.type != SDL_PATHTYPE_FILE) return -1;
    return (long long)pi.size;
}

/* Split "a/b/c" into "a/b" + "c". Either output may be NULL. */
static void splitPath(const char *path, char *dirOut, size_t dirCap, char *baseOut, size_t baseCap) {
    const char *sl = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    if (sl) {
        if (dirOut) {
            size_t n = (size_t)(sl - path);
            if (n >= dirCap) n = dirCap - 1;
            memcpy(dirOut, path, n);
            dirOut[n] = '\0';
        }
        if (baseOut) SDL_strlcpy(baseOut, sl + 1, baseCap);
    } else {
        if (dirOut)  SDL_strlcpy(dirOut, ".", dirCap);
        if (baseOut) SDL_strlcpy(baseOut, path, baseCap);
    }
}

/* Trim a trailing slash so splitPath() sees the directory's own name. */
static void trimTrailingSlash(char *p) {
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = '\0';
}

static bool copyFileWhole(const char *src, const char *dst, unsigned char *buf, size_t bufLen) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    bool ok = true;
    for (;;) {
        size_t got = fread(buf, 1, bufLen, in);
        if (got == 0) break;
        if (fwrite(buf, 1, got, out) != got) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

/* ---- Line scanner -------------------------------------------------------- */

/* Hands a text file out in whole lines through a fixed window, so both the
 * analysis pass and the slicing pass see the same line boundaries and neither
 * has to hold the file. A line longer than the window comes out in pieces;
 * only the first piece reports isLineStart. */
typedef struct {
    FILE          *fp;
    unsigned char *buf;
    size_t         len, pos;
    bool           eof;
    bool           atLineStart;
} LineScan;

static void lineScanInit(LineScan *s, FILE *fp, unsigned char *buf) {
    s->fp  = fp;
    s->buf = buf;
    s->len = 0;
    s->pos = 0;
    s->eof = false;
    s->atLineStart = true;
}

static bool lineScanNext(LineScan *s, char **chunk, size_t *chunkLen,
                         bool *isLineStart, size_t *avail) {
    for (;;) {
        if (s->pos < s->len) {
            size_t have = s->len - s->pos;
            char  *p    = (char *)s->buf + s->pos;
            bool   ls   = s->atLineStart;
            /* At a line start, don't hand anything out until enough of the
             * line is in the window for the caller to read its tick. */
            bool needMore = ls && have < SPLIT_LINE_PREFIX && !s->eof;
            if (!needMore) {
                char *nl = (char *)memchr(p, '\n', have);
                if (nl || s->eof || have >= SPLIT_TEXT_BYTES) {
                    size_t n = nl ? (size_t)(nl - p) + 1 : have;
                    *chunk = p;
                    *chunkLen = n;
                    *isLineStart = ls;
                    *avail = have;
                    s->pos += n;
                    s->atLineStart = (nl != NULL);
                    return true;
                }
            }
        }
        if (s->eof && s->pos >= s->len) return false;
        if (s->pos > 0) {
            memmove(s->buf, s->buf + s->pos, s->len - s->pos);
            s->len -= s->pos;
            s->pos  = 0;
        }
        if (s->eof) continue;   /* bytes left with no newline — emit them next */
        size_t got = fread(s->buf + s->len, 1, SPLIT_TEXT_BYTES - s->len, s->fp);
        if (got == 0) s->eof = true;
        else          s->len += got;
    }
}

/* ---- Brain-tick runs ----------------------------------------------------- */

/* A brain's tick counter is state.tick in its own Lua state, so it restarts at
 * 1 every time the brain is recreated (a bot dying and respawning in a
 * survival game does it several times). print2/jsonl tick numbers are
 * therefore NOT monotonic across a game and can't be compared against the
 * .btr's engine ticks directly. What IS fixed is the rate: within one run the
 * brain thinks every other engine tick, so brain tick b of run r sits at
 * engine tick runStart + 2*(b-1).
 *
 * runStart comes from killbot.log, the only file that stamps a brain-side
 * event with an engine tick: its Nth entry for a bot is the Nth "TICK KILLED"
 * banner in that bot's print2 log, so runStart = engineTick - 2*(brainTick-1).
 * Measured on 20260827_020243_1_survival, bot 8: killbot engine 522 against
 * print2 brain 11 gives runStart 502 — exactly the recording's first frame. */
typedef struct {
    long long startEngine;
    long long maxTick;
    bool      haveStart;
} RunInfo;

/* Where a part boundary falls in a brain log: run index plus brain tick. */
typedef struct {
    int       run;
    long long tick;
} BrainCut;

typedef struct {
    BrainCut cut[SPLIT_MAX_PARTS];
    int      runs;
    bool     valid;
} BotCuts;

/* Engine ticks of each killbot.log entry, per bot. */
typedef struct {
    long long *v[SPLIT_MAX_BOTS];
    int        n[SPLIT_MAX_BOTS];
    int        cap[SPLIT_MAX_BOTS];
} KillAnchors;

static void killAnchorsFree(KillAnchors *ka) {
    for (int i = 0; i < SPLIT_MAX_BOTS; i++) free(ka->v[i]);
    memset(ka, 0, sizeof *ka);
}

static bool killAnchorsPush(KillAnchors *ka, int bot, long long tick) {
    if (bot < 0 || bot >= SPLIT_MAX_BOTS) return true;
    if (ka->n[bot] == ka->cap[bot]) {
        int nc = ka->cap[bot] ? ka->cap[bot] * 2 : 64;
        long long *nv = (long long *)realloc(ka->v[bot], (size_t)nc * sizeof *nv);
        if (!nv) return false;
        ka->v[bot] = nv;
        ka->cap[bot] = nc;
    }
    ka->v[bot][ka->n[bot]++] = tick;
    return true;
}

/* "[<stamp>] tick=<engine> bot=<n> KILLED: ..." */
static bool loadKillAnchors(const char *path, KillAnchors *ka, unsigned char *buf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    LineScan s;
    lineScanInit(&s, fp, buf);
    char *chunk; size_t clen, avail; bool ls;
    bool ok = true;
    while (ok && lineScanNext(&s, &chunk, &clen, &ls, &avail)) {
        if (!ls) continue;
        size_t lim = avail < SPLIT_LINE_PREFIX ? avail : SPLIT_LINE_PREFIX;
        long long tick = -1, bot = -1;
        for (size_t i = 0; i + 5 <= lim; i++) {
            if (chunk[i] == '\n') break;
            if (tick < 0 && memcmp(chunk + i, "tick=", 5) == 0) tick = scanNumber(chunk + i + 5, lim - i - 5);
            if (bot  < 0 && memcmp(chunk + i, "bot=",  4) == 0) bot  = scanNumber(chunk + i + 4, lim - i - 4);
            if (tick >= 0 && bot >= 0) break;
        }
        if (tick >= 0 && bot >= 0) ok = killAnchorsPush(ka, (int)bot, tick);
    }
    fclose(fp);
    return ok;
}

static bool lineIsTickKilled(const char *p, size_t avail) {
    size_t lim = avail < SPLIT_LINE_PREFIX ? avail : SPLIT_LINE_PREFIX;
    for (size_t i = 0; i + 11 <= lim; i++) {
        if (p[i] == '\n') break;
        if (memcmp(p + i, "TICK KILLED", 11) == 0) return true;
    }
    return false;
}

/* Pass over one brain log to find its runs: where each restart happens, how
 * far each run's tick counter got, and (from the kill anchors) which engine
 * tick each run started at. Returns the run count, or -1. */
static int scanBrainRuns(const char *path, TickKind kind, const long long *anchorE,
                         int nAnchors, long long firstEngineTick,
                         RunInfo *runs, unsigned char *buf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    LineScan s;
    lineScanInit(&s, fp, buf);

    memset(runs, 0, (size_t)SPLIT_MAX_RUNS * sizeof *runs);
    int nRuns = 1;
    long long prevTick = -1, curTick = -1;
    int kills = 0;

    char *chunk; size_t clen, avail; bool ls;
    while (lineScanNext(&s, &chunk, &clen, &ls, &avail)) {
        if (!ls) continue;
        long long t = parseLineTick(kind, chunk, avail);
        if (t >= 0) {
            if (prevTick >= 0 && t < prevTick && nRuns < SPLIT_MAX_RUNS) nRuns++;
            prevTick = curTick = t;
            if (t > runs[nRuns - 1].maxTick) runs[nRuns - 1].maxTick = t;
            continue;
        }
        if (kind == TICK_PRINT2 && curTick >= 0 && lineIsTickKilled(chunk, avail)) {
            if (!runs[nRuns - 1].haveStart && kills < nAnchors) {
                runs[nRuns - 1].startEngine = anchorE[kills] - 2 * (curTick - 1);
                runs[nRuns - 1].haveStart = true;
            }
            kills++;
        }
    }
    fclose(fp);

    /* A run with no kill in it gets chained off the previous run's end; so
     * does an anchor that came out non-monotonic. */
    for (int r = 0; r < nRuns; r++) {
        long long chained = (r == 0) ? firstEngineTick
                                     : runs[r - 1].startEngine + 2 * runs[r - 1].maxTick;
        if (!runs[r].haveStart || (r > 0 && runs[r].startEngine < chained)) {
            runs[r].startEngine = chained;
            runs[r].haveStart = true;
        }
    }
    return nRuns;
}

/* Turn the parts' engine-tick boundaries into (run, brain tick) cut points. */
static void computeBrainCuts(const RunInfo *runs, int nRuns, int nParts,
                             const long long *partStartEngine, BotCuts *out) {
    memset(out, 0, sizeof *out);
    out->runs = nRuns;
    out->valid = true;
    for (int p = 1; p < nParts; p++) {
        long long eb = partStartEngine[p];
        int r = 0;
        for (int i = 0; i < nRuns; i++) {
            if (runs[i].startEngine <= eb) r = i;
        }
        long long b = (eb - runs[r].startEngine) / 2 + 1;
        if (b < 0) b = 0;
        if (b > runs[r].maxTick) {
            /* The boundary lands after this run ended: cut at the next
             * restart, or past the end of the log if there isn't one. */
            if (r + 1 < nRuns) { r++; b = 0; }
            else               { b = runs[r].maxTick + 1; }
        }
        if (p > 1 && (r < out->cut[p - 1].run
                      || (r == out->cut[p - 1].run && b < out->cut[p - 1].tick))) {
            r = out->cut[p - 1].run;
            b = out->cut[p - 1].tick;
        }
        out->cut[p].run  = r;
        out->cut[p].tick = b;
    }
}

/* ---- Companion slicing --------------------------------------------------- */

typedef struct {
    long long lines;
    long long firstTick[SPLIT_MAX_PARTS];
    long long lastTick[SPLIT_MAX_PARTS];
    int       firstRun[SPLIT_MAX_PARTS];
    int       lastRun[SPLIT_MAX_PARTS];
    long long partLines[SPLIT_MAX_PARTS];
} SliceReport;

/* Stream one tick-stamped log into the part its lines belong to. Parts are
 * contiguous in time, so this is a single forward pass. Lines before the
 * first tick marker stay with part 1.
 *
 * Engine-tick logs (killbot, perf) compare their tick straight against
 * partStartEngine; brain logs compare (run, tick) against the cut points,
 * because their tick counter restarts. */
static bool sliceTextFile(const char *inPath, TickKind kind, int nParts,
                          const long long *partStartEngine, const BotCuts *cuts,
                          char outPaths[SPLIT_MAX_PARTS][SPLIT_PATH_MAX],
                          unsigned char *buf, SliceReport *rep) {
    FILE *in = fopen(inPath, "rb");
    if (!in) return false;
    FILE *out = fopen(outPaths[0], "wb");
    if (!out) { fclose(in); return false; }

    memset(rep, 0, sizeof *rep);
    for (int p = 0; p < nParts; p++) { rep->firstTick[p] = -1; rep->lastTick[p] = -1; }

    LineScan s;
    lineScanInit(&s, in, buf);
    int  cur = 0, runIdx = 0;
    long long prevTick = -1;
    bool ok = true;
    char *chunk; size_t clen, avail; bool ls;

    while (ok && lineScanNext(&s, &chunk, &clen, &ls, &avail)) {
        if (ls) {
            rep->lines++;
            long long t = parseLineTick(kind, chunk, avail);
            if (t >= 0) {
                if (prevTick >= 0 && t < prevTick) runIdx++;
                prevTick = t;
                for (;;) {
                    if (cur + 1 >= nParts) break;
                    bool past;
                    if (cuts) {
                        const BrainCut *c = &cuts->cut[cur + 1];
                        past = (runIdx > c->run) || (runIdx == c->run && t >= c->tick);
                    } else {
                        past = (t >= partStartEngine[cur + 1]);
                    }
                    if (!past) break;
                    if (fclose(out) != 0) ok = false;
                    cur++;
                    out = fopen(outPaths[cur], "wb");
                    if (!out) { ok = false; break; }
                }
                if (!ok) break;
                if (rep->firstTick[cur] < 0) { rep->firstTick[cur] = t; rep->firstRun[cur] = runIdx; }
                rep->lastTick[cur] = t;
                rep->lastRun[cur]  = runIdx;
            }
            rep->partLines[cur]++;
        }
        if (fwrite(chunk, 1, clen, out) != clen) { ok = false; break; }
    }

    if (ferror(in)) ok = false;
    fclose(in);
    if (out && fclose(out) != 0) ok = false;

    /* Parts the log never reached still need an (empty) file, so a part
     * directory never looks like it is missing a bot's log. */
    for (int p = cur + 1; p < nParts && ok; p++) {
        FILE *f = fopen(outPaths[p], "wb");
        if (!f) { ok = false; break; }
        fclose(f);
    }
    return ok;
}

/* ---- Passes -------------------------------------------------------------- */

/* Pass 1: walk every frame, recording its tick and whether it carries a full
 * map. Nothing is written and nothing but the (tick, keyframe) index is kept,
 * so this costs 5 bytes per frame no matter how fat the frames are. */
static bool indexFrames(const char *btrPath, FrameIxList *ix, char mapName[64],
                        uint32_t *legendLen, unsigned char *copyBuf) {
    gzFile g = gzopen(btrPath, "rb");
    if (!g) {
        fprintf(stderr, "brainrec_split: cannot open '%s'\n", btrPath);
        return false;
    }
    GzTee tee;
    memset(&tee, 0, sizeof tee);
    tee.in  = g;
    tee.buf = copyBuf;
    BrainRecReader r;
    gzTeeBind(&r, &tee);

    if (brainRecWalkPreamble(&r, mapName, legendLen) != BRAINREC_WALK_OK) {
        gzclose(g);
        fprintf(stderr, "brainrec_split: '%s' is not a brainrec.btr this build reads (v6 to v%u)\n",
                btrPath, (unsigned)brainRecWalkFormatVersion());
        return false;
    }

    Uint64 lastReport = SDL_GetTicks();
    for (;;) {
        BrainRecFrameInfo fi;
        BrainRecWalkStatus st = brainRecWalkFrame(&r, &fi);
        if (st == BRAINREC_WALK_EOF) break;
        if (st != BRAINREC_WALK_OK) {
            /* A recording killed mid-frame is normal enough (hard server
             * exit); keep what walked cleanly and say so. */
            fprintf(stderr, "brainrec_split: truncated frame after %llu frames; "
                            "splitting the clean prefix\n",
                    (unsigned long long)ix->count);
            break;
        }
        if (!frameIxPush(ix, fi.tick, fi.mapKeyframe)) {
            gzclose(g);
            fprintf(stderr, "brainrec_split: out of memory building frame index\n");
            return false;
        }
        Uint64 now = SDL_GetTicks();
        if (now - lastReport >= 2000) {
            lastReport = now;
            printf("  scanning... %llu frames (tick %u)\n",
                   (unsigned long long)ix->count, (unsigned)fi.tick);
            fflush(stdout);
        }
    }
    gzclose(g);
    return ix->count > 0;
}

/* Pass 2: copy frames [from,to) into an already-open output that has had the
 * preamble written. Frames go through the walker so the copy is made of whole
 * frames — a byte range guessed from file offsets could land mid-frame. */
static bool copyFrameRange(BrainRecReader *r, size_t count, uint32_t *firstTick,
                           uint32_t *lastTick, bool *firstIsKeyframe) {
    for (size_t i = 0; i < count; i++) {
        BrainRecFrameInfo fi;
        if (brainRecWalkFrame(r, &fi) != BRAINREC_WALK_OK) return false;
        if (i == 0) {
            *firstTick = fi.tick;
            *firstIsKeyframe = fi.mapKeyframe;
        }
        *lastTick = fi.tick;
    }
    return true;
}

/* Pass 3: re-walk a written part with the same walker the loader's peek uses
 * and report what is actually in it. */
static bool verifyPart(const char *btrPath, const char *expectMap, uint32_t expectLegendLen,
                       size_t *framesOut, uint32_t *firstTickOut, uint32_t *lastTickOut,
                       bool *firstKeyframeOut, unsigned char *copyBuf) {
    gzFile g = gzopen(btrPath, "rb");
    if (!g) return false;
    GzTee tee;
    memset(&tee, 0, sizeof tee);
    tee.in  = g;
    tee.buf = copyBuf;
    BrainRecReader r;
    gzTeeBind(&r, &tee);

    char mapName[64];
    uint32_t llen = 0;
    if (brainRecWalkPreamble(&r, mapName, &llen) != BRAINREC_WALK_OK) { gzclose(g); return false; }
    if (strcmp(mapName, expectMap) != 0 || llen != expectLegendLen) { gzclose(g); return false; }

    size_t frames = 0;
    uint32_t first = 0, last = 0;
    bool firstKf = false;
    for (;;) {
        BrainRecFrameInfo fi;
        BrainRecWalkStatus st = brainRecWalkFrame(&r, &fi);
        if (st == BRAINREC_WALK_EOF) break;
        if (st != BRAINREC_WALK_OK) { gzclose(g); return false; }
        if (frames == 0) { first = fi.tick; firstKf = fi.mapKeyframe; }
        last = fi.tick;
        frames++;
    }
    gzclose(g);
    *framesOut = frames;
    *firstTickOut = first;
    *lastTickOut = last;
    *firstKeyframeOut = firstKf;
    return true;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "brainrec_split <session-dir | brainrec.btr> [options]\n"
        "  -parts N             split into N parts (default 4)\n"
        "  -frames-per-part N   size parts by frame count instead of -parts\n"
        "  -out <dir>           where the parts go (default: beside the input)\n"
        "  -dry-run             report the split plan and write nothing\n"
        "  -companions-only     rewrite the parts' logs only; leave their .btr alone\n"
        "\n"
        "Cuts only on frames that carry a full map keyframe, so every part\n"
        "replays the right terrain. The input is never modified.\n");
}

/* Peak working set, for confirming the tool stayed small. */
static double peakResidentMB(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    }
#endif
    return -1.0;
}

int main(int argc, char **argv) {
    SDL_SetMainReady();

    const char *input = NULL;
    const char *outRoot = NULL;
    int  parts = 4;
    long framesPerPart = 0;
    bool dryRun = false;
    bool companionsOnly = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-parts") == 0 && i + 1 < argc) {
            parts = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-frames-per-part") == 0 && i + 1 < argc) {
            framesPerPart = atol(argv[++i]);
        } else if (strcmp(argv[i], "-out") == 0 && i + 1 < argc) {
            outRoot = argv[++i];
        } else if (strcmp(argv[i], "-dry-run") == 0) {
            dryRun = true;
        } else if (strcmp(argv[i], "-companions-only") == 0) {
            companionsOnly = true;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (!input) {
            input = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (!input) { usage(); return 2; }

    char inputPath[SPLIT_PATH_MAX];
    SDL_strlcpy(inputPath, input, sizeof inputPath);
    trimTrailingSlash(inputPath);

    /* A session dir carries the companion logs; a bare .btr is just the
     * recording. Both are supported so the tool works on a loose file. */
    bool sessionMode = false;
    char btrPath[SPLIT_PATH_MAX];
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(inputPath, &pi)) {
        fprintf(stderr, "brainrec_split: '%s' not found\n", inputPath);
        return 1;
    }
    if (pi.type == SDL_PATHTYPE_DIRECTORY) {
        sessionMode = true;
        SDL_snprintf(btrPath, sizeof btrPath, "%s/brainrec.btr", inputPath);
        if (fileSize(btrPath) < 0) {
            fprintf(stderr, "brainrec_split: '%s' has no brainrec.btr\n", inputPath);
            return 1;
        }
    } else {
        SDL_strlcpy(btrPath, inputPath, sizeof btrPath);
    }

    if (parts < 1 || parts > SPLIT_MAX_PARTS) {
        fprintf(stderr, "brainrec_split: -parts must be 1..%d\n", SPLIT_MAX_PARTS);
        return 2;
    }

    unsigned char *copyBuf = (unsigned char *)malloc(SPLIT_COPY_BYTES);
    unsigned char *textBuf = (unsigned char *)malloc(SPLIT_TEXT_BYTES);
    if (!copyBuf || !textBuf) {
        fprintf(stderr, "brainrec_split: out of memory\n");
        return 1;
    }

    long long srcBytes = fileSize(btrPath);
    printf("Input: %s (%.1f MB gzipped)\n", btrPath, srcBytes / 1.0e6);
    printf("Format version: %u\n", (unsigned)brainRecWalkFormatVersion());

    /* ---- Pass 1: index ---- */
    FrameIxList ix;
    memset(&ix, 0, sizeof ix);
    char mapName[64];
    memset(mapName, 0, sizeof mapName);
    uint32_t legendLen = 0;
    if (!indexFrames(btrPath, &ix, mapName, &legendLen, copyBuf)) {
        free(ix.v); free(copyBuf); free(textBuf);
        return 1;
    }
    size_t total = ix.count;
    size_t keyframes = 0;
    for (size_t i = 0; i < total; i++) keyframes += ix.v[i].keyframe;
    printf("Map: %s   legend: %u bytes\n", mapName, (unsigned)legendLen);
    printf("Frames: %llu (engine ticks %u..%u), %llu map keyframes\n",
           (unsigned long long)total, (unsigned)ix.v[0].tick,
           (unsigned)ix.v[total - 1].tick, (unsigned long long)keyframes);

    if (framesPerPart > 0) {
        parts = (int)((total + (size_t)framesPerPart - 1) / (size_t)framesPerPart);
        if (parts < 1) parts = 1;
        if (parts > SPLIT_MAX_PARTS) {
            fprintf(stderr, "brainrec_split: -frames-per-part %ld needs %d parts (max %d)\n",
                    framesPerPart, parts, SPLIT_MAX_PARTS);
            free(ix.v); free(copyBuf); free(textBuf);
            return 2;
        }
    }

    /* ---- Boundaries: nearest keyframe to each even division ---- */
    size_t bound[SPLIT_MAX_PARTS + 1];
    bound[0] = 0;
    bound[parts] = total;
    for (int p = 1; p < parts; p++) {
        size_t ideal = (size_t)((double)total * (double)p / (double)parts + 0.5);
        size_t best = 0;
        bool found = false;
        for (size_t i = bound[p - 1] + 1; i < total; i++) {
            if (!ix.v[i].keyframe) continue;
            if (!found || (size_t)llabs((long long)i - (long long)ideal)
                        < (size_t)llabs((long long)best - (long long)ideal)) {
                best = i;
                found = true;
            }
            if (i > ideal && found) break;   /* keyframes only get further away */
        }
        if (!found) {
            fprintf(stderr, "brainrec_split: not enough map keyframes for %d parts "
                            "(only %llu in the recording)\n",
                    parts, (unsigned long long)keyframes);
            free(ix.v); free(copyBuf); free(textBuf);
            return 1;
        }
        bound[p] = best;
    }
    for (int p = 1; p <= parts; p++) {
        if (bound[p] <= bound[p - 1]) {
            fprintf(stderr, "brainrec_split: keyframes too sparse to make %d distinct parts\n", parts);
            free(ix.v); free(copyBuf); free(textBuf);
            return 1;
        }
    }

    /* ---- Output names ---- */
    char parentDir[SPLIT_PATH_MAX], baseName[SPLIT_PATH_MAX];
    splitPath(inputPath, parentDir, sizeof parentDir, baseName, sizeof baseName);
    if (outRoot) SDL_strlcpy(parentDir, outRoot, sizeof parentDir);
    if (!sessionMode && hasSuffix(baseName, ".btr")) baseName[strlen(baseName) - 4] = '\0';

    char partDir[SPLIT_MAX_PARTS][SPLIT_PATH_MAX];   /* session mode */
    char partBtr[SPLIT_MAX_PARTS][SPLIT_PATH_MAX];
    for (int p = 0; p < parts; p++) {
        if (sessionMode) {
            /* The leading "<YYYYMMDD_HHMMSS>" of the source session name is
             * kept intact so BrainTest's btParseDirTime() still parses it and
             * the Load Session browser lists the part; the suffix goes after
             * the block number the same way -braindebug's own label does. */
            SDL_snprintf(partDir[p], sizeof partDir[p], "%s/%s_part%dof%d",
                         parentDir, baseName, p + 1, parts);
            SDL_snprintf(partBtr[p], sizeof partBtr[p], "%s/brainrec.btr", partDir[p]);
        } else {
            partDir[p][0] = '\0';
            SDL_snprintf(partBtr[p], sizeof partBtr[p], "%s/%s_part%dof%d.btr",
                         parentDir, baseName, p + 1, parts);
        }
    }

    printf("\nPlan: %d parts\n", parts);
    for (int p = 0; p < parts; p++) {
        size_t from = bound[p], to = bound[p + 1];
        printf("  part %d/%d: frames %llu..%llu (%llu), engine ticks %u..%u -> %s\n",
               p + 1, parts, (unsigned long long)from, (unsigned long long)(to - 1),
               (unsigned long long)(to - from), (unsigned)ix.v[from].tick,
               (unsigned)ix.v[to - 1].tick, sessionMode ? partDir[p] : partBtr[p]);
    }
    if (dryRun) {
        printf("\n-dry-run: nothing written.\n");
        free(ix.v); free(copyBuf); free(textBuf);
        return 0;
    }

    /* Refuse to touch anything that is already there — the input session and
     * any earlier run's output both stay untouched. -companions-only is the
     * exception: it reruns the log slicing into parts that already exist,
     * leaving their .btr files alone. */
    for (int p = 0; p < parts; p++) {
        const char *target = sessionMode ? partDir[p] : partBtr[p];
        if (pathExists(target) != companionsOnly) {
            fprintf(stderr, companionsOnly
                        ? "brainrec_split: '%s' does not exist; -companions-only needs the parts already written\n"
                        : "brainrec_split: '%s' already exists; refusing to overwrite\n",
                    target);
            free(ix.v); free(copyBuf); free(textBuf);
            return 1;
        }
    }
    for (int p = 0; p < parts && sessionMode && !companionsOnly; p++) {
        if (!SDL_CreateDirectory(partDir[p])) {
            fprintf(stderr, "brainrec_split: cannot create '%s' (%s)\n", partDir[p], SDL_GetError());
            free(ix.v); free(copyBuf); free(textBuf);
            return 1;
        }
    }

    /* ---- Pass 2: copy frames ---- */
    /* Each part needs its own copy of the header + legend. They're read once
     * into a small buffer here, so the input is opened exactly once more. */
    bool ok = true;
    gzFile in = companionsOnly ? NULL : gzopen(btrPath, "rb");
    unsigned char *preamble = NULL;
    if (!companionsOnly) {
        if (!in) {
            fprintf(stderr, "brainrec_split: cannot reopen '%s'\n", btrPath);
            free(ix.v); free(copyBuf); free(textBuf);
            return 1;
        }
        size_t preambleLen = brainRecWalkHeaderSize() + 4 + legendLen;
        preamble = (unsigned char *)malloc(preambleLen);
        if (!preamble || gzread(in, preamble, (unsigned)preambleLen) != (int)preambleLen) {
            fprintf(stderr, "brainrec_split: cannot read preamble\n");
            gzclose(in); free(preamble); free(ix.v); free(copyBuf); free(textBuf);
            return 1;
        }

        printf("\nWriting parts...\n");
        for (int p = 0; p < parts && ok; p++) {
            /* "wb1": the same fastest-deflate level the recorder writes with —
             * this data is zero-padded structs and repetitive JSON, so level 1
             * keeps nearly all the ratio for a fraction of the CPU. */
            gzFile out = gzopen(partBtr[p], "wb1");
            if (!out) {
                fprintf(stderr, "brainrec_split: cannot create '%s'\n", partBtr[p]);
                ok = false;
                break;
            }
            if (gzwrite(out, preamble, (unsigned)preambleLen) != (int)preambleLen) {
                fprintf(stderr, "brainrec_split: write failed on '%s'\n", partBtr[p]);
                gzclose(out);
                ok = false;
                break;
            }

            GzTee tee;
            memset(&tee, 0, sizeof tee);
            tee.in  = in;
            tee.out = out;
            tee.buf = copyBuf;
            BrainRecReader r;
            gzTeeBind(&r, &tee);
            /* The preamble was copied as bytes, not walked, so take the
             * file's version from the header in it: the frame tail depends
             * on it. */
            r.version = brainRecWalkHeaderVersion(preamble);

            uint32_t first = 0, last = 0;
            bool firstKf = false;
            size_t n = bound[p + 1] - bound[p];
            if (!copyFrameRange(&r, n, &first, &last, &firstKf) || tee.writeFailed) {
                fprintf(stderr, "brainrec_split: copy failed on part %d\n", p + 1);
                gzclose(out);
                ok = false;
                break;
            }
            if (gzclose(out) != Z_OK) {
                fprintf(stderr, "brainrec_split: close failed on '%s'\n", partBtr[p]);
                ok = false;
                break;
            }
            printf("  part %d/%d: %llu frames, ticks %u..%u, keyframe start=%s, %.1f MB\n",
                   p + 1, parts, (unsigned long long)n, (unsigned)first, (unsigned)last,
                   firstKf ? "yes" : "NO", fileSize(partBtr[p]) / 1.0e6);
            fflush(stdout);
        }
        gzclose(in);
        free(preamble);
    }

    if (!ok) {
        free(ix.v); free(copyBuf); free(textBuf);
        return 1;
    }

    /* ---- Companion logs ---- */
    if (sessionMode) {
        long long startEngine[SPLIT_MAX_PARTS];
        for (int p = 0; p < parts; p++) startEngine[p] = (long long)ix.v[bound[p]].tick;
        long long firstEngineTick = (long long)ix.v[0].tick;

        /* killbot.log is the bridge between the two tick counters: its Nth
         * entry for a bot is the Nth "TICK KILLED" banner in that bot's
         * print2 log, so it dates a brain tick in engine time. */
        KillAnchors anchors;
        memset(&anchors, 0, sizeof anchors);
        char killPath[SPLIT_PATH_MAX];
        SDL_snprintf(killPath, sizeof killPath, "%s/killbot.log", inputPath);
        bool haveAnchors = loadKillAnchors(killPath, &anchors, textBuf);

        static BotCuts botCuts[SPLIT_MAX_BOTS];
        static RunInfo runs[SPLIT_MAX_RUNS];
        memset(botCuts, 0, sizeof botCuts);

        int n = 0;
        char **entries = SDL_GlobDirectory(inputPath, "*", 0, &n);
        if (!entries) {
            fprintf(stderr, "brainrec_split: cannot list '%s'\n", inputPath);
            killAnchorsFree(&anchors);
            free(ix.v); free(copyBuf); free(textBuf);
            return 1;
        }
        printf("\nCompanion logs...\n");
        if (!haveAnchors) {
            printf("  (no killbot.log — brain-log run starts estimated from run lengths)\n");
        }

        /* print2 first: it carries the kill banners, so it's what dates each
         * run. player<N>.jsonl counts the same state.tick and restarts with
         * it, so it reuses its bot's cut points. */
        for (int order = 0; order < 2 && ok; order++) {
            for (int e = 0; e < n && ok; e++) {
                if (strcmp(entries[e], "brainrec.btr") == 0) continue;
                TickKind kind = classifyCompanion(entries[e]);
                bool isPrint2 = (kind == TICK_PRINT2);
                if (order == 0 && !isPrint2) continue;
                if (order == 1 && isPrint2)  continue;

                char src[SPLIT_PATH_MAX];
                SDL_snprintf(src, sizeof src, "%s/%s", inputPath, entries[e]);
                if (fileSize(src) < 0) continue;   /* subdirectory — skip */

                char dsts[SPLIT_MAX_PARTS][SPLIT_PATH_MAX];
                for (int p = 0; p < parts; p++)
                    SDL_snprintf(dsts[p], sizeof dsts[p], "%s/%s", partDir[p], entries[e]);

                if (kind == TICK_NONE) {
                    for (int p = 0; p < parts && ok; p++) {
                        if (!copyFileWhole(src, dsts[p], textBuf, SPLIT_TEXT_BYTES)) {
                            fprintf(stderr, "brainrec_split: copy failed for %s\n", entries[e]);
                            ok = false;
                        }
                    }
                    printf("  %-24s copied whole (no tick stamps)\n", entries[e]);
                    fflush(stdout);
                    continue;
                }

                const BotCuts *cuts = NULL;
                BotCuts shifted;
                if (!tickKindIsEngine(kind)) {
                    int bot = isPrint2 ? atoi(entries[e] + strlen("print2_bot"))
                                       : atoi(entries[e] + strlen("player"));
                    if (bot < 0 || bot >= SPLIT_MAX_BOTS) bot = 0;
                    if (isPrint2 || !botCuts[bot].valid) {
                        int nRuns = scanBrainRuns(src, kind, anchors.v[bot], anchors.n[bot],
                                                  firstEngineTick, runs, textBuf);
                        if (nRuns < 0) {
                            fprintf(stderr, "brainrec_split: cannot scan %s\n", entries[e]);
                            ok = false;
                            break;
                        }
                        computeBrainCuts(runs, nRuns, parts, startEngine, &botCuts[bot]);
                        cuts = &botCuts[bot];
                    } else {
                        /* logger.lua opens player<N>.jsonl with "w", so every
                         * brain restart truncates it and the file holds only
                         * the brain's most recent runs. Line its runs up with
                         * the TAIL of the print2 log's runs, which kept all of
                         * them, and shift the cut points to match. */
                        int nJ = scanBrainRuns(src, kind, NULL, 0, firstEngineTick, runs, textBuf);
                        if (nJ < 0) {
                            fprintf(stderr, "brainrec_split: cannot scan %s\n", entries[e]);
                            ok = false;
                            break;
                        }
                        int drop = botCuts[bot].runs - nJ;
                        if (drop < 0) drop = 0;
                        shifted = botCuts[bot];
                        for (int p = 1; p < parts; p++) {
                            shifted.cut[p].run -= drop;
                            if (shifted.cut[p].run < 0) { shifted.cut[p].run = 0; shifted.cut[p].tick = 0; }
                        }
                        cuts = &shifted;
                    }
                }

                SliceReport rep;
                if (!sliceTextFile(src, kind, parts, startEngine, cuts, dsts, textBuf, &rep)) {
                    fprintf(stderr, "brainrec_split: slice failed for %s\n", entries[e]);
                    ok = false;
                    break;
                }
                printf("  %-24s %s ticks, %lld lines:", entries[e],
                       tickKindIsEngine(kind) ? "engine" : "brain", rep.lines);
                long long sum = 0;
                for (int p = 0; p < parts; p++) {
                    sum += rep.partLines[p];
                    printf("  [p%d %lld lines, %.1f MB, ticks %lld..%lld run %d..%d]",
                           p + 1, rep.partLines[p], fileSize(dsts[p]) / 1.0e6,
                           rep.firstTick[p], rep.lastTick[p], rep.firstRun[p], rep.lastRun[p]);
                }
                printf("  sum=%lld %s\n", sum, sum == rep.lines ? "OK" : "MISMATCH");
                if (sum != rep.lines) ok = false;
                fflush(stdout);
            }
        }
        SDL_free(entries);
        killAnchorsFree(&anchors);
    }

    /* ---- Pass 3: verify ---- */
    printf("\nVerifying parts...\n");
    size_t verifiedTotal = 0;
    for (int p = 0; p < parts; p++) {
        size_t frames = 0;
        uint32_t first = 0, last = 0;
        bool firstKf = false;
        if (!verifyPart(partBtr[p], mapName, legendLen, &frames, &first, &last,
                        &firstKf, copyBuf)) {
            fprintf(stderr, "  part %d/%d: FAILED to re-walk\n", p + 1, parts);
            ok = false;
            continue;
        }
        size_t want = bound[p + 1] - bound[p];
        bool match = frames == want
                     && first == ix.v[bound[p]].tick
                     && last  == ix.v[bound[p + 1] - 1].tick
                     && firstKf;
        printf("  part %d/%d: %llu frames, ticks %u..%u, first frame keyframe=%s  [%s]\n",
               p + 1, parts, (unsigned long long)frames, (unsigned)first, (unsigned)last,
               firstKf ? "yes" : "no", match ? "OK" : "MISMATCH");
        if (!match) ok = false;
        verifiedTotal += frames;
    }
    printf("  total %llu frames across %d parts vs %llu in the source  [%s]\n",
           (unsigned long long)verifiedTotal, parts, (unsigned long long)total,
           verifiedTotal == total ? "OK" : "MISMATCH");
    if (verifiedTotal != total) ok = false;

    double peak = peakResidentMB();
    if (peak >= 0.0) printf("\nPeak working set: %.1f MB\n", peak);

    free(ix.v);
    free(copyBuf);
    free(textBuf);
    return ok ? 0 : 1;
}
