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
 *Name:          mDNS Advertiser
 *Filename:      mdns_advertise.c
 *Purpose:
 *  Inline mDNS service advertiser for _winbolo._udp.local.
 *  A host opens one IPv4 multicast socket at startup and
 *  answers discovery queries straight from the server tick
 *  (no thread of its own), then closes the socket at
 *  shutdown. The answer record set is built by the socket-free
 *  mdnsAdvertiseBuildRecords (defined below), which Pass B's
 *  test drives directly.
 *
 *  This is the only TU that pulls in the all-static-inline
 *  mdns.h, so the build scopes -Wno-unused-function / /wd4505
 *  to it (see src/server/CMakeLists.txt).
 *********************************************************/

#include "mdns_advertise.h"

#ifdef __EMSCRIPTEN__

/* mDNS LAN advertising relies on multicast sockets and getifaddrs, which
 * the Emscripten runtime does not provide. The hooks compile to no-ops so
 * the WASM server build links without pulling in mdns.h at all. */
void transportUdpServerStartMdnsAdvertiser(unsigned short gamePort) { (void)gamePort; }
void transportUdpServerPollMdnsAdvertiser(struct ServerSim *sim) { (void)sim; }
void transportUdpServerStopMdnsAdvertiser(void) {}

#else /* !__EMSCRIPTEN__ */

#include <stdio.h>
#include <string.h>

#include "platform_net.h"   /* cross-platform sockets + inet_ntoa */
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "game_sim.h"
#include "bases.h"
#include "pillbox.h"
#include "transport_udp.h"  /* transportUdpServerGetLock */
#include "mdns_records.h"   /* MdnsServerInfo + builder (pulls mdns.h) */
#include "../common/wb_log.h"

/* Single advertiser per process (one server instance per process). sock < 0
 * means "not running" — every hook short-circuits on that, so the poll hook
 * is a cheap no-op when the advertiser was never started or failed to open. */
static struct {
  int            sock;
  unsigned short gamePort;
  struct in_addr localAddr;  /* server LAN address sent in the A record */
} s_mdns = { -1, 0, { 0 } };

/* Context threaded through mdns_socket_listen to the query callback: the
 * socket to answer on plus a snapshot of current server state. */
typedef struct {
  int                   sock;
  const MdnsServerInfo *info;
} MdnsAnswerCtx;

/*---------------------------------------------------------
 * Local LAN address discovery (single interface, v1).
 * Mirrors discovery.c's broadcast-interface selection.
 * Multi-interface advertising (one socket per usable iface)
 * is a follow-up.
 *---------------------------------------------------------*/
static bool mdnsFindLocalAddr(struct in_addr *out) {
#ifdef _WIN32
  SOCKET s;
  INTERFACE_INFO list[20];
  unsigned long bytesReturned;
  bool found = FALSE;

  s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    return FALSE;
  }
  if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, 0, 0, &list, sizeof(list),
               &bytesReturned, 0, 0) != SOCKET_ERROR) {
    int n = (int)(bytesReturned / sizeof(INTERFACE_INFO));
    int i;
    for (i = 0; i < n; i++) {
      long flags = list[i].iiFlags;
      if ((flags & IFF_UP) && !(flags & IFF_LOOPBACK)) {
        struct sockaddr_in *a = (struct sockaddr_in *)&list[i].iiAddress;
        *out = a->sin_addr;
        found = TRUE;
        break;
      }
    }
  }
  closesocket(s);
  return found;
#else
  struct ifaddrs *ifap, *ifa;
  bool found = FALSE;

  if (getifaddrs(&ifap) != 0) {
    return FALSE;
  }
  for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    *out = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
    found = TRUE;
    break;
  }
  freeifaddrs(ifap);
  return found;
#endif
}

/*---------------------------------------------------------
 * Record builder — socket-free, the unit Pass B's test
 * drives directly. Reads only the MdnsServerInfo POD.
 *---------------------------------------------------------*/

/* Service-instance and host names. The instance label is fixed for v1
 * (browsers read the data from SRV/A/TXT, not the label); the host name
 * is the SRV target and the A record name. Both carry the trailing dot
 * that is part of the wire name. */
static const char kInstanceName[] = "WinBolo." MDNS_WINBOLO_SERVICE;
static const char kHostName[]     = "winbolo.local.";

/* mDNS record TTL in seconds. */
#define MDNS_WINBOLO_TTL 60

size_t mdnsAdvertiseBuildRecords(const MdnsServerInfo *info,
                                 mdns_record_t *records, size_t capacity,
                                 char *txtScratch, size_t txtScratchSize) {
  size_t n = 0;
  size_t cursor = 0;

  if (info == NULL || records == NULL || txtScratch == NULL ||
      capacity < MDNS_WINBOLO_RECORD_COUNT) {
    return 0;
  }

  /* records[0]: PTR _winbolo._udp.local. -> instance (the shared
   * service-enumeration answer). */
  memset(&records[n], 0, sizeof(records[n]));
  records[n].name.str    = MDNS_WINBOLO_SERVICE;
  records[n].name.length = sizeof(MDNS_WINBOLO_SERVICE) - 1;
  records[n].type        = MDNS_RECORDTYPE_PTR;
  records[n].data.ptr.name.str    = kInstanceName;
  records[n].data.ptr.name.length = sizeof(kInstanceName) - 1;
  records[n].rclass      = MDNS_CLASS_IN;
  records[n].ttl         = MDNS_WINBOLO_TTL;
  n++;

  /* records[1]: SRV instance -> host:port (the real bound game port). */
  memset(&records[n], 0, sizeof(records[n]));
  records[n].name.str    = kInstanceName;
  records[n].name.length = sizeof(kInstanceName) - 1;
  records[n].type        = MDNS_RECORDTYPE_SRV;
  records[n].data.srv.priority = 0;
  records[n].data.srv.weight   = 0;
  records[n].data.srv.port     = info->port;
  records[n].data.srv.name.str    = kHostName;
  records[n].data.srv.name.length = sizeof(kHostName) - 1;
  records[n].rclass      = MDNS_CLASS_IN;
  records[n].ttl         = MDNS_WINBOLO_TTL;
  n++;

  /* records[2]: A host -> server LAN address. */
  memset(&records[n], 0, sizeof(records[n]));
  records[n].name.str    = kHostName;
  records[n].name.length = sizeof(kHostName) - 1;
  records[n].type        = MDNS_RECORDTYPE_A;
  records[n].data.a.addr.sin_family = AF_INET;
  records[n].data.a.addr.sin_addr   = info->addr;
  records[n].rclass      = MDNS_CLASS_IN;
  records[n].ttl         = MDNS_WINBOLO_TTL;
  n++;

  /* records[3..]: one TXT record per key=value pair. Values are formatted
   * into txtScratch; keep them in the order documented in mdns_records.h. */
#define MDNS_TXT_ADD(KEY, FMT, ...)                                            \
  do {                                                                        \
    int written;                                                              \
    if (cursor >= txtScratchSize) {                                          \
      return 0;                                                               \
    }                                                                         \
    written = snprintf(txtScratch + cursor, txtScratchSize - cursor,         \
                       FMT, __VA_ARGS__);                                     \
    if (written < 0 || (size_t)written >= txtScratchSize - cursor) {         \
      return 0;                                                               \
    }                                                                         \
    memset(&records[n], 0, sizeof(records[n]));                              \
    records[n].name.str        = kInstanceName;                              \
    records[n].name.length     = sizeof(kInstanceName) - 1;                  \
    records[n].type            = MDNS_RECORDTYPE_TXT;                        \
    records[n].data.txt.key.str    = (KEY);                                  \
    records[n].data.txt.key.length = sizeof(KEY) - 1;                        \
    records[n].data.txt.value.str    = txtScratch + cursor;                  \
    records[n].data.txt.value.length = (size_t)written;                      \
    records[n].rclass          = MDNS_CLASS_IN;                              \
    records[n].ttl             = MDNS_WINBOLO_TTL;                           \
    cursor += (size_t)written + 1; /* keep the NUL between values */         \
    n++;                                                                      \
  } while (0)

  MDNS_TXT_ADD("map",     "%s", info->mapName);
  MDNS_TXT_ADD("ver",     "%u.%u.%u", (unsigned)info->versionMajor,
               (unsigned)info->versionMinor, (unsigned)info->versionRevision);
  MDNS_TXT_ADD("players", "%u", (unsigned)info->numPlayers);
  MDNS_TXT_ADD("bases",   "%u", (unsigned)info->numBases);
  MDNS_TXT_ADD("pills",   "%u", (unsigned)info->numPills);
  MDNS_TXT_ADD("pass",    "%d", info->password ? 1 : 0);
  MDNS_TXT_ADD("mines",   "%d", info->mines ? 1 : 0);
  MDNS_TXT_ADD("game",    "%d", (int)info->game);
  MDNS_TXT_ADD("ai",      "%d", (int)info->ai);
  MDNS_TXT_ADD("lobby",   "%d", info->lobby ? 1 : 0);
  MDNS_TXT_ADD("locked",  "%d", info->locked ? 1 : 0);

#undef MDNS_TXT_ADD

  return n;
}

/*---------------------------------------------------------
 * Live server state -> info POD. Reads the same getters
 * serverHandleInfoRequest uses, plus the lobby/locked
 * state the broadcast/tracker paths can't report.
 *---------------------------------------------------------*/
static void mdnsFillServerInfo(ServerSim *sim, MdnsServerInfo *out) {
  GameSim *gs = serverSimGetGameSim(sim);
  const char *map = serverSimGetMapName(sim);
  int i;
  BYTE numPlayers = 0;

  memset(out, 0, sizeof(*out));
  out->port = s_mdns.gamePort;
  out->addr = s_mdns.localAddr;
  if (map != NULL) {
    strncpy(out->mapName, map, sizeof(out->mapName) - 1);
  }
  out->versionMajor    = BOLO_VERSION_MAJOR;
  out->versionMinor    = BOLO_VERSION_MINOR;
  out->versionRevision = BOLO_VERSION_REVISION;

  for (i = 0; i < MAX_TANKS; i++) {
    if (serverSimIsPlayerConnected(sim, (BYTE)i)) {
      numPlayers++;
    }
  }
  out->numPlayers = numPlayers;
  out->numBases   = basesGetNumNeutral(&gs->bs);
  out->numPills   = pillsGetNumNeutral(&gs->pb);
  out->password   = (serverSimGetPassword(sim)[0] != '\0');
  out->mines      = gs->hiddenMines;
  out->game       = gs->game;
  out->ai         = aiNone; /* AI type not tracked in the new sim */
  out->lobby      = (serverSimGetState(sim) == serverStateLobby);
  out->locked     = transportUdpServerGetLock() || !serverSimIsAcceptingJoins(sim);
}

/*---------------------------------------------------------
 * Query callback — answer _winbolo._udp.local discovery
 * questions with the built record set.
 *---------------------------------------------------------*/
static int mdnsQueryCallback(int sock, const struct sockaddr *from, size_t addrlen,
                             mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                             uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                             size_t name_offset, size_t name_length, size_t record_offset,
                             size_t record_length, void *user_data) {
  MdnsAnswerCtx *ctx = (MdnsAnswerCtx *)user_data;
  char namebuf[256];
  size_t ofs = name_offset;
  mdns_string_t name;
  mdns_record_t records[MDNS_WINBOLO_RECORD_COUNT];
  char txtScratch[256];
  uint32_t sendbuf[512]; /* 2 KiB, 32-bit aligned for the mdns send path */
  size_t count;
  bool unicast;

  (void)ttl;
  (void)record_offset;
  (void)record_length;
  (void)name_length;

  /* Only questions; answers/authority/additional records are not ours. */
  if (entry != MDNS_ENTRYTYPE_QUESTION) {
    return 0;
  }

  name = mdns_string_extract(data, size, &ofs, namebuf, sizeof(namebuf));
  if (name.length != (sizeof(MDNS_WINBOLO_SERVICE) - 1) ||
      strncmp(name.str, MDNS_WINBOLO_SERVICE, name.length) != 0) {
    return 0; /* not our service */
  }
  if (rtype != MDNS_RECORDTYPE_PTR && rtype != MDNS_RECORDTYPE_SRV &&
      rtype != MDNS_RECORDTYPE_TXT && rtype != MDNS_RECORDTYPE_ANY) {
    return 0;
  }

  count = mdnsAdvertiseBuildRecords(ctx->info, records, MDNS_WINBOLO_RECORD_COUNT,
                                    txtScratch, sizeof(txtScratch));
  if (count == 0) {
    return 0;
  }

  /* records[0] is the PTR answer; the rest ride along as additionals so a
   * single response satisfies the browser's PTR -> SRV/A/TXT resolution. */
  unicast = (rclass & MDNS_UNICAST_RESPONSE) != 0;
  if (unicast) {
    mdns_query_answer_unicast(sock, from, addrlen, sendbuf, sizeof(sendbuf),
                              query_id, (mdns_record_type_t)rtype,
                              name.str, name.length,
                              records[0], NULL, 0, records + 1, count - 1);
  } else {
    mdns_query_answer_multicast(sock, sendbuf, sizeof(sendbuf),
                                records[0], NULL, 0, records + 1, count - 1);
  }
  return 0;
}

/*---------------------------------------------------------
 * Lifecycle hooks.
 *---------------------------------------------------------*/
void transportUdpServerStartMdnsAdvertiser(unsigned short gamePort) {
  struct sockaddr_in saddr;

  if (s_mdns.sock >= 0) {
    return; /* already running */
  }

  s_mdns.gamePort = gamePort;
  if (!mdnsFindLocalAddr(&s_mdns.localAddr)) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "mdns: no usable LAN interface; advertiser not started");
    return;
  }

  memset(&saddr, 0, sizeof(saddr));
  saddr.sin_family = AF_INET;
  saddr.sin_addr   = s_mdns.localAddr;
  saddr.sin_port   = htons(MDNS_PORT);
#ifdef __APPLE__
  saddr.sin_len = sizeof(saddr);
#endif

  s_mdns.sock = mdns_socket_open_ipv4(&saddr);
  if (s_mdns.sock < 0) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "mdns: failed to open multicast socket; advertiser not started");
    return;
  }

  WB_LOG_INFO(WB_LOG_CAT_NET,
              "mdns: advertising %s on %s:%u",
              MDNS_WINBOLO_SERVICE, inet_ntoa(s_mdns.localAddr),
              (unsigned)gamePort);
}

void transportUdpServerPollMdnsAdvertiser(struct ServerSim *sim) {
  uint32_t buffer[512]; /* 2 KiB, 32-bit aligned for the mdns listen path */
  MdnsServerInfo info;
  MdnsAnswerCtx ctx;

  if (s_mdns.sock < 0) {
    return; /* never started — cheap no-op */
  }

  mdnsFillServerInfo(sim, &info);
  ctx.sock = s_mdns.sock;
  ctx.info = &info;
  mdns_socket_listen(s_mdns.sock, buffer, sizeof(buffer), mdnsQueryCallback, &ctx);
}

void transportUdpServerStopMdnsAdvertiser(void) {
  if (s_mdns.sock < 0) {
    return; /* never started / already stopped */
  }
  mdns_socket_close(s_mdns.sock);
  s_mdns.sock = -1;
  s_mdns.gamePort = 0;
  memset(&s_mdns.localAddr, 0, sizeof(s_mdns.localAddr));
}

#endif /* __EMSCRIPTEN__ */
