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
 *Name:          Discovery (mDNS browser)
 *Filename:      discovery_mdns.c
 *Purpose:
 *  LAN game discovery via mDNS. The consumer twin of the
 *  server-side advertiser: opens an ephemeral mDNS socket,
 *  queries the _winbolo._udp.local service, and resolves each
 *  responding host's SRV/A/TXT records into a DiscoveryServer.
 *  Mirrors discoveryFindBroadcastGamesAsync — blocks ~5s and
 *  is run on a worker thread by the caller. mdns.h is confined
 *  to this TU + discovery_mdns.h; the public discovery.h
 *  surface stays wire-format-free.
 *********************************************************/

#include "discovery.h"

#ifdef __EMSCRIPTEN__

/* No multicast sockets in the WASM runtime — the LAN mDNS browser is a
 * no-op there (matches the advertiser's Emscripten stubs). */
bool discoveryFindMdnsGamesAsync(DiscoveryServerCallback callback, void *userData) {
  (void)callback;
  (void)userData;
  return false;
}
void discoveryAbortMdnsSearch(void) {}

#else /* !__EMSCRIPTEN__ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "platform_net.h"     /* bolo_net_init / sockets / inet_ntoa */
#include "discovery_mdns.h"   /* parse seam (pulls mdns.h) */
#include "netpacks.h"         /* INFO_PACKET + infoPacketReadViewPolicies / ...2 */
#include "../common/wb_log.h"

/* DNS-SD service the host advertises — the contract mirrored from the
 * advertiser (mdns_records.h). The trailing dot is part of the wire name. */
#define MDNS_WINBOLO_SERVICE "_winbolo._udp.local."

/* mDNS browse window, matching the broadcast search. */
#define MDNS_SEARCH_MS 5000

/* mDNS search abort flag — mirrors discovery.c's s_broadcastAbort. Set by
 * discoveryAbortMdnsSearch, polled by the search loop, cleared at the start
 * of each search so a stale set doesn't shortcut a fresh one. */
static SDL_AtomicInt s_mdnsAbort;

void discoveryAbortMdnsSearch(void) {
  SDL_SetAtomicInt(&s_mdnsAbort, 1);
}

/* True if the record's owner name ends with the _winbolo._udp.local service
 * suffix (SRV/TXT owner = "<instance>._winbolo._udp.local."). Guards against
 * folding unrelated mDNS responders sharing our socket into a bogus server. */
static bool mdnsNameMatchesService(const void *data, size_t size, size_t name_offset) {
  char nbuf[256];
  size_t ofs = name_offset;
  mdns_string_t nm = mdns_string_extract(data, size, &ofs, nbuf, sizeof(nbuf));
  size_t svc = sizeof(MDNS_WINBOLO_SERVICE) - 1;
  if (nm.length < svc) {
    return false;
  }
  return memcmp(nm.str + nm.length - svc, MDNS_WINBOLO_SERVICE, svc) == 0;
}

int discoveryMdnsAccumulate(int sock, const struct sockaddr *from, size_t addrlen,
                            mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                            uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                            size_t name_offset, size_t name_length, size_t record_offset,
                            size_t record_length, void *user_data) {
  DiscoveryMdnsResolved *r = (DiscoveryMdnsResolved *)user_data;

  (void)sock;
  (void)addrlen;
  (void)query_id;
  (void)rclass;
  (void)ttl;
  (void)name_length;

  if (r == NULL) {
    return 0;
  }
  /* Only the resource records — the server sends PTR as the answer and
   * SRV/A/TXT as additionals. */
  if (entry != MDNS_ENTRYTYPE_ANSWER && entry != MDNS_ENTRYTYPE_ADDITIONAL) {
    return 0;
  }

  /* Capture the sender as a fallback address (used only if no A record). */
  if (!r->haveFrom && from != NULL && from->sa_family == AF_INET) {
    r->from = ((const struct sockaddr_in *)from)->sin_addr;
    r->haveFrom = true;
  }

  if (rtype == MDNS_RECORDTYPE_SRV) {
    char nbuf[256];
    mdns_record_srv_t srv;
    if (!mdnsNameMatchesService(data, size, name_offset)) {
      return 0;
    }
    srv = mdns_record_parse_srv(data, size, record_offset, record_length,
                                nbuf, sizeof(nbuf));
    r->port = srv.port;
    r->haveSrv = true;
  } else if (rtype == MDNS_RECORDTYPE_A) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    if (mdns_record_parse_a(data, size, record_offset, record_length, &a)) {
      r->addr = a.sin_addr;
      r->haveAddr = true;
    }
  } else if (rtype == MDNS_RECORDTYPE_TXT) {
    mdns_record_txt_t pairs[DISCOVERY_MDNS_TXT_MAX];
    size_t k, i;
    if (!mdnsNameMatchesService(data, size, name_offset)) {
      return 0;
    }
    k = mdns_record_parse_txt(data, size, record_offset, record_length,
                              pairs, DISCOVERY_MDNS_TXT_MAX);
    for (i = 0; i < k && r->txtCount < DISCOVERY_MDNS_TXT_MAX; i++) {
      DiscoveryMdnsTxt *slot = &r->txt[r->txtCount];
      size_t kl = pairs[i].key.length;
      size_t vl = pairs[i].value.length;
      if (kl > DISCOVERY_MDNS_KEY_MAX - 1) kl = DISCOVERY_MDNS_KEY_MAX - 1;
      if (vl > DISCOVERY_MDNS_VAL_MAX - 1) vl = DISCOVERY_MDNS_VAL_MAX - 1;
      if (pairs[i].key.str != NULL && kl) {
        memcpy(slot->key, pairs[i].key.str, kl);
      }
      slot->key[kl] = '\0';
      if (pairs[i].value.str != NULL && vl) {
        memcpy(slot->value, pairs[i].value.str, vl);
      }
      slot->value[vl] = '\0';
      r->txtCount++;
    }
  }
  return 0;
}

bool discoveryMdnsFillServer(const DiscoveryMdnsResolved *r, DiscoveryServer *out) {
  struct in_addr a;
  size_t i;

  if (r == NULL || out == NULL || !r->haveSrv) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->port = r->port;
  /* The mDNS responder is always our current producer, so the rich
   * flags/counts/md5 keys are present in the TXT record. */
  out->hasRichInfo = true;
  /* A record with no view key means the rules an INFO packet without the
   * view bytes reports. The memset above is that answer for six of the
   * seven — pill always, ally always, classic off, allies in trees off,
   * the expanded overview window, sight off — because each of those is
   * zero. The base view is not: viewPolicyOff is 3, so it is set here.
   * A view key in the loop below overwrites all seven. Meaning B in
   * view_policy.h, not the VIEW_POLICY_STOCK_* set. */
  out->baseView = viewPolicyOff;

  if (r->haveAddr) {
    a = r->addr;
  } else if (r->haveFrom) {
    a = r->from;
  } else {
    a.s_addr = 0;
  }
  SDL_strlcpy(out->address, inet_ntoa(a), sizeof(out->address));

  for (i = 0; i < r->txtCount; i++) {
    const char *key = r->txt[i].key;
    const char *val = r->txt[i].value;
    if (strcmp(key, "map") == 0) {
      SDL_strlcpy(out->mapName, val, sizeof(out->mapName));
    } else if (strcmp(key, "ver") == 0) {
      unsigned mj = 0, mn = 0, rv = 0;
      sscanf(val, "%u.%u.%u", &mj, &mn, &rv);
      out->versionMajor = (BYTE)mj;
      out->versionMinor = (BYTE)mn;
      out->versionRevision = (BYTE)rv;
    } else if (strcmp(key, "players") == 0) {
      out->numPlayers = (BYTE)atoi(val);
    } else if (strcmp(key, "bases") == 0) {
      out->numBases = (BYTE)atoi(val);
    } else if (strcmp(key, "pills") == 0) {
      out->numPills = (BYTE)atoi(val);
    } else if (strcmp(key, "pass") == 0) {
      out->password = (atoi(val) != 0);
    } else if (strcmp(key, "mines") == 0) {
      out->mines = (atoi(val) != 0);
    } else if (strcmp(key, "game") == 0) {
      out->game = (gameType)atoi(val);
    } else if (strcmp(key, "ai") == 0) {
      out->ai = (aiType)atoi(val);
    } else if (strcmp(key, "lobby") == 0) {
      out->inLobby = (atoi(val) != 0);
    } else if (strcmp(key, "locked") == 0) {
      out->locked = (atoi(val) != 0);
    } else if (strcmp(key, "md5") == 0) {
      SDL_strlcpy(out->mapMd5, val, sizeof(out->mapMd5));
    } else if (strcmp(key, "newp") == 0) {
      out->allowNewPlayers = (atoi(val) != 0);
    } else if (strcmp(key, "spingoff") == 0) {
      /* No key means a responder that predates the setting, and every one
       * of those accepted smart pings — which is the false the memset
       * above already left here. */
      out->smartPingsOff = (atoi(val) != 0);
    } else if (strcmp(key, "spec") == 0) {
      out->allowSpectators = (atoi(val) != 0);
    } else if (strcmp(key, "nspec") == 0) {
      out->spectatorCount = (BYTE)atoi(val);
    } else if (strcmp(key, "ranked") == 0) {
      out->ranked = (atoi(val) != 0);
    } else if (strcmp(key, "rnd") == 0) {
      out->randomMap = (atoi(val) != 0);
    } else if (strcmp(key, "tlim") == 0) {
      out->timeLimit = (int32_t)atoi(val);
    } else if (strcmp(key, "humans") == 0) {
      out->numHumans = (BYTE)atoi(val);
    } else if (strcmp(key, "bots") == 0) {
      out->numBots = (BYTE)atoi(val);
    } else if (strcmp(key, "max") == 0) {
      out->maxPlayers = (BYTE)atoi(val);
    } else if (strcmp(key, "view") == 0) {
      /* Two packed bytes as four hex digits. They are read back through
       * the INFO packet's own accessors rather than unpacked here, so
       * both discovery paths agree on what the bits mean — including
       * what an unnamed value reads as. A value that is not four hex
       * digits leaves the defaults set above in place. */
      unsigned v1 = 0, v2 = 0;
      if (sscanf(val, "%2X%2X", &v1, &v2) == 2) {
        INFO_PACKET viewBytes;
        memset(&viewBytes, 0, sizeof(viewBytes));
        viewBytes.view_policies  = (BYTE)v1;
        viewBytes.view_policies2 = (BYTE)v2;
        infoPacketReadViewPolicies(&viewBytes, sizeof(viewBytes),
                                   &out->pillView, &out->baseView,
                                   &out->allyView, &out->classicMode,
                                   &out->alliesInTrees);
        infoPacketReadViewPolicies2(&viewBytes, sizeof(viewBytes),
                                    &out->overviewWindow, &out->lineOfSight);
      }
    }
  }
  return true;
}

bool discoveryFindMdnsGamesAsync(DiscoveryServerCallback callback, void *userData) {
  int sock;
  uint32_t recvbuf[512]; /* 2 KiB, 32-bit aligned for the mdns recv path  */
  uint32_t sendbuf[512]; /* 2 KiB, 32-bit aligned for the mdns query path */
  uint32_t tick, lastSend;
  bool sentOk;
  const uint16_t queryId = 1;

  /* Clear any pending abort from a prior session. */
  SDL_SetAtomicInt(&s_mdnsAbort, 0);

  if (bolo_net_init() != 0) {
    WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to initialise network for mDNS");
    return false;
  }

  /* NULL saddr → bind INADDR_ANY on an ephemeral port and join the mDNS
   * multicast group (the one-shot-query setup documented in mdns.h). */
  sock = mdns_socket_open_ipv4(NULL);
  if (sock < 0) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "discovery: Failed to open mDNS socket");
    bolo_net_cleanup();
    return false;
  }

  sentOk = (mdns_query_send(sock, MDNS_RECORDTYPE_PTR,
                            MDNS_WINBOLO_SERVICE, sizeof(MDNS_WINBOLO_SERVICE) - 1,
                            sendbuf, sizeof(sendbuf), queryId) >= 0);

  tick = (uint32_t)SDL_GetTicks();
  lastSend = tick;
  while ((uint32_t)SDL_GetTicks() - tick <= MDNS_SEARCH_MS) {
    uint32_t now;
    if (SDL_GetAtomicInt(&s_mdnsAbort)) {
      WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: mDNS scan aborted by caller");
      break;
    }
    /* Re-issue the query ~once a second so hosts that missed the first one
     * (or came up mid-window) still answer. */
    now = (uint32_t)SDL_GetTicks();
    if (now - lastSend >= 1000) {
      mdns_query_send(sock, MDNS_RECORDTYPE_PTR,
                      MDNS_WINBOLO_SERVICE, sizeof(MDNS_WINBOLO_SERVICE) - 1,
                      sendbuf, sizeof(sendbuf), queryId);
      lastSend = now;
    }
    /* Drain every datagram currently queued; one datagram resolves one host. */
    for (;;) {
      DiscoveryMdnsResolved acc;
      size_t parsed;
      memset(&acc, 0, sizeof(acc));
      parsed = mdns_query_recv(sock, recvbuf, sizeof(recvbuf),
                               discoveryMdnsAccumulate, &acc, 0);
      if (parsed == 0) {
        break;
      }
      if (acc.haveSrv) {
        DiscoveryServer server;
        if (discoveryMdnsFillServer(&acc, &server)) {
          callback(&server, userData);
        }
      }
    }
    SDL_Delay(50);
  }

  mdns_socket_close(sock);
  bolo_net_cleanup();
  return sentOk;
}

#endif /* __EMSCRIPTEN__ */
