/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Discovery (mDNS browser)
 *Filename:      discovery_mdns.c
 *Purpose:
 *  LAN game discovery via mDNS. The consumer twin of the
 *  server-side advertiser: opens an ephemeral IPv4 mDNS socket
 *  and an IPv6 one (each where it opens), queries the
 *  _winbolo._udp.local service on both, and resolves each
 *  responding host's SRV/A/AAAA/TXT records into a
 *  DiscoveryServer.
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
void discoverySetMdnsBrowser(DiscoveryMdnsBrowser browser) { (void)browser; }
bool discoveryMdnsAbortRequested(void) { return false; }
bool discoveryServerFromMdns(const char *address, unsigned short port,
                             const char *const *keys, const char *const *values,
                             size_t count, DiscoveryServer *out) {
  (void)address; (void)port; (void)keys; (void)values; (void)count; (void)out;
  return false;
}

#else /* !__EMSCRIPTEN__ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "platform_net.h"     /* bolo_net_init / sockets */
#include "bolo_addr.h"        /* BoloAddr, boloAddrParse, boloAddrFormatHost */
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif
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

/* The platform's own browser, when it has one (see discovery.h). */
static DiscoveryMdnsBrowser s_platformBrowser = NULL;

void discoverySetMdnsBrowser(DiscoveryMdnsBrowser browser) {
  s_platformBrowser = browser;
}

bool discoveryMdnsAbortRequested(void) {
  return SDL_GetAtomicInt(&s_mdnsAbort) != 0;
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
  (void)query_id;
  (void)rclass;
  (void)ttl;
  (void)name_length;

  if (r == NULL) {
    return 0;
  }
  /* Only the resource records — the server sends PTR as the answer and
   * SRV/A/AAAA/TXT as additionals. */
  if (entry != MDNS_ENTRYTYPE_ANSWER && entry != MDNS_ENTRYTYPE_ADDITIONAL) {
    return 0;
  }

  /* Capture the sender as a fallback address (used only when no address
   * record was usable), and for the scope of a link-local AAAA. */
  if (boloAddrFamily(&r->from) == 0 && from != NULL) {
    (void)boloAddrFromSockaddr(from, (socklen_t)addrlen, &r->from);
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
      (void)boloAddrFromSockaddr((const struct sockaddr *)&a,
                                 (socklen_t)sizeof(a), &r->addr4);
    }
  } else if (rtype == MDNS_RECORDTYPE_AAAA && record_length == 16) {
    struct sockaddr_in6 a6;
    BoloAddr cand;
    memset(&a6, 0, sizeof(a6));
    if (mdns_record_parse_aaaa(data, size, record_offset, record_length, &a6) &&
        boloAddrFromSockaddr((const struct sockaddr *)&a6,
                             (socklen_t)sizeof(a6), &cand) &&
        !boloAddrIsUnspecified(&cand)) {
      bool usable = true;
      if (boloAddrIsV6LinkLocal(&cand)) {
        /* The record names no interface; the one the datagram came in on
         * is the one the address is reachable through. */
        uint32_t scope = 0;
        if (boloAddrFamily(&r->from) == AF_INET6) {
          scope = (uint32_t)((const struct sockaddr_in6 *)&r->from.ss)
                      ->sin6_scope_id;
        }
        if (scope == 0) {
          usable = false;
        } else {
          ((struct sockaddr_in6 *)&cand.ss)->sin6_scope_id = scope;
        }
      }
      if (usable && (boloAddrFamily(&r->addr6) == 0 ||
                     (boloAddrIsV6LinkLocal(&r->addr6) &&
                      !boloAddrIsV6LinkLocal(&cand)))) {
        r->addr6 = cand;
      }
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
  const BoloAddr *a;
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
   * view bytes reports. The memset above is that answer for seven of the
   * eight — pill always, ally always, classic off, allies in trees off,
   * the expanded overview window, sight off, positional sound off —
   * because each of those is zero. The base view is not: viewPolicyOff
   * is 3, so it is set here. A view key in the loop below overwrites all
   * eight. Meaning B in
   * view_policy.h, not the VIEW_POLICY_STOCK_* set. */
  out->baseView = viewPolicyOff;

  /* IPv4 first: it is the address most second machines can reach, and a
   * dual-stack host answering on both sockets then gives one address text,
   * which the game browser lists once. */
  if (boloAddrFamily(&r->addr4) != 0) {
    a = &r->addr4;
  } else if (boloAddrFamily(&r->addr6) != 0) {
    a = &r->addr6;
  } else if (boloAddrFamily(&r->from) != 0) {
    a = &r->from;
  } else {
    a = NULL;
  }
  if (a == NULL ||
      !boloAddrFormatHost(a, out->address, sizeof(out->address))) {
    SDL_strlcpy(out->address, "0.0.0.0", sizeof(out->address));
  }

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
                                    &out->overviewWindow, &out->lineOfSight,
                                    &out->positionalSound);
        /* A record carrying the key at all is a record that says what its
         * rules are; one without it said nothing and kept the stand-in. */
        out->hasViewInfo = true;
      }
    }
  }
  return true;
}

bool discoveryServerFromMdns(const char *address, unsigned short port,
                             const char *const *keys, const char *const *values,
                             size_t count, DiscoveryServer *out) {
  DiscoveryMdnsResolved r;
  BoloAddr parsed;
  size_t i;

  if (address == NULL || out == NULL) {
    return false;
  }
  memset(&r, 0, sizeof(r));
  /* A numeric IPv4 address, or IPv6 with an optional %scope; a name is
   * refused, as boloAddrParse never looks one up. */
  if (!boloAddrParse(address, port, &parsed)) {
    return false;
  }
  if (boloAddrFamily(&parsed) == AF_INET) {
    r.addr4 = parsed;
  } else {
    r.addr6 = parsed;
  }
  r.haveSrv = true;
  r.port = port;
  for (i = 0; i < count && r.txtCount < DISCOVERY_MDNS_TXT_MAX; i++) {
    if (keys[i] == NULL || keys[i][0] == '\0') {
      continue;
    }
    SDL_strlcpy(r.txt[r.txtCount].key, keys[i], DISCOVERY_MDNS_KEY_MAX);
    SDL_strlcpy(r.txt[r.txtCount].value, values[i] ? values[i] : "",
                DISCOVERY_MDNS_VAL_MAX);
    r.txtCount++;
  }
  if (!discoveryMdnsFillServer(&r, out)) {
    return false;
  }
  if (r.txtCount == 0) {
    out->hasRichInfo = false;
  }
  return true;
}

/* Most interfaces one search sends its IPv6 query on. */
#define MDNS_V6_IF_MAX 16

#ifndef _WIN32
/* setsockopt with the argument types the POSIX declaration takes. */
static int mdnsBrowseSetSockOpt(int sock, int level, int name,
                                const void *value, size_t len) {
  return setsockopt(sock, level, name, value, (socklen_t)len);
}

/* Fills ifs with the indexes of the interfaces that are up, not loopback,
 * multicast-capable and carry an IPv6 address, each index once (one
 * interface usually has several IPv6 addresses), at most cap of them.
 * Returns the count. */
static size_t mdnsListV6Interfaces(unsigned int *ifs, size_t cap) {
  struct ifaddrs *ifap, *ifa;
  size_t n = 0;

  if (getifaddrs(&ifap) != 0) {
    return 0;
  }
  for (ifa = ifap; ifa != NULL && n < cap; ifa = ifa->ifa_next) {
    unsigned int idx;
    size_t k;
    bool seen = false;
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET6 ||
        ifa->ifa_name == NULL) {
      continue;
    }
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK) ||
        !(ifa->ifa_flags & IFF_MULTICAST)) {
      continue;
    }
    idx = if_nametoindex(ifa->ifa_name);
    if (idx == 0) {
      continue;
    }
    for (k = 0; k < n; k++) {
      if (ifs[k] == idx) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      ifs[n++] = idx;
    }
  }
  freeifaddrs(ifap);
  return n;
}
#endif

/* Opens the browser's IPv6 socket on an ephemeral port, or returns -1 when
 * the machine has no IPv6. The query asks for unicast answers, which come
 * back to this socket's own port, so the socket does not need to be in the
 * mDNS group to hear them. mdns_socket_setup_ipv6 joins ff02::fb on
 * interface 0, and when the system refuses that join (a LAN with only
 * link-local addresses can have no IPv6 route for the system to choose the
 * interface by) the library leaves the socket unbound; on POSIX systems it
 * is then bound and set non-blocking here, as the library would have done,
 * and used without the group. */
static int mdnsBrowseOpenSocket6(void) {
  int sock = (int)socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    return -1;
  }
  if (mdns_socket_setup_ipv6(sock, NULL) == 0) {
    return sock;
  }
#ifndef _WIN32
  {
    struct sockaddr_in6 any;
    int flags;
    memset(&any, 0, sizeof(any));
    any.sin6_family = AF_INET6;
    any.sin6_addr   = in6addr_any;
#ifdef __APPLE__
    any.sin6_len = sizeof(any);
#endif
    if (bind(sock, (const struct sockaddr *)&any, (socklen_t)sizeof(any)) == 0) {
      flags = fcntl(sock, F_GETFL, 0);
      if (flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0) {
        return sock;
      }
    }
  }
#endif
  mdns_socket_close(sock);
  return -1;
}

/* Sends the PTR query on the IPv6 socket once per interface in ifs, with
 * IPV6_MULTICAST_IF set to that interface, so the query reaches every LAN
 * the machine is on and not only the one the system would choose; an
 * interface the option is refused for is skipped. With no interfaces
 * listed it sends once on interface 0. Windows always sends once on
 * interface 0: listing the interfaces and their indexes there needs
 * GetAdaptersAddresses and the IP Helper library, which this code does not
 * link. Returns true when at least one send succeeded. */
static bool mdnsBrowseSendQuery6(int sock, const unsigned int *ifs, size_t ifCount,
                                 void *sendbuf, size_t sendcap, uint16_t queryId) {
#ifndef _WIN32
  if (ifCount > 0) {
    bool ok = false;
    size_t i;
    for (i = 0; i < ifCount; i++) {
      unsigned int idx = ifs[i];
      if (mdnsBrowseSetSockOpt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx,
                               sizeof(idx)) != 0) {
        continue;
      }
      if (mdns_query_send(sock, MDNS_RECORDTYPE_PTR, MDNS_WINBOLO_SERVICE,
                          sizeof(MDNS_WINBOLO_SERVICE) - 1, sendbuf, sendcap,
                          queryId) >= 0) {
        ok = true;
      }
    }
    return ok;
  }
#else
  (void)ifs;
  (void)ifCount;
#endif
  return mdns_query_send(sock, MDNS_RECORDTYPE_PTR, MDNS_WINBOLO_SERVICE,
                         sizeof(MDNS_WINBOLO_SERVICE) - 1, sendbuf, sendcap,
                         queryId) >= 0;
}

/* Drains every datagram currently queued on sock; one datagram resolves one
 * host. */
static void mdnsBrowseDrain(int sock, void *recvbuf, size_t recvcap,
                            DiscoveryServerCallback callback, void *userData) {
  for (;;) {
    DiscoveryMdnsResolved acc;
    size_t parsed;
    memset(&acc, 0, sizeof(acc));
    parsed = mdns_query_recv(sock, recvbuf, recvcap,
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
}

bool discoveryFindMdnsGamesAsync(DiscoveryServerCallback callback, void *userData) {
  int sock4, sock6;
  uint32_t recvbuf[512]; /* 2 KiB, 32-bit aligned for the mdns recv path  */
  uint32_t sendbuf[512]; /* 2 KiB, 32-bit aligned for the mdns query path */
  unsigned int ifs6[MDNS_V6_IF_MAX] = { 0 };
  size_t ifCount6 = 0;
  uint32_t tick, lastSend;
  bool sentOk = false;
  const uint16_t queryId = 1;

  /* Clear any pending abort from a prior session. */
  SDL_SetAtomicInt(&s_mdnsAbort, 0);

  if (s_platformBrowser != NULL) {
    return s_platformBrowser(callback, userData);
  }

  if (bolo_net_init() != 0) {
    WB_LOG_ERROR(WB_LOG_CAT_NET, "discovery: Failed to initialise network for mDNS");
    return false;
  }

  /* NULL saddr → bind INADDR_ANY on an ephemeral port and join the mDNS
   * multicast group (the one-shot-query setup documented in mdns.h). The
   * IPv6 socket is opened beside it; either alone is enough to search. */
  sock4 = mdns_socket_open_ipv4(NULL);
  if (sock4 < 0) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: the IPv4 mDNS socket did not open");
  }
  sock6 = mdnsBrowseOpenSocket6();
  if (sock6 < 0) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "discovery: the IPv6 mDNS socket did not open");
  }
  if (sock4 < 0 && sock6 < 0) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "discovery: Failed to open mDNS socket");
    bolo_net_cleanup();
    return false;
  }
#ifndef _WIN32
  /* Listed once per search; the re-sends below reuse the list. */
  if (sock6 >= 0) {
    ifCount6 = mdnsListV6Interfaces(ifs6, MDNS_V6_IF_MAX);
  }
#endif

  if (sock4 >= 0 &&
      mdns_query_send(sock4, MDNS_RECORDTYPE_PTR,
                      MDNS_WINBOLO_SERVICE, sizeof(MDNS_WINBOLO_SERVICE) - 1,
                      sendbuf, sizeof(sendbuf), queryId) >= 0) {
    sentOk = true;
  }
  if (sock6 >= 0 &&
      mdnsBrowseSendQuery6(sock6, ifs6, ifCount6, sendbuf, sizeof(sendbuf),
                           queryId)) {
    sentOk = true;
  }

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
      if (sock4 >= 0) {
        mdns_query_send(sock4, MDNS_RECORDTYPE_PTR,
                        MDNS_WINBOLO_SERVICE, sizeof(MDNS_WINBOLO_SERVICE) - 1,
                        sendbuf, sizeof(sendbuf), queryId);
      }
      if (sock6 >= 0) {
        (void)mdnsBrowseSendQuery6(sock6, ifs6, ifCount6, sendbuf,
                                   sizeof(sendbuf), queryId);
      }
      lastSend = now;
    }
    /* Both sockets are non-blocking, so an empty one returns at once. */
    if (sock4 >= 0) {
      mdnsBrowseDrain(sock4, recvbuf, sizeof(recvbuf), callback, userData);
    }
    if (sock6 >= 0) {
      mdnsBrowseDrain(sock6, recvbuf, sizeof(recvbuf), callback, userData);
    }
    SDL_Delay(50);
  }

  if (sock4 >= 0) {
    mdns_socket_close(sock4);
  }
  if (sock6 >= 0) {
    mdns_socket_close(sock6);
  }
  bolo_net_cleanup();
  return sentOk;
}

#endif /* __EMSCRIPTEN__ */
