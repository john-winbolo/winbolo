/*
 * Map-download re-ask throttle.
 *
 * PACKET_MAP_DL_READY has two meanings. The first one of a download says "my
 * receive buffers exist, you may start"; that is cheap and is never held off.
 * A later one, once the stream has already begun, means "I cannot finish this
 * transfer, start it over" — and starting over is the most expensive thing a
 * single small datagram can ask this server to do. It recompresses that
 * slot's whole copy of the terrain, re-sends JOIN_ACCEPT, re-bases the bulk
 * channel and streams the map again.
 *
 * connId stops an off-path attacker forging one, but it does nothing about a
 * client that has legitimately joined and then asks in a loop: the work is
 * per-request and there was no interval on it. MAP_REASK_MIN_TICKS is that
 * interval, and this test is what holds it in place.
 *
 * It stands up the loopback harness, joins a second client from a raw UDP
 * socket the test owns (the same crafted-JOIN scaffolding test_map_amp_gate.c
 * uses, so the cookie handshake is completed for real and the slot is a real
 * one), waits for the map stream to open, then re-asks far faster than any
 * honest client would and checks the server stopped paying for most of them.
 *
 * The re-ask has no reply of its own, so a throttled one is invisible on the
 * wire — hence the counter seam rather than an assertion about datagrams.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_* */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, the counter seam */
#include "transport_udp_internal.h" /* packHeader, packConnId, getPacketType */
#include "test_harness.h"
#include "loopback_harness.h"

/* Pumps allowed for each phase to settle. The reply rides the server tick
 * after the crafted packet is drained, so a couple of dozen is ample; the
 * ceiling only stops a broken run hanging. */
#define RT_PUMP_MAX 200

/* Re-asks the test fires, and pumps between them. The product has to stay
 * inside MAP_REASK_MIN_TICKS (25 server ticks) or the later re-asks fall
 * outside the interval and are served rather than throttled — which is the
 * behaviour we want in the field and would silently weaken the assertion
 * here. 10 x 2 = 20 leaves margin under the 25. */
#define RT_REASKS      10
#define RT_PUMPS_EACH   2

/* Build a JOIN_REQUEST into buf with the server's own version triple and a
 * trailing address-proof cookie (the given bytes, or zeros if NULL). Layout
 * mirrors transport_udp_client.c's builder — kept in step with
 * test_map_amp_gate.c's agBuildJoin, which is the same wire shape. */
static int rtBuildJoin(uint8_t *buf, const char *name,
                       const uint8_t *cookieOrNull) {
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

    if (cookieOrNull != NULL) {
        memcpy(buf + pos, cookieOrNull, JOIN_COOKIE_LEN);
    } else {
        memset(buf + pos, 0, JOIN_COOKIE_LEN);
    }
    pos += JOIN_COOKIE_LEN;
    return pos;
}

/* connId on the wire is two big-endian u32s, low half first — see packConnId
 * in transport_udp_server.c / transport_udp_client.c. Both are static to their
 * translation unit, so the shape is spelled out here rather than shared; it is
 * part of the JOIN_ACCEPT / MAP_DL_READY layout this test is written against
 * anyway. */
static uint64_t rtUnpackConnId(const uint8_t *buf) {
    uint32_t lo = unpackU32(buf);
    uint32_t hi = unpackU32(buf + 4);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void rtPackConnId(uint8_t *buf, uint64_t connId) {
    packU32(buf,     (uint32_t)(connId & 0xffffffffULL));
    packU32(buf + 4, (uint32_t)(connId >> 32));
}

/* Open a non-blocking UDP socket bound to an ephemeral 127.0.0.1 port. */
static SOCKET rtOpenSocket(void) {
    struct sockaddr_in addr;
    unsigned long nonblock = 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &nonblock);
    return s;
}

/* Send pkt once, then pump, draining replies. Records a JOIN_ACCEPT's slot and
 * connId (JOIN_ACCEPT is [header][slot 1][tick 4][mapSize 4][connId 8]) and
 * whether any PACKET_CHANNEL arrived — the bulk carrier, which is how the test
 * knows the map stream has actually opened. */
static void rtCollect(LoopbackHarness *h, SOCKET sock,
                      const uint8_t *pkt, int pktLen,
                      const struct sockaddr_in *serverAddr,
                      bool *outAccept, int *outSlot, uint64_t *outConnId,
                      bool *outChannel, uint8_t *outCookie,
                      bool *outChallenge) {
    int i;
    if (outAccept)    *outAccept = false;
    if (outChannel)   *outChannel = false;
    if (outChallenge) *outChallenge = false;
    for (i = 0; i < RT_PUMP_MAX; i++) {
        uint8_t in[2048];
        int n;
        if (i == 0 && pkt != NULL) {
            sendto(sock, (const char *)pkt, pktLen, 0,
                   (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
        }
        loopbackHarnessPump(h);
        while ((n = (int)recvfrom(sock, (char *)in, sizeof(in), 0,
                                  NULL, NULL)) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT &&
                n >= PACKET_HEADER_SIZE + 1 + 4 + 4 + 8) {
                if (outAccept) *outAccept = true;
                if (outSlot)   *outSlot = in[PACKET_HEADER_SIZE];
                if (outConnId) {
                    *outConnId = rtUnpackConnId(in + PACKET_HEADER_SIZE + 9);
                }
            } else if (type == PACKET_CHANNEL) {
                if (outChannel) *outChannel = true;
            } else if (type == PACKET_JOIN_CHALLENGE &&
                       n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                if (outChallenge) *outChallenge = true;
                if (outCookie != NULL) {
                    memcpy(outCookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                }
            }
        }
    }
}

/* Build a MAP_DL_READY for this connection — [header][connId 8], the same
 * shape udpClientSendMapDlReady puts on the wire. */
static int rtBuildReady(uint8_t *buf, uint64_t connId, uint32_t seq) {
    packHeader(buf, PACKET_MAP_DL_READY, seq);
    rtPackConnId(buf + PACKET_HEADER_SIZE, connId);
    return PACKET_HEADER_SIZE + 8;
}

int run_map_reask_throttle(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    uint8_t readyBuf[PACKET_HEADER_SIZE + 8];
    uint8_t cookie[JOIN_COOKIE_LEN];
    int joinLen, readyLen, slot = -1, i;
    uint64_t connId = 0;
    SOCKET sock;
    bool gotAccept, gotChannel, gotChallenge;
    uint32_t throttled;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ReaskHost", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    sock = rtOpenSocket();
    if (sock == INVALID_SOCKET) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not open raw UDP socket for crafted JOIN");
    }

    /* ---- Join for real: challenge, then the cookie echoed back. ---- */
    joinLen = rtBuildJoin(joinBuf, "Reask", NULL);
    rtCollect(&h, sock, joinBuf, joinLen, &serverAddr,
              NULL, NULL, NULL, NULL, cookie, &gotChallenge);
    if (!gotChallenge) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("crafted JOIN drew no PACKET_JOIN_CHALLENGE within %d pumps",
                RT_PUMP_MAX);
    }

    joinLen = rtBuildJoin(joinBuf, "Reask", cookie);
    rtCollect(&h, sock, joinBuf, joinLen, &serverAddr,
              &gotAccept, &slot, &connId, &gotChannel, NULL, NULL);
    if (!gotAccept || slot < 0) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("cookie-echoing JOIN drew no PACKET_JOIN_ACCEPT within %d pumps",
                RT_PUMP_MAX);
    }
    fprintf(stderr, "  reask throttle: joined slot=%d connId=%llu channel=%d\n",
            slot, (unsigned long long)connId, (int)gotChannel);

    /* Nothing has been refused yet — a fresh connection starts clean, and a
     * non-zero count here would mean the join path itself tripped the
     * interval. */
    throttled = transportUdpServerGetMapReaskThrottled(slot);
    if (throttled != 0) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d had %u throttled re-asks straight after joining",
                slot, (unsigned)throttled);
    }

    /* First READY: the honest one. This is the "buffers are armed" ask, not a
     * re-ask, so it must be served and must not count against the interval. */
    readyLen = rtBuildReady(readyBuf, connId, 1u);
    rtCollect(&h, sock, readyBuf, readyLen, &serverAddr,
              NULL, NULL, NULL, &gotChannel, NULL, NULL);
    throttled = transportUdpServerGetMapReaskThrottled(slot);
    if (throttled != 0) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("the first MAP_DL_READY of a download was throttled (%u) — it "
                "is the arming ask, not a re-ask, and gates the join",
                (unsigned)throttled);
    }

    /* Now hammer it. Each of these lands while the stream is running, so each
     * one is a restart request; only the ones outside the interval should be
     * paid for. */
    for (i = 0; i < RT_REASKS; i++) {
        int p;
        readyLen = rtBuildReady(readyBuf, connId, (uint32_t)(2 + i));
        sendto(sock, (const char *)readyBuf, readyLen, 0,
               (const struct sockaddr *)&serverAddr, sizeof(serverAddr));
        for (p = 0; p < RT_PUMPS_EACH; p++) {
            uint8_t in[2048];
            loopbackHarnessPump(&h);
            while ((int)recvfrom(sock, (char *)in, sizeof(in), 0,
                                 NULL, NULL) > 0) {
                /* Drained and dropped: this test asserts on the counter, and
                 * an unread socket buffer would start discarding for us. */
            }
        }
    }

    throttled = transportUdpServerGetMapReaskThrottled(slot);
    fprintf(stderr, "  reask throttle: %d re-asks in %d pumps -> %u throttled\n",
            RT_REASKS, RT_REASKS * RT_PUMPS_EACH, (unsigned)throttled);

    if (throttled == 0) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("%d map-download re-asks inside %d server ticks were all "
                "served — each one recompresses the map and restarts the "
                "stream, so the interval is not holding",
                RT_REASKS, RT_REASKS * RT_PUMPS_EACH);
    }
    if (throttled >= (uint32_t)RT_REASKS) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("all %d re-asks were throttled — at least the first should "
                "have been served, or a client can never restart a genuinely "
                "broken transfer", RT_REASKS);
    }

    closesocket(sock);
    loopbackHarnessStop(&h);
    return 0;
}
