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
#include "../bolo/game_sim.h"  /* GAME_TICK_LENGTH */
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

typedef enum {
  SERVER_PORTMAP_DISABLED,    /* not requested (dedicated default,
                                 joiners, or BOLO_PORTMAP=OFF builds) */
  SERVER_PORTMAP_PENDING,     /* requested, libplum still working      */
  SERVER_PORTMAP_SUCCEEDED,   /* gateway accepted; externalIp/Port set */
  SERVER_PORTMAP_FAILED,      /* deadline elapsed without a mapping    */
  SERVER_PORTMAP_HOLE_PUNCH_OK,    /* libplum failed but tracker probe
                                      round-trips — joiners on most
                                      networks can still connect via
                                      hole-punching */
  SERVER_PORTMAP_SYMMETRIC_NAT     /* libplum succeeded but the tracker
                                      sees a different reflexive address
                                      than libplum reported — symmetric
                                      NAT layer means joiners cannot
                                      hole-punch through */
} ServerPortmapStatus;

typedef struct {
  ServerPortmapStatus status;
  char           externalIp[64];   /* "" unless SUCCEEDED */
  unsigned short externalPort;     /* 0  unless SUCCEEDED */
  unsigned short internalPort;     /* the host's bound UDP port (for UI) */
} ServerPortmapInfo;

/* Snapshot the current port-mapping status.  Takes the threading mutex
 * briefly so the caller never sees a half-written externalIp from
 * libplum's worker thread. */
void serverInstanceGetPortmapInfo(ServerPortmapInfo *out);

/* Whether the running server instance is firing NAT-keepalive "punch"
 * packets at the public tracker (i.e. the equivalent of NOT passing
 * -no-natpunch). FALSE when no instance is running, when keepalive is
 * disabled via gameFront / CLI override, or when the tracker itself
 * is off. Read by the lobby UI so it can hide the "Checking server
 * reachability..." badge for LAN hosts and other no-punch configs. */
bool serverInstanceIsNatPunchActive(void);

/* Called from the recv path when a PACKET_PUNCH_PROBE_REPLY arrives.
 * The reflexive address is what the tracker sees as our external
 * IP:port — used to detect symmetric NAT and confirm bidirectional
 * reachability. Takes the threading mutex briefly to update state
 * read by serverInstanceGetPortmapInfo. */
void serverInstanceRecordProbeReply(const char *reflexiveIp,
                                    unsigned short reflexivePort);

typedef enum {
  MANUAL_PROBE_IDLE,         /* never run, or reset on shutdown */
  MANUAL_PROBE_IN_PROGRESS,  /* probe sent, waiting for reply */
  MANUAL_PROBE_SUCCESS,      /* reply arrived within timeout */
  MANUAL_PROBE_TIMEOUT       /* no reply within ~2 s */
} ManualProbeState;

/* Trigger a fresh punch probe to the tracker on the next tick. The
 * lobby's "Test connectivity" button uses this to force an immediate
 * reachability check rather than waiting for the periodic probe. */
void serverInstanceTriggerManualProbe(void);

/* Snapshot the manual probe's current state. Returns IDLE when no
 * manual test has run since startup or shutdown. */
ManualProbeState serverInstanceGetManualProbeState(void);

/* Last tick / EWMA wall-clock in ms. Both 0 until the first tick has
 * been recorded. Producer-thread only — caller must hold the server
 * mutex; reads file-statics without synchronisation. */
void serverLifecycleGetTickStats(double *outLastMs, double *outEwmaMs);

/* Last serverSimTick × 2 / EWMA wall-clock in ms. Both 0 until the
 * first running-state tick has been recorded. Producer-thread only —
 * same locking rules as serverLifecycleGetTickStats. */
void serverLifecycleGetSimStats(double *outLastMs, double *outEwmaMs);

/* Feed the EWMA with the wall-clock cost of the tick that just
 * completed. Producer-thread only — called from inside
 * serverInstanceTick after the final mutex release. */
void serverLifecycleRecordTickMs(double ms);

#endif /* SERVER_LIFECYCLE_H */
