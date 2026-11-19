/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_io.c
 *PURPOSE:       The .btr container: zstd writer, zstd/gzip reader
 *               (see brain_record_io.h).
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>
#include <zstd.h>

#include "brain_record_io.h"

#if defined(_WIN32)
#define brio_fseek _fseeki64
#define brio_ftell _ftelli64
#else
#include <sys/types.h>
#define brio_fseek fseeko
#define brio_ftell ftello
#endif

#define SKIPPABLE_MARKER_MAGIC 0x184D2A50u   /* first of the 16 skippable magics */
#define SEEKTABLE_SKIP_MAGIC   0x184D2A5Eu   /* zstd seekable format */
#define SEEKTABLE_FOOTER_MAGIC 0x8F92EAB1u
#define ZSTD_FRAME_MAGIC       0xFD2FB528u

#define OUT_STAGE_CAP  (1u << 20)   /* decompressed bytes gathered per zstd call */
#define IN_BUF_CAP     (1u << 20)

static void put_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t get_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

BrainRecFormat brainRecDetectFormat(const unsigned char *head, size_t n) {
    if (n >= 2 && head[0] == 0x1F && head[1] == 0x8B) return BRAINREC_FMT_GZIP;
    if (n >= 4) {
        uint32_t m = get_le32(head);
        if (m == ZSTD_FRAME_MAGIC) return BRAINREC_FMT_ZSTD;
        if ((m & 0xFFFFFFF0u) == SKIPPABLE_MARKER_MAGIC) return BRAINREC_FMT_ZSTD;
    }
    /* Anything else goes through zlib, which reads plain bytes as they are. */
    return BRAINREC_FMT_GZIP;
}

/* ===================================================================== */
/* Writer                                                                */
/* ===================================================================== */

struct BrainRecOut {
    FILE          *fp;
    ZSTD_CCtx     *cctx;
    unsigned char *stage;     size_t stageLen;
    unsigned char *obuf;      size_t obufCap;
    uint64_t       blockRaw, blockComp;   /* the zstd frame being written */
    uint32_t      *entComp, *entRaw;      /* finished zstd frames */
    uint32_t       nEnt, capEnt;
    uint64_t       totalIn, totalOut;
    bool           err;
};

static bool out_push_entry(BrainRecOut *o, uint32_t comp, uint32_t raw) {
    if (o->nEnt == o->capEnt) {
        uint32_t nc = o->capEnt ? o->capEnt * 2 : 64;
        uint32_t *a = (uint32_t *)realloc(o->entComp, nc * sizeof *a);
        if (!a) return false;
        o->entComp = a;
        uint32_t *b = (uint32_t *)realloc(o->entRaw, nc * sizeof *b);
        if (!b) return false;
        o->entRaw = b;
        o->capEnt = nc;
    }
    o->entComp[o->nEnt] = comp;
    o->entRaw[o->nEnt]  = raw;
    o->nEnt++;
    return true;
}

static bool out_fwrite(BrainRecOut *o, const void *p, size_t n) {
    if (n && fwrite(p, 1, n, o->fp) != n) { o->err = true; return false; }
    o->totalOut += n;
    return true;
}

/* Give the staged bytes to zstd with the given end directive and write
 * what comes out. */
static bool out_pump(BrainRecOut *o, ZSTD_EndDirective mode) {
    if (o->err) return false;
    ZSTD_inBuffer in = { o->stage, o->stageLen, 0 };
    for (;;) {
        ZSTD_outBuffer out = { o->obuf, o->obufCap, 0 };
        size_t r = ZSTD_compressStream2(o->cctx, &out, &in, mode);
        if (ZSTD_isError(r)) { o->err = true; return false; }
        if (!out_fwrite(o, o->obuf, out.pos)) return false;
        o->blockComp += out.pos;
        bool done = (mode == ZSTD_e_continue) ? (in.pos == in.size) : (r == 0);
        if (done) break;
    }
    o->blockRaw += o->stageLen;
    o->stageLen = 0;
    return true;
}

BrainRecOut *brainRecOutOpen(const char *path) {
    BrainRecOut *o = (BrainRecOut *)calloc(1, sizeof *o);
    if (!o) return NULL;
    o->fp      = fopen(path, "wb");
    o->cctx    = ZSTD_createCCtx();
    o->stage   = (unsigned char *)malloc(OUT_STAGE_CAP);
    o->obufCap = ZSTD_CStreamOutSize();
    o->obuf    = (unsigned char *)malloc(o->obufCap);
    if (!o->fp || !o->cctx || !o->stage || !o->obuf) goto fail;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_compressionLevel, BRAINREC_ZSTD_LEVEL)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_windowLog, BRAINREC_ZSTD_WINDOW_LOG)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_checksumFlag, 1)))
        goto fail;

    /* The format marker: a skippable frame holding BRAINREC_ZSTD_MARKER. */
    unsigned char mk[8 + BRAINREC_ZSTD_MARKER_LEN];
    put_le32(mk, SKIPPABLE_MARKER_MAGIC);
    put_le32(mk + 4, BRAINREC_ZSTD_MARKER_LEN);
    memcpy(mk + 8, BRAINREC_ZSTD_MARKER, BRAINREC_ZSTD_MARKER_LEN);
    if (!out_fwrite(o, mk, sizeof mk)) goto fail;
    if (!out_push_entry(o, (uint32_t)sizeof mk, 0)) goto fail;
    return o;

fail:
    if (o->fp) fclose(o->fp);
    ZSTD_freeCCtx(o->cctx);
    free(o->stage);
    free(o->obuf);
    free(o);
    return NULL;
}

bool brainRecOutWrite(BrainRecOut *o, const void *p, size_t n) {
    if (!o || o->err) return false;
    const unsigned char *src = (const unsigned char *)p;
    while (n) {
        /* Keep each block's decompressed size inside its 32-bit field. */
        if (o->blockRaw + o->stageLen >= BRAINREC_ZSTD_BLOCK_MAX) {
            if (!brainRecOutNewBlock(o)) return false;
        }
        size_t room = OUT_STAGE_CAP - o->stageLen;
        size_t k = n < room ? n : room;
        memcpy(o->stage + o->stageLen, src, k);
        o->stageLen += k; o->totalIn += k; src += k; n -= k;
        if (o->stageLen == OUT_STAGE_CAP && !out_pump(o, ZSTD_e_continue)) return false;
    }
    return true;
}

bool brainRecOutNewBlock(BrainRecOut *o) {
    if (!o || o->err) return false;
    if (o->blockRaw == 0 && o->stageLen == 0) return true;   /* empty block */
    if (!out_pump(o, ZSTD_e_end)) return false;
    if (!out_push_entry(o, (uint32_t)o->blockComp, (uint32_t)o->blockRaw)) {
        o->err = true; return false;
    }
    o->blockRaw = o->blockComp = 0;
    return true;
}

bool brainRecOutFlush(BrainRecOut *o) {
    if (!o || o->err) return false;
    if (o->stageLen || o->blockRaw) {
        if (!out_pump(o, ZSTD_e_flush)) return false;
    }
    if (fflush(o->fp) != 0) { o->err = true; return false; }
    return true;
}

bool brainRecOutClose(BrainRecOut *o) {
    if (!o) return true;
    bool ok = brainRecOutNewBlock(o);
    if (ok) {
        /* Seek table, zstd seekable format, no per-entry checksums. */
        size_t tabLen = (size_t)o->nEnt * 8u + 9u;
        unsigned char *t = (unsigned char *)malloc(8 + tabLen);
        if (t) {
            put_le32(t, SEEKTABLE_SKIP_MAGIC);
            put_le32(t + 4, (uint32_t)tabLen);
            unsigned char *q = t + 8;
            for (uint32_t i = 0; i < o->nEnt; i++) {
                put_le32(q, o->entComp[i]); put_le32(q + 4, o->entRaw[i]); q += 8;
            }
            put_le32(q, o->nEnt); q[4] = 0; put_le32(q + 5, SEEKTABLE_FOOTER_MAGIC);
            ok = out_fwrite(o, t, 8 + tabLen);
            free(t);
        } else {
            ok = false;
        }
    }
    if (fclose(o->fp) != 0) ok = false;
    if (o->err) ok = false;
    ZSTD_freeCCtx(o->cctx);
    free(o->stage);
    free(o->obuf);
    free(o->entComp);
    free(o->entRaw);
    free(o);
    return ok;
}

uint64_t brainRecOutBytesIn(const BrainRecOut *o)  { return o ? o->totalIn : 0; }
uint64_t brainRecOutBytesOut(const BrainRecOut *o) { return o ? o->totalOut : 0; }

/* ===================================================================== */
/* Reader                                                                */
/* ===================================================================== */

struct BrainRecIn {
    BrainRecFormat fmt;
    uint64_t       fileSize;
    uint64_t       pos;          /* decompressed bytes handed to the caller */
    bool           err, truncated, eof;
    /* gzip */
    gzFile         gz;
    /* zstd */
    FILE          *fp;
    ZSTD_DCtx     *dctx;
    unsigned char *ibuf;  size_t iLen, iPos;
    unsigned char *obuf;  size_t oCap, oLen, oPos;
    uint64_t       compPos;      /* file offset of the next byte to read into ibuf */
    size_t         lastRet;      /* last ZSTD_decompressStream result (0 = frame end) */
    /* seek table: start offsets of each zstd frame, nSeek + 1 entries */
    uint32_t       nSeek;
    uint64_t      *seekComp, *seekRaw;
};

static uint64_t file_size_of(FILE *fp) {
    if (brio_fseek(fp, 0, SEEK_END) != 0) return 0;
    long long s = (long long)brio_ftell(fp);
    brio_fseek(fp, 0, SEEK_SET);
    return s > 0 ? (uint64_t)s : 0;
}

/* Read the seek table at the end of the file, if there is a valid one. */
static void in_load_seek_table(BrainRecIn *in) {
    if (in->fileSize < 17) return;
    unsigned char ft[9];
    if (brio_fseek(in->fp, (long long)(in->fileSize - 9), SEEK_SET) != 0) goto done;
    if (fread(ft, 1, 9, in->fp) != 9) goto done;
    /* Descriptor bit 7 means 12-byte entries with checksums; this writer
     * never sets it, so such a table is not ours and is not used. */
    if (get_le32(ft + 5) != SEEKTABLE_FOOTER_MAGIC || (ft[4] & 0x80u)) goto done;
    uint32_t n = get_le32(ft);
    const size_t esz = 8;
    uint64_t tab = (uint64_t)n * (uint64_t)esz + 9u + 8u;
    if (n == 0 || tab > in->fileSize) goto done;
    if (brio_fseek(in->fp, (long long)(in->fileSize - tab), SEEK_SET) != 0) goto done;
    unsigned char *t = (unsigned char *)malloc((size_t)tab);
    if (!t) goto done;
    if (fread(t, 1, (size_t)tab, in->fp) != (size_t)tab ||
        get_le32(t) != SEEKTABLE_SKIP_MAGIC || get_le32(t + 4) != (uint32_t)(tab - 8)) {
        free(t); goto done;
    }
    in->seekComp = (uint64_t *)malloc(((size_t)n + 1) * sizeof(uint64_t));
    in->seekRaw  = (uint64_t *)malloc(((size_t)n + 1) * sizeof(uint64_t));
    if (!in->seekComp || !in->seekRaw) {
        free(in->seekComp); free(in->seekRaw);
        in->seekComp = in->seekRaw = NULL; free(t); goto done;
    }
    uint64_t c = 0, r = 0;
    for (uint32_t i = 0; i < n; i++) {
        in->seekComp[i] = c; in->seekRaw[i] = r;
        c += get_le32(t + 8 + (size_t)i * esz);
        r += get_le32(t + 8 + (size_t)i * esz + 4);
    }
    in->seekComp[n] = c; in->seekRaw[n] = r;
    free(t);
    /* The table must describe the bytes in front of it exactly. */
    if (c + tab != in->fileSize) {
        free(in->seekComp); free(in->seekRaw);
        in->seekComp = in->seekRaw = NULL;
        goto done;
    }
    in->nSeek = n;
done:
    brio_fseek(in->fp, 0, SEEK_SET);
}

BrainRecIn *brainRecInOpen(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    unsigned char head[4] = {0};
    size_t hn = fread(head, 1, sizeof head, fp);
    BrainRecIn *in = (BrainRecIn *)calloc(1, sizeof *in);
    if (!in) { fclose(fp); return NULL; }
    in->fmt = brainRecDetectFormat(head, hn);
    in->fileSize = file_size_of(fp);

    if (in->fmt == BRAINREC_FMT_GZIP) {
        fclose(fp);
        in->gz = gzopen(path, "rb");
        if (!in->gz) { free(in); return NULL; }
        gzbuffer(in->gz, 1u << 20);
        return in;
    }

    in->fp   = fp;
    in->dctx = ZSTD_createDCtx();
    in->ibuf = (unsigned char *)malloc(IN_BUF_CAP);
    in->oCap = ZSTD_DStreamOutSize() * 8;
    in->obuf = (unsigned char *)malloc(in->oCap);
    if (!in->dctx || !in->ibuf || !in->obuf) { brainRecInClose(in); return NULL; }
    ZSTD_DCtx_setParameter(in->dctx, ZSTD_d_windowLogMax, 27);
    in_load_seek_table(in);
    return in;
}

/* Decode more bytes into obuf. Returns false at the end of data or on error. */
static bool in_refill(BrainRecIn *in) {
    in->oLen = in->oPos = 0;
    while (in->oLen == 0) {
        if (in->iPos == in->iLen) {
            in->iLen = fread(in->ibuf, 1, IN_BUF_CAP, in->fp);
            in->iPos = 0;
            in->compPos += in->iLen;
            if (in->iLen == 0) {
                in->eof = true;
                if (in->lastRet != 0) in->truncated = true;
                return false;
            }
        }
        ZSTD_inBuffer  zi = { in->ibuf, in->iLen, in->iPos };
        ZSTD_outBuffer zo = { in->obuf, in->oCap, 0 };
        size_t r = ZSTD_decompressStream(in->dctx, &zo, &zi);
        if (ZSTD_isError(r)) { in->err = true; return false; }
        in->lastRet = r;
        in->iPos = zi.pos;
        in->oLen = zo.pos;
    }
    return true;
}

size_t brainRecInRead(BrainRecIn *in, void *dst, size_t n) {
    if (!in || in->err) return 0;
    if (in->fmt == BRAINREC_FMT_GZIP) {
        size_t got = 0;
        unsigned char *d = (unsigned char *)dst;
        while (got < n) {
            unsigned want = (n - got) > (1u << 30) ? (1u << 30) : (unsigned)(n - got);
            int r = gzread(in->gz, d + got, want);
            if (r < 0) { in->err = true; break; }
            if (r == 0) { in->eof = true; break; }
            got += (size_t)r;
        }
        in->pos += got;
        return got;
    }
    unsigned char *d = (unsigned char *)dst;
    size_t got = 0;
    while (got < n) {
        if (in->oPos == in->oLen && !in_refill(in)) break;
        size_t k = in->oLen - in->oPos;
        if (k > n - got) k = n - got;
        if (d) memcpy(d + got, in->obuf + in->oPos, k);
        in->oPos += k; got += k;
    }
    in->pos += got;
    return got;
}

bool brainRecInSkip(BrainRecIn *in, uint64_t n) {
    if (!in || in->err) return false;
    if (n == 0) return true;
    if (in->fmt == BRAINREC_FMT_GZIP) {
        /* Skip by reading into a scratch buffer. A forward gzseek decodes
         * and drops the same bytes, but gzseek returns -1 once the position
         * is past 2 GiB where z_off_t is 32 bits (Windows), and a long old
         * recording is bigger than that uncompressed. */
        unsigned char scratch[64 * 1024];
        uint64_t left = n;
        while (left) {
            unsigned want = left > sizeof scratch ? (unsigned)sizeof scratch
                                                  : (unsigned)left;
            int r = gzread(in->gz, scratch, want);
            if (r < 0) { in->err = true; return false; }
            if (r == 0) { in->eof = true; return false; }
            in->pos += (uint64_t)r;
            left -= (uint64_t)r;
        }
        return true;
    }
    uint64_t target  = in->pos + n;
    uint64_t horizon = in->pos + (in->oLen - in->oPos);
    if (in->nSeek && target > horizon) {
        /* Find the last zstd frame that starts at or before target. */
        uint32_t lo = 0, hi = in->nSeek;   /* seekRaw[hi] is the end */
        if (target >= in->seekRaw[in->nSeek]) {
            in->pos = in->seekRaw[in->nSeek];
            in->eof = true;
            return target == in->seekRaw[in->nSeek];
        }
        while (hi - lo > 1) {
            uint32_t mid = (lo + hi) / 2;
            if (in->seekRaw[mid] <= target) lo = mid; else hi = mid;
        }
        /* Jump only when that frame starts past what is decoded. Frames of
         * zero decompressed bytes share a start; take the first of them. */
        while (lo > 0 && in->seekRaw[lo - 1] == in->seekRaw[lo]) lo--;
        if (in->seekRaw[lo] > horizon) {
            if (brio_fseek(in->fp, (long long)in->seekComp[lo], SEEK_SET) != 0) {
                in->err = true; return false;
            }
            ZSTD_DCtx_reset(in->dctx, ZSTD_reset_session_only);
            in->iLen = in->iPos = 0;
            in->oLen = in->oPos = 0;
            in->compPos = in->seekComp[lo];
            in->lastRet = 0;
            in->pos = in->seekRaw[lo];
        }
    }
    uint64_t left = target - in->pos;
    while (left) {
        size_t step = left > ((uint64_t)1 << 30) ? ((size_t)1 << 30) : (size_t)left;
        size_t got = brainRecInRead(in, NULL, step);
        left -= got;
        if (got < step) return false;
    }
    return true;
}

uint64_t brainRecInTell(const BrainRecIn *in) { return in ? in->pos : 0; }

uint64_t brainRecInCompressedPos(const BrainRecIn *in) {
    if (!in) return 0;
    if (in->fmt == BRAINREC_FMT_GZIP) {
        z_off_t o = gzoffset(in->gz);
        return o > 0 ? (uint64_t)o : 0;
    }
    return in->compPos - (in->iLen - in->iPos);
}

uint64_t brainRecInFileSize(const BrainRecIn *in) { return in ? in->fileSize : 0; }
bool brainRecInError(const BrainRecIn *in)      { return in ? in->err : true; }
bool brainRecInTruncated(const BrainRecIn *in) {
    if (!in) return false;
    if (in->fmt == BRAINREC_FMT_GZIP) return false;   /* zlib does not say */
    return in->truncated;
}
BrainRecFormat brainRecInFormat(const BrainRecIn *in) { return in ? in->fmt : BRAINREC_FMT_NONE; }
uint32_t brainRecInSeekEntries(const BrainRecIn *in)  { return in ? in->nSeek : 0; }

void brainRecInClose(BrainRecIn *in) {
    if (!in) return;
    if (in->gz) gzclose(in->gz);
    if (in->fp) fclose(in->fp);
    ZSTD_freeDCtx(in->dctx);
    free(in->ibuf);
    free(in->obuf);
    free(in->seekComp);
    free(in->seekRaw);
    free(in);
}

/* ===================================================================== */
/* Text logs                                                             */
/* ===================================================================== */

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

BrainLogFormat brainLogFormatOfName(const char *name) {
    if (name == NULL) return BRAINLOG_PLAIN;
    if (ends_with(name, BRAINLOG_ZSTD_EXT)) return BRAINLOG_ZSTD;
    if (ends_with(name, BRAINLOG_GZIP_EXT)) return BRAINLOG_GZIP;
    return BRAINLOG_PLAIN;
}

struct BrainLogOut {
    BrainLogFormat fmt;
    FILE          *fp;      /* plain and zstd */
    gzFile         gz;
    ZSTD_CCtx     *cctx;
    unsigned char *obuf;
    size_t         obufCap;
    bool           err;
};

BrainLogOut *brainLogOutOpen(const char *path, BrainLogFormat fmt,
                             uint64_t srcSize) {
    BrainLogOut *o = (BrainLogOut *)calloc(1, sizeof *o);
    if (!o) return NULL;
    o->fmt = fmt;
    if (fmt == BRAINLOG_GZIP) {
        o->gz = gzopen(path, "wb6");
        if (!o->gz) { free(o); return NULL; }
        gzbuffer(o->gz, 1u << 18);
        return o;
    }
    o->fp = fopen(path, "wb");
    if (!o->fp) { free(o); return NULL; }
    if (fmt != BRAINLOG_ZSTD) return o;

    /* A short text needs no 128 MiB window: the smallest power of two that
     * holds it (at least 1 MiB), so a reader of a small log needs little
     * memory. */
    int wlog = BRAINLOG_ZSTD_WINDOW_LOG;
    if (srcSize > 0) {
        int need = 20;
        while (need < BRAINLOG_ZSTD_WINDOW_LOG && ((uint64_t)1 << need) < srcSize) need++;
        wlog = need;
    }
    o->cctx = ZSTD_createCCtx();
    o->obufCap = ZSTD_CStreamOutSize();
    o->obuf = (unsigned char *)malloc(o->obufCap);
    if (!o->cctx || !o->obuf ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_compressionLevel, BRAINLOG_ZSTD_LEVEL)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_strategy, ZSTD_btopt)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_searchLog, BRAINLOG_ZSTD_SEARCH_LOG)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_minMatch, BRAINLOG_ZSTD_MIN_MATCH)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_windowLog, wlog)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_enableLongDistanceMatching, 1)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(o->cctx, ZSTD_c_checksumFlag, 1))) {
        ZSTD_freeCCtx(o->cctx);
        free(o->obuf);
        fclose(o->fp);
        free(o);
        return NULL;
    }
    return o;
}

static bool log_pump(BrainLogOut *o, const void *p, size_t n, ZSTD_EndDirective mode) {
    ZSTD_inBuffer in = { p, n, 0 };
    for (;;) {
        ZSTD_outBuffer out = { o->obuf, o->obufCap, 0 };
        size_t r = ZSTD_compressStream2(o->cctx, &out, &in, mode);
        if (ZSTD_isError(r)) { o->err = true; return false; }
        if (out.pos > 0 && fwrite(o->obuf, 1, out.pos, o->fp) != out.pos) {
            o->err = true;
            return false;
        }
        if (mode == ZSTD_e_continue ? (in.pos == in.size) : (r == 0)) return true;
    }
}

bool brainLogOutWrite(BrainLogOut *o, const void *p, size_t n) {
    if (!o || o->err) return false;
    if (n == 0) return true;
    switch (o->fmt) {
        case BRAINLOG_GZIP: {
            const unsigned char *d = (const unsigned char *)p;
            while (n > 0) {
                unsigned k = n > (1u << 30) ? (1u << 30) : (unsigned)n;
                if (gzwrite(o->gz, d, k) != (int)k) { o->err = true; return false; }
                d += k;
                n -= k;
            }
            return true;
        }
        case BRAINLOG_ZSTD:
            return log_pump(o, p, n, ZSTD_e_continue);
        default:
            if (fwrite(p, 1, n, o->fp) != n) { o->err = true; return false; }
            return true;
    }
}

bool brainLogOutClose(BrainLogOut *o) {
    bool ok;
    if (!o) return true;
    ok = !o->err;
    if (o->fmt == BRAINLOG_GZIP) {
        if (gzclose(o->gz) != Z_OK) ok = false;
    } else {
        if (ok && o->fmt == BRAINLOG_ZSTD && !log_pump(o, NULL, 0, ZSTD_e_end)) ok = false;
        if (fclose(o->fp) != 0) ok = false;
    }
    ZSTD_freeCCtx(o->cctx);
    free(o->obuf);
    free(o);
    return ok;
}
