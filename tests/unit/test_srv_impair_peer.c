/*
 * srvImpairPeer (src/server/udp/udp_server_recv.c): the server's test of
 * whether a datagram to or from a peer goes through the -netimpair layer.
 * With lanonly=0 every peer is impaired; with lanonly=1 only LAN peers
 * (netImpairAddrIsLan) are. In a build with Bot Host support a
 * -bothosts address is never impaired, and the second half of the test
 * checks that too. That half is compiled only when BOT_HOST_LINK_H, the
 * include guard of bot_host_link.h, is defined.
 *
 * No sockets: the sockaddr_in values are filled constants. srvImpairPeer
 * reads only ni->cfg.lanOnly, so the layer itself is never enabled.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform_net.h"                   /* struct sockaddr_in, htonl */
#include "net_impair.h"
#include "transport_udp_server_internal.h"  /* srvImpairPeer, udpServer */
#include "test_harness.h"

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                         ((uint32_t)(c) << 8) | (uint32_t)(d))

static struct sockaddr_in peerAddr(uint32_t ipHostOrder, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ipHostOrder);
    a.sin_port = htons(port);
    return a;
}

/* NetImpair is about 1 MB (its packet queue), so keep it off the stack. */
static NetImpair ni;

int run_srv_impair_peer_lanonly(void) {
    struct sockaddr_in lan = peerAddr(IP4(192, 168, 0, 130), 27500);
    struct sockaddr_in loop = peerAddr(IP4(127, 0, 0, 1), 27500);
    struct sockaddr_in linkLocal = peerAddr(IP4(169, 254, 10, 20), 27500);
    struct sockaddr_in net = peerAddr(IP4(203, 0, 113, 7), 27500);
    struct sockaddr_in cgnat = peerAddr(IP4(100, 64, 0, 1), 27500);

    /* lanonly=0: every peer is impaired, LAN or internet. */
    netImpairInit(&ni);
    ni.cfg.lanOnly = false;
    UT_ASSERT(srvImpairPeer(&ni, &lan));
    UT_ASSERT(srvImpairPeer(&ni, &loop));
    UT_ASSERT(srvImpairPeer(&ni, &net));
    UT_ASSERT(srvImpairPeer(&ni, &cgnat));

    /* lanonly=1: LAN peers are impaired, internet peers are not. */
    ni.cfg.lanOnly = true;
    UT_ASSERT(srvImpairPeer(&ni, &lan));
    UT_ASSERT(srvImpairPeer(&ni, &loop));
    UT_ASSERT(srvImpairPeer(&ni, &linkLocal));
    UT_ASSERT(!srvImpairPeer(&ni, &net));
    UT_ASSERT(!srvImpairPeer(&ni, &cgnat));

#ifdef BOT_HOST_LINK_H
    /* A -bothosts address is on the LAN but never impaired, on any port.
     * Another LAN peer still is. The list is put back after. */
    {
        int savedCount = udpServer.botHostAllowCount;
        BotHostAllow savedFirst = udpServer.botHostAllow[0];
        struct sockaddr_in hostOtherPort = peerAddr(IP4(192, 168, 0, 130), 40001);
        struct sockaddr_in otherLan = peerAddr(IP4(192, 168, 0, 58), 27500);
        bool hostImpaired, hostOtherPortImpaired, otherLanImpaired;
        bool hostOffLanOnly;

        udpServer.botHostAllow[0].ip = htonl(IP4(192, 168, 0, 130));
        udpServer.botHostAllow[0].port = 0;
        udpServer.botHostAllowCount = 1;
        hostImpaired = srvImpairPeer(&ni, &lan);
        hostOtherPortImpaired = srvImpairPeer(&ni, &hostOtherPort);
        otherLanImpaired = srvImpairPeer(&ni, &otherLan);
        ni.cfg.lanOnly = false;
        hostOffLanOnly = srvImpairPeer(&ni, &lan);
        udpServer.botHostAllow[0] = savedFirst;
        udpServer.botHostAllowCount = savedCount;

        UT_ASSERT(!hostImpaired);
        UT_ASSERT(!hostOtherPortImpaired);
        UT_ASSERT(otherLanImpaired);
        /* With lanonly=0 the list is not read: the old behaviour. */
        UT_ASSERT(hostOffLanOnly);
    }
#endif
    return 0;
}
