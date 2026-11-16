/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Hole punching through the tracker for a host of either address family.
 *
 *   punch_addr_codec          the [family 1][address 16][port 2] body:
 *                             round trips, exact bytes, and what is refused.
 *   punch_request_build       the joiner's request: the old
 *                             PACKET_PUNCH_REQUEST byte for byte for an IPv4
 *                             host, PACKET_PUNCH_REQUEST6 for an IPv6 one,
 *                             nothing for a loopback or link-local host.
 *   punch_notify_dispatch     the server puts the joiner from either notify
 *                             form in the punch queue and drops a link-local
 *                             or loopback one.
 *   punch_probe_reply_status  both probe-reply forms reach the port-map
 *                             status; SERVER_PORTMAP_IPV6_OK appears only
 *                             where the status would otherwise be FAILED.
 *
 * The server cases call serverProcessPacket with no sim: the punch handlers
 * read only the datagram, the punch queue and the probe state.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "platform_net.h"
#include "bolo_addr.h"
#include "netpacks.h"                       /* PACKET_PUNCH_*, PUNCH_ADDR_* */
#include "transport_udp.h"
#include "transport_udp_internal.h"         /* packPunchAddr, unpackPunchAddr,
                                             * buildPunchRequest, packHeader */
#include "transport_udp_server_internal.h"  /* serverProcessPacket, punchQueue */
#include "server_lifecycle.h"               /* serverInstanceGetPortmapInfo,
                                             * serverInstanceTestSetPortmapState */
#include "threads.h"                        /* threadsCreate */
#include "test_harness.h"

#define PUNCH_TEST_PORT 27500

/* 2001:db8::1 port 27500 as a punch address body. */
static const uint8_t kV6Body[PUNCH_ADDR_WIRE_SIZE] = {
    6,
    0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01,
    0x6b, 0x6c
};

/* 203.0.113.5 port 27500 as a punch address body. */
static const uint8_t kV4Body[PUNCH_ADDR_WIRE_SIZE] = {
    4,
    203, 0, 113, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x6b, 0x6c
};

int run_punch_addr_codec(void) {
    BoloAddr v6, v4, mapped, out;
    uint8_t body[PUNCH_ADDR_WIRE_SIZE];
    uint8_t bad[PUNCH_ADDR_WIRE_SIZE];

    UT_ASSERT(boloAddrParse("2001:db8::1", PUNCH_TEST_PORT, &v6));
    UT_ASSERT(boloAddrParse("203.0.113.5", PUNCH_TEST_PORT, &v4));
    UT_ASSERT(boloAddrParse("::ffff:203.0.113.5", PUNCH_TEST_PORT, &mapped));
    UT_ASSERT(boloAddrFamily(&mapped) == AF_INET6);

    /* IPv6: exact bytes and back. */
    UT_ASSERT(packPunchAddr(body, &v6) == PUNCH_ADDR_WIRE_SIZE);
    UT_ASSERT_MSG(memcmp(body, kV6Body, sizeof(kV6Body)) == 0,
                  "IPv6 body bytes differ");
    UT_ASSERT(unpackPunchAddr(body, (int)sizeof(body), &out));
    UT_ASSERT(boloAddrFamily(&out) == AF_INET6);
    UT_ASSERT(boloAddrEqual(&out, &v6));

    /* IPv4: exact bytes and back. */
    UT_ASSERT(packPunchAddr(body, &v4) == PUNCH_ADDR_WIRE_SIZE);
    UT_ASSERT_MSG(memcmp(body, kV4Body, sizeof(kV4Body)) == 0,
                  "IPv4 body bytes differ");
    UT_ASSERT(unpackPunchAddr(body, (int)sizeof(body), &out));
    UT_ASSERT(boloAddrFamily(&out) == AF_INET);
    UT_ASSERT(boloAddrEqual(&out, &v4));

    /* A mapped IPv4 address is packed as family 4. */
    UT_ASSERT(packPunchAddr(body, &mapped) == PUNCH_ADDR_WIRE_SIZE);
    UT_ASSERT_MSG(memcmp(body, kV4Body, sizeof(kV4Body)) == 0,
                  "mapped IPv4 body is not the family-4 body");
    UT_ASSERT(unpackPunchAddr(body, (int)sizeof(body), &out));
    UT_ASSERT(boloAddrEqual(&out, &mapped));

    /* A short body. */
    UT_ASSERT(!unpackPunchAddr(kV6Body, PUNCH_ADDR_WIRE_SIZE - 1, &out));
    UT_ASSERT(!unpackPunchAddr(kV6Body, 0, &out));

    /* Families other than 4 and 6. */
    memcpy(bad, kV6Body, sizeof(bad));
    bad[0] = 0;
    UT_ASSERT(!unpackPunchAddr(bad, (int)sizeof(bad), &out));
    bad[0] = 7;
    UT_ASSERT(!unpackPunchAddr(bad, (int)sizeof(bad), &out));

    /* Unspecified addresses, of each family. */
    memset(bad, 0, sizeof(bad));
    bad[0] = PUNCH_ADDR_FAMILY_V6;
    bad[18] = 1;
    UT_ASSERT(!unpackPunchAddr(bad, (int)sizeof(bad), &out));
    bad[0] = PUNCH_ADDR_FAMILY_V4;
    UT_ASSERT(!unpackPunchAddr(bad, (int)sizeof(bad), &out));

    /* Addresses that are never packed: link-local of each family, loopback,
     * unspecified, empty. */
    {
        static const char *const refused[] = {
            "fe80::1", "169.254.1.1", "::1", "127.0.0.1", "::", "0.0.0.0"
        };
        size_t i;
        for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            BoloAddr a;
            UT_ASSERT_MSG(boloAddrParse(refused[i], PUNCH_TEST_PORT, &a),
                          "parse %s", refused[i]);
            UT_ASSERT_MSG(packPunchAddr(body, &a) == 0,
                          "%s was packed", refused[i]);
        }
        boloAddrClear(&out);
        UT_ASSERT(packPunchAddr(body, &out) == 0);
    }
    return 0;
}

int run_punch_request_build(void) {
    BoloAddr v6, v4, mapped, linkLocal, loop;
    uint8_t buf[PUNCH_REQUEST_MAX_LEN];
    uint8_t want[PUNCH_REQUEST_MAX_LEN];
    struct in_addr ip;
    int len;

    UT_ASSERT(boloAddrParse("2001:db8::1", PUNCH_TEST_PORT, &v6));
    UT_ASSERT(boloAddrParse("203.0.113.5", PUNCH_TEST_PORT, &v4));
    UT_ASSERT(boloAddrParse("::ffff:203.0.113.5", PUNCH_TEST_PORT, &mapped));
    UT_ASSERT(boloAddrParse("fe80::1", PUNCH_TEST_PORT, &linkLocal));
    UT_ASSERT(boloAddrParse("::1", PUNCH_TEST_PORT, &loop));

    /* IPv4 target: the 14-byte request the sender built before, made here
     * the way it made it (address from boloAddrV4, port with packU16). */
    packHeader(want, PACKET_PUNCH_REQUEST, 7);
    UT_ASSERT(boloAddrV4(&v4, &ip));
    memcpy(want + PACKET_HEADER_SIZE, &ip, 4);
    packU16(want + PACKET_HEADER_SIZE + 4, PUNCH_TEST_PORT);
    len = buildPunchRequest(buf, (int)sizeof(buf), &v4, 7);
    UT_ASSERT_MSG(len == PACKET_HEADER_SIZE + 6, "len %d", len);
    UT_ASSERT_MSG(memcmp(buf, want, (size_t)len) == 0,
                  "IPv4 request differs from the old one");
    UT_ASSERT(buf[PACKET_HEADER_SIZE] == 203 &&
              buf[PACKET_HEADER_SIZE + 3] == 5);

    /* A mapped IPv4 target is the same old request. */
    len = buildPunchRequest(buf, (int)sizeof(buf), &mapped, 7);
    UT_ASSERT_MSG(len == PACKET_HEADER_SIZE + 6, "len %d", len);
    UT_ASSERT_MSG(memcmp(buf, want, (size_t)len) == 0,
                  "mapped IPv4 request differs from the old one");

    /* IPv6 target: the new request with the 19-byte body. */
    packHeader(want, PACKET_PUNCH_REQUEST6, 8);
    memcpy(want + PACKET_HEADER_SIZE, kV6Body, sizeof(kV6Body));
    len = buildPunchRequest(buf, (int)sizeof(buf), &v6, 8);
    UT_ASSERT_MSG(len == PACKET_HEADER_SIZE + PUNCH_ADDR_WIRE_SIZE,
                  "len %d", len);
    UT_ASSERT(getPacketType(buf, len) == PACKET_PUNCH_REQUEST6);
    UT_ASSERT_MSG(memcmp(buf, want, (size_t)len) == 0,
                  "IPv6 request bytes differ");

    /* No request for a link-local or loopback host, an empty address or a
     * buffer that is too small. */
    UT_ASSERT(buildPunchRequest(buf, (int)sizeof(buf), &linkLocal, 9) == 0);
    UT_ASSERT(buildPunchRequest(buf, (int)sizeof(buf), &loop, 9) == 0);
    {
        BoloAddr none;
        boloAddrClear(&none);
        UT_ASSERT(buildPunchRequest(buf, (int)sizeof(buf), &none, 9) == 0);
    }
    UT_ASSERT(buildPunchRequest(buf, PUNCH_REQUEST_MAX_LEN - 1, &v4, 9) == 0);
    return 0;
}

/* Hands one tracker datagram to the server's dispatcher. */
static void punchDeliver(uint8_t *pkt, int len) {
    BoloAddr tracker;
    boloAddrParse("198.51.100.1", 50000, &tracker);
    threadsWaitForMutex();
    serverProcessPacket(NULL, pkt, len, &tracker);
    threadsReleaseMutex();
}

static int punchQueueUsed(void) {
    int i, n = 0;
    for (i = 0; i < PUNCH_QUEUE_SIZE; i++) {
        if (punchQueue[i].packetsRemaining != 0) n++;
    }
    return n;
}

int run_punch_notify_dispatch(void) {
    int rc = 0;
    uint8_t pkt[PACKET_HEADER_SIZE + PUNCH_ADDR_WIRE_SIZE];
    BoloAddr joiner6, joiner4;

    threadsCreate(true);
    memset(punchQueue, 0, sizeof(punchQueue));
    UT_CHECK(boloAddrParse("2001:db8::5", 40000, &joiner6));
    UT_CHECK(boloAddrParse("203.0.113.9", 40001, &joiner4));

    /* PACKET_PUNCH_NOTIFY6 with an IPv6 joiner. */
    packHeader(pkt, PACKET_PUNCH_NOTIFY6, 0);
    UT_CHECK(packPunchAddr(pkt + PACKET_HEADER_SIZE, &joiner6) ==
             PUNCH_ADDR_WIRE_SIZE);
    punchDeliver(pkt, (int)sizeof(pkt));
    UT_CHECK(punchQueueUsed() == 1);
    UT_CHECK(punchQueue[0].packetsRemaining == PUNCH_BURST_PACKETS);
    UT_CHECK(boloAddrFamily(&punchQueue[0].addr) == AF_INET6);
    UT_CHECK(boloAddrEqual(&punchQueue[0].addr, &joiner6));
    UT_CHECK(boloAddrPort(&punchQueue[0].addr) == 40000);

    /* The old PACKET_PUNCH_NOTIFY with an IPv4 joiner. */
    {
        struct in_addr ip;
        packHeader(pkt, PACKET_PUNCH_NOTIFY, 0);
        UT_CHECK(boloAddrV4(&joiner4, &ip));
        memcpy(pkt + PACKET_HEADER_SIZE, &ip, 4);
        packU16(pkt + PACKET_HEADER_SIZE + 4, 40001);
        punchDeliver(pkt, PACKET_HEADER_SIZE + 6);
    }
    UT_CHECK(punchQueueUsed() == 2);
    UT_CHECK(boloAddrFamily(&punchQueue[1].addr) == AF_INET);
    UT_CHECK(boloAddrEqual(&punchQueue[1].addr, &joiner4));

    /* A link-local and a loopback joiner in PACKET_PUNCH_NOTIFY6 are
     * dropped. The bodies are written by hand: packPunchAddr refuses both. */
    {
        static const uint8_t linkLocal[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                               0, 0, 0, 0, 0, 0, 0, 1 };
        static const uint8_t loop[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1 };
        packHeader(pkt, PACKET_PUNCH_NOTIFY6, 0);
        pkt[PACKET_HEADER_SIZE] = PUNCH_ADDR_FAMILY_V6;
        memcpy(pkt + PACKET_HEADER_SIZE + 1, linkLocal, 16);
        packU16(pkt + PACKET_HEADER_SIZE + 17, 40002);
        punchDeliver(pkt, (int)sizeof(pkt));
        UT_CHECK_MSG(punchQueueUsed() == 2, "link-local joiner was queued");

        memcpy(pkt + PACKET_HEADER_SIZE + 1, loop, 16);
        punchDeliver(pkt, (int)sizeof(pkt));
        UT_CHECK_MSG(punchQueueUsed() == 2, "loopback joiner was queued");
    }

    /* A PACKET_PUNCH_NOTIFY6 one byte short is dropped. */
    packHeader(pkt, PACKET_PUNCH_NOTIFY6, 0);
    UT_CHECK(packPunchAddr(pkt + PACKET_HEADER_SIZE, &joiner6) ==
             PUNCH_ADDR_WIRE_SIZE);
    punchDeliver(pkt, (int)sizeof(pkt) - 1);
    UT_CHECK(punchQueueUsed() == 2);

done:
    memset(punchQueue, 0, sizeof(punchQueue));
    return rc;
}

/* Delivers a PACKET_PUNCH_PROBE_REPLY6 carrying addr. */
static void deliverProbeReply6(const BoloAddr *addr) {
    uint8_t pkt[PACKET_HEADER_SIZE + PUNCH_ADDR_WIRE_SIZE];
    packHeader(pkt, PACKET_PUNCH_PROBE_REPLY6, 0);
    if (packPunchAddr(pkt + PACKET_HEADER_SIZE, addr) == PUNCH_ADDR_WIRE_SIZE) {
        punchDeliver(pkt, (int)sizeof(pkt));
    }
}

/* Delivers the old PACKET_PUNCH_PROBE_REPLY carrying an IPv4 addr. */
static void deliverProbeReply(const BoloAddr *addr) {
    uint8_t pkt[PACKET_HEADER_SIZE + 6];
    struct in_addr ip;
    packHeader(pkt, PACKET_PUNCH_PROBE_REPLY, 0);
    boloAddrV4(addr, &ip);
    memcpy(pkt + PACKET_HEADER_SIZE, &ip, 4);
    packU16(pkt + PACKET_HEADER_SIZE + 4, boloAddrPort(addr));
    punchDeliver(pkt, (int)sizeof(pkt));
}

int run_punch_probe_reply_status(void) {
    int rc = 0;
    ServerPortmapInfo pm;
    BoloAddr v6, v4;

    threadsCreate(true);
    UT_CHECK(boloAddrParse("2001:db8::7", PUNCH_TEST_PORT, &v6));
    UT_CHECK(boloAddrParse("203.0.113.5", PUNCH_TEST_PORT + 1, &v4));

    /* No IPv4 path: port mapping gave up and the IPv4 probe timed out. */
    serverInstanceTestSetPortmapState(true, NULL, 0, true, true);
    serverInstanceGetPortmapInfo(&pm);
    UT_CHECK_MSG(pm.status == SERVER_PORTMAP_FAILED, "status %d",
                 (int)pm.status);

    /* The IPv6 probe answers: reachable over IPv6 only. */
    deliverProbeReply6(&v6);
    serverInstanceGetPortmapInfo(&pm);
    UT_CHECK_MSG(pm.status == SERVER_PORTMAP_IPV6_OK, "status %d",
                 (int)pm.status);
    UT_CHECK_MSG(strcmp(pm.externalIp, "2001:db8::7") == 0, "ip '%s'",
                 pm.externalIp);
    UT_CHECK(pm.externalPort == PUNCH_TEST_PORT);

    /* Then the IPv4 probe answers too: the IPv4 outcome is what it was. */
    deliverProbeReply(&v4);
    serverInstanceGetPortmapInfo(&pm);
    UT_CHECK_MSG(pm.status == SERVER_PORTMAP_HOLE_PUNCH_OK, "status %d",
                 (int)pm.status);
    UT_CHECK_MSG(strcmp(pm.externalIp, "203.0.113.5") == 0, "ip '%s'",
                 pm.externalIp);
    UT_CHECK(pm.externalPort == PUNCH_TEST_PORT + 1);

    /* The old reply alone still records the IPv4 address. */
    serverInstanceTestSetPortmapState(true, NULL, 0, true, true);
    deliverProbeReply(&v4);
    serverInstanceGetPortmapInfo(&pm);
    UT_CHECK(pm.status == SERVER_PORTMAP_HOLE_PUNCH_OK);
    UT_CHECK(strcmp(pm.externalIp, "203.0.113.5") == 0);

    /* A family-4 body in PACKET_PUNCH_PROBE_REPLY6 is the IPv4 reply. */
    serverInstanceTestSetPortmapState(true, NULL, 0, true, true);
    deliverProbeReply6(&v4);
    serverInstanceGetPortmapInfo(&pm);
    UT_CHECK_MSG(pm.status == SERVER_PORTMAP_HOLE_PUNCH_OK, "status %d",
                 (int)pm.status);
    UT_CHECK(strcmp(pm.externalIp, "203.0.113.5") == 0);

    /* With a port mapping the IPv6 reply changes nothing. */
    serverInstanceTestSetPortmapState(true, "198.51.100.20", 27600, false,
                                      false);
    deliverProbeReply6(&v6);
    serverInstanceGetPortmapInfo(&pm);
    UT_CHECK_MSG(pm.status == SERVER_PORTMAP_SUCCEEDED, "status %d",
                 (int)pm.status);
    UT_CHECK(strcmp(pm.externalIp, "198.51.100.20") == 0);
    UT_CHECK(pm.externalPort == 27600);

    /* While the IPv4 probe can still answer, the status stays pending. */
    serverInstanceTestSetPortmapState(true, NULL, 0, true, false);
    deliverProbeReply6(&v6);
    serverInstanceGetPortmapInfo(&pm);
    UT_CHECK_MSG(pm.status == SERVER_PORTMAP_PENDING, "status %d",
                 (int)pm.status);

done:
    serverInstanceTestSetPortmapState(false, NULL, 0, false, false);
    return rc;
}
