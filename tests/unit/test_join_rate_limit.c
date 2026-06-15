/*
 * Per-source-IP JOIN rate-limit test.
 *
 * A blind spoofer can walk source ports to send one JOIN per slot and
 * exhaust all MAX_TANKS slots. The server defends with a token-bucket rate
 * limit keyed on the source IP alone (port ignored, since a spoof walks
 * ports): each source IP gets a small burst of joins, and over-rate joins
 * are dropped silently — before any slot allocation — with no reply (a
 * JOIN_REJECT would reflect to a possibly-spoofed source).
 *
 * The cookie gate is what prevents slot exhaustion, so the rate limiter only
 * guards the unproven path: a cookie-less JOIN that passes the limiter draws a
 * PACKET_JOIN_CHALLENGE, while one dropped by the limiter draws nothing (a
 * valid-cookie JOIN is never rate-limited). Counting challenges therefore tests
 * the limiter directly, without depending on slot allocation.
 *
 * Like test_join_version_gate.c this stands up the loopback harness (whose
 * built-in UDP client is a real loopback joiner) and then drives hand-built
 * JOIN_REQUESTs from raw UDP sockets the test owns. Two assertions:
 *
 *   (A) Cooldown fires (portable, all 127.0.0.1): a burst of cookie-less JOINs,
 *       each from a distinct ephemeral 127.0.0.1 port (so each looks like a
 *       new source to the server's per-port client lookup, rather than an
 *       already-connected resend), shares the single 127.0.0.1 token bucket.
 *       The count of distinct PACKET_JOIN_CHALLENGE replies must be ≤ the burst
 *       capacity — far under the 16 sources otherwise admitted — and ≥ 1. The
 *       harness's own real client spends ~2 tokens completing its handshake, so
 *       the ≤ bound is inclusive of that.
 *
 *   (B) Distinct sources independent (Linux only): a JOIN from 127.0.0.2
 *       draws a PACKET_JOIN_CHALLENGE from its own fresh bucket even though
 *       127.0.0.1 is in cooldown. If binding 127.0.0.2 isn't possible
 *       (non-Linux loopback), the case logs a skip and does not fail.
 *
 * The crafted body mirrors the client's JOIN_REQUEST layout (correct version
 * triple + a valid ASCII name) and omits the trailing cookie, so it clears the
 * length, version, and name gates, passes (or is dropped by) the rate limiter,
 * and — when it passes — is challenged at the cookie gate. Refill over the
 * test's millisecond span is ~0, so the bucket drains deterministically.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, select, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_* */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, JOIN_FLAG_* */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "test_harness.h"
#include "loopback_harness.h"

/* Mirrors JOIN_RL_BURST in transport_udp_server.c — the token-bucket capacity
 * per source IP. Kept in sync by hand: the limiter constant is file-static and
 * not exported, and a wire change here would otherwise go unnoticed. */
#define JRL_BURST          5

/* Distinct-port sources fired at the single 127.0.0.1 bucket. Comfortably
 * above the burst capacity and above what the bucket can admit, and well over
 * MAX_TANKS so a missing limiter would visibly over-accept. */
#define JRL_NUM_SOCKETS    20

/* Generous bound: a crafted JOIN's reply lands on the next server tick after
 * it's drained, so a few hundred pumps is ample for the whole burst. */
#define JRL_PUMP_MAX       400

/* Build a JOIN_REQUEST into buf with the server's own version triple and the
 * given name. Returns the total length. Layout mirrors
 * transport_udp_client.c's builder: header + name + pass + 3 version bytes +
 * WBN token + flags + clientType + clientHints + 2-byte fallbackCountry. */
static int jrlBuildJoin(uint8_t *buf, const char *name) {
    int pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_JOIN_REQUEST, 0);

    memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
    strncpy((char *)(buf + pos), name, PACKET_MAX_PLAYER_NAME - 1);
    pos += PACKET_MAX_PLAYER_NAME;

    memset(buf + pos, 0, MAP_STR_SIZE);   /* empty password */
    pos += MAP_STR_SIZE;

    buf[pos++] = BOLO_VERSION_MAJOR;
    buf[pos++] = BOLO_VERSION_MINOR;
    buf[pos++] = BOLO_VERSION_REVISION;

    memset(buf + pos, 0, WBN_JOIN_KEY_WIRE_LEN);  /* anonymous join */
    pos += WBN_JOIN_KEY_WIRE_LEN;

    buf[pos++] = 0;   /* flags: no rejoin, no auth */
    buf[pos++] = 0;   /* clientType (unknown) */
    buf[pos++] = 0;   /* clientHints */
    buf[pos++] = 0;   /* fallbackCountry[0] */
    buf[pos++] = 0;   /* fallbackCountry[1] */
    return pos;
}

/* Open a non-blocking UDP socket bound to an ephemeral port on the given
 * loopback IP. Returns INVALID_SOCKET if the bind isn't possible (e.g. a
 * non-127.0.0.1 loopback alias on platforms without the full 127/8 range). */
static SOCKET jrlOpenSocketOnIp(const char *ip) {
    struct sockaddr_in addr;
    unsigned long nonblock = 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port        = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    /* Non-blocking so the pump loop can poll without stalling when no reply
     * is waiting (FIONBIO maps to ioctl on POSIX via platform_net.h). */
    ioctlsocket(s, FIONBIO, &nonblock);
    return s;
}

/* Non-blocking poll: returns the byte count of a datagram now waiting on s,
 * or <= 0 if none is currently available. */
static int jrlTryRecv(SOCKET s, uint8_t *buf, int cap) {
    return (int)recvfrom(s, (char *)buf, cap, 0, NULL, NULL);
}

int run_join_rate_limit(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET socks[JRL_NUM_SOCKETS];
    uint8_t joinBuf[JRL_NUM_SOCKETS][1024];
    int joinLen[JRL_NUM_SOCKETS];
    bool challenged[JRL_NUM_SOCKETS];
    int challengeCount = 0;
    int i, pump;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Joiner", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* ---- (A) Cooldown fires: burst from distinct 127.0.0.1 ports ---- */
    for (i = 0; i < JRL_NUM_SOCKETS; i++) {
        char name[PACKET_MAX_PLAYER_NAME];
        socks[i]      = jrlOpenSocketOnIp("127.0.0.1");
        challenged[i] = false;
        if (socks[i] == INVALID_SOCKET) {
            int j;
            for (j = 0; j < i; j++) closesocket(socks[j]);
            loopbackHarnessStop(&h);
            UT_FAIL("could not open raw UDP socket %d for burst JOIN", i);
        }
        /* Distinct, valid names so each JOIN clears name validation; name
         * collisions are moot here since a cookie-less JOIN is challenged
         * before slot allocation is ever reached. */
        snprintf(name, sizeof(name), "RL%d", i);
        joinLen[i] = jrlBuildJoin(joinBuf[i], name);
    }

    for (pump = 0; pump < JRL_PUMP_MAX; pump++) {
        uint8_t in[1024];
        /* Resend each not-yet-challenged socket's JOIN periodically to ride out
         * best-effort loopback delivery and the recv-thread drain. A JOIN the
         * limiter drops is silent (no challenge), so an un-challenged socket's
         * resend can only ever be dropped or — if a token frees up — draw its
         * first challenge; it can't inflate the distinct-challenge count. */
        if (pump % 8 == 0) {
            for (i = 0; i < JRL_NUM_SOCKETS; i++) {
                if (!challenged[i]) {
                    sendto(socks[i], (const char *)joinBuf[i], joinLen[i], 0,
                           (const struct sockaddr *)&serverAddr,
                           sizeof(serverAddr));
                }
            }
        }
        loopbackHarnessPump(&h);
        for (i = 0; i < JRL_NUM_SOCKETS; i++) {
            int n;
            while ((n = jrlTryRecv(socks[i], in, sizeof(in))) > 0) {
                if (getPacketType(in, n) == PACKET_JOIN_CHALLENGE &&
                    !challenged[i]) {
                    challenged[i] = true;
                    challengeCount++;
                }
            }
        }
    }

    for (i = 0; i < JRL_NUM_SOCKETS; i++) closesocket(socks[i]);

    fprintf(stderr, "  join rate limit: %d/%d distinct-port JOINs challenged "
            "(burst=%d)\n", challengeCount, JRL_NUM_SOCKETS, JRL_BURST);

    if (challengeCount < 1) {
        loopbackHarnessStop(&h);
        UT_FAIL("no JOIN drew a challenge from the 127.0.0.1 burst within "
                "%d pumps", JRL_PUMP_MAX);
    }
    if (challengeCount > JRL_BURST) {
        loopbackHarnessStop(&h);
        UT_FAIL("rate limit let %d JOINs past the limiter from one source IP, "
                "expected <= %d", challengeCount, JRL_BURST);
    }

    /* ---- (B) Distinct sources independent: 127.0.0.2 (Linux only) ---- */
    {
        SOCKET altSock = jrlOpenSocketOnIp("127.0.0.2");
        if (altSock == INVALID_SOCKET) {
            SDL_Log("  join rate limit: skipping 127.0.0.2 case "
                    "(distinct loopback source not bindable on this platform)");
        } else {
            uint8_t altJoin[1024];
            int altLen = jrlBuildJoin(altJoin, "RLalt");
            bool altChallenge = false;

            for (pump = 0; pump < JRL_PUMP_MAX && !altChallenge; pump++) {
                uint8_t in[1024];
                int n;
                if (pump % 8 == 0) {
                    sendto(altSock, (const char *)altJoin, altLen, 0,
                           (const struct sockaddr *)&serverAddr,
                           sizeof(serverAddr));
                }
                loopbackHarnessPump(&h);
                while ((n = jrlTryRecv(altSock, in, sizeof(in))) > 0) {
                    if (getPacketType(in, n) == PACKET_JOIN_CHALLENGE) {
                        altChallenge = true;
                    }
                }
            }
            closesocket(altSock);

            fprintf(stderr, "  join rate limit: 127.0.0.2 challenged=%d "
                    "(127.0.0.1 in cooldown)\n", (int)altChallenge);
            if (!altChallenge) {
                loopbackHarnessStop(&h);
                UT_FAIL("JOIN from distinct source 127.0.0.2 drew no "
                        "PACKET_JOIN_CHALLENGE within %d pumps", JRL_PUMP_MAX);
            }
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}
