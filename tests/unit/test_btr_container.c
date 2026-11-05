/*
 * test_btr_container.c - the brainrec.btr container (brain_record_io.c).
 *
 * The -brain-debug recorder writes brainrec.btr as zstd: a marker frame, one
 * zstd frame per map keyframe block, and a seek table at the end. Old
 * recordings are gzip. Every reader opens the file through brainRecInOpen,
 * which detects the format from the first bytes.
 *
 * These cases record a short session with the real recorder and read it back
 * the two ways the tools read it: through the shared frame walker (the
 * splitter, the unit tests) and through the byte-skipping walk that
 * BrainTest's session peek and loader do (btPeekSession). A gzip copy of the
 * same bytes, written here with zlib, must read back the same. Further cases
 * check skip across blocks and a file cut off by a hard kill.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <zlib.h>

#include "global.h"
#include "server_sim.h"
#include "brain_record.h"
#include "brain_record_io.h"
#include "brain_record_walk.h"
#include "test_harness.h"

/* Frames per test recording: more than two keyframe intervals, so the zstd
 * file has at least three map blocks. */
#define BTR_TEST_FRAMES (2 * BRAINREC_MAP_KEYFRAME_INTERVAL + 100)

bool utBtrRecordSession(const char *dir, int frames, uint32_t *firstTick,
                        uint32_t *lastTick) {
    ServerSim *sim = ut_make_running_sim("Recorded");
    int n;
    if (sim == NULL) {
        return false;
    }
    if (!SDL_CreateDirectory(dir)) {
        serverSimDestroy(sim);
        return false;
    }
    brainRecordSetSessionDir(dir);
    brainRecordSetEnabled(true);
    for (n = 0; n < frames; n++) {
        serverSimTick(sim);
        if (n == 0 && firstTick != NULL) {
            *firstTick = serverSimGetTick(sim);
        }
        brainRecordTick(sim);
    }
    if (lastTick != NULL) {
        *lastTick = serverSimGetTick(sim);
    }
    brainRecordEndGame();
    brainRecordSetEnabled(false);
    brainRecordSetSessionDir(NULL);
    serverSimDestroy(sim);
    return true;
}

/* Walk every frame with the shared walker. Returns the frame count, or -1.
 * ticks (may be NULL) gets each frame's tick. */
static int btrWalkFrames(const char *path, uint32_t *ticks, int maxTicks,
                         BrainRecFormat *fmtOut) {
    BrainRecIn *g = brainRecInOpen(path);
    BrainRecReader r;
    BrainRecFrameInfo fi;
    BrainRecWalkStatus st;
    int n = 0;
    if (g == NULL) {
        return -1;
    }
    if (fmtOut != NULL) {
        *fmtOut = brainRecInFormat(g);
    }
    brainRecWalkReaderFromIn(&r, g);
    if (brainRecWalkPreamble(&r, NULL, NULL) != BRAINREC_WALK_OK) {
        brainRecInClose(g);
        return -1;
    }
    while ((st = brainRecWalkFrame(&r, &fi)) == BRAINREC_WALK_OK) {
        if (ticks != NULL && n < maxTicks) {
            ticks[n] = fi.tick;
        }
        n++;
    }
    brainRecInClose(g);
    return st == BRAINREC_WALK_EOF ? n : -1;
}

static uint8_t btrU8(BrainRecIn *g) {
    uint8_t v = 0;
    brainRecInRead(g, &v, 1);
    return v;
}

static uint16_t btrU16(BrainRecIn *g) {
    uint16_t v = 0;
    brainRecInRead(g, &v, 2);
    return v;
}

static uint32_t btrU32(BrainRecIn *g) {
    uint32_t v = 0;
    brainRecInRead(g, &v, 4);
    return v;
}

/* The walk BrainTest's btPeekSession (deep scan) and btLoadSession do: read
 * the header, then each frame field by field, skipping the bodies with
 * brainRecInSkip. Returns the frame count, or -1. */
static int btrPeekWalk(const char *path, uint32_t *ticks, int maxTicks) {
    BrainRecIn *g = brainRecInOpen(path);
    BrainRecHeader hdr;
    uint32_t llen;
    uint32_t fm;
    int n = 0;
    if (g == NULL) {
        return -1;
    }
    if (brainRecInRead(g, &hdr, sizeof hdr) != sizeof hdr || !brainRecMagicMatches(&hdr) ||
        !brainRecVersionReadable(&hdr)) {
        brainRecInClose(g);
        return -1;
    }
    llen = btrU32(g);
    if (llen != 0 && !brainRecInSkip(g, llen)) {
        brainRecInClose(g);
        return -1;
    }
    fm = btrU32(g);
    while (fm == BRAINREC_FRAME_MAGIC) {
        uint32_t tick = btrU32(g);
        uint8_t tc, sc, bc, pc, kf, nbot;
        int b;
        if (ticks != NULL && n < maxTicks) {
            ticks[n] = tick;
        }
        tc = btrU8(g); brainRecInSkip(g, (uint64_t)tc * sizeof(TankSnapshot));
        sc = btrU8(g); brainRecInSkip(g, (uint64_t)sc * sizeof(ShellSnapshot));
        bc = btrU8(g); brainRecInSkip(g, (uint64_t)bc * sizeof(BaseSnapshot));
        pc = btrU8(g); brainRecInSkip(g, (uint64_t)pc * sizeof(PillSnapshot));
        kf = btrU8(g);
        if (kf) {
            brainRecInSkip(g, MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
        } else {
            uint32_t nd = btrU32(g);
            brainRecInSkip(g, (uint64_t)nd * 5);
        }
        nbot = btrU8(g);
        for (b = 0; b < nbot; b++) {
            uint16_t nc;
            uint32_t oc, k, pl;
            (void)btrU8(g);                 /* slot */
            brainRecInSkip(g, 4);           /* thinkMs */
            brainRecInSkip(g, 32 + 12 + 32);
            nc = btrU16(g);
            brainRecInSkip(g, (uint64_t)nc * sizeof(BrainRecCandidate));
            oc = btrU32(g);
            for (k = 0; k < oc; k++) {
                uint8_t tl;
                brainRecInSkip(g, 27);
                tl = btrU8(g);
                if (tl) {
                    brainRecInSkip(g, tl);
                }
            }
            pl = btrU32(g);
            if (pl) {
                brainRecInSkip(g, pl);
            }
        }
        brainRecInSkip(g, MAX_TANKS * 4);
        if (hdr.version >= 7u) {
            brainRecInSkip(g, 8);
        }
        if (hdr.version >= 8u) {            /* v8 Dijkstra box */
            uint32_t dl = btrU32(g);
            if (dl) {
                brainRecInSkip(g, dl);
            }
        }
        if (brainRecInError(g)) {
            brainRecInClose(g);
            return -1;
        }
        n++;
        fm = btrU32(g);
    }
    brainRecInClose(g);
    return n;
}

/* Read every decompressed byte of path. Caller frees. */
static unsigned char *btrReadAll(const char *path, size_t *lenOut) {
    BrainRecIn *g = brainRecInOpen(path);
    size_t cap = 1u << 20, len = 0;
    unsigned char *buf;
    if (g == NULL) {
        return NULL;
    }
    buf = (unsigned char *)malloc(cap);
    while (buf != NULL) {
        size_t got;
        if (len == cap) {
            unsigned char *nb = (unsigned char *)realloc(buf, cap * 2);
            if (nb == NULL) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        got = brainRecInRead(g, buf + len, cap - len);
        if (got == 0) {
            break;
        }
        len += got;
    }
    if (buf != NULL && brainRecInError(g)) {
        free(buf);
        buf = NULL;
    }
    brainRecInClose(g);
    *lenOut = len;
    return buf;
}

int run_btr_zstd_recording_reads_back(void) {
    char dir[512], path[600], gzPath[600];
    static uint32_t ticksWalk[BTR_TEST_FRAMES + 8];
    static uint32_t ticksPeek[BTR_TEST_FRAMES + 8];
    static uint32_t ticksGz[BTR_TEST_FRAMES + 8];
    uint32_t firstTick = 0, lastTick = 0;
    BrainRecFormat fmt = BRAINREC_FMT_NONE;
    unsigned char *raw;
    size_t rawLen = 0;
    int nWalk, nPeek, nGzWalk, nGzPeek, i;

    UT_ASSERT_MSG(utScratchPath(dir, sizeof(dir), "session"), "no scratch dir");
    UT_ASSERT(utBtrRecordSession(dir, BTR_TEST_FRAMES, &firstTick, &lastTick));
    snprintf(path, sizeof(path), "%s/%s", dir, BRAINREC_FILENAME);

    /* zstd, with a seek table that lists the marker, the header block and
     * one block per map keyframe. */
    {
        BrainRecIn *g = brainRecInOpen(path);
        UT_ASSERT_MSG(g != NULL, "the recorder wrote no %s", path);
        fmt = brainRecInFormat(g);
        i = (int)brainRecInSeekEntries(g);
        brainRecInClose(g);
        UT_ASSERT_MSG(fmt == BRAINREC_FMT_ZSTD, "the recording is format %d, not zstd", (int)fmt);
        UT_ASSERT_MSG(i >= 5, "the seek table has %d entries; wanted the marker, the header "
                      "and at least three map blocks", i);
    }

    nWalk = btrWalkFrames(path, ticksWalk, BTR_TEST_FRAMES + 8, NULL);
    UT_ASSERT_MSG(nWalk == BTR_TEST_FRAMES, "the walker read %d frames, wanted %d",
                  nWalk, BTR_TEST_FRAMES);
    UT_ASSERT_MSG(ticksWalk[0] == firstTick && ticksWalk[nWalk - 1] == lastTick,
                  "the walker's ticks run %lu..%lu, wanted %lu..%lu",
                  (unsigned long)ticksWalk[0], (unsigned long)ticksWalk[nWalk - 1],
                  (unsigned long)firstTick, (unsigned long)lastTick);
    /* One frame per sim step, so the ticks rise by the same step each
     * frame. */
    for (i = 2; i < nWalk; i++) {
        UT_ASSERT_MSG(ticksWalk[1] > ticksWalk[0] &&
                      ticksWalk[i] - ticksWalk[i - 1] == ticksWalk[1] - ticksWalk[0],
                      "frame %d has tick %lu after %lu",
                      i, (unsigned long)ticksWalk[i], (unsigned long)ticksWalk[i - 1]);
    }

    nPeek = btrPeekWalk(path, ticksPeek, BTR_TEST_FRAMES + 8);
    UT_ASSERT_MSG(nPeek == nWalk, "the BrainTest walk read %d frames, the walker %d", nPeek, nWalk);
    UT_ASSERT(memcmp(ticksPeek, ticksWalk, (size_t)nWalk * sizeof(uint32_t)) == 0);

    /* The same bytes as an old gzip recording. */
    raw = btrReadAll(path, &rawLen);
    UT_ASSERT_MSG(raw != NULL && rawLen > 0, "the zstd recording did not decompress");
    UT_ASSERT_MSG(utScratchPath(gzPath, sizeof(gzPath), "old.btr"), "no scratch path");
    {
        gzFile gz = gzopen(gzPath, "wb1");
        int ok = gz != NULL && gzwrite(gz, raw, (unsigned)rawLen) == (int)rawLen;
        if (gz != NULL && gzclose(gz) != Z_OK) {
            ok = 0;
        }
        free(raw);
        UT_ASSERT_MSG(ok, "could not write %s", gzPath);
    }
    nGzWalk = btrWalkFrames(gzPath, ticksGz, BTR_TEST_FRAMES + 8, &fmt);
    UT_ASSERT_MSG(fmt == BRAINREC_FMT_GZIP, "the gzip copy reads as format %d", (int)fmt);
    UT_ASSERT_MSG(nGzWalk == nWalk, "the walker read %d frames of the gzip copy, wanted %d",
                  nGzWalk, nWalk);
    UT_ASSERT(memcmp(ticksGz, ticksWalk, (size_t)nWalk * sizeof(uint32_t)) == 0);
    nGzPeek = btrPeekWalk(gzPath, ticksGz, BTR_TEST_FRAMES + 8);
    UT_ASSERT_MSG(nGzPeek == nWalk, "the BrainTest walk read %d frames of the gzip copy, wanted %d",
                  nGzPeek, nWalk);
    UT_ASSERT(memcmp(ticksGz, ticksWalk, (size_t)nWalk * sizeof(uint32_t)) == 0);
    return 0;
}

/* Byte i of the test stream. */
static unsigned char btrPattern(uint64_t i) {
    return (unsigned char)((i * 2654435761u) >> 13);
}

#define BTR_BLOCK_BYTES (3u * 1024u * 1024u)
#define BTR_BLOCKS      4u

static bool btrWritePatternFile(const char *path) {
    BrainRecOut *o = brainRecOutOpen(path);
    static unsigned char buf[64 * 1024];
    uint64_t pos = 0;
    unsigned b;
    bool ok = o != NULL;
    for (b = 0; b < BTR_BLOCKS && ok; b++) {
        uint64_t end = pos + BTR_BLOCK_BYTES;
        if (b > 0) {
            ok = brainRecOutNewBlock(o);
        }
        while (ok && pos < end) {
            size_t n = sizeof buf, k;
            if (n > end - pos) {
                n = (size_t)(end - pos);
            }
            for (k = 0; k < n; k++) {
                buf[k] = btrPattern(pos + k);
            }
            ok = brainRecOutWrite(o, buf, n);
            pos += n;
        }
        if (ok && b == 1) {
            ok = brainRecOutFlush(o);
        }
    }
    if (o != NULL && !brainRecOutClose(o)) {
        ok = false;
    }
    return ok;
}

int run_btr_zstd_skip_across_blocks(void) {
    char path[600];
    static unsigned char buf[4096];
    const uint64_t total = (uint64_t)BTR_BLOCK_BYTES * BTR_BLOCKS;
    /* Places to land: inside block 0, on a block start, mid block 2, the
     * last bytes. */
    const uint64_t at[4] = { 1000, (uint64_t)BTR_BLOCK_BYTES, 2 * (uint64_t)BTR_BLOCK_BYTES + 12345,
                             total - sizeof buf };
    BrainRecIn *g;
    unsigned char head[16];
    FILE *f;
    int i;

    UT_ASSERT_MSG(utScratchPath(path, sizeof(path), "pattern.btr"), "no scratch path");
    UT_ASSERT_MSG(btrWritePatternFile(path), "could not write %s", path);

    /* The file starts with the format marker. */
    f = fopen(path, "rb");
    UT_ASSERT(f != NULL);
    i = (int)fread(head, 1, sizeof head, f);
    fclose(f);
    UT_ASSERT(i == (int)sizeof head);
    UT_ASSERT(brainRecDetectFormat(head, sizeof head) == BRAINREC_FMT_ZSTD);
    UT_ASSERT_MSG(memcmp(head + 8, "WBBTRZS1", 8) == 0, "the marker frame is missing");

    g = brainRecInOpen(path);
    UT_ASSERT(g != NULL);
    UT_ASSERT_MSG(brainRecInSeekEntries(g) == 1 + BTR_BLOCKS,
                  "the seek table has %lu entries, wanted %u",
                  (unsigned long)brainRecInSeekEntries(g), 1 + BTR_BLOCKS);
    for (i = 0; i < 4; i++) {
        uint64_t here = brainRecInTell(g);
        size_t k;
        if (!brainRecInSkip(g, at[i] - here) || brainRecInTell(g) != at[i] ||
            brainRecInRead(g, buf, sizeof buf) != sizeof buf) {
            brainRecInClose(g);
            UT_ASSERT_MSG(false, "skip to %llu failed", (unsigned long long)at[i]);
        }
        for (k = 0; k < sizeof buf; k++) {
            if (buf[k] != btrPattern(at[i] + k)) {
                brainRecInClose(g);
                UT_ASSERT_MSG(false, "byte %llu is wrong after a skip",
                              (unsigned long long)(at[i] + k));
            }
        }
    }
    /* At the end: nothing more, and not cut off. */
    i = (int)brainRecInRead(g, buf, sizeof buf);
    UT_ASSERT_MSG(i == 0 && !brainRecInTruncated(g) && !brainRecInError(g),
                  "the end of the stream reads %d more bytes", i);
    UT_ASSERT(!brainRecInSkip(g, 1));
    brainRecInClose(g);
    return 0;
}

/* An old gzip .btr bigger than 2 GiB uncompressed: a skip past 2 GiB
 * lands on the right bytes. gzseek fails there where z_off_t is 32 bits,
 * so the skip reads instead. The data is zeros, so the file is small. */
int run_btr_gzip_skip_past_2gib(void) {
    char path[600];
    static unsigned char zeros[1u << 20];
    const unsigned char tail[8] = { 'W', 'B', 'E', 'N', 'D', '2', 'G', 'B' };
    const uint64_t body = ((uint64_t)1 << 31) + ((uint64_t)1 << 20);
    unsigned char got[8];
    uint64_t left;
    BrainRecIn *g;
    gzFile gz;
    bool ok = true;

    UT_ASSERT_MSG(utScratchPath(path, sizeof(path), "big_old.btr"), "no scratch path");
    gz = gzopen(path, "wb1");
    UT_ASSERT_MSG(gz != NULL, "could not write %s", path);
    gzbuffer(gz, 1u << 20);
    for (left = body; left > 0 && ok; left -= sizeof zeros) {
        ok = gzwrite(gz, zeros, (unsigned)sizeof zeros) == (int)sizeof zeros;
    }
    ok = ok && gzwrite(gz, tail, (unsigned)sizeof tail) == (int)sizeof tail;
    ok = (gzclose(gz) == Z_OK) && ok;
    UT_ASSERT_MSG(ok, "could not write %s", path);

    g = brainRecInOpen(path);
    UT_ASSERT(g != NULL);
    UT_ASSERT(brainRecInFormat(g) == BRAINREC_FMT_GZIP);
    ok = brainRecInSkip(g, body) && brainRecInTell(g) == body &&
         brainRecInRead(g, got, sizeof got) == sizeof got &&
         memcmp(got, tail, sizeof tail) == 0;
    ok = ok && !brainRecInSkip(g, 1) && !brainRecInError(g);
    brainRecInClose(g);
    SDL_RemovePath(path);
    UT_ASSERT_MSG(ok, "a skip of %llu bytes did not land on the last 8 bytes",
                  (unsigned long long)body);
    return 0;
}

int run_btr_zstd_cut_file_reads_its_prefix(void) {
    char path[600], cutPath[600];
    static unsigned char buf[64 * 1024];
    unsigned char *file;
    long size;
    uint64_t got = 0;
    BrainRecIn *g;
    FILE *f;

    UT_ASSERT_MSG(utScratchPath(path, sizeof(path), "pattern.btr"), "no scratch path");
    UT_ASSERT_MSG(utScratchPath(cutPath, sizeof(cutPath), "cut.btr"), "no scratch path");
    UT_ASSERT_MSG(btrWritePatternFile(path), "could not write %s", path);

    /* Keep 60% of the file: no seek table, and the end falls inside a
     * block, as after a hard kill. */
    f = fopen(path, "rb");
    UT_ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (unsigned char *)malloc((size_t)size);
    UT_ASSERT(file != NULL);
    if (fread(file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(file);
        UT_ASSERT_MSG(false, "could not read %s", path);
    }
    fclose(f);
    f = fopen(cutPath, "wb");
    if (f == NULL) {
        free(file);
        UT_ASSERT_MSG(false, "could not write %s", cutPath);
    }
    fwrite(file, 1, (size_t)(size * 6 / 10), f);
    fclose(f);
    free(file);

    g = brainRecInOpen(cutPath);
    UT_ASSERT(g != NULL);
    UT_ASSERT(brainRecInFormat(g) == BRAINREC_FMT_ZSTD);
    UT_ASSERT(brainRecInSeekEntries(g) == 0);
    for (;;) {
        size_t n = brainRecInRead(g, buf, sizeof buf), k;
        for (k = 0; k < n; k++) {
            if (buf[k] != btrPattern(got + k)) {
                brainRecInClose(g);
                UT_ASSERT_MSG(false, "byte %llu of the cut file is wrong",
                              (unsigned long long)(got + k));
            }
        }
        got += n;
        if (n < sizeof buf) {
            break;
        }
    }
    {
        bool cut = brainRecInTruncated(g);
        brainRecInClose(g);
        UT_ASSERT_MSG(cut, "a file cut inside a block does not report truncated");
    }
    /* Two whole blocks were flushed before the cut; at least those read. */
    UT_ASSERT_MSG(got >= 2u * BTR_BLOCK_BYTES, "only %llu bytes of the cut file read",
                  (unsigned long long)got);
    UT_ASSERT(got < (uint64_t)BTR_BLOCK_BYTES * BTR_BLOCKS);
    return 0;
}

int run_btr_block_logs_compress(void) {
    char dir[512], p[600], gz[600];
    const char *names[3] = { "print2_bot0.log", "player0.jsonl", "brain_p0.jsonl" };
    const char *keep = "killbot.log";
    const char *text = "===TICK 1===\nline one\n===TICK 2===\nline two\n";
    int i;

    UT_ASSERT_MSG(utScratchPath(dir, sizeof(dir), NULL), "no scratch dir");
    for (i = 0; i < 4; i++) {
        FILE *f;
        snprintf(p, sizeof p, "%s/%s", dir, i < 3 ? names[i] : keep);
        f = fopen(p, "wb");
        UT_ASSERT(f != NULL);
        fputs(text, f);
        fclose(f);
    }
    UT_ASSERT_MSG(brainRecordCompressLogsNow(dir) == 3, "not three logs compressed");
    for (i = 0; i < 3; i++) {
        char back[256];
        gzFile g;
        int n;
        SDL_PathInfo pi;
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        snprintf(gz, sizeof gz, "%s/%s.gz", dir, names[i]);
        UT_ASSERT_MSG(!SDL_GetPathInfo(p, &pi), "%s is still there", names[i]);
        g = gzopen(gz, "rb");
        UT_ASSERT_MSG(g != NULL, "no %s.gz", names[i]);
        n = gzread(g, back, sizeof back - 1);
        gzclose(g);
        UT_ASSERT(n == (int)strlen(text) && memcmp(back, text, (size_t)n) == 0);
    }
    snprintf(p, sizeof p, "%s/%s", dir, keep);
    {
        SDL_PathInfo pi;
        UT_ASSERT_MSG(SDL_GetPathInfo(p, &pi), "killbot.log was touched");
    }
    /* A second pass finds nothing to do. */
    UT_ASSERT(brainRecordCompressLogsNow(dir) == 0);
    return 0;
}
