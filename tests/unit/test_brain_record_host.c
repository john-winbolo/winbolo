/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Bot Host brain recordings: the merge of a host's .hbr files into the
 * server's brainrec.btr, the path checks the upload and cleardebug use,
 * and the CTRL_BOT_HOST_REC wire message.
 *
 * The .btr files here are made by hand: frames with no tanks, shells,
 * bases, pills or map changes, so the only thing in them is the per-bot
 * entries. Each local body carries a thinkMs the test picks, and each host
 * record carries its own input sequence as its thinkMs, so the merged file
 * says which record filled which frame.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <zlib.h>

#include "global.h"
#include "brain_record.h"
#include "brain_record_host.h"
#include "brain_record_walk.h"
#include "client_sim.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "bulk_transfer.h"
#include "transport_udp_server_internal.h" /* serverBotRecPiece, udpServer */
#include "test_harness.h"

/* One bot entry of a hand-made frame. */
typedef struct {
    uint8_t  slot;
    bool     hosted;
    float    thinkMs;    /* local body */
    uint16_t gen;        /* hosted marker */
    uint32_t lpi;        /* hosted marker: the last input the server took */
} TbrBot;

static bool tbrWrite(BrainRecOut *f, const void *p, size_t n) {
    return n == 0 || brainRecOutWrite(f, p, n);
}

static BrainRecOut *tbrOpenBtr(const char *path) {
    BrainRecHeader hdr;
    const char *legend = "{\"cats\":[]}";
    uint32_t llen = (uint32_t)strlen(legend);
    BrainRecOut *f = brainRecOutOpen(path);
    if (f == NULL) return NULL;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, BRAINREC_MAGIC, BRAINREC_MAGIC_LEN);
    hdr.version = BRAINREC_VERSION;
    memcpy(hdr.mapName, "Test", 4);
    if (!tbrWrite(f, &hdr, sizeof(hdr)) || !tbrWrite(f, &llen, 4) ||
        !tbrWrite(f, legend, llen)) {
        (void)brainRecOutClose(f);
        return NULL;
    }
    return f;
}

static bool tbrFrame(BrainRecOut *f, uint32_t tick, const TbrBot *bots,
                     int n) {
    uint32_t magic = BRAINREC_FRAME_MAGIC;
    uint8_t zero4[4] = {0, 0, 0, 0};
    uint8_t kf = 0;
    uint32_t nd = 0;
    uint8_t nb = (uint8_t)n;
    uint8_t alliances[MAX_TANKS * 4];
    uint32_t masks[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    uint32_t dijLen = 0;   /* v8: an empty recorded Dijkstra box */
    BrainRecBuf b;
    int i;
    bool ok;

    ok = tbrWrite(f, &magic, 4) && tbrWrite(f, &tick, 4) &&
         tbrWrite(f, zero4, 4) &&        /* tanks, shells, bases, pills */
         tbrWrite(f, &kf, 1) && tbrWrite(f, &nd, 4) && tbrWrite(f, &nb, 1);
    brainRecBufInit(&b);
    for (i = 0; ok && i < n; i++) {
        brainRecBufReset(&b);
        if (bots[i].hosted) {
            brainRecBodyEncodeHosted(&b, bots[i].gen, "testhost", bots[i].lpi);
        } else {
            (void)brainRecBodyEncode(&b, bots[i].thinkMs, NULL, NULL, NULL,
                                     NULL);
        }
        ok = !b.failed && tbrWrite(f, &bots[i].slot, 1) &&
             tbrWrite(f, b.data, b.len);
    }
    brainRecBufFree(&b);
    memset(alliances, 0, sizeof(alliances));
    return ok && tbrWrite(f, alliances, sizeof(alliances)) &&
           tbrWrite(f, masks, sizeof(masks)) &&
           (BRAINREC_VERSION < 8u || tbrWrite(f, &dijLen, 4));
}

/* A host file for (slot, gen) with one record per seq in [first, last],
 * step 2. Each record's thinkMs is its seq; the even ones carry a pool. */
static bool tbrHostFile(const char *path, uint8_t slot, uint16_t gen,
                        uint32_t first, uint32_t last) {
    HostRecWriter w;
    HostRecHeader hdr;
    BrainRecBuf b;
    uint32_t s;
    bool ok = true;
    memset(&hdr, 0, sizeof(hdr));
    SDL_strlcpy(hdr.hostName, "testhost", sizeof(hdr.hostName));
    SDL_strlcpy(hdr.block, "2026-11-04_120000", sizeof(hdr.block));
    hdr.slot = slot;
    hdr.placeGen = gen;
    if (!hostRecWriterOpen(&w, path, &hdr, "{\"cats\":[\"host\"]}")) {
        return false;
    }
    brainRecBufInit(&b);
    for (s = first; ok && s <= last; s += 2) {
        brainRecBufReset(&b);
        (void)brainRecBodyEncode(&b, (float)s, NULL, NULL, NULL,
                                 (s % 4u) == 0 ? "{\"pool\":1}" : NULL);
        ok = !b.failed && hostRecWriterAppend(&w, s, 1000u + s / 2u, b.data,
                                              b.len);
    }
    brainRecBufFree(&b);
    hostRecWriterClose(&w);
    return ok;
}

/* Reading a merged file back. */
typedef struct {
    BrainRecIn   *f;
    BrainRecBuf  *buf;
} TbrCap;

static size_t tbrCapRead(void *ctx, void *dst, size_t len) {
    TbrCap *c = (TbrCap *)ctx;
    size_t total = 0;
    while (total < len) {
        size_t got = brainRecInRead(c->f, (uint8_t *)dst + total, len - total);
        if (got == 0) break;
        total += got;
    }
    if (total > 0) brainRecBufPut(c->buf, dst, total);
    return total;
}

static bool tbrCapSkip(void *ctx, size_t len) {
    TbrCap *c = (TbrCap *)ctx;
    uint8_t tmp[4096];
    while (len > 0) {
        size_t n = len < sizeof(tmp) ? len : sizeof(tmp);
        if (!hostRecReadExact(c->f, tmp, n)) return false;
        brainRecBufPut(c->buf, tmp, n);
        len -= n;
    }
    return true;
}

typedef struct {
    int      frames;
    /* Per frame, per bot (at most 4 bots): slot, hosted, thinkMs, pool. */
    uint8_t  slot[64][4];
    bool     hosted[64][4];
    float    thinkMs[64][4];
    uint32_t poolLen[64][4];
    int      bots[64];
} TbrOut;

/* Walk path with the frame walker (what BrainTest's loader and the
 * splitter use) and pull out each bot's entry. */
static bool tbrRead(const char *path, TbrOut *o) {
    BrainRecBuf buf;
    TbrCap c;
    BrainRecReader r;
    bool ok = true;
    memset(o, 0, sizeof(*o));
    c.f = brainRecInOpen(path);
    if (c.f == NULL) return false;
    /* Every file here, the merged one too, is in the zstd container. */
    if (brainRecInFormat(c.f) != BRAINREC_FMT_ZSTD) {
        brainRecInClose(c.f);
        return false;
    }
    brainRecBufInit(&buf);
    c.buf = &buf;
    memset(&r, 0, sizeof(r));
    r.ctx = &c;
    r.read = tbrCapRead;
    r.skip = tbrCapSkip;
    if (brainRecWalkPreamble(&r, NULL, NULL) != BRAINREC_WALK_OK) {
        ok = false;
    }
    while (ok) {
        BrainRecFrameInfo fi;
        BrainRecWalkStatus ws;
        size_t pos;
        int b;
        brainRecBufReset(&buf);
        ws = brainRecWalkFrame(&r, &fi);
        if (ws == BRAINREC_WALK_EOF) break;
        if (ws != BRAINREC_WALK_OK || o->frames >= 64 || fi.botCount > 4) {
            ok = false;
            break;
        }
        /* magic, tick, four zero counts, kf 0, nd 0 */
        pos = 4 + 4 + 4 + 1 + 4;
        o->bots[o->frames] = buf.data[pos++];
        for (b = 0; b < o->bots[o->frames]; b++) {
            size_t bl;
            uint32_t pl;
            o->slot[o->frames][b] = buf.data[pos++];
            bl = brainRecBodyMeasure(buf.data + pos, buf.len - pos);
            if (bl < 8) { ok = false; break; }
            o->hosted[o->frames][b] =
                brainRecBodyIsHosted(buf.data + pos, bl, NULL, NULL, NULL);
            memcpy(&o->thinkMs[o->frames][b], buf.data + pos, 4);
            memcpy(&pl, buf.data + pos + bl - 4, 4);
            /* the pool JSON is the tail: its length sits before it */
            o->poolLen[o->frames][b] = 0;
            if (bl >= 4) {
                size_t k;
                /* Walk back: find a u32 L at bl-4-L that equals L. */
                for (k = 0; k + 4 <= bl && k <= 64; k++) {
                    uint32_t cand;
                    memcpy(&cand, buf.data + pos + bl - 4 - k, 4);
                    if (cand == (uint32_t)k) {
                        o->poolLen[o->frames][b] = cand;
                        break;
                    }
                }
            }
            (void)pl;
            pos += bl;
        }
        o->frames++;
    }
    brainRecInClose(c.f);
    brainRecBufFree(&buf);
    return ok;
}

/* ================================================================
 * Host records fill the hosted markers by input sequence.
 * ================================================================ */
int run_brainrec_host_merge_by_tick(void) {
    char btr[1024], hbr[1024], out[1024];
    const char *files[1];
    BrainRecOut *f;
    BrainRecMergeStats st;
    TbrOut o;
    char err[256];
    int i;

    UT_ASSERT(utScratchPath(btr, sizeof(btr), "brainrec.btr"));
    UT_ASSERT(utScratchPath(hbr, sizeof(hbr), "bot1_g5.hbr"));
    UT_ASSERT(utScratchPath(out, sizeof(out), "merged.btr"));

    /* 20 frames: slot 0 runs on the server, slot 1 on the host (gen 5).
     * The server took input 10, 12, ... 48 by these frames. */
    f = tbrOpenBtr(btr);
    UT_ASSERT(f != NULL);
    for (i = 0; i < 20; i++) {
        TbrBot bots[2];
        memset(bots, 0, sizeof(bots));
        bots[0].slot = 0;
        bots[0].thinkMs = 1.5f;
        bots[1].slot = 1;
        bots[1].hosted = true;
        bots[1].gen = 5;
        bots[1].lpi = 10u + 2u * (uint32_t)i;
        UT_ASSERT(tbrFrame(f, 100u + 2u * (uint32_t)i, bots, 2));
    }
    UT_ASSERT(brainRecOutClose(f));
    /* The host thought from input 12 on (its first think came one think
     * after the server's first marker) and stopped at 44: the last two
     * frames lag by 2 and 4, inside the stale window. */
    UT_ASSERT(tbrHostFile(hbr, 1, 5, 12, 44));

    files[0] = hbr;
    err[0] = '\0';
    UT_ASSERT_MSG(brainRecMergeFiles(btr, files, 1, out, &st, err, sizeof(err)),
                  "merge failed: %s", err);
    UT_ASSERT(st.frames == 20);
    UT_ASSERT(st.markers == 20);
    UT_ASSERT(st.hostFiles == 1);
    UT_ASSERT_MSG(st.missing == 0, "missing %u", (unsigned)st.missing);
    UT_ASSERT_MSG(st.filled == 18 && st.stale == 2, "filled %u stale %u",
                  (unsigned)st.filled, (unsigned)st.stale);

    UT_ASSERT(tbrRead(out, &o));
    UT_ASSERT(o.frames == 20);
    for (i = 0; i < 20; i++) {
        uint32_t lpi = 10u + 2u * (uint32_t)i;
        float want = (float)(lpi < 12u ? 12u : (lpi > 44u ? 44u : lpi));
        UT_ASSERT(o.bots[i] == 2);
        UT_ASSERT(o.slot[i][0] == 0 && !o.hosted[i][0]);
        UT_ASSERT(o.thinkMs[i][0] == 1.5f);
        UT_ASSERT(o.slot[i][1] == 1);
        UT_ASSERT_MSG(!o.hosted[i][1], "frame %d still a marker", i);
        UT_ASSERT_MSG(o.thinkMs[i][1] == want, "frame %d: %f, want %f", i,
                      (double)o.thinkMs[i][1], (double)want);
    }
    /* A record used twice keeps its pool only once. Frame 0 used record
     * 12 (pool), frame 1 used it again: no pool. */
    UT_ASSERT(o.poolLen[0][1] > 0);
    UT_ASSERT(o.poolLen[1][1] == 0);
    return 0;
}

/* ================================================================
 * A bot that moves between the host and the server inside one block,
 * and is placed on the host twice (two gens).
 * ================================================================ */
int run_brainrec_host_merge_mid_block_move(void) {
    char btr[1024], h5[1024], h9[1024], out[1024];
    const char *files[2];
    BrainRecOut *f;
    BrainRecMergeStats st;
    TbrOut o;
    char err[256];
    int i;

    UT_ASSERT(utScratchPath(btr, sizeof(btr), "brainrec.btr"));
    UT_ASSERT(utScratchPath(h5, sizeof(h5), "bot2_g5.hbr"));
    UT_ASSERT(utScratchPath(h9, sizeof(h9), "bot2_g9.hbr"));
    UT_ASSERT(utScratchPath(out, sizeof(out), "merged.btr"));

    /* Frames 0-4: slot 2 on the server. 5-9: on the host, gen 5 (its
     * input sequence starts again from 2). 10-14: back on the server.
     * 15-19: on the host again, gen 9, a new connection from 2. */
    f = tbrOpenBtr(btr);
    UT_ASSERT(f != NULL);
    for (i = 0; i < 20; i++) {
        TbrBot bot;
        memset(&bot, 0, sizeof(bot));
        bot.slot = 2;
        if ((i >= 5 && i < 10) || i >= 15) {
            bot.hosted = true;
            bot.gen = (uint16_t)(i < 10 ? 5 : 9);
            bot.lpi = 2u + 2u * (uint32_t)(i < 10 ? i - 5 : i - 15);
        } else {
            bot.thinkMs = 500.0f + (float)i;
        }
        UT_ASSERT(tbrFrame(f, 200u + 2u * (uint32_t)i, &bot, 1));
    }
    UT_ASSERT(brainRecOutClose(f));
    UT_ASSERT(tbrHostFile(h5, 2, 5, 2, 10));
    UT_ASSERT(tbrHostFile(h9, 2, 9, 2, 10));

    /* The order of the files does not matter: gen picks the file. */
    files[0] = h9;
    files[1] = h5;
    UT_ASSERT_MSG(brainRecMergeFiles(btr, files, 2, out, &st, err, sizeof(err)),
                  "merge failed: %s", err);
    UT_ASSERT(st.markers == 10);
    UT_ASSERT(st.hostFiles == 2);
    UT_ASSERT_MSG(st.filled == 10 && st.missing == 0, "filled %u missing %u",
                  (unsigned)st.filled, (unsigned)st.missing);
    UT_ASSERT(tbrRead(out, &o));
    UT_ASSERT(o.frames == 20);
    for (i = 0; i < 20; i++) {
        UT_ASSERT(o.bots[i] == 1 && o.slot[i][0] == 2);
        UT_ASSERT(!o.hosted[i][0]);
        if ((i >= 5 && i < 10) || i >= 15) {
            float want = (float)(2 + 2 * (i < 10 ? i - 5 : i - 15));
            UT_ASSERT_MSG(o.thinkMs[i][0] == want, "frame %d: %f want %f", i,
                          (double)o.thinkMs[i][0], (double)want);
        } else {
            UT_ASSERT(o.thinkMs[i][0] == 500.0f + (float)i);
        }
    }
    return 0;
}

/* ================================================================
 * Andrew's first game with a Bot Host (Nov 4): the host was on another
 * BRAINREC_VERSION than the server, and the merge read none of its files.
 * Two bots on the host (gen 1: assigned in the lobby, built again before
 * the round, no new gen), one on the server. Slot 0's brain is built
 * again inside the block, so its records are in two parts.
 * ================================================================ */

/* tbrHostFile, with the header's brainrecVersion set to ver. */
static bool tbrHostFileV(const char *path, uint8_t slot, uint16_t gen,
                         uint32_t first, uint32_t last, uint32_t ver) {
    HostRecWriter w;
    HostRecHeader hdr;
    BrainRecBuf b;
    const char *legend = "{\"cats\":[\"host\"]}";
    uint32_t ll = (uint32_t)strlen(legend);
    uint32_t s;
    bool ok;
    BrainRecOut *f = brainRecOutOpen(path);
    if (f == NULL) return false;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, HOSTREC_MAGIC, sizeof(HOSTREC_MAGIC));
    hdr.version = HOSTREC_VERSION;
    hdr.brainrecVersion = ver;
    SDL_strlcpy(hdr.hostName, "testhost", sizeof(hdr.hostName));
    SDL_strlcpy(hdr.block, "2026-11-04_212558", sizeof(hdr.block));
    hdr.slot = slot;
    hdr.placeGen = gen;
    ok = tbrWrite(f, &hdr, sizeof(hdr)) && tbrWrite(f, &ll, 4) &&
         tbrWrite(f, legend, ll);
    memset(&w, 0, sizeof(w));
    w.file = f;
    brainRecBufInit(&b);
    for (s = first; ok && s <= last; s += 2) {
        brainRecBufReset(&b);
        (void)brainRecBodyEncode(&b, (float)s, NULL, NULL, NULL,
                                 (s % 4u) == 0 ? "{\"pool\":1}" : NULL);
        ok = !b.failed && hostRecWriterAppend(&w, s, 1000u + s / 2u, b.data,
                                              b.len);
    }
    brainRecBufFree(&b);
    hostRecWriterClose(&w);
    return ok;
}

int run_brainrec_host_merge_other_version_rebuilt_brain(void) {
    char btr[1024], h0a[1024], h0b[1024], h0bad[1024], h1[1024], out[1024];
    char name[64];
    const char *files[4];
    BrainRecOut *f;
    BrainRecMergeStats st;
    TbrOut o;
    char err[256];
    int i;

    /* The part names the host writes when a brain is built again. */
    hostRecPartFileName(name, sizeof(name), 0, 1, 0);
    UT_ASSERT_MSG(strcmp(name, "bot0_g1.hbr") == 0, "part 0: %s", name);
    hostRecPartFileName(name, sizeof(name), 0, 1, 1);
    UT_ASSERT_MSG(strcmp(name, "bot0_g1_p1.hbr") == 0, "part 1: %s", name);
    hostRecFileName(name, sizeof(name), 0, 1);
    UT_ASSERT(strcmp(name, "bot0_g1.hbr") == 0);

    UT_ASSERT(utScratchPath(btr, sizeof(btr), "brainrec.btr"));
    UT_ASSERT(utScratchPath(h0a, sizeof(h0a), "bot0_g1.hbr"));
    UT_ASSERT(utScratchPath(h0b, sizeof(h0b), "bot0_g1_p1.hbr"));
    UT_ASSERT(utScratchPath(h0bad, sizeof(h0bad), "bot0_g1_p2.hbr"));
    UT_ASSERT(utScratchPath(h1, sizeof(h1), "bot1_g1.hbr"));
    UT_ASSERT(utScratchPath(out, sizeof(out), "merged.btr"));

    /* 20 frames. Slots 0 and 1 are hosted markers of gen 1 whose last
     * input goes 0, 2, ... 38; slot 2 runs on the server. */
    f = tbrOpenBtr(btr);
    UT_ASSERT(f != NULL);
    for (i = 0; i < 20; i++) {
        TbrBot bots[3];
        memset(bots, 0, sizeof(bots));
        bots[0].slot = 0;
        bots[0].hosted = true;
        bots[0].gen = 1;
        bots[0].lpi = 2u * (uint32_t)i;
        bots[1].slot = 1;
        bots[1].hosted = true;
        bots[1].gen = 1;
        bots[1].lpi = 2u * (uint32_t)i;
        bots[2].slot = 2;
        bots[2].thinkMs = 3.0f;
        UT_ASSERT(tbrFrame(f, 50u + 2u * (uint32_t)i, bots, 3));
    }
    UT_ASSERT(brainRecOutClose(f));

    /* Slot 0: an older brainrec version than the server's. Its brain is
     * built again after input 18; the new part starts at 22. Slot 1: a
     * newer version. A file below BRAINREC_VERSION_MIN_READ is skipped. */
    UT_ASSERT(tbrHostFileV(h0a, 0, 1, 0, 18, BRAINREC_VERSION - 1u));
    UT_ASSERT(tbrHostFileV(h0b, 0, 1, 22, 38, BRAINREC_VERSION - 1u));
    UT_ASSERT(tbrHostFileV(h1, 1, 1, 0, 38, BRAINREC_VERSION + 1u));
    UT_ASSERT(tbrHostFileV(h0bad, 0, 1, 0, 38, BRAINREC_VERSION_MIN_READ - 1u));

    /* The new part first: the order of the files does not matter. */
    files[0] = h0b;
    files[1] = h1;
    files[2] = h0bad;
    files[3] = h0a;
    err[0] = '\0';
    UT_ASSERT_MSG(brainRecMergeFiles(btr, files, 4, out, &st, err, sizeof(err)),
                  "merge failed: %s", err);
    UT_ASSERT(st.frames == 20);
    UT_ASSERT(st.markers == 40);
    UT_ASSERT_MSG(st.hostFiles == 3 && st.hostFilesSkipped == 1,
                  "used %u skipped %u", (unsigned)st.hostFiles,
                  (unsigned)st.hostFilesSkipped);
    UT_ASSERT_MSG(st.filled == 40 && st.stale == 0 && st.missing == 0,
                  "filled %u stale %u missing %u", (unsigned)st.filled,
                  (unsigned)st.stale, (unsigned)st.missing);

    UT_ASSERT(tbrRead(out, &o));
    UT_ASSERT(o.frames == 20);
    for (i = 0; i < 20; i++) {
        uint32_t lpi = 2u * (uint32_t)i;
        /* Input 20 has no record on slot 0: the new part's first record
         * (22) is one think ahead and fills it. */
        float want0 = (float)(lpi == 20u ? 22u : lpi);
        UT_ASSERT(o.bots[i] == 3);
        UT_ASSERT(o.slot[i][0] == 0 && o.slot[i][1] == 1 && o.slot[i][2] == 2);
        UT_ASSERT_MSG(!o.hosted[i][0] && !o.hosted[i][1],
                      "frame %d still a marker", i);
        UT_ASSERT_MSG(o.thinkMs[i][0] == want0, "frame %d slot 0: %f want %f",
                      i, (double)o.thinkMs[i][0], (double)want0);
        UT_ASSERT_MSG(o.thinkMs[i][1] == (float)lpi, "frame %d slot 1: %f", i,
                      (double)o.thinkMs[i][1]);
        UT_ASSERT(!o.hosted[i][2] && o.thinkMs[i][2] == 3.0f);
        /* The pool JSON of the host comes through (every second record
         * has one). */
        if ((lpi % 4u) == 0) {
            UT_ASSERT_MSG(o.poolLen[i][1] > 0, "frame %d slot 1: no pool", i);
            if (lpi != 20u) {
                UT_ASSERT_MSG(o.poolLen[i][0] > 0, "frame %d slot 0: no pool",
                              i);
            }
        }
    }
    return 0;
}

/* ================================================================
 * No host data: the markers stay and the file still walks.
 * ================================================================ */
int run_brainrec_host_merge_missing_block(void) {
    char btr[1024], hWrong[1024], out[1024];
    const char *files[2];
    BrainRecOut *f;
    BrainRecMergeStats st;
    TbrOut o;
    char err[256];
    int i;

    UT_ASSERT(utScratchPath(btr, sizeof(btr), "brainrec.btr"));
    UT_ASSERT(utScratchPath(hWrong, sizeof(hWrong), "bot3_g4.hbr"));
    UT_ASSERT(utScratchPath(out, sizeof(out), "merged.btr"));

    f = tbrOpenBtr(btr);
    UT_ASSERT(f != NULL);
    for (i = 0; i < 6; i++) {
        TbrBot bots[2];
        memset(bots, 0, sizeof(bots));
        bots[0].slot = 3;
        bots[0].hosted = true;
        bots[0].gen = 6;
        bots[0].lpi = 20u + 2u * (uint32_t)i;
        bots[1].slot = 4;
        bots[1].thinkMs = 2.0f;
        UT_ASSERT(tbrFrame(f, 300u + 2u * (uint32_t)i, bots, 2));
    }
    UT_ASSERT(brainRecOutClose(f));

    /* No files at all. */
    UT_ASSERT_MSG(brainRecMergeFiles(btr, NULL, 0, out, &st, err, sizeof(err)),
                  "merge failed: %s", err);
    UT_ASSERT(st.markers == 6 && st.missing == 6 && st.filled == 0);
    UT_ASSERT(tbrRead(out, &o));
    UT_ASSERT(o.frames == 6);
    for (i = 0; i < 6; i++) {
        UT_ASSERT(o.hosted[i][0]);
        UT_ASSERT(!o.hosted[i][1] && o.thinkMs[i][1] == 2.0f);
    }

    /* A file of another placement (gen 4) and one that is not a host
     * file: neither fills gen 6. */
    UT_ASSERT(tbrHostFile(hWrong, 3, 4, 20, 40));
    files[0] = hWrong;
    files[1] = btr;   /* not an .hbr: skipped */
    UT_ASSERT_MSG(brainRecMergeFiles(btr, files, 2, out, &st, err, sizeof(err)),
                  "merge failed: %s", err);
    UT_ASSERT(st.hostFiles == 1);
    UT_ASSERT(st.missing == 6 && st.filled == 0 && st.stale == 0);

    /* A record too far behind the marker does not fill it. */
    UT_ASSERT(tbrHostFile(hWrong, 3, 6, 2, 2));
    files[0] = hWrong;
    UT_ASSERT_MSG(brainRecMergeFiles(btr, files, 1, out, &st, err, sizeof(err)),
                  "merge failed: %s", err);
    UT_ASSERT_MSG(st.missing == 6, "missing %u", (unsigned)st.missing);
    return 0;
}

/* ================================================================
 * A host's gzipped print2 log that has the name of the server's plain one
 * is merged by tick into the server's file, which stays plain.
 * ================================================================ */
static bool tbrWriteText(const char *path, const char *text, bool gz) {
    size_t n = strlen(text);
    if (gz) {
        gzFile g = gzopen(path, "wb6");
        bool ok;
        if (g == NULL) return false;
        ok = gzwrite(g, text, (unsigned)n) == (int)n;
        return gzclose(g) == Z_OK && ok;
    } else {
        FILE *f = fopen(path, "wb");
        bool ok;
        if (f == NULL) return false;
        ok = fwrite(text, 1, n, f) == n;
        return fclose(f) == 0 && ok;
    }
}

static bool tbrReadText(const char *path, char *out, size_t sz) {
    gzFile g = gzopen(path, "rb");
    int n;
    if (g == NULL) return false;
    n = gzread(g, out, (unsigned)(sz - 1));
    gzclose(g);
    if (n < 0) return false;
    out[n] = '\0';
    return true;
}

int run_brainrec_host_print2_gz(void) {
    char sess[1024], host[1024], a[1100], b[1100], other[1100], text[512];
    SDL_PathInfo pi;

    UT_ASSERT(utScratchPath(sess, sizeof(sess), "p2sess"));
    UT_ASSERT(utScratchPath(host, sizeof(host), "p2host"));
    UT_ASSERT(SDL_CreateDirectory(sess));
    UT_ASSERT(SDL_CreateDirectory(host));
    snprintf(a, sizeof(a), "%s/print2_bot1.log", sess);
    snprintf(b, sizeof(b), "%s/print2_bot1.log.gz", host);
    snprintf(other, sizeof(other), "%s/print2_bot0.log.gz", host);
    UT_ASSERT(tbrWriteText(a, "head s\n===TICK 10===\ns10\n===TICK 30===\ns30\n", false));
    UT_ASSERT(tbrWriteText(b, "===TICK 20===\nh20\n===TICK 40===\nh40\n", true));
    UT_ASSERT(tbrWriteText(other, "===TICK 5===\nh5\n", true));

    UT_ASSERT(brainRecMergeDir(sess, host, "h", NULL, NULL, NULL));

    /* The clash: one plain file, the ticks in order, no copy beside it. */
    UT_ASSERT(tbrReadText(a, text, sizeof(text)));
    UT_ASSERT(strcmp(text, "head s\n===TICK 10===\ns10\n===TICK 20===\nh20\n"
                           "===TICK 30===\ns30\n===TICK 40===\nh40\n") == 0);
    snprintf(b, sizeof(b), "%s/print2_bot1.log.gz", sess);
    UT_ASSERT(!SDL_GetPathInfo(b, &pi));
    snprintf(b, sizeof(b), "%s/bothost_h_print2_bot1.log.gz", sess);
    UT_ASSERT(!SDL_GetPathInfo(b, &pi));
    /* No clash: the .gz file moves in as it is. */
    snprintf(other, sizeof(other), "%s/print2_bot0.log.gz", sess);
    UT_ASSERT(tbrReadText(other, text, sizeof(text)));
    UT_ASSERT(strcmp(text, "===TICK 5===\nh5\n") == 0);
    /* The host's copy of the merged log is kept as it came. */
    snprintf(b, sizeof(b), "%s/%s/h_print2_bot1.log.gz", sess,
             BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(tbrReadText(b, text, sizeof(text)));
    UT_ASSERT(strcmp(text, "===TICK 20===\nh20\n===TICK 40===\nh40\n") == 0);
    /* The host folder is gone after a full merge. */
    UT_ASSERT(!SDL_GetPathInfo(host, &pi));
    return 0;
}

/* ================================================================
 * The folder lock: a log compression of a folder waits while a merge
 * holds it, and a merge waits while a compression holds it.
 * ================================================================ */
typedef struct {
    const char   *dir;
    SDL_AtomicInt started;
    int           count;
} TbrGz;

static int SDLCALL tbrGzThread(void *arg) {
    TbrGz *g = (TbrGz *)arg;
    SDL_SetAtomicInt(&g->started, 1);
    g->count = brainRecordCompressLogsNow(g->dir);
    return 0;
}

static bool tbrExists(const char *path) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi);
}

int run_brainrec_dir_lock_orders_merge_and_gzip(void) {
    char sess[1024], other[1024], plain[1100], gz[1100];
    TbrGz g;
    SDL_Thread *th;
    int i;

    UT_ASSERT(utScratchPath(sess, sizeof(sess), "lockblock"));
    UT_ASSERT(utScratchPath(other, sizeof(other), "lockother"));
    UT_ASSERT(SDL_CreateDirectory(sess));
    UT_ASSERT(SDL_CreateDirectory(other));
    snprintf(plain, sizeof(plain), "%s/print2_bot0.log", sess);
    snprintf(gz, sizeof(gz), "%s/print2_bot0.log.gz", sess);
    (void)SDL_RemovePath(gz);
    UT_ASSERT(tbrWriteText(plain, "===TICK 1===\nx\n", false));

    /* A merge holds the folder (by another spelling of its path). */
    {
        char alias[1100];
        snprintf(alias, sizeof(alias), "%s/../lockblock/", sess);
        UT_ASSERT(brainRecordDirLock(alias));
    }
    /* Another folder is not held up. */
    UT_ASSERT(brainRecordCompressLogsNow(other) == 0);

    memset(&g, 0, sizeof(g));
    g.dir = sess;
    g.count = -1;
    th = SDL_CreateThread(tbrGzThread, "tbr_gz", &g);
    UT_ASSERT(th != NULL);
    for (i = 0; i < 200 && SDL_GetAtomicInt(&g.started) == 0; i++) SDL_Delay(5);
    SDL_Delay(300);
    /* The compression waits: the plain log is still there, no .gz. */
    UT_ASSERT(g.count == -1);
    UT_ASSERT(tbrExists(plain));
    UT_ASSERT(!tbrExists(gz));
    brainRecordDirUnlock(sess);
    SDL_WaitThread(th, NULL);
    UT_ASSERT(g.count == 1);
    UT_ASSERT(!tbrExists(plain));
    UT_ASSERT(tbrExists(gz));

    /* Pending: a block left for the next one to open is compressed by the
     * recorder, so a merge leaves it alone. */
    UT_ASSERT(!brainRecordLogCompressPending(sess));
    brainRecordDeferLogCompress(sess);
    UT_ASSERT(brainRecordLogCompressPending("debug_sessions/lockblock"));
    brainRecordFinishLogCompress();
    UT_ASSERT(!brainRecordLogCompressPending(sess));
    return 0;
}

/* ================================================================
 * A relative --cachedir becomes a full path.
 * ================================================================ */
int run_brainrec_host_make_absolute(void) {
    char p[512];
    char *cwd;

    SDL_strlcpy(p, "D:\\a\\.\\b\\..\\c\\", sizeof(p));
    UT_ASSERT(brainRecMakeAbsolute(p, sizeof(p)));
    UT_ASSERT(strcmp(p, "D:/a/c") == 0);
    SDL_strlcpy(p, "/x/y/..", sizeof(p));
    UT_ASSERT(brainRecMakeAbsolute(p, sizeof(p)));
    UT_ASSERT(strcmp(p, "/x") == 0);
    SDL_strlcpy(p, "/..", sizeof(p));
    UT_ASSERT(brainRecMakeAbsolute(p, sizeof(p)));
    UT_ASSERT(strcmp(p, "/") == 0);

    SDL_strlcpy(p, "./rec/cache", sizeof(p));
    UT_ASSERT(brainRecMakeAbsolute(p, sizeof(p)));
    cwd = SDL_GetCurrentDirectory();
    UT_ASSERT(cwd != NULL);
    {
        char want[512];
        char *c;
        SDL_snprintf(want, sizeof(want), "%srec/cache", cwd);
        for (c = want; *c != '\0'; c++) {
            if (*c == '\\') *c = '/';
        }
        SDL_free(cwd);
        UT_ASSERT(strcmp(p, want) == 0);
    }
    UT_ASSERT(!brainRecMakeAbsolute(p, 4));
    return 0;
}

/* ================================================================
 * Names and paths a peer sends, and the delete that cleardebug and a sent
 * block use.
 * ================================================================ */
int run_brainrec_host_path_checks(void) {
    char root[1024], sub[1024], file[1024], out[64];
    uint64_t bytes = 0;
    uint32_t files = 0;
    FILE *fp;

    /* Names. */
    UT_ASSERT(brainRecNameOk("2026-11-04_120000", false));
    UT_ASSERT(brainRecNameOk("bot3_g12.hbr", true));
    UT_ASSERT(!brainRecNameOk("bot3_g12.hbr", false));
    UT_ASSERT(!brainRecNameOk("", true));
    UT_ASSERT(!brainRecNameOk(".", true));
    UT_ASSERT(!brainRecNameOk("..", true));
    UT_ASSERT(!brainRecNameOk(".hidden", true));
    UT_ASSERT(!brainRecNameOk("a/b", true));
    UT_ASSERT(!brainRecNameOk("a\\b", true));
    UT_ASSERT(!brainRecNameOk("C:", true));
    UT_ASSERT(!brainRecNameOk("a b", true));

    brainRecSafeName("127.0.0.1:27650", out, sizeof(out), true);
    UT_ASSERT_MSG(strcmp(out, "127.0.0.1_27650") == 0, "got %s", out);
    UT_ASSERT(brainRecNameOk(out, true));
    brainRecSafeName("..", out, sizeof(out), true);
    UT_ASSERT(brainRecNameOk(out, true));
    brainRecSafeName("", out, sizeof(out), false);
    UT_ASSERT(brainRecNameOk(out, false));
    brainRecSafeName("andrew-bsmt/../x", out, sizeof(out), false);
    UT_ASSERT(brainRecNameOk(out, false));

    /* Paths. */
    UT_ASSERT(brainRecPathUnder("D:/c/recordings", "D:/c/recordings/a"));
    UT_ASSERT(brainRecPathUnder("D:/c/recordings", "D:/c/recordings/a/b.hbr"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c/recordings"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c/recordings/"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c/recordingsX/a"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c/recordings/../x"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c/recordings/a/../../x"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c/recordings/./a"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c/recordings//a"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "D:/c"));
    UT_ASSERT(!brainRecPathUnder("D:/c/recordings", "E:/c/recordings/a"));

    /* The delete keeps to its root. */
    UT_ASSERT(utScratchPath(root, sizeof(root), "recordings"));
    UT_ASSERT(SDL_CreateDirectory(root));
    SDL_snprintf(sub, sizeof(sub), "%s/srv/blk", root);
    UT_ASSERT(SDL_CreateDirectory(sub));
    SDL_snprintf(file, sizeof(file), "%s/bot1_g2.hbr", sub);
    fp = fopen(file, "wb");
    UT_ASSERT(fp != NULL);
    fwrite("12345", 1, 5, fp);
    fclose(fp);

    /* Outside the root, or the root itself: refused, nothing deleted. */
    UT_ASSERT(!brainRecRemoveTree(sub, root, &bytes, &files));
    UT_ASSERT(!brainRecRemoveTree(root, root, &bytes, &files));
    {
        char up[1100];
        SDL_snprintf(up, sizeof(up), "%s/srv/../srv", root);
        UT_ASSERT(!brainRecRemoveTree(root, up, &bytes, &files));
    }
    {
        SDL_PathInfo pi;
        UT_ASSERT(SDL_GetPathInfo(file, &pi) && pi.type == SDL_PATHTYPE_FILE);
    }
    /* Inside: deleted, with the bytes counted. */
    {
        char srv[1100];
        SDL_snprintf(srv, sizeof(srv), "%s/srv", root);
        bytes = 0;
        files = 0;
        UT_ASSERT(brainRecRemoveTree(root, srv, &bytes, &files));
        UT_ASSERT_MSG(bytes == 5 && files == 1, "bytes %llu files %u",
                      (unsigned long long)bytes, (unsigned)files);
        UT_ASSERT(!SDL_GetPathInfo(srv, NULL));
        /* Gone already: true, nothing deleted. */
        UT_ASSERT(brainRecRemoveTree(root, srv, &bytes, &files));
        UT_ASSERT(bytes == 0 && files == 0);
    }
    return 0;
}

/* ================================================================
 * CTRL_BOT_HOST_REC and the recording fields of CTRL_BOT_HOST_STATUS.
 * ================================================================ */
int run_brainrec_host_rec_codec(void) {
    ControlEvent in, out;
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t len = 0;
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_REC);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_REC);

    UT_ASSERT(enc != NULL && dec != NULL);
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_REC;
    in.u.botHostRec.op = BOT_HOST_REC_OP_STATE;
    in.u.botHostRec.flags = BOT_HOST_REC_F_LOWDISK;
    in.u.botHostRec.state = BOT_HOST_REC_STATE_UPLOADING;
    in.u.botHostRec.percent = 42;
    in.u.botHostRec.botsDone = 1;
    in.u.botHostRec.botsTotal = 2;
    in.u.botHostRec.a = 0x01020304u;
    in.u.botHostRec.b = 0xA0B0C0D0u;
    SDL_strlcpy(in.u.botHostRec.block, "2026-11-04_120000",
                sizeof(in.u.botHostRec.block));
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.u.botHostRec.op == BOT_HOST_REC_OP_STATE);
    UT_ASSERT(out.u.botHostRec.flags == BOT_HOST_REC_F_LOWDISK);
    UT_ASSERT(out.u.botHostRec.state == BOT_HOST_REC_STATE_UPLOADING);
    UT_ASSERT(out.u.botHostRec.percent == 42);
    UT_ASSERT(out.u.botHostRec.botsDone == 1);
    UT_ASSERT(out.u.botHostRec.botsTotal == 2);
    UT_ASSERT(out.u.botHostRec.a == 0x01020304u);
    UT_ASSERT(out.u.botHostRec.b == 0xA0B0C0D0u);
    UT_ASSERT(strcmp(out.u.botHostRec.block, "2026-11-04_120000") == 0);
    UT_ASSERT(!dec(buf, len - 1, &out));

    /* A block name that is not one path part is refused. */
    SDL_strlcpy(in.u.botHostRec.block, "../x", sizeof(in.u.botHostRec.block));
    if (enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK) {
        UT_ASSERT(!dec(buf, len, &out));
    }

    /* The lobby row: progress and bot counts. */
    enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_STATUS);
    dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_STATUS);
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_STATUS;
    in.u.botHostStatus.idx = 1;
    in.u.botHostStatus.connected = 1;
    in.u.botHostStatus.state = BOT_HOST_SYNC_READY;
    in.u.botHostStatus.used = 2;
    in.u.botHostStatus.slots = 2;
    in.u.botHostStatus.seats = (uint16_t)((1u << 2) | (1u << 5));
    in.u.botHostStatus.recState = BOT_HOST_REC_STATE_UPLOADING;
    in.u.botHostStatus.recPercent = 42;
    in.u.botHostStatus.recBotsDone = 1;
    in.u.botHostStatus.recBots = 2;
    SDL_strlcpy(in.u.botHostStatus.name, "andrew-bsmt",
                sizeof(in.u.botHostStatus.name));
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.u.botHostStatus.recState == BOT_HOST_REC_STATE_UPLOADING);
    UT_ASSERT(out.u.botHostStatus.recPercent == 42);
    UT_ASSERT(out.u.botHostStatus.recBotsDone == 1);
    UT_ASSERT(out.u.botHostStatus.recBots == 2);
    /* More bots done than to send is refused. */
    in.u.botHostStatus.recBotsDone = 3;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(!dec(buf, len, &out));
    return 0;
}

/* ================================================================
 * The server keeps every file a host uploaded, as it came, in
 * <session>/bothost_raw/, and answers MERGED only when every host file
 * was read and filled entries.
 * ================================================================ */

/* A session folder whose brainrec.btr has 6 frames: slot 1 hosted (gen 5,
 * inputs 20 to 30), slot 0 local. */
static bool tbrSession(const char *dir) {
    char btr[1100];
    BrainRecOut *f;
    int i;
    if (!SDL_CreateDirectory(dir)) return false;
    snprintf(btr, sizeof(btr), "%s/%s", dir, BRAINREC_FILENAME);
    f = tbrOpenBtr(btr);
    if (f == NULL) return false;
    for (i = 0; i < 6; i++) {
        TbrBot bots[2];
        memset(bots, 0, sizeof(bots));
        bots[0].slot = 1;
        bots[0].hosted = true;
        bots[0].gen = 5;
        bots[0].lpi = 20u + 2u * (uint32_t)i;
        bots[1].slot = 0;
        bots[1].thinkMs = 1.0f;
        if (!tbrFrame(f, 200u + 2u * (uint32_t)i, bots, 2)) {
            (void)brainRecOutClose(f);
            return false;
        }
    }
    return brainRecOutClose(f);
}

int run_brainrec_host_merge_dir_keeps_raw(void) {
    char sess[1024], host[1024], p[1200];
    BrainRecMergeStats st;
    HostRecReader r;
    TbrOut o;
    int i;

    UT_ASSERT(utScratchPath(sess, sizeof(sess), "rawsess"));
    UT_ASSERT(utScratchPath(host, sizeof(host), "rawhost"));
    UT_ASSERT(tbrSession(sess));
    UT_ASSERT(SDL_CreateDirectory(host));
    snprintf(p, sizeof(p), "%s/bot1_g5.hbr", host);
    UT_ASSERT(tbrHostFile(p, 1, 5, 20, 30));
    snprintf(p, sizeof(p), "%s/print2_bot1.log", sess);
    UT_ASSERT(tbrWriteText(p, "===TICK 10===\ns10\n", false));
    snprintf(p, sizeof(p), "%s/print2_bot1.log.gz", host);
    UT_ASSERT(tbrWriteText(p, "===TICK 20===\nh20\n", true));

    UT_ASSERT(brainRecMergeDir(sess, host, "h", &st, NULL, NULL));
    UT_ASSERT_MSG(st.missing == 0 && st.hostFiles == 1, "missing %u files %u",
                  (unsigned)st.missing, (unsigned)st.hostFiles);
    UT_ASSERT(st.hostFilesUnused == 0 && st.btrMissing == 0);
    UT_ASSERT(brainRecMergeVerdict(true, &st) == BOT_HOST_REC_FAIL_NONE);

    /* The merge used the host file ... */
    snprintf(p, sizeof(p), "%s/%s", sess, BRAINREC_FILENAME);
    UT_ASSERT(tbrRead(p, &o));
    UT_ASSERT(o.frames == 6);
    for (i = 0; i < 6; i++) UT_ASSERT(!o.hosted[i][0]);
    /* ... and kept it as it came, with the host's print2 log. */
    snprintf(p, sizeof(p), "%s/%s/h_bot1_g5.hbr", sess, BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(hostRecReaderOpen(&r, p));
    UT_ASSERT(r.hdr.slot == 1 && r.hdr.placeGen == 5);
    hostRecReaderClose(&r);
    snprintf(p, sizeof(p), "%s/%s/h_print2_bot1.log.gz", sess,
             BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(tbrExists(p));
    /* Not beside the session's files, and the staging folder is gone. */
    snprintf(p, sizeof(p), "%s/bothost_h_bot1_g5.hbr", sess);
    UT_ASSERT(!tbrExists(p));
    UT_ASSERT(!tbrExists(host));
    return 0;
}

int run_brainrec_host_merge_verdict(void) {
    char sess[1024], host[1024], p[1200];
    BrainRecMergeStats st;
    uint8_t r;

    /* A host file this server cannot read: UNREADABLE, still kept. */
    UT_ASSERT(utScratchPath(sess, sizeof(sess), "vsessA"));
    UT_ASSERT(utScratchPath(host, sizeof(host), "vhostA"));
    UT_ASSERT(tbrSession(sess));
    UT_ASSERT(SDL_CreateDirectory(host));
    snprintf(p, sizeof(p), "%s/bot1_g5.hbr", host);
    UT_ASSERT(tbrHostFile(p, 1, 5, 20, 30));
    snprintf(p, sizeof(p), "%s/bot2_g1.hbr", host);
    UT_ASSERT(tbrHostFileV(p, 2, 1, 20, 30, BRAINREC_VERSION_MIN_READ - 1u));
    UT_ASSERT(brainRecMergeDir(sess, host, "h", &st, NULL, NULL));
    UT_ASSERT(st.hostFilesSkipped == 1);
    r = brainRecMergeVerdict(true, &st);
    UT_ASSERT_MSG(r == BOT_HOST_REC_FAIL_UNREADABLE, "reason %u", (unsigned)r);
    snprintf(p, sizeof(p), "%s/%s/h_bot2_g1.hbr", sess, BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(tbrExists(p));

    /* A host file of another placement fills nothing: NO_FILL. */
    UT_ASSERT(utScratchPath(sess, sizeof(sess), "vsessB"));
    UT_ASSERT(utScratchPath(host, sizeof(host), "vhostB"));
    UT_ASSERT(tbrSession(sess));
    UT_ASSERT(SDL_CreateDirectory(host));
    snprintf(p, sizeof(p), "%s/bot1_g4.hbr", host);
    UT_ASSERT(tbrHostFile(p, 1, 4, 20, 30));
    UT_ASSERT(brainRecMergeDir(sess, host, "h", &st, NULL, NULL));
    UT_ASSERT(st.hostFiles == 1 && st.hostFilesUnused == 1);
    UT_ASSERT(st.missing == 6);
    r = brainRecMergeVerdict(true, &st);
    UT_ASSERT_MSG(r == BOT_HOST_REC_FAIL_NO_FILL, "reason %u", (unsigned)r);
    snprintf(p, sizeof(p), "%s/%s/h_bot1_g4.hbr", sess, BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(tbrExists(p));

    /* A session with no brainrec.btr: NO_BTR, the host file kept. */
    UT_ASSERT(utScratchPath(sess, sizeof(sess), "vsessC"));
    UT_ASSERT(utScratchPath(host, sizeof(host), "vhostC"));
    UT_ASSERT(SDL_CreateDirectory(sess));
    UT_ASSERT(SDL_CreateDirectory(host));
    snprintf(p, sizeof(p), "%s/bot1_g5.hbr", host);
    UT_ASSERT(tbrHostFile(p, 1, 5, 20, 30));
    (void)brainRecMergeDir(sess, host, "h", &st, NULL, NULL);
    UT_ASSERT(st.btrMissing == 1);
    r = brainRecMergeVerdict(true, &st);
    UT_ASSERT_MSG(r == BOT_HOST_REC_FAIL_NO_BTR, "reason %u", (unsigned)r);
    snprintf(p, sizeof(p), "%s/%s/h_bot1_g5.hbr", sess, BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(tbrExists(p));

    /* A merge that stopped is never MERGED. */
    memset(&st, 0, sizeof(st));
    UT_ASSERT(brainRecMergeVerdict(true, &st) == BOT_HOST_REC_FAIL_NONE);
    UT_ASSERT(brainRecMergeVerdict(false, &st) == BOT_HOST_REC_FAIL_MERGE);
    UT_ASSERT(brainRecMergeVerdict(true, NULL) == BOT_HOST_REC_FAIL_MERGE);
    /* Each reason has its own text. */
    for (r = BOT_HOST_REC_FAIL_NONE; r <= BOT_HOST_REC_FAIL_NO_FILL; r++) {
        const char *t = brainRecMergeFailText(r);
        UT_ASSERT(t != NULL && t[0] != '\0');
        UT_ASSERT(strcmp(t, "unknown reason") != 0);
    }
    UT_ASSERT(strcmp(brainRecMergeFailText(99), "unknown reason") == 0);
    return 0;
}

/* A slot legend from an uploaded file that is not a closed object (no '}')
 * leaves the global legend as it was; a good one still goes in. */
int run_brainrec_host_legend_unclosed(void) {
    char *r;

    r = brainRecLegendAddSlot("{}", 2, "{\"a\":1");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(strcmp(r, "{}") == 0, "got %s", r);
    free(r);

    r = brainRecLegendAddSlot("{\"x\":1}", 2, "{");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(strcmp(r, "{\"x\":1}") == 0, "got %s", r);
    free(r);

    /* A global legend with no closing '}' is kept as it is. */
    r = brainRecLegendAddSlot("{\"x\":1", 2, "{\"a\":1}");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(strcmp(r, "{\"x\":1") == 0, "got %s", r);
    free(r);

    r = brainRecLegendAddSlot("{}", 2, "{\"a\":1}");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(strcmp(r, "{\"a\":1,\"slots\":{\"2\":{\"a\":1}}}") == 0, "got %s", r);
    free(r);
    return 0;
}

/* A host's recording piece for a block this server has no finished
 * session folder for writes nothing: no staging folder beside it, no
 * file, no bytes counted. Its DONE is answered NO_SESSION. */
int run_brainrec_host_upload_no_session(void) {
    static BotHostConn saved;
    BotHostConn *hc = &udpServer.botHosts[0];
    BulkStreamHeader hd;
    uint8_t data[16];
    bool noSession, wrote;
    uint64_t bytes;

    saved = *hc;
    memset(hc, 0, sizeof(*hc));
    SDL_strlcpy(hc->name, "ut652host", sizeof(hc->name));

    memset(&hd, 0, sizeof(hd));
    memset(data, 0x5A, sizeof(data));
    hd.kind = BULK_KIND_REC_CHUNK;
    hd.gen = sizeof(data);
    hd.totalSize = sizeof(data);
    SDL_strlcpy(hd.path, "ut652_no_such_block/bot1_g1.hbr/0", sizeof(hd.path));
    hd.pathLen = (uint8_t)strlen(hd.path);
    serverBotRecPiece(0, &hd, data);

    noSession = hc->recNoSession;
    wrote = hc->recFile != NULL || hc->recStage[0] != '\0';
    bytes = hc->recBytes;
    if (hc->recFile != NULL) fclose(hc->recFile);
    *hc = saved;

    UT_ASSERT(noSession);
    UT_ASSERT(!wrote);
    UT_ASSERT(bytes == 0);
    UT_ASSERT(!tbrExists("debug_sessions/ut652_no_such_block_bothost_ut652host"));
    UT_ASSERT(!tbrExists("debug_sessions/ut652_no_such_block"));
    return 0;
}

/* An uploaded file that is not a .hbr and not a print2_bot log is not
 * taken into the session folder: a brainrec.btr the session lacks stays
 * out, as does any other name. Each is kept as a raw copy in bothost_raw/
 * instead. The print2_bot logs still move in, and the staging folder is
 * gone after the merge. */
int run_brainrec_host_merge_dir_other_names(void) {
    char sess[1024], host[1024], p[1200];
    BrainRecMergeStats st;

    UT_ASSERT(utScratchPath(sess, sizeof(sess), "othersess"));
    UT_ASSERT(utScratchPath(host, sizeof(host), "otherhost"));
    UT_ASSERT(SDL_CreateDirectory(sess));
    UT_ASSERT(SDL_CreateDirectory(host));
    snprintf(p, sizeof(p), "%s/print2_bot2.log", host);
    UT_ASSERT(tbrWriteText(p, "===TICK 7===\nh7\n", false));
    snprintf(p, sizeof(p), "%s/print2_bot3.log.gz", host);
    UT_ASSERT(tbrWriteText(p, "===TICK 8===\nh8\n", true));
    snprintf(p, sizeof(p), "%s/%s", host, BRAINREC_FILENAME);
    UT_ASSERT(tbrWriteText(p, "not a recording\n", false));
    snprintf(p, sizeof(p), "%s/optimize.log", host);
    UT_ASSERT(tbrWriteText(p, "x\n", false));
    snprintf(p, sizeof(p), "%s/print2_bot4.txt", host);
    UT_ASSERT(tbrWriteText(p, "x\n", false));

    UT_ASSERT(brainRecMergeDir(sess, host, "h", &st, NULL, NULL));

    snprintf(p, sizeof(p), "%s/print2_bot2.log", sess);
    UT_ASSERT(tbrExists(p));
    snprintf(p, sizeof(p), "%s/print2_bot3.log.gz", sess);
    UT_ASSERT(tbrExists(p));
    snprintf(p, sizeof(p), "%s/%s", sess, BRAINREC_FILENAME);
    UT_ASSERT(!tbrExists(p));
    snprintf(p, sizeof(p), "%s/bothost_h_%s", sess, BRAINREC_FILENAME);
    UT_ASSERT(!tbrExists(p));
    snprintf(p, sizeof(p), "%s/optimize.log", sess);
    UT_ASSERT(!tbrExists(p));
    snprintf(p, sizeof(p), "%s/print2_bot4.txt", sess);
    UT_ASSERT(!tbrExists(p));
    /* Each is kept as it came, by the host's name, in bothost_raw/. */
    snprintf(p, sizeof(p), "%s/%s/h_%s", sess, BRAINREC_HOST_RAW_DIR,
             BRAINREC_FILENAME);
    UT_ASSERT(tbrExists(p));
    snprintf(p, sizeof(p), "%s/%s/h_optimize.log", sess, BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(tbrExists(p));
    snprintf(p, sizeof(p), "%s/%s/h_print2_bot4.txt", sess,
             BRAINREC_HOST_RAW_DIR);
    UT_ASSERT(tbrExists(p));
    UT_ASSERT(!tbrExists(host));
    return 0;
}

/* The upload's file cap counts a file when it opens. Pieces that each
 * open a new file and never finish it still count; at the cap the next
 * new file is refused and not made. */
int run_brainrec_host_upload_file_cap(void) {
    static BotHostConn saved;
    static const char *block = "ut652_cap_block";
    BotHostConn *hc = &udpServer.botHosts[0];
    BulkStreamHeader hd;
    char sess[256], stage[512], p[700];
    uint8_t data[4];
    uint32_t opened, finished, openedAtCap;
    bool bad, madeAtCap, sessMade;
    int i;

    snprintf(sess, sizeof(sess), "debug_sessions/%s", block);
    sessMade = SDL_CreateDirectory(sess);
    saved = *hc;
    memset(hc, 0, sizeof(*hc));
    SDL_strlcpy(hc->name, "ut652host", sizeof(hc->name));
    memset(data, 0x5A, sizeof(data));

    /* Three files, each 4 bytes long, each sent no further than byte 0. */
    for (i = 0; i < 3; i++) {
        memset(&hd, 0, sizeof(hd));
        hd.kind = BULK_KIND_REC_CHUNK;
        hd.gen = sizeof(data);
        hd.totalSize = 0;
        snprintf(hd.path, sizeof(hd.path), "%s/print2_bot%d.log/0", block, i);
        hd.pathLen = (uint8_t)strlen(hd.path);
        serverBotRecPiece(0, &hd, data);
    }
    opened = hc->recFilesOpened;
    finished = hc->recFiles;
    SDL_strlcpy(stage, hc->recStage, sizeof(stage));

    /* At the cap: the next new file is refused. */
    hc->recFilesOpened = 4096u;
    memset(&hd, 0, sizeof(hd));
    hd.kind = BULK_KIND_REC_CHUNK;
    hd.gen = sizeof(data);
    hd.totalSize = sizeof(data);
    snprintf(hd.path, sizeof(hd.path), "%s/print2_bot9.log/0", block);
    hd.pathLen = (uint8_t)strlen(hd.path);
    serverBotRecPiece(0, &hd, data);
    bad = hc->recBad;
    openedAtCap = hc->recFilesOpened;
    snprintf(p, sizeof(p), "%s/print2_bot9.log", stage);
    madeAtCap = tbrExists(p);

    if (hc->recFile != NULL) fclose(hc->recFile);
    *hc = saved;
    for (i = 0; i < 10; i++) {
        snprintf(p, sizeof(p), "%s/print2_bot%d.log", stage, i);
        (void)SDL_RemovePath(p);
    }
    if (stage[0] != '\0') (void)SDL_RemovePath(stage);
    (void)SDL_RemovePath(sess);

    UT_ASSERT(sessMade);
    UT_ASSERT(stage[0] != '\0');
    UT_ASSERT_MSG(opened == 3, "opened %u", (unsigned)opened);
    UT_ASSERT_MSG(finished == 0, "finished %u", (unsigned)finished);
    UT_ASSERT(bad);
    UT_ASSERT(openedAtCap == 4096u);
    UT_ASSERT(!madeAtCap);
    return 0;
}
