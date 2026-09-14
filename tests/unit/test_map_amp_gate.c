/*
 * Map-send amplification-gate test.
 *
 * The server must not blast the compressed map (tens of KB streamed on
 * CHANNEL_BULK) to a freshly-joined address before that address has proven it
 * can receive a reply. Otherwise a ~170-byte spoofed JOIN_REQUEST reflects a
 * large payload at a forged source. The gate is the address-proof cookie: a
 * slot is allocated — and the map stream begins — only after the joiner echoes
 * a cookie the server can recompute for its source address, which a spoofer at
 * a forged source never receives. The old MAP_ACK 0xFFFF "ready" round-trip that
 * used to gate the chunk send is retired; the map now streams on CHANNEL_BULK
 * right after JOIN_ACCEPT, so the cookie is the sole amplification gate.
 *
 * This stands up the loopback harness (a real server on an ephemeral port) and
 * drives a hand-built JOIN_REQUEST from a raw UDP socket the test owns, then
 * inspects the actual datagrams the server sends back:
 *   1. A cookie-less JOIN draws a small PACKET_JOIN_CHALLENGE and NOTHING else —
 *      no JOIN_ACCEPT and no PACKET_CHANNEL (the bulk carrier). With no slot the
 *      map can never stream, so a spoofed source draws no amplified reply.
 *   2. Echoing the cookie draws JOIN_ACCEPT and then PACKET_CHANNEL frames — the
 *      server begins streaming the map on CHANNEL_BULK once the address is
 *      proven. (The raw socket never acks the bulk channel, so the server keeps
 *      re-sending the window; one frame is enough to prove the carrier opened.)
 *
 * The crafted JOIN mirrors the client's JOIN_REQUEST layout (see
 * test_join_version_gate.c) so it clears the length/name/version gates and
 * completes the cookie handshake to a full join with map-download init.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_* */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN */
#include "transport_udp_internal.h" /* packHeader, packU16, getPacketType */
#include "test_harness.h"
#include "loopback_harness.h"

/* Pumps per phase. The reply rides the next server tick after the crafted
 * packet is drained; a couple dozen pumps is ample, and we keep pumping past
 * the accept to give any (wrongly) eager carrier a chance to show up. */
#define AG_PUMP_MAX 200

/* Build a JOIN_REQUEST into buf with the server's own version triple and a
 * trailing address-proof cookie (the given bytes, or zeros if NULL). Layout
 * mirrors transport_udp_client.c's builder: header + name + pass + 3 version
 * bytes + WBN token + flags + clientType + clientHints + 2-byte
 * fallbackCountry + JOIN_COOKIE_LEN cookie. Returns the total length. */
static int agBuildJoin(uint8_t *buf, const char *name,
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

/* Open a non-blocking UDP socket bound to an ephemeral 127.0.0.1 port. */
static SOCKET agOpenSocket(void) {
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

/* Pump the harness AG_PUMP_MAX times and record whether a JOIN_ACCEPT, a
 * PACKET_CHANNEL (the bulk map carrier), and/or a PACKET_JOIN_CHALLENGE was seen
 * across all replies on `sock`; on a challenge, *outCookie (if non-NULL)
 * receives the cookie bytes. `resendEvery` controls retransmits: 0 sends `pkt`
 * exactly once (used for JOINs, so each spends only a single per-source
 * rate-limit token).
 *
 * Only replies from `serverAddr` count, which is what makes the result
 * evidence about this server. This is an amplification test: a stray
 * JOIN_ACCEPT off the shared loopback would turn "the cookie-echoed JOIN drew
 * an accept" green without the server ever having sent one, and a stray in the
 * negative case would fail an address proof that actually held. */
static void agCollect(LoopbackHarness *h, SOCKET sock,
                      const uint8_t *pkt, int pktLen,
                      const struct sockaddr_in *serverAddr, int resendEvery,
                      bool *outAccept, bool *outChannel,
                      bool *outChallenge, uint8_t *outCookie) {
    int i;
    *outAccept = false;
    *outChannel = false;
    *outChallenge = false;
    for (i = 0; i < AG_PUMP_MAX; i++) {
        uint8_t in[2048];
        int n;
        bool doSend = (resendEvery > 0) ? (i % resendEvery == 0) : (i == 0);
        if (doSend) {
            sendto(sock, (const char *)pkt, pktLen, 0,
                   (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
        }
        loopbackHarnessPump(h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           serverAddr)) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT)            *outAccept = true;
            else if (type == PACKET_CHANNEL)           *outChannel = true;
            else if (type == PACKET_JOIN_CHALLENGE &&
                     n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                *outChallenge = true;
                if (outCookie != NULL) {
                    memcpy(outCookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                }
            }
        }
    }
}

int run_map_amp_gate(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    int joinLen;
    SOCKET sock;
    bool gotAccept, gotChannel, gotChallenge;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "AmpHost", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    sock = agOpenSocket();
    if (sock == INVALID_SOCKET) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not open raw UDP socket for crafted JOIN");
    }

    /* ---- Phase 1: cookie-less JOIN draws a challenge and nothing else ---- */
    joinLen = agBuildJoin(joinBuf, "AmpGate", NULL);
    agCollect(&h, sock, joinBuf, joinLen, &serverAddr, /*resendEvery*/ 0,
              &gotAccept, &gotChannel, &gotChallenge, cookie);
    fprintf(stderr, "  amp gate (cookie-less): challenge=%d accept=%d channel=%d\n",
            (int)gotChallenge, (int)gotAccept, (int)gotChannel);
    if (!gotChallenge) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("crafted JOIN drew no PACKET_JOIN_CHALLENGE within %d pumps",
                AG_PUMP_MAX);
    }
    if (gotAccept) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("crafted cookie-less JOIN was accepted (no address proof)");
    }
    if (gotChannel) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("server streamed a CHANNEL_BULK carrier before the address was "
                "proven — amplification gate is open");
    }

    /* ---- Phase 2: cookie echoed -> accept, and the map carrier opens ---- */
    joinLen = agBuildJoin(joinBuf, "AmpGate", cookie);
    agCollect(&h, sock, joinBuf, joinLen, &serverAddr, /*resendEvery*/ 0,
              &gotAccept, &gotChannel, &gotChallenge, NULL);
    fprintf(stderr, "  amp gate (cookie echoed): accept=%d channel=%d\n",
            (int)gotAccept, (int)gotChannel);

    closesocket(sock);

    if (!gotAccept) {
        loopbackHarnessStop(&h);
        UT_FAIL("cookie-echoed JOIN drew no JOIN_ACCEPT within %d pumps",
                AG_PUMP_MAX);
    }
    if (!gotChannel) {
        loopbackHarnessStop(&h);
        UT_FAIL("server sent no PACKET_CHANNEL (bulk map carrier) after the "
                "cookie-proven JOIN — the proven address opened no map stream");
    }

    loopbackHarnessStop(&h);
    return 0;
}
