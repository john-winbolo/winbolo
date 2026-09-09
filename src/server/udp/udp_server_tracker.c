/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Tracker
 *Filename:      udp_server_tracker.c
 *Author:        John Morrison
 *Purpose:
 *  The server transport's outward-facing address work,
 *  split out of transport_udp_server.c.
 *    - Reports the game to the tracker as an
 *      INFO_RESPONSE.
 *    - Keeps the server's NAT mapping toward the tracker
 *      open, and drives the hole-punch queue that opens
 *      one toward a joiner.
 *    - Holds the public-address override those advertise.
 *********************************************************/

#include <stdio.h>
#include <string.h>

#include "transport_udp_internal.h"        /* packHeader, packU32 */
#include "transport_udp_server_internal.h" /* udpServer, srvSendTo, buildInfoPacket,
                                            * PunchQueueEntry, INFO_PACKET,
                                            * serverSimGetTimeCreated, bolo_resolve_ipv4 */

/* Public-address override populated by transportUdpServerSetPublicAddress
 * once libplum negotiates a UPnP/NAT-PMP/PCP mapping.  When non-empty the
 * INFO_PACKET build sites advertise these instead of the internal port
 * and a zero address. */
char           udpServerPublicIp[64];
unsigned short udpServerPublicPort;

PunchQueueEntry punchQueue[PUNCH_QUEUE_SIZE];

/* Send an INFO_RESPONSE packet to the tracker so the game is listed. */
void transportUdpServerSendTrackerUpdate(ServerSim *sim,
                                         const char *trackerAddr,
                                         unsigned short trackerPort) {
    INFO_PACKET pkt;
    struct sockaddr_in dest;
    struct in_addr trackerIp;

    if (bolo_resolve_ipv4(trackerAddr, &trackerIp) != 0) {
        fprintf(stderr, "[TRACKER] Failed to resolve %s\n", trackerAddr);
        return;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr = trackerIp;
    dest.sin_port = htons(trackerPort);

    buildInfoPacket(sim, &pkt);

    srvSendTo((const uint8_t *)&pkt, sizeof(pkt), &dest);
}

void transportUdpServerSetPublicAddress(const char *externalIp,
                                        unsigned short externalPort) {
    if (externalPort == 0 || externalIp == NULL || externalIp[0] == '\0') {
        udpServerPublicIp[0] = '\0';
        udpServerPublicPort  = 0;
        return;
    }
    strncpy(udpServerPublicIp, externalIp, sizeof(udpServerPublicIp) - 1);
    udpServerPublicIp[sizeof(udpServerPublicIp) - 1] = '\0';
    udpServerPublicPort = externalPort;
}

void transportUdpServerSendNatKeepalive(ServerSim *sim,
                                        const char *trackerAddr,
                                        unsigned short trackerPort) {
    struct sockaddr_in dest;
    struct in_addr trackerIp;
    uint8_t buf[8];

    if (udpServer.sock == INVALID_SOCKET) return;
    if (trackerAddr == NULL || trackerAddr[0] == '\0') return;

    if (bolo_resolve_ipv4(trackerAddr, &trackerIp) != 0) return;

    buf[0] = 'W';
    buf[1] = 'B';
    buf[2] = 'K';
    buf[3] = 'A';
    /* 4-byte game token = serverSimGetTimeCreated(sim), big-endian. Tracker's exact-
     * match path uses (sourceIp, starttime) so multiple games behind one
     * NAT each refresh their own entry. */
    packU32(buf + 4, (uint32_t)serverSimGetTimeCreated(sim));

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr = trackerIp;
    dest.sin_port = htons(trackerPort);

    srvSendTo(buf, sizeof(buf), &dest);
}

void transportUdpServerSendPunchProbe(const char *trackerAddr,
                                      unsigned short trackerPort) {
    struct sockaddr_in dest;
    struct in_addr trackerIp;
    uint8_t buf[PACKET_HEADER_SIZE];

    if (udpServer.sock == INVALID_SOCKET) return;
    if (trackerAddr == NULL || trackerAddr[0] == '\0') return;

    if (bolo_resolve_ipv4(trackerAddr, &trackerIp) != 0) return;

    packHeader(buf, PACKET_PUNCH_PROBE_REQUEST, 0);

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr = trackerIp;
    dest.sin_port = htons(trackerPort);

    srvSendTo(buf, sizeof(buf), &dest);
}

void transportUdpServerDrainPunchQueue(void) {
    int i;
    if (udpServer.sock == INVALID_SOCKET) return;
    for (i = 0; i < PUNCH_QUEUE_SIZE; i++) {
        PunchQueueEntry *e = &punchQueue[i];
        uint8_t sentinel;
        if (e->packetsRemaining == 0) continue;
        if (e->ticksUntilNext > 0) {
            e->ticksUntilNext--;
            continue;
        }
        /* 1-byte sentinel — joiner's recv loop drops anything shorter
         * than PACKET_HEADER_SIZE (8 bytes), so this is harmless on
         * arrival; its only purpose is to open our outbound NAT
         * mapping toward the joiner. */
        sentinel = 'P';
        srvSendTo(&sentinel, 1, &e->addr);
        e->packetsRemaining--;
        e->ticksUntilNext = PUNCH_BURST_INTERVAL;
    }
}
