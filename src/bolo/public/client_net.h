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
                         const char *password,
                         const char *wbnApiToken,
                         const char *wbnServerKey,
                         bool wantRejoin, const char *trackerAddr,
                         unsigned short trackerPort);
/* Run the full local-join handshake against an in-process ServerSim:
 * pick a slot via serverSimLocalJoin, install the server's compressed
 * map, set up the local tank, register the auto-subscriber, apply the
 * initial snapshot. Returns false and writes a rendered rejection
 * reason into cs (readable via clientSimGetConnectErrorReason) on
 * failure. clientType / clientFlags are recorded on the slot at join
 * time; fallbackCountry is used directly (no GeoIP). */
bool clientSimConnectLocal(ClientSim *cs, struct ServerSim *sim,
                           const char *playerName,
                           const char *fallbackCountry,
                           uint8_t clientType, uint8_t clientFlags);
/* Like clientSimConnectLocal, but constructs the local transport in
 * passive mode — the transport's tick path will NOT drive
 * serverSimTick. The caller is responsible for stepping the ServerSim
 * itself (e.g. via a host-side timer/finisher). */
bool clientSimConnectLocalPassive(ClientSim *cs, struct ServerSim *sim,
                                  const char *playerName,
                                  const char *fallbackCountry,
                                  uint8_t clientType, uint8_t clientFlags);
void clientSimDisconnect(ClientSim *cs);
bool clientSimHasTransport(const ClientSim *cs);

/* === Per-tick driver === */
void clientSimNetTick(ClientSim *cs);
void clientSimNetSendInput(ClientSim *cs, const InputPacket *pkt);
void clientSimNetRecordInput(ClientSim *cs, const InputPacket *pkt);
/* Build an InputPacket from key state + flags. Used by platform
 * frontends ahead of the client-side prediction tick
 * (clientSimGameTick / clientSimKeysTick) and the send-to-server
 * step (clientSimNetSendInput) or input record step
 * (clientSimNetRecordInput). The InputPacket type itself is the
 * public wire-input format defined in input_packet.h. */
void clientBuildInputPacket(ClientSim *cs, InputPacket *pkt,
                            tankButton tb,
                            bool isShoot, bool isMine,
                            bool isBrain, bool isGameTick,
                            BYTE playerNum, uint32_t tick);
bool clientSimNetGetSnapshot(ClientSim *cs, SnapshotHeader *hdr,
                             TankSnapshot *tanks, int maxTanks,
                             ShellSnapshot *shells, int maxShells,
                             TkExplosionSnapshot *tkExpl, int maxTkExpl,
                             BaseSnapshot *bases, int maxBases,
                             PillSnapshot *pills, int maxPills,
                             GameEvent *events, int maxEvents);
bool clientSimNetSyncSnapshot(ClientSim *cs);

/* Finalize the local tank after the server has placed it. Called by
 * GUI frontends once clientLoadCompressedMap + the first snapshot
 * have populated the sim. */
void clientSimNetSetupTankGo(ClientSim *cs);

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
/* Add-bot with explicit team, brain, and pool-picked name. The
 * zero-arg clientSimNetSendAddBot above sends teamNumber=0 / brainIdx=0xFF /
 * botName="" and lets the server pick defaults.
 *
 * The server currently ignores brainIdx on PACKET_LOBBY_ADD_BOT — bots
 * always start on the server's CLI-configured default. To apply a
 * non-default brain, callers should follow this with
 * clientSimNetSendLobbySetBotBrain once the new slot lands. The
 * parameter is kept on the API for symmetry with the rest of the
 * lobby-bot send wrappers. */
void clientSimNetSendAddBotConfigured(ClientSim *cs, BYTE teamNumber,
                                      uint8_t brainIdx,
                                      const char *botName);
void clientSimNetSendRemoveBot(ClientSim *cs, BYTE playerNum);
void clientSimNetSendLobbyBotConfig(ClientSim *cs, BYTE slot,
                                    uint8_t difficulty,
                                    uint8_t personality,
                                    const char *name);
/* Assign brain catalogue entry brainIdx to lobby bot slot. 0xFF =
 * use the server's CLI-configured default brain. */
void clientSimNetSendLobbySetBotBrain(ClientSim *cs, BYTE slot,
                                      uint8_t brainIdx);
/* Host (or openHost / admin) only — swap the running lobby map.
 * mapRelPath is relative to data/maps/ (e.g. "Foo.map" or
 * "subdir/Foo.map"). Server rejects "..", absolute paths, and
 * Windows drive letters before opening the file. */
void clientSimNetSendLobbySetMap(ClientSim *cs, const char *mapRelPath);

/* Lobby preview cycle. SET_MAP and a completed upload auto-stash
 * the previous committed map; these two close the loop:
 *   - Cancel: roll back to the stashed map (server re-broadcasts).
 *   - Commit: free the stash; the sim already shows the previewed
 *             map, so no further broadcast is needed. */
void clientSimNetSendLobbyPreviewCancel(ClientSim *cs);
void clientSimNetSendLobbyPreviewCommit(ClientSim *cs);

/* Procedural-map preview. seedStr is a mapGenConfigToSeed-encoded
 * string the server decodes back into a MapGenConfig. Server applies
 * as a preview (stashes previous map, regenerates, broadcasts
 * MAP_CHANGE). */
void clientSimNetSendLobbyPreviewRandom(ClientSim *cs, const char *seedStr);

/* Ask the server to list data/maps/<relPath>. Response arrives async
 * via PACKET_LOBBY_MAP_LIST_RSP and is stored on the ClientSim
 * (lobbyMapList* fields). Any lobby client may request — read-only. */
void clientSimNetSendLobbyMapListRequest(ClientSim *cs,
                                         const char *relPath);

/* Recursive search variant. Response stored on lobbyMapSearch*. */
void clientSimNetSendLobbyMapSearchRequest(ClientSim *cs,
                                           const char *relPath,
                                           const char *query);

/* Map upload: BEGIN announces a file with its byte length and
 * server-relative name (e.g. "Uploaded/Foo.map"); CHUNK delivers
 * data segments at the given offset (1024-byte cap). Server
 * acknowledges BEGIN with MAP_UPLOAD_ACK and the final chunk with
 * MAP_UPLOAD_DONE — both update lobbyMapUploadStatus on the
 * ClientSim. */
void clientSimNetSendLobbyMapUploadBegin(ClientSim *cs,
                                         uint32_t totalLen,
                                         const char *name);
void clientSimNetSendLobbyMapUploadChunk(ClientSim *cs,
                                         uint32_t offset,
                                         const uint8_t *data,
                                         uint16_t dataLen);
/* Pre-upload optimisation: if the server already has the same file
 * (matching MD5) at relPath under its data/maps/, it installs that
 * file directly and replies MAP_UPLOAD_DONE — no chunk transfer
 * needed. On a miss it replies MAP_USE_LOCAL_NACK and the caller
 * falls back to clientSimNetSendLobbyMapUploadBegin. relPath is
 * the same scheme PACKET_LOBBY_MAP_PREVIEW_REQ uses (relative to
 * data/maps/, no leading "data/maps/" segment). */
void clientSimNetSendLobbyMapUseLocal(ClientSim *cs,
                                      uint32_t totalLen,
                                      const char *name,
                                      const char *relPath,
                                      const uint8_t md5[16]);
void clientSimNetSendLobbyTeamMeta(ClientSim *cs, BYTE teamId,
                                   uint8_t color, uint8_t namingPool,
                                   const char *name);
void clientSimNetSendLobbyTeamClear(ClientSim *cs, BYTE teamId);
void clientSimNetSendLobbySetting(ClientSim *cs, uint8_t settingType,
                                  const uint8_t *value, uint8_t valueLen);
void clientSimNetSendLobbyOpenHost(ClientSim *cs, bool openHost);
void clientSimNetSendLobbyKick(ClientSim *cs, uint8_t slot);

/* Host- or admin-only: set or clear the server password. NULL or
 * empty pw clears. Server replies by broadcasting a fresh lobby
 * state so has_password updates on every client. The password text
 * itself is never echoed to other clients. */
void clientSimNetSendLobbySetPassword(ClientSim *cs, const char *pw);
void clientSimNetSendMapSkipVote(ClientSim *cs);

/* In-game vote toggle. kind = GAME_VOTE_KIND_BACK_TO_LOBBY or
 * GAME_VOTE_KIND_SURRENDER; toggleMode = GAME_VOTE_TOGGLE_NO/YES/OPEN_ONLY. */
void clientSimNetSendGameVoteToggle(ClientSim *cs,
                                    uint8_t kind, uint8_t toggleMode);
/* `includeBots`: when true the server hands every bot slot to WBN
 * with a non-WBN-player sentinel so bots end up assigned to one of
 * the two balanced teams. When false bots are removed from the
 * lobby as the proposal is applied (humans-only matchup). */
void clientSimNetSendBalanceRequest(ClientSim *cs, BYTE teamSize,
                                    bool includeBots);
/* Debug: append a printf-style line to ./balance.log in cwd. Lazily
 * opens the file on first call, flushes after every write. Safe to
 * call from any thread (single FILE* + best-effort, no mutex). */
void balanceDebugLog(const char *fmt, ...);

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
