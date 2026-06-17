/*
 * Off-socket proof for the bulk-transfer framing (src/bolo/bulk_transfer.c)
 * that carries a sized blob over the stream-flavor bulk channel.
 *
 * The matrix proves: the stream header round-trips and rejects a truncated
 * one with no over-read; a preview-sized blob reassembles byte-identical
 * through two ChannelMux instances under burst loss + reorder; a garbage or
 * oversized header is rejected by the sink without corrupting the stream; two
 * transfers back-to-back on one direction reassemble as two distinct blobs
 * (no interleave); and the send-side serializer rejects a second transfer
 * requested mid-flight rather than interleaving it. Two further cases cover the
 * map-transfer kinds that ride this channel: DOWNLOAD and RESYNC blobs
 * reassemble byte-identical under loss + reorder, and a recovery-time sweep
 * streams a map-sized blob across a loss sweep (0/10/20/30%), reporting the
 * ticks-to-reassembly (x 20 ms) — the reproducible number that says whether
 * selective-ack would ever be worth adding (the no-loss run is the baseline).
 *
 * All randomness is drawn from bolo_rand under a fixed seed, so every case is
 * deterministic. No sockets, no threads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bulk_transfer.h"
#include "channel_mux.h"
#include "bolo_rand.h"
#include "test_harness.h"

#define FRAME_BUDGET 1400
#define MAXFRAME     1600
#define PIPE_CAP     2048
#define LINK_RTT_MS  40   /* -> retransmit timeout of 4 ticks */
#define MAXBLOB      16384

/* ---- a unidirectional in-flight frame pipe (loss / reorder) ---- */

typedef struct {
    uint8_t data[MAXFRAME];
    int     len;
    int     releaseTick;
} PipeFrame;

typedef struct {
    PipeFrame f[PIPE_CAP];
    int       n;
} Pipe;

static void pipePush(Pipe *p, const uint8_t *buf, int len, int releaseTick) {
    if (len <= 0 || p->n >= PIPE_CAP) return;  /* a full pipe is extra loss */
    PipeFrame *fr = &p->f[p->n++];
    memcpy(fr->data, buf, (size_t)len);
    fr->len = len;
    fr->releaseTick = releaseTick;
}

static void pipeEnqueue(Pipe *p, const uint8_t *buf, int len, int tick,
                        bool drop, int maxJitter) {
    if (drop) return;
    int jitter = (maxJitter > 0) ? (int)bolo_rand_below((uint32_t)maxJitter + 1)
                                 : 0;
    pipePush(p, buf, len, tick + jitter);
}

static void pipeDeliver(Pipe *p, ChannelMux *dst, int tick) {
    int w = 0, r;
    for (r = 0; r < p->n; r++) {
        if (p->f[r].releaseTick <= tick) {
            channelRecvFrame(dst, p->f[r].data, p->f[r].len);
        } else {
            if (w != r) p->f[w] = p->f[r];
            w++;
        }
    }
    p->n = w;
}

/* ---- recording sink ---- */

#define SINK_MAX_BLOBS 4

typedef struct {
    uint8_t  staging[MAXBLOB];   /* in-progress blob, handed to the receiver */
    uint32_t cap;                /* reject totalSize > cap (0 = no cap)      */
    uint8_t  rejectKind;         /* reject this kind (0 = reject none)       */
    int      begins;
    int      rejects;
    int      completions;
    uint8_t  blobKind[SINK_MAX_BLOBS];
    uint32_t blobGen[SINK_MAX_BLOBS];
    uint32_t blobLen[SINK_MAX_BLOBS];
    char     blobPath[SINK_MAX_BLOBS][BULK_PATH_MAX + 1];
    uint8_t  blob[SINK_MAX_BLOBS][MAXBLOB];
} TestSink;

static uint8_t *sinkOnBegin(void *ctx, const BulkStreamHeader *h) {
    TestSink *s = (TestSink *)ctx;
    s->begins++;
    if (h->totalSize == 0 || h->totalSize > MAXBLOB ||
        (s->cap != 0 && h->totalSize > s->cap) ||
        (s->rejectKind != 0 && h->kind == s->rejectKind)) {
        s->rejects++;
        return NULL;
    }
    return s->staging;
}

static void sinkOnComplete(void *ctx, const BulkStreamHeader *h, uint8_t *buf) {
    TestSink *s = (TestSink *)ctx;
    if (s->completions >= SINK_MAX_BLOBS) return;
    int i = s->completions++;
    s->blobKind[i] = h->kind;
    s->blobGen[i] = h->gen;
    s->blobLen[i] = h->totalSize;
    memcpy(s->blobPath[i], h->path, sizeof(s->blobPath[i]));
    memcpy(s->blob[i], buf, h->totalSize);
}

static void sinkInit(TestSink *s) {
    memset(s, 0, sizeof(*s));
}

/* ---- case 1: header round-trip + truncation rejection ---- */

static int t_header_roundtrip(void) {
    BulkStreamHeader in, out;
    uint8_t buf[BULK_STREAM_HEADER_MAX];
    int n, m;
    const char *path = "Uploads/Foo.map";

    memset(&in, 0, sizeof(in));
    in.kind = BULK_KIND_PREVIEW;
    in.gen = 0xA1B2C3D4u;
    in.totalSize = 0x00112233u;
    in.pathLen = (uint8_t)strlen(path);
    memcpy(in.path, path, in.pathLen);
    in.path[in.pathLen] = '\0';

    n = bulkPackStreamHeader(buf, &in);
    if (n != BULK_STREAM_HEADER_FIXED + (int)in.pathLen) {
        UT_FAIL("pack length %d, want %d", n,
                BULK_STREAM_HEADER_FIXED + (int)in.pathLen);
    }

    memset(&out, 0, sizeof(out));
    m = bulkParseStreamHeader(buf, (size_t)n, &out);
    if (m != n) UT_FAIL("parse consumed %d, want %d", m, n);
    if (out.kind != in.kind || out.gen != in.gen ||
        out.totalSize != in.totalSize || out.pathLen != in.pathLen ||
        strcmp(out.path, in.path) != 0) {
        UT_FAIL("header fields did not round-trip");
    }

    /* Truncated: fewer than the fixed prefix, and fewer than fixed+path. No
     * over-read — the parser only reports it cannot proceed. */
    if (bulkParseStreamHeader(buf, BULK_STREAM_HEADER_FIXED - 1, &out) != 0) {
        UT_FAIL("short-of-fixed header was not rejected");
    }
    if (bulkParseStreamHeader(buf, (size_t)(n - 1), &out) != 0) {
        UT_FAIL("short-of-path header was not rejected");
    }
    return 0;
}

/* ---- case 2: a preview-sized blob reassembles byte-identical under loss ---- */

static int t_blob_under_loss(void) {
    bolo_srand(0xB17EA11ULL);
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    Pipe *ab = (Pipe *)calloc(1, sizeof(*ab));
    Pipe *ba = (Pipe *)calloc(1, sizeof(*ba));
    TestSink *sink = (TestSink *)malloc(sizeof(*sink));
    BulkSender snd;
    BulkReceiver rcv;
    BulkStreamHeader h;
    const uint32_t BLOB = 8000;   /* preview-sized */
    uint8_t *src = (uint8_t *)malloc(BLOB);
    uint8_t frame[MAXFRAME];
    int rc = 1, tick;
    BulkRecvSink rsink;

    bulkSenderInit(&snd);   /* safe to bulkSenderReset at done after this */
    bulkReceiverInit(&rcv);
    if (!a || !b || !ab || !ba || !sink || !src) goto done;
    sinkInit(sink);
    channelMuxInit(a);
    channelMuxInit(b);
    rsink.onBegin = sinkOnBegin;
    rsink.onComplete = sinkOnComplete;
    rsink.ctx = sink;

    {
        uint32_t i;
        for (i = 0; i < BLOB; i++) src[i] = (uint8_t)bolo_rand();
    }
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW;
    h.gen = 7;
    h.totalSize = BLOB;
    h.pathLen = 8;
    memcpy(h.path, "a/b.map", 7);
    h.path[7] = '\0';
    h.pathLen = (uint8_t)strlen(h.path);

    if (!bulkSenderBegin(&snd, &h, src, BLOB)) goto done;

    for (tick = 0; tick < 60000 && sink->completions == 0; tick++) {
        bulkSenderPump(&snd, a);
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);

        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        bool dropAB = ((int)bolo_rand_below(100) < 20) ||
                      (tick >= 30 && tick < 36);   /* burst loss window */
        pipeEnqueue(ab, frame, la, tick, dropAB, 4);

        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        bool dropBA = ((int)bolo_rand_below(100) < 20);
        pipeEnqueue(ba, frame, lb, tick, dropBA, 4);

        pipeDeliver(ab, b, tick);
        pipeDeliver(ba, a, tick);

        uint8_t out[CHANNEL_MAX_SEG];
        uint16_t olen;
        while (channelReceive(b, CHANNEL_BULK, out, &olen)) {
            bulkReceiverFeed(&rcv, out, olen, &rsink);
        }
    }

    if (sink->completions != 1) goto done;
    if (sink->blobKind[0] != BULK_KIND_PREVIEW || sink->blobGen[0] != 7 ||
        sink->blobLen[0] != BLOB || strcmp(sink->blobPath[0], "a/b.map") != 0) {
        goto done;
    }
    if (memcmp(sink->blob[0], src, BLOB) != 0) goto done;
    rc = 0;
done:
    free(a); free(b); free(ab); free(ba); free(sink); free(src);
    bulkSenderReset(&snd);
    if (rc) UT_FAIL("preview-sized blob did not reassemble byte-identical under loss");
    return 0;
}

/* ---- case 3: garbage / oversized headers rejected without corruption ---- */

static int t_header_robustness(void) {
    TestSink sink;
    BulkReceiver rcv;
    BulkRecvSink rsink;
    BulkStreamHeader h;
    uint8_t wire[BULK_STREAM_HEADER_MAX + 256];
    int hn;

    rsink.onBegin = sinkOnBegin;
    rsink.onComplete = sinkOnComplete;

    /* Oversized totalSize: onBegin caps it and rejects; the body that follows
     * is consumed and discarded, and the stream stays aligned so a later valid
     * transfer still completes. */
    sinkInit(&sink);
    sink.cap = 100;          /* anything larger is "oversized" */
    rsink.ctx = &sink;
    bulkReceiverInit(&rcv);

    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW;
    h.gen = 1;
    h.totalSize = 500;       /* > cap */
    h.pathLen = 0;
    h.path[0] = '\0';
    hn = bulkPackStreamHeader(wire, &h);
    {
        uint8_t body[500];
        memset(body, 0xAB, sizeof(body));
        bulkReceiverFeed(&rcv, wire, (uint32_t)hn, &rsink);
        bulkReceiverFeed(&rcv, body, sizeof(body), &rsink);
    }
    if (sink.rejects != 1 || sink.completions != 0) {
        UT_FAIL("oversized header was not rejected cleanly (rejects=%d completions=%d)",
                sink.rejects, sink.completions);
    }

    /* A following well-sized transfer on the same receiver completes — proof
     * the rejected body left the stream aligned. */
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW;
    h.gen = 2;
    h.totalSize = 50;        /* <= cap */
    h.pathLen = 0;
    h.path[0] = '\0';
    hn = bulkPackStreamHeader(wire, &h);
    {
        uint8_t body[50];
        memset(body, 0x5A, sizeof(body));
        bulkReceiverFeed(&rcv, wire, (uint32_t)hn, &rsink);
        bulkReceiverFeed(&rcv, body, sizeof(body), &rsink);
    }
    if (sink.completions != 1 || sink.blobGen[0] != 2 || sink.blobLen[0] != 50) {
        UT_FAIL("valid transfer after a rejected one did not complete");
    }

    /* Garbage kind: the sink rejects an unknown kind; no completion. */
    sinkInit(&sink);
    sink.rejectKind = 0x42;
    rsink.ctx = &sink;
    bulkReceiverInit(&rcv);
    memset(&h, 0, sizeof(h));
    h.kind = 0x42;
    h.gen = 9;
    h.totalSize = 10;
    h.pathLen = 0;
    h.path[0] = '\0';
    hn = bulkPackStreamHeader(wire, &h);
    {
        uint8_t body[10];
        memset(body, 0x11, sizeof(body));
        bulkReceiverFeed(&rcv, wire, (uint32_t)hn, &rsink);
        bulkReceiverFeed(&rcv, body, sizeof(body), &rsink);
    }
    if (sink.rejects != 1 || sink.completions != 0) {
        UT_FAIL("garbage-kind header was not rejected");
    }

    /* A header split across two feeds (byte-at-a-time) must still parse — no
     * over-read past the bytes handed in each feed. */
    sinkInit(&sink);
    rsink.ctx = &sink;
    bulkReceiverInit(&rcv);
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW;
    h.gen = 3;
    h.totalSize = 4;
    h.pathLen = 5;
    memcpy(h.path, "x/y.m", 5);
    h.path[5] = '\0';
    hn = bulkPackStreamHeader(wire, &h);
    {
        uint8_t body[4] = { 1, 2, 3, 4 };
        int i;
        for (i = 0; i < hn; i++) {
            bulkReceiverFeed(&rcv, wire + i, 1, &rsink);  /* one byte per feed */
        }
        bulkReceiverFeed(&rcv, body, sizeof(body), &rsink);
    }
    if (sink.completions != 1 || sink.blobGen[0] != 3 ||
        strcmp(sink.blobPath[0], "x/y.m") != 0) {
        UT_FAIL("byte-at-a-time header did not reassemble");
    }
    return 0;
}

/* ---- case 4: two transfers back-to-back reassemble as two distinct blobs ---- */

static int t_pipelining(void) {
    TestSink sink;
    BulkReceiver rcv;
    BulkRecvSink rsink;
    BulkStreamHeader h;
    uint8_t stream[2 * (BULK_STREAM_HEADER_MAX + 64)];
    uint32_t pos = 0;
    uint8_t blobA[40], blobB[24];
    int i;

    sinkInit(&sink);
    rsink.onBegin = sinkOnBegin;
    rsink.onComplete = sinkOnComplete;
    rsink.ctx = &sink;
    bulkReceiverInit(&rcv);

    for (i = 0; i < (int)sizeof(blobA); i++) blobA[i] = (uint8_t)(0x10 + i);
    for (i = 0; i < (int)sizeof(blobB); i++) blobB[i] = (uint8_t)(0x80 + i);

    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW; h.gen = 100; h.totalSize = sizeof(blobA);
    h.pathLen = 3; memcpy(h.path, "aaa", 3); h.path[3] = '\0';
    pos += (uint32_t)bulkPackStreamHeader(stream + pos, &h);
    memcpy(stream + pos, blobA, sizeof(blobA)); pos += sizeof(blobA);

    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW; h.gen = 101; h.totalSize = sizeof(blobB);
    h.pathLen = 3; memcpy(h.path, "bbb", 3); h.path[3] = '\0';
    pos += (uint32_t)bulkPackStreamHeader(stream + pos, &h);
    memcpy(stream + pos, blobB, sizeof(blobB)); pos += sizeof(blobB);

    /* Feed the whole concatenated stream as one fragment: the receiver must
     * split it into two transfers. */
    bulkReceiverFeed(&rcv, stream, pos, &rsink);

    if (sink.completions != 2) {
        UT_FAIL("pipelined stream produced %d completions, want 2", sink.completions);
    }
    if (sink.blobGen[0] != 100 || sink.blobLen[0] != sizeof(blobA) ||
        memcmp(sink.blob[0], blobA, sizeof(blobA)) != 0) {
        UT_FAIL("first pipelined blob mismatched");
    }
    if (sink.blobGen[1] != 101 || sink.blobLen[1] != sizeof(blobB) ||
        memcmp(sink.blob[1], blobB, sizeof(blobB)) != 0) {
        UT_FAIL("second pipelined blob mismatched");
    }
    return 0;
}

/* ---- case 5: the send-side serializer rejects a mid-flight second transfer ---- */

static int t_serializer_guard(void) {
    ChannelMux *m = (ChannelMux *)malloc(sizeof(*m));
    BulkSender snd;
    BulkStreamHeader h;
    uint8_t blob1[300], blob2[20];
    int rc = 1, i;

    bulkSenderInit(&snd);   /* safe to bulkSenderReset at done after this */
    if (!m) goto done;
    channelMuxInit(m);
    for (i = 0; i < (int)sizeof(blob1); i++) blob1[i] = (uint8_t)i;
    for (i = 0; i < (int)sizeof(blob2); i++) blob2[i] = (uint8_t)(0xF0 + i);

    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW; h.gen = 1; h.totalSize = sizeof(blob1);
    h.pathLen = 0; h.path[0] = '\0';
    if (!bulkSenderBegin(&snd, &h, blob1, sizeof(blob1))) goto done;
    if (!bulkSenderBusy(&snd)) goto done;

    /* A second transfer requested while busy is rejected — never interleaved. */
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW; h.gen = 2; h.totalSize = sizeof(blob2);
    h.pathLen = 0; h.path[0] = '\0';
    if (bulkSenderBegin(&snd, &h, blob2, sizeof(blob2))) {
        goto done;   /* must have been rejected */
    }
    /* The rejected request left the in-flight transfer untouched: the staged
     * buffer is exactly t1's header+blob, never t2's and never the two
     * concatenated. */
    if (snd.kind != BULK_KIND_PREVIEW || snd.offset != 0) goto done;
    if (snd.total != (uint32_t)BULK_STREAM_HEADER_FIXED + sizeof(blob1)) goto done;

    /* One pump hands the whole (small) transfer to the mux, which has ample
     * room, so the sender empties and frees its busy flag. */
    bulkSenderPump(&snd, m);
    if (bulkSenderBusy(&snd)) goto done;
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_PREVIEW; h.gen = 3; h.totalSize = sizeof(blob2);
    h.pathLen = 0; h.path[0] = '\0';
    if (!bulkSenderBegin(&snd, &h, blob2, sizeof(blob2))) goto done;
    rc = 0;
done:
    bulkSenderReset(&snd);
    free(m);
    if (rc) UT_FAIL("serializer guard did not hold");
    return 0;
}

/* ---- case 6: the kind byte is opaque — an UPLOAD blob reassembles the same
 *      way a PREVIEW one does (the framing carries any transferKind) ---- */

static int t_upload_kind(void) {
    TestSink sink;
    BulkReceiver rcv;
    BulkRecvSink rsink;
    BulkStreamHeader h;
    uint8_t wire[BULK_STREAM_HEADER_MAX + 64];
    uint8_t blob[40];
    uint32_t pos;
    int i;

    sinkInit(&sink);
    rsink.onBegin = sinkOnBegin;
    rsink.onComplete = sinkOnComplete;
    rsink.ctx = &sink;
    bulkReceiverInit(&rcv);

    for (i = 0; i < (int)sizeof(blob); i++) blob[i] = (uint8_t)(0xC0 + i);
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_UPLOAD;
    h.gen = 0;
    h.totalSize = sizeof(blob);
    h.pathLen = 9;
    memcpy(h.path, "Foo.map", 7);
    h.path[7] = '\0';
    h.pathLen = (uint8_t)strlen(h.path);
    pos = (uint32_t)bulkPackStreamHeader(wire, &h);
    memcpy(wire + pos, blob, sizeof(blob));
    pos += sizeof(blob);

    /* Feed in two arbitrary splits to exercise the header/body boundary. */
    bulkReceiverFeed(&rcv, wire, 5, &rsink);
    bulkReceiverFeed(&rcv, wire + 5, pos - 5, &rsink);

    if (sink.completions != 1 || sink.blobKind[0] != BULK_KIND_UPLOAD ||
        sink.blobLen[0] != sizeof(blob) ||
        strcmp(sink.blobPath[0], "Foo.map") != 0 ||
        memcmp(sink.blob[0], blob, sizeof(blob)) != 0) {
        UT_FAIL("UPLOAD-kind blob did not reassemble");
    }
    return 0;
}

/* ---- recovery harness: stream one blob A->B through a lossy pipe ---- */

/* A minimal sink with a single caller-owned dst, so the recovery sweep can use
 * a map-sized blob without bloating the fixed-array TestSink. */
typedef struct {
    uint8_t *dst;
    uint32_t cap;
    bool     done;
    uint8_t  kind;
    uint32_t gen;
    uint32_t len;
} RecoverSink;

static uint8_t *recoverOnBegin(void *ctx, const BulkStreamHeader *h) {
    RecoverSink *s = (RecoverSink *)ctx;
    if (h->totalSize == 0 || h->totalSize > s->cap) return NULL;
    return s->dst;
}

static void recoverOnComplete(void *ctx, const BulkStreamHeader *h, uint8_t *buf) {
    RecoverSink *s = (RecoverSink *)ctx;
    (void)buf;
    s->done = true;
    s->kind = h->kind;
    s->gen  = h->gen;
    s->len  = h->totalSize;
}

/* Stream src (blobLen bytes) under a stream header of (kind, gen) from A to B
 * through a lossy + reorder pipe (one frame per direction per tick). Returns the
 * tick count from the first content frame A emits to full reassembly on B, or -1
 * if it never completed / setup failed. dst (>= blobLen) receives the bytes;
 * *outKind / *outGen get the completed header's fields. Deterministic per seed. */
static int streamBlobRecover(uint8_t kind, uint32_t gen, const uint8_t *src,
                             uint32_t blobLen, int lossPct, uint64_t seed,
                             uint8_t *dst, uint32_t dstCap,
                             uint8_t *outKind, uint32_t *outGen) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    Pipe *ab = (Pipe *)calloc(1, sizeof(*ab));
    Pipe *ba = (Pipe *)calloc(1, sizeof(*ba));
    BulkSender snd;
    BulkReceiver rcv;
    BulkStreamHeader h;
    RecoverSink rs;
    BulkRecvSink sink;
    uint8_t frame[MAXFRAME];
    int ticks = -1, firstContentTick = -1, tick;

    bolo_srand(seed);
    bulkSenderInit(&snd);
    bulkReceiverInit(&rcv);
    if (!a || !b || !ab || !ba) goto done;
    channelMuxInit(a);
    channelMuxInit(b);
    rs.dst = dst; rs.cap = dstCap; rs.done = false;
    rs.kind = 0; rs.gen = 0; rs.len = 0;
    sink.onBegin = recoverOnBegin;
    sink.onComplete = recoverOnComplete;
    sink.ctx = &rs;

    memset(&h, 0, sizeof(h));
    h.kind = kind; h.gen = gen; h.totalSize = blobLen; h.pathLen = 0;
    h.path[0] = '\0';
    if (!bulkSenderBegin(&snd, &h, src, blobLen)) goto done;

    for (tick = 0; tick < 200000 && !rs.done; tick++) {
        int la, lb;
        uint8_t out[CHANNEL_MAX_SEG];
        uint16_t olen;
        bulkSenderPump(&snd, a);
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);

        la = channelBuildFrame(a, frame, FRAME_BUDGET);
        if (firstContentTick < 0 && la > 2) firstContentTick = tick;
        pipeEnqueue(ab, frame, la, tick,
                    (int)bolo_rand_below(100) < lossPct, 4);

        lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        pipeEnqueue(ba, frame, lb, tick,
                    (int)bolo_rand_below(100) < lossPct, 4);

        pipeDeliver(ab, b, tick);
        pipeDeliver(ba, a, tick);

        while (channelReceive(b, CHANNEL_BULK, out, &olen)) {
            bulkReceiverFeed(&rcv, out, olen, &sink);
        }
    }

    if (rs.done && firstContentTick >= 0) {
        ticks = (tick - 1) - firstContentTick;
        if (outKind) *outKind = rs.kind;
        if (outGen)  *outGen  = rs.gen;
    }
done:
    free(a); free(b); free(ab); free(ba);
    bulkSenderReset(&snd);
    return ticks;
}

/* ---- case 6: DOWNLOAD and RESYNC kinds reassemble byte-identical ---- */

static int t_download_resync_kinds(void) {
    const uint32_t BLOB = 8000;       /* spans many bulk segments */
    uint8_t *src = (uint8_t *)malloc(BLOB);
    uint8_t *dst = (uint8_t *)malloc(BLOB);
    int rc = 1, ticks;
    uint8_t gotKind = 0;
    uint32_t gotGen = 0, i;
    if (!src || !dst) goto done;
    for (i = 0; i < BLOB; i++) src[i] = (uint8_t)((i * 131u + 7u) & 0xFF);

    /* Join download (gen 0) under burst-ish loss + reorder. */
    ticks = streamBlobRecover(BULK_KIND_DOWNLOAD, 0, src, BLOB, 20,
                              0xD0117AD1ULL, dst, BLOB, &gotKind, &gotGen);
    if (ticks < 0 || gotKind != BULK_KIND_DOWNLOAD || gotGen != 0 ||
        memcmp(dst, src, BLOB) != 0) {
        UT_FAIL("DOWNLOAD-kind blob did not reassemble byte-identical under loss");
    }

    /* Live resync (nonzero gen). */
    memset(dst, 0, BLOB);
    ticks = streamBlobRecover(BULK_KIND_RESYNC, 9, src, BLOB, 20,
                              0x9E5114CULL, dst, BLOB, &gotKind, &gotGen);
    if (ticks < 0 || gotKind != BULK_KIND_RESYNC || gotGen != 9 ||
        memcmp(dst, src, BLOB) != 0) {
        UT_FAIL("RESYNC-kind blob did not reassemble byte-identical under loss");
    }
    rc = 0;
done:
    free(src); free(dst);
    return rc;
}

/* ---- case 7: recovery-time sweep — the SACK-revisit number ----
 *
 * Streams a fixed map-sized blob A->B and counts ticks from the first emitted
 * bulk segment to first full reassembly across a loss sweep. The no-loss run is
 * the baseline; the per-loss figure (recoveryTicks x 20 ms) is the reproducible
 * number that says whether selective-ack would ever be worth adding. */
static int t_recovery_sweep(void) {
    const uint32_t BLOB = 60000;   /* map-sized; header+blob exceeds one window */
    static const int losses[] = { 0, 10, 20, 30 };
    uint8_t *src = (uint8_t *)malloc(BLOB);
    uint8_t *dst = (uint8_t *)malloc(BLOB);
    int rc = 1;
    uint32_t i;
    size_t k;
    if (!src || !dst) goto done;
    /* Deterministic, no-RNG fill so the payload is identical every run. */
    for (i = 0; i < BLOB; i++) src[i] = (uint8_t)((i * 2654435761u) >> 13);

    for (k = 0; k < sizeof(losses) / sizeof(losses[0]); k++) {
        int loss = losses[k];
        int ticks;
        memset(dst, 0, BLOB);
        ticks = streamBlobRecover(BULK_KIND_DOWNLOAD, 0, src, BLOB, loss,
                                  0x5ACC0DEULL + (uint64_t)loss,
                                  dst, BLOB, NULL, NULL);
        if (ticks < 0 || memcmp(dst, src, BLOB) != 0) {
            UT_FAIL("recovery sweep: %u-byte blob did not reassemble at loss=%d%%",
                    (unsigned)BLOB, loss);
        }
        fprintf(stderr,
                "  bulk recovery: loss=%2d%%  %5d ticks  (%6d ms)%s\n",
                loss, ticks, ticks * 20, loss == 0 ? "  [baseline]" : "");
    }
    rc = 0;
done:
    free(src); free(dst);
    return rc;
}

/* ---- case 8: a CHANNEL_BULK re-base drops the in-flight transfer cleanly ----
 *
 * Mirrors the lobby-map-change path: a partial transfer is staged on the sender
 * (staging bytes pending + unacked segments in the window), then the channel is
 * re-based (bulkSenderReset + channelResetSend(CHANNEL_BULK) on the server,
 * channelResetExpected on the client). A fresh transfer started at the new
 * baseline must reassemble byte-identical — proving the reset cleared the
 * un-segmentized staging tail (without that clear, the old transfer's leftover
 * bytes would segmentize into the new stream and the receiver would misparse). */
static int t_rebase_drops_inflight(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    Pipe *ab = (Pipe *)calloc(1, sizeof(*ab));
    Pipe *ba = (Pipe *)calloc(1, sizeof(*ba));
    BulkSender snd;
    BulkReceiver rcv;
    RecoverSink rs;
    BulkRecvSink sink;
    BulkStreamHeader h;
    uint8_t *blobA = (uint8_t *)malloc(40000);
    uint8_t *blobB = (uint8_t *)malloc(9000);
    uint8_t *dst   = (uint8_t *)malloc(9000);
    uint8_t frame[MAXFRAME];
    int rc = 1, tick;
    uint32_t i, b3;

    bolo_srand(0x5EBA5E11ULL);
    bulkSenderInit(&snd);
    bulkReceiverInit(&rcv);
    if (!a || !b || !ab || !ba || !blobA || !blobB || !dst) goto done;
    channelMuxInit(a);
    channelMuxInit(b);
    for (i = 0; i < 40000; i++) blobA[i] = (uint8_t)((i * 7u + 3u) & 0xFF);
    for (i = 0; i < 9000;  i++) blobB[i] = (uint8_t)((i * 251u + 5u) & 0xFF);

    /* Stage transfer A and pump once so the window fills and the staging buffer
     * still holds the un-segmentized tail (40000 + header > one 96-segment
     * window of 256-byte segments). Deliver nothing — A is left in flight. */
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_DOWNLOAD; h.gen = 1; h.totalSize = 40000;
    if (!bulkSenderBegin(&snd, &h, blobA, 40000)) goto done;
    bulkSenderPump(&snd, a);
    if (a->streamCount == 0) goto done;                       /* expect a pending tail */
    if (a->ch[CHANNEL_BULK].nextSeq == a->ch[CHANNEL_BULK].ackedSeq) goto done; /* unacked segs */

    /* Re-base: drop the sender's staged blob and reset the bulk channel. */
    bulkSenderReset(&snd);
    b3 = channelResetSend(a, CHANNEL_BULK);
    if (a->streamCount != 0) {
        UT_FAIL("channelResetSend left %u staging bytes on CHANNEL_BULK",
                (unsigned)a->streamCount);
    }
    if (a->ch[CHANNEL_BULK].ackedSeq != a->ch[CHANNEL_BULK].nextSeq ||
        b3 != a->ch[CHANNEL_BULK].nextSeq) {
        UT_FAIL("channelResetSend did not collapse the bulk send window");
    }
    /* The client adopts the carried baseline (the CTRL_CHANNEL_RESET ch3). */
    channelResetExpected(b, CHANNEL_BULK, b3);

    /* Stream a fresh transfer B at the new baseline; it must reassemble clean. */
    rs.dst = dst; rs.cap = 9000; rs.done = false;
    rs.kind = 0; rs.gen = 0; rs.len = 0;
    sink.onBegin = recoverOnBegin;
    sink.onComplete = recoverOnComplete;
    sink.ctx = &rs;
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_DOWNLOAD; h.gen = 2; h.totalSize = 9000;
    if (!bulkSenderBegin(&snd, &h, blobB, 9000)) goto done;

    for (tick = 0; tick < 100000 && !rs.done; tick++) {
        int la, lb;
        uint8_t out[CHANNEL_MAX_SEG];
        uint16_t olen;
        bulkSenderPump(&snd, a);
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        la = channelBuildFrame(a, frame, FRAME_BUDGET);
        pipeEnqueue(ab, frame, la, tick, false, 0);
        lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        pipeEnqueue(ba, frame, lb, tick, false, 0);
        pipeDeliver(ab, b, tick);
        pipeDeliver(ba, a, tick);
        while (channelReceive(b, CHANNEL_BULK, out, &olen)) {
            bulkReceiverFeed(&rcv, out, olen, &sink);
        }
    }
    if (!rs.done || rs.gen != 2 || rs.len != 9000 ||
        memcmp(dst, blobB, 9000) != 0) {
        UT_FAIL("post-rebase transfer did not reassemble byte-identical");
    }
    rc = 0;
done:
    free(a); free(b); free(ab); free(ba);
    free(blobA); free(blobB); free(dst);
    bulkSenderReset(&snd);
    if (rc) UT_FAIL("CHANNEL_BULK re-base did not drop the in-flight transfer cleanly");
    return 0;
}

int run_bulk_transfer(void) {
    int rc = t_header_roundtrip();
    if (rc != 0) return rc;
    rc = t_blob_under_loss();
    if (rc != 0) return rc;
    rc = t_header_robustness();
    if (rc != 0) return rc;
    rc = t_pipelining();
    if (rc != 0) return rc;
    rc = t_serializer_guard();
    if (rc != 0) return rc;
    rc = t_upload_kind();
    if (rc != 0) return rc;
    rc = t_download_resync_kinds();
    if (rc != 0) return rc;
    rc = t_rebase_drops_inflight();
    if (rc != 0) return rc;
    return t_recovery_sweep();
}
