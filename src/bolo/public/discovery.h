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
 *Filename:      discovery.h
 *Purpose:
 *  Game discovery: LAN UDP broadcast and mDNS scanning,
 *  and per-server info ping. Surface uses POD result
 *  types — the wire-protocol INFO_PACKET never escapes
 *  this module.
 *********************************************************/

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "global.h"        /* BYTE / WORD / MAP_STR_SIZE / bool */
#include "gametype.h"      /* gameType */
#include "client_enums.h"  /* aiType */
#include "view_policy.h"   /* ViewPolicy — the advertised visibility rules */
#include "server_voice_mode.h"  /* ServerVoiceMode — the advertised voice mode */

/* Result of a single discoveryPingServer() call. rttMs is the round-trip
 * time in milliseconds when the function returns true; the rest of the
 * fields carry the server-reported counts. The version triple is the
 * server's build (from the INFO_RESPONSE header) — populated regardless
 * of whether it matches the client, so callers can pre-flight a join
 * against a mixed-version server. The remaining fields mirror the rich
 * game-info the INFO_PACKET now carries: map md5, lobby/lock/join state,
 * ranked/random-map flags, the human/bot split, and the time limit. */
typedef struct {
  int  rttMs;
  WORD freePills;
  WORD freeBases;
  WORD numPlayers;
  BYTE versionMajor;
  BYTE versionMinor;
  BYTE versionRevision;
  char mapMd5[33];
  bool allowNewPlayers;
  bool locked;
  bool inLobby;
  bool allowSpectators;
  BYTE spectatorCount;
  bool ranked;
  bool randomMap;
  BYTE numHumans;
  BYTE numBots;
  BYTE maxPlayers;  /* server's join-slot cap; MAX_TANKS when unset */
  int32_t timeLimit;
  bool hasRichInfo;  /* true when the rich INFO (flags/counts/md5) was
                      * received; false for legacy 76-byte servers — consumers
                      * hide the rich fields when false. */
  /* Whether the sender said anything at all about its visibility rules.
   * The fields above always hold something, because a packet that stops
   * short of them is read as the back-compatibility set; this says
   * whether that reading is a real answer or a stand-in, so a caller
   * naming the rules can say "not reported" rather than name a set the
   * server never chose. */
  bool hasViewInfo;
  /* Server visibility rules. A server whose INFO predates the
   * view_policies byte reports the defaults (pill always, base off,
   * ally always) with classic mode and allies in trees both off, and one
   * whose INFO predates view_policies2 reports the expanded overview
   * window with nothing blocking sight inside it. */
  ViewPolicy pillView;
  ViewPolicy baseView;
  ViewPolicy allyView;
  bool classicMode;
  bool alliesInTrees;
  uint8_t overviewWindow;  /* OverviewWindow the server advertises */
  uint8_t lineOfSight;     /* LineOfSightMode the server advertises */
  /* Voice the server forwards. A server whose INFO predates the flag bits
   * reports serverVoiceOn, which is what it does. */
  ServerVoiceMode voiceMode;
} DiscoveryPingResult;

/* A server discovered via LAN broadcast. Plain data — no wire-format
 * structures, no socket handles. Filled in by discoveryFindBroadcastGamesAsync
 * before each callback invocation. */
typedef struct {
  char           address[256];
  unsigned short port;
  char           mapName[MAP_STR_SIZE];
  BYTE           versionMajor;
  BYTE           versionMinor;
  BYTE           versionRevision;
  BYTE           numPlayers;
  BYTE           numBases;
  BYTE           numPills;
  bool           mines;
  gameType       game;
  aiType         ai;
  bool           password;
  bool           inLobby;  /* server is in its lobby (pre-game) phase.
                            * Populated by both the broadcast/ping INFO
                            * flags byte and the mDNS producer. */
  bool           locked;   /* server is locked / not accepting joins.
                            * Populated by both the broadcast/ping INFO
                            * flags byte and the mDNS producer. */
  char           mapMd5[33];      /* 32 hex chars + NUL; "" when random/unknown */
  bool           allowNewPlayers;
  bool           allowSpectators;
  BYTE           spectatorCount;
  bool           ranked;
  bool           randomMap;
  BYTE           numHumans;
  BYTE           numBots;
  BYTE           maxPlayers;      /* server's join-slot cap; MAX_TANKS when unset */
  int32_t        timeLimit;       /* raw game-length units from the wire; 0 if none */
  bool           hasRichInfo;     /* true when the rich INFO (flags/counts/md5)
                                   * was received; false for legacy 76-byte servers
                                   * — consumers hide the rich fields when false. */
  /* Whether the sender said anything at all about its visibility rules.
   * The fields above always hold something, because a packet that stops
   * short of them is read as the back-compatibility set; this says
   * whether that reading is a real answer or a stand-in, so a caller
   * naming the rules can say "not reported" rather than name a set the
   * server never chose. */
  bool hasViewInfo;
  /* Server visibility rules. A server whose INFO predates the
   * view_policies byte reports the defaults (pill always, base off,
   * ally always) with classic mode and allies in trees both off, and one
   * whose INFO predates view_policies2 reports the expanded overview
   * window with nothing blocking sight inside it. */
  ViewPolicy     pillView;
  ViewPolicy     baseView;
  ViewPolicy     allyView;
  bool           classicMode;
  bool           alliesInTrees;
  uint8_t        overviewWindow;  /* OverviewWindow the server advertises */
  uint8_t        lineOfSight;     /* LineOfSightMode the server advertises */
  /* Voice the server forwards. A server whose INFO predates the flag bits
   * reports serverVoiceOn, which is what it does. */
  ServerVoiceMode voiceMode;
  /* Host banned smart pings. Carried by the mDNS record only ("spingoff");
   * the broadcast INFO packet has no room for it and leaves this false.
   * Negative sense, so false — what both an old record and a server that
   * never mentions it give — means smart pings are ALLOWED. */
  bool           smartPingsOff;
} DiscoveryServer;

/* Callback delivered for each LAN server that responds to a broadcast
 * search. The DiscoveryServer pointer is valid only for the duration
 * of the call; copy what you need. userData is passed through unchanged. */
typedef void (*DiscoveryServerCallback)(const DiscoveryServer *server, void *userData);

/* Asynchronous LAN broadcast search. Sends an info request on each
 * usable interface and invokes callback for every valid response that
 * arrives within the scan window (~5s). Returns true if the broadcast
 * was sent successfully. */
bool discoveryFindBroadcastGamesAsync(DiscoveryServerCallback callback, void *userData);

/* Signal the in-flight discoveryFindBroadcastGamesAsync (if any) to
 * abort its 5-second poll window early. Safe to call from any thread;
 * the search loop checks the flag every 50ms. The flag is auto-cleared
 * at the start of the next discoveryFindBroadcastGamesAsync, so a stale
 * set from a prior session doesn't shortcut a fresh search. */
void discoveryAbortBroadcastSearch(void);

/* Asynchronous LAN mDNS search. Opens an ephemeral mDNS socket, sends a
 * PTR query for the _winbolo._udp.local service, and for ~5s resolves each
 * responding host into a DiscoveryServer (including the inLobby/locked
 * fields the broadcast/tracker paths can't report) and invokes callback.
 * Returns true if the query was sent successfully. Self-contained — owns
 * its own socket, safe to run on a worker thread alongside the broadcast
 * search (both can feed the same callback). */
bool discoveryFindMdnsGamesAsync(DiscoveryServerCallback callback, void *userData);

/* Signal the in-flight discoveryFindMdnsGamesAsync (if any) to abort its
 * poll window early. Mirrors discoveryAbortBroadcastSearch: safe from any
 * thread, auto-cleared at the start of the next mDNS search. */
void discoveryAbortMdnsSearch(void);

/* Send an info request to a single server and wait up to 5 seconds for
 * a response. On success fills *out (rttMs >= 0) and returns true; on
 * timeout / DNS failure / send failure returns false with out->rttMs
 * set to -2 to mark "no answer". Self-contained — creates and tears
 * down its own UDP socket, safe to call from a worker thread. */
bool discoveryPingServer(const char *address, unsigned short port, DiscoveryPingResult *out);

#endif /* DISCOVERY_H */
