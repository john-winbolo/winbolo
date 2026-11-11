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
 *Name:          mDNS Records
 *Filename:      mdns_records.h
 *Purpose:
 *  Socket-free builder for the _winbolo._udp.local mDNS
 *  answer record set (PTR / SRV / A / AAAA / TXT). Split out from
 *  the socket-owning advertiser so it can be unit-tested
 *  directly: given a snapshot of server info it fills an
 *  mdns_record_t array with no I/O.
 *********************************************************/

#ifndef MDNS_RECORDS_H
#define MDNS_RECORDS_H

#include "global.h"        /* BYTE / MAP_STR_SIZE / bool */
#include "gametype.h"      /* gameType */
#include "client_enums.h"  /* aiType */
#include "bolo_addr.h"     /* BoloAddr */
#include "mdns.h"         /* mdns_record_t and friends (header-only) */

/* DNS-SD service the host advertises on the LAN. RFC 6763 service type
 * under .local; the trailing dot is part of the wire name. */
#define MDNS_WINBOLO_SERVICE "_winbolo._udp.local."

/* TXT key schema — the producer side of the LAN-discovery contract.
 * Pass B's browser parses exactly these keys, so keep them short and
 * stable. Each maps to one TXT record:
 *   map=<name> ver=<maj.min.rev> players=<n> bases=<n> pills=<n>
 *   pass=<0|1> mines=<0|1> game=<gameType int> ai=<aiType int>
 *   lobby=<0|1> locked=<0|1>
 *   md5=<32 hex chars|empty> newp=<0|1> spingoff=<0|1> spec=<0|1>
 *   nspec=<n> ranked=<0|1> rnd=<0|1> tlim=<game length, 0 if none>
 *   humans=<n> bots=<n> max=<n> view=<4 hex chars>
 * The TXT count is fixed at MDNS_WINBOLO_TXT_COUNT. The address records
 * are not: an A record is written only when the host has an IPv4 address
 * and an AAAA record only when it has an IPv6 one. MDNS_WINBOLO_RECORD_COUNT
 * is the most records one answer holds (PTR + SRV + A + AAAA + the TXT
 * records); a host with one address family gets one fewer. */
#define MDNS_WINBOLO_TXT_COUNT 23
#define MDNS_WINBOLO_RECORD_COUNT (4 + MDNS_WINBOLO_TXT_COUNT)

/* Plain-data snapshot of the server state advertised in one answer. The
 * advertiser fills this from live server getters; the builder reads only
 * this POD (no sim / transport access), which is what keeps it testable. */
typedef struct {
  unsigned short port;     /* bound game port — goes in the SRV record */
  BoloAddr       addr4;    /* IPv4 LAN address for the A record; empty
                              when the host has none */
  BoloAddr       addr6;    /* IPv6 address for the AAAA record; empty when
                              the host has none */
  char           mapName[MAP_STR_SIZE];
  BYTE           versionMajor;
  BYTE           versionMinor;
  BYTE           versionRevision;
  BYTE           numPlayers;
  BYTE           numBases;
  BYTE           numPills;
  bool           password;
  bool           mines;
  gameType       game;
  aiType         ai;
  bool           lobby;    /* server in its lobby (pre-game) phase */
  bool           locked;   /* server locked / not accepting joins   */
  char           mapMd5Hex[33]; /* 32 hex chars + NUL; "" when random/unknown */
  bool           allowNewPlayers;
  bool           smartPingsOff;   /* host banned smart pings. Negative sense,
                                     so the zeroed POD and a record with no
                                     such key both read as "allowed" */
  bool           allowSpectators; /* future work — false for now */
  BYTE           spectatorCount;  /* future work — 0 for now */
  bool           ranked;
  bool           randomMap;
  int32_t        timeLimit;       /* game length; 0 if none */
  BYTE           numHumans;       /* humans among numPlayers */
  BYTE           numBots;         /* bots among numPlayers */
  BYTE           maxPlayers;      /* join-slot cap; MAX_TANKS when unset */
  /* The server's visibility rules, packed exactly as the INFO packet
   * carries them: the three view policies with the classic-mode and
   * allies-in-trees flags in one byte, the overview window and line of
   * sight in the other. Two bytes rather than seven fields, so the POD
   * carries what the wire carries. */
  BYTE           viewPolicies;    /* infoPacketPackViewPolicies output */
  BYTE           viewPolicies2;   /* infoPacketPackViewPolicies2 output */
} MdnsServerInfo;

/* Fill records[] with the PTR / SRV / A / AAAA / TXT answer set for info
 * and return the number of records written (0 when capacity is below
 * MDNS_WINBOLO_RECORD_COUNT or the scratch is too small). records[0] is the
 * PTR answer; records[1..] are the SRV, the A (only when info->addr4 is
 * set), the AAAA (only when info->addr6 is set) and the TXT records,
 * suitable as the "additional" array of an mDNS answer.
 *
 * The instance name is "WinBolo-<a>-<b>-<c>-<d>-<port>" from the IPv4
 * address when there is one, and "WinBolo-v6-<last 8 hex digits of the
 * IPv6 address, lower case>-<port>" when there is only an IPv6 address.
 *
 * The TXT record values point into txtScratch, so that buffer must stay
 * alive for as long as the returned records are used (i.e. until the
 * answer has been sent). No sockets, no allocation, no global state. */
size_t mdnsAdvertiseBuildRecords(const MdnsServerInfo *info,
                                 mdns_record_t *records, size_t capacity,
                                 char *txtScratch, size_t txtScratchSize);

/* The same answer as plain strings, for a platform advertiser that
 * registers the service itself (see mdnsAdvertiseSetPlatform): the instance
 * label alone, without the service suffix ("WinBolo-<a>-<b>-<c>-<d>-<port>"
 * or "WinBolo-v6-<8 hex digits>-<port>"),
 * written to instanceOut, and the TXT pairs as NUL-terminated keys and
 * values. Taken from mdnsAdvertiseBuildRecords' own records, so both
 * advertisers send one key schema. The values point into scratch, which
 * must outlive their use. Returns the number of pairs, 0 on failure. */
size_t mdnsAdvertiseTxtPairs(const MdnsServerInfo *info,
                             char *instanceOut, size_t instanceCap,
                             const char **keys, const char **values,
                             size_t capacity,
                             char *scratch, size_t scratchSize);

#endif /* MDNS_RECORDS_H */
