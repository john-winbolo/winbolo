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
 *Filename:      discovery.h
 *Purpose:
 *  Game discovery: tracker TCP queries, LAN UDP broadcast
 *  scanning, and per-server info ping. Surface uses POD
 *  result types — the wire-protocol INFO_PACKET never
 *  escapes this module.
 *********************************************************/

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "global.h"        /* BYTE / WORD / MAP_STR_SIZE / bool */
#include "gametype.h"      /* gameType */
#include "client_enums.h"  /* aiType */

/* currentGames is a typedef for struct currentGamesObj * in
 * src/gui/currentgames.h. discoveryFindTrackedGames only passes
 * it through by pointer, so we forward-declare the underlying
 * struct rather than reaching into the gui subtree. */
struct currentGamesObj;

/* Result of a single discoveryPingServer() call. rttMs is the round-trip
 * time in milliseconds when the function returns true; the rest of the
 * fields carry the server-reported counts. The version triple is the
 * server's build (from the INFO_RESPONSE header) — populated regardless
 * of whether it matches the client, so callers can pre-flight a join
 * against a mixed-version server. */
typedef struct {
  int  rttMs;
  WORD freePills;
  WORD freeBases;
  WORD numPlayers;
  BYTE versionMajor;
  BYTE versionMinor;
  BYTE versionRevision;
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
} DiscoveryServer;

/*********************************************************
 *NAME:          discoveryFindTrackedGames
 *PURPOSE:
 * Connects to a tracker server via TCP and downloads the
 * list of current games. Returns success.
 *
 *ARGUMENTS:
 *  cg             - Pointer to the currentGames structure
 *  trackerAddress - Address of the tracker to use
 *  port           - Port of the tracker
 *  motd           - Buffer to hold the message of the day
 *********************************************************/
bool discoveryFindTrackedGames(struct currentGamesObj **cg, char *trackerAddress, unsigned short port, char *motd);

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

/* Send an info request to a single server and wait up to 5 seconds for
 * a response. On success fills *out (rttMs >= 0) and returns true; on
 * timeout / DNS failure / send failure returns false with out->rttMs
 * set to -2 to mark "no answer". Self-contained — creates and tears
 * down its own UDP socket, safe to call from a worker thread. */
bool discoveryPingServer(const char *address, unsigned short port, DiscoveryPingResult *out);

#endif /* DISCOVERY_H */
