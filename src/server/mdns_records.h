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
 *Name:          mDNS Records
 *Filename:      mdns_records.h
 *Purpose:
 *  Socket-free builder for the _winbolo._udp.local mDNS
 *  answer record set (PTR / SRV / A / TXT). Split out from
 *  the socket-owning advertiser so it can be unit-tested
 *  directly: given a snapshot of server info it fills an
 *  mdns_record_t array with no I/O.
 *********************************************************/

#ifndef MDNS_RECORDS_H
#define MDNS_RECORDS_H

#include "global.h"        /* BYTE / MAP_STR_SIZE / bool */
#include "gametype.h"      /* gameType */
#include "client_enums.h"  /* aiType */
#include "mdns.h"          /* mdns_record_t and friends (header-only) */

/* DNS-SD service the host advertises on the LAN. RFC 6763 service type
 * under .local; the trailing dot is part of the wire name. */
#define MDNS_WINBOLO_SERVICE "_winbolo._udp.local."

/* TXT key schema — the producer side of the LAN-discovery contract.
 * Pass B's browser parses exactly these keys, so keep them short and
 * stable. Each maps to one TXT record:
 *   map=<name> ver=<maj.min.rev> players=<n> bases=<n> pills=<n>
 *   pass=<0|1> mines=<0|1> game=<gameType int> ai=<aiType int>
 *   lobby=<0|1> locked=<0|1>
 *   md5=<32 hex chars|empty> newp=<0|1> spec=<0|1> nspec=<n>
 *   ranked=<0|1> rnd=<0|1> tlim=<game length, 0 if none>
 *   humans=<n> bots=<n> max=<n>
 * Count is fixed at MDNS_WINBOLO_TXT_COUNT; with PTR + SRV + A that is
 * MDNS_WINBOLO_RECORD_COUNT records total. */
#define MDNS_WINBOLO_TXT_COUNT 21
#define MDNS_WINBOLO_RECORD_COUNT (3 + MDNS_WINBOLO_TXT_COUNT)

/* Plain-data snapshot of the server state advertised in one answer. The
 * advertiser fills this from live server getters; the builder reads only
 * this POD (no sim / transport access), which is what keeps it testable. */
typedef struct {
  unsigned short port;     /* bound game port — goes in the SRV record */
  struct in_addr addr;     /* server LAN address — goes in the A record */
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
  bool           allowSpectators; /* future work — false for now */
  BYTE           spectatorCount;  /* future work — 0 for now */
  bool           ranked;
  bool           randomMap;
  int32_t        timeLimit;       /* game length; 0 if none */
  BYTE           numHumans;       /* humans among numPlayers */
  BYTE           numBots;         /* bots among numPlayers */
  BYTE           maxPlayers;      /* join-slot cap; MAX_TANKS when unset */
} MdnsServerInfo;

/* Fill records[] with the PTR / SRV / A / TXT answer set for info and
 * return the number of records written (0 on insufficient capacity).
 * records[0] is the PTR answer; records[1..] are the SRV, A and TXT
 * records, suitable as the "additional" array of an mDNS answer.
 *
 * The TXT record values point into txtScratch, so that buffer must stay
 * alive for as long as the returned records are used (i.e. until the
 * answer has been sent). No sockets, no allocation, no global state. */
size_t mdnsAdvertiseBuildRecords(const MdnsServerInfo *info,
                                 mdns_record_t *records, size_t capacity,
                                 char *txtScratch, size_t txtScratchSize);

#endif /* MDNS_RECORDS_H */
