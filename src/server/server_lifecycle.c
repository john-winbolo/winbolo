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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#ifdef _WIN32
/* WIN32_LEAN_AND_MEAN keeps windows.h from pulling the legacy winsock.h, which
 * would clash with the winsock2.h the transport headers below include. We only
 * need fileapi.h (GetDiskFreeSpaceEx) for the recording disk-space guard. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

#include "global.h"
#include "control_event.h"
#include "gametype.h"
#include "nat_portmap.h"
#include "transport_udp.h"
#include "mdns_advertise.h"
#include "bot_manager.h"
#include "brain_record.h"
#include "../winbolonet/winbolonet_core.h"
#include "../common/mp_diag_log.h"
#include "../common/wb_log.h"
#include "../winbolonet/winbolonet_server.h"
#include "../winbolonet/winbolonetthread.h"
#include "threads.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"
#include "spectator_ring.h"
#include "log_internal.h"   /* logSetSpectatorRing */

/* Round-end log hooks installed by WinBoloDS via
 * serverLifecycleSetRoundLogHooks. NULL on every other binary that
 * links server_static, so the lifecycle's stash/flush calls become
 * no-ops there. */
/* Adapter: serverSimEmitBrainAnnounces hands each event to a deliver
 * callback; the return-to-lobby path wants them on the broadcast bus. */
static void serverSimPublishBrainAnnounceCb(void *ctx, const ControlEvent *evt) {
  serverSimPublishControl((ServerSim *)ctx, evt);
}

static void (*s_roundLogStash)(void) = NULL;
static void (*s_roundLogFlush)(void) = NULL;

void serverLifecycleSetRoundLogHooks(void (*stash)(void),
                                     void (*flush)(void)) {
  s_roundLogStash = stash;
  s_roundLogFlush = flush;
}

static void roundLogStash(void) {
  if (s_roundLogStash != NULL) s_roundLogStash();
}

static void roundLogFlush(void) {
  if (s_roundLogFlush != NULL) s_roundLogFlush();
}

/*********************************************************
*NAME:          serverLifecycleRegisterTail
*PURPOSE:
* The work that follows a successful server/register, on the
* tick: close the rotation window, push the lobby change
* that was held dirty while it was open, hand every
* WBN-participating client the new key, and tell the new
* session about the lock the old one was told about.
*
* Run from the register's result handler for a queued
* register, and straight after the call for a synchronous
* one.
*********************************************************/
static void serverLifecycleRegisterTail(ServerSim *sim) {
  /* First: everything below reads the key through the window. */
  sim->wbnSessionRotating = FALSE;
  serverSimWbnLobbyTick(sim);
  transportUdpServerBroadcastWbnRekey(sim);
  /* The new session knows nothing about the lock the old one was told
   * about. An idempotent state push, and it carries no player key. */
  winboloNetSendLock(transportUdpServerGetLock());
}

/*********************************************************
*NAME:          serverLifecycleQueueRotation
*PURPOSE:
* Queues one WinBolo.net session rotation for the worker:
* server/quit for the round that ended, the round-log
* upload, then server/register for the next round, in that
* order, which is the order the tracker needs. The three
* sites that end a round call this; none of them sends
* anything on the tick.
*
* Only one register may be outstanding. If the last one has
* not answered yet, nothing is queued and the rotation is
* marked deferred: serverLifecycleWbnResult runs it when that
* result lands, so the quit names the session the result
* installs rather than the one before it, and the upload sits
* between that quit and the next register. The stash the
* caller made stays pending until then.
*
* With WinBolo.net off, the upload flush still runs (it is a
* no-op with nothing pending) and the window closes here,
* since no result is coming.
*
* When the worker refuses the quit it is not running at all
* (winbolonetThreadCreate failed at boot, and nothing
* recreates it), so the register behind it would be refused
* too and the round would end with no quit, no upload and no
* register - the finished session listed on WinBolo.net for
* good. The three then go out on this thread instead, in the
* same order. That is the blocking round transition this
* server otherwise never does, and it is the lesser cost:
* with no worker there is no later moment to send them.
*********************************************************/
static void serverLifecycleQueueRotation(ServerSim *sim) {
  uint32_t registerJob = 0;

  if (winbolonetIsRunning() && sim->wbnRegisterJob != 0) {
    sim->wbnRotateDeferred = TRUE;
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "WinBolo.net rotation deferred: register job %u is still "
                "outstanding", (unsigned)sim->wbnRegisterJob);
    return;
  }

  if (!winbolonetIsRunning()) {
    /* Nothing to rotate. The flush is a no-op with nothing pending, and
     * with WinBolo.net off it goes nowhere in any case. */
    roundLogFlush();
    sim->wbnRegisterJob = 0;
    sim->wbnSessionRotating = FALSE;
    return;
  }

  if (winbolonetQueueEndSession() != TRUE) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "WinBolo.net worker refused the session quit; sending the "
                "round transition on this thread");
    /* The quit first, so the upload that follows is accepted: WinBolo.net
     * refuses a round log for a session that is still live. The flush hook
     * is the queued one, and its own enqueue is refused the same way, so it
     * posts the log from here as well. */
    winbolonetEndSession(/*drainMaxMs*/ 0);
    roundLogFlush();
    serverSimRefreshWbnLobbyInfo(sim);
    sim->wbnRegisterJob = 0;
    if (winbolonetBeginSession(
          sim->mapName, sim->serverPort,
          (BYTE)gameTypeGet(&sim->sim.game),
          (BYTE)sim->botAiType,
          (BYTE)sim->sim.hiddenMines,
          sim->hasPassword,
          basesGetNumActive(&sim->sim.bs),
          pillsGetNumActive(&sim->sim.pb),
          serverSimGetNumNeutralBases(sim),
          serverSimGetNumNeutralPills(sim),
          serverSimGetNumPlayers(sim)) == TRUE) {
      serverLifecycleRegisterTail(sim);
    } else {
      /* winbolonetBeginSession has switched WinBolo.net off and cleared the
       * bearer, as a failed queued register does. Close the window anyway. */
      sim->wbnSessionRotating = FALSE;
    }
    return;
  }

  roundLogFlush();
  serverSimRefreshWbnLobbyInfo(sim);
  registerJob = winbolonetQueueBeginSession(
    sim->mapName, sim->serverPort,
    (BYTE)gameTypeGet(&sim->sim.game),
    (BYTE)sim->botAiType,
    (BYTE)sim->sim.hiddenMines,
    sim->hasPassword,
    basesGetNumActive(&sim->sim.bs),
    pillsGetNumActive(&sim->sim.pb),
    serverSimGetNumNeutralBases(sim),
    serverSimGetNumNeutralPills(sim),
    serverSimGetNumPlayers(sim));
  sim->wbnRegisterJob = registerJob;
  if (registerJob == 0) {
    sim->wbnSessionRotating = FALSE;
  }
}

/*********************************************************
*NAME:          serverLifecycleWbnResult
*PURPOSE:
* Handles one WinBolo.net job result, on the tick thread,
* out of winbolonetThreadDrainResults. Two kinds have a
* result: the round transition's server/register, handled
* below, and a re-authenticating client's client/verify,
* which the server transport places by job id.
*
* The register tail here is what the tick used to run
* straight after winbolonetBeginSession returned. It waits
* for the register because all of it depends on the new
* session key: the held lobby_update would name the finished
* round, and the rekey would hand clients a key that is about
* to be replaced.
*********************************************************/
static void serverLifecycleWbnResult(uint32_t id, uint8_t kind, int status,
                                     const char *response, void *ctx) {
  ServerSim *sim = (ServerSim *)ctx;

  if (sim == NULL) {
    return;
  }

  if (kind == WBN_JOB_VERIFY) {
    udpServerApplyReauthResult(sim, id, status, response);
    return;
  }

  if (kind != WBN_JOB_REGISTER) {
    return;
  }

  if (sim->wbnRegisterJob != 0 && id != sim->wbnRegisterJob) {
    /* Not the register this server is waiting on. Only one is ever out,
     * so this is a result from before a restart of the transport; the key
     * it carries is not the one the next rotation quits. */
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "WinBolo.net register result %u ignored: waiting on %u",
                (unsigned)id, (unsigned)sim->wbnRegisterJob);
    return;
  }
  sim->wbnRegisterJob = 0;

  if (winbolonetApplyRegisterResult(status, response) == TRUE) {
    if (sim->wbnRotateDeferred) {
      /* A round ended while this register was out. Its session is the one
       * just installed, so quit it now, upload that round's log, and
       * register the round the server is on. The window stays open until
       * that register answers; the rekey and the lock re-send below are
       * for a key that would be replaced straight away. */
      sim->wbnRotateDeferred = FALSE;
      serverLifecycleQueueRotation(sim);
      if (sim->wbnRegisterJob != 0) {
        return;
      }
    }
    /* New key and bearer are installed: close the window, push the held
     * lobby change, rekey every participating client and re-send the lock. */
    serverLifecycleRegisterTail(sim);
  } else {
    /* The register failed, so winbolonetApplyRegisterResult has switched
     * WinBolo.net off and cleared the bearer. Close the window anyway — a
     * server stuck rotating holds every later lobby change dirty forever,
     * which is worse than one with WBN off. A deferred rotation has
     * nothing to register against any more. */
    sim->wbnRotateDeferred = FALSE;
    sim->wbnSessionRotating = FALSE;
  }
}

static char  instanceTrackerAddr[FILENAME_MAX] = "";
static unsigned short instanceTrackerPort = 0;
static unsigned short instanceUdpPort = 0;
static bool  instanceAcceptRemoteClients = FALSE;
static bool  instanceUseTracker = FALSE;
static bool  instanceUseWbn = FALSE;
static bool  instanceUseNatKeepalive = FALSE;
static bool  instanceUseNatPortmap = FALSE;
static NatPortMap instancePortMap;
static int   natPortmapWaitTicks = 0;
static bool  natPortmapNotified = FALSE;
static bool  natPortmapTimedOut = FALSE;

static int   probeWaitTicks       = 0;     /* ticks since last probe send  */
static int   probeReplyWaitTicks  = 0;     /* ticks since last probe reply */
static bool  probeReceived        = FALSE; /* any reply ever arrived       */
static bool  probeTimedOut        = FALSE; /* deadline elapsed w/o reply   */
static char  probeReflexiveIp[64];
static unsigned short probeReflexivePort = 0;

#define PROBE_SEND_INTERVAL_TICKS  500   /* 10 s @ 50 Hz */
#define PROBE_TIMEOUT_TICKS       1500   /* 30 s @ 50 Hz */

static ManualProbeState manualProbeState = MANUAL_PROBE_IDLE;
static int              manualProbeWaitTicks = 0;

#define MANUAL_PROBE_TIMEOUT_TICKS 100   /* 2 s @ 50 Hz */

/* 1500 slices @ ~50Hz (SERVER_TICK_LENGTH = 20ms) ≈ 30s.  After this many
 * ticks without a successful libplum mapping, give up and let Phase 2d's
 * lobby UI surface the "could not open port automatically" state. */
#define NAT_PORTMAP_TIMEOUT_TICKS 1500

static int trackerTime = 5500;
static int wbnTime = 0;
static int natKeepaliveTime = 0;

/* Wall-clock duration of the most recent tick (ms) and its EWMA. Both
 * stay 0 until the first tick has been recorded; the EWMA is seeded
 * from that first measurement so it doesn't bias toward zero. */
static double s_lastTickMs = 0.0;
static double s_tickMsEwma = 0.0;
static const  double kTickAlpha = 0.1;

/* The worst tick recorded since the last reset, and how many ticks cost at
 * least the SERVER_TICK_LENGTH ms the loop has to serve one in. The EWMA
 * above answers "is the server keeping up right now" and decays a spike by an
 * order of magnitude in roughly 22 ticks, so a burst that lasts a handful of
 * frames is back at baseline before an operator can type a console command.
 * These two hold their values until serverLifecycleResetTickPeak clears them
 * at the next round start, so the cost of a burst can be read afterwards. */
static double       s_peakTickMs      = 0.0;
static unsigned int s_ticksOverBudget = 0;

/* Wall-clock cost (ms) of the two serverSimTick calls combined for the
 * most recent tick, plus its EWMA. Same seeding rule as above. */
static double s_lastSimMs = 0.0;
static double s_simMsEwma = 0.0;

void serverLifecycleRecordTickMs(double ms) {
  s_lastTickMs = ms;
  if (s_tickMsEwma <= 0.0) {
    s_tickMsEwma = ms;
  } else {
    s_tickMsEwma = kTickAlpha * ms + (1.0 - kTickAlpha) * s_tickMsEwma;
  }
  if (ms > s_peakTickMs) {
    s_peakTickMs = ms;
  }
  /* A tick that exactly spends its budget has nothing left for the next one,
   * so the count is of ticks at or above it, not strictly over. */
  if (ms >= (double)SERVER_TICK_LENGTH) {
    s_ticksOverBudget++;
  }
}

void serverLifecycleGetTickStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs)  *outLastMs  = s_lastTickMs;
  if (outEwmaMs)  *outEwmaMs  = s_tickMsEwma;
}

void serverLifecycleGetTickPeak(double *outPeakMs,
                                unsigned int *outOverBudget) {
  if (outPeakMs)     *outPeakMs     = s_peakTickMs;
  if (outOverBudget) *outOverBudget = s_ticksOverBudget;
}

void serverLifecycleResetTickPeak(void) {
  s_peakTickMs      = 0.0;
  s_ticksOverBudget = 0;
}

uint32_t serverTickCatchUp(uint32_t nowMs, uint32_t *oldTick, uint32_t *ticks,
                           ServerTickStepFn step, void *ctx) {
  if (oldTick == NULL || step == NULL) {
    return 0;
  }

  /* The debt as it stood on entry, so the warning reports what the callback
   * was handed rather than the remainder it left behind. */
  uint32_t debtMs   = nowMs - *oldTick;
  uint32_t ran      = 0;
  uint32_t worstMs  = 0;

  while ((nowMs - *oldTick) > SERVER_TICK_LENGTH) {
    Uint64 stepStart = SDL_GetTicks();
    uint32_t stepMs;
    /* The step answers the shutdown flag before it runs anything, so a
     * teardown raised mid-burst stops here with neither the counter nor
     * oldTick moved for a tick that did not happen. */
    if (!step(ctx)) {
      break;
    }
    stepMs = (uint32_t)(SDL_GetTicks() - stepStart);
    if (stepMs > worstMs) {
      worstMs = stepMs;
    }
    ran++;
    if (ticks != NULL) {
      (*ticks)++;
    }
    *oldTick += SERVER_TICK_LENGTH;
  }

  /* One line for the whole burst, outside the loop: a hitch must not turn
   * into a per-tick write that costs more than the hitch it reports. */
  if (ran > SERVER_HITCH_WARN_TICKS) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "tick catch-up: debt %u ms, ran %u ticks, slowest tick %u ms",
                (unsigned)debtMs, (unsigned)ran, (unsigned)worstMs);
  }

  return ran;
}

static void serverLifecycleRecordSimMs(double ms) {
  s_lastSimMs = ms;
  if (s_simMsEwma <= 0.0) {
    s_simMsEwma = ms;
  } else {
    s_simMsEwma = kTickAlpha * ms + (1.0 - kTickAlpha) * s_simMsEwma;
  }
}

void serverLifecycleGetSimStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs)  *outLastMs  = s_lastSimMs;
  if (outEwmaMs)  *outEwmaMs  = s_simMsEwma;
}

/* The live delayed-stream ring for this instance, NULL when spectating is
 * disabled (maxSpectators == 0) or no MP server is up. Single instance per
 * process, matching the instance* file-statics above. */
static SpectatorRing *s_spectatorRing = NULL;

/* Keyframe cadence, in GAME TICKS. The ring's spectatorRingNeedsKeyframe
 * compares the sim's gameTick, which advances by 2 per recorded tick (two
 * half-steps per serverSimTick), so a 5-second cadence is 250 records = 500
 * gameTicks. Distinct from retention, which is counted in recordSeq. */
#define SPECTATOR_KEYFRAME_CADENCE_GAMETICKS 500
/* The cadence expressed in records (recordSeq), for the retention derivation:
 * 5 s at 50 records/s = 250 records (half the gameTick figure). */
#define SPECTATOR_KEYFRAME_CADENCE_RECORDS   250
/* Slack kept beyond the delay so the gameover tail stays watchable. */
#define SPECTATOR_RETENTION_MARGIN_RECORDS   100

/* Create the live ring for this instance and register it with the log tap, so
 * every serverSimTick records one ring tick while the log is running. No-op
 * (leaves the ring NULL) when spectating is disabled. Sized in DD-4 units:
 * cadence in gameTicks, retention in recordSeq = specDelayTicks + two cadence
 * intervals + margin, so the newest keyframe at-or-before head - delay always
 * survives. */
void serverInstanceCreateSpectatorRing(ServerSim *sim) {
  uint32_t retentionRecords;

  if (serverSimGetMaxSpectators(sim) == 0) {
    s_spectatorRing = NULL;   /* spectating disabled — record nothing */
    return;
  }
  retentionRecords = serverSimGetSpecDelayTicks(sim) +
                     2u * SPECTATOR_KEYFRAME_CADENCE_RECORDS +
                     SPECTATOR_RETENTION_MARGIN_RECORDS;
  s_spectatorRing = spectatorRingCreate(SPECTATOR_KEYFRAME_CADENCE_GAMETICKS,
                                        retentionRecords);
  logSetSpectatorRing(s_spectatorRing, sim);
}

/* Unregister the tap before freeing, so the timer thread can never touch a
 * freed ring, then destroy it. NULL-safe and idempotent. */
void serverInstanceDestroySpectatorRing(void) {
  logSetSpectatorRing(NULL, NULL);
  spectatorRingDestroy(s_spectatorRing);
  s_spectatorRing = NULL;
}

SpectatorRing *serverInstanceGetSpectatorRing(void) {
  return s_spectatorRing;
}

/* Where a script a player uploads lands, decided once here so the transport
 * and the scenario host read the same directory: none under OFF, the persist
 * directory under PERSIST (the configured one, else <map root>/Uploads/Scripts)
 * and the session directory under ALLOW (the configured one, else
 * <map root>/Uploads/Session-<port>). The map root is already known: the map
 * directory is installed on the sim before startup. The port is in the
 * session directory's name because the session directory is emptied, at
 * startup and whenever the lobby empties: two dedicated servers on one map
 * root would otherwise each throw away the other's session. Two servers on
 * one machine have two ports, and a desktop host names its own directory
 * under its prefs path.
 *
 * The session directory is recorded, and emptied, only on a host that takes
 * remote clients. The welcome-screen sim runs startup too, and on a desktop
 * its map root is inside the application bundle. */
static void serverInstanceResolveScriptDirs(ServerSim *sim,
                                            const ServerInstanceConfig *cfg) {
  char dir[FILENAME_MAX];
  const char *root = serverSimGetMapDirRoot(sim);

  dir[0] = '\0';
  if (cfg->scriptUploadPolicy == SCRIPT_UPLOAD_PERSIST) {
    if (cfg->scriptUploadDir != NULL && cfg->scriptUploadDir[0] != '\0') {
      SDL_strlcpy(dir, cfg->scriptUploadDir, sizeof(dir));
    } else {
      SDL_snprintf(dir, sizeof(dir), "%s/Uploads/Scripts", root);
    }
  } else if (cfg->scriptUploadPolicy == SCRIPT_UPLOAD_ALLOW) {
    if (cfg->scriptSessionDir != NULL && cfg->scriptSessionDir[0] != '\0') {
      SDL_strlcpy(dir, cfg->scriptSessionDir, sizeof(dir));
    } else {
      SDL_snprintf(dir, sizeof(dir), "%s/Uploads/Session-%u", root,
                   (unsigned)cfg->udpPort);
    }
  }
  serverSimSetScriptUploadDir(sim, dir);
  serverSimSetScriptSessionDir(
      sim, (cfg->acceptRemoteClients &&
            cfg->scriptUploadPolicy == SCRIPT_UPLOAD_ALLOW) ? dir : "");
  /* Whatever the last session left, gone before anything lists it. */
  serverSimEmptyScriptSessionDir(sim);
}

bool serverInstanceStartup(ServerSim *sim, const ServerInstanceConfig *cfg) {
  const char *bindAddr = (cfg->bindAddr != NULL) ? cfg->bindAddr : "";
  const char *password = (cfg->password != NULL) ? cfg->password : "";
  unsigned short boundPort = cfg->udpPort;

  instanceAcceptRemoteClients = cfg->acceptRemoteClients;

  serverInstanceResolveScriptDirs(sim, cfg);

  /* Gate the MP diagnostic log on acceptRemoteClients so bg_game's own
   * ServerSim (which also runs serverInstanceStartup at welcome-screen
   * boot) doesn't fill the log with its publishes.  Enable BEFORE
   * serverSimApplyInstanceConfig below so the very first lobby-settings
   * publish for the real MP host is captured. */
  if (cfg->acceptRemoteClients) {
    mpDiagLogEnable(1);
  }

  if (cfg->acceptRemoteClients) {
    sim->maxPlayers = (cfg->maxPlayers > 0) ? cfg->maxPlayers : (BYTE)MAX_TANKS;
    if (transportUdpServerCreate(cfg->udpPort, bindAddr, sim,
                                 password) == FALSE) {
      return FALSE;
    }
    /* From here on the port that matters is the one the socket actually got,
       not the one that was asked for: a requested 0 means the OS chose, and
       everything below advertises where clients should connect. */
    boundPort = transportUdpServerGetBoundPort();
    transportUdpServerSetUploadConfig(cfg->uploadPolicy,
                                      cfg->uploadMaxFiles,
                                      cfg->uploadMaxStorageBytes,
                                      cfg->uploadPersistDir,
                                      cfg->scriptUploadPolicy,
                                      cfg->scriptUploadMaxFiles,
                                      cfg->scriptUploadMaxStorageBytes);
    sim->uploadPolicy = cfg->uploadPolicy;
    serverSimSetScriptUploadPolicy(sim, cfg->scriptUploadPolicy);
    serverSimSetScriptSharing(sim, !cfg->noScriptSharing);
    /* Same source (cfg->uploadPersistDir) as the transport copy above, so the
     * write target and the "Uploads/" resolver redirect never diverge. */
    serverSimSetUploadPersistDir(sim, cfg->uploadPersistDir);
    /* serverVoiceOff is the only mode that stops forwarding: proximity is
     * not implemented, so it forwards like on. */
    transportUdpServerSetVoiceEnabled(cfg->voiceMode != serverVoiceOff);
    if (cfg->mdnsAdvertise) {
      transportUdpServerStartMdnsAdvertiser(boundPort);
    }
  }

  serverSimApplyInstanceConfig(sim, cfg);

  /* Stand up the live delayed-stream ring once the spectator cap / delay are
   * populated. Only for a real MP host — the welcome-screen sim runs startup
   * with acceptRemoteClients == FALSE and must record nothing. */
  if (cfg->acceptRemoteClients) {
    serverInstanceCreateSpectatorRing(sim);
  }

  instanceUseWbn = cfg->acceptRemoteClients && cfg->useWbn;
  if (instanceUseWbn) {
    /* Populate the WBN lobby snapshot so register carries the
     * extended settings + human/bot counts on the first POST. */
    serverSimRefreshWbnLobbyInfo(sim);
    winbolonetCreateServer(sim->mapName, boundPort,
                           (BYTE)gameTypeGet(&sim->sim.game),
                           cfg->compTanks,
                           (BYTE)sim->sim.hiddenMines,
                           (BYTE)sim->hasPassword,
                           basesGetNumActive(&sim->sim.bs),
                           pillsGetNumActive(&sim->sim.pb),
                           serverSimGetNumNeutralBases(sim),
                           serverSimGetNumNeutralPills(sim),
                           serverSimGetNumPlayers(sim));
    if (!sim->lobbyEnabled) {
      winbolonetSendLobbyStatus(FALSE);
    }
  }

  if (cfg->acceptRemoteClients) {
    instanceUseTracker = cfg->useTracker;
    if (cfg->useTracker && cfg->trackerAddr != NULL) {
      strncpy(instanceTrackerAddr, cfg->trackerAddr, FILENAME_MAX - 1);
      instanceTrackerAddr[FILENAME_MAX - 1] = '\0';
    } else {
      instanceTrackerAddr[0] = '\0';
    }
    instanceTrackerPort = cfg->trackerPort;
    instanceUseNatKeepalive = cfg->useNatKeepalive;
  } else {
    instanceUseTracker = FALSE;
    instanceTrackerAddr[0] = '\0';
    instanceTrackerPort = 0;
    instanceUseNatKeepalive = FALSE;
  }
  instanceUdpPort = boundPort;

  trackerTime = 5500;
  wbnTime = 0;
  natKeepaliveTime = 0;

  instanceUseNatPortmap = cfg->acceptRemoteClients && cfg->useNatPortmap;
  memset(&instancePortMap, 0, sizeof(instancePortMap));
  natPortmapWaitTicks = 0;
  natPortmapNotified  = FALSE;
  natPortmapTimedOut  = FALSE;
  probeWaitTicks       = 0;
  probeReplyWaitTicks  = 0;
  probeReceived        = FALSE;
  probeTimedOut        = FALSE;
  probeReflexiveIp[0]  = '\0';
  probeReflexivePort   = 0;
  manualProbeState     = MANUAL_PROBE_IDLE;
  manualProbeWaitTicks = 0;
  if (instanceUseNatPortmap) {
    natPortMapRequest(boundPort, &instancePortMap);
  }
  return TRUE;
}

/* Boot-all map-rotation round restart. Pairs serverSimMapRotateRound's
 * sim-core reset with the surrounding transport teardown and WBN session
 * rotation, mirroring the gameOver->lobby and empty-reset dances but starting
 * a fresh no-lobby running round instead of returning to a lobby.
 *
 * Always stashes the finished round's log here: the dedicated-log
 * subscriber's handleGameOver no-ops in no-lobby mode (it defers to the
 * process-shutdown path that doesn't run in rotation), so neither the win
 * trigger's CTRL_GAME_PHASE_GAME_OVER publish nor the empty trigger stashes
 * for us. serverDedicatedLogStashCurrentRound is idempotent (no-ops once the
 * log is stopped), so an extra call is harmless. */
static void serverLifecycleRotateRound(ServerSim *sim) {
  /* Boot everyone first — the round is over and nobody carries forward. */
  if (instanceAcceptRemoteClients) {
    transportUdpServerDisconnectAll(sim);
  }

  roundLogStash();

  /* Sim-core reset: opens the WBN rotation window, picks the next map and
   * starts a fresh running round (state -> running). serverSimStartGame inside
   * recreated the server-side tanks for every still-connected player, which
   * after the disconnect-all above is just the bots (humans were booted). */
  serverSimMapRotateRound(sim);

  /* Re-arm any kept bots for the new round: reload their ClientSim map from the
   * freshly-loaded world, recreate their tanks and reset brain state. Mirrors
   * the countdown->running transition's botManagerOnGameStart call — the
   * no-lobby rotation path skips that transition, so it must do this itself, or
   * a -maprotate -bots server would keep stale round-1 bot state. */
  if (serverSimGetNumBots(sim) > 0) {
    botManagerOnGameStart(sim);
  }

  /* WBN session rotation around the round-log upload. End the finished round's
   * session (server/quit) so WBN accepts the upload against the still-live
   * key, flush the upload, then register the next round's session — which
   * overwrites winboloNetServerKey with the freshly-picked map's key. Same
   * sandwich as the gameOver->lobby and empty-reset sites. Done after
   * serverSimMapRotateRound so the register reports the new map / base / pill
   * counts, not the round that just ended. All three are queued, not sent
   * here — see the gameOver->lobby site for what that means for the rotation
   * window. */
  if (winbolonetIsRunning()) {
    /* Flush any WBN events still queued from the finished round (win
     * crediting, final kills) against the live key before tearing the
     * session down — mirrors the gameOver->lobby block's pre-EndSession
     * flush. Player counts read 0 here (everyone was just booted); only
     * the queued events matter. */
    winbolonetServerUpdate(serverSimGetNumPlayers(sim),
                           serverSimGetNumNeutralBases(sim),
                           serverSimGetNumNeutralPills(sim), TRUE);
  }
  serverLifecycleQueueRotation(sim);

  /* Start the next round's log and push the fresh map to any in-process
   * subscriber (SP host loopback, replay-log writer). serverSimMapRotateRound
   * already moved the sim to running; publish RUNNING (the dedicated-log
   * subscriber opens a fresh round log on it) and the map change. */
  {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_RUNNING;
    serverSimPublishControl(sim, &evt);
  }
  if (instanceAcceptRemoteClients) {
    transportUdpServerOnLobbyMapChange(sim);
  }
  {
    ControlEvent mapEvt;
    memset(&mapEvt, 0, sizeof(mapEvt));
    mapEvt.type = CTRL_LOBBY_MAP_CHANGE;
    serverSimPublishControl(sim, &mapEvt);
  }
}

/* ── Brain-debug recording session lifecycle (-braindebug) ─────────────────
 * Recording is split into fixed wall-clock BLOCKS so a long run produces a
 * sequence of bounded files instead of one giant .btr. Each block is its own
 * debug_sessions/<baseTS>_<N>/ dir: <baseTS> is the GAME's start time and <N>
 * is the 1-based 15-minute block, so block N began at baseTS + (N-1)*15 min
 * (just add 15 min per suffix to read the wall-clock of each). A new game
 * resets the base timestamp and block counter. */
#define BRAINDBG_BLOCK_MS         (15u * 60u * 1000u)            /* 15-minute blocks */
#define BRAINDBG_DISK_FLOOR_BYTES (50ULL * 1024 * 1024 * 1024)  /* stop recording below 50 GB free */
#define BRAINDBG_DISK_CHECK_MS    5000u                         /* throttle the free-space syscall */

static bool   s_braindbgSessionOpen     = false;  /* a block is currently open */
static int    s_braindbgBlockNum        = 0;      /* 1-based block index */
static char   s_braindbgBaseTS[32]      = "";     /* game-start timestamp, shared by all blocks */
static char   s_braindbgLabel[24]       = "";     /* optional session-name suffix (e.g. "autotest") */
static Uint64 s_braindbgBlockStartMs    = 0;      /* SDL_GetTicks when this block opened */
static Uint64 s_braindbgLastDiskCheckMs = 0;

/* Free bytes on the volume holding debug_sessions/ (falls back to cwd). Returns
 * a huge value when it can't be determined, so an unknowable disk never aborts. */
static unsigned long long serverLifecycleFreeDiskBytes(void) {
#ifdef _WIN32
  ULARGE_INTEGER freeAvail;
  if (GetDiskFreeSpaceExA("debug_sessions", &freeAvail, NULL, NULL)
      || GetDiskFreeSpaceExA(".", &freeAvail, NULL, NULL)) {
    return (unsigned long long)freeAvail.QuadPart;
  }
#else
  struct statvfs vfs;
  if (statvfs(".", &vfs) == 0) {
    return (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
  }
#endif
  return ~0ULL;  /* unknown — treat as plenty */
}

/* Open one recording block: debug_sessions/<baseTS>_<N>/. Finalizes any prior
 * block, creates the new dir, points the recorder at it, and (re)publishes
 * DEBUG_SESSION_DIR + resets print2 on every bot so their logs land in the new
 * block. Stamps the block start time. */
static void serverLifecycleOpenBraindbgBlock(ServerSim *sim) {
  brainRecordEndGame();   /* close the prior block's .btr + reset recorder state */

  char dir[FILENAME_MAX];
  /* Optional label is appended AFTER the block number so the leading
   * "<baseTS>" stays intact for BrainTest's btParseDirTime() (duration) while
   * still tagging the dir — e.g. debug_sessions/20260704_153000_1_autotest/. */
  if (s_braindbgLabel[0]) {
    snprintf(dir, sizeof(dir), "debug_sessions/%s_%d_%s",
             s_braindbgBaseTS, s_braindbgBlockNum, s_braindbgLabel);
  } else {
    snprintf(dir, sizeof(dir), "debug_sessions/%s_%d", s_braindbgBaseTS, s_braindbgBlockNum);
  }
  if (!SDL_CreateDirectory(dir)) {
    fprintf(stderr, "brain_record: couldn't create %s (%s); recording off\n",
            dir, SDL_GetError());
    return;
  }
  brainRecordSetSessionDir(dir);

  char setSession[FILENAME_MAX + 32];
  snprintf(setSession, sizeof(setSession), "_G.DEBUG_SESSION_DIR=\"%s\"", dir);
  for (int i = 0; i < MAX_TANKS; i++) {
    if (!serverSimIsBot(sim, (BYTE)i)) continue;
    serverSimBotExecLua(sim, (BYTE)i, setSession);
    serverSimBotExecLua(sim, (BYTE)i,
        "local ok,p=pcall(require,'print2'); if ok and p.reset_log then p.reset_log() end");
  }
  s_braindbgBlockStartMs = SDL_GetTicks();
  fprintf(stderr, "brain_record: recording block -> %s\n", dir);
}

/* Start a new game's recording at block 1: stamp the base timestamp (the game's
 * wall-clock start) and open debug_sessions/<baseTS>_1/. Called on the first
 * running tick of each game so lobby time is excluded and the timeline anchors
 * at tick 0. */
static void serverLifecycleStartBrainDebugSession(ServerSim *sim) {
  time_t t = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  strftime(s_braindbgBaseTS, sizeof(s_braindbgBaseTS), "%Y%m%d_%H%M%S", &tmv);

  /* Optional session label (e.g. "autotest") from WINBOLO_BRAINDBG_LABEL, so an
   * automated-test recording is self-identifying in the debug_sessions/ dir name
   * and in BrainTest's Load Session browser. Sanitized to [A-Za-z0-9_-]. */
  s_braindbgLabel[0] = '\0';
  const char *lbl = getenv("WINBOLO_BRAINDBG_LABEL");
  if (lbl && *lbl) {
    size_t j = 0;
    for (size_t i = 0; lbl[i] && j + 1 < sizeof(s_braindbgLabel); i++) {
      char c = lbl[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-') {
        s_braindbgLabel[j++] = c;
      }
    }
    s_braindbgLabel[j] = '\0';
  }

  s_braindbgBlockNum = 1;
  serverLifecycleOpenBraindbgBlock(sim);
}

void serverInstanceTick(ServerSim *sim) {
  /* Measure the entire tick wall-clock — outside the mutex acquire so
   * the EWMA captures contention wait time too. Single bottom-of-function
   * end measurement; the function has no early-return paths. */
  Uint64 tickStart = SDL_GetPerformanceCounter();

  trackerTime++;
  wbnTime++;

  transportUdpServerDrainPunchQueue();

  threadsWaitForMutex();
  /* Reads sim state (player/base/pill counts, lobby phase) for the mDNS TXT
   * records, so it runs under the tick mutex to avoid a torn read. */
  transportUdpServerPollMdnsAdvertiser(sim);
  /* Receive packets — queues inputs for both ticks */
  if (transportUdpServerHasRecvThread()) {
    transportUdpServerDrainRecvQueue(sim);
  } else {
    transportUdpServerRecv(sim);
  }

  /* Run deferred removals for slots force-disconnected mid-publish (control
   * queue overflow). Done here, after recv processing and outside any
   * publish, so serverSimRemovePlayer can safely fan its events out. */
  transportUdpServerDrainPendingRemovals(sim);

  /* Hand back whatever the WinBolo.net worker finished since the last tick.
   * Alongside the removal drain above for the same reason: deferred work
   * that has to run on this thread, before the sim does. */
  winbolonetThreadDrainResults(serverLifecycleWbnResult, sim);

  if (sim->state == serverStateRunning) {
    /* First running tick of this game → open a fresh recording session
     * (deferred from startup so lobby/countdown time is excluded and the
     * timeline anchors at this game's tick 0). Runs before serverSimBotTick so
     * DEBUG_SESSION_DIR is set before any bot thinks / brainRecordTick fires. */
    if (brainRecordIsEnabled() && !s_braindbgSessionOpen) {
      /* First running tick of this game → open recording block 1, unless the
       * disk is already below the floor. */
      if (serverLifecycleFreeDiskBytes() < BRAINDBG_DISK_FLOOR_BYTES) {
        fprintf(stderr, "brain_record: LOW DISK (<50 GB free) — recording NOT started\n");
        brainRecordSetEnabled(false);
      } else {
        serverLifecycleStartBrainDebugSession(sim);
        s_braindbgSessionOpen = true;
      }
    } else if (s_braindbgSessionOpen) {
      Uint64 nowMs = SDL_GetTicks();
      /* Low-disk abort (throttled): stop ALL debug writes so a long run can't
       * fill the disk. The server keeps running — only recording stops. */
      if (nowMs - s_braindbgLastDiskCheckMs >= BRAINDBG_DISK_CHECK_MS) {
        s_braindbgLastDiskCheckMs = nowMs;
        if (serverLifecycleFreeDiskBytes() < BRAINDBG_DISK_FLOOR_BYTES) {
          fprintf(stderr, "brain_record: LOW DISK (<50 GB free) — stopping recording\n");
          brainRecordEndGame();
          brainRecordSetEnabled(false);
          s_braindbgSessionOpen = false;
          for (int i = 0; i < MAX_TANKS; i++) {
            if (serverSimIsBot(sim, (BYTE)i)) {
              serverSimBotExecLua(sim, (BYTE)i, "_G._PRINT2_ENABLED=false");
            }
          }
        }
      }
      /* 15-minute block roll: finalize the current block and open the next,
       * seamlessly (no game interruption). */
      if (s_braindbgSessionOpen && (nowMs - s_braindbgBlockStartMs) >= BRAINDBG_BLOCK_MS) {
        s_braindbgBlockNum++;
        serverLifecycleOpenBraindbgBlock(sim);
      }
    }
    /* Run brain AI bots — queues two InputPackets per bot (keys + game) */
    if (serverSimGetNumBots(sim) > 0) {
      serverSimBotTick(sim, sim->botAiType);
    }
    /* Advance the sim by one 20ms frame.  serverSimTick internally runs
     * the keys-tick + game-tick pair and accumulates events from both
     * half-steps into a single frame's worth of state, so the prior
     * save/restore dance is no longer needed here.  The half-step split
     * is private to server_sim.c. */
    ServerState preTickState = sim->state;
    Uint64 simStart = SDL_GetPerformanceCounter();
    serverSimTick(sim);
    Uint64 simEnd = SDL_GetPerformanceCounter();
    /* If game ended during this tick, publish game-over events */
    if (preTickState == serverStateRunning && sim->state == serverStateGameOver) {
      /* Game ended → finalize this game's recording so the .btr is complete on
       * disk; clearing the flag makes the next game open a fresh dir (the
       * map-rotate restart below re-enters running on a later tick). */
      if (s_braindbgSessionOpen) {
        brainRecordEndGame();
        s_braindbgSessionOpen = false;
      }
      /* Decide the win/exit message and WBN crediting. The policy lives in
       * the sim core (serverSimResolveGameOver) so the dedicated server and
       * the in-process SP/host both resolve a game over identically. */
      serverSimResolveGameOver(sim);
      {
        ControlEvent phaseEvt;
        ControlEvent overEvt;
        memset(&phaseEvt, 0, sizeof(phaseEvt));
        phaseEvt.type = CTRL_GAME_PHASE_GAME_OVER;
        serverSimPublishControl(sim, &phaseEvt);
        memset(&overEvt, 0, sizeof(overEvt));
        overEvt.type = CTRL_GAME_OVER;
        serverSimPublishControl(sim, &overEvt);
      }
      /* Ship the round's scoreboard + awards while the accumulator is still
       * intact (returnToLobby clears it later). This path runs only for a
       * round that actually reached game-over. */
#if POSTGAME_STATS_ENABLED
      {
        ControlEvent rsEvt;
        rsEvt.type = CTRL_ROUND_STATS;
        serverSimBuildRoundStatsSummary(sim, &rsEvt.u.roundStats);
        serverSimPublishControl(sim, &rsEvt);
      }
#endif
      /* No-lobby map rotation: a win boots everyone and restarts a fresh
       * round here, inside the tick, so sim->state leaves gameOver before
       * the main loop's exit check observes it — the server never quits. */
      if (serverSimIsMapRotateEnabled(sim)) {
        serverLifecycleRotateRound(sim);
      }
    }
    if (sim->state == serverStateRunning) {
      if (instanceAcceptRemoteClients) {
        transportUdpServerDrainEvents(sim);
      }
    }
    double simFreq = (double)SDL_GetPerformanceFrequency();
    double simMs = (double)(simEnd - simStart) * 1000.0 / simFreq;
    serverLifecycleRecordSimMs(simMs);
    /* Send snapshots only if still running */
    if (sim->state == serverStateRunning) {
      if (instanceAcceptRemoteClients) {
        transportUdpServerSend(sim);
      }
    }
  } else {
    /* Lobby/countdown/gameover: single tick for state machine processing */
    ServerState preTickState = sim->state;
    serverSimTick(sim);

    /* Check if a balance proposal just completed */
    if (sim->balanceProposal.broadcastNeeded) {
      ControlEvent evt;
      memset(&evt, 0, sizeof(evt));
      evt.type = CTRL_BALANCE_PROPOSAL;
      memcpy(evt.u.balanceProposal.teamForSlot,
             sim->balanceProposal.teamForSlot, MAX_TANKS);
      serverSimPublishControl(sim, &evt);
      sim->balanceProposal.broadcastNeeded = false;
    }

    /* Handle state transitions */
    if (preTickState == serverStateCountdown) {
      if (sim->state == serverStateRunning) {
        /* Countdown finished — game started.  Reset per-client and
         * per-slot transport state before publishing the RUNNING
         * transition so the codec encodes PACKET_GAME_START against
         * fresh queues. */
        transportUdpServerOnGameStart(sim);
        {
          ControlEvent evt;
          memset(&evt, 0, sizeof(evt));
          evt.type = CTRL_GAME_PHASE_RUNNING;
          serverSimPublishControl(sim, &evt);
        }
        /* The table this round runs on has already been stated: the tick
         * that ended the countdown ran serverSimStartGame, which publishes
         * it at the end of every start. */
        if (serverSimGetNumBots(sim) > 0) {
          botManagerOnGameStart(sim);
        }
        /* Re-assert team alliances now that (a) the reliable queues were
         * reset above — discarding the CTRL_ALLIANCE_RESET the start
         * sequence published, which left remote clients rendering their
         * own teammates as enemies — and (b) botManagerOnGameStart just
         * rebuilt the bot ClientSims, whose alliance matrices start
         * empty. One republish + direct bot sync fixes both sides. */
        serverSimReapplyTeamAlliances(sim);
        /* Notify WBN that we are now in-game */
        winbolonetSendLobbyStatus(FALSE);
        /* Send EVENT_PLAYER_JOIN for each connected WBN player */
        {
          BYTE pi;
          for (pi = 0; pi < MAX_TANKS; pi++) {
            if (sim->playerConnected[pi] &&
                winboloNetIsPlayerParticipant(pi)) {
              winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                 pi, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
            }
          }
        }
        /* Force a snapshot to every client in the same tick as the
         * RUNNING publish above. The running-state branch's periodic
         * transportUdpServerSend fires on the NEXT tick (~20ms), so
         * without this push the client receives CTRL_GAME_PHASE_RUNNING,
         * flips inLobby=false, and renders its (stale, round-1) MY_TANK
         * for a frame before the first authoritative snapshot lands.
         * serverSendSnapshot drains the per-client control queue into
         * the same packet, so RUNNING and the fresh tank state arrive
         * bundled — pairs with the client's hasPredictedTank=FALSE
         * reset on LOBBY to make that first snapshot run the init
         * branch (stocks, camera centre). */
        if (instanceAcceptRemoteClients) {
          transportUdpServerSend(sim);
        }
      } else if (sim->state == serverStateCountdown &&
                 sim->countdownTicks > 0 &&
                 sim->countdownTicks % 50 == 0) {
        /* Broadcast countdown tick (once per second) */
        uint8_t secs = (uint8_t)((sim->countdownTicks + 49) / 50);
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_GAME_PHASE_COUNTDOWN;
        evt.u.gamePhase.countdownSeconds = secs;
        serverSimPublishControl(sim, &evt);
      }
    }
    if (preTickState == serverStateGameOver &&
        sim->state == serverStateLobby) {
      /* Flush remaining WBN events (win, final kills, etc.) */
      winbolonetServerUpdate(serverSimGetNumPlayers(sim),
                             serverSimGetNumNeutralBases(sim),
                             serverSimGetNumNeutralPills(sim), TRUE);
      /* Pick next map from rotation if mapdir is configured */
      if (sim->mapDirFiles != NULL) {
        serverSimMapDirPickRandom(sim);
      }
      /* End the round's WBN session, upload the round log against the
       * just-quit key (WBN rejects uploads to an active session), then
       * register a fresh session for the next round. handleGameOver already
       * stashed the round's filename when the GAME_OVER phase fired; the
       * flush is a no-op when there's nothing pending or when WBN is offline.
       *
       * All three are queued for the worker rather than sent here. It sends
       * them in the order they were queued, which is the order WinBolo.net
       * needs — quit before the upload, upload before the key swap — and
       * none of the three costs this tick anything.
       *
       * The rotation window stays open. serverLifecycleWbnResult closes it
       * when the register result lands, and does the rekey broadcast and the
       * lock re-send there too. With no register queued there is no result
       * coming, so the window closes in the helper instead. A register
       * still out from the previous round defers all three until it
       * answers. */
      serverLifecycleQueueRotation(sim);
      /* Republish the bot brain catalogue.  Mid-game joiners were gated
       * out of the BrainList during their sync replay (see
       * serverSimSyncSubscriber), so they need it now before the lobby
       * UI's AiConfig combobox appears.  In-lobby clients get it as a
       * (cheap) refresh. */
      {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        serverSimFillLobbyBrainListEvent(sim, &evt);
        serverSimPublishControl(sim, &evt);
        /* ... and the brains' announce lines that go with it, so the
         * returning lobby can announce a bot's brain the same way a fresh
         * join does. The refresh first: this seam between rounds is where an
         * operator would have edited a brain's texts, and it is off the tick
         * path, so a re-read costs nothing anybody feels. Docs that changed
         * get a new generation here, which tells each client to drop the
         * copy it holds. */
        serverSimRefreshBrainDocs(sim);
        serverSimEmitBrainAnnounces(sim, serverSimPublishBrainAnnounceCb, sim);
      }
      /* Republish lobby state so every client's mirror reflects the
       * fresh lobby. serverSimReturnToLobby's contract says the caller
       * does this fan-out; CTRL_GAME_PHASE_LOBBY alone doesn't carry
       * the inLobby flag or per-slot data, so without these the host's
       * own UDP loopback ClientSim leaves cs->inLobby false and never
       * opens the lobby dialog — the window looks frozen because there
       * is no game view either. */
      serverSimPublishLobbySettings(sim);
      {
        BYTE pi;
        /* Republish EVERY slot, not just connected ones. A player who
         * left mid-round had their CTRL_LOBBY_SLOT suppressed — the
         * leave-time publish is gated to lobby/countdown state
         * (transport_udp_server.c PACKET_QUIT), so a running-state quit
         * never told clients to clear that slot. The client's lobbySlots
         * mirror is only mutated by CTRL_LOBBY_SLOT (CTRL_PLAYER_LEAVE is
         * chat-only), so without this the departed player lingers as a
         * ghost in the returning lobby. A vacant slot fills as
         * connected=false (serverSimFillLobbySlotEvent), which clears it. */
        for (pi = 0; pi < MAX_TANKS; pi++) {
          serverSimPublishLobbySlot(sim, pi);
        }
      }
      /* Send the win message now that players are back in the lobby */
      if (sim->pendingWinMessage[0] != '\0') {
        transportUdpServerSendServerMessage(sim->pendingWinMessage);
        sim->pendingWinMessage[0] = '\0';
      }
    }

    /* Periodic lobby-slot republish so the ping column in the lobby
     * UI tracks live values instead of freezing between unrelated
     * slot changes (ready toggle, bot config, etc.). CTRL_LOBBY_SLOT
     * carries pingMs; without this heartbeat a quiet lobby shows the
     * value from whenever someone last clicked something. 250 ticks
     * at 50 Hz is ~5 s, well under the perceptible-staleness window
     * and far below the wire cost the queue can absorb. Skipped in
     * running state — snapshots already carry pingMs per tick there. */
    if (transportUdpServerGetTickCount() % 250 == 0 &&
        (sim->state == serverStateLobby ||
         sim->state == serverStateCountdown)) {
      BYTE pi;
      for (pi = 0; pi < MAX_TANKS; pi++) {
        if (sim->playerConnected[pi]) {
          serverSimPublishLobbySlot(sim, pi);
        }
      }
    }

    /* Bot-config events queued by serverSimApplyNewBotDefaults — a freshly
     * added or seeded bot's mode and difficulty — sent a couple per tick
     * instead of inside the add. A scenario seeds ten bots in one call stack
     * while no client ack can be read; ten more events there would grow the
     * burst that once overran a client's 64-event reliable window and
     * dropped the host. Runs for single player too: its timer drives this
     * same function. */
    if (sim->state == serverStateLobby ||
        sim->state == serverStateCountdown) {
      serverSimFlushBotConfigPublishes(sim);
    }

    /* Timeout check — not called via transportUdpServerSend() during lobby */
    transportUdpServerCheckTimeouts(sim);
  }

  /* Auto-close / empty-rotation check — works in any state but game over.
   * The shared serverSimCheckAutoClose latches hadPlayersEver and fires once
   * the server empties after having had players. In map-rotation mode an empty
   * server rotates to a fresh round instead of shutting down; otherwise
   * -autoclose forces a no-lobby shutdown (no players to return to a lobby
   * for). Skipped in game over because the shutdown this triggers is already
   * under way: the console loop only notices serverSimIsTerminalGameOver on
   * its next poll, and without the skip every tick until then re-enters
   * serverSimEnterGameOver and repeats its message. */
  if (sim->state != serverStateGameOver &&
      (sim->autoCloseOnEmpty || serverSimIsMapRotateEnabled(sim)) &&
      serverSimCheckAutoClose(sim)) {
    if (serverSimIsMapRotateEnabled(sim)) {
      serverSimConsoleMessage("Server empty - rotating to a new round.");
      serverLifecycleRotateRound(sim);
    } else {
      sim->lobbyEnabled = FALSE;
      serverSimEnterGameOver(sim);
    }
  }

  /* Empty reset check — when enabled and no players are connected,
   * count down and reset to lobby with map reload after the timeout.
   * Skipped if autoCloseOnEmpty is active (it takes priority). */
  if (sim->emptyResetEnabled && !sim->autoCloseOnEmpty &&
      sim->lobbyEnabled &&
      sim->state != serverStateGameOver &&
      sim->state != serverStateLobby &&
      sim->state != serverStateCountdown &&
      serverSimCheckEmptyReset(sim)) {
    serverSimConsoleMessage("Empty reset timer expired. Resetting to lobby...");
    /* Empty-reset bypasses serverSimReturnToLobby, so open the WBN
     * session-rotation window here before the map pick below reports
     * the next round's map. Cleared after BeginSession installs the
     * new key. */
    sim->wbnSessionRotating = TRUE;
    /* Empty-reset fires from the running state without going through
     * GAME_OVER, so handleGameOver never stashed the in-flight round.
     * Do it here so the upload below picks it up. */
    roundLogStash();
    /* Empty-reset bypasses serverSimReturnToLobby, so the release of the
     * round's parked runners is this path's to make. A parked brain keeps
     * its state table, and the round it remembers is the one ending here. */
    botManagerReleaseParkedRunners(sim);
    serverSimResetGameWorld(sim);
    sim->state = serverStateLobby;
    sim->gameLength = sim->originalGameLength;
    sim->hadPlayersEver = FALSE;
    sim->emptyResetTicks = -1;
    /* Pick next map from rotation if mapdir is configured. The seats the
     * round fielded go back to the template's own lobby, as they do in
     * serverSimMapRotateRound (see the comment there): the server is empty,
     * so there is no host edit to keep. Where no map loads, the lobby is
     * the one it was and the flag goes back to what it said about it. */
    if (sim->mapDirFiles != NULL) {
      bool wasSeated = sim->scenarioLobbySeated;
      sim->scenarioLobbySeated = false;
      if (!serverSimMapDirPickRandom(sim)) {
        sim->scenarioLobbySeated = wasSeated;
      }
    }
    /* End the WBN session, upload the round's log against the just-
     * quit key, then begin a new session for the next round. Same
     * sandwich as the game-over → lobby site (see comment there). */
    /* Same queued rotation as the game-over → lobby site (see the comment
     * there). */
    serverLifecycleQueueRotation(sim);
    /* Empty-reset bypasses serverSimReturnToLobby, so the lobby phase
     * event is never published from the state machine. Publish it
     * explicitly so handleLobbyEnter fires and starts a fresh log for
     * the next round (mirroring serverSimReturnToLobby's tail). */
    {
      ControlEvent evt;
      memset(&evt, 0, sizeof(evt));
      serverSimFillGamePhaseEvent(sim, &evt);
      serverSimPublishControl(sim, &evt);
    }
    /* Push the freshly-reset map to every audience — same reasoning
     * as the serverSimReturnToLobby tail. The wire helper updates
     * the UDP server's cached compressed map / JOIN_ACCEPT / per-
     * client download tracking; the CTRL_LOBBY_MAP_CHANGE publish
     * fans through the bus so in-process subscribers reinstall via
     * boundServerSim. Empty-reset is usually a no-op for the wire
     * leg (no UDP clients at the moment the reset fires), but any
     * SP host or replay-log subscriber bound to this sim still
     * needs the event. */
    if (instanceAcceptRemoteClients) {
      transportUdpServerOnLobbyMapChange(sim);
    }
    {
      ControlEvent mapEvt;
      memset(&mapEvt, 0, sizeof(mapEvt));
      mapEvt.type = CTRL_LOBBY_MAP_CHANGE;
      serverSimPublishControl(sim, &mapEvt);
    }
  }

  threadsReleaseMutex();

  if (wbnTime > 100) {
    threadsWaitForMutex();
    /* Refresh the WBN lobby snapshot so server/update carries current
     * human/bot counts, then flush a deferred lobby_update if its rate
     * window has elapsed. */
    serverSimRefreshWbnLobbyInfo(sim);
    winbolonetServerUpdate(serverSimGetNumPlayers(sim), serverSimGetNumNeutralBases(sim), serverSimGetNumNeutralPills(sim), FALSE);
    serverSimWbnLobbyTick(sim);
    threadsReleaseMutex();
    wbnTime = 0;
  }

  if (trackerTime >= 6000 && instanceUseTracker) {
    transportUdpServerSendTrackerUpdate(sim, instanceTrackerAddr, instanceTrackerPort);
    trackerTime = 0;
  }

  if (instanceUseNatKeepalive && instanceUseTracker) {
    natKeepaliveTime++;
    if (natKeepaliveTime >= 1250) {
      transportUdpServerSendNatKeepalive(sim, instanceTrackerAddr, instanceTrackerPort);
      natKeepaliveTime = 0;
    }
  }

  if (instanceUseNatPortmap) {
    natPortMapRenewIfNeeded(&instancePortMap);
  }

  if (instanceUseNatPortmap && !natPortmapNotified && !natPortmapTimedOut) {
    if (instancePortMap.mapped) {
      /* First completion — point INFO_PACKET at the external address and
       * force a tracker refresh so the registered entry self-corrects in
       * one round trip instead of waiting up to ~120s for the next
       * periodic update. */
      transportUdpServerSetPublicAddress(instancePortMap.externalIp,
                                         instancePortMap.externalPort);
      if (instanceUseTracker && instanceTrackerAddr[0] != '\0') {
        transportUdpServerSendTrackerUpdate(sim, instanceTrackerAddr,
                                            instanceTrackerPort);
        trackerTime = 0;
      }
      natPortmapNotified = TRUE;
    } else if (++natPortmapWaitTicks >= NAT_PORTMAP_TIMEOUT_TICKS) {
      natPortmapTimedOut = TRUE;
    }
  }

  if (instanceUseNatPortmap && instanceUseTracker &&
      instanceTrackerAddr[0] != '\0') {
    if (++probeWaitTicks >= PROBE_SEND_INTERVAL_TICKS) {
      transportUdpServerSendPunchProbe(instanceTrackerAddr,
                                       instanceTrackerPort);
      probeWaitTicks = 0;
    }
    if (!probeReceived) {
      if (++probeReplyWaitTicks >= PROBE_TIMEOUT_TICKS) {
        probeTimedOut = TRUE;
      }
    }
  }

  if (manualProbeState == MANUAL_PROBE_IN_PROGRESS) {
    if (++manualProbeWaitTicks >= MANUAL_PROBE_TIMEOUT_TICKS) {
      manualProbeState = MANUAL_PROBE_TIMEOUT;
    }
  }

  Uint64 tickEnd = SDL_GetPerformanceCounter();
  double freq = (double)SDL_GetPerformanceFrequency();
  double tickMs = (double)(tickEnd - tickStart) * 1000.0 / freq;
  serverLifecycleRecordTickMs(tickMs);
}

void serverInstanceShutdown(ServerSim *sim) {
  /* Unregister + free the live ring first, so nothing taps it during the rest
   * of teardown. NULL-safe — a no-op for a sim that never had a ring. */
  serverInstanceDestroySpectatorRing();
  if (instanceAcceptRemoteClients) {
    mpDiagLogEnable(0);
  }
  if (instanceUseNatPortmap) {
    natPortMapRelease(&instancePortMap);
  }
  if (instanceUseWbn) {
    winbolonetDestroy(TRUE);
  }
  if (instanceAcceptRemoteClients) {
    transportUdpServerStopMdnsAdvertiser();
    transportUdpServerDestroy();
    /* The session's uploaded scripts go with the session. After the
     * transport, so no upload can land behind the emptying. */
    serverSimEmptyScriptSessionDir(sim);
  }
  botManagerDestroy(sim);
  instanceAcceptRemoteClients = FALSE;
  instanceUseWbn = FALSE;
  instanceUseTracker = FALSE;
  instanceUseNatKeepalive = FALSE;
  instanceUseNatPortmap = FALSE;
  instanceUdpPort = 0;
  natPortmapWaitTicks = 0;
  natPortmapNotified  = FALSE;
  natPortmapTimedOut  = FALSE;
  probeWaitTicks       = 0;
  probeReplyWaitTicks  = 0;
  probeReceived        = FALSE;
  probeTimedOut        = FALSE;
  probeReflexiveIp[0]  = '\0';
  probeReflexivePort   = 0;
  manualProbeState     = MANUAL_PROBE_IDLE;
  manualProbeWaitTicks = 0;
}

bool serverInstanceIsNatPunchActive(void) {
  /* No mutex needed — these are plain bools written once at startup
   * / cleared once at shutdown. */
  return instanceUseNatKeepalive && instanceUseTracker;
}

void serverInstanceGetPortmapInfo(ServerPortmapInfo *out) {
  if (out == NULL) {
    return;
  }
  threadsWaitForMutex();
  if (!instanceUseNatPortmap) {
    out->status = SERVER_PORTMAP_DISABLED;
  } else if (instancePortMap.mapped) {
    /* libplum says we have a stable external mapping. Cross-check the
     * reflexive address against what the tracker reports — if they
     * differ, a NAT layer above libplum's gateway is rewriting our
     * port per destination (symmetric NAT) and the libplum mapping
     * is unreachable from third parties. */
    if (probeReceived &&
        (strcmp(probeReflexiveIp, instancePortMap.externalIp) != 0 ||
         probeReflexivePort != instancePortMap.externalPort)) {
      out->status = SERVER_PORTMAP_SYMMETRIC_NAT;
    } else {
      out->status = SERVER_PORTMAP_SUCCEEDED;
    }
  } else if (natPortmapTimedOut) {
    /* libplum gave up. Hole-punching is the fallback — viable iff the
     * tracker probe round-trips. */
    if (probeReceived) {
      out->status = SERVER_PORTMAP_HOLE_PUNCH_OK;
    } else if (probeTimedOut) {
      out->status = SERVER_PORTMAP_FAILED;
    } else {
      out->status = SERVER_PORTMAP_PENDING;
    }
  } else {
    out->status = SERVER_PORTMAP_PENDING;
  }
  out->internalPort   = instanceUdpPort;
  out->externalIp[0]  = '\0';
  out->externalPort   = 0;
  if (out->status == SERVER_PORTMAP_SUCCEEDED) {
    strncpy(out->externalIp, instancePortMap.externalIp,
            sizeof(out->externalIp) - 1);
    out->externalIp[sizeof(out->externalIp) - 1] = '\0';
    out->externalPort = instancePortMap.externalPort;
  } else if (probeReceived) {
    strncpy(out->externalIp, probeReflexiveIp,
            sizeof(out->externalIp) - 1);
    out->externalIp[sizeof(out->externalIp) - 1] = '\0';
    out->externalPort = probeReflexivePort;
  }
  threadsReleaseMutex();
}

void serverInstanceRecordProbeReply(const char *reflexiveIp,
                                    unsigned short reflexivePort) {
  if (reflexiveIp == NULL) return;
  threadsWaitForMutex();
  strncpy(probeReflexiveIp, reflexiveIp, sizeof(probeReflexiveIp) - 1);
  probeReflexiveIp[sizeof(probeReflexiveIp) - 1] = '\0';
  probeReflexivePort  = reflexivePort;
  probeReceived       = TRUE;
  probeTimedOut       = FALSE;       /* mapping is alive; reset deadline */
  probeReplyWaitTicks = 0;
  if (manualProbeState == MANUAL_PROBE_IN_PROGRESS) {
    manualProbeState = MANUAL_PROBE_SUCCESS;
  }
  threadsReleaseMutex();
}

void serverInstanceTriggerManualProbe(void) {
  threadsWaitForMutex();
  manualProbeState     = MANUAL_PROBE_IN_PROGRESS;
  manualProbeWaitTicks = 0;
  /* Force the periodic-probe block to fire on the next tick. The block
   * tests `++probeWaitTicks >= PROBE_SEND_INTERVAL_TICKS`, so setting
   * the counter to the threshold causes an immediate send. */
  probeWaitTicks = PROBE_SEND_INTERVAL_TICKS;
  threadsReleaseMutex();
}

ManualProbeState serverInstanceGetManualProbeState(void) {
  ManualProbeState s;
  threadsWaitForMutex();
  s = manualProbeState;
  threadsReleaseMutex();
  return s;
}
