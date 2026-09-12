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

#include <string.h>

#include "platform_net.h"     /* sockets / htons / inet_addr / bolo_net_init */
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
  info.addr.s_addr = inet_addr("127.0.0.1");
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
      (uint8_t)lineOfSightBuildingsAndTrees);

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
  /* The view key: seven values in four hex chars, over the real wire. */
  UT_ASSERT_MSG(s.pillView == viewPolicyKey, "pill=%d", (int)s.pillView);
  UT_ASSERT_MSG(s.baseView == viewPolicyAlways, "base=%d", (int)s.baseView);
  UT_ASSERT_MSG(s.allyView == viewPolicyOff, "ally=%d", (int)s.allyView);
  UT_ASSERT(s.classicMode == true);
  UT_ASSERT(s.alliesInTrees == true);
  UT_ASSERT_MSG(s.overviewWindow == (uint8_t)overviewWindowClassic,
                "window=%u", (unsigned)s.overviewWindow);
  UT_ASSERT_MSG(s.lineOfSight == (uint8_t)lineOfSightBuildingsAndTrees,
                "sight=%u", (unsigned)s.lineOfSight);

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
