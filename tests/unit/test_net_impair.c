/*
 * Pure determinism / behaviour tests for the network impairment layer in
 * src/bolo/net_impair.c.  No sockets: sockaddr_in values are filled
 * constants and time is a synthetic nowMs the test advances by hand.
 *
 * The headline property is that, under a fixed bolo_srand() seed, the
 * layer produces the same drop/delay/reorder sequence every run — the
 * same guarantee test_bolo_rand.c pins for the underlying PRNG.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "net_impair.h"
#include "bolo_rand.h"
#include "test_harness.h"

/* A constant peer address — never interpreted by the layer, just carried. */
static struct sockaddr_in mkAddr(uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = port;
    return a;
}

/* Encode / decode a packet index in the first four payload bytes so the
 * test can follow individual datagrams through offer/pop. */
static void putIdx(uint8_t *buf, int idx) {
    buf[0] = (uint8_t)(idx & 0xFF);
    buf[1] = (uint8_t)((idx >> 8) & 0xFF);
    buf[2] = (uint8_t)((idx >> 16) & 0xFF);
    buf[3] = (uint8_t)((idx >> 24) & 0xFF);
}

static int getIdx(const uint8_t *buf) {
    return (int)((uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                 ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24));
}

/* ---- Parse ------------------------------------------------------------ */

static int parse_test(void) {
    NetImpairConfig cfg;

    /* Full spec round-trips into the right fields. */
    UT_ASSERT(netImpairParseConfig("delay=75,jitter=30,loss=2,burst=2", &cfg));
    UT_ASSERT(cfg.baseDelayMs == 75);
    UT_ASSERT(cfg.jitterMs == 30);
    UT_ASSERT(cfg.lossPercent == 2);
    UT_ASSERT(cfg.burstLossLen == 2);

    /* Omitted keys take their defaults (0/0/0/1). */
    UT_ASSERT(netImpairParseConfig("loss=10", &cfg));
    UT_ASSERT(cfg.baseDelayMs == 0);
    UT_ASSERT(cfg.jitterMs == 0);
    UT_ASSERT(cfg.lossPercent == 10);
    UT_ASSERT(cfg.burstLossLen == 1);

    /* loss clamps to 100; burst clamps up to a minimum of 1. */
    UT_ASSERT(netImpairParseConfig("loss=250,burst=0", &cfg));
    UT_ASSERT(cfg.lossPercent == 100);
    UT_ASSERT(cfg.burstLossLen == 1);

    /* Malformed specs return false. */
    UT_ASSERT(!netImpairParseConfig("delay=abc", &cfg));
    UT_ASSERT(!netImpairParseConfig("bogus=5", &cfg));
    UT_ASSERT(!netImpairParseConfig("delay", &cfg));
    UT_ASSERT(!netImpairParseConfig("delay=", &cfg));
    UT_ASSERT(!netImpairParseConfig("delay=10,", &cfg));
    UT_ASSERT(!netImpairParseConfig("delay=10,,loss=2", &cfg));
    UT_ASSERT(!netImpairParseConfig("delay=-5", &cfg));
    UT_ASSERT(!netImpairParseConfig("", &cfg));
    UT_ASSERT(!netImpairParseConfig(NULL, &cfg));

    return 0;
}

/* ---- Disabled --------------------------------------------------------- */

static int disabled_test(void) {
    NetImpair ni;
    struct sockaddr_in addr = mkAddr(3);
    struct sockaddr_in paddr;
    uint8_t buf[8];
    uint8_t pbuf[NET_IMPAIR_MAX_PACKET];

    netImpairInit(&ni);
    UT_ASSERT(!netImpairEnabled(&ni));
    memset(buf, 0, sizeof(buf));
    /* Offer returns false and queues nothing when disabled. */
    UT_ASSERT(!netImpairOffer(&ni, buf, (int)sizeof(buf), &addr, 0));
    UT_ASSERT(ni.count == 0);
    UT_ASSERT(netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, 1000) == -1);
    return 0;
}

/* ---- Due-time --------------------------------------------------------- */

static int due_time_test(void) {
    NetImpair ni;
    NetImpairConfig cfg;
    struct sockaddr_in addr = mkAddr(4);
    struct sockaddr_in paddr;
    uint8_t buf[8];
    uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
    int plen;

    cfg.baseDelayMs = 50;
    cfg.jitterMs = 0;
    cfg.lossPercent = 0;
    cfg.burstLossLen = 1;

    bolo_srand(7ULL);
    netImpairInit(&ni);
    netImpairEnable(&ni, &cfg);

    memset(buf, 0, sizeof(buf));
    putIdx(buf, 99);
    UT_ASSERT(netImpairOffer(&ni, buf, (int)sizeof(buf), &addr, 100));

    /* deliverAt = 100 + 50 = 150. Must not pop before then. */
    UT_ASSERT(netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, 100) == -1);
    UT_ASSERT(netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, 149) == -1);
    plen = netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, 150);
    UT_ASSERT(plen == (int)sizeof(buf));
    UT_ASSERT(getIdx(pbuf) == 99);
    /* Drained. */
    UT_ASSERT(netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, 150) == -1);
    return 0;
}

/* ---- Determinism ------------------------------------------------------ */

#define DET_N 200

/* Run a fixed synthetic timeline and capture the delivered-packet order
 * (dropped packets never appear). Returns the number delivered. */
static int run_determinism_sequence(uint64_t seed, int *out, int outCap) {
    NetImpair ni;
    NetImpairConfig cfg;
    struct sockaddr_in addr = mkAddr(1234);
    struct sockaddr_in paddr;
    uint8_t buf[16];
    uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
    int produced = 0;
    int step;
    int plen;

    cfg.baseDelayMs = 50;
    cfg.jitterMs = 40;
    cfg.lossPercent = 20;
    cfg.burstLossLen = 2;

    bolo_srand(seed);
    netImpairInit(&ni);
    netImpairEnable(&ni, &cfg);

    for (step = 0; step < DET_N; step++) {
        uint64_t now = (uint64_t)step * 5;
        memset(buf, 0, sizeof(buf));
        putIdx(buf, step);
        netImpairOffer(&ni, buf, (int)sizeof(buf), &addr, now);
        while ((plen = netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, now)) > 0) {
            if (produced < outCap) out[produced] = getIdx(pbuf);
            produced++;
        }
    }
    /* Flush whatever remains far in the future. */
    {
        uint64_t now = (uint64_t)DET_N * 5 + 1000000;
        while ((plen = netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, now)) > 0) {
            if (produced < outCap) out[produced] = getIdx(pbuf);
            produced++;
        }
    }
    return produced;
}

static int determinism_test(void) {
    int a[DET_N];
    int b[DET_N];
    int d[DET_N];
    int na, nb, nd;
    int i;
    bool differs;

    na = run_determinism_sequence(0x00C0FFEEULL, a, DET_N);
    nb = run_determinism_sequence(0x00C0FFEEULL, b, DET_N);
    nd = run_determinism_sequence(0x00012345ULL, d, DET_N);

    /* Same seed → identical drop/delay/reorder sequence. */
    UT_ASSERT_MSG(na == nb, "same seed delivered %d vs %d", na, nb);
    for (i = 0; i < na; i++) {
        UT_ASSERT_MSG(a[i] == b[i], "same seed diverged at %d: %d vs %d",
                      i, a[i], b[i]);
    }

    /* Different seed → some observable difference (count or order). */
    differs = (nd != na);
    for (i = 0; i < na && i < nd && !differs; i++) {
        if (a[i] != d[i]) differs = true;
    }
    UT_ASSERT_MSG(differs, "different seed produced an identical sequence");
    return 0;
}

/* ---- Reordering ------------------------------------------------------- */

static int reorder_test(void) {
    NetImpair ni;
    NetImpairConfig cfg;
    struct sockaddr_in addr = mkAddr(1);
    struct sockaddr_in paddr;
    uint8_t buf[8];
    uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
    int order[64];
    int n = 0;
    int i;
    int plen;
    bool reordered = false;

    /* delay=0 so deliverAt is driven entirely by per-packet jitter; all
     * 32 packets are offered at the same instant (spacing 0 < jitter span). */
    cfg.baseDelayMs = 0;
    cfg.jitterMs = 100;
    cfg.lossPercent = 0;
    cfg.burstLossLen = 1;

    bolo_srand(0x00ABCDEFULL);
    netImpairInit(&ni);
    netImpairEnable(&ni, &cfg);

    for (i = 0; i < 32; i++) {
        memset(buf, 0, sizeof(buf));
        putIdx(buf, i);
        netImpairOffer(&ni, buf, (int)sizeof(buf), &addr, 0);
    }
    while ((plen = netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, 1000)) > 0) {
        order[n++] = getIdx(pbuf);
    }
    UT_ASSERT_MSG(n == 32, "expected 32 popped, got %d", n);

    for (i = 1; i < n; i++) {
        if (order[i] < order[i - 1]) {
            reordered = true;
            break;
        }
    }
    UT_ASSERT_MSG(reordered, "jitter produced no reordering");
    return 0;
}

/* ---- Burst loss ------------------------------------------------------- */

static int burst_test(void) {
    NetImpair ni;
    NetImpairConfig cfg;
    struct sockaddr_in addr = mkAddr(2);
    struct sockaddr_in paddr;
    uint8_t buf[8];
    uint8_t pbuf[NET_IMPAIR_MAX_PACKET];
    const int BURST = 3;
    int i;
    int plen;
    int runLen = 0;
    int runsFound = 0;

    /* delay=0,jitter=0 so a queued packet always pops immediately; only one
     * datagram is ever in flight, so pop==-1 unambiguously means "dropped". */
    cfg.baseDelayMs = 0;
    cfg.jitterMs = 0;
    cfg.lossPercent = 50;
    cfg.burstLossLen = (uint32_t)BURST;

    bolo_srand(0x0005EED1ULL);
    netImpairInit(&ni);
    netImpairEnable(&ni, &cfg);

    /* Run past 500 offers but always stop on a burst boundary
     * (burstRemaining == 0) so a trailing run is never truncated mid-burst. */
    for (i = 0; i < 500 || ni.burstRemaining != 0; i++) {
        bool dropped;
        if (i > 1000000) break;  /* safety net; never reached in practice */
        memset(buf, 0, sizeof(buf));
        putIdx(buf, i);
        netImpairOffer(&ni, buf, (int)sizeof(buf), &addr, 0);
        plen = netImpairPop(&ni, pbuf, sizeof(pbuf), &paddr, 1000000);
        dropped = (plen < 0);
        if (dropped) {
            runLen++;
        } else if (runLen > 0) {
            UT_ASSERT_MSG(runLen % BURST == 0,
                "drop run length %d is not a multiple of burst %d",
                runLen, BURST);
            runsFound++;
            runLen = 0;
        }
    }
    if (runLen > 0) {
        UT_ASSERT_MSG(runLen % BURST == 0,
            "trailing drop run length %d is not a multiple of burst %d",
            runLen, BURST);
        runsFound++;
    }
    UT_ASSERT_MSG(runsFound > 0, "no drop runs observed");
    return 0;
}

int run_net_impair(void) {
    int rc;
    if ((rc = parse_test()) != 0) return rc;
    if ((rc = disabled_test()) != 0) return rc;
    if ((rc = due_time_test()) != 0) return rc;
    if ((rc = determinism_test()) != 0) return rc;
    if ((rc = reorder_test()) != 0) return rc;
    if ((rc = burst_test()) != 0) return rc;
    return 0;
}
