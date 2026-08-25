/*
 * Round-log transfer: the wire contract for BULK_KIND_ROUND_LOG and the
 * refusal path a client takes when the server will not serve one.
 *
 * Three cases, cheapest first:
 *
 *  - the stream header round-trips at the extremes the round-log transfer
 *    actually uses (kind 8, a reqSeq echo in gen, a totalSize at the
 *    ROUND_LOG_MAX_BYTES cap, a path at BULK_PATH_MAX), with the packed bytes
 *    checked for the big-endian layout the wire promises, and a truncated
 *    header parsed out of an exactly-sized heap buffer so an over-read is a
 *    heap overflow rather than a silent stack peek;
 *
 *  - a rejected round-log stream (onBegin returning NULL, which is what the
 *    client does for a reply to a superseded request or one over the cap)
 *    leaves the byte stream aligned: the stream that follows it in the same
 *    byte run still reassembles intact. That is the invariant the dst != NULL
 *    guard in bulkReceiverFeed's body arm supplies, proved from the outside —
 *    on the following stream's bytes, not on receiver internals;
 *
 *  - a real client asking a real server over loopback for a round log it
 *    cannot serve lands on CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED. The loopback
 *    harness never installs the dedicated-server recorder, so no RoundLogSource
 *    is registered and the server's gate answers ROUND_LOG_ERR_DISABLED — which
 *    is what makes the refusal reachable without recording a round.
 *
 * The success path (a real .wbv arriving and being handed over) is not covered
 * here: it needs a recorded round on disk and a recorder installed in the
 * server, neither of which the loopback harness has. It is human-gated on two
 * machines instead.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"            /* ClientRoundLogState + the round-log API */
#include "client_connect_state.h"  /* CLIENT_CONNECT_CONNECTED */
#include "wire_limits.h"           /* ROUND_LOG_MAX_BYTES */
#include "bulk_transfer.h"
#include "loopback_harness.h"
#include "test_harness.h"

/* ---- case 1: header round-trip at the round-log extremes ---- */

int run_round_log_header_roundtrip(void) {
    BulkStreamHeader in, out;
    uint8_t buf[BULK_STREAM_HEADER_MAX];
    size_t shortLens[2];
    int n, m, i, k;

    /* A path at exactly BULK_PATH_MAX — the longest basename the header can
     * carry, and the case where pack fills buf to BULK_STREAM_HEADER_MAX. */
    memset(&in, 0, sizeof(in));
    in.kind = BULK_KIND_ROUND_LOG;
    in.gen = 0x1234ABCDu;                  /* the request's reqSeq, echoed */
    in.totalSize = ROUND_LOG_MAX_BYTES - 1u;
    in.pathLen = BULK_PATH_MAX;
    for (i = 0; i < BULK_PATH_MAX; i++) {
        in.path[i] = (char)('a' + (i % 26));
    }
    in.path[BULK_PATH_MAX] = '\0';

    n = bulkPackStreamHeader(buf, &in);
    if (n != BULK_STREAM_HEADER_FIXED + BULK_PATH_MAX) {
        UT_FAIL("pack length %d, want %d", n,
                BULK_STREAM_HEADER_FIXED + BULK_PATH_MAX);
    }

    /* The wire layout itself: kind is the literal 8, gen and totalSize are
     * big-endian, pathLen is the byte at offset 9. */
    if (buf[0] != 8) {
        UT_FAIL("kind byte %u on the wire, want 8", (unsigned)buf[0]);
    }
    {
        uint8_t want[4];
        want[0] = (uint8_t)(in.gen >> 24);
        want[1] = (uint8_t)(in.gen >> 16);
        want[2] = (uint8_t)(in.gen >> 8);
        want[3] = (uint8_t)in.gen;
        if (memcmp(buf + 1, want, 4) != 0) {
            UT_FAIL("gen is not big-endian at offset 1");
        }
        want[0] = (uint8_t)(in.totalSize >> 24);
        want[1] = (uint8_t)(in.totalSize >> 16);
        want[2] = (uint8_t)(in.totalSize >> 8);
        want[3] = (uint8_t)in.totalSize;
        if (memcmp(buf + 5, want, 4) != 0) {
            UT_FAIL("totalSize is not big-endian at offset 5");
        }
    }
    if (buf[9] != BULK_PATH_MAX) {
        UT_FAIL("pathLen byte %u at offset 9, want %d", (unsigned)buf[9],
                BULK_PATH_MAX);
    }

    memset(&out, 0, sizeof(out));
    m = bulkParseStreamHeader(buf, (size_t)n, &out);
    if (m != n) UT_FAIL("parse consumed %d, want %d", m, n);
    if (out.kind != in.kind || out.gen != in.gen ||
        out.totalSize != in.totalSize || out.pathLen != in.pathLen ||
        strcmp(out.path, in.path) != 0) {
        UT_FAIL("header fields did not round-trip at BULK_PATH_MAX");
    }

    /* The cap itself must survive the 4-byte field — the client compares the
     * parsed totalSize against ROUND_LOG_MAX_BYTES, so the boundary value has
     * to come back exactly. */
    memset(&in, 0, sizeof(in));
    in.kind = BULK_KIND_ROUND_LOG;
    in.gen = 1;
    in.totalSize = ROUND_LOG_MAX_BYTES;
    in.pathLen = 0;
    in.path[0] = '\0';
    n = bulkPackStreamHeader(buf, &in);
    if (n != BULK_STREAM_HEADER_FIXED) {
        UT_FAIL("empty-path pack length %d, want %d", n,
                BULK_STREAM_HEADER_FIXED);
    }
    memset(&out, 0, sizeof(out));
    m = bulkParseStreamHeader(buf, (size_t)n, &out);
    if (m != n || out.totalSize != ROUND_LOG_MAX_BYTES || out.pathLen != 0 ||
        out.path[0] != '\0') {
        UT_FAIL("a totalSize at the cap did not round-trip");
    }

    /* Truncated headers report "cannot proceed" and read nothing past avail.
     * Each short parse runs against a heap buffer of exactly the length handed
     * in, so an over-read is a heap overflow an ASAN build traps on rather than
     * a silent read of the rest of the stack buffer. Repack the long-path
     * header first so the second cut lands inside the path. */
    memset(&in, 0, sizeof(in));
    in.kind = BULK_KIND_ROUND_LOG;
    in.gen = 0x1234ABCDu;
    in.totalSize = ROUND_LOG_MAX_BYTES - 1u;
    in.pathLen = BULK_PATH_MAX;
    for (i = 0; i < BULK_PATH_MAX; i++) {
        in.path[i] = (char)('a' + (i % 26));
    }
    in.path[BULK_PATH_MAX] = '\0';
    n = bulkPackStreamHeader(buf, &in);

    shortLens[0] = (size_t)BULK_STREAM_HEADER_FIXED - 1;  /* short of fixed */
    shortLens[1] = (size_t)n - 1;                         /* short of path  */
    for (k = 0; k < 2; k++) {
        uint8_t *exact = (uint8_t *)malloc(shortLens[k]);
        int r;
        if (exact == NULL) UT_FAIL("out of memory sizing a truncated header");
        memcpy(exact, buf, shortLens[k]);
        memset(&out, 0, sizeof(out));
        r = bulkParseStreamHeader(exact, shortLens[k], &out);
        free(exact);
        if (r != 0) {
            UT_FAIL("a %u-byte header parsed as %d, want 0",
                    (unsigned)shortLens[k], r);
        }
    }
    return 0;
}

/* ---- case 2: a rejected stream leaves the next one intact ---- */

#define RL_STAGING_MAX 512
#define RL_REJ_BODY    200
#define RL_ACC_BODY    120
#define RL_RUN_MAX     (2 * (BULK_STREAM_HEADER_MAX + RL_STAGING_MAX))

/* The accepted request's reqSeq. Anything else is a reply to a request this
 * client has already superseded, which is one of the two reasons the real
 * client's onBegin returns NULL (the other is a size over the cap). */
#define RL_ACCEPT_GEN  0x00ABCDEFu
#define RL_STALE_GEN   0x00ABCDEEu

typedef struct {
    uint8_t  staging[RL_STAGING_MAX];  /* handed to the receiver on accept */
    uint32_t acceptGen;
    int      begins;
    int      rejects;
    int      completions;
    uint8_t  kind;                     /* last completion's header + blob */
    uint32_t gen;
    uint32_t len;
    char     path[BULK_PATH_MAX + 1];
    uint8_t  blob[RL_STAGING_MAX];
} RoundLogSink;

static uint8_t *rlOnBegin(void *ctx, const BulkStreamHeader *h) {
    RoundLogSink *s = (RoundLogSink *)ctx;
    s->begins++;
    /* The client's own gate, in the same order: an answer to a superseded
     * request, an empty blob, or one the receiver has nowhere to put. */
    if (h->gen != s->acceptGen || h->totalSize == 0 ||
        h->totalSize > ROUND_LOG_MAX_BYTES ||
        h->totalSize > (uint32_t)RL_STAGING_MAX) {
        s->rejects++;
        return NULL;
    }
    return s->staging;
}

static void rlOnComplete(void *ctx, const BulkStreamHeader *h, uint8_t *buf) {
    RoundLogSink *s = (RoundLogSink *)ctx;
    s->completions++;
    s->kind = h->kind;
    s->gen  = h->gen;
    s->len  = h->totalSize;
    memcpy(s->path, h->path, sizeof(s->path));
    if (h->totalSize <= (uint32_t)RL_STAGING_MAX) {
        memcpy(s->blob, buf, h->totalSize);
    }
}

/* Append one BULK_KIND_ROUND_LOG stream (header + body) at pos, returning the
 * new end offset. */
static uint32_t rlAppendStream(uint8_t *out, uint32_t pos, uint32_t gen,
                               const uint8_t *body, uint32_t bodyLen,
                               const char *path) {
    BulkStreamHeader h;
    memset(&h, 0, sizeof(h));
    h.kind = BULK_KIND_ROUND_LOG;
    h.gen = gen;
    h.totalSize = bodyLen;
    h.pathLen = (uint8_t)strlen(path);
    memcpy(h.path, path, h.pathLen);
    h.path[h.pathLen] = '\0';
    pos += (uint32_t)bulkPackStreamHeader(out + pos, &h);
    memcpy(out + pos, body, bodyLen);
    return pos + bodyLen;
}

int run_round_log_sink_rejection_realigns(void) {
    static const char *kStalePath = "round-stale.wbv";
    static const char *kAcceptPath = "round-2026.wbv";
    uint8_t run[RL_RUN_MAX];
    uint8_t rejected[RL_REJ_BODY];
    uint8_t accepted[RL_ACC_BODY];
    uint32_t accStart, runLen;
    int i, pass;

    for (i = 0; i < RL_REJ_BODY; i++) rejected[i] = (uint8_t)(0x30 + i);
    for (i = 0; i < RL_ACC_BODY; i++) accepted[i] = (uint8_t)(i * 7 + 11);

    /* One byte run: the refused stream, then the accepted one immediately
     * behind it — no gap, exactly how two answers arrive back to back. */
    accStart = rlAppendStream(run, 0, RL_STALE_GEN, rejected, RL_REJ_BODY,
                              kStalePath);
    runLen = rlAppendStream(run, accStart, RL_ACCEPT_GEN, accepted, RL_ACC_BODY,
                            kAcceptPath);

    /* Pass 0 feeds the run in one go; pass 1 cuts it inside the refused body
     * and again inside the accepted stream's header, so the realignment has to
     * survive fragment boundaries in both halves. */
    for (pass = 0; pass < 2; pass++) {
        RoundLogSink sink;
        BulkReceiver rcv;
        BulkRecvSink rsink;

        memset(&sink, 0, sizeof(sink));
        sink.acceptGen = RL_ACCEPT_GEN;
        rsink.onBegin = rlOnBegin;
        rsink.onComplete = rlOnComplete;
        rsink.ctx = &sink;
        bulkReceiverInit(&rcv);

        if (pass == 0) {
            bulkReceiverFeed(&rcv, run, runLen, &rsink);
        } else {
            uint32_t cut1 = accStart - (RL_REJ_BODY / 2);  /* mid refused body */
            uint32_t cut2 = accStart + 4;                  /* mid next header  */
            bulkReceiverFeed(&rcv, run, cut1, &rsink);
            bulkReceiverFeed(&rcv, run + cut1, cut2 - cut1, &rsink);
            bulkReceiverFeed(&rcv, run + cut2, runLen - cut2, &rsink);
        }

        if (sink.begins != 2) {
            UT_FAIL("pass %d: %d headers parsed, want 2", pass, sink.begins);
        }
        if (sink.rejects != 1) {
            UT_FAIL("pass %d: %d rejections, want 1", pass, sink.rejects);
        }
        /* The whole point: the refused body was consumed and discarded, so the
         * stream that followed it still lands whole. */
        if (sink.completions != 1) {
            UT_FAIL("pass %d: %d completions after a rejected stream, want 1",
                    pass, sink.completions);
        }
        if (sink.kind != BULK_KIND_ROUND_LOG || sink.gen != RL_ACCEPT_GEN ||
            sink.len != RL_ACC_BODY || strcmp(sink.path, kAcceptPath) != 0) {
            UT_FAIL("pass %d: the accepted stream's header did not survive the "
                    "rejected one", pass);
        }
        if (memcmp(sink.blob, accepted, RL_ACC_BODY) != 0) {
            UT_FAIL("pass %d: the accepted stream's body was corrupted by the "
                    "rejected one", pass);
        }
    }
    return 0;
}

/* ---- case 3: a server that cannot serve refuses over the real wire ---- */

#define RL_CONNECT_MAX 2000   /* join handshake + lobby map download */
#define RL_SETTLE_MAX  2000   /* request -> refusal round trip, clean path */

static bool rlPredConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool rlPredSettled(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetRoundLogState(h->cs) != CLIENT_ROUND_LOG_WAITING;
}

int run_round_log_refused_when_unavailable(void) {
    LoopbackHarness h;
    int connectedAt, settledAt, state;
    size_t len = 1;      /* the take must zero this even when it hands back NULL */
    uint8_t *blob;

    memset(&h, 0, sizeof(h));
    if (!loopbackHarnessStart(&h, "RoundLog", /*lobbyMode*/ true,
                              /*impairSpec*/ NULL, /*seed*/ 0x0D10610Bu)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (round log) failed");
    }

    connectedAt = loopbackHarnessPumpUntil(&h, RL_CONNECT_MAX, rlPredConnected,
                                           NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps",
                RL_CONNECT_MAX);
    }

    /* Nothing asked for yet. */
    state = clientSimGetRoundLogState(h.cs);
    if (state != (int)CLIENT_ROUND_LOG_IDLE) {
        loopbackHarnessStop(&h);
        UT_FAIL("round-log state %d before any request, want IDLE (%d)",
                state, (int)CLIENT_ROUND_LOG_IDLE);
    }

    /* The real PACKET_ROUND_LOG_REQ goes out here. */
    clientSimNetSendRoundLogRequest(h.cs);
    state = clientSimGetRoundLogState(h.cs);
    if (state != (int)CLIENT_ROUND_LOG_WAITING) {
        loopbackHarnessStop(&h);
        UT_FAIL("state %d after the request, want WAITING (%d)", state,
                (int)CLIENT_ROUND_LOG_WAITING);
    }

    settledAt = loopbackHarnessPumpUntil(&h, RL_SETTLE_MAX, rlPredSettled, NULL);
    state = clientSimGetRoundLogState(h.cs);
    fprintf(stderr, "  loopback round log: connected@%d settled@%d state=%d\n",
            connectedAt, settledAt, state);
    if (settledAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the request never left WAITING within %d pumps",
                RL_SETTLE_MAX);
    }

    /* No RoundLogSource is registered on this server, so its gate answers
     * ROUND_LOG_ERR_DISABLED and the client's code dispatch parks it here. */
    if (state != (int)CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED) {
        loopbackHarnessStop(&h);
        UT_FAIL("round-log state %d, want UNAVAILABLE_DISABLED (%d)", state,
                (int)CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED);
    }

    blob = clientSimTakeRoundLog(h.cs, &len);
    if (blob != NULL) {
        free(blob);
        loopbackHarnessStop(&h);
        UT_FAIL("the take handed back a blob after a refusal");
    }
    if (len != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the take left len at %u, want 0", (unsigned)len);
    }
    /* A refusal is not cleared by asking for the blob — the state has to stay
     * put so a caller can word it. */
    if (clientSimGetRoundLogState(h.cs) !=
        (int)CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED) {
        loopbackHarnessStop(&h);
        UT_FAIL("the take reset a refused state");
    }
    if (clientSimGetRoundLogPercent(h.cs) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("percent %u after a refusal, want 0",
                (unsigned)clientSimGetRoundLogPercent(h.cs));
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during the round-log exchange");
    }

    loopbackHarnessStop(&h);
    return 0;
}
