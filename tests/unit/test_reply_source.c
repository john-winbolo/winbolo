/*
 * The server's replies leave from the address each datagram arrived on.
 *
 * The server socket is bound to the any address. udpEnableRecvLocalAddr asks
 * it to report the destination address of every datagram, udpRecvFromLocal
 * returns that address beside the source, and udpSendToFrom names it as the
 * source of the reply, so a host with several addresses of one family answers
 * from the one the peer dialled. Five cases:
 *
 *   reply_source_recv_reports_local_v6
 *       A datagram to [::1]:port on the dual-stack server socket reports ::1
 *       as its local address.
 *
 *   reply_source_recv_reports_local_v4_mapped
 *       A datagram to 127.0.0.1:port on the same dual-stack socket reports
 *       IPv4 loopback, in the socket's family (mapped). No report at all is a
 *       failure, not a skip: macOS and Linux both have to give one.
 *
 *   reply_source_send_from_local
 *       After a receive as above, a reply sent with udpSendToFrom naming the
 *       reported local address reaches the peer from the address it dialled,
 *       over ::1 and over 127.0.0.1.
 *
 *   reply_source_ipv4_socket
 *       The same receive and reply on the AF_INET fallback socket
 *       (udpTestForceIPv4Socket), over 127.0.0.1.
 *
 *   reply_source_slot_stores_local
 *       Through the loopback harness, one client dialling ::1 and one dialling
 *       127.0.0.1 join one server, and each slot stores the loopback address
 *       of the family its client dialled.
 *
 * The cases that need an IPv6 loopback pass as skipped when the machine has
 * none (loopbackIpv6Usable); the mapped cases pass as skipped when the game's
 * socket factory opens the IPv4 fallback.
 *
 * Every datagram carries a nonce, and a datagram without it is read and
 * dropped: a full ctest -j run has other loopback servers sending to
 * ephemeral ports, and one may land on a port these cases hold.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "platform_net.h"
#include "bolo_addr.h"
#include "client_sim.h"
#include "client_net.h"                     /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "transport_udp.h"                  /* transportUdpServerTestClientLocalAddr */
#include "transport_udp_internal.h"         /* createUdpSocket,
                                             * udpEnableRecvLocalAddr,
                                             * udpRecvFromLocal, udpSendToFrom,
                                             * udpTestForceIPv4Socket */
#include "test_harness.h"
#include "loopback_harness.h"

/* How long one datagram has to cross the loopback interface. */
#define RS_WAIT_MS 2000

/* Join on a clean path, as the other loopback cases budget it. */
#define RS_CONNECT_MAX 2000

#define RS_SEED 0x5EC5u

#define RS_DATAGRAM_LEN 8

typedef struct {
    SOCKET   srv;
    int      srvFamily;
    uint16_t port;
    SOCKET   peer;
} RsSockets;

static void rsInit(RsSockets *s) {
    s->srv = INVALID_SOCKET;
    s->srvFamily = 0;
    s->port = 0;
    s->peer = INVALID_SOCKET;
}

static void rsClosePeer(RsSockets *s) {
    if (s->peer != INVALID_SOCKET) {
        closesocket(s->peer);
        s->peer = INVALID_SOCKET;
    }
}

static void rsClose(RsSockets *s) {
    rsClosePeer(s);
    if (s->srv != INVALID_SOCKET) {
        closesocket(s->srv);
        s->srv = INVALID_SOCKET;
    }
}

static uint32_t rsNextNonce(void) {
    static uint32_t count = 0;
    count++;
    return (uint32_t)SDL_GetPerformanceCounter() ^ (count * 0x9e3779b9u);
}

static void rsPack(uint8_t out[RS_DATAGRAM_LEN], uint32_t nonce) {
    out[0] = 'R';
    out[1] = 'S';
    out[2] = 'R';
    out[3] = 'C';
    out[4] = (uint8_t)(nonce >> 24);
    out[5] = (uint8_t)(nonce >> 16);
    out[6] = (uint8_t)(nonce >> 8);
    out[7] = (uint8_t)nonce;
}

static bool rsMatches(const uint8_t *buf, int len, uint32_t nonce) {
    uint8_t want[RS_DATAGRAM_LEN];
    rsPack(want, nonce);
    return len == RS_DATAGRAM_LEN && memcmp(buf, want, RS_DATAGRAM_LEN) == 0;
}

/* Waits up to ms for s to have a datagram to read. */
static bool rsWaitReadable(SOCKET s, int ms) {
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(s, &readfds);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select((int)(s + 1), &readfds, NULL, NULL, &tv) > 0;
}

static void rsText(const BoloAddr *a, char *out, size_t outLen) {
    if (boloAddrFamily(a) == 0) {
        snprintf(out, outLen, "(none)");
    } else if (!boloAddrFormatHost(a, out, outLen)) {
        snprintf(out, outLen, "(unprintable)");
    }
}

/* Opens a server socket as transportUdpServerCreate does: createUdpSocket,
 * the local-address option, then a bind to the any address of the socket's
 * family on a port the system picks. With forceV4 the IPv4 fallback socket
 * is opened, and the switch is turned off again before this returns. */
static bool rsOpenServer(RsSockets *s, bool forceV4) {
    BoloAddr any;
    BoloAddr bound;
    struct sockaddr_storage ss;
    socklen_t ssLen = (socklen_t)sizeof(ss);

    bolo_net_init();
    if (forceV4) {
        udpTestForceIPv4Socket(true);
    }
    s->srv = createUdpSocket(true, &s->srvFamily);
    if (forceV4) {
        udpTestForceIPv4Socket(false);
    }
    if (s->srv == INVALID_SOCKET) {
        return false;
    }
    udpEnableRecvLocalAddr(s->srv, s->srvFamily);
    boloAddrAny(s->srvFamily, 0, &any);
    if (bind(s->srv, (const struct sockaddr *)&any.ss, any.len) != 0) {
        return false;
    }
    memset(&ss, 0, sizeof(ss));
    if (getsockname(s->srv, (struct sockaddr *)&ss, &ssLen) != 0 ||
        !boloAddrFromSockaddr((const struct sockaddr *)&ss, ssLen, &bound)) {
        return false;
    }
    s->port = boloAddrPort(&bound);
    return s->port != 0;
}

/* Reads the server socket until the datagram carrying nonce arrives. */
static bool rsServerRecv(RsSockets *s, uint32_t nonce, BoloAddr *from,
                         BoloAddr *localAddr) {
    uint64_t deadline = SDL_GetTicks() + RS_WAIT_MS;
    while (SDL_GetTicks() < deadline) {
        uint8_t buf[UDP_MAX_PAYLOAD];
        int n;
        if (!rsWaitReadable(s->srv, 50)) {
            continue;
        }
        while ((n = udpRecvFromLocal(s->srv, buf, (int)sizeof(buf), from,
                                     localAddr)) > 0) {
            if (rsMatches(buf, n, nonce)) {
                return true;
            }
        }
    }
    return false;
}

/* Reads the peer socket until the datagram carrying nonce arrives, and gives
 * the source it came from. */
static bool rsPeerRecv(RsSockets *s, uint32_t nonce, BoloAddr *src) {
    uint64_t deadline = SDL_GetTicks() + RS_WAIT_MS;
    while (SDL_GetTicks() < deadline) {
        uint8_t buf[UDP_MAX_PAYLOAD];
        struct sockaddr_storage ss;
        socklen_t ssLen = (socklen_t)sizeof(ss);
        int n;
        if (!rsWaitReadable(s->peer, 50)) {
            continue;
        }
        memset(&ss, 0, sizeof(ss));
        n = (int)recvfrom(s->peer, (char *)buf, (int)sizeof(buf), 0,
                          (struct sockaddr *)&ss, &ssLen);
        if (n > 0 && rsMatches(buf, n, nonce) &&
            boloAddrFromSockaddr((const struct sockaddr *)&ss, ssLen, src)) {
            return true;
        }
    }
    return false;
}

/* A peer socket dials hostText on the server's port and sends one datagram,
 * which the server receives with udpRecvFromLocal. */
static int rsReceiveOne(RsSockets *s, const char *hostText, BoloAddr *dialled,
                        BoloAddr *from, BoloAddr *localAddr) {
    uint8_t out[RS_DATAGRAM_LEN];
    uint32_t nonce = rsNextNonce();

    if (!boloAddrParse(hostText, s->port, dialled)) {
        UT_FAIL("\"%s\" does not parse", hostText);
    }
    rsClosePeer(s);
    s->peer = socket(boloAddrFamily(dialled), SOCK_DGRAM, IPPROTO_UDP);
    if (s->peer == INVALID_SOCKET) {
        UT_FAIL("no peer socket for %s", hostText);
    }
    rsPack(out, nonce);
    if (sendto(s->peer, (const char *)out, RS_DATAGRAM_LEN, 0,
               (const struct sockaddr *)&dialled->ss, dialled->len) !=
        RS_DATAGRAM_LEN) {
        UT_FAIL("the peer could not send to %s:%u", hostText,
                (unsigned)s->port);
    }
    if (!rsServerRecv(s, nonce, from, localAddr)) {
        UT_FAIL("the server socket never received the datagram sent to "
                "%s:%u", hostText, (unsigned)s->port);
    }
    return 0;
}

/* The server replies to from naming localAddr as the source, and the reply
 * reaches the peer from exactly the address it dialled. */
static int rsReplyComesFromDialled(RsSockets *s, const BoloAddr *dialled,
                                   const BoloAddr *from,
                                   const BoloAddr *localAddr) {
    uint8_t out[RS_DATAGRAM_LEN];
    uint32_t nonce = rsNextNonce();
    BoloAddr src;
    char srcText[BOLO_ADDR_STR_LEN];
    char wantText[BOLO_ADDR_STR_LEN];

    rsPack(out, nonce);
    udpSendToFrom(s->srv, s->srvFamily, out, RS_DATAGRAM_LEN, from, localAddr);
    if (!rsPeerRecv(s, nonce, &src)) {
        UT_FAIL("the reply sent from the local address never reached the "
                "peer");
    }
    if (!boloAddrEqual(&src, dialled)) {
        if (!boloAddrFormat(&src, srcText, sizeof(srcText))) {
            snprintf(srcText, sizeof(srcText), "(unprintable)");
        }
        if (!boloAddrFormat(dialled, wantText, sizeof(wantText))) {
            snprintf(wantText, sizeof(wantText), "(unprintable)");
        }
        UT_FAIL("the reply came from %s, not the dialled %s", srcText,
                wantText);
    }
    return 0;
}

static int rsCaseRecvV6(RsSockets *s) {
    BoloAddr dialled, from, localAddr;
    char text[BOLO_ADDR_STR_LEN];

    if (!rsOpenServer(s, false)) {
        UT_FAIL("the server socket did not open and bind");
    }
    if (s->srvFamily != AF_INET6) {
        SDL_Log("  reply_source_recv_reports_local_v6: skipped — the game's "
                "socket is the IPv4 fallback here");
        return 0;
    }
    if (rsReceiveOne(s, "::1", &dialled, &from, &localAddr) != 0) {
        return 1;
    }
    rsText(&localAddr, text, sizeof(text));
    if (boloAddrFamily(&localAddr) != AF_INET6 || boloAddrIsV4(&localAddr) ||
        !boloAddrIsLoopback(&localAddr)) {
        UT_FAIL("a datagram to [::1] reported local address %s, not ::1",
                text);
    }
    if (boloAddrPort(&localAddr) != 0) {
        UT_FAIL("the local address carries port %u, not 0",
                (unsigned)boloAddrPort(&localAddr));
    }
    fprintf(stderr, "  reply source: datagram to [::1]:%u reported %s\n",
            (unsigned)s->port, text);
    return 0;
}

int run_reply_source_recv_reports_local_v6(void) {
    RsSockets s;
    int rc;
    if (!loopbackIpv6Usable("reply_source_recv_reports_local_v6")) {
        return 0;
    }
    rsInit(&s);
    rc = rsCaseRecvV6(&s);
    rsClose(&s);
    return rc;
}

static int rsCaseRecvV4Mapped(RsSockets *s) {
    BoloAddr dialled, from, localAddr;
    char text[BOLO_ADDR_STR_LEN];

    if (!rsOpenServer(s, false)) {
        UT_FAIL("the server socket did not open and bind");
    }
    if (s->srvFamily != AF_INET6) {
        SDL_Log("  reply_source_recv_reports_local_v4_mapped: skipped — the "
                "game's socket is the IPv4 fallback here, so there is no "
                "dual-stack socket");
        return 0;
    }
    if (rsReceiveOne(s, "127.0.0.1", &dialled, &from, &localAddr) != 0) {
        return 1;
    }
    rsText(&localAddr, text, sizeof(text));
    if (boloAddrFamily(&localAddr) == 0) {
        UT_FAIL("the dual-stack socket reported no local address for an "
                "IPv4 datagram");
    }
    if (boloAddrFamily(&localAddr) != AF_INET6 || !boloAddrIsV4(&localAddr) ||
        !boloAddrIsLoopback(&localAddr)) {
        UT_FAIL("a datagram to 127.0.0.1 reported local address %s (family "
                "%d), not mapped IPv4 loopback", text, boloAddrFamily(&localAddr));
    }
    fprintf(stderr, "  reply source: datagram to 127.0.0.1:%u reported %s\n",
            (unsigned)s->port, text);
    return 0;
}

int run_reply_source_recv_reports_local_v4_mapped(void) {
    RsSockets s;
    int rc;
    rsInit(&s);
    rc = rsCaseRecvV4Mapped(&s);
    rsClose(&s);
    return rc;
}

static int rsCaseSendFromLocal(RsSockets *s) {
    BoloAddr dialled, from, localAddr;

    if (!rsOpenServer(s, false)) {
        UT_FAIL("the server socket did not open and bind");
    }
    if (s->srvFamily != AF_INET6) {
        SDL_Log("  reply_source_send_from_local: skipped — the game's socket "
                "is the IPv4 fallback here");
        return 0;
    }

    if (loopbackIpv6Usable("reply_source_send_from_local (::1)")) {
        if (rsReceiveOne(s, "::1", &dialled, &from, &localAddr) != 0) {
            return 1;
        }
        if (boloAddrFamily(&localAddr) == 0) {
            UT_FAIL("a datagram to [::1] reported no local address");
        }
        if (rsReplyComesFromDialled(s, &dialled, &from, &localAddr) != 0) {
            return 1;
        }
        fprintf(stderr, "  reply source: reply over ::1 came from the "
                        "dialled address\n");
    }

    if (rsReceiveOne(s, "127.0.0.1", &dialled, &from, &localAddr) != 0) {
        return 1;
    }
    if (boloAddrFamily(&localAddr) == 0) {
        UT_FAIL("a datagram to 127.0.0.1 reported no local address");
    }
    if (rsReplyComesFromDialled(s, &dialled, &from, &localAddr) != 0) {
        return 1;
    }
    fprintf(stderr, "  reply source: reply over 127.0.0.1 (mapped) came from "
                    "the dialled address\n");
    return 0;
}

int run_reply_source_send_from_local(void) {
    RsSockets s;
    int rc;
    rsInit(&s);
    rc = rsCaseSendFromLocal(&s);
    rsClose(&s);
    return rc;
}

static int rsCaseIpv4Socket(RsSockets *s) {
    BoloAddr dialled, from, localAddr;
    char text[BOLO_ADDR_STR_LEN];

    /* rsOpenServer turns udpTestForceIPv4Socket off again before it
     * returns, on every path. */
    if (!rsOpenServer(s, true)) {
        UT_FAIL("the IPv4 fallback server socket did not open and bind");
    }
    if (s->srvFamily != AF_INET) {
        UT_FAIL("the server socket is family %d, not the AF_INET fallback",
                s->srvFamily);
    }
    if (rsReceiveOne(s, "127.0.0.1", &dialled, &from, &localAddr) != 0) {
        return 1;
    }
    rsText(&localAddr, text, sizeof(text));
    if (boloAddrFamily(&localAddr) != AF_INET || !boloAddrIsLoopback(&localAddr)) {
        UT_FAIL("a datagram to 127.0.0.1 on the AF_INET socket reported "
                "local address %s, not 127.0.0.1", text);
    }
    if (rsReplyComesFromDialled(s, &dialled, &from, &localAddr) != 0) {
        return 1;
    }
    fprintf(stderr, "  reply source: AF_INET socket reported %s and the "
                    "reply came from the dialled address\n", text);
    return 0;
}

int run_reply_source_ipv4_socket(void) {
    RsSockets s;
    int rc;
    rsInit(&s);
    rc = rsCaseIpv4Socket(&s);
    rsClose(&s);
    udpTestForceIPv4Socket(false);
    return rc;
}

static bool rsBothConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs)  == CLIENT_CONNECT_CONNECTED &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

static int rsCaseSlotStoresLocal(LoopbackHarness *h) {
    BoloAddr localAddr1, localAddr2;
    char text1[BOLO_ADDR_STR_LEN];
    char text2[BOLO_ADDR_STR_LEN];
    BYTE slot1, slot2;
    int at;

    /* Started with the IPv6 dial, so the server binds the any address and
     * both clients can reach it. */
    if (!loopbackHarnessStartDial(h, "SixSource", /*lobbyMode*/ false,
                                  /*impairSpec*/ NULL, RS_SEED,
                                  LOOPBACK_DIAL_IPV6)) {
        UT_FAIL("harness start dialling ::1 failed");
    }
    if (!loopbackHarnessAddClientDial(h, "FourSource", LOOPBACK_DIAL_IPV4)) {
        UT_FAIL("the 127.0.0.1 client failed to connect");
    }
    at = loopbackHarnessPumpUntil(h, RS_CONNECT_MAX, rsBothConnected, NULL);
    if (at < 0) {
        UT_FAIL("both clients never reached CONNECTED within %d pumps",
                RS_CONNECT_MAX);
    }

    slot1 = clientSimGetMyPlayerNum(h->cs);
    slot2 = clientSimGetMyPlayerNum(h->cs2);
    if (slot1 >= MAX_TANKS || slot2 >= MAX_TANKS || slot1 == slot2) {
        UT_FAIL("the two clients hold slots %u and %u", (unsigned)slot1,
                (unsigned)slot2);
    }
    if (!transportUdpServerTestClientLocalAddr(slot1, &localAddr1) ||
        !transportUdpServerTestClientLocalAddr(slot2, &localAddr2)) {
        UT_FAIL("the server has no connected slot %u or %u", (unsigned)slot1,
                (unsigned)slot2);
    }
    rsText(&localAddr1, text1, sizeof(text1));
    rsText(&localAddr2, text2, sizeof(text2));
    if (boloAddrFamily(&localAddr1) != AF_INET6 || boloAddrIsV4(&localAddr1) ||
        !boloAddrIsLoopback(&localAddr1)) {
        UT_FAIL("the ::1 client's slot %u stores local address %s, not ::1",
                (unsigned)slot1, text1);
    }
    if (!boloAddrIsV4(&localAddr2) || !boloAddrIsLoopback(&localAddr2)) {
        UT_FAIL("the 127.0.0.1 client's slot %u stores local address %s, "
                "not IPv4 loopback", (unsigned)slot2, text2);
    }
    fprintf(stderr, "  reply source: slot %u stores %s, slot %u stores %s\n",
            (unsigned)slot1, text1, (unsigned)slot2, text2);
    return 0;
}

int run_reply_source_slot_stores_local(void) {
    LoopbackHarness h;
    int rc;
    if (!loopbackIpv6Usable("reply_source_slot_stores_local")) {
        return 0;
    }
    memset(&h, 0, sizeof(h));
    rc = rsCaseSlotStoresLocal(&h);
    loopbackHarnessStop(&h);
    return rc;
}
