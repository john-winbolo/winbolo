/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *  Game discovery: tracker TCP queries and LAN UDP
 *  broadcast scanning. Extracted from netclient.c.
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
#include "../gui/currentgames.h"
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

/*---------------------------------------------------------
 * Game finder helper functions (platform-independent)
 *---------------------------------------------------------*/

static int gameFinderGetParamFromLine(char *src, char *dest, char *pattern) {
  int returnValue;
  char *ptr;
  int count;

  dest[0] = '\0';
  count = 0;
  returnValue = -1;
  if (strncmp(src, pattern, strlen(pattern)) == 0) {
    returnValue = (int) (strlen(pattern) + 1);
    ptr = src + returnValue;
    while (*ptr != '\n' && *ptr != '\r') {
      count++;
      ptr++;
    }
    strncat(dest, (char *) (src+returnValue), (size_t) count);
  }

  return returnValue;
}


static int gameFinderReplace(char *buff, int len) {
  int returnValue = -1;
  char *ptr = buff;
  while (strncmp(ptr, "\r\n", 2) != 0  && returnValue < len) {
    returnValue++;
    ptr++;
  }
  if (returnValue >= len) {
    returnValue = -1;
  } else {
    *ptr = '\0';
  }
  return returnValue+1;
}

static char * gameFinderProcessMotd(char *buff, char *motd, int numLines) {
  int count;
  char *ptr;
  char param[256];

  ptr = buff;
  motd[0] = '\0';
  count = 0;
  while (count < numLines) {
    gameFinderGetParamFromLine(ptr, param, (char *) "MOTD");
    strcat(motd, param);
    strcat(motd, "\n");
    count++;
    ptr += gameFinderReplace(ptr, 20000);
    ptr++;
    ptr++;
  }
  return ptr;
}

static unsigned short gameFrontGameLocation(char *argItem, char *trackerAddr) {
  char *tmp;
  tmp = strtok(argItem, ":");
  if (tmp != NULL) {
    strcpy(trackerAddr, tmp);
    tmp = strtok(NULL, ":");
    if (tmp != NULL) {
      return (unsigned short) atoi(tmp);
    }
  }
  return 0;
}


static bool gameFinderYesNoToBool(char *str) {
  bool returnValue;
  returnValue = FALSE;
  if (str[0] == 'Y' || str[0] == 'y') {
    returnValue = TRUE;
  }
  return returnValue;
}

static void gameFinderProcessV1(currentGames *cg, char *buff, int len, char *motd) {
  (void)len;
  char param[256];
  char *ptr;
  int numMotdLines = 0;
  int count = 0;
  int numGames;
  char gameNum[100];
  char gameId[100];
  char address[255];
  unsigned short port;
  char mapName[255];
  char version[255];
  gameType game;
  BYTE players;
  BYTE basesNum;
  BYTE pillsNum;
  bool password;
  bool hiddenMines;
  aiType ai;

  numGames = 0;
  ptr = buff;
  if (gameFinderGetParamFromLine(buff, param, (char *) "MOTDL") != -1) {
    numMotdLines = atoi(param);
    ptr += gameFinderReplace(ptr, 20000);
    ptr++;
    ptr++;
    ptr = gameFinderProcessMotd(ptr, motd, numMotdLines);
  }

  if (gameFinderGetParamFromLine(ptr, param, (char *) "NGAMES") != -1) {
    numGames = atoi(param);
  } else {
    numGames = 0;
  }
  if (numGames > 0) {
    ptr += gameFinderReplace(ptr, 20000);
    ptr++;
    ptr++;
    while (count < numGames) {
      sprintf(gameNum, "%.03d", count);
      strcpy(gameId, "GAME");
      strcat(gameId, gameNum);
      gameFinderGetParamFromLine(ptr, param, gameId);
      port = gameFrontGameLocation(param, address);
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      gameFinderGetParamFromLine(ptr, version, (char *) "VERSION");
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      gameFinderGetParamFromLine(ptr, mapName, (char *) "MAP");
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      gameFinderGetParamFromLine(ptr, param, (char *) "TYPE");
      if (param[0] == 'o' || param[0] == 'O') {
        game = gameOpen;
      } else if (param[0] == 't' || param[0] == 'T') {
        game = gameTournament;
      } else {
        game = gameStrictTournament;
      }
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      gameFinderGetParamFromLine(ptr, param, (char *) "PLAYERS");
      players = (BYTE) atoi(param);
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      gameFinderGetParamFromLine(ptr, param, (char *) "BASES");
      basesNum = (BYTE) atoi(param);
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      gameFinderGetParamFromLine(ptr, param, (char *) "PILLS");
      pillsNum = (BYTE) atoi(param);
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;

      gameFinderGetParamFromLine(ptr, param, (char *) "HIDMINES");
      hiddenMines = gameFinderYesNoToBool(param);
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      gameFinderGetParamFromLine(ptr, param, (char *) "PASSWORD");
      password = gameFinderYesNoToBool(param);
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;

      gameFinderGetParamFromLine(ptr, param, (char *) "BRAINS");
      if (param[0] == 'n' || param[0] == 'N') {
        ai = aiNone;
      } else if (strcasecmp(param, "yesFull") == 0) {
        ai = aiFull;
      } else if (strlen(param) > 3) {
        ai = aiYesAdvantage;
      } else {
        ai = aiYes;
      }
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;

      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;
      ptr += gameFinderReplace(ptr, 20000);
      ptr++;
      ptr++;

      currentGamesAddItem(cg, address, port, mapName, version, players, basesNum, pillsNum, hiddenMines, game, ai, password);
      count++;
    }
  }
}


static void gameFinderProcess(currentGames *cg, char *buff, int len, char *motd) {
  (void)len;
  char *ptr;
  char *ptr2;
  char param[256];
  int amount;
  int version;
  ptr2 = ptr = buff;
  amount = gameFinderGetParamFromLine(buff, param, (char *) "TVERSION");
  ptr2 += amount;

  amount = gameFinderReplace(ptr, 20000);
  ptr += amount;
  ptr++;
  ptr++;

  version = atoi(ptr2);
  switch (version) {
    case 1:
      gameFinderProcessV1(cg, ptr, 20000, motd);
      break;
    default:
      WB_LOG_WARN(WB_LOG_CAT_NET, "discovery: Unsupported tracker version %d", version);
  }
}


/* Translate an INFO_PACKET (wire format) into the public DiscoveryServer
 * POD. addr is the source address from recvfrom — used as a fallback
 * when the packet's gameid.serveraddress is unset. */
static void discoveryFillServerFromInfoPacket(const INFO_PACKET *info, const struct in_addr *addr, DiscoveryServer *out) {
  memset(out, 0, sizeof(*out));
  utilPtoCString((char *)info->mapname, out->mapName);
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
}

static void gameFinderProcessBroadcast(INFO_PACKET *info, struct in_addr *pack, DiscoveryServerCallback callback, void *userData) {
  DiscoveryServer server;
  discoveryFillServerFromInfoPacket(info, pack, &server);
  callback(&server, userData);
}


bool discoveryFindTrackedGames(currentGames *cg, char *trackerAddress, unsigned short port, char *motd) {
  bool returnValue;
  int ret;
  struct sockaddr_in con;
  SOCKET sock;
  struct hostent *phe;
  BYTE *buff;
  BYTE *ptr;
  int len = 0;
  int new_bytes_read;
  unsigned long noBlock;
  bool done;
  uint32_t tick, timeOut;

  buff = malloc(1024 * 1024);
  if (buff == NULL) {
    return FALSE;
  }
  sock = INVALID_SOCKET;
  returnValue = TRUE;
  ptr = buff;

  ret = bolo_net_init();
  if (ret != 0) {
    WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to initialise network for tracker");
    returnValue = FALSE;
  }

  if (returnValue == TRUE) {
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
      returnValue = FALSE;
      WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to create TCP socket for tracker");
    }
  }

  if (returnValue == TRUE) {
    con.sin_family = AF_INET;
    con.sin_port = htons(port);
    con.sin_addr.s_addr = inet_addr(trackerAddress);
    if (con.sin_addr.s_addr == INADDR_NONE) {
      phe = gethostbyname(trackerAddress);
      if (phe == 0) {
        returnValue = FALSE;
        WB_LOG_WARN(WB_LOG_CAT_NET, "discovery: Tracker DNS lookup failed");
      } else {
        con.sin_addr.s_addr = *((uint32_t*)phe->h_addr_list[0]);
      }
    }
  }

  if (returnValue == TRUE) {
    ret = connect(sock, (struct sockaddr *) &con, sizeof(con));
    if (ret == SOCKET_ERROR) {
      WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to connect to tracker");
      returnValue = FALSE;
    }
  }

  noBlock = NO_BLOCK_SOCK;
  if (returnValue == TRUE) {
    ret = ioctlsocket(sock, (long) FIONBIO, &noBlock);
    if (ret == SOCKET_ERROR) {
      WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Error setting socket to non blocking mode");
      returnValue = FALSE;
    }
  }

  if (returnValue == TRUE) {
    len = 0;
    ptr = buff;
    new_bytes_read = recv(sock, (char *) (ptr+len), (int) ((1024 * 1024)-len), 0);
    tick = (uint32_t)SDL_GetTicks();
    timeOut = 0;
    done = FALSE;
    while (timeOut <= 10000 && done == FALSE) {
      if (new_bytes_read > 0) {
        len += new_bytes_read;
      } else if (new_bytes_read == SOCKET_ERROR) {
#ifdef _WIN32
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
          done = TRUE;
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          done = TRUE;
        }
#endif
      } else if (new_bytes_read == 0) {
        done = TRUE;
      }
      timeOut = (uint32_t)SDL_GetTicks() - tick;
      new_bytes_read = recv(sock, (char *) (ptr+len), (int) ((1024 * 1024)-len), 0);
    }

    if (len == 0) {
      WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: No data received from tracker");
      returnValue = FALSE;
    } else {
      gameFinderProcess(cg, (char *) buff, len, motd);
    }
  }

  free(buff);
  if (sock != INVALID_SOCKET) {
    shutdown(sock, SD_BOTH);
    closesocket(sock);
  }
  bolo_net_cleanup();
  return returnValue;
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
      if (len == (int) sizeof(INFO_PACKET)) {
        /* Magic + type only — the INFO_RESPONSE is the universal
         * version-negotiation primitive, so we deliver mixed-version
         * servers up to the UI; the caller pre-flights versions before
         * attempting a join. */
        if (strncmp(buff, BOLO_SIGNITURE, BOLO_SIGNITURE_SIZE) == 0 && buff[BOLOPACKET_REQUEST_TYPEPOS] == BOLOPACKET_INFORESPONSE) {
          WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Valid INFO_PACKET response, adding server");
          gameFinderProcessBroadcast((INFO_PACKET *) buff, &(last.sin_addr), callback, userData);
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

  if (bolo_net_init() != 0) {
    return FALSE;
  }

  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  dest.sin_addr.s_addr = inet_addr(address);
  if (dest.sin_addr.s_addr == INADDR_NONE) {
    struct hostent *phe = gethostbyname(address);
    if (phe == NULL) {
      WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: Ping DNS lookup failed for %s", address);
      bolo_net_cleanup();
      return FALSE;
    }
    dest.sin_addr.s_addr = *((uint32_t *)phe->h_addr_list[0]);
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

  if (len < (int)sizeof(INFO_PACKET)) {
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
    out->rttMs = (int)(recvTime - sendTime);
    out->freePills = info->free_pills;
    out->freeBases = info->free_bases;
    out->numPlayers = info->num_players;
    out->versionMajor = info->h.versionMajor;
    out->versionMinor = info->h.versionMinor;
    out->versionRevision = info->h.versionRevision;
    WB_LOG_TRACE(WB_LOG_CAT_NET, "ping: %s:%u responded in %dms, v%u.%u.%u, players=%u",
                 address, port, out->rttMs,
                 (unsigned)out->versionMajor, (unsigned)out->versionMinor,
                 (unsigned)out->versionRevision, (unsigned)out->numPlayers);
  }
  return TRUE;
}
