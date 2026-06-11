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
#include "../gui/dnsLookups.h"             /* server-address reverse DNS thread */
#include "../common/wb_log.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* === Lifecycle === */

static void clientSimTeardownTransport(ClientSim *cs) {
  if (!cs->hasTransport) return;
  if (cs->isUdpTransport) {
    /* Stop the server-address reverse-DNS thread started in
     * clientSimConnectUdp. Safe / no-op if it was never created. */
    dnsLookupsDestroy();
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
  /* Start the background reverse-DNS thread so the lobby / net-info screens
   * can resolve the server IP to a hostname (visual only; never used to
   * connect). Torn down in clientSimTeardownTransport. The prior teardown at
   * the top of this function already stopped any earlier instance, so this
   * never double-creates. */
  dnsLookupsCreate(cs);
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
 * Command helpers build a ClientCommand from their arguments and hand
 * it to clientSimSubmitCommand, which routes through the reliable
 * carrier on UDP or serverSimApplyCommand under the mutex on local. A
 * few bespoke-channel helpers (snapshot input, map list/search, map
 * upload, MapUseLocal, WbnReauth) keep their own UDP-only paths. */

void clientSimNetSendChat(ClientSim *cs, BYTE destPlayer, const char *message) {
  if (cs == NULL || !cs->hasTransport) return;
  if (message == NULL || message[0] == '\0') return;
  size_t msgLen = strlen(message);
  if (msgLen > PACKET_MAX_CHAT_MESSAGE) msgLen = PACKET_MAX_CHAT_MESSAGE;
  ClientCommand cmd = { .type = CMD_CHAT };
  cmd.u.chat.destPlayer = destPlayer;
  cmd.u.chat.bodyLen    = (uint16_t)msgLen;
  memcpy(cmd.u.chat.body, message, msgLen);
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendNameChange(ClientSim *cs, const char *newName) {
  if (cs == NULL || !cs->hasTransport) return;
  if (newName == NULL || newName[0] == '\0') return;
  ClientCommand cmd = { .type = CMD_NAME_CHANGE };
  size_t nl = strlen(newName);
  if (nl >= PACKET_MAX_PLAYER_NAME) nl = PACKET_MAX_PLAYER_NAME - 1;
  memcpy(cmd.u.nameChange.newName, newName, nl);
  cmd.u.nameChange.newName[nl] = '\0';
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendAllianceRequest(ClientSim *cs, BYTE toPlayer) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_ALLIANCE_REQUEST };
  cmd.u.allianceRequest.toPlayer = toPlayer;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendAllianceAccept(ClientSim *cs, BYTE toPlayer) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_ALLIANCE_ACCEPT };
  cmd.u.allianceAccept.newMember = toPlayer;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendAllianceLeave(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_ALLIANCE_LEAVE };
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLockToggle(ClientSim *cs, bool allow) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOCK_TOGGLE };
  cmd.u.lockToggle.allow = allow;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimSubmitCommand(ClientSim *cs, const ClientCommand *cmd) {
  if (cs == NULL || !cs->hasTransport || cmd == NULL) return;
  if (cs->isUdpTransport) {
    transportUdpClientSubmitCommand(&cs->transport, cmd);
    return;
  }
  if (cs->boundServerSim == NULL) return;
  threadsWaitForMutex();
  (void)serverSimApplyCommand(cs->boundServerSim,
                              clientSimGetMyPlayerNum(cs), cmd);
  threadsReleaseMutex();
}

void clientSimNetSendTeamSet(ClientSim *cs, BYTE slot, BYTE teamNumber) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_TEAM_SET };
  cmd.u.teamSet.slot = slot;
  cmd.u.teamSet.team = teamNumber;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendReady(ClientSim *cs, bool ready) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_READY };
  cmd.u.ready.ready = ready;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendAddBot(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_ADD_BOT };
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendAddBotConfigured(ClientSim *cs, BYTE teamNumber,
                                      uint8_t brainIdx,
                                      const char *botName) {
  if (cs == NULL || !cs->hasTransport) return;
  /* brainIdx accepted for API symmetry; the server applies the default
   * brain on add. Use clientSimNetSendLobbySetBotBrain to change it. */
  (void)brainIdx;
  ClientCommand cmd = { .type = CMD_LOBBY_ADD_BOT };
  cmd.u.lobbyAddBot.teamNumber = teamNumber;
  if (botName != NULL && botName[0] != '\0') {
    size_t nl = strlen(botName);
    if (nl >= sizeof(cmd.u.lobbyAddBot.name)) return;
    cmd.u.lobbyAddBot.nameLen = (uint8_t)nl;
    memcpy(cmd.u.lobbyAddBot.name, botName, nl);
  }
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendRemoveBot(ClientSim *cs, BYTE playerNum) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_REMOVE_BOT };
  cmd.u.lobbyRemoveBot.slot = playerNum;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyBotConfig(ClientSim *cs, BYTE slot,
                                    uint8_t difficulty,
                                    uint8_t personality,
                                    const char *name) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_BOT_CONFIG };
  cmd.u.lobbyBotConfig.slot        = slot;
  cmd.u.lobbyBotConfig.difficulty  = difficulty;
  cmd.u.lobbyBotConfig.personality = personality;
  if (name != NULL && name[0] != '\0') {
    size_t nl = strlen(name);
    if (nl > 31) return;  /* matches wire-side nameLen<=31 guard */
    cmd.u.lobbyBotConfig.nameLen = (uint8_t)nl;
    memcpy(cmd.u.lobbyBotConfig.name, name, nl);
  }
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbySetBotBrain(ClientSim *cs, BYTE slot,
                                      uint8_t brainIdx) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_SET_BOT_BRAIN };
  cmd.u.lobbySetBotBrain.slot     = slot;
  cmd.u.lobbySetBotBrain.brainIdx = brainIdx;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbySetMap(ClientSim *cs, const char *mapRelPath) {
  if (cs == NULL || !cs->hasTransport) return;
  if (mapRelPath == NULL) return;
  size_t pl = strlen(mapRelPath);
  if (pl == 0 || pl > 255) return;
  ClientCommand cmd = { .type = CMD_LOBBY_SET_MAP };
  cmd.u.lobbySetMap.relPathLen = (uint8_t)pl;
  memcpy(cmd.u.lobbySetMap.relPath, mapRelPath, pl);
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyPreviewCancel(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_PREVIEW_CANCEL };
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyPreviewCommit(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_PREVIEW_COMMIT };
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyPreviewRandom(ClientSim *cs, const char *seedStr) {
  if (cs == NULL || !cs->hasTransport) return;
  if (seedStr == NULL) return;
  size_t sl = strlen(seedStr);
  if (sl == 0 || sl > 63) return;
  ClientCommand cmd = { .type = CMD_LOBBY_PREVIEW_RANDOM };
  cmd.u.lobbyPreviewRandom.seedLen = (uint8_t)sl;
  memcpy(cmd.u.lobbyPreviewRandom.seed, seedStr, sl);
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyMapListRequest(ClientSim *cs,
                                         const char *relPath) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyMapListRequest(&cs->transport, relPath);
}

void clientSimNetSendLobbyMapPreviewRequest(ClientSim *cs,
                                            const char *relPath) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyMapPreviewRequest(&cs->transport, relPath);
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
  ClientCommand cmd = { .type = CMD_LOBBY_TEAM_META };
  cmd.u.lobbyTeamMeta.teamId     = teamId;
  cmd.u.lobbyTeamMeta.color      = color;
  cmd.u.lobbyTeamMeta.namingPool = namingPool;
  if (name != NULL) {
    size_t nl = strlen(name);
    if (nl > LOBBY_TEAM_NAME_LEN - 1) return;
    cmd.u.lobbyTeamMeta.nameLen = (uint8_t)nl;
    memcpy(cmd.u.lobbyTeamMeta.name, name, nl);
  }
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyTeamClear(ClientSim *cs, BYTE teamId) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_TEAM_CLEAR };
  cmd.u.lobbyTeamClear.teamId = teamId;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbySetting(ClientSim *cs, uint8_t settingType,
                                  const uint8_t *value, uint8_t valueLen) {
  if (cs == NULL || !cs->hasTransport) return;
  if (valueLen > 32) return;
  ClientCommand cmd = { .type = CMD_LOBBY_SETTING };
  cmd.u.lobbySetting.settingType = settingType;
  cmd.u.lobbySetting.valueLen    = valueLen;
  if (valueLen > 0 && value != NULL) {
    memcpy(cmd.u.lobbySetting.value, value, valueLen);
  }
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbySetPassword(ClientSim *cs, const char *pw) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_SET_PASSWORD };
  if (pw != NULL && pw[0] != '\0') {
    size_t pl = strlen(pw);
    if (pl >= sizeof(cmd.u.lobbySetPassword.password)) return;
    cmd.u.lobbySetPassword.pwLen = (uint8_t)pl;
    memcpy(cmd.u.lobbySetPassword.password, pw, pl);
  }
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyOpenHost(ClientSim *cs, bool openHost) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_OPEN_HOST };
  cmd.u.lobbyOpenHost.openHost = openHost;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyKick(ClientSim *cs, uint8_t slot) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_KICK };
  cmd.u.lobbyKick.slot = slot;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendLobbyTransferHost(ClientSim *cs, uint8_t slot) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_LOBBY_TRANSFER_HOST };
  cmd.u.lobbyTransferHost.slot = slot;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendMapSkipVote(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_MAP_SKIP_VOTE };
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendGameVoteToggle(ClientSim *cs,
                                    uint8_t kind, uint8_t toggleMode) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_GAME_VOTE_TOGGLE };
  cmd.u.gameVoteToggle.kind       = kind;
  cmd.u.gameVoteToggle.toggleMode = toggleMode;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendBalanceRequest(ClientSim *cs, BYTE teamSize,
                                    bool includeBots) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_BALANCE_REQUEST };
  cmd.u.balanceRequest.teamSize    = teamSize;
  cmd.u.balanceRequest.includeBots = includeBots;
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendBalanceApply(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_BALANCE_APPLY };
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendBalanceDismiss(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  ClientCommand cmd = { .type = CMD_BALANCE_DISMISS };
  clientSimSubmitCommand(cs, &cmd);
}

void clientSimNetSendWbnReauth(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport) return;
  if (cs->isUdpTransport) {
    transportUdpClientSendWbnReauth(&cs->transport);
    return;
  }
  if (cs->boundServerSim == NULL) return;
  ClientCommand cmd = { .type = CMD_WBN_REAUTH };  /* token left zeroed */
  threadsWaitForMutex();
  (void)serverSimApplyCommand(cs->boundServerSim,
                              clientSimGetMyPlayerNum(cs), &cmd);
  threadsReleaseMutex();
}

/* === Net stats === */

uint16_t clientSimGetNetPing(const ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return 0;
  return transportUdpClientGetPing((Transport *)&cs->transport);
}

void clientSimGetUdpNetStats(ClientSim *cs, int *ppsRecv, int *ppsSent,
                             int *bpsRecv, int *bpsSent, int *numErrors,
                             int *snapshotsRecv, int *snapshotsLost,
                             int *snapshotsLostTotal) {
  if (ppsRecv) *ppsRecv = 0;
  if (ppsSent) *ppsSent = 0;
  if (bpsRecv) *bpsRecv = 0;
  if (bpsSent) *bpsSent = 0;
  if (numErrors) *numErrors = 0;
  if (snapshotsRecv) *snapshotsRecv = 0;
  if (snapshotsLost) *snapshotsLost = 0;
  if (snapshotsLostTotal) *snapshotsLostTotal = 0;
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientGetNetStats(&cs->transport, ppsRecv, ppsSent,
                                bpsRecv, bpsSent, numErrors,
                                snapshotsRecv, snapshotsLost,
                                snapshotsLostTotal);
}

void clientSimGetReconcileStats(ClientSim *cs, int *countPerSec,
                                float *avgErrPx, float *maxErrPx) {
  if (countPerSec) *countPerSec = 0;
  if (avgErrPx) *avgErrPx = 0.0f;
  if (maxErrPx) *maxErrPx = 0.0f;
  if (cs == NULL) return;
  if (countPerSec) *countPerSec = cs->reconCountLastSec;
  if (avgErrPx) *avgErrPx = cs->reconErrAvgPxLast;
  if (maxErrPx) *maxErrPx = cs->reconErrMaxPxLast;
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
