/*
 * client_net.h
 *
 * Public API that lets ClientSim own the network transport. External
 * callers (desktop GUI, android/wasm glue, headless, gym, lobby
 * dialogs) drive the connection through these wrappers instead of
 * holding a raw Transport pointer.
 *
 * Lifecycle: clientSimConnectUdp or clientSimConnectLocal stashes a
 * Transport inside the ClientSim. The matching clientSimDisconnect
 * tears it down (clientSimDestroy also covers a forgotten disconnect).
 */

#ifndef CLIENT_NET_H
#define CLIENT_NET_H

#include "client_sim.h"
#include "client_connect_state.h"
#include "input_packet.h"        /* SnapshotHeader, TankSnapshot, etc. */
#include "gametype.h"

struct ServerSim;

/* === Lifecycle === */
bool clientSimConnectUdp(ClientSim *cs, const char *serverAddr,
                         unsigned short serverPort, const char *playerName,
                         const char *password, const char *wbnToken,
                         bool wantRejoin, const char *trackerAddr,
                         unsigned short trackerPort);
bool clientSimConnectLocal(ClientSim *cs, struct ServerSim *sim, BYTE playerNum);
void clientSimDisconnect(ClientSim *cs);
bool clientSimHasTransport(const ClientSim *cs);

/* === Per-tick driver === */
void clientSimNetTick(ClientSim *cs);
void clientSimNetSendInput(ClientSim *cs, const InputPacket *pkt);
void clientSimNetRecordInput(ClientSim *cs, const InputPacket *pkt);
bool clientSimNetGetSnapshot(ClientSim *cs, SnapshotHeader *hdr,
                             TankSnapshot *tanks, int maxTanks,
                             ShellSnapshot *shells, int maxShells,
                             TkExplosionSnapshot *tkExpl, int maxTkExpl,
                             BaseSnapshot *bases, int maxBases,
                             PillSnapshot *pills, int maxPills,
                             GameEvent *events, int maxEvents);
bool clientSimNetSyncSnapshot(ClientSim *cs);

/* === State queries === */
ClientConnectState clientSimGetConnectState(const ClientSim *cs);
const char *clientSimGetConnectErrorReason(const ClientSim *cs);
BYTE        clientSimGetServerPlayerNum(const ClientSim *cs);
const BYTE *clientSimGetServerMapData(const ClientSim *cs, int *outLen);
void        clientSimGetServerGameSettings(const ClientSim *cs, gameType *game,
                                           bool *hiddenMines, int32_t *startDelay,
                                           int32_t *gameLen);

/* === Send wrappers === */
void clientSimNetSendChat(ClientSim *cs, BYTE destPlayer, const char *message);
void clientSimNetSendNameChange(ClientSim *cs, const char *newName);
void clientSimNetSendAllianceRequest(ClientSim *cs, BYTE toPlayer);
void clientSimNetSendAllianceAccept(ClientSim *cs, BYTE toPlayer);
void clientSimNetSendAllianceLeave(ClientSim *cs);
void clientSimNetSendLockToggle(ClientSim *cs, bool allow);
void clientSimNetSendTeamSet(ClientSim *cs, BYTE teamNumber);
void clientSimNetSendReady(ClientSim *cs, bool ready);
void clientSimNetSendAddBot(ClientSim *cs);
void clientSimNetSendRemoveBot(ClientSim *cs, BYTE playerNum);
void clientSimNetSendMapSkipVote(ClientSim *cs);
void clientSimNetSendBalanceRequest(ClientSim *cs, BYTE teamSize);
void clientSimNetSendBalanceApply(ClientSim *cs);
void clientSimNetSendBalanceDismiss(ClientSim *cs);
void clientSimNetSendWbnReauth(ClientSim *cs);

/* === Net stats === */
uint16_t clientSimGetNetPing(const ClientSim *cs);
void     clientSimGetUdpNetStats(ClientSim *cs, int *ppsRecv, int *ppsSent,
                                 int *bpsRecv, int *bpsSent, int *numErrors);

/* === Local-transport tuning === */
void     clientSimNetSetLocalDelay(ClientSim *cs, uint16_t delay_ms);
uint16_t clientSimNetGetLocalDelay(const ClientSim *cs);

#endif /* CLIENT_NET_H */
