/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Query
 *Filename:      udp_server_query.c
 *Author:        John Morrison
 *Purpose:
 *  The server-info query protocol, split out of
 *  transport_udp_server.c.
 *    - Filling the INFO packet from the current sim state,
 *      shared by the query reply and the tracker update.
 *    - Answering an info request, and recognising the
 *      old-protocol form of it.
 *    - The terrain-name helper the map-resync diagnostics
 *      print.
 *********************************************************/

#include <stdio.h>   /* snprintf */
#include <string.h>  /* memcmp, memcpy, memset */

#include "transport_udp_server_internal.h" /* srvSendTo, udpServerPublicIp,
                                            * udpServerPublicPort, and via
                                            * platform_net.h the in_addr /
                                            * inet_addr / inet_ntoa / htonl the
                                            * game id and the reply log use */
#include "transport_udp.h"   /* transportUdpServerGetLock */
#include "netpacks.h"        /* INFO_PACKET, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE,
                              * BOLOPACKET_INFOREQUEST, BOLOPACKET_INFORESPONSE,
                              * BOLOPACKET_REQUEST_SIZE, BOLOPACKET_REQUEST_TYPEPOS,
                              * HIDDEN_MINES, ALL_MINES_VISIBLE, the INFO_FLAG_*
                              * bits, infoPacketPackVoiceMode,
                              * infoPacketPackViewPolicies,
                              * infoPacketPackViewPolicies2 */
#include "global.h"          /* BYTE, MAX_TANKS and the terrain ids the
                              * resync name table covers */
#include "util.h"            /* utilCtoPString */
#include "pillbox.h"         /* pillsGetNumNeutral */
#include "bases.h"           /* basesGetNumNeutral */
#include "game_sim.h"        /* GameSim — gs->game, gs->hiddenMines, gs->pb, gs->bs */
#include "server_sim.h"      /* ServerSim, ServerState, serverStateLobby, and the
                              * advertised settings' accessors, including
                              * serverSimGetViewPolicy / viewCategory* via
                              * view_policy.h and serverSimGetVoiceMode via
                              * server_voice_mode.h */
#include "server_sim_lifecycle.h" /* serverSimGetPassword */

/* Human-readable terrain name for map-resync diagnostics. Covers the terrain
 * byte stored in mapItem, including the mine range (10-15). */
const char *resyncTerrainName(BYTE t) {
    switch (t) {
        case DEEP_SEA:     return "DEEP_SEA";
        case BUILDING:     return "BUILDING";
        case RIVER:        return "RIVER";
        case SWAMP:        return "SWAMP";
        case CRATER:       return "CRATER";
        case ROAD:         return "ROAD";
        case FOREST:       return "FOREST";
        case RUBBLE:       return "RUBBLE";
        case GRASS:        return "GRASS";
        case HALFBUILDING: return "HALFBUILDING";
        case BOAT:         return "BOAT";
        case MINE_SWAMP:   return "MINE_SWAMP";
        case MINE_CRATER:  return "MINE_CRATER";
        case MINE_ROAD:    return "MINE_ROAD";
        case MINE_FOREST:  return "MINE_FOREST";
        case MINE_RUBBLE:  return "MINE_RUBBLE";
        case MINE_GRASS:   return "MINE_GRASS";
        default:           return "?";
    }
}

/* Fill an INFO_PACKET from the current sim state. The info-request reply
 * and the tracker update both advertise the same server, so they share
 * this builder and differ only in where the finished packet is sent.
 * Layout and field semantics are documented in docs/info_packet_wire.md. */
void buildInfoPacket(ServerSim *sim, INFO_PACKET *pkt) {
    GameSim *gs = serverSimGetGameSim(sim);
    int i;
    BYTE numPlayers = 0, numHumans = 0, numBots = 0;

    memset(pkt, 0, sizeof(*pkt));

    /* Header */
    memcpy(pkt->h.signature, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE);
    pkt->h.versionMajor = BOLO_VERSION_MAJOR;
    pkt->h.versionMinor = BOLO_VERSION_MINOR;
    pkt->h.versionRevision = BOLO_VERSION_REVISION;
    pkt->h.type = BOLOPACKET_INFORESPONSE;

    /* Game ID — address zeroed (browser uses UDP source), port and timestamp set.
     * Tracker reads port raw for v1.1.8 (only ntohs for v1.1.1-3).
     * start_time is the only field the tracker byte-swaps on read. */
    if (udpServerPublicPort != 0) {
        pkt->gameid.serveraddress.s_addr = inet_addr(udpServerPublicIp);
        pkt->gameid.serverport = udpServerPublicPort;
    } else {
        pkt->gameid.serveraddress.s_addr = 0;
        pkt->gameid.serverport = serverSimGetServerPort(sim);
    }
    pkt->gameid.start_time = htonl(serverSimGetTimeCreated(sim));

    /* Map name as Pascal string */
    utilCtoPString((char *)serverSimGetMapName(sim), pkt->mapname);

    /* Game settings */
    pkt->gametype = (BYTE)gs->game;
    pkt->allow_mines = gs->hiddenMines ? HIDDEN_MINES : ALL_MINES_VISIBLE;
    pkt->allow_AI = 0;  /* AI type not tracked in new sim — report as none */
    {
        BYTE flags = 0;
        if (serverSimIsAcceptingJoins(sim))              flags |= INFO_FLAG_ALLOW_NEW_PLAYERS;
        if (transportUdpServerGetLock() || !serverSimIsAcceptingJoins(sim)) flags |= INFO_FLAG_LOCKED;
        if (serverSimGetRanked(sim))                     flags |= INFO_FLAG_RANKED;
        if (serverSimIsRandomMapEnabled(sim))            flags |= INFO_FLAG_RANDOM_MAP;
        if (serverSimGetState(sim) == serverStateLobby)  flags |= INFO_FLAG_IN_LOBBY;
        /* Advertise spectator support so finders can enable a Spectate action;
         * the cap accessor returns 0 when spectating is disabled. The live
         * spectator_count has no accessor yet, so it stays 0 below. */
        if (serverSimGetMaxSpectators(sim) > 0)          flags |= INFO_FLAG_ALLOW_SPECTATORS;
        /* The voice mode is two bits rather than a flag, in the top of the
         * same byte. serverVoiceOn packs as zero. */
        flags |= infoPacketPackVoiceMode(serverSimGetVoiceMode(sim));
        pkt->flags = flags;
    }
    pkt->start_delay = serverSimGetStartDelay(sim);
    pkt->time_limit = serverSimGetGameLength(sim);

    /* Count connected players, classifying humans vs bots */
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(sim, i)) {
            numPlayers++;
            if (serverSimIsBot(sim, (BYTE)i)) numBots++;
            else                              numHumans++;
        }
    }
    pkt->num_players = numPlayers;
    pkt->num_humans  = numHumans;
    pkt->num_bots    = numBots;
    pkt->max_players = serverSimGetMaxPlayers(sim);

    /* Neutral pills and bases */
    pkt->free_pills = pillsGetNumNeutral(&gs->pb);
    pkt->free_bases = basesGetNumNeutral(&gs->bs);

    pkt->has_password = serverSimGetPassword(sim)[0] != '\0' ? 1 : 0;
    pkt->spectator_count = 0;

    {
        const char *md5Hex = serverSimGetMapMd5Hex(sim);
        if (md5Hex[0] != '\0' && !serverSimIsRandomMapEnabled(sim)) {
            memcpy(pkt->map_md5, md5Hex, 32);
        }
    }

    pkt->view_policies = infoPacketPackViewPolicies(
        serverSimGetViewPolicy(sim, viewCategoryPill),
        serverSimGetViewPolicy(sim, viewCategoryBase),
        serverSimGetViewPolicy(sim, viewCategoryAlly),
        serverSimGetClassicMode(sim),
        serverSimGetAlliesInTrees(sim));
    pkt->view_policies2 = infoPacketPackViewPolicies2(
        serverSimGetOverviewWindow(sim),
        serverSimGetLineOfSight(sim),
        serverSimGetPositionalSound(sim));
}

/* Handle an old-protocol info request (server browser compatibility).
 * Replies to the requester with the current server advertisement. */
void serverHandleInfoRequest(const struct sockaddr_in *fromAddr,
                             ServerSim *sim) {
    INFO_PACKET pkt;
    char consoleMsg[256];

    buildInfoPacket(sim, &pkt);

    /* wire-only: tracker / external reply (no in-process audience) */
    srvSendTo((uint8_t *)&pkt, sizeof(pkt), fromAddr);

    {
        struct in_addr addrCopy = fromAddr->sin_addr;
        snprintf(consoleMsg, sizeof(consoleMsg), "Info packet request from %s",
                 inet_ntoa(addrCopy));
    }
    serverSimConsoleMessage(consoleMsg);
}

/* Check if a packet is an old-protocol info request.
 * Gate is magic + length + type only — the info-request is the universal
 * version-negotiation primitive, so a v1.0 client asking a v2.0 server
 * (or vice versa) must receive an INFO_RESPONSE carrying the server's
 * own version triple.  Mismatched-version joiners then see a localized
 * pre-flight error rather than a silent JOIN_REQUEST length-gate drop.
 * The version bytes inside the request body are still parsed elsewhere
 * for logging but no longer gate the response. */
bool isOldProtocolInfoRequest(const uint8_t *buf, int len) {
    return len == BOLOPACKET_REQUEST_SIZE &&
           memcmp(buf, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE) == 0 &&
           buf[BOLOPACKET_REQUEST_TYPEPOS] == BOLOPACKET_INFOREQUEST;
}
