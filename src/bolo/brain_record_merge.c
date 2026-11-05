/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_merge.c
 *PURPOSE:       Merge a Bot Host's recordings into the server's session
 *               folder (see brain_record_host.h).
 *********************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <zlib.h>   /* print2 logs: plain or .gz */

#include "brain_record.h"
#include "brain_record_host.h"
#include "brain_record_walk.h"
#include "input_packet.h"

/* ---- Small helpers ------------------------------------------------------ */

static void setErr(char *err, size_t errSz, const char *what, const char *path) {
    if (err != NULL && errSz > 0) {
        snprintf(err, errSz, "%s%s%s", what, path ? ": " : "", path ? path : "");
    }
}

static bool pathExists(const char *p) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi);
}

static bool isFile(const char *p) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_FILE;
}

static bool copyFile(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out;
    char buf[65536];
    size_t n;
    bool ok = true;
    if (in == NULL) return false;
    out = fopen(to, "wb");
    if (out == NULL) { fclose(in); return false; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) (void)SDL_RemovePath(to);
    return ok;
}

/* Move a file; a copy and remove when a rename cannot do it (another
 * volume). An existing target is replaced. */
static bool moveFile(const char *from, const char *to) {
    if (pathExists(to)) (void)SDL_RemovePath(to);
    if (SDL_RenamePath(from, to)) return true;
    if (!copyFile(from, to)) return false;
    (void)SDL_RemovePath(from);
    return true;
}

/* Replace dst with src: dst goes to dst.bak first, so a failure keeps it. */
static bool replaceFile(const char *src, const char *dst) {
    char bak[1100];
    snprintf(bak, sizeof bak, "%s.bak", dst);
    if (pathExists(bak)) (void)SDL_RemovePath(bak);
    if (pathExists(dst) && !SDL_RenamePath(dst, bak)) return false;
    if (!SDL_RenamePath(src, dst)) {
        if (pathExists(bak)) (void)SDL_RenamePath(bak, dst);
        return false;
    }
    if (pathExists(bak)) (void)SDL_RemovePath(bak);
    return true;
}

/* ---- Legend --------------------------------------------------------------- */

static char *dupStr(const char *s) {
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static bool jsonObjectEmpty(const char *s) {
    if (s == NULL || *s != '{') return true;
    s++;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return *s == '}';
}

char *brainRecLegendAddSlot(const char *legend, int slot, const char *slotLegend) {
    const char *base = (legend != NULL && legend[0] == '{') ? legend : "{}";
    char key[24];
    const char *slots, *last;
    char *out;
    size_t cap;

    if (slotLegend == NULL || slotLegend[0] != '{') return dupStr(base);
    snprintf(key, sizeof key, "\"%d\":{", slot);
    cap = strlen(base) + strlen(slotLegend) * 2 + 64;
    out = (char *)malloc(cap);
    if (out == NULL) return NULL;

    slots = strstr(base, "\"slots\":{");
    if (slots != NULL) {
        const char *body = slots + 9;
        size_t head = (size_t)(body - base);
        if (strstr(body, key) != NULL) {
            free(out);
            return dupStr(base);
        }
        memcpy(out, base, head);
        snprintf(out + head, cap - head, "\"%d\":%s%s%s", slot, slotLegend,
                 *body == '}' ? "" : ",", body);
        return out;
    }

    /* No slots object yet. An empty global legend takes the slot's. */
    if (jsonObjectEmpty(base)) {
        size_t sl;
        last = strrchr(slotLegend, '}');
        sl = (size_t)(last - slotLegend);
        memcpy(out, slotLegend, sl);
        snprintf(out + sl, cap - sl, "%s\"slots\":{\"%d\":%s}}",
                 jsonObjectEmpty(slotLegend) ? "" : ",", slot, slotLegend);
        return out;
    }
    last = strrchr(base, '}');
    {
        size_t bl = (size_t)(last - base);
        memcpy(out, base, bl);
        snprintf(out + bl, cap - bl, ",\"slots\":{\"%d\":%s}}", slot, slotLegend);
    }
    return out;
}

/* ---- Frame copy ----------------------------------------------------------- */

typedef struct {
    BrainRecIn   *file;
    BrainRecBuf  *frame;  /* every byte read or skipped is kept here */
} CaptureCtx;

static size_t capRead(void *ctx, void *dst, size_t len) {
    CaptureCtx *c = (CaptureCtx *)ctx;
    uint8_t *d = (uint8_t *)dst;
    size_t total = 0;
    /* Read until len bytes or the end: a container may return less. */
    while (total < len) {
        size_t got = brainRecInRead(c->file, d + total, len - total);
        if (got == 0) break;
        total += got;
    }
    if (total > 0) brainRecBufPut(c->frame, dst, total);
    return total;
}

static bool capSkip(void *ctx, size_t len) {
    CaptureCtx *c = (CaptureCtx *)ctx;
    BrainRecBuf *b = c->frame;
    if (len == 0) return true;
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        uint8_t *d;
        while (cap < b->len + len) cap *= 2;
        d = (uint8_t *)realloc(b->data, cap);
        if (d == NULL) return false;
        b->data = d;
        b->cap = cap;
    }
    if (!hostRecReadExact(c->file, b->data + b->len, len)) return false;
    b->len += len;
    return true;
}

typedef struct {
    size_t   botsStart;            /* offset of the first bot's slot byte */
    int      bots;
    size_t   bodyOff[MAX_TANKS * 2];
    size_t   bodyLen[MAX_TANKS * 2];
    uint8_t  slot[MAX_TANKS * 2];
    size_t   tailStart;
} FrameLayout;

/* Find the per-bot entries of one whole frame already in memory. */
static bool frameLocate(const uint8_t *f, size_t len, FrameLayout *out) {
    size_t pos = 8;
    uint8_t n, kf;
    int b;
    memset(out, 0, sizeof(*out));
#define NEED(x) do { if ((size_t)(x) > len - pos) return false; } while (0)
    NEED(1); n = f[pos++]; NEED((size_t)n * sizeof(TankSnapshot)); pos += (size_t)n * sizeof(TankSnapshot);
    NEED(1); n = f[pos++]; NEED((size_t)n * sizeof(ShellSnapshot)); pos += (size_t)n * sizeof(ShellSnapshot);
    NEED(1); n = f[pos++]; NEED((size_t)n * sizeof(BaseSnapshot)); pos += (size_t)n * sizeof(BaseSnapshot);
    NEED(1); n = f[pos++]; NEED((size_t)n * sizeof(PillSnapshot)); pos += (size_t)n * sizeof(PillSnapshot);
    NEED(1); kf = f[pos++];
    if (kf) {
        NEED((size_t)MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
        pos += (size_t)MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
    } else {
        uint32_t nd;
        NEED(4); memcpy(&nd, f + pos, 4); pos += 4;
        NEED((size_t)nd * 5); pos += (size_t)nd * 5;
    }
    NEED(1); n = f[pos++];
    if (n > MAX_TANKS * 2) return false;
    out->botsStart = pos;
    out->bots = n;
    for (b = 0; b < n; b++) {
        size_t bl;
        NEED(1);
        out->slot[b] = f[pos++];
        bl = brainRecBodyMeasure(f + pos, len - pos);
        if (bl == 0) return false;
        out->bodyOff[b] = pos;
        out->bodyLen[b] = bl;
        pos += bl;
    }
    out->tailStart = pos;
#undef NEED
    return true;
}

/* One host file being read alongside the .btr. */
typedef struct {
    HostRecReader rd;
    bool        open;
    bool        eof;
    BrainRecBuf cur, peek;
    uint32_t    curSeq, peekSeq;
    bool        haveCur, havePeek;
    bool        curUsed;
    bool        peekUsed; /* the first record, used one think early */
} HostStream;

static void hsReadPeek(HostStream *h, BrainRecMergeStats *st) {
    int r;
    h->havePeek = false;
    h->peekUsed = false;
    if (h->eof) return;
    r = hostRecReaderNext(&h->rd, &h->peekSeq, NULL, &h->peek);
    if (r == 1) {
        h->havePeek = true;
        st->hostRecords++;
    } else {
        h->eof = true;
    }
}

/* Move cur forward to the latest record with seq <= target. */
static void hsAdvance(HostStream *h, uint32_t target, BrainRecMergeStats *st) {
    while (h->havePeek && h->peekSeq <= target) {
        BrainRecBuf t = h->cur;
        h->cur = h->peek;
        h->peek = t;
        h->curSeq = h->peekSeq;
        h->haveCur = true;
        h->curUsed = h->peekUsed;
        hsReadPeek(h, st);
    }
}

static bool writeAll(BrainRecOut *f, const void *p, size_t n) {
    return n == 0 || brainRecOutWrite(f, p, n);
}

/* Count the frames of a .btr with the walker; -1 when its preamble is bad. */
static long countFrames(const char *path) {
    BrainRecIn *f = brainRecInOpen(path);
    BrainRecBuf scratch;
    CaptureCtx c;
    BrainRecReader r;
    BrainRecFrameInfo fi;
    long n = 0;
    if (f == NULL) return -1;
    brainRecBufInit(&scratch);
    c.file = f;
    c.frame = &scratch;
    memset(&r, 0, sizeof r);
    r.ctx = &c;
    r.read = capRead;
    r.skip = capSkip;
    if (brainRecWalkPreamble(&r, NULL, NULL) != BRAINREC_WALK_OK) {
        brainRecInClose(f);
        brainRecBufFree(&scratch);
        return -1;
    }
    for (;;) {
        BrainRecWalkStatus s;
        brainRecBufReset(&scratch);
        s = brainRecWalkFrame(&r, &fi);
        if (s != BRAINREC_WALK_OK) {
            if (s == BRAINREC_WALK_BAD) n = -1 - n;   /* damaged after n frames */
            break;
        }
        n++;
    }
    brainRecInClose(f);
    brainRecBufFree(&scratch);
    return n;
}

bool brainRecMergeFiles(const char *btrPath, const char *const *hostFiles,
                        int hostCount, const char *outPath,
                        BrainRecMergeStats *st, char *err, size_t errSz) {
    BrainRecMergeStats local;
    HostStream *hs = NULL;
    int byKey[MAX_TANKS][8];    /* small (slot, gen) lookup: index or -1 */
    BrainRecIn  *in = NULL;
    BrainRecOut *out = NULL;
    BrainRecHeader hdr;
    uint32_t llen = 0;
    char *legend = NULL;
    BrainRecBuf frame, body;
    CaptureCtx cap;
    BrainRecReader rd;
    bool ok = false;
    int i;

    if (st == NULL) st = &local;
    memset(st, 0, sizeof(*st));
    brainRecBufInit(&frame);
    brainRecBufInit(&body);
    memset(byKey, 0xff, sizeof byKey);

    in = brainRecInOpen(btrPath);
    if (in == NULL) { setErr(err, errSz, "cannot open", btrPath); goto done; }
    if (!hostRecReadExact(in, &hdr, sizeof hdr) ||
        !brainRecMagicMatches(&hdr) || !brainRecVersionReadable(&hdr) ||
        !hostRecReadExact(in, &llen, 4) || llen > (16u << 20)) {
        setErr(err, errSz, "not a readable brainrec.btr", btrPath);
        goto done;
    }
    legend = (char *)malloc((size_t)llen + 1);
    if (legend == NULL || (llen && !hostRecReadExact(in, legend, llen))) {
        setErr(err, errSz, "legend cut short", btrPath);
        goto done;
    }
    legend[llen] = '\0';

    hs = (HostStream *)calloc((size_t)(hostCount > 0 ? hostCount : 1), sizeof(HostStream));
    if (hs == NULL) { setErr(err, errSz, "out of memory", NULL); goto done; }
    for (i = 0; i < hostCount; i++) {
        HostStream *h = &hs[i];
        int k;
        if (!hostRecReaderOpen(&h->rd, hostFiles[i])) {
            fprintf(stderr, "brain_record_merge: skipped '%s' (not a readable "
                            "host recording)\n", hostFiles[i]);
            continue;
        }
        if (h->rd.hdr.slot >= MAX_TANKS) { hostRecReaderClose(&h->rd); continue; }
        for (k = 0; k < 8; k++) {
            if (byKey[h->rd.hdr.slot][k] < 0) {
                byKey[h->rd.hdr.slot][k] = i;
                break;
            }
        }
        if (k == 8) { hostRecReaderClose(&h->rd); continue; }
        h->open = true;
        st->hostFiles++;
        brainRecBufInit(&h->cur);
        brainRecBufInit(&h->peek);
        hsReadPeek(h, st);
        if (h->rd.legend != NULL && h->rd.legend[0] == '{') {
            char *nl = brainRecLegendAddSlot(legend, h->rd.hdr.slot, h->rd.legend);
            if (nl != NULL) { free(legend); legend = nl; }
        }
    }

    out = brainRecOutOpen(outPath);
    if (out == NULL) { setErr(err, errSz, "cannot write", outPath); goto done; }
    llen = (uint32_t)strlen(legend);
    if (!writeAll(out, &hdr, sizeof hdr) || !writeAll(out, &llen, 4) ||
        !writeAll(out, legend, llen)) {
        setErr(err, errSz, "write failed", outPath);
        goto done;
    }

    cap.file = in;
    cap.frame = &frame;
    memset(&rd, 0, sizeof rd);
    rd.ctx = &cap;
    rd.read = capRead;
    rd.skip = capSkip;
    rd.version = hdr.version;

    for (;;) {
        BrainRecFrameInfo fi;
        FrameLayout lay;
        BrainRecWalkStatus ws;
        int b;

        brainRecBufReset(&frame);
        ws = brainRecWalkFrame(&rd, &fi);
        if (ws == BRAINREC_WALK_EOF) break;
        if (ws == BRAINREC_WALK_BAD) {
            /* The server was stopped mid-frame: keep the whole frames. */
            if (frame.len > 0) {
                fprintf(stderr, "brain_record_merge: '%s' ends inside a "
                                "frame; %u whole frames kept\n", btrPath,
                        (unsigned)st->frames);
            }
            break;
        }
        if (frame.failed || !frameLocate(frame.data, frame.len, &lay)) {
            setErr(err, errSz, "frame layout not understood", btrPath);
            goto done;
        }
        st->frames++;
        /* A new zstd block at each map keyframe, as the recorder and the
         * splitter do, so the merged file has the same seek points. */
        if (fi.mapKeyframe && !brainRecOutNewBlock(out)) goto writeFail;
        if (!writeAll(out, frame.data, lay.botsStart)) goto writeFail;
        for (b = 0; b < lay.bots; b++) {
            const uint8_t *bp = frame.data + lay.bodyOff[b];
            size_t bl = lay.bodyLen[b];
            uint16_t gen;
            uint32_t lpi;
            uint8_t slot = lay.slot[b];
            if (!writeAll(out, &slot, 1)) goto writeFail;
            if (brainRecBodyIsHosted(bp, bl, &gen, &lpi, NULL)) {
                HostStream *h = NULL;
                int k;
                st->markers++;
                if (slot < MAX_TANKS) {
                    for (k = 0; k < 8 && byKey[slot][k] >= 0; k++) {
                        HostStream *c = &hs[byKey[slot][k]];
                        if (c->open && c->rd.hdr.placeGen == gen) { h = c; break; }
                    }
                }
                if (h != NULL) {
                    uint32_t target = lpi + 1;
                    const BrainRecBuf *use = NULL;
                    bool reused = false;
                    hsAdvance(h, target, st);
                    if (h->haveCur && h->curSeq >= lpi) {
                        use = &h->cur; reused = h->curUsed; h->curUsed = true;
                        st->filled++;
                    } else if (h->haveCur && lpi - h->curSeq <= BRAINREC_MERGE_STALE_MAX) {
                        use = &h->cur; reused = h->curUsed; h->curUsed = true;
                        st->stale++;
                    } else if (!h->haveCur && h->havePeek && h->peekSeq <= target + 2) {
                        /* The first record is one think ahead. */
                        use = &h->peek; reused = h->peekUsed;
                        h->peekUsed = true;
                        st->filled++;
                    }
                    if (use != NULL) {
                        if (reused) {
                            /* One pool sample per think, as in-process. */
                            brainRecBufReset(&body);
                            brainRecBufPut(&body, use->data, use->len);
                            if (body.failed) goto writeFail;
                            body.len = brainRecBodyDropPool(body.data, body.len);
                            bp = body.data; bl = body.len;
                        } else {
                            bp = use->data; bl = use->len;
                        }
                    } else {
                        st->missing++;
                    }
                } else {
                    st->missing++;
                }
            }
            if (!writeAll(out, bp, bl)) goto writeFail;
        }
        if (!writeAll(out, frame.data + lay.tailStart, frame.len - lay.tailStart)) {
            goto writeFail;
        }
    }

    if (!brainRecOutClose(out)) { out = NULL; goto writeFail; }
    out = NULL;
    {
        long n = countFrames(outPath);
        if (n != (long)st->frames) {
            setErr(err, errSz, "merged file failed the frame check", outPath);
            goto done;
        }
    }
    ok = true;
    goto done;

writeFail:
    setErr(err, errSz, "write failed", outPath);
done:
    (void)brainRecOutClose(out);
    brainRecInClose(in);
    if (!ok && outPath != NULL) (void)SDL_RemovePath(outPath);
    if (hs != NULL) {
        for (i = 0; i < hostCount; i++) {
            if (!hs[i].open) continue;
            hostRecReaderClose(&hs[i].rd);
            brainRecBufFree(&hs[i].cur);
            brainRecBufFree(&hs[i].peek);
        }
        free(hs);
    }
    free(legend);
    brainRecBufFree(&frame);
    brainRecBufFree(&body);
    return ok;
}

/* ---- print2 logs ---------------------------------------------------------- */

typedef struct {
    gzFile f;            /* gzopen reads a plain file as it is */
    char  *line;
    size_t cap;
    size_t len;
    bool   have;
} LineIn;

static bool lineNext(LineIn *in) {
    in->len = 0;
    in->have = false;
    for (;;) {
        if (in->cap - in->len < 4096) {
            size_t cap = in->cap ? in->cap * 2 : 8192;
            char *d = (char *)realloc(in->line, cap);
            if (d == NULL) return false;
            in->line = d;
            in->cap = cap;
        }
        if (gzgets(in->f, in->line + in->len, (int)(in->cap - in->len)) == NULL) {
            in->have = in->len > 0;
            return in->have;
        }
        in->len += strlen(in->line + in->len);
        if (in->len > 0 && in->line[in->len - 1] == '\n') {
            in->have = true;
            return true;
        }
    }
}

static bool isTickLine(const LineIn *in, long long *tick) {
    if (!in->have || in->len < 11 || strncmp(in->line, "===TICK ", 8) != 0) {
        return false;
    }
    *tick = strtoll(in->line + 8, NULL, 10);
    return true;
}

typedef struct {
    FILE  *fp;
    gzFile gz;
} LineOut;

static bool lineWrite(LineOut *out, const char *p, size_t n) {
    if (out->gz != NULL) return n == 0 || gzwrite(out->gz, p, (unsigned)n) == (int)n;
    return fwrite(p, 1, n, out->fp) == n;
}

/* Copy lines from in to out until the next tick line (left in in). */
static bool copyUntilTick(LineIn *in, LineOut *out, bool includeCurrent) {
    long long t;
    if (includeCurrent && in->have) {
        if (!lineWrite(out, in->line, in->len)) return false;
        lineNext(in);
    }
    while (in->have && !isTickLine(in, &t)) {
        if (!lineWrite(out, in->line, in->len)) return false;
        lineNext(in);
    }
    return true;
}

bool brainRecMergePrint2(const char *a, const char *b, const char *outPath,
                         bool gzOut) {
    LineIn ia, ib;
    LineOut out;
    bool ok = true;
    memset(&ia, 0, sizeof ia);
    memset(&ib, 0, sizeof ib);
    memset(&out, 0, sizeof out);
    ia.f = gzopen(a, "rb");
    ib.f = gzopen(b, "rb");
    if (gzOut) out.gz = gzopen(outPath, "wb6");
    else out.fp = fopen(outPath, "wb");
    if (ia.f == NULL || ib.f == NULL || (out.gz == NULL && out.fp == NULL)) {
        ok = false;
        goto done;
    }
    lineNext(&ia);
    lineNext(&ib);
    /* Lines before the first block: a's, then b's. */
    ok = copyUntilTick(&ia, &out, false) && copyUntilTick(&ib, &out, false);
    while (ok && (ia.have || ib.have)) {
        long long ta = 0, tb = 0;
        bool ha = isTickLine(&ia, &ta), hb = isTickLine(&ib, &tb);
        if (ha && (!hb || ta <= tb)) ok = copyUntilTick(&ia, &out, true);
        else if (hb) ok = copyUntilTick(&ib, &out, true);
        else break;
    }
done:
    if (ia.f) gzclose(ia.f);
    if (ib.f) gzclose(ib.f);
    if (out.gz && gzclose(out.gz) != Z_OK) ok = false;
    if (out.fp && fclose(out.fp) != 0) ok = false;
    free(ia.line);
    free(ib.line);
    if (!ok) (void)SDL_RemovePath(outPath);
    return ok;
}

/* ---- Folder merge --------------------------------------------------------- */

static void logLine(BrainRecMergeLogFn log, void *ctx, const char *fmt, ...) {
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (log != NULL) log(ctx, line);
    else fprintf(stderr, "%s\n", line);
}

static bool endsWith(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

bool brainRecMergeDir(const char *sessionDir, const char *hostDir,
                      const char *hostName, BrainRecMergeStats *st,
                      BrainRecMergeLogFn log, void *logCtx) {
    char **entries;
    int n = 0, e, nh = 0;
    char **hbr = NULL;
    bool allOk = true;
    char btr[1100];
    BrainRecMergeStats local;
    const char *hn = (hostName != NULL && hostName[0]) ? hostName : "host";

    if (st == NULL) st = &local;
    memset(st, 0, sizeof(*st));
    entries = SDL_GlobDirectory(hostDir, "*", 0, &n);
    if (entries == NULL) {
        logLine(log, logCtx, "brain_record_merge: cannot list %s", hostDir);
        return false;
    }
    hbr = (char **)calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    if (hbr == NULL) { SDL_free(entries); return false; }

    for (e = 0; e < n; e++) {
        const char *name = entries[e];
        char src[1100], dst[1100];
        if (name[0] == '.' || strchr(name, '/') || strchr(name, '\\')) continue;
        snprintf(src, sizeof src, "%s/%s", hostDir, name);
        if (!isFile(src)) continue;
        if (endsWith(name, HOSTREC_EXT)) {
            size_t l = strlen(src) + 1;
            hbr[nh] = (char *)malloc(l);
            if (hbr[nh]) { memcpy(hbr[nh], src, l); nh++; }
            continue;
        }
        {
            /* A log can come as <name> or <name>.gz (the host gzips its
             * finished logs, the server gzips its own when the next block
             * opens), so a name clash is checked in both forms. */
            char plain[300];
            bool gzName = endsWith(name, ".gz");
            SDL_strlcpy(plain, name, sizeof plain);
            if (gzName && strlen(plain) > 3) plain[strlen(plain) - 3] = '\0';
            snprintf(dst, sizeof dst, "%s/%s", sessionDir, plain);
            if (!pathExists(dst)) {
                snprintf(dst, sizeof dst, "%s/%s.gz", sessionDir, plain);
            }
            if (!pathExists(dst)) {
                snprintf(dst, sizeof dst, "%s/%s", sessionDir, name);
                if (!moveFile(src, dst)) {
                    logLine(log, logCtx, "brain_record_merge: cannot move %s", src);
                    allOk = false;
                }
                continue;
            }
            if (strncmp(plain, "print2_bot", 10) == 0 && endsWith(plain, ".log")) {
                /* The merged log keeps the form of the server's file. */
                char tmp[1200];
                snprintf(tmp, sizeof tmp, "%s.merge.tmp", dst);
                if (brainRecMergePrint2(dst, src, tmp, endsWith(dst, ".gz")) &&
                    replaceFile(tmp, dst)) {
                    (void)SDL_RemovePath(src);
                    logLine(log, logCtx, "brain_record_merge: %s merged by tick", dst);
                    continue;
                }
                (void)SDL_RemovePath(tmp);
                logLine(log, logCtx, "brain_record_merge: %s could not be merged "
                        "by tick; kept beside it", dst);
            }
        }
        snprintf(dst, sizeof dst, "%s/bothost_%s_%s", sessionDir, hn, name);
        if (!moveFile(src, dst)) {
            logLine(log, logCtx, "brain_record_merge: cannot move %s", src);
            allOk = false;
        } else {
            logLine(log, logCtx, "brain_record_merge: %s/%s exists; the host's "
                    "copy is %s", sessionDir, name, dst);
        }
    }
    SDL_free(entries);

    snprintf(btr, sizeof btr, "%s/%s", sessionDir, BRAINREC_FILENAME);
    if (nh > 0) {
        bool merged = false;
        if (isFile(btr)) {
            char tmp[1200], err[512];
            snprintf(tmp, sizeof tmp, "%s.merge.tmp", btr);
            err[0] = '\0';
            if (brainRecMergeFiles(btr, (const char *const *)hbr, nh, tmp, st,
                                   err, sizeof err) &&
                replaceFile(tmp, btr)) {
                merged = true;
                logLine(log, logCtx, "brain_record_merge: %s: %u host files, "
                        "%u frames, %u hosted entries (%u filled, %u a few "
                        "thinks old, %u with no host record)", btr,
                        (unsigned)st->hostFiles, (unsigned)st->frames,
                        (unsigned)st->markers, (unsigned)st->filled,
                        (unsigned)st->stale, (unsigned)st->missing);
            } else {
                logLine(log, logCtx, "brain_record_merge: %s not merged (%s); "
                        "the host files are kept beside it", btr,
                        err[0] ? err : "replace failed");
                allOk = false;
            }
        } else {
            logLine(log, logCtx, "brain_record_merge: %s has no %s; the host "
                    "files are kept as they are", sessionDir, BRAINREC_FILENAME);
        }
        for (e = 0; e < nh; e++) {
            if (merged) {
                (void)SDL_RemovePath(hbr[e]);
            } else {
                char dst[1200];
                const char *base = strrchr(hbr[e], '/');
                base = base ? base + 1 : hbr[e];
                snprintf(dst, sizeof dst, "%s/bothost_%s_%s", sessionDir, hn, base);
                if (!moveFile(hbr[e], dst)) allOk = false;
            }
        }
    }
    for (e = 0; e < nh; e++) free(hbr[e]);
    free(hbr);
    if (allOk) (void)SDL_RemovePath(hostDir);
    return allOk;
}

/* ---- Folder helpers ------------------------------------------------------- */

void brainRecSafeName(const char *in, char *out, size_t outSz, bool allowDot) {
    size_t i, j = 0;
    if (outSz == 0) return;
    for (i = 0; in != NULL && in[i] != '\0' && j + 1 < outSz; i++) {
        char c = in[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  (allowDot && c == '.');
        /* No leading dot and no two dots in a row: the result always
         * passes brainRecNameOk. */
        if (c == '.' && (j == 0 || out[j - 1] == '.')) ok = false;
        out[j++] = ok ? c : '_';
    }
    out[j] = '\0';
    if (j == 0 && outSz > 1) { out[0] = '_'; out[1] = '\0'; }
}

bool brainRecNameOk(const char *s, bool allowDot) {
    size_t i;
    if (s == NULL || s[0] == '\0' || s[0] == '.') return false;
    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              (allowDot && c == '.'))) {
            return false;
        }
    }
    return strstr(s, "..") == NULL;
}

bool brainRecPathUnder(const char *root, const char *path) {
    size_t rl;
    const char *p;
    if (root == NULL || path == NULL || root[0] == '\0') return false;
    rl = strlen(root);
    while (rl > 0 && (root[rl - 1] == '/' || root[rl - 1] == '\\')) rl--;
    if (rl == 0) return false;
#ifdef _WIN32
    if (SDL_strncasecmp(path, root, rl) != 0) return false;
#else
    if (strncmp(path, root, rl) != 0) return false;
#endif
    if (path[rl] != '/' && path[rl] != '\\') return false;
    p = path + rl + 1;
    if (*p == '\0') return false;
    /* No part may be "." or "..", and no part may be empty. */
    while (*p != '\0') {
        size_t n = strcspn(p, "/\\");
        if (n == 0) return false;
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        p += n;
        if (*p != '\0') p++;
        else break;
    }
    return true;
}

typedef struct {
    const char *dir;
    uint64_t    bytes;
    uint32_t    files;
    bool        ok;
} TreeCtx;

static bool removeTreeIn(const char *dir, uint64_t *bytes, uint32_t *files);

static SDL_EnumerationResult SDLCALL treeEntry(void *userdata,
                                               const char *dirname,
                                               const char *fname) {
    TreeCtx *t = (TreeCtx *)userdata;
    char path[1100];
    SDL_PathInfo pi;
    (void)dirname;
    if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    snprintf(path, sizeof path, "%s/%s", t->dir, fname);
    if (!SDL_GetPathInfo(path, &pi)) { t->ok = false; return SDL_ENUM_CONTINUE; }
    if (pi.type == SDL_PATHTYPE_DIRECTORY) {
        if (!removeTreeIn(path, &t->bytes, &t->files)) t->ok = false;
        return SDL_ENUM_CONTINUE;
    }
    if (SDL_RemovePath(path)) {
        t->bytes += pi.size;
        t->files++;
    } else {
        t->ok = false;
    }
    return SDL_ENUM_CONTINUE;
}

static bool removeTreeIn(const char *dir, uint64_t *bytes, uint32_t *files) {
    TreeCtx t;
    t.dir = dir;
    t.bytes = 0;
    t.files = 0;
    t.ok = true;
    if (!SDL_EnumerateDirectory(dir, treeEntry, &t)) t.ok = false;
    *bytes += t.bytes;
    *files += t.files;
    if (!SDL_RemovePath(dir)) t.ok = false;
    return t.ok;
}

bool brainRecRemoveTree(const char *root, const char *path, uint64_t *bytes,
                        uint32_t *files) {
    uint64_t b = 0;
    uint32_t f = 0;
    SDL_PathInfo pi;
    bool ok;
    if (bytes) *bytes = 0;
    if (files) *files = 0;
    if (!brainRecPathUnder(root, path)) return false;
    if (!SDL_GetPathInfo(path, &pi)) return true;   /* nothing there */
    if (pi.type != SDL_PATHTYPE_DIRECTORY) {
        ok = SDL_RemovePath(path);
        if (ok) { b = pi.size; f = 1; }
    } else {
        ok = removeTreeIn(path, &b, &f);
    }
    if (bytes) *bytes = b;
    if (files) *files = f;
    return ok;
}
