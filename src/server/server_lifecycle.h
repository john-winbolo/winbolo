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

#include "global.h"  /* GAME_TICK_LENGTH */
#include "server_sim.h"
#include "server_voice_mode.h"  /* ServerVoiceMode — the voiceMode field below */
#include "upload_policy.h"

#define SERVER_TICK_LENGTH (GAME_TICK_LENGTH * 2)

/* Override the operator-controlled upload policies and storage caps.
 * Called once at startup after transportUdpServerCreate. A maxFiles or
 * maxStorageBytes value of 0 leaves that cap at the create-time default
 * (64 files / 8 MiB) — lets the GUI host-and-play path use ServerInstanceConfig
 * zero-init without explicit values. policy is always applied (0 = ALLOW).
 * persistDir is the absolute directory PERSIST-policy uploads are written to;
 * NULL or "" leaves it unset (writes fall back to "<mapDirRoot>/Uploads").
 * The script* arguments are the same for player scripts: scriptPolicy is
 * always applied (0 = ALLOW), a scriptMaxFiles or scriptMaxStorageBytes of 0
 * keeps the create-time default (32 files / 64 MiB). */
void transportUdpServerSetUploadConfig(UploadPolicy policy,
                                       uint8_t maxFiles,
                                       uint32_t maxStorageBytes,
                                       const char *persistDir,
                                       ScriptUploadPolicy scriptPolicy,
                                       uint8_t scriptMaxFiles,
                                       uint32_t scriptMaxStorageBytes);

typedef struct {
  unsigned short udpPort;
  const char    *bindAddr;        /* "" or NULL = INADDR_ANY */
  const char    *password;        /* "" or NULL = no password */
  BYTE           maxPlayers;
  BYTE           maxBots;          /* cap on AI bots addable in the lobby;
                                      0 = no cap */
  BYTE           maxSpectators;    /* cap on spectator connections; 0 = spectating disabled */
  uint16_t       specDelaySeconds; /* spectator view delay in seconds; 0 = live (no floor) */

  bool           acceptRemoteClients; /* false = skip UDP bind, WBN, tracker
                                         and NAT portmap setup; serverInstanceTick
                                         short-circuits per-tick UDP send/drain;
                                         serverInstanceShutdown skips matching
                                         teardown. */

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

  bool           mdnsAdvertise;   /* advertise the game on the LAN via mDNS
                                     (_winbolo._udp.local). Listen-server
                                     hosts set true; dedicated defaults
                                     false (opt in with -mdns). */

  ServerVoiceMode voiceMode;      /* the voice mode the server runs.
                                     serverVoiceOff forwards no voice at all —
                                     the server still drains each client's
                                     voice ring so a client that sends anyway
                                     cannot stall on it. serverVoiceProximity
                                     is not implemented and forwards like
                                     serverVoiceOn. Zero-init =
                                     serverVoiceOn; the dedicated server sets
                                     serverVoiceOff from -no-voice. */

  /* What the game finder shows for this server in place of its address:
   * a short name and a longer description. NULL or "" = none, and the
   * finder falls back to the address. Sanitised and cut to
   * SERVER_NAME_MAX / SERVER_DESC_MAX bytes by the sim's setters. */
  const char    *serverName;
  const char    *serverDescription;

  /* Operator-controlled handling for client-pushed map uploads.
   * Zero-init = ALLOW + transport defaults (64 files / 8 MiB), so the GUI
   * host-and-play path needs no explicit plumbing. */
  UploadPolicy   uploadPolicy;
  uint8_t        uploadMaxFiles;        /* 0 = leave transport default (64) */
  uint32_t       uploadMaxStorageBytes; /* 0 = leave transport default (8 MiB) */

  /* Absolute directory for PERSIST-policy uploaded maps. NULL = the
   * built-in "<mapDirRoot>/Uploads". A GUI host points this under the
   * prefs path since its maps root is a read-only bundle. */
  const char    *uploadPersistDir;

  /* Operator-controlled handling for player-uploaded scripts.
   * Zero-init = ALLOW + transport defaults (32 files / 64 MiB). */
  ScriptUploadPolicy scriptUploadPolicy;
  uint8_t        scriptUploadMaxFiles;        /* 0 = leave transport default (32) */
  uint32_t       scriptUploadMaxStorageBytes; /* 0 = leave transport default (64 MiB) */

  /* Absolute directory for PERSIST-policy uploaded scripts. NULL = the
   * built-in "<mapDirRoot>/Uploads/Scripts". */
  const char    *scriptUploadDir;
  bool           noScriptSharing; /* refuse players' requests for script copies; zero-init shares */

  /* Directory ALLOW-policy uploaded scripts land in for the session, emptied
   * at startup, at shutdown and when the lobby resets. NULL = the built-in
   * "<mapDirRoot>/Uploads/Session-<udpPort>", one per server on a shared map
   * root. A GUI host points this under the prefs path, as it does
   * uploadPersistDir. */
  const char    *scriptSessionDir;

  /* Initial state + lobby/per-sim toggles applied by serverInstanceStartup.
   * Zero-init means "don't touch what serverSimCreate* set" for the lobby
   * branch, and "match the new-cfg defaults (false / 0 / NULL / aiNone)"
   * for the rest. lobbyEnabled and skipLobby are mutually exclusive;
   * skipLobby wins if both are set. */
  bool           lobbyEnabled;        /* true → host wants a lobby. Joiners
                                       * follow whatever phase the server
                                       * reports. */
  bool           skipLobby;           /* true → enter running state directly
                                       * (tutorial, gym, bg_game, braintest,
                                       * headless --fast). Mutually exclusive
                                       * with lobbyEnabled. */
  bool           emptyResetEnabled;   /* serverSimSetEmptyResetEnabled */
  bool           hasPassword;         /* serverSimSetHasPassword */
  const char    *botBrainPath;        /* serverSimSetBotBrainPath; NULL =
                                       * leave unset */
  BYTE           botAiType;           /* serverSimSetBotAiType; aiNone =
                                       * leave unset */
  bool           autoLockOnGameStart; /* serverSimSetAutoLockOnGameStart */
  bool           ranked;              /* serverSimSetRanked. ranked forces
                                       * autoLockOnGameStart inside startup. */
  bool           openHost;            /* serverSimSetOpenHost */
  uint32_t       serverLocks;         /* serverSimSetServerLocks bitmask */
  BYTE           viewPlayer;          /* sim->sim.viewPlayer at startup —
                                       * SP/host/headless designate which
                                       * slot the in-process renderer
                                       * watches. Zero-init = slot 0, the
                                       * SP convention. */
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

/* Live delayed-stream spectator ring for this instance. The full type lives in
 * internal/spectator_ring.h (T2 sim-internal); only a pointer is exposed here
 * so this header's wider consumers need not pull that header in. */
struct SpectatorRing;

/* Create the live ring and register it with the log tap (serverInstanceStartup
 * calls this for a real MP host once the spectator cap / delay are set). No-op
 * leaving the ring NULL when spectating is disabled (maxSpectators == 0). */
void serverInstanceCreateSpectatorRing(ServerSim *sim);

/* Unregister the tap then free the ring (serverInstanceShutdown calls this).
 * NULL-safe and idempotent. */
void serverInstanceDestroySpectatorRing(void);

/* The live ring handle, or NULL when spectating is disabled / no server is up.
 * For the serve path (2d-c+) and tests. */
struct SpectatorRing *serverInstanceGetSpectatorRing(void);

/* Round-end log-upload hooks for WinBoloDS. Only the dedicated-server
 * binary ships server_dedicated_log.c (it touches servermain.c-owned
 * globals and the http stack); the other server_static consumers
 * (WinBolo, WinBoloHeadless, winbolo_gym, WinBoloUnitTests) leave
 * these unset and the lifecycle treats them as no-ops. WinBoloDS
 * registers serverDedicatedLogStashCurrentRound /
 * serverDedicatedLogFlushPendingUpload via serverDedicatedLogInstall
 * at startup. */
void serverLifecycleSetRoundLogHooks(void (*stash)(void),
                                     void (*flush)(void));

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

/* The worst tick recorded since the last reset, in ms, and how many ticks
 * cost at least SERVER_TICK_LENGTH ms. Both 0 until the first tick has been
 * recorded, and both describe the round in progress — the round start clears
 * them. They survive where the EWMA does not: a few expensive frames decay
 * out of the average long before anyone asks the server what happened, while
 * these can be read once the round is over. Producer-thread only — same
 * locking rules as serverLifecycleGetTickStats. */
void serverLifecycleGetTickPeak(double *outPeakMs,
                                unsigned int *outOverBudget);

/* Drop the peak and the over-budget count so they describe the round that is
 * starting rather than an accumulation across map rotations. Called from the
 * two authoritative round starts. */
void serverLifecycleResetTickPeak(void);

/* Feed the EWMA with the wall-clock cost of the tick that just
 * completed. Producer-thread only — called from inside
 * serverInstanceTick after the final mutex release. */
void serverLifecycleRecordTickMs(double ms);

/* A single catch-up burst that runs more owed ticks than this leaves one
 * warning line in the log. Five ticks is 100 ms of debt, the point at which
 * a slot that has fallen behind stops recovering on its own. */
#define SERVER_HITCH_WARN_TICKS 5

/* Runs one owed tick. Returns false to stop the catch-up immediately —
 * the shutdown handshake uses this. */
typedef bool (*ServerTickStepFn)(void *ctx);

/* Runs the ticks the wall clock owes, advancing *oldTick by
 * SERVER_TICK_LENGTH and *ticks by one per tick run. Returns how many ran.
 *
 * The debt is (nowMs - *oldTick) and a tick is owed while that is strictly
 * greater than SERVER_TICK_LENGTH, so the burst stops with one tick length
 * still on the clock. Uncapped on purpose: the server's tick clock stays
 * aligned with wall time, which every client's input counter assumes.
 * A burst longer than SERVER_HITCH_WARN_TICKS logs one line carrying the
 * debt, the tick count and the slowest single step.
 *
 * ticks may be NULL; oldTick and step may not. */
uint32_t serverTickCatchUp(uint32_t nowMs, uint32_t *oldTick, uint32_t *ticks,
                           ServerTickStepFn step, void *ctx);

#endif /* SERVER_LIFECYCLE_H */
