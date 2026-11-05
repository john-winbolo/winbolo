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
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "brain_record.h"
#include "brain_record_host.h"
#include "brain_record_walk.h"
#include "client_sim.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* One bot entry of a hand-made frame. */
typedef struct {
    uint8_t  slot;
    bool     hosted;
    float    thinkMs;    /* local body */
    uint16_t gen;        /* hosted marker */
    uint32_t lpi;        /* hosted marker: the last input the server took */
} TbrBot;

static bool tbrWrite(BrainRecFile *f, const void *p, size_t n) {
    return n == 0 || brainRecFileWrite(f, p, n);
}

static BrainRecFile *tbrOpenBtr(const char *path) {
    BrainRecHeader hdr;
    const char *legend = "{\"cats\":[]}";
    uint32_t llen = (uint32_t)strlen(legend);
    BrainRecFile *f = brainRecFileOpenWrite(path);
    if (f == NULL) return NULL;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, BRAINREC_MAGIC, BRAINREC_MAGIC_LEN);
    hdr.version = BRAINREC_VERSION;
    memcpy(hdr.mapName, "Test", 4);
    if (!tbrWrite(f, &hdr, sizeof(hdr)) || !tbrWrite(f, &llen, 4) ||
        !tbrWrite(f, legend, llen)) {
        (void)brainRecFileClose(f);
        return NULL;
    }
    return f;
}

static bool tbrFrame(BrainRecFile *f, uint32_t tick, const TbrBot *bots,
                     int n) {
    uint32_t magic = BRAINREC_FRAME_MAGIC;
    uint8_t zero4[4] = {0, 0, 0, 0};
    uint8_t kf = 0;
    uint32_t nd = 0;
    uint8_t nb = (uint8_t)n;
    uint8_t alliances[MAX_TANKS * 4];
    uint32_t masks[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
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
           tbrWrite(f, masks, sizeof(masks));
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
    BrainRecFile *f;
    BrainRecBuf  *buf;
} TbrCap;

static size_t tbrCapRead(void *ctx, void *dst, size_t len) {
    TbrCap *c = (TbrCap *)ctx;
    size_t total = 0;
    while (total < len) {
        long got = brainRecFileRead(c->f, (uint8_t *)dst + total, len - total);
        if (got <= 0) break;
        total += (size_t)got;
    }
    if (total > 0) brainRecBufPut(c->buf, dst, total);
    return total;
}

static bool tbrCapSkip(void *ctx, size_t len) {
    TbrCap *c = (TbrCap *)ctx;
    uint8_t tmp[4096];
    while (len > 0) {
        size_t n = len < sizeof(tmp) ? len : sizeof(tmp);
        if (!brainRecFileReadExact(c->f, tmp, n)) return false;
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
    c.f = brainRecFileOpenRead(path);
    if (c.f == NULL) return false;
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
    (void)brainRecFileClose(c.f);
    brainRecBufFree(&buf);
    return ok;
}

/* ================================================================
 * Host records fill the hosted markers by input sequence.
 * ================================================================ */
int run_brainrec_host_merge_by_tick(void) {
    char btr[1024], hbr[1024], out[1024];
    const char *files[1];
    BrainRecFile *f;
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
    UT_ASSERT(brainRecFileClose(f));
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
    BrainRecFile *f;
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
    UT_ASSERT(brainRecFileClose(f));
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
 * No host data: the markers stay and the file still walks.
 * ================================================================ */
int run_brainrec_host_merge_missing_block(void) {
    char btr[1024], hWrong[1024], out[1024];
    const char *files[2];
    BrainRecFile *f;
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
    UT_ASSERT(brainRecFileClose(f));

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
