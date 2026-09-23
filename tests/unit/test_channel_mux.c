/*
 * Off-socket proof for the ChannelMux reliability primitive
 * (src/bolo/channel_mux.c). Two ChannelMux instances are driven through
 * an in-test loss/reorder/dup shuttle — no sockets, no threads. All
 * randomness is drawn from bolo_rand under a fixed bolo_srand seed so
 * each case is deterministic and reproducible.
 *
 * The matrix proves the full reliability burden: clean delivery, loss,
 * reorder, duplication, burst loss, a multi-seed soak, window / flow
 * control, overflow, hold-and-resume of a burst larger than one window,
 * per-channel independence, the stream flavor, frame
 * coalescing under a tight budget, a worst-case control event fitting one
 * datagram, malformed-input rejection, the named
 * live-play regressions (lobby-ack resend, seq-space across game start,
 * drop-don't-reset), the game-boundary baseline reset (send-tail drop,
 * coordinated send/recv truncation, buffer-before-lift, near-window
 * retransmit recovery, no-rewind), and the best-effort sequence-jump bound
 * (a top-of-space or far-ahead seq is refused, a forgery inside the allowance
 * is resynchronised away, an ordinary jump still evicts the oldest, delivery
 * survives the wrap at 0xFFFFFFFF).
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
/* A varied message length that always fits the channel's segment and leaves
 * room for the 4-byte index tag. On a 16-byte channel this yields 4..16; on a
 * 256-byte channel 4..20. */
static uint16_t msgLenFor(uint32_t i, uint16_t segSize) {
    uint16_t span = (uint16_t)(segSize - 4);
    if (span > 16) {
        span = 16;
    }
    return (uint16_t)(4 + (i % (span + 1)));
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
static int runMsgExchange(uint8_t ch, int total,
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
    channelMuxInit(a);
    channelMuxInit(b);
    uint32_t window = a->ch[ch].window;        /* this channel's own depth   */
    uint16_t segSize = (uint16_t)a->ch[ch].segSize;

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
            uint16_t mlen = msgLenFor(nextToSend, segSize);
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
            if (olen != msgLenFor(idx, segSize)) {
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
    if (runMsgExchange(CHANNEL_CONTROL, 200, imp, 5000, 0xC1EA41ULL)) {
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
    channelMuxInit(a);
    channelMuxInit(b);
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
    if (runMsgExchange(CHANNEL_GAME, 400, imp, 40000, 0x10551ULL)) {
        UT_FAIL("loss path did not recover every message in order");
    }
    return 0;
}

static int t_reorder(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.maxJitter = 5; /* jitter reorders frames without dropping them */
    if (runMsgExchange(CHANNEL_CONTROL, 400, imp, 20000, 0x9E04DEULL)) {
        UT_FAIL("reordered frames did not deliver in order");
    }
    return 0;
}

static int t_duplicate(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.dupPct = 60; /* frequent duplication, no loss */
    if (runMsgExchange(CHANNEL_MAP, 400, imp, 20000, 0xD0B1EULL)) {
        UT_FAIL("duplicated frames were not deduped");
    }
    return 0;
}

static int t_burst_loss(void) {
    Impair imp;
    memset(&imp, 0, sizeof(imp));
    imp.burstStart = 20;
    imp.burstLen = 40; /* 40 consecutive a->b frames vanish */
    if (runMsgExchange(CHANNEL_CONTROL, 300, imp, 40000, 0xB0451ULL)) {
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
        /* Cycle the three message channels so the soak exercises each one's
         * own window and segment size. */
        if (runMsgExchange((uint8_t)(seed % 3), 2000, imp, 200000,
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
    if (pos + ackCount * 9 > len) {
        return -1;
    }
    pos += ackCount * 9;
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
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    /* Drive CHANNEL_MAP and fill its own window. */
    const uint32_t window = a->ch[CHANNEL_MAP].window;

    /* Fill the window: exactly `window` sends succeed, the next fails. */
    uint32_t i;
    for (i = 0; i < window; i++) {
        uint8_t msg[4];
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_MAP, msg, 4)) {
            goto done; /* should have fit */
        }
    }
    {
        uint8_t msg[4];
        putIdx(msg, window);
        if (channelSend(a, CHANNEL_MAP, msg, 4)) {
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
        while (channelReceive(b, CHANNEL_MAP, out, &olen)) {
        }
        int ly = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, ly);
    }
    /* The window has drained; a new send must now succeed. */
    {
        uint8_t msg[4];
        putIdx(msg, window);
        if (!channelSend(a, CHANNEL_MAP, msg, 4)) {
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

/* A producer that holds its unsent remainder rather than treating a full
 * window as fatal gets everything across, in order and exactly once, as acks
 * free the window. This is the contract the server's map-event drain leans on:
 * it stops at the first refused send, leaves the rest in its hold buffer with
 * the cumulative ack un-advanced, and resumes at the same message on a later
 * snapshot. The burst deliberately exceeds one window, so the hold path is the
 * only way every message can arrive. */
static int t_hold_and_resume(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    const uint32_t total = 300;   /* > CHANNEL_MAP_WINDOW */
    uint32_t window;
    uint32_t queued = 0;          /* next index the producer still owes */
    uint32_t received = 0;        /* next index the consumer expects */
    int refused = 0;
    int tick;
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    window = a->ch[CHANNEL_MAP].window;
    if (total <= window) {
        goto done; /* the burst must outgrow the window to test anything */
    }

    /* First pass with no acks yet: exactly one window fits and the next send
     * is refused, leaving the remainder owed. */
    while (queued < total) {
        uint8_t msg[CHANNEL_MAX_SEG];
        memset(msg, 0, sizeof(msg));
        putIdx(msg, queued);
        if (!channelSend(a, CHANNEL_MAP, msg,
                         msgLenFor(queued, CHANNEL_MAP_SEG))) {
            refused = 1;
            break;
        }
        queued++;
    }
    if (!refused || queued != window) {
        goto done; /* the window did not bound the burst as expected */
    }

    /* Carry the exchange. Each tick re-offers the remainder, stopping at the
     * first refusal exactly as the drain does, then moves one frame each way
     * and takes delivery in order. */
    for (tick = 0; tick < 2000 && received < total; tick++) {
        int la, lb;
        uint16_t olen;
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        while (queued < total) {
            uint8_t msg[CHANNEL_MAX_SEG];
            memset(msg, 0, sizeof(msg));
            putIdx(msg, queued);
            if (!channelSend(a, CHANNEL_MAP, msg,
                             msgLenFor(queued, CHANNEL_MAP_SEG))) {
                break; /* still full — hold the rest for a later tick */
            }
            queued++;
        }
        if (checkSendInvariants(a, window)) {
            goto done;
        }
        la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
        while (channelReceive(b, CHANNEL_MAP, out, &olen)) {
            if (getIdx(out) != received) {
                goto done; /* out of order, a gap, or a duplicate delivered */
            }
            if (olen != msgLenFor(received, CHANNEL_MAP_SEG)) {
                goto done; /* payload boundary lost */
            }
            received++;
        }
        lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, lb);
    }
    if (queued != total || received != total) {
        goto done; /* the held remainder never resumed */
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("held remainder did not resume in order after the window freed "
                "(sent %u, received %u of %u)",
                (unsigned)queued, (unsigned)received, (unsigned)total);
    }
    return 0;
}

static int t_overflow(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    int rc = 1;
    if (!a) {
        goto done;
    }
    channelMuxInit(a);
    const uint32_t window = a->ch[CHANNEL_GAME].window;
    uint32_t i;
    for (i = 0; i < window; i++) {
        uint8_t msg[8] = {0};
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_GAME, msg, 8)) {
            goto done;
        }
    }
    /* Exceeding the window without acks is the defined false / disconnect
     * signal — never a silent drop or buffer corruption. The game channel
     * carries it; the control channel queues behind its window instead and
     * refuses only once that backlog is full. */
    uint8_t msg[8] = {0};
    putIdx(msg, window);
    if (channelSend(a, CHANNEL_GAME, msg, 8)) {
        goto done;
    }
    if (checkSendInvariants(a, window)) {
        goto done; /* invariants must survive the rejected send */
    }
    if (a->ch[CHANNEL_GAME].nextSeq != window) {
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
    channelMuxInit(a);
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
    /* a message past the control channel's larger segment is rejected */
    uint8_t big[CHANNEL_MAX_SEG + 8];
    memset(big, 7, sizeof(big));
    if (channelSend(a, CHANNEL_CONTROL, big, CHANNEL_CONTROL_SEG + 1)) {
        goto done;
    }
    /* each channel enforces its own segment: a send one byte over the game
     * or map channel's smaller segment is rejected even though it would fit
     * the control channel. */
    if (channelSend(a, CHANNEL_GAME, big, CHANNEL_GAME_SEG + 1)) {
        goto done;
    }
    if (channelSend(a, CHANNEL_MAP, big, CHANNEL_MAP_SEG + 1)) {
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
        memcpy(out + op, in + ip, 9);
        ip += 9;
        op += 9;
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

/* ---- a live bulk transfer does not starve the best-effort channels ----
 *
 * Frames used to fill in channel id order, which puts CHANNEL_BULK (3) ahead
 * of CHANNEL_GAME_EFFECT (4) and CHANNEL_VOICE (5). A map download has as
 * many stream segments ready as every frame of the tick will hold, so the
 * two best-effort channels were framed only when bulk ran out - which during
 * a download is never. Their rings are 64 and 8 deep and neither waits: what
 * is not framed is dropped by the next burst over it.
 *
 * channelBuildFrame now frames bulk last. This queues a stream big enough to
 * fill several frames, one tick's worth of effect segments and one voice
 * frame, and asserts the first frame carries the best-effort traffic - and
 * still carries bulk with what is left, so the ordering did not simply move
 * the starvation. */

/* Segments for one channel in a frame we built, or -1 when it will not
 * parse. */
static int countChannelSegments(const uint8_t *buf, int len, uint8_t want) {
    int pos = 0;
    int i;
    uint8_t ackCount;
    uint8_t segCount;
    int n = 0;

    if (len < 1) {
        return -1;
    }
    ackCount = buf[pos++];
    if (pos + ackCount * 9 + 1 > len) {
        return -1;
    }
    pos += ackCount * 9;
    segCount = buf[pos++];
    for (i = 0; i < segCount; i++) {
        int slen;
        if (pos + 7 > len) {
            return -1;
        }
        slen = u16be(buf + pos + 5);
        if (pos + 7 + slen > len) {
            return -1;
        }
        if (buf[pos] == want) {
            n++;
        }
        pos += 7 + slen;
    }
    return n;
}

static int t_best_effort_not_starved_by_bulk(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    uint8_t *blob = (uint8_t *)malloc(32768);
    uint8_t frame[MAXFRAME];
    uint8_t seg[16];
    int rc = 1;
    int i;
    int len;
    int fxSegs = 0, vxSegs = 0, bulkSegs = 0;

    if (!a || !blob) {
        goto done;
    }
    channelMuxInit(a);
    memset(blob, 0x5A, 32768);

    /* A transfer with more ready than any one frame can carry. */
    if (!channelStreamSend(a, CHANNEL_BULK, blob, 32768)) {
        goto done;
    }
    /* One tick's burst of effects, and one voice frame, as a running tick
     * raises them. */
    for (i = 0; i < 16; i++) {
        memset(seg, (uint8_t)i, sizeof(seg));
        if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, seg,
                                   (uint16_t)sizeof(seg))) {
            goto done;
        }
    }
    memset(seg, 0xC0, sizeof(seg));
    if (!channelSendBestEffort(a, CHANNEL_VOICE, seg, (uint16_t)sizeof(seg))) {
        goto done;
    }

    channelTick(a, 1, LINK_RTT_MS);
    len = channelBuildFrame(a, frame, FRAME_BUDGET);
    if (len < 2) {
        goto done;
    }
    fxSegs   = countChannelSegments(frame, len, CHANNEL_GAME_EFFECT);
    vxSegs   = countChannelSegments(frame, len, CHANNEL_VOICE);
    bulkSegs = countChannelSegments(frame, len, CHANNEL_BULK);
    if (fxSegs != 16 || vxSegs != 1 || bulkSegs <= 0) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(blob);
    if (rc) {
        UT_FAIL("one frame with a live bulk transfer carried %d of 16 effect "
                "segment(s), %d of 1 voice segment(s) and %d bulk segment(s) - "
                "the best-effort channels are framed before bulk, and bulk "
                "takes what is left", fxSegs, vxSegs, bulkSegs);
    }
    return 0;
}

static int t_multichannel_independence(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t stripped[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    const uint32_t ch0Backlog = 8;
    const int ch1Total = 300;
    if (!a || !b) {
        goto done;
    }
    bolo_srand(0xC0FFEEULL);
    channelMuxInit(a);
    channelMuxInit(b);

    /* Give ch0 a standing backlog of messages that will be stripped on the
     * wire — ch0 can never deliver and its tail keeps retransmitting. */
    uint32_t i;
    for (i = 0; i < ch0Backlog; i++) {
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
    channelMuxInit(a);
    channelMuxInit(b);

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
    channelMuxInit(a);
    channelMuxInit(b);

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
        channelMuxInit(c);
        channelMuxInit(d);
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

/* ---- worst-case control event fits one segment / one datagram ---- */

/* The largest control event (a full BRAIN_LIST is ~900 B as a channel
 * message) must be accepted, build as a single segment within the datagram
 * budget, and round-trip byte-identical — proving CHANNEL_CONTROL_SEG leaves
 * room under UDP_MAX_PAYLOAD once the packet header and channel-frame overhead
 * are charged. */
static int t_control_max_event_fits_wire(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    uint8_t *msg = (uint8_t *)malloc(CHANNEL_MAX_SEG);
    int rc = 1;
    /* The 8-byte packet header wraps the channel frame on the wire, so the
     * frame itself must fit UDP_MAX_PAYLOAD (1400) minus that header. */
    const int kPacketHeader = 8;
    const int kWireBudget = 1400 - kPacketHeader;
    const uint16_t worstCase = 900; /* full BRAIN_LIST channel message */
    if (!a || !b || !msg) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    if (worstCase > CHANNEL_CONTROL_SEG) {
        goto done; /* the segment must hold the worst-case event */
    }
    uint16_t i;
    for (i = 0; i < worstCase; i++) {
        msg[i] = (uint8_t)(i * 31 + 7);
    }

    /* channelSend accepts the worst-case event. */
    if (!channelSend(a, CHANNEL_CONTROL, msg, worstCase)) {
        goto done;
    }
    /* It builds as a single segment within the datagram budget ... */
    channelTick(a, 0, LINK_RTT_MS);
    int la = channelBuildFrame(a, frame, kWireBudget);
    if (parseSegCount(frame, la) != 1) {
        goto done; /* one event, one segment, one datagram */
    }
    if (la + kPacketHeader > 1400) {
        goto done; /* frame plus packet header must fit a UDP datagram */
    }
    /* ... and round-trips byte-identical. */
    if (channelRecvFrame(b, frame, la) != la) {
        goto done;
    }
    uint16_t olen = 0;
    if (!channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        goto done;
    }
    if (olen != worstCase || memcmp(out, msg, worstCase) != 0) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    free(msg);
    if (rc) {
        UT_FAIL("worst-case control event did not fit / round-trip on the wire");
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
    channelMuxInit(m);

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
    good[gp++] = 0;
    good[gp++] = 0;
    good[gp++] = 0;
    good[gp++] = 5;        /* highestSeen = 5 (no gap)      */
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
        channelMuxInit(fresh);
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
    if (runMsgExchange(CHANNEL_CONTROL, 100, imp, 40000, 0xACC2E5ULL)) {
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
    channelMuxInit(a);
    channelMuxInit(b);

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
    const int total = 500;
    if (!a || !b || !ab || !ba) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);

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

/* ---- baseline reset: forward truncation at a game boundary ---- */

/* Return the first segment's sequence number (via seqOut) and the segCount of
 * a built frame, or -1 on a malformed/short frame. */
static int firstSegInfo(const uint8_t *buf, int len, uint32_t *seqOut) {
    if (len < 1) {
        return -1;
    }
    int pos = 0;
    uint8_t ackCount = buf[pos++];
    if (pos + ackCount * 9 > len) {
        return -1;
    }
    pos += ackCount * 9;
    if (pos + 1 > len) {
        return -1;
    }
    uint8_t segCount = buf[pos++];
    if (segCount > 0) {
        if (pos + 7 > len) {
            return -1;
        }
        *seqOut = ((uint32_t)buf[pos + 1] << 24) | ((uint32_t)buf[pos + 2] << 16) |
                  ((uint32_t)buf[pos + 3] << 8) | (uint32_t)buf[pos + 4];
    }
    return segCount;
}

/* Drive a no-loss exchange until `total` messages have delivered in order on
 * channel `ch`, leaving the sender fully acked. tickIO carries the tick
 * forward so callers can keep driving the same pair. Returns 0 on success. */
static int pumpDeliver(ChannelMux *a, ChannelMux *b, uint8_t ch,
                       uint32_t total, int *tickIO) {
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    uint32_t sent = 0;
    int tick = *tickIO;
    int guard = 0;
    while (b->ch[ch].expectedSeq < total && guard++ < 100000) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        while (sent < total) {
            uint8_t msg[8];
            putIdx(msg, sent);
            if (!channelSend(a, ch, msg, 8)) {
                break;
            }
            sent++;
        }
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
        uint16_t olen;
        while (channelReceive(b, ch, out, &olen)) {
        }
        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, lb);
        tick++;
    }
    *tickIO = tick;
    return (b->ch[ch].expectedSeq == total &&
            a->ch[ch].ackedSeq == total) ? 0 : 1;
}

/* channelResetSend drops the dead tail: the unacked window collapses, nothing
 * is retransmitted past it, and new sends resume at the returned baseline. */
static int t_reset_drops_dead_tail(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    uint8_t frame[MAXFRAME];
    int rc = 1;
    if (!a) {
        goto done;
    }
    channelMuxInit(a);
    const uint32_t n = 10;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint8_t msg[8];
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_CONTROL, msg, 8)) {
            goto done;
        }
    }
    /* Transmit the tail once (nothing acked), so there is a live tail to drop. */
    channelTick(a, 0, LINK_RTT_MS);
    int la = channelBuildFrame(a, frame, FRAME_BUDGET);
    if (parseSegCount(frame, la) <= 0) {
        goto done; /* the tail should have gone out */
    }

    uint32_t baseline = channelResetSend(a, CHANNEL_CONTROL);
    if (baseline != n) {
        goto done;
    }
    if (a->ch[CHANNEL_CONTROL].ackedSeq != n ||
        a->ch[CHANNEL_CONTROL].nextSeq != n ||
        a->ch[CHANNEL_CONTROL].txNext != n) {
        goto done; /* window must have collapsed to empty */
    }

    /* Well past the RTO, the dropped tail must not reappear. */
    channelTick(a, 100, LINK_RTT_MS);
    la = channelBuildFrame(a, frame, FRAME_BUDGET);
    if (parseSegCount(frame, la) != 0) {
        goto done;
    }

    /* A new send continues from the baseline, not from a reused low seq. */
    uint8_t nm[8];
    putIdx(nm, 0x99);
    if (!channelSend(a, CHANNEL_CONTROL, nm, 8)) {
        goto done;
    }
    if (a->ch[CHANNEL_CONTROL].nextSeq != n + 1) {
        goto done;
    }
    channelTick(a, 200, LINK_RTT_MS);
    la = channelBuildFrame(a, frame, FRAME_BUDGET);
    uint32_t seq = 0;
    if (firstSegInfo(frame, la, &seq) != 1 || seq != baseline) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    if (rc) {
        UT_FAIL("send reset did not drop the dead tail / continue at baseline");
    }
    return 0;
}

/* A coordinated reset across two muxes: the sender truncates its send
 * baseline and the receiver lifts expected to match. A straggler from the old
 * game is then dropped by dedup; a new message at the baseline delivers once. */
static int t_reset_coordinated(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t strag[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);

    int tick = 0;
    if (pumpDeliver(a, b, CHANNEL_CONTROL, 5, &tick)) {
        goto done; /* previous game settled: 5 delivered and acked */
    }
    /* Old-game unacked tail: seq 5,6,7 sent but never delivered. */
    uint32_t i;
    for (i = 5; i < 8; i++) {
        uint8_t msg[8];
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_CONTROL, msg, 8)) {
            goto done;
        }
    }
    channelTick(a, (uint32_t)tick, LINK_RTT_MS);
    int sl = channelBuildFrame(a, strag, FRAME_BUDGET); /* captures 5,6,7 */
    uint32_t stragSeq = 0;
    if (firstSegInfo(strag, sl, &stragSeq) <= 0 || stragSeq != 5) {
        goto done;
    }

    /* Game-start coordinated reset. */
    uint32_t baseline = channelResetSend(a, CHANNEL_CONTROL);
    if (baseline != 8 || a->ch[CHANNEL_CONTROL].nextSeq != 8) {
        goto done;
    }
    channelResetExpected(b, CHANNEL_CONTROL, baseline);
    if (b->ch[CHANNEL_CONTROL].expectedSeq != 8) {
        goto done;
    }

    /* The captured old-game straggler arriving after the reset is dropped. */
    channelRecvFrame(b, strag, sl);
    uint16_t olen;
    if (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        goto done;
    }
    if (b->ch[CHANNEL_CONTROL].expectedSeq != 8) {
        goto done;
    }

    /* A new-game message at the baseline delivers exactly once, in order. */
    tick++;
    uint8_t nm[8];
    putIdx(nm, 0xABCD);
    if (!channelSend(a, CHANNEL_CONTROL, nm, 8) ||
        a->ch[CHANNEL_CONTROL].nextSeq != 9) {
        goto done;
    }
    channelTick(a, (uint32_t)tick, LINK_RTT_MS);
    int la = channelBuildFrame(a, frame, FRAME_BUDGET);
    channelRecvFrame(b, frame, la);
    int deliveries = 0;
    while (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        if (getIdx(out) != 0xABCD) {
            goto done;
        }
        deliveries++;
    }
    if (deliveries != 1) {
        goto done;
    }
    /* Replaying the straggler still dedups — no double delivery. */
    channelRecvFrame(b, strag, sl);
    if (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("coordinated reset mishandled straggler / new delivery");
    }
    return 0;
}

/* A new-baseline message buffered before the receiver lifts expected still
 * delivers once expected is raised; an old-seq straggler after the reset is
 * dropped. */
static int t_reset_buffer_before_lift(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t strag[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);

    int tick = 0;
    if (pumpDeliver(a, b, CHANNEL_CONTROL, 5, &tick)) {
        goto done; /* b->expectedSeq = 5, a fully acked */
    }
    /* Old tail seq 5,6,7 sent (will be the straggler), captured but not yet
     * delivered. */
    uint32_t i;
    for (i = 5; i < 8; i++) {
        uint8_t msg[8];
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_CONTROL, msg, 8)) {
            goto done;
        }
    }
    channelTick(a, (uint32_t)tick, LINK_RTT_MS);
    int sl = channelBuildFrame(a, strag, FRAME_BUDGET); /* 5,6,7 */

    uint32_t baseline = channelResetSend(a, CHANNEL_CONTROL);
    if (baseline != 8) {
        goto done;
    }

    /* New-game message lands at the baseline (seq 8) and is delivered to the
     * receiver while its expected is still 5 — buffered, not yet deliverable. */
    uint8_t nm[8];
    putIdx(nm, 0x42);
    if (!channelSend(a, CHANNEL_CONTROL, nm, 8)) {
        goto done;
    }
    tick++;
    channelTick(a, (uint32_t)tick, LINK_RTT_MS);
    int la = channelBuildFrame(a, frame, FRAME_BUDGET);
    channelRecvFrame(b, frame, la);
    uint16_t olen;
    if (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        goto done; /* gap at 5..7 — nothing yet */
    }
    if (b->ch[CHANNEL_CONTROL].expectedSeq != 5) {
        goto done;
    }

    /* Lifting expected to the baseline makes the buffered seq-8 deliverable. */
    channelResetExpected(b, CHANNEL_CONTROL, baseline);
    if (b->ch[CHANNEL_CONTROL].expectedSeq != 8) {
        goto done;
    }
    int deliveries = 0;
    while (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        if (getIdx(out) != 0x42) {
            goto done;
        }
        deliveries++;
    }
    if (deliveries != 1 || b->ch[CHANNEL_CONTROL].expectedSeq != 9) {
        goto done;
    }

    /* The old-seq straggler arriving after the reset is dropped. */
    channelRecvFrame(b, strag, sl);
    if (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("buffered-before-lift delivery or post-reset straggler failed");
    }
    return 0;
}

/* Window-bound reset: a near-window stale tail on the game channel. With the
 * gap (baseline - oldExpected) at a full window, a new-game event at the
 * baseline that arrives BEFORE the receiver lifts expected falls outside the
 * live window and is dropped — not silently lost: the sender's retransmit
 * recovers it once expected is lifted, delivering exactly once. */
static int t_reset_window_bound_recovery(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    const uint8_t ch = CHANNEL_GAME;
    const uint32_t window = a->ch[ch].window;   /* CHANNEL_GAME_WINDOW */
    const uint32_t base = 4;                     /* a few delivered first */

    int tick = 0;
    if (pumpDeliver(a, b, ch, base, &tick)) {
        goto done; /* both settled at `base` */
    }

    /* Fill the send window with an undelivered old-game tail: exactly `window`
     * messages from seq `base`, none delivered to b (b->expectedSeq stays
     * `base`). The next send must be refused — the window is full. */
    uint32_t i;
    for (i = 0; i < window; i++) {
        uint8_t msg[8];
        putIdx(msg, base + i);
        if (!channelSend(a, ch, msg, 8)) {
            goto done; /* the whole window should fit */
        }
    }
    {
        uint8_t msg[8];
        putIdx(msg, base + window);
        if (channelSend(a, ch, msg, 8)) {
            goto done; /* window full — must refuse */
        }
    }

    /* Game-start reset: the tail collapses, the baseline is one window above the
     * receiver's still-current expected. */
    uint32_t baseline = channelResetSend(a, ch);
    if (baseline != base + window) {
        goto done;
    }
    if (b->ch[ch].expectedSeq != base) {
        goto done; /* receiver hasn't lifted yet */
    }

    /* A new-game event lands at the baseline and is delivered to b while its
     * expected is still `base` — the gap is a full window, so it cannot be
     * buffered (it would alias a live slot) and must be dropped, not delivered
     * out of order. */
    uint8_t nm[8];
    putIdx(nm, 0xBEEF);
    if (!channelSend(a, ch, nm, 8) || a->ch[ch].nextSeq != base + window + 1) {
        goto done;
    }
    tick++;
    channelTick(a, (uint32_t)tick, LINK_RTT_MS);
    int la = channelBuildFrame(a, frame, FRAME_BUDGET);
    channelRecvFrame(b, frame, la);
    uint16_t olen;
    if (channelReceive(b, ch, out, &olen)) {
        goto done; /* nothing is deliverable across a full-window gap */
    }
    if (b->ch[ch].expectedSeq != base) {
        goto done; /* the out-of-window seg must not have advanced expected */
    }

    /* Lift the receive baseline (the CTRL_CHANNEL_RESET apply). The seq-baseline
     * event was dropped, not buffered, so nothing delivers immediately. */
    channelResetExpected(b, ch, baseline);
    if (b->ch[ch].expectedSeq != baseline) {
        goto done;
    }
    if (channelReceive(b, ch, out, &olen)) {
        goto done; /* recovery is by retransmit, not an instant delivery */
    }

    /* The sender keeps retransmitting the unacked baseline event; once it
     * arrives within the lifted window it delivers exactly once. */
    int deliveries = 0;
    for (tick++; tick < 400 && deliveries == 0; tick++) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        int lx = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, lx);
        while (channelReceive(b, ch, out, &olen)) {
            if (getIdx(out) != 0xBEEF) {
                goto done; /* wrong event recovered */
            }
            deliveries++;
        }
        int ly = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, ly);
    }
    if (deliveries != 1) {
        goto done; /* silent drop, or a double delivery */
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("near-window reset did not recover the baseline event by retransmit");
    }
    return 0;
}

/* channelResetExpected never rewinds: a value at or below the current
 * expected is a no-op, and the send-side invariants survive a reset. */
static int t_reset_no_rewind(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);

    int tick = 0;
    if (pumpDeliver(a, b, CHANNEL_CONTROL, 10, &tick)) {
        goto done; /* b->expectedSeq = 10 */
    }
    channelResetExpected(b, CHANNEL_CONTROL, 5); /* below current — no-op */
    if (b->ch[CHANNEL_CONTROL].expectedSeq != 10) {
        goto done;
    }
    channelResetExpected(b, CHANNEL_CONTROL, 10); /* equal — no-op */
    if (b->ch[CHANNEL_CONTROL].expectedSeq != 10) {
        goto done;
    }
    if (checkSendInvariants(a, a->ch[CHANNEL_CONTROL].window) ||
        checkSendInvariants(b, b->ch[CHANNEL_CONTROL].window)) {
        goto done;
    }
    /* A send reset keeps ackedSeq <= nextSeq. */
    uint32_t baseline = channelResetSend(a, CHANNEL_CONTROL);
    if (baseline != 10 ||
        a->ch[CHANNEL_CONTROL].ackedSeq > a->ch[CHANNEL_CONTROL].nextSeq) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("expected reset rewound or broke the send invariants");
    }
    return 0;
}

/* ---- best-effort flavor: deliver-on-arrival, no head-of-line wait ---- */

/* An out-of-order best-effort delivery never waits on a gap, and the sender
 * retains nothing once a segment has been framed once. */
static int t_best_effort_no_hol(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    channelTick(a, 0, LINK_RTT_MS);
    channelTick(b, 0, LINK_RTT_MS);

    /* Index 0 delivers cleanly. */
    putIdx(msg, 0);
    if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, msg, 8)) {
        goto done;
    }
    {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
    }
    uint16_t olen;
    if (!channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen) ||
        getIdx(out) != 0) {
        goto done;
    }
    if (channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen)) {
        goto done; /* nothing more buffered */
    }

    /* Indices 1 and 2 are framed but their frame is dropped (simulated loss). */
    putIdx(msg, 1);
    if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, msg, 8)) {
        goto done;
    }
    putIdx(msg, 2);
    if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, msg, 8)) {
        goto done;
    }
    channelBuildFrame(a, frame, FRAME_BUDGET); /* built, never delivered */

    /* Index 3 is framed and delivered: the gap at 1,2 is skipped, not waited. */
    putIdx(msg, 3);
    if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, msg, 8)) {
        goto done;
    }
    {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
    }
    if (!channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen) ||
        getIdx(out) != 3) {
        goto done; /* delivered past the gap, did not stall */
    }
    if (channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen)) {
        goto done;
    }
    if (b->ch[CHANNEL_GAME_EFFECT].expectedSeq != 4) {
        goto done; /* cursor advanced past the skipped 1,2 */
    }

    /* The sender retained nothing: every framed seq is acked-to-self, the
     * window is empty, and the transmit cursor sits at nextSeq. */
    if (a->ch[CHANNEL_GAME_EFFECT].ackedSeq !=
            a->ch[CHANNEL_GAME_EFFECT].nextSeq ||
        a->ch[CHANNEL_GAME_EFFECT].nextSeq != 4 ||
        a->ch[CHANNEL_GAME_EFFECT].txNext !=
            a->ch[CHANNEL_GAME_EFFECT].nextSeq) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("best-effort waited on a gap or retained a framed segment");
    }
    return 0;
}

/* Overflow on the best-effort ring never fails the send: the oldest pending
 * segment is dropped to make room, so exactly one window of the newest
 * segments survives to be delivered. */
static int t_best_effort_overflow_drops_oldest(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    const uint32_t window = a->ch[CHANNEL_GAME_EFFECT].window;
    const uint32_t EXTRA = 10;

    /* Overflow the ring by EXTRA: every send must succeed (overflow drops the
     * oldest, never returns false). No frame is built during the loop, so the
     * ring genuinely overflows rather than draining. */
    uint32_t i;
    for (i = 0; i < window + EXTRA; i++) {
        putIdx(msg, i);
        if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, msg, 8)) {
            goto done; /* overflow must never fail the send */
        }
    }
    if (a->ch[CHANNEL_GAME_EFFECT].nextSeq != window + EXTRA) {
        goto done;
    }
    if (a->ch[CHANNEL_GAME_EFFECT].nextSeq -
            a->ch[CHANNEL_GAME_EFFECT].ackedSeq != window) {
        goto done; /* in-flight pinned at exactly one window */
    }

    /* Drain a's surviving ring to b. One frame may not hold all surviving
     * segments under FRAME_BUDGET, so keep building until the transmit cursor
     * catches up to nextSeq. */
    while (a->ch[CHANNEL_GAME_EFFECT].txNext !=
           a->ch[CHANNEL_GAME_EFFECT].nextSeq) {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        if (la == 2) {
            break; /* empty frame — nothing left to drain */
        }
        channelRecvFrame(b, frame, la);
    }

    /* The surviving window delivers: exactly `window` messages, the oldest
     * EXTRA dropped, indices strictly ascending from EXTRA to window+EXTRA-1. */
    uint16_t olen;
    uint32_t count = 0;
    uint32_t prev = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    while (channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen)) {
        uint32_t idx = getIdx(out);
        if (count == 0) {
            first = idx;
        } else if (idx <= prev) {
            goto done; /* not strictly ascending */
        }
        prev = idx;
        last = idx;
        count++;
    }
    if (count != window || first != EXTRA || last != window + EXTRA - 1) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("best-effort overflow did not drop the oldest / failed a send");
    }
    return 0;
}

/* A duplicated best-effort frame delivers each seq once; a replayed (stale)
 * frame delivers nothing. */
static int t_best_effort_stale_and_dup(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    int frameLen;
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    channelTick(a, 0, LINK_RTT_MS);
    channelTick(b, 0, LINK_RTT_MS);

    uint32_t i;
    for (i = 0; i < 3; i++) {
        putIdx(msg, i);
        if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, msg, 8)) {
            goto done;
        }
    }
    frameLen = channelBuildFrame(a, frame, FRAME_BUDGET); /* carries 0,1,2 */

    /* Deliver the same frame twice before draining: the duplicate dedups. */
    channelRecvFrame(b, frame, frameLen);
    channelRecvFrame(b, frame, frameLen);

    uint16_t olen;
    uint32_t count = 0;
    while (channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen)) {
        if (getIdx(out) != count) {
            goto done; /* wrong index / out of order */
        }
        count++;
    }
    if (count != 3 || b->ch[CHANNEL_GAME_EFFECT].expectedSeq != 3) {
        goto done;
    }

    /* Replaying the frame now is stale (every seq is below the cursor): it
     * delivers nothing and does not move the cursor. */
    channelRecvFrame(b, frame, frameLen);
    if (channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen)) {
        goto done;
    }
    if (b->ch[CHANNEL_GAME_EFFECT].expectedSeq != 3) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("best-effort dup/stale handling violated exactly-once-or-drop");
    }
    return 0;
}

/* Best-effort usage errors are rejected and advance no channel: a non
 * best-effort channel id, a bad id, and an oversized message all return false,
 * and the receive guard rejects a non best-effort / bad id too. */
static int t_best_effort_usage(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    uint8_t msg[8] = {0};
    uint8_t big[CHANNEL_GAME_EFFECT_SEG + 1];
    uint8_t out[CHANNEL_MAX_SEG];
    int rc = 1;
    if (!a) {
        goto done;
    }
    channelMuxInit(a);
    memset(big, 0, sizeof(big));

    /* A best-effort send on a non best-effort channel is a usage error. */
    if (channelSendBestEffort(a, CHANNEL_GAME, msg, 8)) {
        goto done;
    }
    if (channelSendBestEffort(a, CHANNEL_CONTROL, msg, 8)) {
        goto done;
    }
    if (channelSendBestEffort(a, CHANNEL_BULK, msg, 8)) {
        goto done;
    }
    /* Out-of-range channel id is rejected. */
    if (channelSendBestEffort(a, CHANNEL_COUNT, msg, 8)) {
        goto done;
    }
    /* A message one byte past the best-effort segment is rejected. */
    if (channelSendBestEffort(a, CHANNEL_GAME_EFFECT, big,
                              CHANNEL_GAME_EFFECT_SEG + 1)) {
        goto done;
    }

    /* The receive guard rejects a non best-effort and a bad id. */
    uint16_t olen;
    if (channelReceiveBestEffort(a, CHANNEL_GAME, out, &olen)) {
        goto done;
    }
    if (channelReceiveBestEffort(a, CHANNEL_COUNT, out, &olen)) {
        goto done;
    }

    /* None of the rejected calls advanced any channel. */
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
        UT_FAIL("best-effort usage errors not rejected cleanly");
    }
    return 0;
}

/* ---- best-effort sequence jumps: bounded, and safe across the wrap ---- */

/* Mirrors CHANNEL_BEST_EFFORT_JUMP_WINDOWS in channel_mux.c: a best-effort
 * receiver follows a forward jump of up to this many of its own windows on one
 * segment's say-so, and needs a second agreeing segment past that. */
#define BE_JUMP_WINDOWS 4u

/* Hand-build a one-segment frame (no acks) carrying an arbitrary sequence
 * number. channelBuildFrame can only emit the numbers its own sender reached,
 * so a hostile or corrupted seq has to be written out by hand. */
static int makeSegFrame(uint8_t *buf, uint8_t ch, uint32_t seq,
                        const uint8_t *payload, uint16_t len) {
    int pos = 0;
    buf[pos++] = 0;  /* ackCount */
    buf[pos++] = 1;  /* segCount */
    buf[pos++] = ch;
    buf[pos++] = (uint8_t)(seq >> 24);
    buf[pos++] = (uint8_t)(seq >> 16);
    buf[pos++] = (uint8_t)(seq >> 8);
    buf[pos++] = (uint8_t)seq;
    buf[pos++] = (uint8_t)(len >> 8);
    buf[pos++] = (uint8_t)len;
    memcpy(buf + pos, payload, len);
    return pos + (int)len;
}

/* One segment claiming the top of the sequence space must not wedge the
 * channel: the receiver refuses to follow it, keeps its cursor, and ordinary
 * traffic keeps delivering afterwards. There is no reset for a best-effort
 * channel, so a cursor moved up there would silence it for the connection. */
static int t_best_effort_absurd_seq_no_wedge(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t olen;
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    channelTick(a, 0, LINK_RTT_MS);
    channelTick(b, 0, LINK_RTT_MS);

    /* Ordinary voice traffic first: seq 0 delivers, the cursor sits at 1. */
    putIdx(msg, 0);
    if (!channelSendBestEffort(a, CHANNEL_VOICE, msg, 8)) {
        goto done;
    }
    {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
    }
    if (!channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen) ||
        getIdx(out) != 0) {
        goto done;
    }

    /* One datagram claiming 0xFFFFFFFF. */
    putIdx(msg, 999);
    {
        int lf = makeSegFrame(frame, CHANNEL_VOICE, 0xFFFFFFFFu, msg, 8);
        channelRecvFrame(b, frame, lf);
    }
    if (channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen)) {
        goto done; /* the segment was refused, so nothing is deliverable */
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != 1) {
        goto done; /* the cursor did not follow it */
    }

    /* The channel still works: the next real segments deliver in order. */
    uint32_t i;
    for (i = 1; i <= 3; i++) {
        putIdx(msg, i);
        if (!channelSendBestEffort(a, CHANNEL_VOICE, msg, 8)) {
            goto done;
        }
    }
    {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
    }
    for (i = 1; i <= 3; i++) {
        if (!channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen) ||
            getIdx(out) != i) {
            goto done;
        }
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != 4) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("a top-of-space seq wedged the best-effort channel");
    }
    return 0;
}

/* A jump far past the cursor is refused without disturbing the channel: what
 * was already buffered still delivers, in order, and the refused segment is
 * not among it. */
static int t_best_effort_far_jump_refused(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t olen;
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    channelTick(a, 0, LINK_RTT_MS);
    channelTick(b, 0, LINK_RTT_MS);

    /* Two real segments arrive and stay buffered (nothing popped yet). */
    uint32_t i;
    for (i = 0; i < 2; i++) {
        putIdx(msg, i);
        if (!channelSendBestEffort(a, CHANNEL_GAME_EFFECT, msg, 8)) {
            goto done;
        }
    }
    {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
    }

    /* A seq a billion past the cursor — no sender reaches that on a connection
     * that drops itself after one timeout of silence. */
    putIdx(msg, 777);
    {
        int lf = makeSegFrame(frame, CHANNEL_GAME_EFFECT, 0x40000000u, msg, 8);
        channelRecvFrame(b, frame, lf);
    }

    for (i = 0; i < 2; i++) {
        if (!channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen) ||
            getIdx(out) != i) {
            goto done; /* the buffered segments survived the refusal */
        }
    }
    if (channelReceiveBestEffort(b, CHANNEL_GAME_EFFECT, out, &olen)) {
        goto done; /* the refused segment was not buffered */
    }
    if (b->ch[CHANNEL_GAME_EFFECT].expectedSeq != 2) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("a far-ahead seq was followed or cost the buffered segments");
    }
    return 0;
}

/* A forgery does not have to be absurd to strand the cursor: one inside the
 * jump allowance is followed, which leaves the real sender behind the cursor
 * and every one of its segments refused. Two of those segments agree with each
 * other, so the cursor re-bases onto them and the channel delivers again —
 * no cursor position outlives the traffic. */
static int t_best_effort_in_allowance_forgery_recovers(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t olen;
    int rc = 1;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);
    channelTick(a, 0, LINK_RTT_MS);
    channelTick(b, 0, LINK_RTT_MS);
    const uint32_t window = b->ch[CHANNEL_VOICE].window;

    /* Ordinary traffic: seqs 0..2 delivered, the cursor sits at 3. */
    uint32_t i;
    for (i = 0; i < 3; i++) {
        putIdx(msg, i);
        if (!channelSendBestEffort(a, CHANNEL_VOICE, msg, 8)) {
            goto done;
        }
    }
    {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
    }
    for (i = 0; i < 3; i++) {
        if (!channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen) ||
            getIdx(out) != i) {
            goto done;
        }
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != 3) {
        goto done;
    }

    /* One forged datagram, as far ahead as the allowance permits. Following it
     * is what a single segment inside the allowance is allowed to do. */
    uint32_t forged = 3 + window * BE_JUMP_WINDOWS - 1;
    putIdx(msg, 999);
    {
        int lf = makeSegFrame(frame, CHANNEL_VOICE, forged, msg, 8);
        channelRecvFrame(b, frame, lf);
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != forged - window + 1) {
        goto done; /* the cursor is now well ahead of the real sender */
    }
    while (channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen)) {
    }

    /* The real sender carries on from seq 3. Its first segment is refused and
     * recorded, the second agrees with it and re-bases the cursor, and the
     * rest deliver — one segment lost to the resync, not the channel. */
    for (i = 3; i < 9; i++) {
        putIdx(msg, i);
        if (!channelSendBestEffort(a, CHANNEL_VOICE, msg, 8)) {
            goto done;
        }
    }
    {
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
    }
    for (i = 4; i < 9; i++) {
        if (!channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen) ||
            getIdx(out) != i) {
            goto done;
        }
    }
    if (channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen)) {
        goto done;
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != 9) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("a forgery inside the allowance kept the best-effort channel");
    }
    return 0;
}

/* An ordinary forward jump still evicts the oldest rather than waiting, all
 * the way out to the last seq inside the allowance; the first seq past it is
 * refused. */
static int t_best_effort_forward_jump_bound(void) {
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t olen;
    int rc = 1;
    if (!b) {
        goto done;
    }
    channelMuxInit(b);
    const uint32_t window = b->ch[CHANNEL_VOICE].window;
    const uint32_t maxAhead = window * BE_JUMP_WINDOWS; /* jump allowance */

    /* Seqs 0 and 1 buffered, nothing popped. */
    uint32_t i;
    for (i = 0; i < 2; i++) {
        putIdx(msg, i);
        int lf = makeSegFrame(frame, CHANNEL_VOICE, i, msg, 8);
        channelRecvFrame(b, frame, lf);
    }

    /* Seq 9 is past the window: the cursor moves to 2, the two older segments
     * are dropped rather than waited for, and only seq 9 delivers. */
    putIdx(msg, 9);
    {
        int lf = makeSegFrame(frame, CHANNEL_VOICE, 9, msg, 8);
        channelRecvFrame(b, frame, lf);
    }
    if (!channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen) ||
        getIdx(out) != 9) {
        goto done;
    }
    if (channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen)) {
        goto done;
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != 10) {
        goto done;
    }

    /* The last seq inside the allowance is still followed. */
    uint32_t edge = b->ch[CHANNEL_VOICE].expectedSeq + maxAhead - 1;
    putIdx(msg, 11);
    {
        int lf = makeSegFrame(frame, CHANNEL_VOICE, edge, msg, 8);
        channelRecvFrame(b, frame, lf);
    }
    if (!channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen) ||
        getIdx(out) != 11) {
        goto done;
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != edge + 1) {
        goto done;
    }

    /* One seq further out is refused, and the cursor stays put. */
    uint32_t cursor = b->ch[CHANNEL_VOICE].expectedSeq;
    putIdx(msg, 12);
    {
        int lf = makeSegFrame(frame, CHANNEL_VOICE, cursor + maxAhead, msg, 8);
        channelRecvFrame(b, frame, lf);
    }
    if (channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen)) {
        goto done;
    }
    if (b->ch[CHANNEL_VOICE].expectedSeq != cursor) {
        goto done;
    }
    rc = 0;
done:
    free(b);
    if (rc) {
        UT_FAIL("best-effort forward jump did not evict / bound as expected");
    }
    return 0;
}

/* A channel whose numbering legitimately reaches the top keeps delivering
 * across the wrap at 0xFFFFFFFF — the property that lets the receiver run
 * without a reset. channelResetExpected places the cursor up there directly;
 * arriving at it a segment at a time would take the whole sequence space. */
static int t_best_effort_wrap_delivers(void) {
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t msg[8];
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t olen;
    int rc = 1;
    if (!b) {
        goto done;
    }
    channelMuxInit(b);
    channelResetExpected(b, CHANNEL_VOICE, 0xFFFFFFFEu);

    static const uint32_t seqs[4] = {0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u};
    uint32_t i;
    for (i = 0; i < 4; i++) {
        putIdx(msg, i);
        int lf = makeSegFrame(frame, CHANNEL_VOICE, seqs[i], msg, 8);
        channelRecvFrame(b, frame, lf);
        if (!channelReceiveBestEffort(b, CHANNEL_VOICE, out, &olen) ||
            getIdx(out) != i) {
            goto done; /* delivery stopped at or after the wrap */
        }
        if (b->ch[CHANNEL_VOICE].expectedSeq != seqs[i] + 1) {
            goto done;
        }
    }
    rc = 0;
done:
    free(b);
    if (rc) {
        UT_FAIL("best-effort delivery broke across the sequence wrap");
    }
    return 0;
}

/* ---- fast loss recovery: a reported gap retransmits before the RTO ---- */

/* One segment is lost mid-stream so the receiver buffers a gap (seq 0 and 2,
 * missing 1) and reports it (ackedSeq stuck at 1, highestSeen at 3). The
 * sender rewinds its cursor on that report and resends the tail at a tick well
 * under the retransmit timeout (4 at LINK_RTT_MS) — the timeout could not have
 * fired, so recovery is the NAK. The storm guard is checked too: replaying the
 * same gap-ack does not rewind a second time. */
static int t_fast_retransmit(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t frame[MAXFRAME];
    uint8_t ackFrame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    uint8_t msg[4];
    int rc = 1;
    const uint8_t ch = CHANNEL_GAME;
    if (!a || !b) {
        goto done;
    }
    channelMuxInit(a);
    channelMuxInit(b);

    /* Each message is built into its own frame so exactly the middle one can
     * be dropped. Ticks stay below the RTO throughout, so any retransmit is
     * driven by the gap report, never the timeout. */

    /* tick 0: seq 0 built and delivered. */
    channelTick(a, 0, LINK_RTT_MS);
    channelTick(b, 0, LINK_RTT_MS);
    putIdx(msg, 0);
    if (!channelSend(a, ch, msg, 4)) {
        goto done;
    }
    int la = channelBuildFrame(a, frame, FRAME_BUDGET);
    channelRecvFrame(b, frame, la);

    /* tick 1: seq 1 built and dropped (never delivered). */
    channelTick(a, 1, LINK_RTT_MS);
    channelTick(b, 1, LINK_RTT_MS);
    putIdx(msg, 1);
    if (!channelSend(a, ch, msg, 4)) {
        goto done;
    }
    la = channelBuildFrame(a, frame, FRAME_BUDGET); /* dropped on the wire */

    /* tick 2: seq 2 built and delivered — b now holds the gap. */
    channelTick(a, 2, LINK_RTT_MS);
    channelTick(b, 2, LINK_RTT_MS);
    putIdx(msg, 2);
    if (!channelSend(a, ch, msg, 4)) {
        goto done;
    }
    la = channelBuildFrame(a, frame, FRAME_BUDGET);
    channelRecvFrame(b, frame, la);

    /* b delivered seq 0 in order but is stuck at the gap: expectedSeq == 1
     * with seq 2 buffered past it (recvHighestSeq == 3). */
    uint16_t olen;
    uint32_t delivered = 0;
    while (channelReceive(b, ch, out, &olen)) {
        if (getIdx(out) != delivered) {
            goto done;
        }
        delivered++;
    }
    if (delivered != 1 || b->ch[ch].expectedSeq != 1 ||
        b->ch[ch].recvHighestSeq != 3) {
        goto done;
    }

    /* tick 3 (still < RTO of 4): b's gap report reaches a. */
    channelTick(a, 3, LINK_RTT_MS);
    channelTick(b, 3, LINK_RTT_MS);
    int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
    memcpy(ackFrame, frame, (size_t)lb); /* keep it to replay for the guard */
    channelRecvFrame(a, ackFrame, lb);

    /* The gap rewound a's transmit cursor to the cumulative ack. */
    if (a->ch[ch].ackedSeq != 1 || a->ch[ch].txNext != 1 ||
        !a->ch[ch].nakIssued) {
        goto done;
    }

    /* a's retransmit (still tick 3) carries seq 1 (and 2); b fills the gap and
     * delivers everything — recovery completed before the RTO could fire (a
     * last transmitted at tick 2, so the timeout is not reachable until 6). */
    la = channelBuildFrame(a, frame, FRAME_BUDGET);
    channelRecvFrame(b, frame, la);
    while (channelReceive(b, ch, out, &olen)) {
        if (getIdx(out) != delivered) {
            goto done;
        }
        delivered++;
    }
    if (delivered != 3 || b->ch[ch].expectedSeq != 3) {
        goto done;
    }

    /* Storm guard: a is now caught up (txNext == nextSeq). Replaying the same
     * gap-ack must not rewind the cursor again — nakIssued is still set and the
     * cumulative ack did not advance, so no second tail resend is emitted. */
    channelRecvFrame(a, ackFrame, lb);
    if (a->ch[ch].txNext != a->ch[ch].nextSeq) {
        goto done; /* the cursor was rewound a second time */
    }
    la = channelBuildFrame(a, frame, FRAME_BUDGET);
    if (parseSegCount(frame, la) != 0) {
        goto done; /* nothing should be resent for the same stall */
    }

    if (checkSendInvariants(a, a->ch[ch].window)) {
        goto done;
    }
    rc = 0;
done:
    free(a);
    free(b);
    if (rc) {
        UT_FAIL("fast-retransmit did not recover the gap before the RTO");
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
    if (t_hold_and_resume()) {
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
    if (t_control_max_event_fits_wire()) {
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
    if (t_reset_drops_dead_tail()) {
        return 1;
    }
    if (t_reset_coordinated()) {
        return 1;
    }
    if (t_reset_buffer_before_lift()) {
        return 1;
    }
    if (t_reset_window_bound_recovery()) {
        return 1;
    }
    if (t_reset_no_rewind()) {
        return 1;
    }
    if (t_best_effort_no_hol()) {
        return 1;
    }
    if (t_best_effort_not_starved_by_bulk()) {
        return 1;
    }
    if (t_best_effort_overflow_drops_oldest()) {
        return 1;
    }
    if (t_best_effort_stale_and_dup()) {
        return 1;
    }
    if (t_best_effort_usage()) {
        return 1;
    }
    if (t_best_effort_absurd_seq_no_wedge()) {
        return 1;
    }
    if (t_best_effort_far_jump_refused()) {
        return 1;
    }
    if (t_best_effort_in_allowance_forgery_recovers()) {
        return 1;
    }
    if (t_best_effort_forward_jump_bound()) {
        return 1;
    }
    if (t_best_effort_wrap_delivers()) {
        return 1;
    }
    if (t_fast_retransmit()) {
        return 1;
    }
    if (t_soak()) {
        return 1;
    }
    return 0;
}

/* ---- the control channel's backlog behind the window ----
 *
 * A joiner's lobby replay can put more control events on the channel in one
 * tick than CHANNEL_CONTROL_WINDOW holds unacked. The control channel queues
 * what the window cannot take and sends it as acks free room; it refuses a
 * send only once that queue is full too. Every other reliable channel still
 * refuses at its window. channelResetSend on the control channel drops the
 * queue along with the unacked tail.
 *
 * These are separate cases rather than steps of run_channel_mux, so each one
 * reports on its own. */

#define BK_EXTRA   100   /* sends past the window                        */
#define BK_MAX_LEN  20   /* longest message the backlog cases send        */

/* Message i: its index in the first four bytes, then a length and fill that
 * both follow from i, so a reordered, dropped, duplicated or altered message
 * cannot pass for the one expected. */
static uint16_t bkMake(uint32_t i, uint8_t *msg) {
    uint16_t len = (uint16_t)(8 + (i % (BK_MAX_LEN - 8 + 1)));
    uint16_t k;
    putIdx(msg, i);
    for (k = 4; k < len; k++) {
        msg[k] = (uint8_t)(i * 31u + k);
    }
    return len;
}

static bool bkMatches(uint32_t i, const uint8_t *got, uint16_t gotLen) {
    uint8_t want[BK_MAX_LEN];
    uint16_t len = bkMake(i, want);
    return gotLen == len && memcmp(got, want, len) == 0;
}

/* What the receiver popped, in order. */
typedef struct {
    uint32_t count;
    uint32_t firstBad;     /* position of the first message out of place */
    bool     anyBad;
    uint32_t badIdx;       /* the index that message carried             */
} BkRecv;

/* Pump frames a -> b and acks b -> a on a clean link until `expect`
 * messages have arrived (then a few ticks more, so a duplicate would show),
 * or maxTicks pass. Each arrival is checked against bkMake(firstIdx + n). */
static void bkPump(ChannelMux *a, ChannelMux *b, uint8_t ch, uint32_t firstIdx,
                   uint32_t expect, int *tickIO, int maxTicks, BkRecv *r) {
    uint8_t frame[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t olen;
    int tick = *tickIO;
    int end = tick + maxTicks;
    int tail = -1;

    while (tick < end && (tail < 0 || tick < tail)) {
        channelTick(a, (uint32_t)tick, LINK_RTT_MS);
        channelTick(b, (uint32_t)tick, LINK_RTT_MS);
        int la = channelBuildFrame(a, frame, FRAME_BUDGET);
        channelRecvFrame(b, frame, la);
        while (channelReceive(b, ch, out, &olen)) {
            if (!r->anyBad && !bkMatches(firstIdx + r->count, out, olen)) {
                r->anyBad   = true;
                r->firstBad = r->count;
                r->badIdx   = olen >= 4 ? getIdx(out) : 0xFFFFFFFFu;
            }
            r->count++;
        }
        int lb = channelBuildFrame(b, frame, FRAME_BUDGET);
        channelRecvFrame(a, frame, lb);
        tick++;
        if (tail < 0 && r->count >= expect) {
            tail = tick + 20;
        }
    }
    *tickIO = tick;
}

/* Sends past the control window are taken, not refused. */
int run_channel_mux_control_queues_past_window(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    uint8_t msg[BK_MAX_LEN];
    uint32_t total;
    uint32_t i;

    UT_ASSERT(a != NULL);
    channelMuxInit(a);
    total = CHANNEL_CONTROL_WINDOW + BK_EXTRA;
    for (i = 0; i < total; i++) {
        uint16_t len = bkMake(i, msg);
        if (!channelSend(a, CHANNEL_CONTROL, msg, len)) {
            free(a);
            UT_FAIL("control send %u of %u was refused; the window is %d and "
                    "sends past it should queue", (unsigned)i,
                    (unsigned)total, CHANNEL_CONTROL_WINDOW);
        }
    }
    free(a);
    return 0;
}

/* What was queued past the window reaches the far side once acks come back:
 * every message, once, in the order it was sent, byte for byte. */
int run_channel_mux_control_backlog_delivers_in_order(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t msg[BK_MAX_LEN];
    uint32_t total = CHANNEL_CONTROL_WINDOW + BK_EXTRA;
    uint32_t accepted = 0;
    BkRecv r;
    int tick = 0;
    uint32_t i;

    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        UT_FAIL("allocation failed");
    }
    channelMuxInit(a);
    channelMuxInit(b);

    /* All sent before any frame moves, as the join replay does. A refused
       send is counted rather than failed on here, so the case reports what
       the far side got. */
    for (i = 0; i < total; i++) {
        uint16_t len = bkMake(i, msg);
        if (channelSend(a, CHANNEL_CONTROL, msg, len)) {
            accepted++;
        }
    }

    memset(&r, 0, sizeof(r));
    bkPump(a, b, CHANNEL_CONTROL, 0, total, &tick, 5000, &r);
    free(a);
    free(b);

    UT_ASSERT_MSG(r.count == total,
                  "the receiver got %u messages of %u sent (%u accepted by "
                  "channelSend)", (unsigned)r.count, (unsigned)total,
                  (unsigned)accepted);
    UT_ASSERT_MSG(!r.anyBad,
                  "message %u in arrival order was not message %u as sent "
                  "(it carried index %u)", (unsigned)r.firstBad,
                  (unsigned)r.firstBad, (unsigned)r.badIdx);
    return 0;
}

/* The queue behind the window is bounded: sends go on being taken past the
 * window, and a send is refused once the queue is full. How deep the queue
 * is belongs to the channel, so the case only asks that it is deeper than
 * nothing and not endless. */
int run_channel_mux_control_backlog_full_refuses(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    uint8_t msg[16];
    const uint32_t guard = 1u << 20;
    uint32_t accepted = 0;

    UT_ASSERT(a != NULL);
    channelMuxInit(a);
    memset(msg, 0x5A, sizeof(msg));
    while (accepted < guard) {
        putIdx(msg, accepted);
        if (!channelSend(a, CHANNEL_CONTROL, msg, sizeof(msg))) {
            break;
        }
        accepted++;
    }
    free(a);

    UT_ASSERT_MSG(accepted > CHANNEL_CONTROL_WINDOW,
                  "the first refused control send came after %u sends; the "
                  "window is %d and the queue behind it should take more",
                  (unsigned)accepted, CHANNEL_CONTROL_WINDOW);
    UT_ASSERT_MSG(accepted < guard,
                  "%u control sends were all taken with no acks; the queue "
                  "behind the window has no bound", (unsigned)accepted);
    return 0;
}

/* Only the control channel queues. The game channel still refuses the send
 * past its window and leaves its state as it was. */
int run_channel_mux_game_channel_still_refuses_at_window(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    uint8_t msg[8];
    uint32_t window;
    uint32_t i;

    UT_ASSERT(a != NULL);
    channelMuxInit(a);
    window = a->ch[CHANNEL_GAME].window;
    for (i = 0; i < window; i++) {
        memset(msg, 0, sizeof(msg));
        putIdx(msg, i);
        if (!channelSend(a, CHANNEL_GAME, msg, sizeof(msg))) {
            free(a);
            UT_FAIL("game send %u was refused inside the window of %u",
                    (unsigned)i, (unsigned)window);
        }
    }
    memset(msg, 0, sizeof(msg));
    putIdx(msg, window);
    if (channelSend(a, CHANNEL_GAME, msg, sizeof(msg))) {
        free(a);
        UT_FAIL("the game channel took a send past its window of %u",
                (unsigned)window);
    }
    if (a->ch[CHANNEL_GAME].nextSeq != window ||
        a->ch[CHANNEL_GAME].ackedSeq != 0) {
        uint32_t next = a->ch[CHANNEL_GAME].nextSeq;
        uint32_t acked = a->ch[CHANNEL_GAME].ackedSeq;
        free(a);
        UT_FAIL("the refused game send moved the channel: nextSeq %u "
                "ackedSeq %u, expected %u and 0", (unsigned)next,
                (unsigned)acked, (unsigned)window);
    }
    free(a);
    return 0;
}

/* A send reset on the control channel drops the queue behind the window as
 * well as the unacked tail. The receiver, lifted to the returned baseline as
 * the game-start reset does, gets the one message sent after the reset and
 * none of those queued before it. */
int run_channel_mux_control_reset_clears_backlog(void) {
    ChannelMux *a = (ChannelMux *)malloc(sizeof(*a));
    ChannelMux *b = (ChannelMux *)malloc(sizeof(*b));
    uint8_t msg[BK_MAX_LEN];
    uint8_t strag[MAXFRAME];
    uint8_t out[CHANNEL_MAX_SEG];
    uint16_t olen;
    uint32_t total = CHANNEL_CONTROL_WINDOW + BK_EXTRA;
    const uint32_t fresh = 0xABCDu;
    uint32_t baseline;
    BkRecv r;
    int tick = 0;
    int sl;
    uint32_t i;

    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        UT_FAIL("allocation failed");
    }
    channelMuxInit(a);
    channelMuxInit(b);

    for (i = 0; i < total; i++) {
        uint16_t len = bkMake(i, msg);
        if (!channelSend(a, CHANNEL_CONTROL, msg, len)) {
            free(a);
            free(b);
            UT_FAIL("control send %u of %u was refused before the reset; the "
                    "window is %d and sends past it should queue",
                    (unsigned)i, (unsigned)total, CHANNEL_CONTROL_WINDOW);
        }
    }

    /* One frame of the old tail goes out and is held back: a straggler that
       reaches the receiver only after the reset. */
    channelTick(a, (uint32_t)tick, LINK_RTT_MS);
    sl = channelBuildFrame(a, strag, FRAME_BUDGET);
    tick++;

    baseline = channelResetSend(a, CHANNEL_CONTROL);
    channelResetExpected(b, CHANNEL_CONTROL, baseline);

    channelRecvFrame(b, strag, sl);
    if (channelReceive(b, CHANNEL_CONTROL, out, &olen)) {
        free(a);
        free(b);
        UT_FAIL("a message sent before the reset was delivered after it");
    }

    uint16_t freshLen = bkMake(fresh, msg);
    if (!channelSend(a, CHANNEL_CONTROL, msg, freshLen)) {
        free(a);
        free(b);
        UT_FAIL("the first control send after the reset was refused");
    }

    memset(&r, 0, sizeof(r));
    bkPump(a, b, CHANNEL_CONTROL, fresh, 1, &tick, 2000, &r);
    free(a);
    free(b);

    UT_ASSERT_MSG(r.count == 1,
                  "the receiver got %u messages after the reset, expected only "
                  "the one sent after it", (unsigned)r.count);
    UT_ASSERT_MSG(!r.anyBad,
                  "the message delivered after the reset carried index %u, "
                  "not the one sent after it (%u)", (unsigned)r.badIdx,
                  (unsigned)fresh);
    return 0;
}
