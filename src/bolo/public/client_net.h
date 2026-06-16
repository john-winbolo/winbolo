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

#include <stddef.h>

#include "client_sim.h"
#include "client_connect_state.h"
#include "input_packet.h"        /* SnapshotHeader, TankSnapshot, etc. */
#include "gametype.h"

struct ServerSim;

/* === Lifecycle === */
/* fallbackCountry is the ISO-3166 two-letter code the server falls
 * back to when GeoIP doesn't resolve the joiner's IP (loopback joins,
 * private LAN, missing MMDB). Pass NULL or "" to leave the slot empty
 * — the server treats both the same. */
bool clientSimConnectUdp(ClientSim *cs, const char *serverAddr,
                         unsigned short serverPort, const char *playerName,
                         const char *fallbackCountry,
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

/* Drain the transport's socket and apply any pending snapshots without
 * advancing per-tick state (no localTick advance, resends, acks, ping, or
 * timeouts).  No-op for the local transport.  Caller holds the client mutex. */
void clientSimNetDrainSnapshots(ClientSim *cs);

/* Per-render-frame seam (3.3 + 3.2): drains the freshest snapshots, advances
 * the adaptive display-delay controller from measured jitter, and recomputes
 * remote tanks' interpolated display positions against the render clock.
 * Call once per frame from the front-end render loop, inside the client-mutex
 * bracket that already guards clientRenderFrame, passing the render clock
 * (e.g. SDL_GetTicks()). */
void clientSimRenderPrepare(ClientSim *cs, uint32_t nowMs);

/* === State queries === */
ClientConnectState clientSimGetConnectState(const ClientSim *cs);
const char *clientSimGetConnectErrorReason(const ClientSim *cs);
BYTE        clientSimGetServerPlayerNum(const ClientSim *cs);
const BYTE *clientSimGetServerMapData(const ClientSim *cs, int *outLen);
/* serverTick of the frame interp is currently displaying (second-newest applied
 * snapshot). Stamped onto outgoing inputs as viewTick so the server can rewind
 * hit-detection by the true view age. Returns 0 until two snapshots have been
 * applied, which routes the server to its ping-based fallback. */
uint32_t    clientSimGetViewTick(const ClientSim *cs);

/* === Send wrappers === */
void clientSimNetSendChat(ClientSim *cs, BYTE destPlayer, const char *message);
void clientSimNetSendNameChange(ClientSim *cs, const char *newName);
void clientSimNetSendAllianceRequest(ClientSim *cs, BYTE toPlayer);
void clientSimNetSendAllianceAccept(ClientSim *cs, BYTE toPlayer);
void clientSimNetSendAllianceLeave(ClientSim *cs);
void clientSimNetSendLockToggle(ClientSim *cs, bool allow);
/* Move `slot` to team `teamNumber`. Over UDP the slot byte is packed
 * into the wire but the server uses the sender's clientIdx for the
 * apply (so a non-host client can only change its own team — the slot
 * arg is informational). On the SP-host local transport, the apply
 * uses the supplied slot directly, which is how the lobby UI moves
 * bots / drags one human into another team's column. Self-team
 * callers pass clientSimGetMyPlayerNum(cs). */
void clientSimNetSendTeamSet(ClientSim *cs, BYTE slot, BYTE teamNumber);
/* Reserve startIdx (1-based map start, or 0xFF to release) for targetSlot.
 * targetSlot == own slot is a self-claim (server requires a free start);
 * targetSlot != own slot is host-only and swaps an occupied target.
 * Server-authoritative — the marker moves when the CTRL_LOBBY_SLOT
 * broadcast arrives, never on send. */
void clientSimNetSendLobbyClaimStart(ClientSim *cs, BYTE targetSlot,
                                     BYTE startIdx);
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

/* Ask the server for the raw .map bytes of data/maps/<relPath> so the
 * chooser can rasterise a preview without committing a SET_MAP. The
 * reply streams back async via PACKET_LOBBY_MAP_PREVIEW_BEGIN/_CHUNK
 * (or _ERR) into the ClientSim's lobbyMapPreview* fields; poll the
 * Ready/Error getters. No-op without a UDP transport. */
void clientSimNetSendLobbyMapPreviewRequest(ClientSim *cs,
                                            const char *relPath);

/* Map upload — file flavour. Read `localFilePath`, validate it via
 * boloMapValidate, and drive the chunked PACKET_LOBBY_MAP_UPLOAD_*
 * state machine over the wire. Returns false on file-not-found /
 * validate-failed / no-transport / upload-already-in-flight; on true
 * the transport owns the rest. The frontend polls
 * clientSimGetLobbyMapUpload* status getters for progress, completion,
 * and reject. When `localFilePath` lives under data/maps/, the
 * transport tries PACKET_LOBBY_MAP_USE_LOCAL first (server installs
 * directly if it already has a matching MD5) and falls back to a
 * regular byte upload on NACK. */
bool clientSimNetSendLobbyMapUpload(ClientSim *cs, const char *localFilePath);

/* Map upload — bytes flavour. Same as above but the bytes are already
 * in memory (e.g. a WinBolo.net blob with no on-disk identity). The
 * transport copies into its own state immediately; caller retains
 * ownership of `buf`. `mapName` is the wire-side filename the server
 * records. USE_LOCAL is skipped (no on-disk identity to claim) — the
 * upload always goes via BEGIN+CHUNK. */
bool clientSimNetSendLobbyMapUploadBytes(ClientSim *cs,
                                         const uint8_t *buf, size_t len,
                                         const char *mapName);

/* Upload progress as 0..100 driven by bytesSent / fileLen. Returns 0
 * when no upload is in flight. */
uint8_t clientSimGetLobbyMapUploadProgressPercent(const ClientSim *cs);

/* Pre-upload optimisation: if the server already has the same file
 * (matching MD5) at relPath under its data/maps/, it installs that
 * file directly and replies MAP_UPLOAD_DONE — no chunk transfer
 * needed. On a miss it replies MAP_USE_LOCAL_NACK and the caller
 * falls back to clientSimNetSendLobbyMapUpload. relPath is the same
 * scheme PACKET_LOBBY_MAP_PREVIEW_REQ uses (relative to data/maps/,
 * no leading "data/maps/" segment). Vestigial: the new
 * clientSimNetSendLobbyMapUpload runs USE_LOCAL internally, so no
 * frontend caller remains; kept for symmetry pending a later sweep. */
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
void clientSimNetSendLobbyTransferHost(ClientSim *cs, uint8_t slot);

/* Host- or admin-only: set or clear the server password. NULL or
 * empty pw clears. The server stores it locally; remote clients
 * learn about the new flag via the next INFO_RESPONSE query (LAN
 * browser refresh). The password text itself is never echoed to
 * other clients. */
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

void clientSimNetSendBalanceApply(ClientSim *cs);
void clientSimNetSendBalanceDismiss(ClientSim *cs);
void clientSimNetSendWbnReauth(ClientSim *cs);

/* === Net stats === */
uint16_t clientSimGetNetPing(const ClientSim *cs);
void     clientSimGetUdpNetStats(ClientSim *cs, int *ppsRecv, int *ppsSent,
                                 int *bpsRecv, int *bpsSent, int *numErrors,
                                 int *snapshotsRecv, int *snapshotsLost,
                                 int *snapshotsLostTotal);
/* Cumulative successful map resyncs (desync recovery) this session. */
int      clientSimGetMapResyncCount(ClientSim *cs);
/* Feed the per-full-sync map-checksum compare result to the transport's
 * resync state machine (request on mismatch / clear backoff on match). */
void     clientSimNetReportMapChecksum(ClientSim *cs, bool matched);
/* renderOffsetPx (out, may be NULL): current render-only error-offset
 * magnitude in pixels — the live correction being smoothed out, distinct
 * from the per-window reconcile error counts. */
void clientSimGetReconcileStats(ClientSim *cs, int *countPerSec,
                                float *avgErrPx, float *maxErrPx,
                                float *renderOffsetPx);

/* Measurement-only client timing estimates: clock offset (10ms server-tick
 * units), snapshot inter-arrival jitter (ms), RTT floor (ms), and pipeline
 * depth (10ms input-tick units — sent-but-unprocessed inputs in flight).  All
 * min-over-window smoothed.  Out pointers may be NULL; all zero on a non-UDP or
 * absent transport.  Surfaced in Net Info; nothing consumes them to alter
 * timing yet. */
void clientSimGetTimingStats(ClientSim *cs, int *clockOffsetTicks,
                             int *jitterMs, int *rttMs,
                             int *pipelineDepthTicks);

/* === Local-transport tuning === */
void     clientSimNetSetLocalDelay(ClientSim *cs, uint16_t delay_ms);
uint16_t clientSimNetGetLocalDelay(const ClientSim *cs);

#endif /* CLIENT_NET_H */
