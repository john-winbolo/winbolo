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
#include "transport.h"
#include "transport_udp.h"
#include "input_packet.h"
#include "global.h"

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
                         const char *password, const char *wbnToken,
                         bool wantRejoin, const char *trackerAddr,
                         unsigned short trackerPort) {
  if (cs == NULL) return false;
  clientSimTeardownTransport(cs);
  cs->transport = transportUdpClientCreate(cs, serverAddr, serverPort,
                                           playerName, password, wbnToken,
                                           wantRejoin, trackerAddr, trackerPort);
  cs->hasTransport = true;
  cs->isUdpTransport = true;
  clientSimSetLocalTransport(cs, false);
  return true;
}

bool clientSimConnectLocal(ClientSim *cs, struct ServerSim *sim, BYTE playerNum) {
  if (cs == NULL) return false;
  clientSimTeardownTransport(cs);
  cs->transport = transportLocalCreate(sim, playerNum);
  cs->hasTransport = true;
  cs->isUdpTransport = false;
  clientSimSetLocalTransport(cs, true);
  return true;
}

void clientSimDisconnect(ClientSim *cs) {
  if (cs == NULL) return;
  clientSimTeardownTransport(cs);
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
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return NULL;
  return transportUdpClientGetJoinRejectReason((Transport *)&cs->transport);
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

void clientSimGetServerGameSettings(const ClientSim *cs, gameType *game,
                                    bool *hiddenMines, int32_t *startDelay,
                                    int32_t *gameLen) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientGetGameSettings((Transport *)&cs->transport, game,
                                    hiddenMines, startDelay, gameLen);
}

/* === Send wrappers ===
 * Sends are UDP-only — local transport ignores them. The (!isUdpTransport)
 * branch is a no-op so single-player callers don't have to gate calls. */

void clientSimNetSendChat(ClientSim *cs, BYTE destPlayer, const char *message) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendChat(&cs->transport, destPlayer, message);
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

void clientSimNetSendTeamSet(ClientSim *cs, BYTE teamNumber) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendTeamSet(&cs->transport, cs->myPlayerNum, teamNumber);
}

void clientSimNetSendReady(ClientSim *cs, bool ready) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendReady(&cs->transport, ready);
}

void clientSimNetSendAddBot(ClientSim *cs) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendAddBot(&cs->transport, 0, NULL, NULL);
}

void clientSimNetSendAddBotConfigured(ClientSim *cs, BYTE teamNumber,
                                      const char *brainPath,
                                      const char *botName) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendAddBot(&cs->transport, teamNumber, brainPath, botName);
}

void clientSimNetSendRemoveBot(ClientSim *cs, BYTE playerNum) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendRemoveBot(&cs->transport, playerNum);
}

void clientSimNetSendLobbyBotConfig(ClientSim *cs, BYTE slot,
                                    uint8_t difficulty,
                                    uint8_t personality,
                                    const char *name) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyBotConfig(&cs->transport, slot,
                                       difficulty, personality, name);
}

void clientSimNetSendLobbySetBotBrain(ClientSim *cs, BYTE slot,
                                      const char *brainPath) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbySetBotBrain(&cs->transport, slot, brainPath);
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

void clientSimNetSendLobbyMapUploadBegin(ClientSim *cs,
                                         uint32_t totalLen,
                                         const char *name) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyMapUploadBegin(&cs->transport, totalLen, name);
}

void clientSimNetSendLobbyMapUploadChunk(ClientSim *cs,
                                         uint32_t offset,
                                         const uint8_t *data,
                                         uint16_t dataLen) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyMapUploadChunk(&cs->transport, offset,
                                            data, dataLen);
}

void clientSimNetSendLobbyTeamMeta(ClientSim *cs, BYTE teamId,
                                   uint8_t color, uint8_t namingPool,
                                   const char *name) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyTeamMeta(&cs->transport, teamId, color,
                                      namingPool, name);
}

void clientSimNetSendLobbyTeamClear(ClientSim *cs, BYTE teamId) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyTeamClear(&cs->transport, teamId);
}

void clientSimNetSendLobbySetting(ClientSim *cs, uint8_t settingType,
                                  const uint8_t *value, uint8_t valueLen) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbySetting(&cs->transport, settingType, value, valueLen);
}

void clientSimNetSendLobbySetPassword(ClientSim *cs, const char *pw) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbySetPassword(&cs->transport, pw);
}

void clientSimNetSendLobbyOpenHost(ClientSim *cs, bool openHost) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendLobbyOpenHost(&cs->transport, openHost);
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

void clientSimNetSendBalanceRequest(ClientSim *cs, BYTE teamSize) {
  if (cs == NULL || !cs->hasTransport || !cs->isUdpTransport) return;
  transportUdpClientSendBalanceRequest(&cs->transport, teamSize);
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
