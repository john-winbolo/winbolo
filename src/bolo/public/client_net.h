/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

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
#include "lang_message.h"        /* langid */
#include "input_packet.h"        /* SnapshotHeader, TankSnapshot, etc. */
#include "gametype.h"

struct ServerSim;

/* === Lifecycle === */
/* fallbackCountry is the ISO-3166 two-letter code the server falls
 * back to when GeoIP doesn't resolve the joiner's IP (loopback joins,
 * private LAN, missing MMDB). Pass NULL or "" to leave the slot empty
 * — the server treats both the same. */
/* spectator: true connects as a tankless spectator (JOIN_FLAG_SPECTATOR; the
 * accept lands in CLIENT_CONNECT_SPECTATING with no tank slot or map download)
 * instead of a normal player join. */
bool clientSimConnectUdp(ClientSim *cs, const char *serverAddr,
                         unsigned short serverPort, const char *playerName,
                         const char *fallbackCountry,
                         const char *password,
                         const char *wbnApiToken,
                         const char *wbnServerKey,
                         bool wantRejoin, const char *trackerAddr,
                         unsigned short trackerPort,
                         bool spectator);
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
/* The langid behind clientSimGetConnectErrorReason, or 0 when the reason
 * was not rendered from one (transport failure, plain-text fallback). A
 * frontend compares it with STR_REJECT_INCORRECT_PASSWORD to decide whether
 * to ask for the password again. */
langid      clientSimGetConnectErrorLangId(const ClientSim *cs);
BYTE        clientSimGetServerPlayerNum(const ClientSim *cs);
const BYTE *clientSimGetServerMapData(const ClientSim *cs, int *outLen);
/* serverTick of the frame interp is currently displaying (second-newest applied
 * snapshot). Stamped onto outgoing inputs as viewTick so the server can rewind
 * hit-detection by the true view age. Returns 0 until two snapshots have been
 * applied, which routes the server to its ping-based fallback. */
uint32_t    clientSimGetViewTick(const ClientSim *cs);

/* === Send wrappers === */
void clientSimNetSendChat(ClientSim *cs, BYTE destPlayer, const char *message);
/* Mute or unmute one player for this client: the server stops forwarding
 * that player's voice and chat. Session-scoped. */
void clientSimNetSendPlayerMute(ClientSim *cs, BYTE targetPlayer, bool muted);
/* Mute or unmute one player's smart pings for this client, independent of the
 * voice/chat mute above: the server stops delivering that player's EVENT_PING
 * to this client. Session-scoped. */
void clientSimNetSendPlayerPingMute(ClientSim *cs, BYTE targetPlayer, bool muted);
/* Report this client's own mic status. hasMic is voice enabled with an input
 * device open; selfMuted is having one but not transmitting. The server keeps
 * it in the sender's clientFlags and shows it to the players who could hear
 * that voice. Sent on a change, not per tick. */
void clientSimNetSendVoiceState(ClientSim *cs, bool hasMic, bool selfMuted);
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
/* mode indexes the brain's own mode list (brain_list.h) and difficulty
 * indexes THAT mode's level list — mode 0 with 0/1/2 is the pre-manifest
 * easy / medium / hard. */
void clientSimNetSendLobbyBotConfig(ClientSim *cs, BYTE slot,
                                    uint8_t mode,
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

/* Host (or openHost / admin) only — pick one of the scenarios the server
 * offers on its own. relPath is the file name the scenario list gave, and
 * "" selects none; NULL is a no-op. The server rejects "..", absolute
 * paths, Windows drive letters and a name its scenarios directory does not
 * hold. Picking is a commit and not a preview: the scenario it names takes
 * effect at once, over the committed map's own script if that map has one,
 * and the lobby settings event says which is playing. */
void clientSimNetSendLobbySetScenario(ClientSim *cs, const char *relPath);

/* Host (or openHost / admin) only - set the lobby's whole script list: one
 * scenario deciding the round and mods behind it, in load order. files is
 * `count` file names as the scenario list gave them; count 0 clears the list
 * and is a message rather than a mistake.
 *
 * The whole list and not one entry, so two hosts editing at the same moment
 * cannot interleave into a list neither asked for - the later command simply
 * wins. The server refuses the list outright, changing nothing, if any name
 * is one its scenarios directory does not hold, is a path rather than a
 * name, is bound to a map, or repeats an earlier one, or if more than one
 * entry is a scenario rather than a mod. Refused as a whole and not entry by
 * entry: a list half applied is one the host never asked for.
 *
 * A no-op here, sending nothing, when count is outside 0..CMD_SCRIPT_LIST_MAX
 * or a name is empty or too long for the field to carry.
 *
 * The result comes back as the lobby settings event and the script list
 * event together, which is what the clientSimGetLobbyScript* accessors
 * answer from. */
void clientSimNetSendSetScriptList(ClientSim *cs,
                                   const char *const *files, int count);

/* Lobby preview cycle. SET_MAP and a completed upload auto-stash
 * the previous committed map; these two close the loop:
 *   - Cancel: roll back to the stashed map (server re-broadcasts).
 *   - Commit: free the stash; the sim already shows the previewed
 *             map, so no further broadcast is needed. */
void clientSimNetSendLobbyPreviewCancel(ClientSim *cs);
/* Ask the server to read the map's script again. Host-only and lobby-only
 * at the server; what it changes takes effect at the next round. The server
 * answers with a line addressed to the sender, whether it worked or not. */
void clientSimNetSendLobbyReloadScenario(ClientSim *cs);
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

/* Ask what scenarios the server offers on their own, independently of any
 * map. No path: the scenarios directory is flat, unlike the map chooser's
 * tree. The response arrives async via PACKET_LOBBY_SCENARIO_LIST_RSP and is
 * stored on the ClientSim; read it back through the
 * clientSimGetLobbyScenario* accessors in client_sim.h. Any lobby client may
 * ask — read-only. No-op without a UDP transport. */
void clientSimNetSendLobbyScenarioListRequest(ClientSim *cs);

/* Recursive search variant. Response stored on lobbyMapSearch*. */
void clientSimNetSendLobbyMapSearchRequest(ClientSim *cs,
                                           const char *relPath,
                                           const char *query);

/* Ask the server for the raw .map bytes of data/maps/<relPath> so the
 * chooser can rasterise a preview without committing a SET_MAP. The
 * reply streams back async over CHANNEL_BULK behind a bulk-transfer
 * stream header (or _ERR) into the ClientSim's lobbyMapPreview* fields; poll the
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

/* Script upload: send a .scenario or .lua of up to
 * LOBBY_PACKAGE_UPLOAD_MAX_BYTES to the server, which hands it to its
 * script accept callback. Returns false, sending nothing, on a missing,
 * empty or over-cap file, a name without either suffix, a spectator, an
 * upload already in flight, or a transport that is not UDP (an
 * in-process host never sends). Progress and the outcome come back
 * through the same clientSimGetLobbyMapUpload* accessors as a map, and
 * clientSimGetLobbyUploadKind says it is a script. */
bool clientSimNetSendLobbyScriptUpload(ClientSim *cs, const char *localFilePath);

/* Upload progress as 0..100 driven by bytesSent / fileLen. Returns 0
 * when no upload is in flight. Serves a map or a script upload. */
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
                                      const char md5Hex[32]);
void clientSimNetSendLobbyTeamMeta(ClientSim *cs, BYTE teamId,
                                   uint8_t color, uint8_t namingPool,
                                   uint8_t startSide,
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

/* Tell the server this client has just had a rating or comment accepted on
 * the WinBolo.net page for round `key32` (the summary's wbnLogKey), so the
 * other clients in the lobby can re-read that page instead of showing a
 * stale list. Sends nothing without a transport, on an empty key, or from a
 * spectator. */
void clientSimNetSendRatingPosted(ClientSim *cs, const char *key32);

/* Tell the server which view this client is in: kind is a ViewStateKind
 * (VIEW_KIND_TANK/PILL/BASE/ALLY) and target is the pill/base index or the
 * ally's player number, ignored for the tank view. The server stores the
 * claim and grants at most the rect its view policies allow, so a stale or
 * bogus claim costs nothing. Sends nothing without a transport or from a
 * spectator. */
void clientSimNetSendViewState(ClientSim *cs, uint8_t kind, uint8_t target);

/* Ask the server which item this client should watch next. kind is a
 * ViewStateKind, direction a ViewCycleDirection, and from the item currently
 * watched (VIEW_CYCLE_FROM_NONE when there is none). The server picks from
 * live state and answers with CTRL_VIEW_TARGET, copying from back so a late
 * answer to an earlier press can be told apart from the answer to this one.
 * Sends nothing without a transport or from a spectator. */
void clientSimNetSendViewCycle(ClientSim *cs, uint8_t kind, uint8_t direction,
                               uint8_t from);

/* Ask the server to place a smart ping at (worldX, worldY) in WORLD units
 * (256 per map tile) for the sender's team. kind is a PING_KIND_*
 * (input_packet.h). The server re-checks state, range and the per-player rate
 * limit, so a ping the sender is not entitled to costs nothing but the
 * packet. Sends nothing without a transport, from a spectator, or for a kind
 * this build does not know. */
void clientSimNetSendPing(ClientSim *cs, uint8_t kind,
                          uint16_t worldX, uint16_t worldY);

/* === Last completed round's replay log === */

/* State of the round-log transfer. A joined client cannot record a round
 * itself, so to replay the round its recap describes it asks the server for
 * that round's .wbv bytes and takes delivery over the bulk channel.
 *
 * The three unavailable states stay distinct so a caller can word each
 * refusal: the server does not serve logs, it has no completed round, or the
 * round's log is over the transfer cap. A refusal the client can simply retry
 * — the server is at its concurrent-transfer cap, or this client asked too
 * soon — is not a state: the transport backs off and stays WAITING. */
typedef enum {
  CLIENT_ROUND_LOG_IDLE = 0,     /* nothing asked for, or the blob was taken */
  CLIENT_ROUND_LOG_WAITING,      /* request sent, no bytes yet               */
  CLIENT_ROUND_LOG_DOWNLOADING,  /* bytes arriving; see the percent getter   */
  CLIENT_ROUND_LOG_READY,        /* whole blob held, waiting to be taken     */
  CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED,
  CLIENT_ROUND_LOG_UNAVAILABLE_NONE,
  CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE
} ClientRoundLogState;

/* Current state, as a ClientRoundLogState. CLIENT_ROUND_LOG_IDLE without a
 * UDP transport. */
int clientSimGetRoundLogState(const ClientSim *cs);

/* Transfer progress as 0..100. Only meaningful while the state is
 * CLIENT_ROUND_LOG_DOWNLOADING; 0 otherwise. */
uint8_t clientSimGetRoundLogPercent(const ClientSim *cs);

/* Ask the server for the last completed round's log. Supersedes anything the
 * transport already holds for an earlier request — a completed blob or a
 * part-received one is freed — and moves the state to CLIENT_ROUND_LOG_WAITING.
 * Returns false and changes nothing without a connected UDP transport, so a
 * caller that means to ask only once must keep asking until it returns true. */
bool clientSimNetSendRoundLogRequest(ClientSim *cs);

/* Take ownership of a completed transfer: returns the blob, writes its length
 * through outLen (may be NULL), clears the transport's pointer and returns the
 * state to CLIENT_ROUND_LOG_IDLE. Returns NULL unless the state is
 * CLIENT_ROUND_LOG_READY.
 *
 * The caller owns the returned buffer from this point on — nothing in the
 * transport reads or frees it again. Release it with plain free(), or hand it
 * to lvEmbedBegin, which takes ownership and frees it itself, including on
 * every refusal. */
uint8_t *clientSimTakeRoundLog(ClientSim *cs, size_t *outLen);

/* === Voice ===
 * Encoded audio frames move as opaque bytes: the caller supplies and
 * receives whatever the codec produced, and the wire framing stays inside
 * the transport. Voice does not go through the command queue or the
 * control-event bus - it is a best-effort payload at the tick rate, and
 * nothing about it reaches the sim or the event stream. */

/* Largest encoded frame one voice segment can carry. A frame past this is
 * dropped by the transport rather than split or truncated, so a caller that
 * wants to notice checks before it sends. Mirrors VOICE_SEG_MAX_OPUS, the
 * wire-side bound in src/bolo/internal/voice_segment.h, which the client
 * frontends do not see; transport_udp_client.c sees both and asserts them
 * equal at compile time, so the two cannot drift apart. */
#define CLIENT_VOICE_MAX_FRAME_BYTES 125

/* Queue one encoded 20 ms voice frame for the server. flags is the frame's
 * flags byte, carried to the wire untouched; 0 for an ordinary frame. No-op
 * for in-process (local) transports, which never carry voice. */
void clientSimNetSendVoice(ClientSim *cs, const uint8_t *opus, int opusLen,
                           uint8_t flags);

/* Pop one received voice frame. Returns the payload length written to
 * out, or 0 when nothing is pending. */
int clientSimNetReceiveVoice(ClientSim *cs, uint8_t *fromPlayer, uint8_t *seq,
                             uint8_t *flags, uint8_t *out, int outCap);

/* True when voice sent on this client actually reaches the wire: a UDP
 * transport is attached. False with no transport and for the in-process
 * (local) transport single-player uses, which never carries voice. */
bool clientSimNetHasVoiceTransport(const ClientSim *cs);

/* What became of the voice frames this client queued: framed onto the wire,
 * dropped by a full ring, or left behind by a frame with no budget for them.
 * All three are losses the protocol itself cannot show. Any out pointer may
 * be NULL; a client with no UDP transport reports zeroes. */
void clientSimNetGetVoiceChannelStats(ClientSim *cs, uint32_t *outSent,
                                      uint32_t *outRingDropped,
                                      uint32_t *outBudgetSkipped);

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
