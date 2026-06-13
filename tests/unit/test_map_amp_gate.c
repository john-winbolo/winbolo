/*
 * Map-send amplification-gate test.
 *
 * The server must not blast the compressed map (up to 64KB across many
 * PACKET_MAP_DOWNLOAD chunks) to a freshly-joined address before that address
 * has proven it can receive a reply. Otherwise a ~170-byte spoofed
 * JOIN_REQUEST reflects a large payload at a forged source. The gate: chunks
 * are withheld until the client's PACKET_MAP_ACK 0xFFFF "ready" round-trip
 * arrives.
 *
 * This stands up the loopback harness (a real server on an ephemeral port) and
 * drives a hand-built JOIN_REQUEST from a raw UDP socket the test owns, then
 * inspects the actual datagrams the server sends back:
 *   1. After a valid JOIN (and repeated resends), the replies contain a
 *      JOIN_ACCEPT but NOT a single PACKET_MAP_DOWNLOAD — no map before ready.
 *   2. After sending PACKET_MAP_ACK 0xFFFF from the same socket, the server
 *      now sends PACKET_MAP_DOWNLOAD chunks — the round-trip opened the gate.
 *
 * The crafted JOIN mirrors the client's JOIN_REQUEST layout (see
 * test_join_version_gate.c) so it clears the length/name/version gates and
 * reaches a full join with map-download init.
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
 * the accept to give any (wrongly) eager chunk send a chance to show up. */
#define AG_PUMP_MAX 200

/* Build a JOIN_REQUEST into buf with the server's own version triple. Layout
 * mirrors transport_udp_client.c's builder: header + name + pass + 3 version
 * bytes + WBN token + flags + clientType + clientHints + 2-byte
 * fallbackCountry. Returns the total length. */
static int agBuildJoin(uint8_t *buf, const char *name) {
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

/* Pump the harness AG_PUMP_MAX times, resending `pkt` every 8 pumps to ride
 * out best-effort delivery, and record whether a JOIN_ACCEPT and/or a
 * PACKET_MAP_DOWNLOAD chunk was seen across all replies on `sock`. */
static void agPumpCollect(LoopbackHarness *h, SOCKET sock,
                          const uint8_t *pkt, int pktLen,
                          const struct sockaddr_in *serverAddr,
                          bool *outAccept, bool *outMapDownload) {
    int i;
    *outAccept = false;
    *outMapDownload = false;
    for (i = 0; i < AG_PUMP_MAX; i++) {
        uint8_t in[2048];
        int n;
        if (i % 8 == 0) {
            sendto(sock, (const char *)pkt, pktLen, 0,
                   (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
        }
        loopbackHarnessPump(h);
        while ((n = (int)recvfrom(sock, (char *)in, sizeof(in), 0,
                                  NULL, NULL)) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT)       *outAccept = true;
            else if (type == PACKET_MAP_DOWNLOAD) *outMapDownload = true;
        }
    }
}

int run_map_amp_gate(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    uint8_t ackBuf[PACKET_HEADER_SIZE + 2];
    int joinLen;
    SOCKET sock;
    bool gotAccept, gotMapDownload;

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

    /* ---- Phase 1: JOIN with no ready ack -> accept, but NO map chunks ---- */
    joinLen = agBuildJoin(joinBuf, "AmpGate");
    agPumpCollect(&h, sock, joinBuf, joinLen, &serverAddr,
                  &gotAccept, &gotMapDownload);
    fprintf(stderr, "  amp gate (join only): accept=%d mapDownload=%d\n",
            (int)gotAccept, (int)gotMapDownload);

    if (!gotAccept) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("crafted JOIN drew no JOIN_ACCEPT within %d pumps", AG_PUMP_MAX);
    }
    if (gotMapDownload) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        UT_FAIL("server sent PACKET_MAP_DOWNLOAD before the MAP_ACK 0xFFFF "
                "ready round-trip — amplification gate is open");
    }

    /* ---- Phase 2: send MAP_ACK 0xFFFF -> chunks now flow ---- */
    packHeader(ackBuf, PACKET_MAP_ACK, 0);
    packU16(ackBuf + PACKET_HEADER_SIZE, 0xFFFF); /* 0xFFFF = "ready for map" */
    agPumpCollect(&h, sock, ackBuf, (int)sizeof(ackBuf), &serverAddr,
                  &gotAccept, &gotMapDownload);
    fprintf(stderr, "  amp gate (after ready): mapDownload=%d\n",
            (int)gotMapDownload);

    closesocket(sock);

    if (!gotMapDownload) {
        loopbackHarnessStop(&h);
        UT_FAIL("server sent no PACKET_MAP_DOWNLOAD after MAP_ACK 0xFFFF "
                "within %d pumps — ready round-trip did not open the gate",
                AG_PUMP_MAX);
    }

    loopbackHarnessStop(&h);
    return 0;
}
