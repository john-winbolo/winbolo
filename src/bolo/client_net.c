/*
 * client_net.c
 *
 * Implementation of the client_net.h wrappers — ClientSim owns the
 * Transport handle and exposes lifecycle, per-tick driver, state
 * queries, and send operations through clientSim* functions. Replaces
 * direct transport.h / transport_udp.h calls from external glue
 * (desktop GUI, android/wasm, headless, gym, lobby dialogs).
 */

#include "client_net.h"
#include "client_sim_internal.h"
#include "client_sim.h"
#include "server_sim.h"                    /* serverSimLocalJoin + accessors */
#include "server_sim_join.h"
#include "server_sim_lifecycle.h"          /* privatised setters/accessors used by local-transport branches */
#include "control_event.h"                 /* ControlEvent (local-transport ready toggle) */
#include "frontend.h"                      /* frontEndApplyLocalTankPrefs */
#include "transport.h"
#include "transport_udp.h"
#include "netpacks.h"                      /* MAP_DOWNLOAD_MAX_SIZE, lobbyBotNameAcceptable */
#include "input_packet.h"
#include "global.h"
#include "lobby_bot_pools.h"                /* lobbyBotPoolCount (pool uniqueness rewrite) */
#include "../server/threads.h"
#include "../gui/lang.h"
#include "../common/wb_log.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void balanceDebugLog(const char *fmt, ...) {
  static FILE *s_f = NULL;
  static int   s_failed = 0;
  if (s_failed) return;
  if (s_f == NULL) {
    s_f = fopen("balance.log", "a");
    if (s_f == NULL) { s_failed = 1; return; }
    /* unbuffered so a crash doesn't lose the last lines */
    setvbuf(s_f, NULL, _IONBF, 0);
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    tmv = *localtime(&now);
#endif
    fprintf(s_f, "\n----- balance.log opened %04d-%02d-%02d %02d:%02d:%02d -----\n",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(s_f, fmt, ap);
  va_end(ap);
  fputc('\n', s_f);
  fflush(s_f);
}

/* === Lifecycle === */

static void clientSimTeardownTransport(ClientSim *cs) {
  if (!cs->hasTransport) return;
  if (cs->isUdpTransport) {
    transportUdpClientDestroy(&cs->transport);
  } else {
    transportLocalDestroy(&cs->transport);
  }
  cs->hasTransport = false;
}

bool clientSimConnectUdp(ClientSim *cs, const char *serverAddr,
                         unsigned short serverPort, const char *playerName,
                         const char *fallbackCountry,
                         const char *password,
                         const char *wbnApiToken,
                         const char *wbnServerKey,
                         bool wantRejoin, const char *trackerAddr,
                         unsigned short trackerPort) {
  if (cs == NULL) return false;
  clientSimTeardownTransport(cs);
  cs->transport = transportUdpClientCreate(cs, serverAddr, serverPort,
                                           playerName, fallbackCountry,
                                           password,
                                           wbnApiToken, wbnServerKey,
                                           wantRejoin, trackerAddr, trackerPort);
  cs->hasTransport = true;
  cs->isUdpTransport = true;
  clientSimSetLocalTransport(cs, false);
  /* Symmetric with the SP path's clientSimSetNetType(cs, netSingle) at
   * the bottom of clientSimConnectLocalBody.  Multiple sim sites branch
   * on `networkGameType == netUdp` (client_sim.c:974, :1013, :1024,
   * client_snapshot.c:737, :781); leaving it at netNone caused those
   * branches to silently take the wrong path for UDP joiners.  Set
   * here at connect time so the value is right from the first tick. */
  clientSimSetNetType(cs, netUdp);
  return true;
}

/* Map a serverSimLocalJoin failure code to the langid the UDP path
 * uses for the equivalent reject, render it, and stash on the
 * ClientSim. INVALID_INPUT has no dedicated langid — best-fit onto
 * the name-validation reject. */
static void clientSimRenderLocalJoinReject(ClientSim *cs, LocalJoinResult res) {
  langid id;
  const char *rendered;
  switch (res) {
    case LOCAL_JOIN_INVALID_NAME:  id = STR_REJECT_INVALID_PLAYER_NAME; break;
    case LOCAL_JOIN_SLOT_FULL:     id = STR_REJECT_SERVER_FULL;         break;
    case LOCAL_JOIN_GAME_LOCKED:   id = STR_REJECT_GAME_LOCKED;         break;
    case LOCAL_JOIN_INVALID_INPUT: id = STR_REJECT_INVALID_PLAYER_NAME; break;
    default:                       id = STR_REJECT_GAME_LOCKED;         break;
  }
  rendered = langGetText(id);
  clientSimSetConnectErrorReason(cs, rendered ? rendered : "Connection rejected");
}

/* Shared body for the active / passive local-connect paths. Runs the
 * twelve-step join+install dance under the server's threads mutex.
 * Differs from clientSimConnectLocalPassive only by which transport
 * constructor the caller passed in. */
static bool clientSimConnectLocalBody(ClientSim *cs, struct ServerSim *sim,
                                      const char *playerName,
                                      const char *fallbackCountry,
                                      uint8_t clientType, uint8_t clientFlags,
                                      bool passive) {
  Transport tr;
  LocalJoinResult res;
  BYTE slot = 0;
  BYTE compressedMap[MAP_DOWNLOAD_MAX_SIZE];
  int compLen;

  if (cs == NULL || sim == NULL) return false;

  /* 1. Allocate the transport (placeholder slot 0; refined in step 6).
   *    Transport teardown of any prior binding happens before alloc so
   *    a failed reconnect leaves cs in a known state. */
  clientSimTeardownTransport(cs);
  cs->connectErrorReason[0] = '\0';
  tr = passive ? transportLocalCreatePassive(sim, cs, 0)
               : transportLocalCreate       (sim, cs, 0);

  /* 2. Serialise the join + map install against the host's timer
   *    thread (which may already be ticking sim). */
  threadsWaitForMutex();

  /* 3. Hand the join off to ServerSim — name validation, slot search,
   *    lock-state predicate, country fill, type/flags, PLAYER_JOIN
   *    publish, WBN tracker event. */
  res = serverSimLocalJoin(sim, playerName, fallbackCountry,
                           clientType, clientFlags, &slot);
  if (res != LOCAL_JOIN_OK) {
    clientSimRenderLocalJoinReject(cs, res);
    threadsReleaseMutex();
    transportLocalDestroy(&tr);
    return false;
  }

  /* 4. Pull the compressed map blob. */
  compLen = serverSimGetCompressedMap(sim, compressedMap);
  if (compLen <= 0) {
    const char *rendered = langGetText(NETERR_MAPSERIALIZE);
    clientSimSetConnectErrorReason(cs, rendered ? rendered : "Map serialise failed");
    /* Undo the join so the slot is reclaimable. */
    serverSimRemovePlayer(sim, slot);
    threadsReleaseMutex();
    transportLocalDestroy(&tr);
    return false;
  }

  /* 5. Install the map onto the ClientSim. */
  if (!installCompressedMap(cs, compressedMap, compLen,
                            serverSimGetMapName(sim))) {
    const char *rendered = langGetText(NETERR_MAPSERIALIZE);
    clientSimSetConnectErrorReason(cs, rendered ? rendered : "Map serialise failed");
    serverSimRemovePlayer(sim, slot);
    threadsReleaseMutex();
    transportLocalDestroy(&tr);
    return false;
  }

  /* 6. Refine the transport's snapshot-target slot from the placeholder 0
   *    (local-transport-specific; not symmetric with UDP). */
  transportLocalSetPlayerNum(&tr, slot);

  /* 7. Slot-assignment funnel: setPlayerNum + setupSelf (tank + name +
   *    client type/flags) + applyLocalTankPrefs.  Same function the
   *    UDP JOIN_ACCEPT handler calls, so the two transports cannot
   *    drift out of sync. */
  clientSimOnAssignedSlot(cs, slot, playerName, clientType, clientFlags);

  /* 9. Commit the transport binding. boundServerSim drives the
   *     local-transport branch of CTRL_LOBBY_MAP_CHANGE; the
   *     networkGameType / isLocalTransport mirrors make this a
   *     single-player ClientSim. */
  cs->transport          = tr;
  cs->hasTransport       = true;
  cs->isUdpTransport     = false;
  clientSimSetBoundServerSim(cs, sim);
  clientSimSetLocalTransport(cs, true);
  clientSimSetNetType(cs, netSingle);

  /* 10. Register the auto-subscriber so future control events fan in
   *     through the bus. The sync-replay walks lobby state on top of
   *     the freshly-installed map. Stash the handle so disconnect /
   *     destroy can unregister cleanly. */
  cs->autoSubHandle = serverSimRegisterClientSubscriber(sim, cs);

  /* 11. Apply the first snapshot synchronously so the ClientSim has
   *     valid tank state before the next tick wakes up. The localTick
   *     path then takes over for subsequent ticks. */
  {
    SnapshotHeader snapHdr;
    TankSnapshot snapTanks[MAX_TANKS];
    ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
    PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
    GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
    if (cs->transport.getSnapshot(cs->transport.ctx, slot, &snapHdr,
                                  snapTanks, MAX_TANKS,
                                  snapShells, MAX_SNAPSHOT_SHELLS,
                                  snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                  snapBases, MAX_SNAPSHOT_BASES,
                                  snapPills, MAX_SNAPSHOT_PILLS,
                                  snapEvents, MAX_SNAPSHOT_EVENTS)) {
      clientSimSyncFromSnapshot(cs, &snapHdr,
                                snapTanks, snapHdr.tankCount,
                                snapShells, snapHdr.shellCount,
                                snapTkExplosions, snapHdr.tkExplosionCount,
                                snapBases, snapHdr.baseCount,
                                snapPills, snapHdr.pillCount,
                                snapEvents, snapHdr.reliableEventCount,
                                slot);
    }
  }

  /* 12. Release. */
  threadsReleaseMutex();
  return true;
}

bool clientSimConnectLocal(ClientSim *cs, struct ServerSim *sim,
                           const char *playerName,
                           const char *fallbackCountry,
                           uint8_t clientType, uint8_t clientFlags) {
  return clientSimConnectLocalBody(cs, sim, playerName, fallbackCountry,
                                   clientType, clientFlags, /*passive=*/false);
}

bool clientSimConnectLocalPassive(ClientSim *cs, struct ServerSim *sim,
                                  const char *playerName,
                                  const char *fallbackCountry,
                                  uint8_t clientType, uint8_t clientFlags) {
  return clientSimConnectLocalBody(cs, sim, playerName, fallbackCountry,
                                   clientType, clientFlags, /*passive=*/true);
}

void clientSimDisconnect(ClientSim *cs) {
  if (cs == NULL) return;
  /* Unregister the auto-subscriber BEFORE teardown so boundServerSim
   * is still valid. clientSimConnectLocal{,Passive} stashed the
   * handle into cs->autoSubHandle; the UDP path leaves it
   * SUBSCRIBER_HANDLE_INVALID and the check below short-circuits. */
  if (cs->boundServerSim != NULL &&
      cs->autoSubHandle != SUBSCRIBER_HANDLE_INVALID) {
    serverSimUnregisterSubscriber(cs->boundServerSim, cs->autoSubHandle);
    cs->autoSubHandle = SUBSCRIBER_HANDLE_INVALID;
  }
  clientSimTeardownTransport(cs);
  clientSimSetBoundServerSim(cs, NULL);
}

bool clientSimHasTransport(const ClientSim *cs) {
  return cs != NULL && cs->hasTransport;
}

/* === Per-tick driver === */

void clientSimNetTick(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  cs->transport.tick(cs->transport.ctx);
}

void clientSimNetSendInput(ClientSim *cs, const InputPacket *pkt) {
  if (cs == NULL || !cs->hasTransport) return;
  cs->transport.sendInput(cs->transport.ctx, pkt);
}

void clientSimNetRecordInput(ClientSim *cs, const InputPacket *pkt) {
  if (cs == NULL || !cs->hasTransport) return;
  cs->transport.recordInput(cs->transport.ctx, pkt);
}

bool clientSimNetGetSnapshot(ClientSim *cs, SnapshotHeader *hdr,
                             TankSnapshot *tanks, int maxTanks,
                             ShellSnapshot *shells, int maxShells,
                             TkExplosionSnapshot *tkExpl, int maxTkExpl,
                             BaseSnapshot *bases, int maxBases,
                             PillSnapshot *pills, int maxPills,
                             GameEvent *events, int maxEvents) {
  if (cs == NULL || !cs->hasTransport) return false;
  return cs->transport.getSnapshot(cs->transport.ctx, cs->myPlayerNum, hdr,
                                   tanks, maxTanks, shells, maxShells,
                                   tkExpl, maxTkExpl, bases, maxBases,
                                   pills, maxPills, events, maxEvents);
}

bool clientSimNetSyncSnapshot(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return false;
  SnapshotHeader snapHdr;
  TankSnapshot snapTanks[MAX_TANKS];
  ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
  TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
  BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
  PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
  GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
  if (!cs->transport.getSnapshot(cs->transport.ctx, cs->myPlayerNum, &snapHdr,
                                 snapTanks, MAX_TANKS,
                                 snapShells, MAX_SNAPSHOT_SHELLS,
                                 snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                 snapBases, MAX_SNAPSHOT_BASES,
                                 snapPills, MAX_SNAPSHOT_PILLS,
                                 snapEvents, MAX_SNAPSHOT_EVENTS)) {
    return false;
  }
  clientSimSyncFromSnapshot(cs, &snapHdr,
                            snapTanks, snapHdr.tankCount,
                            snapShells, snapHdr.shellCount,
                            snapTkExplosions, snapHdr.tkExplosionCount,
                            snapBases, snapHdr.baseCount,
                            snapPills, snapHdr.pillCount,
                            snapEvents, snapHdr.reliableEventCount,
                            cs->myPlayerNum);
  return true;
}

/* === State queries === */

ClientConnectState clientSimGetConnectState(const ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return CLIENT_CONNECT_DISCONNECTED;
  if (!cs->isUdpTransport) return CLIENT_CONNECT_CONNECTED;
  return transportUdpClientGetJoinState((Transport *)&cs->transport);
}

const char *clientSimGetConnectErrorReason(const ClientSim *cs) {
  if (cs == NULL || cs->connectErrorReason[0] == '\0') return NULL;
  return cs->connectErrorReason;
}

BYTE clientSimGetServerPlayerNum(const ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return 0;
  return transportUdpClientGetPlayerNum((Transport *)&cs->transport);
}

const BYTE *clientSimGetServerMapData(const ClientSim *cs, int *outLen) {
  if (outLen) *outLen = 0;
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return NULL;
  return transportUdpClientGetMapData((Transport *)&cs->transport, outLen);
}

/* === Send wrappers ===
 * Most sends are still UDP-only (local transport ignores them) — those
 * wrappers continue to early-return on a non-UDP transport. The chat
 * wrapper below is the first to drop that gate: it builds the wire
 * packet itself and pushes it through the transport's sendBytes hook,
 * so bots (on a passive local transport) reach the server through the
 * same call path as a UDP-connected human. As features that need bot
 * participation come online — alliance, vote, team set, etc. — apply
 * the same pattern to those wrappers and mirror the matching handler
 * in transport_local.c's localSendBytes. */

void clientSimNetSendChat(ClientSim *cs, BYTE destPlayer, const char *message) {
  uint8_t buf[PACKET_HEADER_SIZE + 1 + PACKET_MAX_CHAT_MESSAGE];
  size_t msgLen;
  size_t len;

  if (cs == NULL || !cs->hasTransport) return;
  if (cs->transport.sendBytes == NULL) return;
  if (message == NULL || message[0] == '\0') return;

  msgLen = strlen(message);
  if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;

  /* PACKET_CHAT_MESSAGE wire format:
   *   [magic 2][type 1][reserved 1][sequence 4][destPlayer 1][message...]
   * Sequence stays zero — the server's chat handler doesn't consume it
   * (UDP retransmit/redundancy applies to PACKET_INPUT only). */
  buf[0] = BOLO_NEW_MAGIC_0;
  buf[1] = BOLO_NEW_MAGIC_1;
  buf[2] = PACKET_CHAT_MESSAGE;
  buf[3] = 0;
  memset(buf + 4, 0, 4);
  buf[PACKET_HEADER_SIZE] = destPlayer;
  memcpy(buf + PACKET_HEADER_SIZE + 1, message, msgLen);
  len = PACKET_HEADER_SIZE + 1 + msgLen;

  cs->transport.sendBytes(cs->transport.ctx, buf, len);
}

void clientSimNetSendNameChange(ClientSim *cs, const char *newName) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendNameChange(&cs->transport, newName);
}

void clientSimNetSendAllianceRequest(ClientSim *cs, BYTE toPlayer) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendAllianceRequest(&cs->transport, toPlayer);
}

void clientSimNetSendAllianceAccept(ClientSim *cs, BYTE toPlayer) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendAllianceAccept(&cs->transport, toPlayer);
}

void clientSimNetSendAllianceLeave(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendAllianceLeave(&cs->transport);
}

void clientSimNetSendLockToggle(ClientSim *cs, bool allow) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLockToggle(&cs->transport, allow);
}

void clientSimNetSendTeamSet(ClientSim *cs, BYTE slot, BYTE teamNumber) {
  if (cs == NULL || !cs->hasTransport) return;
  if (cs->isUdpTransport) {
    /* Wire side: server uses clientIdx for the apply regardless of
     * the slot byte we send; non-self moves are SP-host-only. */
    transportUdpClientSendTeamSet(&cs->transport, teamNumber);
    return;
  }
  /* Local transport: mirror PACKET_LOBBY_TEAM_SET's server-side
   * handler. Slot comes from the caller (SP-host can move any
   * lobby slot, including bot slots); team must fit MAX_TANKS. */
  if (cs->boundServerSim == NULL) return;
  threadsWaitForMutex();
  do {
    ServerSim *sim = cs->boundServerSim;
    if (!serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby) break;
    if (slot >= MAX_TANKS || teamNumber >= MAX_TANKS) break;
    serverSimSetTeam(sim, slot, teamNumber);
    serverSimPublishLobbySlot(sim, slot);
    lobbyAutoUnreadyOnChange(sim);
  } while (0);
  threadsReleaseMutex();
}

void clientSimNetSendReady(ClientSim *cs, bool ready) {
  if (cs == NULL || !cs->hasTransport) return;
  if (cs->isUdpTransport) {
    transportUdpClientSendReady(&cs->transport, ready);
    return;
  }
  /* Local transport: drive the server-side ready toggle directly.
   * Mirrors the PACKET_LOBBY_READY handler at transport_udp_server.c:
   *   setReady → serverSimPublishLobbySlot → LobbyCheckAllReady. The all-ready
   * detector picks the worldPreLoaded branch (StartGameInPlace) on a
   * fresh SP sim, or the countdown branch on subsequent rounds. */
  if (cs->boundServerSim == NULL) return;
  threadsWaitForMutex();
  if (serverSimIsLobbyEnabled(cs->boundServerSim) &&
      serverSimGetState(cs->boundServerSim) == serverStateLobby) {
    BYTE slot = clientSimGetMyPlayerNum(cs);
    serverSimSetReady(cs->boundServerSim, slot, ready);
    {
      ControlEvent slotEvt;
      memset(&slotEvt, 0, sizeof(slotEvt));
      serverSimFillLobbySlotEvent(cs->boundServerSim, slot, &slotEvt);
      serverSimPublishControl(cs->boundServerSim, &slotEvt);
    }
    serverSimLobbyCheckAllReady(cs->boundServerSim);
  }
  threadsReleaseMutex();
}

void clientSimNetSendAddBot(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendAddBot(&cs->transport, 0, NULL);
}

void clientSimNetSendAddBotConfigured(ClientSim *cs, BYTE teamNumber,
                                      uint8_t brainIdx,
                                      const char *botName) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  /* brainIdx accepted for API symmetry; the server applies the default
   * brain on add. Use clientSimNetSendLobbySetBotBrain to change it. */
  (void)brainIdx;
  transportUdpClientSendAddBot(&cs->transport, teamNumber, botName);
}

void clientSimNetSendRemoveBot(ClientSim *cs, BYTE playerNum) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendRemoveBot(&cs->transport, playerNum);
}

void clientSimNetSendLobbyBotConfig(ClientSim *cs, BYTE slot,
                                    uint8_t difficulty,
                                    uint8_t personality,
                                    const char *name) {
  if (cs == NULL || !cs->hasTransport) return;
  if (cs->isUdpTransport) {
    transportUdpClientSendLobbyBotConfig(&cs->transport, slot,
                                         difficulty, personality, name);
    return;
  }
  /* Local transport: mirror the PACKET_LOBBY_BOT_CONFIG handler in
   * transport_udp_server.c so SP-host produces the same sim state as
   * a UDP host. Held under the threads mutex so the timer thread
   * can't race the apply. Empty name == "keep current name", same
   * as the wire path's nameLen == 0 branch. */
  if (cs->boundServerSim == NULL) return;
  threadsWaitForMutex();
  do {
    ServerSim *sim = cs->boundServerSim;
    if (!serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby) break;
    if (slot >= MAX_TANKS || difficulty > 2 || personality > 3) break;
    if (!serverSimIsBot(sim, slot)) break;

    char validatedName[PACKET_MAX_PLAYER_NAME];
    const bool haveName = (name != NULL && name[0] != '\0');
    if (haveName) {
      if (strlen(name) > 31) break;
      /* Format validation only. Uniqueness was already enforced by
       * the SP-host caller against the ClientSim's lobby mirror,
       * which on SP-host is what serverSimRenameBotSlot writes
       * through — that mirror is the canonical name table here.
       * The wire-side handler uses udpServer.clients[] for the same
       * check, but that table is unused on SP-host. */
      if (!lobbyBotNameAcceptable(name, validatedName, sizeof(validatedName),
                                  (int)slot, NULL, NULL, NULL)) {
        break;
      }
    }

    /* SP-host has no UDP server, so the wire handler's
     * transportUdpServerSetBotName step is a no-op here.
     * serverSimSetBotConfig handles the write, the optional rename
     * (sim.plyrs + CTRL_PLAYER_NAME via serverSimRenameBotSlot), the
     * config + slot publishes, and the auto-unready tail. */
    serverSimSetBotConfig(sim, slot, difficulty, personality,
                          haveName ? validatedName : NULL);
  } while (0);
  threadsReleaseMutex();
}

void clientSimNetSendLobbySetBotBrain(ClientSim *cs, BYTE slot,
                                      uint8_t brainIdx) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbySetBotBrain(&cs->transport, slot, brainIdx);
}

void clientSimNetSendLobbySetMap(ClientSim *cs, const char *mapRelPath) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbySetMap(&cs->transport, mapRelPath);
}

void clientSimNetSendLobbyPreviewCancel(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyPreviewCancel(&cs->transport);
}

void clientSimNetSendLobbyPreviewCommit(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyPreviewCommit(&cs->transport);
}

void clientSimNetSendLobbyPreviewRandom(ClientSim *cs, const char *seedStr) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyPreviewRandom(&cs->transport, seedStr);
}

void clientSimNetSendLobbyMapListRequest(ClientSim *cs,
                                         const char *relPath) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyMapListRequest(&cs->transport, relPath);
}

void clientSimNetSendLobbyMapSearchRequest(ClientSim *cs,
                                           const char *relPath,
                                           const char *query) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyMapSearchRequest(&cs->transport,
                                              relPath, query);
}

bool clientSimNetSendLobbyMapUpload(ClientSim *cs, const char *localFilePath) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return false;
  return transportUdpClientStartLobbyMapUploadFromPath(&cs->transport,
                                                       localFilePath);
}

bool clientSimNetSendLobbyMapUploadBytes(ClientSim *cs,
                                         const uint8_t *buf, size_t len,
                                         const char *mapName) {
  if (cs == NULL || !cs->hasTransport) return false;
  if (cs->isUdpTransport) {
    return transportUdpClientStartLobbyMapUploadFromBytes(&cs->transport,
                                                          buf, len, mapName);
  }
  /* Local transport: SP-host has no chunked upload — the bytes are
   * already in process, so install them straight onto the sim as a
   * lobby preview. Mirrors the wire-side UPLOAD_DONE tail in
   * transport_udp_server.c (reload + publish settings; the multi-
   * client MAP_CHANGE broadcast is wire-only). */
  if (cs->boundServerSim == NULL) return false;
  bool ok = false;
  threadsWaitForMutex();
  do {
    ServerSim *sim = cs->boundServerSim;
    if (!serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby) break;
    if (buf == NULL || len == 0 || len > (size_t)INT_MAX) break;
    if (!serverSimReloadCompressedInMemory(sim, buf, (int)len, mapName)) break;
    ok = true;
  } while (0);
  threadsReleaseMutex();
  return ok;
}

uint8_t clientSimGetLobbyMapUploadProgressPercent(const ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return 0;
  return transportUdpClientGetLobbyMapUploadProgressPercent(
      (Transport *)&cs->transport);
}

void clientSimNetSendLobbyMapUseLocal(ClientSim *cs,
                                      uint32_t totalLen,
                                      const char *name,
                                      const char *relPath,
                                      const uint8_t md5[16]) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyMapUseLocal(&cs->transport, totalLen, name,
                                          relPath, md5);
}

void clientSimNetSendLobbyTeamMeta(ClientSim *cs, BYTE teamId,
                                   uint8_t color, uint8_t namingPool,
                                   const char *name) {
  if (cs == NULL || !cs->hasTransport) return;
  if (cs->isUdpTransport) {
    transportUdpClientSendLobbyTeamMeta(&cs->transport, teamId, color,
                                        namingPool, name);
    return;
  }
  /* Local transport: delegate to the shared setter so SP-host and
   * wire follow one code path (including the pool-uniqueness rewrite
   * and the publish + auto-unready tail). */
  if (cs->boundServerSim == NULL) return;
  threadsWaitForMutex();
  do {
    ServerSim *sim = cs->boundServerSim;
    if (!serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby) break;
    if (teamId == 0 || teamId >= MAX_TANKS) break;
    size_t nameLen = (name != NULL) ? strlen(name) : 0;
    if (nameLen > LOBBY_TEAM_NAME_LEN - 1) break;
    serverSimSetTeamMeta(sim, teamId, color, namingPool,
                         (const uint8_t *)name, (uint8_t)nameLen);
  } while (0);
  threadsReleaseMutex();
}

void clientSimNetSendLobbyTeamClear(ClientSim *cs, BYTE teamId) {
  if (cs == NULL || !cs->hasTransport) return;
  if (cs->isUdpTransport) {
    transportUdpClientSendLobbyTeamClear(&cs->transport, teamId);
    return;
  }
  /* Local transport: delegate to the shared setter. */
  if (cs->boundServerSim == NULL) return;
  threadsWaitForMutex();
  do {
    ServerSim *sim = cs->boundServerSim;
    if (!serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby) break;
    if (teamId == 0 || teamId >= MAX_TANKS) break;
    serverSimClearTeamMeta(sim, teamId);
  } while (0);
  threadsReleaseMutex();
}

void clientSimNetSendLobbySetting(ClientSim *cs, uint8_t settingType,
                                  const uint8_t *value, uint8_t valueLen) {
  if (cs == NULL || !cs->hasTransport) return;
  if (cs->isUdpTransport) {
    transportUdpClientSendLobbySetting(&cs->transport, settingType, value, valueLen);
    return;
  }
  /* Local transport: SP-host is its own operator, so the lock-bit /
   * authority gates the UDP handler runs aren't applicable. The
   * shared helper publishes settings + clears humans' ready state
   * on success. */
  if (cs->boundServerSim == NULL) return;
  threadsWaitForMutex();
  do {
    ServerSim *sim = cs->boundServerSim;
    if (!serverSimIsLobbyEnabled(sim) ||
        serverSimGetState(sim) != serverStateLobby) break;
    serverSimApplyLobbySetting(sim, settingType, value, valueLen);
  } while (0);
  threadsReleaseMutex();
}

void clientSimNetSendLobbySetPassword(ClientSim *cs, const char *pw) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbySetPassword(&cs->transport, pw);
}

void clientSimNetSendLobbyOpenHost(ClientSim *cs, bool openHost) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyOpenHost(&cs->transport, openHost);
}

void clientSimNetSendLobbyKick(ClientSim *cs, uint8_t slot) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyKick(&cs->transport, slot);
}

void clientSimNetSendMapSkipVote(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendMapSkipVote(&cs->transport);
}

void clientSimNetSendGameVoteToggle(ClientSim *cs,
                                    uint8_t kind, uint8_t toggleMode) {
  /* Network path only. SP / host-in-process dispatch lives in the GUI
   * shim (sdl3imgui.cpp) so the bolo library doesn't pick up a build
   * dep on server_sim. */
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendGameVoteToggle(&cs->transport, kind, toggleMode);
}

void clientSimNetSendBalanceRequest(ClientSim *cs, BYTE teamSize,
                                    bool includeBots) {
  balanceDebugLog("[BAL CLIENT] SendBalanceRequest teamSize=%u includeBots=%d "
                  "cs=%p hasTransport=%d isUdpTransport=%d",
                  (unsigned)teamSize, includeBots ? 1 : 0,
                  (void *)cs,
                  cs ? cs->hasTransport : 0,
                  cs ? cs->isUdpTransport : 0);
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) {
    balanceDebugLog("[BAL CLIENT] dropping: no UDP transport bound");
    return;
  }
  balanceDebugLog("[BAL CLIENT] calling transportUdpClientSendBalanceRequest");
  transportUdpClientSendBalanceRequest(&cs->transport, teamSize, includeBots);
  balanceDebugLog("[BAL CLIENT] transportUdpClientSendBalanceRequest returned");
}

void clientSimNetSendBalanceApply(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendBalanceApply(&cs->transport);
}

void clientSimNetSendBalanceDismiss(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendBalanceDismiss(&cs->transport);
}

void clientSimNetSendWbnReauth(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendWbnReauth(&cs->transport);
}

/* === Net stats === */

uint16_t clientSimGetNetPing(const ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return 0;
  return transportUdpClientGetPing((Transport *)&cs->transport);
}

void clientSimGetUdpNetStats(ClientSim *cs, int *ppsRecv, int *ppsSent,
                             int *bpsRecv, int *bpsSent, int *numErrors) {
  if (ppsRecv) *ppsRecv = 0;
  if (ppsSent) *ppsSent = 0;
  if (bpsRecv) *bpsRecv = 0;
  if (bpsSent) *bpsSent = 0;
  if (numErrors) *numErrors = 0;
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientGetNetStats(&cs->transport, ppsRecv, ppsSent,
                                bpsRecv, bpsSent, numErrors);
}

/* === Local-transport tuning === */

void clientSimNetSetLocalDelay(ClientSim *cs, uint16_t delay_ms) {
  if (cs == NULL || !cs->hasTransport || cs->isUdpTransport) return;
  transportLocalSetDelay(&cs->transport, delay_ms);
}

uint16_t clientSimNetGetLocalDelay(const ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || cs->isUdpTransport) return 0;
  return transportLocalGetDelay((Transport *)&cs->transport);
}
