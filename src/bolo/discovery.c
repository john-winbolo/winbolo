/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Discovery
 *Filename:      discovery.c
 *Purpose:
 *  Game discovery: LAN UDP broadcast and mDNS scanning,
 *  plus per-server info ping. Extracted from netclient.c.
 *  Each function creates its own temporary sockets.
 *********************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "global.h"
#include "platform_net.h"
#ifndef _WIN32
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#else
#define strcasecmp _stricmp
#endif
#include "netpacks.h"
#include "crc.h"
#include "udppackets.h"
#include "util.h"
#include "discovery.h"
#include "../common/wb_log.h"

/* Used to stop socket blocking */
#define NO_BLOCK_SOCK 1

/* Default tracker port (TCP game finder) */
#ifndef TRACKER_PORT
#define TRACKER_PORT 50000
#endif

/* Default game port used for LAN broadcast discovery */
#define LAN_BROADCAST_PORT 27500

/* LAN broadcast search abort flag. Set by discoveryAbortBroadcastSearch
 * to break the discoveryFindBroadcastGamesAsync poll loop early; the
 * loop polls it every 50ms. Cleared at the top of each search so a
 * stale set from a prior session doesn't shortcut a fresh one. */
static SDL_AtomicInt s_broadcastAbort;

void discoveryAbortBroadcastSearch(void) {
  SDL_SetAtomicInt(&s_broadcastAbort, 1);
}

/* Translate an INFO_PACKET (wire format) into the public DiscoveryServer
 * POD. addr is the source address from recvfrom — used as a fallback
 * when the packet's gameid.serveraddress is unset. `len` is the number
 * of bytes actually received, so the view-policy byte is read only from
 * a full-length packet. */
static void discoveryFillServerFromInfoPacket(const INFO_PACKET *info, const struct in_addr *addr, DiscoveryServer *out, bool rich, size_t len) {
  memset(out, 0, sizeof(*out));
  /* The map name arrives as a pascal string in a MAP_STR_SIZE field of a
   * packet off the wire, so its length byte is whatever the sender wrote
   * there — bound the copy by the destination rather than trusting it. */
  {
    unsigned int nameLen = (unsigned char)info->mapname[0];
    if (nameLen > sizeof(out->mapName) - 1) {
      nameLen = sizeof(out->mapName) - 1;
    }
    if (nameLen > sizeof(info->mapname) - 1) {
      nameLen = sizeof(info->mapname) - 1;
    }
    memcpy(out->mapName, info->mapname + 1, nameLen);
    out->mapName[nameLen] = '\0';
  }
  out->password = (info->has_password != 0);
  out->mines = ((info->allow_mines & 0x80) != 0);

  if (info->gameid.serveraddress.s_addr == 0) {
    SDL_strlcpy(out->address, inet_ntoa(*addr), sizeof(out->address));
  } else {
    struct in_addr serverAddr = info->gameid.serveraddress;
    SDL_strlcpy(out->address, inet_ntoa(serverAddr), sizeof(out->address));
  }
  out->port = info->gameid.serverport;
  out->versionMajor = info->h.versionMajor;
  out->versionMinor = info->h.versionMinor;
  out->versionRevision = info->h.versionRevision;
  out->numPlayers = (BYTE)info->num_players;
  out->numBases = (BYTE)info->free_bases;
  out->numPills = (BYTE)info->free_pills;
  out->game = (gameType)info->gametype;
  out->ai = (aiType)info->allow_AI;
  out->timeLimit       = info->time_limit;

  /* The flags/count/md5 fields live past the legacy 76-byte prefix; read them
   * only when the full packet arrived, otherwise they hold garbage from a
   * short read. (out is memset above, so they stay at their zero/empty
   * defaults for legacy servers.) */
  if (rich) {
    out->numHumans = info->num_humans;
    out->numBots   = info->num_bots;
    out->maxPlayers = info->max_players;
    out->allowNewPlayers = (info->flags & INFO_FLAG_ALLOW_NEW_PLAYERS) != 0;
    out->locked          = (info->flags & INFO_FLAG_LOCKED) != 0;
    out->ranked          = (info->flags & INFO_FLAG_RANKED) != 0;
    out->randomMap       = (info->flags & INFO_FLAG_RANDOM_MAP) != 0;
    out->allowSpectators = (info->flags & INFO_FLAG_ALLOW_SPECTATORS) != 0;
    out->inLobby         = (info->flags & INFO_FLAG_IN_LOBBY) != 0;
    /* The voice mode is two bits of the same flags byte rather than a
     * flag; the memset above already left it serverVoiceOn for a legacy
     * server that never sent them. */
    out->voiceMode       = infoPacketReadVoiceMode(info->flags);
    out->spectatorCount  = info->spectator_count;
    /* map_md5 is 32 fixed-width hex chars with no NUL on the wire; a leading
     * '\0' means "no md5" (random/unknown map). Copy 32 and NUL-terminate. */
    if (info->map_md5[0] != '\0') {
      memcpy(out->mapMd5, info->map_md5, 32);
      out->mapMd5[32] = '\0';
    }
  }
  /* The view-policy byte sits past the rich block, so it has its own
   * length tier: a shorter packet reports the built-in defaults. */
  infoPacketReadViewPolicies(info, len,
                             &out->pillView, &out->baseView, &out->allyView,
                             &out->classicMode, &out->alliesInTrees);
  out->hasRichInfo = rich;
}

static void gameFinderProcessBroadcast(INFO_PACKET *info, struct in_addr *pack, DiscoveryServerCallback callback, void *userData, bool rich, size_t len) {
  DiscoveryServer server;
  discoveryFillServerFromInfoPacket(info, pack, &server, rich, len);
  callback(&server, userData);
}

bool discoveryFindBroadcastGamesAsync(DiscoveryServerCallback callback, void *userData) {
  bool returnValue;
  int ret;
  struct sockaddr_in con;
  struct sockaddr_in from;
  struct sockaddr_in addr;
  SOCKET sock;
  BYTE *ptr;
  int len = 0;
  char buff[MAX_UDPPACKET_SIZE] = INFOREQUESTHEADER;
  unsigned long noBlock;
  unsigned int timeOut;
  uint32_t tick;
  socklen_t fromlen;
  struct sockaddr_in last;
  socklen_t szlast;
  int sendOk;

  szlast = sizeof(last);

  sock = INVALID_SOCKET;
  returnValue = TRUE;
  ptr = (BYTE *)buff;

  /* Clear any pending abort from a prior session — without this, a
   * caller that aborted the previous search would cause this one to
   * exit the poll loop on its first iteration. */
  SDL_SetAtomicInt(&s_broadcastAbort, 0);

  ret = bolo_net_init();
  if (ret != 0) {
    WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to initialise network for broadcast");
    returnValue = FALSE;
  }

  if (returnValue == TRUE) {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
      returnValue = FALSE;
      WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to create UDP socket for broadcast");
    }
  }

  if (returnValue == TRUE) {
    addr.sin_family = AF_INET;
    addr.sin_port = INADDR_ANY;
    addr.sin_addr.s_addr = INADDR_ANY;
    ret = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret != 0) {
      returnValue = FALSE;
      WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to bind broadcast UDP socket");
    }
  }

  if (returnValue == TRUE) {
    noBlock = NO_BLOCK_SOCK;
    ret = ioctlsocket(sock, (long) FIONBIO, &noBlock);
    if (ret == SOCKET_ERROR) {
      returnValue = FALSE;
      WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Error setting broadcast socket options");
    }
  }
  if (returnValue == TRUE) {
    int optval = 1;
    ret = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&optval, sizeof(optval));
    if (ret != 0) {
      returnValue = FALSE;
      WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Error setting broadcast socket option");
    }
  }

  if (returnValue == TRUE) {
    sendOk = 0;
    memset(&con, 0, sizeof(con));
    con.sin_family = AF_INET;
    con.sin_port = htons(LAN_BROADCAST_PORT);
#ifdef _WIN32
    {
      INTERFACE_INFO InterfaceList[20];
      unsigned long nBytesReturned;
      if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, 0, 0, &InterfaceList,
                    sizeof(InterfaceList), &nBytesReturned, 0, 0) != SOCKET_ERROR) {
        int nNumInterfaces = nBytesReturned / sizeof(INTERFACE_INFO);
        for (int i = 0; i < nNumInterfaces; i++) {
          long nFlags = InterfaceList[i].iiFlags;
          if ((nFlags & IFF_UP) && (nFlags & IFF_BROADCAST) && !(nFlags & IFF_LOOPBACK)) {
            struct sockaddr_in bcast;
            memcpy(&bcast, &InterfaceList[i].iiBroadcastAddress, sizeof(bcast));
            bcast.sin_port = htons(LAN_BROADCAST_PORT);
            bcast.sin_family = AF_INET;
            WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Sending broadcast to %s:%d", inet_ntoa(bcast.sin_addr), LAN_BROADCAST_PORT);
            ret = sendto(sock, buff, BOLOPACKET_REQUEST_SIZE, 0, (struct sockaddr *)&bcast, sizeof(bcast));
            WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: sendto returned %d (expected %d)", ret, BOLOPACKET_REQUEST_SIZE);
            if (ret == BOLOPACKET_REQUEST_SIZE)
              sendOk = 1;
          }
        }
      }
    }
#else
    /* Use subnet-directed broadcasts via getifaddrs() for reliable delivery.
     * INADDR_BROADCAST (255.255.255.255) is often blocked on Linux. */
    {
      struct ifaddrs *ifap, *ifa;
      if (getifaddrs(&ifap) == 0) {
        for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
          if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
            continue;
          if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_BROADCAST) ||
              (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
          if (ifa->ifa_broadaddr == NULL)
            continue;
          {
            struct sockaddr_in bcast;
            memcpy(&bcast, ifa->ifa_broadaddr, sizeof(bcast));
            bcast.sin_port = htons(LAN_BROADCAST_PORT);
            bcast.sin_family = AF_INET;
            ret = sendto(sock, buff, BOLOPACKET_REQUEST_SIZE, 0,
                         (struct sockaddr *)&bcast, sizeof(bcast));
            if (ret == BOLOPACKET_REQUEST_SIZE)
              sendOk = 1;
          }
        }
        freeifaddrs(ifap);
      }
      /* Fallback to limited broadcast if no interfaces found */
      if (!sendOk) {
        con.sin_addr.s_addr = INADDR_BROADCAST;
        ret = sendto(sock, buff, BOLOPACKET_REQUEST_SIZE, 0,
                     (struct sockaddr *)&con, sizeof(con));
        if (ret == BOLOPACKET_REQUEST_SIZE)
          sendOk = 1;
      }
    }
#endif
    if (!sendOk) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "discovery: Broadcast send failed");
      returnValue = FALSE;
    }
  }

  SDL_Delay(50);
  if (returnValue == TRUE) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Waiting for broadcast responses...");
    szlast = sizeof(last);
    len = recvfrom(sock, (char *) (ptr+len), (int) (sizeof(buff)-len), 0, (struct sockaddr *) &last, &szlast);
    timeOut = 0;
    tick = (uint32_t)SDL_GetTicks();

    while (timeOut <= 5000) {
      if (SDL_GetAtomicInt(&s_broadcastAbort)) {
        WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Broadcast scan aborted by caller");
        break;
      }
      if (len > 0) {
        WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Received %d bytes from %s:%u (expect %d for INFO_PACKET)",
                len, inet_ntoa(last.sin_addr), ntohs(last.sin_port), (int)sizeof(INFO_PACKET));
      }
      if (len == (int)INFO_PACKET_LEGACY_SIZE ||
          len == (int)INFO_PACKET_PRE_VIEWS_SIZE ||
          len == (int) sizeof(INFO_PACKET)) {
        /* Magic + type only — the INFO_RESPONSE is the universal
         * version-negotiation primitive, so we deliver mixed-version
         * servers up to the UI; the caller pre-flights versions before
         * attempting a join. Legacy 76-byte servers parse the common
         * prefix only (rich fields gated off), and a packet that stops
         * before the view-policy byte gets the view defaults. */
        if (strncmp(buff, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE) == 0 && buff[BOLOPACKET_REQUEST_TYPEPOS] == BOLOPACKET_INFORESPONSE) {
          bool rich = (len >= (int)INFO_PACKET_PRE_VIEWS_SIZE);
          WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Valid INFO_PACKET response, adding server");
          gameFinderProcessBroadcast((INFO_PACKET *) buff, &(last.sin_addr), callback, userData, rich, (size_t)len);
        } else {
          WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Packet signature/type mismatch");
        }
      }
      SDL_Delay(50);
      timeOut = (uint32_t)SDL_GetTicks() - tick;
      fromlen = sizeof(from);
      len = recvfrom(sock, buff, MAX_UDPPACKET_SIZE, 0, (struct sockaddr *)&from, &fromlen);
      if (len > 0) {
        memcpy(&last, &from, sizeof(from));
      }
    }
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Broadcast scan complete (timeout after 5s)");
  }

  if (sock != INVALID_SOCKET) {
    shutdown(sock, SD_BOTH);
    closesocket(sock);
  }
  bolo_net_cleanup();
  return returnValue;
}

bool discoveryPingServer(const char *address, unsigned short port, DiscoveryPingResult *out) {
  struct sockaddr_in dest;
  struct sockaddr_in from;
  socklen_t fromlen;
  SOCKET sock;
  BYTE buff[MAX_UDPPACKET_SIZE] = INFOREQUESTHEADER;
  uint32_t sendTime;
  int sent;
  int len;

  if (out == NULL) {
    return FALSE;
  }
  out->rttMs = -2;
  out->freePills = 0;
  out->freeBases = 0;
  out->numPlayers = 0;
  out->versionMajor = 0;
  out->versionMinor = 0;
  out->versionRevision = 0;
  /* Default the rich fields here: callers pass an uninitialized result, and a
   * legacy 76-byte server leaves them gated off below — without this they'd
   * carry stack garbage. */
  out->mapMd5[0] = '\0';
  out->allowNewPlayers = false;
  out->locked = false;
  out->inLobby = false;
  out->allowSpectators = false;
  out->spectatorCount = 0;
  out->ranked = false;
  out->randomMap = false;
  out->numHumans = 0;
  out->numBots = 0;
  out->maxPlayers = 0;
  out->timeLimit = 0;
  out->hasRichInfo = false;

  if (bolo_net_init() != 0) {
    return FALSE;
  }

  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  dest.sin_addr.s_addr = inet_addr(address);
  if (dest.sin_addr.s_addr == INADDR_NONE) {
    if (bolo_resolve_ipv4(address, &dest.sin_addr) != 0) {
      WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Ping DNS lookup failed for %s", address);
      bolo_net_cleanup();
      return FALSE;
    }
  }

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    bolo_net_cleanup();
    return FALSE;
  }

#ifdef _WIN32
  {
    DWORD tv = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  }
#else
  {
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  }
#endif

  sendTime = (uint32_t)SDL_GetTicks();
  sent = sendto(sock, (const char *)buff, BOLOPACKET_REQUEST_SIZE, 0,
                (struct sockaddr *)&dest, sizeof(dest));
  WB_LOG_TRACE(WB_LOG_CAT_NET, "ping: sent %d bytes to %s:%u (expected %d)",
               sent, address, port, BOLOPACKET_REQUEST_SIZE);
  if (sent != BOLOPACKET_REQUEST_SIZE) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "ping: sendto failed for %s:%u", address, port);
    closesocket(sock);
    bolo_net_cleanup();
    return FALSE;
  }

  fromlen = sizeof(from);
  len = recvfrom(sock, (char *)buff, MAX_UDPPACKET_SIZE, 0,
                 (struct sockaddr *)&from, &fromlen);
  closesocket(sock);
  bolo_net_cleanup();

  if (len < (int)INFO_PACKET_LEGACY_SIZE) {
#ifdef _WIN32
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "ping: %s:%u no response (len=%d, err=%d)", address, port, len, WSAGetLastError());
#else
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "ping: %s:%u no response (len=%d, errno=%d)", address, port, len, errno);
#endif
    return FALSE;
  }

  {
    uint32_t recvTime = (uint32_t)SDL_GetTicks();
    INFO_PACKET *info = (INFO_PACKET *)buff;
    /* Legacy 76-byte servers don't carry the flags/count/md5 fields; read
     * them only once the rich block has arrived. */
    bool rich = (len >= (int)INFO_PACKET_PRE_VIEWS_SIZE);
    out->rttMs = (int)(recvTime - sendTime);
    out->freePills = info->free_pills;
    out->freeBases = info->free_bases;
    out->numPlayers = info->num_players;
    out->versionMajor = info->h.versionMajor;
    out->versionMinor = info->h.versionMinor;
    out->versionRevision = info->h.versionRevision;
    out->timeLimit       = info->time_limit;
    /* map_md5 is 32 fixed-width hex chars with no NUL on the wire; a leading
     * '\0' means "no md5" (random/unknown map). This path does not zero out,
     * so NUL-init before the conditional copy. */
    out->mapMd5[0] = '\0';
    /* Same reason as mapMd5 above: this path does not zero out, so set the
     * mode a server that sent no voice bits runs before reading them. */
    out->voiceMode = serverVoiceOn;
    if (rich) {
      out->numHumans = info->num_humans;
      out->numBots   = info->num_bots;
      out->maxPlayers = info->max_players;
      out->allowNewPlayers = (info->flags & INFO_FLAG_ALLOW_NEW_PLAYERS) != 0;
      out->locked          = (info->flags & INFO_FLAG_LOCKED) != 0;
      out->ranked          = (info->flags & INFO_FLAG_RANKED) != 0;
      out->randomMap       = (info->flags & INFO_FLAG_RANDOM_MAP) != 0;
      out->allowSpectators = (info->flags & INFO_FLAG_ALLOW_SPECTATORS) != 0;
      out->inLobby         = (info->flags & INFO_FLAG_IN_LOBBY) != 0;
      out->voiceMode       = infoPacketReadVoiceMode(info->flags);
      out->spectatorCount  = info->spectator_count;
      if (info->map_md5[0] != '\0') {
        memcpy(out->mapMd5, info->map_md5, 32);
        out->mapMd5[32] = '\0';
      }
    }
    /* Own length tier — see discoveryFillServerFromInfoPacket. */
    infoPacketReadViewPolicies(info, (size_t)len,
                               &out->pillView, &out->baseView, &out->allyView,
                               &out->classicMode, &out->alliesInTrees);
    out->hasRichInfo = rich;
    WB_LOG_TRACE(WB_LOG_CAT_NET, "ping: %s:%u responded in %dms, v%u.%u.%u, players=%u",
                 address, port, out->rttMs,
                 (unsigned)out->versionMajor, (unsigned)out->versionMinor,
                 (unsigned)out->versionRevision, (unsigned)out->numPlayers);
  }
  return TRUE;
}
