/*
 * mDNS LAN discovery round-trip (test_mdns_discovery.c).
 *
 * Exercises the real serialize + parse code over a unicast loopback path —
 * no multicast socket, no 5s browse loop:
 *
 *   - Build the advertiser's answer set with mdnsAdvertiseBuildRecords
 *     (the Pass-A socket-free builder).
 *   - Send it to a second loopback UDP socket with mdns_query_answer_unicast.
 *   - Receive on that socket with mdns_query_recv, driving the browser's own
 *     parse path (discoveryMdnsAccumulate + discoveryMdnsFillServer).
 *
 * Asserts the SRV port, the inLobby/locked flags, and every TXT/A/SRV field,
 * then proves two hosts resolve to two distinct DiscoveryServers with the
 * unique per-host instance label from the builder.
 */

#include "test_harness.h"

#include <stdio.h>
#include <string.h>

#include "platform_net.h"     /* sockets / htons / inet_addr / bolo_net_init */
#ifndef _WIN32
#include <net/if.h>           /* if_nametoindex */
#endif
#include "bolo_addr.h"        /* BoloAddr, boloAddrParse */
#include "mdns.h"             /* mdns_query_answer_unicast / mdns_query_recv */
#include "mdns_records.h"     /* MdnsServerInfo + mdnsAdvertiseBuildRecords */
#include "discovery.h"        /* DiscoveryServer */
#include "netpacks.h"         /* infoPacketPackViewPolicies / ...2 */
#include "view_policy.h"      /* the policies the view key carries */
#include "discovery_mdns.h"   /* parse seam */

#define MDNS_TEST_SERVICE "_winbolo._udp.local."

/* A loopback UDP socket bound to 127.0.0.1:0, with a receive timeout so a
 * missing datagram fails the test instead of hanging. Returns the bound port
 * via *outPort. */
static bolo_socket_t makeUdpSocket(unsigned short *outPort) {
  bolo_socket_t s;
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);

  s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == BOLO_INVALID_SOCKET) {
    return BOLO_INVALID_SOCKET;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = 0;
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(s);
    return BOLO_INVALID_SOCKET;
  }
  if (getsockname(s, (struct sockaddr *)&addr, &len) != 0) {
    closesocket(s);
    return BOLO_INVALID_SOCKET;
  }
  *outPort = ntohs(addr.sin_port);

#ifdef _WIN32
  {
    DWORD tv = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  }
#else
  {
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  }
#endif
  return s;
}

/* Build info's answer set and unicast it to dest. Returns 0 on success. */
static int sendServerAnswer(bolo_socket_t responder, const struct sockaddr_in *dest,
                            const MdnsServerInfo *info) {
  mdns_record_t records[MDNS_WINBOLO_RECORD_COUNT];
  char txtScratch[256];
  uint32_t sendbuf[512];
  size_t n = mdnsAdvertiseBuildRecords(info, records, MDNS_WINBOLO_RECORD_COUNT,
                                       txtScratch, sizeof(txtScratch));
  if (n == 0) {
    return -1;
  }
  return mdns_query_answer_unicast((int)responder, dest, sizeof(*dest),
                                   sendbuf, sizeof(sendbuf), 1 /*query_id*/,
                                   MDNS_RECORDTYPE_PTR,
                                   MDNS_TEST_SERVICE, sizeof(MDNS_TEST_SERVICE) - 1,
                                   records[0], NULL, 0, records + 1, n - 1);
}

/* Receive one datagram on browser and resolve it to *out via the browser's
 * real parse path. Returns 0 on success. */
static int recvServer(bolo_socket_t browser, DiscoveryServer *out) {
  uint32_t recvbuf[512];
  int tries;
  for (tries = 0; tries < 4; tries++) {
    DiscoveryMdnsResolved acc;
    size_t parsed;
    memset(&acc, 0, sizeof(acc));
    parsed = mdns_query_recv((int)browser, recvbuf, sizeof(recvbuf),
                             discoveryMdnsAccumulate, &acc, 0);
    if (parsed > 0 && acc.haveSrv) {
      return discoveryMdnsFillServer(&acc, out) ? 0 : -1;
    }
    if (parsed == 0) {
      break; /* recv timed out — no datagram waiting */
    }
  }
  return -1;
}

/* Runs one received answer datagram through the browser's record callback
 * as if it had arrived from `from`, which a loopback socket cannot supply
 * for a scoped IPv6 source. Returns 0 when the header and questions parse. */
static int parseWithSource(const uint32_t *buf, size_t len,
                           const struct sockaddr *from, size_t fromLen,
                           DiscoveryMdnsResolved *acc) {
  const uint16_t *hdr = (const uint16_t *)buf;
  uint16_t queryId, questions, answers, authority, additional, q;
  size_t offset = 12;

  if (len < 12) {
    return -1;
  }
  queryId    = mdns_ntohs(hdr);
  questions  = mdns_ntohs(hdr + 2);
  answers    = mdns_ntohs(hdr + 3);
  authority  = mdns_ntohs(hdr + 4);
  additional = mdns_ntohs(hdr + 5);
  for (q = 0; q < questions; q++) {
    if (!mdns_string_skip(buf, len, &offset) || offset + 4 > len) {
      return -1;
    }
    offset += 4; /* type and class */
  }
  mdns_records_parse(-1, from, fromLen, buf, len, &offset, MDNS_ENTRYTYPE_ANSWER,
                     queryId, answers, discoveryMdnsAccumulate, acc);
  mdns_records_parse(-1, from, fromLen, buf, len, &offset, MDNS_ENTRYTYPE_AUTHORITY,
                     queryId, authority, discoveryMdnsAccumulate, acc);
  mdns_records_parse(-1, from, fromLen, buf, len, &offset, MDNS_ENTRYTYPE_ADDITIONAL,
                     queryId, additional, discoveryMdnsAccumulate, acc);
  return 0;
}

/* The index of this machine's loopback interface and its name, so that a
 * link-local address scoped to it formats as "%<name>" and parses back. 0
 * when neither "lo0" nor "lo" exists, and always on Windows, where
 * if_nametoindex is in the IP Helper library the tests do not link. */
static unsigned int loopbackScope(const char **nameOut) {
#ifndef _WIN32
  unsigned int idx = if_nametoindex("lo0");
  if (idx != 0) {
    *nameOut = "lo0";
    return idx;
  }
  idx = if_nametoindex("lo");
  if (idx != 0) {
    *nameOut = "lo";
    return idx;
  }
#endif
  *nameOut = "";
  return 0;
}

/* The fields the address cases check besides the address. */
static void fillBasicInfo(MdnsServerInfo *info, unsigned short port) {
  memset(info, 0, sizeof(*info));
  info->port = port;
  strncpy(info->mapName, "Everard Island", sizeof(info->mapName) - 1);
  info->versionMajor = 1;
  info->versionMinor = 2;
  info->versionRevision = 3;
  info->numPlayers = 4;
  info->numBases = 5;
  info->numPills = 6;
  info->lobby = true;
  info->maxPlayers = 12;
}

/* Counts the A and AAAA records in a built answer set. */
static void countAddressRecords(const mdns_record_t *records, size_t n,
                                int *aCount, int *aaaaCount) {
  size_t i;
  *aCount = 0;
  *aaaaCount = 0;
  for (i = 0; i < n; i++) {
    if (records[i].type == MDNS_RECORDTYPE_A) {
      (*aCount)++;
    } else if (records[i].type == MDNS_RECORDTYPE_AAAA) {
      (*aaaaCount)++;
    }
  }
}

/* A browser and a responder socket on 127.0.0.1, with the browser's address
 * in *browserAddr. Returns 0 on success. */
static int openLoopbackPair(bolo_socket_t *browser, bolo_socket_t *responder,
                            struct sockaddr_in *browserAddr) {
  unsigned short browserPort = 0, responderPort = 0;
  *browser = makeUdpSocket(&browserPort);
  *responder = makeUdpSocket(&responderPort);
  if (*browser == BOLO_INVALID_SOCKET || *responder == BOLO_INVALID_SOCKET) {
    return -1;
  }
  memset(browserAddr, 0, sizeof(*browserAddr));
  browserAddr->sin_family = AF_INET;
  browserAddr->sin_addr.s_addr = inet_addr("127.0.0.1");
  browserAddr->sin_port = htons(browserPort);
  return 0;
}

/* A host with only an IPv6 address: no A record, an AAAA record, the
 * WinBolo-v6- instance name, and the browser lists it at that address. */
int run_mdns_discovery_aaaa_only(void) {
  bolo_socket_t browser, responder;
  struct sockaddr_in browserAddr;
  MdnsServerInfo info;
  mdns_record_t records[MDNS_WINBOLO_RECORD_COUNT];
  char recScratch[512];
  char instance[96];
  const char *keys[MDNS_WINBOLO_TXT_COUNT];
  const char *values[MDNS_WINBOLO_TXT_COUNT];
  char scratch[512];
  size_t n;
  int aCount, aaaaCount;
  DiscoveryServer s;

  bolo_net_init();
  UT_ASSERT(openLoopbackPair(&browser, &responder, &browserAddr) == 0);

  fillBasicInfo(&info, 27520);
  UT_ASSERT(boloAddrParse("2001:db8::20", 0, &info.addr6));

  n = mdnsAdvertiseBuildRecords(&info, records, MDNS_WINBOLO_RECORD_COUNT,
                                recScratch, sizeof(recScratch));
  UT_ASSERT_MSG(n == MDNS_WINBOLO_RECORD_COUNT - 1, "records=%u", (unsigned)n);
  countAddressRecords(records, n, &aCount, &aaaaCount);
  UT_ASSERT_MSG(aCount == 0, "A records=%d", aCount);
  UT_ASSERT_MSG(aaaaCount == 1, "AAAA records=%d", aaaaCount);

  UT_ASSERT(mdnsAdvertiseTxtPairs(&info, instance, sizeof(instance), keys, values,
                                  MDNS_WINBOLO_TXT_COUNT, scratch,
                                  sizeof(scratch)) == MDNS_WINBOLO_TXT_COUNT);
  UT_ASSERT_MSG(strcmp(instance, "WinBolo-v6-00000020-27520") == 0,
                "instance='%s'", instance);

  UT_ASSERT(sendServerAnswer(responder, &browserAddr, &info) == 0);
  memset(&s, 0, sizeof(s));
  UT_ASSERT(recvServer(browser, &s) == 0);
  UT_ASSERT_MSG(strcmp(s.address, "2001:db8::20") == 0, "addr='%s'", s.address);
  UT_ASSERT_MSG(s.port == 27520, "port=%u", (unsigned)s.port);
  UT_ASSERT_MSG(strcmp(s.mapName, "Everard Island") == 0, "map='%s'", s.mapName);
  UT_ASSERT(s.versionMajor == 1 && s.versionMinor == 2 && s.versionRevision == 3);
  UT_ASSERT(s.numPlayers == 4 && s.numBases == 5 && s.numPills == 6);
  UT_ASSERT(s.inLobby == true);
  UT_ASSERT_MSG(s.maxPlayers == 12, "max=%u", (unsigned)s.maxPlayers);

  closesocket(browser);
  closesocket(responder);
  bolo_net_cleanup();
  return 0;
}

/* A host with both addresses: A and AAAA records, the IPv4 instance name,
 * and the browser lists it at the IPv4 address. */
int run_mdns_discovery_both_prefers_v4(void) {
  bolo_socket_t browser, responder;
  struct sockaddr_in browserAddr;
  MdnsServerInfo info;
  mdns_record_t records[MDNS_WINBOLO_RECORD_COUNT];
  char recScratch[512];
  char instance[96];
  const char *keys[MDNS_WINBOLO_TXT_COUNT];
  const char *values[MDNS_WINBOLO_TXT_COUNT];
  char scratch[512];
  size_t n;
  int aCount, aaaaCount;
  DiscoveryServer s;

  bolo_net_init();
  UT_ASSERT(openLoopbackPair(&browser, &responder, &browserAddr) == 0);

  fillBasicInfo(&info, 27521);
  UT_ASSERT(boloAddrParse("192.168.1.20", 0, &info.addr4));
  UT_ASSERT(boloAddrParse("2001:db8::20", 0, &info.addr6));

  n = mdnsAdvertiseBuildRecords(&info, records, MDNS_WINBOLO_RECORD_COUNT,
                                recScratch, sizeof(recScratch));
  UT_ASSERT_MSG(n == MDNS_WINBOLO_RECORD_COUNT, "records=%u", (unsigned)n);
  countAddressRecords(records, n, &aCount, &aaaaCount);
  UT_ASSERT_MSG(aCount == 1, "A records=%d", aCount);
  UT_ASSERT_MSG(aaaaCount == 1, "AAAA records=%d", aaaaCount);

  UT_ASSERT(mdnsAdvertiseTxtPairs(&info, instance, sizeof(instance), keys, values,
                                  MDNS_WINBOLO_TXT_COUNT, scratch,
                                  sizeof(scratch)) == MDNS_WINBOLO_TXT_COUNT);
  UT_ASSERT_MSG(strcmp(instance, "WinBolo-192-168-1-20-27521") == 0,
                "instance='%s'", instance);

  /* The answer comes from 127.0.0.1, so 192.168.1.20 can only be the A
   * record's address. */
  UT_ASSERT(sendServerAnswer(responder, &browserAddr, &info) == 0);
  memset(&s, 0, sizeof(s));
  UT_ASSERT(recvServer(browser, &s) == 0);
  UT_ASSERT_MSG(strcmp(s.address, "192.168.1.20") == 0, "addr='%s'", s.address);
  UT_ASSERT_MSG(s.port == 27521, "port=%u", (unsigned)s.port);

  closesocket(browser);
  closesocket(responder);
  bolo_net_cleanup();
  return 0;
}

/* A link-local AAAA takes its scope from the IPv6 source of the datagram,
 * and is ignored when the source is IPv4. */
int run_mdns_discovery_link_local_scope(void) {
  bolo_socket_t browser, responder;
  struct sockaddr_in browserAddr;
  MdnsServerInfo info;
  uint32_t recvbuf[512];
  int got;
  struct sockaddr_in6 src6;
  struct sockaddr_in src4;
  DiscoveryMdnsResolved acc;
  DiscoveryServer s;
  BoloAddr parsed;
  const char *loName = "";
  unsigned int scope = loopbackScope(&loName);

  if (scope == 0) {
    fprintf(stderr, "  mdns_discovery_link_local_scope: skipped — no lo0 or lo "
                    "interface to scope a link-local address to\n");
    return 0;
  }

  bolo_net_init();
  UT_ASSERT(openLoopbackPair(&browser, &responder, &browserAddr) == 0);

  fillBasicInfo(&info, 27522);
  UT_ASSERT(boloAddrParse("fe80::20", 0, &info.addr6));
  UT_ASSERT(sendServerAnswer(responder, &browserAddr, &info) == 0);
  got = (int)recv(browser, (char *)recvbuf, (int)sizeof(recvbuf), 0);
  UT_ASSERT_MSG(got > 0, "recv=%d", got);

  /* From an IPv6 source on the loopback interface: the address carries
   * that interface as its scope. */
  memset(&src6, 0, sizeof(src6));
  src6.sin6_family = AF_INET6;
#ifdef __APPLE__
  src6.sin6_len = sizeof(src6);
#endif
  UT_ASSERT(inet_pton(AF_INET6, "fe80::20", &src6.sin6_addr) == 1);
  src6.sin6_port = htons(5353);
  src6.sin6_scope_id = scope;
  memset(&acc, 0, sizeof(acc));
  UT_ASSERT(parseWithSource(recvbuf, (size_t)got, (const struct sockaddr *)&src6,
                            sizeof(src6), &acc) == 0);
  UT_ASSERT(acc.haveSrv);
  UT_ASSERT(boloAddrFamily(&acc.addr6) == AF_INET6);
  memset(&s, 0, sizeof(s));
  UT_ASSERT(discoveryMdnsFillServer(&acc, &s));
  UT_ASSERT_MSG(s.port == 27522, "port=%u", (unsigned)s.port);
  UT_ASSERT_MSG(boloAddrParse(s.address, 0, &parsed), "addr='%s'", s.address);
  UT_ASSERT_MSG(boloAddrIsLinkLocal(&parsed), "addr='%s'", s.address);
  UT_ASSERT_MSG(((const struct sockaddr_in6 *)&parsed.ss)->sin6_scope_id == scope,
                "addr='%s' scope=%lu, %s is %u", s.address,
                (unsigned long)((const struct sockaddr_in6 *)&parsed.ss)->sin6_scope_id,
                loName, scope);

  /* The same answer from an IPv4 source: no scope to give the link-local
   * AAAA, so it is ignored and the sender's address is used. */
  memset(&src4, 0, sizeof(src4));
  src4.sin_family = AF_INET;
  UT_ASSERT(inet_pton(AF_INET, "192.0.2.7", &src4.sin_addr) == 1);
  src4.sin_port = htons(5353);
  memset(&acc, 0, sizeof(acc));
  UT_ASSERT(parseWithSource(recvbuf, (size_t)got, (const struct sockaddr *)&src4,
                            sizeof(src4), &acc) == 0);
  UT_ASSERT(acc.haveSrv);
  UT_ASSERT(boloAddrFamily(&acc.addr6) == 0);
  memset(&s, 0, sizeof(s));
  UT_ASSERT(discoveryMdnsFillServer(&acc, &s));
  UT_ASSERT_MSG(strcmp(s.address, "192.0.2.7") == 0, "addr='%s'", s.address);

  closesocket(browser);
  closesocket(responder);
  bolo_net_cleanup();
  return 0;
}

int run_mdns_discovery(void) {
  bolo_socket_t browser, responder;
  unsigned short browserPort = 0, responderPort = 0;
  struct sockaddr_in browserAddr;
  MdnsServerInfo info;
  DiscoveryServer s;

  bolo_net_init();

  browser = makeUdpSocket(&browserPort);
  responder = makeUdpSocket(&responderPort);
  UT_ASSERT(browser != BOLO_INVALID_SOCKET);
  UT_ASSERT(responder != BOLO_INVALID_SOCKET);

  memset(&browserAddr, 0, sizeof(browserAddr));
  browserAddr.sin_family = AF_INET;
  browserAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  browserAddr.sin_port = htons(browserPort);

  /* ---- single-server round-trip: every field through the real codecs ---- */
  memset(&info, 0, sizeof(info));
  info.port = 27510; /* non-default game port */
  UT_ASSERT(boloAddrParse("127.0.0.1", 0, &info.addr4));
  strncpy(info.mapName, "Everard Island", sizeof(info.mapName) - 1);
  info.versionMajor = 1;
  info.versionMinor = 2;
  info.versionRevision = 3;
  info.numPlayers = 4;
  info.numBases = 5;
  info.numPills = 6;
  info.password = true;
  info.mines = true;
  info.game = (gameType)2;
  info.ai = (aiType)0;
  info.lobby = true;
  info.locked = true;
  strncpy(info.mapMd5Hex, "0123456789abcdef0123456789abcdef", sizeof(info.mapMd5Hex) - 1);
  info.allowNewPlayers = true;
  info.allowSpectators = false;
  info.spectatorCount  = 0;
  info.ranked          = true;
  info.randomMap       = false;
  info.timeLimit       = 3000;
  info.numHumans       = 3;
  info.numBots         = 1;
  info.maxPlayers      = 12;
  info.viewPolicies    = infoPacketPackViewPolicies(viewPolicyKey,
                                                    viewPolicyAlways,
                                                    viewPolicyOff, true, true);
  info.viewPolicies2   = infoPacketPackViewPolicies2(
      (uint8_t)overviewWindowClassic,
      (uint8_t)lineOfSightBuildingsAndTrees, true);
  /* Negative sense on the wire: spingoff=1 means the host banned them. */
  info.smartPingsOff   = true;

  UT_ASSERT(sendServerAnswer(responder, &browserAddr, &info) == 0);

  memset(&s, 0, sizeof(s));
  UT_ASSERT(recvServer(browser, &s) == 0);

  UT_ASSERT_MSG(s.port == 27510, "port=%u", (unsigned)s.port);
  UT_ASSERT(s.hasRichInfo == true);
  UT_ASSERT(s.inLobby == true);
  UT_ASSERT(s.locked == true);
  UT_ASSERT_MSG(strcmp(s.mapMd5, "0123456789abcdef0123456789abcdef") == 0, "md5='%s'", s.mapMd5);
  UT_ASSERT(s.allowNewPlayers == true);
  UT_ASSERT(s.ranked == true);
  UT_ASSERT(s.randomMap == false);
  UT_ASSERT_MSG(s.timeLimit == 3000, "tlim=%d", (int)s.timeLimit);
  UT_ASSERT(s.numHumans == 3 && s.numBots == 1);
  UT_ASSERT_MSG(s.maxPlayers == 12, "max=%u", (unsigned)s.maxPlayers);
  UT_ASSERT_MSG(strcmp(s.mapName, "Everard Island") == 0, "map='%s'", s.mapName);
  UT_ASSERT(s.versionMajor == 1 && s.versionMinor == 2 && s.versionRevision == 3);
  UT_ASSERT(s.numPlayers == 4 && s.numBases == 5 && s.numPills == 6);
  UT_ASSERT(s.password == true);
  UT_ASSERT(s.mines == true);
  UT_ASSERT(s.game == (gameType)2);
  UT_ASSERT(s.ai == (aiType)0);
  UT_ASSERT_MSG(strcmp(s.address, "127.0.0.1") == 0, "addr='%s'", s.address);
  /* The view key: eight values in four hex chars, over the real wire. */
  UT_ASSERT_MSG(s.pillView == viewPolicyKey, "pill=%d", (int)s.pillView);
  UT_ASSERT_MSG(s.baseView == viewPolicyAlways, "base=%d", (int)s.baseView);
  UT_ASSERT_MSG(s.allyView == viewPolicyOff, "ally=%d", (int)s.allyView);
  UT_ASSERT(s.classicMode == true);
  UT_ASSERT(s.alliesInTrees == true);
  UT_ASSERT_MSG(s.overviewWindow == (uint8_t)overviewWindowClassic,
                "window=%u", (unsigned)s.overviewWindow);
  UT_ASSERT_MSG(s.lineOfSight == (uint8_t)lineOfSightBuildingsAndTrees,
                "sight=%u", (unsigned)s.lineOfSight);
  UT_ASSERT_MSG(s.positionalSound == true,
                "positional sound did not survive the wire");
  UT_ASSERT_MSG(s.smartPingsOff == true,
                "spingoff did not survive the wire: smartPingsOff=%d",
                (int)s.smartPingsOff);

  /* And the other way: a host that left pings on advertises spingoff=0 and
     the browser reads it back as allowed. */
  {
    MdnsServerInfo allowed = info;
    DiscoveryServer sAllowed;
    allowed.smartPingsOff = false;
    allowed.port = 27513;
    UT_ASSERT(sendServerAnswer(responder, &browserAddr, &allowed) == 0);
    memset(&sAllowed, 0, sizeof(sAllowed));
    UT_ASSERT(recvServer(browser, &sAllowed) == 0);
    UT_ASSERT_MSG(sAllowed.smartPingsOff == false,
                  "spingoff=0 read back as banned");
  }

  /* Positional sound off packs bit 4 clear, and the browser reads it back
     as off with the other view values unchanged. */
  {
    MdnsServerInfo soundOff = info;
    DiscoveryServer sSoundOff;
    soundOff.viewPolicies2 = infoPacketPackViewPolicies2(
        (uint8_t)overviewWindowClassic,
        (uint8_t)lineOfSightBuildingsAndTrees, false);
    soundOff.port = 27514;
    UT_ASSERT(sendServerAnswer(responder, &browserAddr, &soundOff) == 0);
    memset(&sSoundOff, 0, sizeof(sSoundOff));
    UT_ASSERT(recvServer(browser, &sSoundOff) == 0);
    UT_ASSERT_MSG(sSoundOff.positionalSound == false,
                  "bit 4 clear read back as positional sound on");
    UT_ASSERT(sSoundOff.overviewWindow == (uint8_t)overviewWindowClassic &&
              sSoundOff.lineOfSight == (uint8_t)lineOfSightBuildingsAndTrees);
  }

  /* A record from a server old enough to send no spingoff key at all must
     read as allowed, not as whatever the caller's buffer held. The parse
     seam is driven directly here because the advertiser always emits the
     key — there is no way to produce this record over the wire. */
  {
    DiscoveryMdnsResolved r;
    DiscoveryServer sOld;
    memset(&r, 0, sizeof(r));
    r.haveSrv = true;
    r.port    = 27500;
    snprintf(r.txt[0].key, sizeof(r.txt[0].key), "%s", "newp");
    snprintf(r.txt[0].value, sizeof(r.txt[0].value), "%s", "1");
    r.txtCount = 1;
    memset(&sOld, 0, sizeof(sOld));
    sOld.smartPingsOff = true;   /* the stale value a reuse could leave */
    UT_ASSERT(discoveryMdnsFillServer(&r, &sOld));
    UT_ASSERT_MSG(sOld.smartPingsOff == false,
                  "a record with no spingoff key read as banned; absent means "
                  "smart pings are allowed");
  }

  /* ---- unique instance label: distinct ports => distinct SRV owner name ---- */
  {
    MdnsServerInfo infoA = info, infoB = info;
    mdns_record_t ra[MDNS_WINBOLO_RECORD_COUNT], rb[MDNS_WINBOLO_RECORD_COUNT];
    char sa[256], sb[256];
    size_t na, nb;
    infoA.port = 27511;
    infoB.port = 27512;
    na = mdnsAdvertiseBuildRecords(&infoA, ra, MDNS_WINBOLO_RECORD_COUNT, sa, sizeof(sa));
    nb = mdnsAdvertiseBuildRecords(&infoB, rb, MDNS_WINBOLO_RECORD_COUNT, sb, sizeof(sb));
    UT_ASSERT(na > 0 && nb > 0);
    /* records[1] is the SRV record; its owner name is the instance label. */
    UT_ASSERT(ra[1].type == MDNS_RECORDTYPE_SRV && rb[1].type == MDNS_RECORDTYPE_SRV);
    UT_ASSERT(ra[1].name.length != rb[1].name.length ||
              memcmp(ra[1].name.str, rb[1].name.str, ra[1].name.length) != 0);

    /* ---- two-instance resolution over the wire ---- */
    {
      DiscoveryServer s1, s2;
      UT_ASSERT(sendServerAnswer(responder, &browserAddr, &infoA) == 0);
      UT_ASSERT(sendServerAnswer(responder, &browserAddr, &infoB) == 0);
      memset(&s1, 0, sizeof(s1));
      memset(&s2, 0, sizeof(s2));
      UT_ASSERT(recvServer(browser, &s1) == 0);
      UT_ASSERT(recvServer(browser, &s2) == 0);
      UT_ASSERT_MSG(s1.port != s2.port, "ports collided: %u", (unsigned)s1.port);
      UT_ASSERT((s1.port == 27511 && s2.port == 27512) ||
                (s1.port == 27512 && s2.port == 27511));
    }
  }

  closesocket(browser);
  closesocket(responder);
  bolo_net_cleanup();
  return 0;
}

/*
 * The platform advertiser's strings (mdnsAdvertiseTxtPairs): what iOS hands
 * to DNSServiceRegister. The instance label is the name the browsers read
 * the address and port from, the pairs are the builder's own TXT records,
 * and they read back through discoveryServerFromMdns, the parse the iOS
 * browser uses, to the server they came from.
 */
int run_mdns_platform_pairs(void) {
  MdnsServerInfo info;
  mdns_record_t records[MDNS_WINBOLO_RECORD_COUNT];
  char recScratch[512];
  char instance[96];
  const char *keys[MDNS_WINBOLO_TXT_COUNT];
  const char *values[MDNS_WINBOLO_TXT_COUNT];
  char scratch[512];
  size_t count, recCount, i, pair = 0;
  DiscoveryServer s;

  /* Windows reads an IPv6 literal with getaddrinfo, which needs Winsock. */
  bolo_net_init();
  memset(&info, 0, sizeof(info));
  info.port = 27510;
  UT_ASSERT(boloAddrParse("192.168.1.20", 0, &info.addr4));
  strncpy(info.mapName, "Everard Island", sizeof(info.mapName) - 1);
  info.versionMajor = 1;
  info.versionMinor = 2;
  info.versionRevision = 3;
  info.numPlayers = 4;
  info.numBases = 5;
  info.numPills = 6;
  info.password = true;
  info.lobby = true;
  strncpy(info.mapMd5Hex, "0123456789abcdef0123456789abcdef", sizeof(info.mapMd5Hex) - 1);
  info.allowNewPlayers = true;
  info.timeLimit = 3000;
  info.numHumans = 3;
  info.numBots = 1;
  info.maxPlayers = 12;
  info.viewPolicies = infoPacketPackViewPolicies(viewPolicyKey, viewPolicyAlways,
                                                 viewPolicyOff, true, true);
  info.viewPolicies2 = infoPacketPackViewPolicies2(
      (uint8_t)overviewWindowClassic, (uint8_t)lineOfSightBuildingsAndTrees, true);

  count = mdnsAdvertiseTxtPairs(&info, instance, sizeof(instance), keys, values,
                                MDNS_WINBOLO_TXT_COUNT, scratch, sizeof(scratch));
  UT_ASSERT_MSG(count == MDNS_WINBOLO_TXT_COUNT, "pairs=%u", (unsigned)count);
  UT_ASSERT_MSG(strcmp(instance, "WinBolo-192-168-1-20-27510") == 0,
                "instance='%s'", instance);

  /* The same keys and values, in the same order, as the records the
   * raw-socket advertiser sends. */
  recCount = mdnsAdvertiseBuildRecords(&info, records, MDNS_WINBOLO_RECORD_COUNT,
                                       recScratch, sizeof(recScratch));
  /* The info has no IPv6 address, so no AAAA record is built: one fewer
   * than the most an answer holds. */
  UT_ASSERT(recCount == MDNS_WINBOLO_RECORD_COUNT - 1);
  for (i = 0; i < recCount; i++) {
    if (records[i].type != MDNS_RECORDTYPE_TXT) {
      continue;
    }
    UT_ASSERT(pair < count);
    UT_ASSERT_MSG(strlen(keys[pair]) == records[i].data.txt.key.length &&
                  memcmp(keys[pair], records[i].data.txt.key.str,
                         records[i].data.txt.key.length) == 0,
                  "key %u is '%s'", (unsigned)pair, keys[pair]);
    UT_ASSERT_MSG(strlen(values[pair]) == records[i].data.txt.value.length &&
                  memcmp(values[pair], records[i].data.txt.value.str,
                         records[i].data.txt.value.length) == 0,
                  "value of '%s' is '%s'", keys[pair], values[pair]);
    pair++;
  }
  UT_ASSERT(pair == count);

  /* Read back the way the iOS browser reads a resolved instance. */
  memset(&s, 0, sizeof(s));
  UT_ASSERT(discoveryServerFromMdns("192.168.1.20", 27510, keys, values, count, &s));
  UT_ASSERT_MSG(strcmp(s.address, "192.168.1.20") == 0, "address='%s'", s.address);
  UT_ASSERT(s.port == 27510);
  UT_ASSERT(s.hasRichInfo == true);
  UT_ASSERT_MSG(strcmp(s.mapName, "Everard Island") == 0, "map='%s'", s.mapName);
  UT_ASSERT(s.versionMajor == 1 && s.versionMinor == 2 && s.versionRevision == 3);
  UT_ASSERT(s.numPlayers == 4 && s.numBases == 5 && s.numPills == 6);
  UT_ASSERT(s.password == true && s.inLobby == true);
  UT_ASSERT(s.numHumans == 3 && s.numBots == 1 && s.maxPlayers == 12);
  UT_ASSERT_MSG(s.timeLimit == 3000, "tlim=%d", (int)s.timeLimit);
  UT_ASSERT(s.hasViewInfo == true && s.pillView == viewPolicyKey &&
            s.overviewWindow == (uint8_t)overviewWindowClassic);

  /* An IPv6 literal reads back unchanged. */
  memset(&s, 0, sizeof(s));
  UT_ASSERT(discoveryServerFromMdns("2001:db8::20", 27510, keys, values, count, &s));
  UT_ASSERT_MSG(strcmp(s.address, "2001:db8::20") == 0, "address='%s'", s.address);
  UT_ASSERT(s.port == 27510);

  /* A link-local literal keeps its %scope. */
  {
    const char *loName = "";
    unsigned int scope = loopbackScope(&loName);
    if (scope == 0) {
      fprintf(stderr, "  mdns_platform_pairs: scoped literal skipped — no lo0 "
                      "or lo interface\n");
    } else {
      char scoped[64];
      BoloAddr parsed;
      snprintf(scoped, sizeof(scoped), "fe80::1%%%s", loName);
      memset(&s, 0, sizeof(s));
      UT_ASSERT_MSG(discoveryServerFromMdns(scoped, 27510, keys, values, count, &s),
                    "'%s' was refused", scoped);
      UT_ASSERT_MSG(boloAddrParse(s.address, 0, &parsed), "address='%s'", s.address);
      UT_ASSERT_MSG(boloAddrIsLinkLocal(&parsed), "address='%s'", s.address);
      UT_ASSERT_MSG(((const struct sockaddr_in6 *)&parsed.ss)->sin6_scope_id == scope,
                    "address='%s', %s is %u", s.address, loName, scope);
    }
  }

  /* A name is not an address. */
  memset(&s, 0, sizeof(s));
  UT_ASSERT(!discoveryServerFromMdns("winbolo.local", 27510, keys, values, count, &s));

  /* Too little room for the pairs is a failure, not a short list. */
  UT_ASSERT(mdnsAdvertiseTxtPairs(&info, instance, sizeof(instance), keys, values,
                                  MDNS_WINBOLO_TXT_COUNT - 1, scratch,
                                  sizeof(scratch)) == 0);
  UT_ASSERT(mdnsAdvertiseTxtPairs(&info, instance, 8, keys, values,
                                  MDNS_WINBOLO_TXT_COUNT, scratch,
                                  sizeof(scratch)) == 0);
  bolo_net_cleanup();
  return 0;
}
