/*
 * Off-socket proof for the ChannelMux reliability primitive
 * (src/bolo/channel_mux.c). Two ChannelMux instances are driven through
 * an in-test loss/reorder/dup shuttle — no sockets, no threads. All
 * randomness is drawn from bolo_rand under a fixed bolo_srand seed so
 * each case is deterministic and reproducible.
 *
 * The matrix proves the full reliability burden: clean delivery, loss,
 * reorder, duplication, burst loss, a multi-seed soak, window / flow
 * control, overflow, per-channel independence, the stream flavor, frame
 * coalescing under a tight budget, malformed-input rejection, and the
 * named live-play regressions (lobby-ack resend, seq-space across game
 * start, drop-don't-reset).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "channel_mux.h"
#include "bolo_rand.h"
#include "test_harness.h"

#define FRAME_BUDGET 1400
#define MAXFRAME     1600
#define PIPE_CAP     1024
#define LINK_RTT_MS  40   /* -> retransmit timeout of 4 ticks */

/* ---- a unidirectional in-flight frame pipe ---- */

typedef struct {
    uint8_t data[MAXFRAME];
    int     len;
    int     releaseTick;
} PipeFrame;

typedef struct {
    PipeFrame f[PIPE_CAP];
    int       n;
} Pipe;

/* Shuttle impairment knobs (all percentages are 0..100). */
typedef struct {
    int dropPctAB;   /* per-frame loss, a -> b               */
    int dropPctBA;   /* per-frame loss, b -> a (acks)        */
    int dupPct;      /* chance a delivered frame is doubled  */
    int maxJitter;   /* extra delivery delay 0..maxJitter    */
    int burstStart;  /* extra a -> b drop window start tick  */
    int burstLen;    /* extra a -> b drop window length      */
    int dropBAUntil; /* drop every b -> a frame for tick <   */
} Impair;

static void pipePush(Pipe *p, const uint8_t *buf, int len, int releaseTick) {
    if (len <= 0 || p->n >= PIPE_CAP) {
        return; /* a full pipe behaves as extra loss */
    }
    PipeFrame *fr = &p->f[p->n++];
    memcpy(fr->data, buf, (size_t)len);
    fr->len = len;
    fr->releaseTick = releaseTick;
}

/* Enqueue with loss / jitter / duplication. */
static void pipeEnqueue(Pipe *p, const uint8_t *buf, int len, int tick,
                        bool drop, int dupPct, int maxJitter) {
    if (drop) {
        return;
    }
    int jitter = (maxJitter > 0) ? (int)bolo_rand_below((uint32_t)maxJitter + 1)
                                 : 0;
    pipePush(p, buf, len, tick + jitter);
    if (dupPct > 0 && (int)bolo_rand_below(100) < dupPct) {
        int j2 = (maxJitter > 0)
                     ? (int)bolo_rand_below((uint32_t)maxJitter + 1)
                     : 0;
        pipePush(p, buf, len, tick + j2);
    }
}

/* Deliver every frame whose release tick has arrived; keep the rest. */
static void pipeDeliver(Pipe *p, ChannelMux *dst, int tick) {
    int w = 0;
    int r;
    for (r = 0; r < p->n; r++) {
        if (p->f[r].releaseTick <= tick) {
            channelRecvFrame(dst, p->f[r].data, p->f[r].len);
        } else {
            if (w != r) {
                p->f[w] = p->f[r];
            }
            w++;
        }
    }
    p->n = w;
}

/* ---- message payloads tag their index so order is checkable ---- */

static void putIdx(uint8_t *b, uint32_t i) {
    b[0] = (uint8_t)(i >> 24);
    b[1] = (uint8_t)(i >> 16);
    b[2] = (uint8_t)(i >> 8);
    b[3] = (uint8_t)i;
}
static uint32_t getIdx(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}
static uint16_t msgLenFor(uint32_t i) {
    return (uint16_t)(4 + (i % 17)); /* 4..20 bytes */
}

/* Per-channel send-side invariants that must hold at every observable
 * point: the cumulative ack never overruns nextSeq, and in-flight never
 * exceeds the window. */
static int checkSendInvariants(const ChannelMux *m, uint32_t window) {
    int ch;
    for (ch = 0; ch < CHANNEL_COUNT; ch++) {
        const ChannelState *c = &m->ch[ch];
        if (c->ackedSeq > c->nextSeq) {
            return 1;
        }
        if ((c->nextSeq - c->ackedSeq) > window) {
            return 1;
        }
    }
    return 0;
}

/* Drive `total` messages on one message channel from a -> b under the
 * given impairment, asserting in-order exactly-once delivery and the
 * send-side invariants every tick. Returns 0 on success. */
static int runMsgExchange(uint32_t window, uint8_t ch, int total,
                          Impair imp, int maxTicks, uint64_t seed) {
    bolo_srand(seed);
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    Pipe *ab = (Pipe *)calloc(1, sizeof(*ab));
    Pipe *ba = (Pipe *)calloc(1, sizeof(*ba));
    int rc = 1;
    if (!a || !b || !ab || !ba) {
        goto done;
    }
    channelMuxInit(a, window);
    channelMuxInit(b, window);

    uint32_t nextToSend = 0;
    uint32_t nextExpected = 0;
    int tick = 0;
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];

    while (nextExpected < (uint32_t)total && tick < maxTicks) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);

        /* Enqueue as many messages as the window currently allows. */
        while (nextToSend < (uint32_t)total) {
            uint8_t msg[CHANNEL_MAX_SEG];
            uint16_t mlen = msgLenFor(nextToSend);
            memset(msg, (int)(nextToSend & 0xff), mlen);
            putIdx(msg, nextToSend);
            if (!channelSend(a, ch, msg, mlen)) {
                break; /* window full for now */
            }
            nextToSend++;
        }

        /* a -> b (data) */
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        bool dropAB = false;
        if (imp.dropPctAB > 0 && (int)bolo_rand_below(100) < imp.dropPctAB) {
            dropAB = true;
        }
        if (imp.burstLen > 0 && tick >= imp.burstStart &&
            tick < imp.burstStart + imp.burstLen) {
            dropAB = true;
        }
        pipeEnqueue(ab, frame, la, tick, dropAB, imp.dupPct, imp.maxJitter);

        /* b -> a (acks) */
        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        bool dropBA = false;
        if (imp.dropPctBA > 0 && (int)bolo_rand_below(100) < imp.dropPctBA) {
            dropBA = true;
        }
        if (tick < imp.dropBAUntil) {
            dropBA = true;
        }
        pipeEnqueue(ba, frame, lb, tick, dropBA, imp.dupPct, imp.maxJitter);

        /* deliver due frames */
        pipeDeliver(ab, b, tick);
        pipeDeliver(ba, a, tick);

        /* drain in-order deliveries on b */
        uint16_t olen;
        while (channelReceive(b, ch, out, &olen)) {
            uint32_t idx = getIdx(out);
            if (idx != nextExpected) {
                goto done; /* out of order / gap / double delivery */
            }
            if (olen != msgLenFor(idx)) {
                goto done; /* wrong payload length */
            }
            nextExpected++;
        }

        if (checkSendInvariants(a, window)) {
            goto done;
        }
        tick++;
    }

    if (nextExpected != (uint32_t)total) {
        goto done; /* did not converge -> livelock / loss */
    }
    rc = 0;
done:
    free(a);
    free(b);
    free(ab);
    free(ba);
    return rc;
}

/* ---- clean path, with the caught-up "emits nothing" assertion ---- */

static int t_clean_path(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    if (runMsgExchange(16, CHANNEL_CONTROL, 200, imp, 5000, 0xC1EA41ULL)) {
        UT_FAIL("clean path did not deliver in order");
    }

    /* A caught-up exchange must go quiet: neither side emits acks or
     * segments once everything is delivered and acknowledged. */
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a, 16);
    channelMuxInit(b, 16);
    int tick;
    for (tick = 0; tick < 200; tick++) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        if (tick == 0) {
            uint8_t msg[4];
            putIdx(msg, 0);
            channelSend(a, CHANNEL_CONTROL, msg, 4);
        }
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, lb);
        uint16_t olen;
        while (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        }
    }
    /* Two more idle ticks: both sides must now emit empty frames
     * (the 2-byte ackCount=0/segCount=0 header). */
    channelTick(a, 250, LINK_RTT_MS);
    channelTick(b, 250, LINK_RTT_MS);
    if (channelBuildFrame(a, frame, FRAME_BUDGET) != 2) {
        goto done;
    }
    if (channelBuildFrame(b, frame, FRAME_BUDGET) != 2) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("caught-up channel did not go quiet");
    }
    return 0;
}

static int t_loss(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.dropPctAB = 30;
    imp.dropPctBA = 30;
    if (runMsgExchange(16, CHANNEL_GAME, 400, imp, 40000, 0x10551ULL)) {
        UT_FAIL("loss path did not recover every message in order");
    }
    return 0;
}

static int t_reorder(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.maxJitter = 5; /* jitter reorders frames without dropping them */
    if (runMsgExchange(16, CHANNEL_CONTROL, 400, imp, 20000, 0x9E04DEULL)) {
        UT_FAIL("reordered frames did not deliver in order");
    }
    return 0;
}

static int t_duplicate(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.dupPct = 60; /* frequent duplication, no loss */
    if (runMsgExchange(16, CHANNEL_MAP, 400, imp, 20000, 0xD0B1EULL)) {
        UT_FAIL("duplicated frames were not deduped");
    }
    return 0;
}

static int t_burst_loss(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.burstStart = 20;
    imp.burstLen = 40; /* 40 consecutive a->b frames vanish */
    if (runMsgExchange(16, CHANNEL_CONTROL, 300, imp, 40000, 0xB0451ULL)) {
        UT_FAIL("did not recover from a burst loss");
    }
    return 0;
}

static int t_soak(void) {
    int seed;
    for (seed = 1; seed <= 8; seed++) {
        Impair imp;
        memset(&imp, 0, sizeof(imp));
        imp.dropPctAB = 20;
        imp.dropPctBA = 20;
        imp.dupPct = 15;
        imp.maxJitter = 4;
        uint32_t window = 8 + (uint32_t)(seed % 24); /* 8..31 */
        if (runMsgExchange(window, (uint8_t)(seed % 3), 2000, imp, 200000,
                           0x50A4000ULL + (uint64_t)seed)) {
            UT_FAIL("combined soak failed at seed %d", seed);
        }
    }
    return 0;
}

/* ---- window / flow control + the full-tail-resend bound ---- */

static int parseSegCount(const uint8_t *buf, int len) {
    if (len < 1) {
        return -1;
    }
    int pos = 0;
    uint8_t ackCount = buf[pos++];
    if (pos + ackCount * 5 > len) {
        return -1;
    }
    pos += ackCount * 5;
    if (pos + 1 > len) {
        return -1;
    }
    return buf[pos];
}

static int t_window_flow(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    const uint32_t window = 4;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a, window);
    channelMuxInit(b, window);

    /* Fill the window: exactly `window` sends succeed, the next fails. */
    uint32_t i;
    for (i = 0; i < window; i++) {
        uint8_t msg[4];
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_CONTROL, msg, 4)) {
            goto done; /* should have fit */
        }
    }
    {
        uint8_t msg[4];
        putIdx(msg, window);
        if (channelSend(a, CHANNEL_CONTROL, msg, 4)) {
            goto done; /* window was full — must reject */
        }
    }

    /* Force a retransmit-timeout resend with no acks yet and assert the
     * full tail is at most one window of segments. */
    channelTick(a, 0, LINK_RTT_MS);
    channelBuildFrame(a, frame, FRAME_BUDGET); /* first (immediate) send */
    channelTick(a, 100, LINK_RTT_MS);          /* well past the RTO */
    int la = channelBuildFrame(a, frame, FRAME_BUDGET);
    int segs = parseSegCount(frame, la);
    if (segs < 0 || (uint32_t)segs > window) {
        goto done; /* resend must not exceed one window */
    }

    /* Acking frees the window so sends resume. Deliver to b, ack back. */
    int tick;
    for (tick = 101; tick < 200; tick++) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        int lx = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, lx);
        uint16_t olen;
        while (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        }
        int ly = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, ly);
    }
    /* The window has drained; a new send must now succeed. */
    {
        uint8_t msg[4];
        putIdx(msg, window);
        if (!channelSend(a, CHANNEL_CONTROL, msg, 4)) {
            goto done; /* ack should have freed the window */
        }
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("window / flow-control contract violated");
    }
    return 0;
}

static int t_overflow(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    int rc = 1;
    const uint32_t window = 6;
    if (!a) {
        goto done;
    }
    channelMuxInit(a, window);
    uint32_t i;
    for (i = 0; i < window; i++) {
        uint8_t msg[8] = {0};
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_CONTROL, msg, 8)) {
            goto done;
        }
    }
    /* Exceeding the window without acks is the defined false / disconnect
     * signal — never a silent drop or buffer corruption. */
    uint8_t msg[8] = {0};
    putIdx(msg, window);
    if (channelSend(a, CHANNEL_CONTROL, msg, 8)) {
        goto done;
    }
    if (checkSendInvariants(a, window)) {
        goto done; /* invariants must survive the rejected send */
    }
    if (a->ch[CHANNEL_CONTROL].nextSeq != window) {
        goto done; /* the rejected send must not have advanced state */
    }
    rc = 0;
done:
    free(a);
    if (rc) {
        UT_FAIL("overflow handling violated the contract");
    }
    return 0;
}

/* ---- usage errors: wrong flavor per channel ---- */

static int t_flavor_usage(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    int rc = 1;
    uint8_t msg[4] = {0, 0, 0, 0};
    if (!a) {
        goto done;
    }
    channelMuxInit(a, 8);
    /* message send on the stream channel is rejected */
    if (channelSend(a, CHANNEL_BULK, msg, 4)) {
        goto done;
    }
    /* stream send on a message channel is rejected */
    if (channelStreamSend(a, CHANNEL_CONTROL, msg, 4)) {
        goto done;
    }
    /* out-of-range channel id is rejected */
    if (channelSend(a, CHANNEL_COUNT, msg, 4)) {
        goto done;
    }
    /* an oversized message is rejected */
    uint8_t big[CHANNEL_MAX_SEG + 8];
    memset(big, 7, sizeof(big));
    if (channelSend(a, CHANNEL_CONTROL, big, CHANNEL_MAX_SEG + 1)) {
        goto done;
    }
    /* none of the rejected calls advanced any channel */
    int ch;
    for (ch = 0; ch < CHANNEL_COUNT; ch++) {
        if (a->ch[ch].nextSeq != 0) {
            goto done;
        }
    }
    rc = 0;
done:
    free(a);
    if (rc) {
        UT_FAIL("flavor usage errors not rejected cleanly");
    }
    return 0;
}

/* ---- multi-channel independence: total loss on ch0 must not stall ch1 ---- */

static int u16be(const uint8_t *b) {
    return ((int)b[0] << 8) | (int)b[1];
}

/* Rebuild a frame omitting every segment for dropCh; acks and all other
 * channels' segments are copied verbatim. Input is one we built, so it is
 * well-formed. */
static int stripChannelSegments(const uint8_t *in, int len, uint8_t dropCh,
                                uint8_t *out) {
    if (len < 1) {
        return 0;
    }
    int ip = 0, op = 0;
    uint8_t ackCount = in[ip++];
    out[op++] = ackCount;
    int i;
    for (i = 0; i < ackCount; i++) {
        memcpy(out + op, in + ip, 5);
        ip += 5;
        op += 5;
    }
    uint8_t segCount = in[ip++];
    int segCountPos = op;
    out[op++] = 0;
    uint8_t kept = 0;
    for (i = 0; i < segCount; i++) {
        uint8_t ch = in[ip];
        int slen = u16be(in + ip + 5);
        int reclen = 7 + slen;
        if (ch != dropCh) {
            memcpy(out + op, in + ip, (size_t)reclen);
            op += reclen;
            kept++;
        }
        ip += reclen;
    }
    out[segCountPos] = kept;
    return op;
}

static int t_multichannel_independence(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t stripped[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    const uint32_t window = 8;
    const int ch1Total = 300;
    if (!a || !b) {
        goto done;
    }
    bolo_srand(0xC0FFEEULL);
    channelMuxInit(a, window);
    channelMuxInit(b, window);

    /* Saturate ch0 with a full window of messages that will be stripped on
     * the wire — ch0 can never deliver. */
    uint32_t i;
    for (i = 0; i < window; i++) {
        uint8_t msg[4];
        putIdx(msg, i);
        channelSend(a, CHANNEL_GAME, msg, 4);
    }

    uint32_t ch1Sent = 0, ch1Recv = 0;
    int tick = 0;
    while (ch1Recv < (uint32_t)ch1Total && tick < 30000) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        while (ch1Sent < (uint32_t)ch1Total) {
            uint8_t msg[4];
            putIdx(msg, ch1Sent);
            if (!channelSend(a, CHANNEL_MAP, msg, 4)) {
                break;
            }
            ch1Sent++;
        }
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        int sl = stripChannelSegments(frame, la, CHANNEL_GAME, stripped);
        channelRecvFrame(b, stripped, sl); /* ch0 segments never arrive */
        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, lb);

        uint16_t olen;
        while (channelReceive(b, CHANNEL_MAP, out, &olen)) {
            if (getIdx(out) != ch1Recv) {
                goto done;
            }
            ch1Recv++;
        }
        /* ch0 must remain perpetually stuck at zero delivered. */
        if (channelReceive(b, CHANNEL_GAME, out, &olen)) {
            goto done;
        }
        tick++;
    }
    if (ch1Recv != (uint32_t)ch1Total) {
        goto done; /* ch1 stalled behind ch0 — head-of-line leak */
    }
    if (b->ch[CHANNEL_GAME].expectedSeq != 0) {
        goto done; /* ch0 should have delivered nothing */
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("a stalled channel blocked an independent one");
    }
    return 0;
}

/* ---- stream flavor: a large byte stream, lossy + reordered, byte-exact ---- */

static int t_stream(void) {
    bolo_srand(0x57EA11ULL);
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    Pipe *ab = (Pipe *)calloc(1, sizeof(*ab));
    Pipe *ba = (Pipe *)calloc(1, sizeof(*ba));
    const int STREAM_LEN = 40000; /* bulk-map sized, exceeds one window */
    uint8_t *src = (uint8_t *)malloc((size_t)STREAM_LEN);
    uint8_t *dst = (uint8_t *)malloc((size_t)STREAM_LEN);
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b || !ab || !ba || !src || !dst) {
        goto done;
    }
    int i;
    for (i = 0; i < STREAM_LEN; i++) {
        src[i] = (uint8_t)bolo_rand();
    }
    channelMuxInit(a, 32);
    channelMuxInit(b, 32);

    /* Hand the whole stream to the staging buffer up front. */
    if (!channelStreamSend(a, CHANNEL_BULK, src, (uint32_t)STREAM_LEN)) {
        goto done; /* 40000 fits the 64 KB staging buffer */
    }

    int got = 0;
    int tick = 0;
    while (got < STREAM_LEN && tick < 60000) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);

        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        bool dropAB = ((int)bolo_rand_below(100) < 20);
        pipeEnqueue(ab, frame, la, tick, dropAB, 10, 4);

        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        bool dropBA = ((int)bolo_rand_below(100) < 20);
        pipeEnqueue(ba, frame, lb, tick, dropBA, 10, 4);

        pipeDeliver(ab, b, tick);
        pipeDeliver(ba, a, tick);

        uint16_t olen;
        while (channelReceive(b, CHANNEL_BULK, out, &olen)) {
            if (got + olen > STREAM_LEN) {
                goto done; /* over-delivered */
            }
            memcpy(dst + got, out, olen);
            got += olen;
        }
        tick++;
    }
    if (got != STREAM_LEN) {
        goto done; /* did not reassemble the whole stream */
    }
    if (memcmp(src, dst, (size_t)STREAM_LEN) != 0) {
        goto done; /* not byte-identical */
    }
    rc = 0;
done:
    free(a);
    free(b);
    free(ab);
    free(ba);
    free(src);
    free(dst);
    if (rc) {
        UT_FAIL("stream reassembly was not byte-identical under loss");
    }
    return 0;
}

/* ---- coalescing / budget: tight budget emits a prefix, rest stays queued;
 *      an ack-only frame round-trips ---- */

static int t_coalescing_budget(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a, 32);
    channelMuxInit(b, 32);

    /* Queue several small messages, then build under a budget that fits
     * only a couple of segments. */
    uint32_t i;
    for (i = 0; i < 10; i++) {
        uint8_t msg[10];
        putIdx(msg, i);
        channelSend(a, CHANNEL_CONTROL, msg, 10);
    }
    channelTick(a, 0, LINK_RTT_MS);
    /* tight: ack header (1) + segCount (1) + a couple of 17-byte records. */
    int tight = 1 + 1 + 2 * (7 + 10);
    int la = channelBuildFrame(a, frame, tight);
    int segs = parseSegCount(frame, la);
    if (segs <= 0 || segs >= 10) {
        goto done; /* must emit a partial prefix, not everything */
    }
    channelRecvFrame(b, frame, la);

    /* The remainder must still flow on subsequent full-budget frames. */
    int tick;
    uint32_t recv = 0;
    for (tick = 1; tick < 200 && recv < 10; tick++) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        int lx = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, lx);
        uint16_t olen;
        while (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
            if (getIdx(out) != recv) {
                goto done;
            }
            recv++;
        }
        int ly = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, ly);
    }
    if (recv != 10) {
        goto done;
    }

    /* Ack-only frame: b has acks pending but no segments of its own. After
     * receiving b must produce a frame with ackCount>0 and segCount==0 that
     * round-trips into a and advances a's ackedSeq. */
    {
        ChannelMux *c = (ChannelMux *)malloc(sizeof(*c));
        ChannelMux *d = (ChannelMux *)malloc(sizeof(*d));
        if (!c || !d) {
            free(c);
            free(d);
            goto done;
        }
        channelMuxInit(c, 8);
        channelMuxInit(d, 8);
        uint8_t msg[4];
        putIdx(msg, 0);
        channelSend(c, CHANNEL_CONTROL, msg, 4);
        channelTick(c, 0, LINK_RTT_MS);
        channelTick(d, 0, LINK_RTT_MS);
        int lc = channelBuildFrame(c, frame, FRAME_BUDGET);
        channelRecvFrame(d, frame, lc);
        uint16_t olen;
        channelReceive(d, CHANNEL_CONTROL, out, &olen);
        int ld = channelBuildFrame(d, frame, FRAME_BUDGET);
        int ackSegs = parseSegCount(frame, ld);
        int ackOk = (frame[0] > 0 && ackSegs == 0); /* acks, no segments */
        channelRecvFrame(c, frame, ld);
        int advanced = (c->ch[CHANNEL_CONTROL].ackedSeq == 1);
        free(c);
        free(d);
        if (!ackOk || !advanced) {
            goto done;
        }
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("budget/coalescing or ack-only round-trip failed");
    }
    return 0;
}

/* ---- malformed input: random / truncated bytes never crash or over-read ---- */

static int t_malformed_input(void) {
    bolo_srand(0xBADBEEFULL);
    ChannelMux *m = (ChannelMux *)malloc(sizeof(*m));
    int rc = 1;
    if (!m) {
        goto done;
    }
    channelMuxInit(m, 16);

    int iter;
    for (iter = 0; iter < 20000; iter++) {
        uint8_t buf[80];
        int len = (int)bolo_rand_below(sizeof(buf) + 1); /* 0..80 */
        int i;
        for (i = 0; i < len; i++) {
            buf[i] = (uint8_t)bolo_rand();
        }
        int r = channelRecvFrame(m, buf, len);
        if (r > len) {
            goto done; /* consumed more than it was given — over-read */
        }
    }

    /* Hand-built frames truncated at every boundary must be rejected
     * rather than over-read. */
    uint8_t good[64];
    int gp = 0;
    good[gp++] = 1;        /* ackCount = 1                  */
    good[gp++] = CHANNEL_CONTROL;
    good[gp++] = 0;
    good[gp++] = 0;
    good[gp++] = 0;
    good[gp++] = 5;        /* ackedSeq = 5                  */
    good[gp++] = 1;        /* segCount = 1                  */
    good[gp++] = CHANNEL_CONTROL;
    good[gp++] = 0;
    good[gp++] = 0;
    good[gp++] = 0;
    good[gp++] = 0;        /* seq = 0                       */
    good[gp++] = 0;
    good[gp++] = 3;        /* len = 3                       */
    good[gp++] = 0xAA;
    good[gp++] = 0xBB;
    good[gp++] = 0xCC;
    int t;
    for (t = 0; t < gp; t++) {
        ChannelMux *fresh = (ChannelMux *)malloc(sizeof(*fresh));
        if (!fresh) {
            goto done;
        }
        channelMuxInit(fresh, 16);
        int r = channelRecvFrame(fresh, good, t); /* every prefix length */
        if (r > t) {
            free(fresh);
            goto done;
        }
        free(fresh);
    }
    /* The complete frame parses cleanly and consumes exactly its length. */
    if (channelRecvFrame(m, good, gp) != gp) {
        goto done;
    }
    rc = 0;
done:
    free(m);
    if (rc) {
        UT_FAIL("malformed input was not safely rejected");
    }
    return 0;
}

/* ---- named regression: lobby-ack resend ---- */

static int t_lobby_ack_resend(void) {
    /* Drop every ack frame for the first stretch while the sender keeps
     * retransmitting an already-delivered tail; once acks flow the unchanged
     * re-sent ack recovers it. Delivery stays exactly-once throughout. */
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.dropBAUntil = 120; /* ack channel dark for 120 ticks */
    if (runMsgExchange(8, CHANNEL_CONTROL, 100, imp, 40000, 0xACC2E5ULL)) {
        UT_FAIL("dropped-ack tail did not recover / double-delivered");
    }
    return 0;
}

/* ---- named regression: seq space persists across the lobby->running
 *      boundary, no wipe / adopt special-casing ---- */

static int t_seq_space_across_game_start(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    bolo_srand(0x57A47ULL);
    channelMuxInit(a, 16);
    channelMuxInit(b, 16);

    const int lobbyMsgs = 12;  /* messages before the phase boundary */
    const int runningMsgs = 12; /* messages after — same seq space     */
    const int total = lobbyMsgs + runningMsgs;

    uint32_t sent = 0, recv = 0;
    int tick = 0;
    /* The "game start" boundary is at sent == lobbyMsgs. There is no wipe,
     * no seq reset, no adopt call — the same ChannelMux carries the control
     * stream straight across. The test asserts the boundary is a no-op for
     * the channel: messages on both sides deliver in one continuous order. */
    while (recv < (uint32_t)total && tick < 20000) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        while (sent < (uint32_t)total) {
            uint8_t msg[4];
            putIdx(msg, sent);
            if (!channelSend(a, CHANNEL_CONTROL, msg, 4)) {
                break;
            }
            sent++;
        }
        /* mild loss to make the boundary crossing non-trivial */
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        bool drop = ((int)bolo_rand_below(100) < 25);
        if (!drop) {
            channelRecvFrame(b, frame, la);
        }
        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, lb);
        uint16_t olen;
        while (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
            if (getIdx(out) != recv) {
                goto done; /* a gap or reset at the boundary */
            }
            recv++;
        }
        tick++;
    }
    if (recv != (uint32_t)total) {
        goto done;
    }
    /* Seq space never restarted: nextSeq counted straight through. */
    if (a->ch[CHANNEL_CONTROL].nextSeq != (uint32_t)total) {
        goto done;
    }
    if (b->ch[CHANNEL_CONTROL].expectedSeq != (uint32_t)total) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("control seq space did not persist across game start");
    }
    return 0;
}

/* ---- named regression: drop-don't-reset ---- */

static int t_drop_dont_reset(void) {
    bolo_srand(0xD4097ULL);
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    Pipe *ab = (Pipe *)calloc(1, sizeof(*ab));
    Pipe *ba = (Pipe *)calloc(1, sizeof(*ba));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    const uint32_t window = 12;
    const int total = 500;
    if (!a || !b || !ab || !ba) {
        goto done;
    }
    channelMuxInit(a, window);
    channelMuxInit(b, window);

    uint32_t sent = 0, recv = 0;
    uint32_t prevAcked = 0, prevExpected = 0;
    int tick = 0;
    while (recv < (uint32_t)total && tick < 40000) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        while (sent < (uint32_t)total) {
            uint8_t msg[4];
            putIdx(msg, sent);
            if (!channelSend(a, CHANNEL_CONTROL, msg, 4)) {
                break;
            }
            sent++;
        }
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        pipeEnqueue(ab, frame, la, tick, (int)bolo_rand_below(100) < 35, 0, 3);
        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        pipeEnqueue(ba, frame, lb, tick, (int)bolo_rand_below(100) < 35, 0, 3);
        pipeDeliver(ab, b, tick);
        pipeDeliver(ba, a, tick);

        /* Invariants under heavy loss: ackedSeq never passes nextSeq and
         * never rewinds; expectedSeq never rewinds. A dropped segment must
         * not reset either counter. */
        uint32_t acked = a->ch[CHANNEL_CONTROL].ackedSeq;
        uint32_t nextS = a->ch[CHANNEL_CONTROL].nextSeq;
        uint32_t expd = b->ch[CHANNEL_CONTROL].expectedSeq;
        if (acked > nextS) {
            goto done;
        }
        if (acked < prevAcked || expd < prevExpected) {
            goto done; /* a counter rewound — a reset slipped in */
        }
        prevAcked = acked;
        prevExpected = expd;

        uint16_t olen;
        while (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
            if (getIdx(out) != recv) {
                goto done;
            }
            recv++;
        }
        tick++;
    }
    if (recv != (uint32_t)total) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    free(ab);
    free(ba);
    if (rc) {
        UT_FAIL("a dropped segment reset the queue / pushed ackedSeq past nextSeq");
    }
    return 0;
}

int run_channel_mux(void) {
    /* The control channel must carry its sequence space straight across a
     * lobby->running phase change with no reset; that is the load-bearing
     * regression, so it runs first. */
    if (t_seq_space_across_game_start()) {
        return 1;
    }
    if (t_clean_path()) {
        return 1;
    }
    if (t_loss()) {
        return 1;
    }
    if (t_reorder()) {
        return 1;
    }
    if (t_duplicate()) {
        return 1;
    }
    if (t_burst_loss()) {
        return 1;
    }
    if (t_window_flow()) {
        return 1;
    }
    if (t_overflow()) {
        return 1;
    }
    if (t_flavor_usage()) {
        return 1;
    }
    if (t_multichannel_independence()) {
        return 1;
    }
    if (t_stream()) {
        return 1;
    }
    if (t_coalescing_budget()) {
        return 1;
    }
    if (t_malformed_input()) {
        return 1;
    }
    if (t_lobby_ack_resend()) {
        return 1;
    }
    if (t_drop_dont_reset()) {
        return 1;
    }
    if (t_soak()) {
        return 1;
    }
    return 0;
}
