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
 *Name:          Server Lifecycle
 *Filename:      server_lifecycle.h
 *Author:        John Morrison
 *Purpose:
 *  Shared startup / per-tick / shutdown for the dedicated
 *  server and the in-process GUI host. Single instance per
 *  process.
 *********************************************************/

#ifndef SERVER_LIFECYCLE_H
#define SERVER_LIFECYCLE_H

#include "../bolo/global.h"
#include "../bolo/screen.h"
#include "server_sim.h"

#define SERVER_TICK_LENGTH (GAME_TICK_LENGTH * 2)

typedef struct {
  unsigned short udpPort;
  const char    *bindAddr;        /* "" or NULL = INADDR_ANY */
  const char    *password;        /* "" or NULL = no password */
  BYTE           maxPlayers;

  bool           useWbn;          /* false = skip winbolonetCreateServer */
  BYTE           compTanks;       /* AI type — only used when useWbn */

  bool           useTracker;      /* false = skip tracker periodic update */
  const char    *trackerAddr;     /* required if useTracker */
  unsigned short trackerPort;     /* required if useTracker */

  bool           useNatKeepalive; /* fire transportUdpServerSendNatKeepalive
                                     on the tracker connection ~every 25s,
                                     to keep the host's NAT mapping alive
                                     for tracker push-back. */

  bool           useNatPortmap;   /* request UPnP/NAT-PMP/PCP port mapping
                                     via libplum on startup, release on
                                     shutdown.  Hosted MP sets true;
                                     dedicated defaults false (admins
                                     control routers). */
} ServerInstanceConfig;

/* Bind UDP transport, optionally register with WBN, store tracker config
 * for use by serverInstanceTick. The sim must already be created (via
 * serverSimCreate / serverSimCreateRandomMap / serverSimCreateCompressed)
 * with all desired fields set: lobbyEnabled, emptyResetEnabled,
 * autoCloseOnEmpty, hasPassword, mapDirFiles, randomMapEnabled,
 * randomMapConfig, etc. Returns false on UDP bind failure; caller still
 * owns the sim and is responsible for serverSimDestroy. */
bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg);

/* One periodic slice: receive packets, run sim tick(s), drain events,
 * send snapshots, broadcast lobby/state-transition packets, run
 * auto-close + empty-reset checks, and run WBN/tracker periodic
 * updates when their thresholds elapse. Acquires the threading mutex
 * internally to match servermain.c's existing pattern.
 *
 * Caller drives the cadence: dedicated server calls in a 20 ms
 * catch-up loop from its timer callback; the GUI host (commit 3)
 * will call once per 20 ms tick of its own loop. */
void serverInstanceTick(ServerSim *sim);

/* Reverse of startup: stop UDP recv thread (sends PACKET_SERVER_SHUTDOWN
 * to all connected clients), winbolonetDestroy(TRUE) if WBN was active,
 * botManagerDestroy(sim). Does NOT destroy or free sim — caller owns. */
void serverInstanceShutdown(ServerSim *sim);

#endif /* SERVER_LIFECYCLE_H */
