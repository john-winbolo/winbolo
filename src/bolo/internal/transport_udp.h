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

/*********************************************************
 *Name:          Transport UDP
 *Filename:      transport_udp.h
 *Author:        John Morrison
 *Purpose:
 *  UDP network transport for multiplayer games.
 *  Client side: sends InputPackets to server, receives
 *  state snapshots.
 *  Server side: receives inputs from clients, broadcasts
 *  state snapshots.
 *********************************************************/

#ifndef TRANSPORT_UDP_H
#define TRANSPORT_UDP_H

#include "global.h"
#include "platform_net.h"
#include "input_packet.h"
#include "transport.h"
#include "netpacks.h"
#include "gametype.h"
#include "client_connect_state.h"
#include "server_sim.h"      /* SubscriberHandle */

/* Forward declarations */
struct ClientSim;
struct ServerSim;

/*********************************************************
 * Snapshot wire format types are defined in input_packet.h:
 *   SnapshotHeader, TankSnapshot, GameEvent
 *********************************************************/

/* Header prepended to every new-protocol UDP packet */
typedef struct {
    uint8_t  magic[2];      /* BOLO_NEW_MAGIC_0, BOLO_NEW_MAGIC_1 */
    uint8_t  packetType;    /* PACKET_* constant */
    uint8_t  reserved;
    uint32_t sequence;      /* Monotonic sequence number */
} PacketHeader;

/* WBN join key: 64-char hex string + null */
#define WBN_JOIN_KEY_WIRE_LEN 65

/* JOIN_REQUEST flags byte (after the WBN join key on the wire).  Bit 0 is
 * a client rejoin request; bit 1 is a client assertion that it is signed
 * in and will authenticate via PACKET_WBN_REAUTH after JOIN_ACCEPT.  The
 * will-authenticate bit is untrusted — it only lets the collision policy
 * admit provisionally instead of rejecting; it never grants priority. */
#define JOIN_FLAG_WANT_REJOIN       0x01
#define JOIN_FLAG_WILL_AUTHENTICATE 0x02
/* bit 2: client requests a tankless spectator connection rather than a tank
 * slot.  Branches the join handler to the spectator-accept path. */
#define JOIN_FLAG_SPECTATOR         0x04

/* JOIN_ACCEPT slot byte the server sends to a tankless spectator in place of a
 * real 0..MAX_TANKS-1 slot — the viewer holds no tank.  A spectator client keys
 * its accept handling off this sentinel; a player-join client sees it as an
 * out-of-range slot (>= MAX_TANKS) and rejects the accept. */
#define SPECTATOR_ACCEPT_NO_SLOT    0xFFu

/* Cap on concurrent tankless spectator connections held transport-side in
 * spectators[], independent of the MAX_TANKS player slots.  The operator's
 * -maxspectators cap is enforced as min(maxSpectators, MAX_SPECTATORS). */
#define MAX_SPECTATORS 32

/* Spectator status wire shape — a small reliable message carried on the
 * spectator's own CHANNEL_CONTROL.  While the delayed ring holds less than
 * specDelayTicks of history (cold start at ring creation / segment youth), the
 * server re-sends a countdown instead of seeding live state: a status-type byte
 * followed by the big-endian ticks remaining until head - specDelayTicks
 * becomes seekable.  The client mirrors this shape to decode the wait.
 *   [u8 SPEC_CTRL_COUNTDOWN][u32 remainingTicks]   (5 bytes, big-endian) */
#define SPEC_CTRL_COUNTDOWN      1   /* status-type byte */
#define SPEC_CTRL_COUNTDOWN_LEN  5   /* type byte + u32 remainingTicks */

/* Per-record transport header the server prepends to each forward-feed blob
 * (BULK_KIND_SPEC_RECORD) ahead of the raw ring payload, big-endian:
 *   [u8 isKeyframe][u32 gameTick][u32 segment]
 * The spectator client strips this to recover the payload. (The seed blob,
 * BULK_KIND_SPEC_SEED, carries no such header — it is the raw keyframe.) */
#define SPEC_RECORD_HEADER_LEN   9

/* ── Deferred WBN PLAYER_JOIN bookkeeping (pure core) ────────────────
 * A slot owes WBN a PLAYER_JOIN event once we learn its identity for
 * the current session: keyed when a reauth fills the slot's WBN key,
 * or anonymous when a grace window elapses with no reauth.  Because
 * winbolonetEndSession empties every per-slot key at a round boundary,
 * the next reauth re-fires the join for the new session — which is how
 * a "return to lobby = new game" gets a fresh join burst.
 *
 * Exposed as a value-only core so the join/reauth/grace/disconnect
 * sequencing is unit-testable without sockets or the WBN HTTP layer;
 * transport_udp_server.c holds one WbnJoinState per UdpServerClient and
 * performs the actual winbolonetAddEvent calls off these return values. */
typedef struct {
    bool     pending;       /* a join event is owed for this slot/session */
    uint32_t deadlineTick;  /* emit anonymous once the tick counter reaches this */
} WbnJoinState;

/* Arm a deferred join with an anonymous-fallback deadline graceTicks
 * ahead of nowTick. */
void wbnJoinArm(WbnJoinState *s, uint32_t nowTick, uint32_t graceTicks);

/* A reauth verify just succeeded.  wasParticipant is whether the slot
 * already held a WBN key for this session *before* the verify.  Clears
 * any pending anonymous fallback and returns TRUE iff the caller should
 * emit a keyed PLAYER_JOIN — i.e. the key went absent->present, which
 * is a fresh join or a post-rotation re-registration.  A repeat verify
 * on an already-keyed slot (wasParticipant) returns FALSE so an
 * idempotent rekey resend never double-counts. */
bool wbnJoinOnReauth(WbnJoinState *s, bool wasParticipant);

/* Per-tick poll.  Returns TRUE exactly once — when the grace window has
 * elapsed with the join still pending — so the caller emits an
 * anonymous PLAYER_JOIN.  nowTick uses the same monotonic counter
 * passed to wbnJoinArm. */
bool wbnJoinOnTick(WbnJoinState *s, uint32_t nowTick);

/* Slot left (or is being recycled) before the join resolved: drop the
 * owed event so no orphan anonymous join is emitted for a player who
 * never got a keyed one. */
void wbnJoinClear(WbnJoinState *s);

/* Should this slot receive a PACKET_WBN_REKEY on session rotation?
 * Gated on the slot being connected and having been a WBN player last
 * round (the durable per-connection wbnWasVerified bit).  It deliberately
 * does NOT read the sim-side PLAYER_FLAG_WBN_VERIFIED: serverSimReturnToLobby
 * clears that flag on every slot immediately before the rekey broadcast
 * runs, so a flag-based gate would match nobody and strand every player
 * un-keyed for the new round. */
bool wbnRekeyTargetSelected(bool connected, bool wbnWasVerified);

/* JOIN name-collision verdict.  Pure value core so the policy is
 * unit-testable without sockets or the WBN layer.  The will-auth flag is
 * client-asserted and only ever downgrades a reject to a provisional
 * admit — it never grants priority. */
typedef enum {
    JOIN_COLLISION_REJECT_IN_USE,      /* unverified squatter, joiner won't auth */
    JOIN_COLLISION_REJECT_VERIFIED,    /* squatter is verified; flag irrelevant */
    JOIN_COLLISION_ADMIT_PROVISIONAL,  /* unverified squatter, joiner will auth */
} JoinCollisionVerdict;

/* Decide the verdict for an incoming joiner whose validated name already
 * matches a connected slot.  incomingWillAuth is the client-asserted
 * "signed in, will authenticate" JOIN flag; existingIsVerified is whether
 * the matched slot already holds PLAYER_FLAG_WBN_VERIFIED. */
JoinCollisionVerdict joinCollisionDecide(bool incomingWillAuth,
                                         bool existingIsVerified);

/* Reauth-time resolution of a pending provisional name claim.  Pure value
 * core: given whether the desired bare name is currently held and, if so,
 * whether the holder is WBN-verified, decide what to do with the
 * reclaiming slot.  The squatter-suffix-pool-exhaustion fallback is a
 * runtime concern handled at the call site, not encoded here. */
typedef enum {
    CLAIM_RESOLVE_PROMOTE_FREE,      /* bare name free → promote the slot to it */
    CLAIM_RESOLVE_PREEMPT_SQUATTER,  /* unverified holder → rename it off, then promote */
    CLAIM_RESOLVE_KEEP_TEMP,         /* verified holder → slot keeps its temp name */
} ClaimResolveAction;

ClaimResolveAction claimResolveDecide(bool bareNameHeld, bool holderIsVerified);

/* Join request packet (client -> server) */
typedef struct {
    PacketHeader hdr;
    char playerName[PACKET_MAX_PLAYER_NAME];
    char password[MAP_STR_SIZE];
    uint8_t versionMajor;
    uint8_t versionMinor;
    uint8_t versionRevision;
    char wbnJoinKey[WBN_JOIN_KEY_WIRE_LEN];
} JoinRequestPacket;

/* Join accept packet (server -> client) */
typedef struct {
    PacketHeader hdr;
    uint8_t playerNum;
    uint32_t serverTick;
} JoinAcceptPacket;

/* Join reject packet (server -> client) */
typedef struct {
    PacketHeader hdr;
    char reason[64];
} JoinRejectPacket;

/* Player joined/left notification */
typedef struct {
    PacketHeader hdr;
    uint8_t playerNum;
    char playerName[PACKET_MAX_PLAYER_NAME];
} PlayerEventPacket;

/* Ping/pong packet */
typedef struct {
    PacketHeader hdr;
    uint32_t clientTime;   /* Timestamp from client for RTT measurement */
    uint32_t serverTime;   /* Server timestamp (only in pong) */
} PingPacket;

/*********************************************************
 * UDP Transport — Client Side
 *********************************************************/

/* State of the client join handshake — UdpClientJoinState and
 * UDP_CLIENT_* values are defined in client_connect_state.h as
 * aliases for ClientConnectState. */

/* Creates a client-side UDP transport that connects to a server.
 * Returns a Transport struct with sendInput and tick callbacks.
 * serverAddr/serverPort: the server to connect to.
 * playerName: name to use in join request.
 * password: game password (empty string if none).
 * trackerAddr: "" or NULL = no punch fallback (LAN/manual-connect).
 * trackerPort: ignored if trackerAddr empty.
 * spectator: true requests a tankless spectator connection (JOIN carries
 *   JOIN_FLAG_SPECTATOR; the accept lands in UDP_CLIENT_SPECTATING with no
 *   tank slot or map download) instead of a normal player join. */
Transport transportUdpClientCreate(struct ClientSim *clientSim,
                                   const char *serverAddr,
                                   unsigned short serverPort,
                                   const char *playerName,
                                   const char *fallbackCountry,
                                   const char *password,
                                   const char *wbnApiToken,
                                   const char *wbnServerKey,
                                   bool wantRejoin,
                                   const char *trackerAddr,
                                   unsigned short trackerPort,
                                   bool spectator);

/* Destroys a client-side UDP transport. */
void transportUdpClientDestroy(Transport *t);

/* Returns the client join state (for UI to display connecting/error). */
UdpClientJoinState transportUdpClientGetJoinState(Transport *t);

/* Returns the assigned player number after successful join. */
BYTE transportUdpClientGetPlayerNum(Transport *t);

/* Returns TRUE if a new snapshot is available.
 * If TRUE, copies the snapshot header, tank data, shell data,
 * base/pill state, and game events into the output params.
 * Arrays may be NULL if not needed. */
bool transportUdpClientGetSnapshot(Transport *t,
                                   SnapshotHeader *hdr,
                                   TankSnapshot *tanks,
                                   int maxTanks,
                                   ShellSnapshot *shellsOut,
                                   int maxShells,
                                   BaseSnapshot *basesOut,
                                   int maxBases,
                                   PillSnapshot *pillsOut,
                                   int maxPills,
                                   GameEvent *eventsOut,
                                   int maxEvents);

/* Returns the current ping in milliseconds. */
uint16_t transportUdpClientGetPing(Transport *t);

/* Returns client-side network stats: packets/sec received, packets/sec sent,
 * bytes/sec received, bytes/sec sent, cumulative error count, the
 * snapshots-received / snapshots-lost counts from the most recently
 * completed 1-second window (loss is inferred from serverTick gaps),
 * and the cumulative inferred-lost snapshot count since join.
 * Out pointers after numErrors may be NULL. */
void transportUdpClientGetNetStats(Transport *t, int *ppsRecv, int *ppsSent,
                                   int *bpsRecv, int *bpsSent, int *numErrors,
                                   int *snapshotsRecv, int *snapshotsLost,
                                   int *snapshotsLostTotal);

/* Client timing estimates (client_timing.{c,h}): clock offset in 10ms
 * server-tick units, snapshot inter-arrival jitter in milliseconds, RTT floor
 * in milliseconds, and pipeline depth in server-tick units.  Out pointers may
 * be NULL.  The jitter value drives the render-clock interpolation's adaptive
 * display delay (clientSimRenderPrepare -> interpRenderControl); clock offset,
 * RTT, and pipeline depth are read-only (Net Info readout). */
void transportUdpClientGetTimingStats(Transport *t, int *clockOffsetTicks,
                                      int *jitterMs, int *rttMs,
                                      int *pipelineDepthTicks);

/* Enqueue a ClientCommand on the reliable carrier. Assigns cmdSeq,
 * appends to the per-connection out-queue, and eager-sends a
 * PACKET_COMMAND_TICK if the queue was empty. Retransmits until the
 * server returns PACKET_COMMAND_ACK with the matching seq. */
struct ClientCommand;
void transportUdpClientSubmitCommand(Transport *t,
                                     const struct ClientCommand *cmd);

/* ── Layout A lobby commands — Client → Server ───────────────────── */
void transportUdpClientSendLobbyMapListRequest(Transport *t, const char *relPath);
void transportUdpClientSendLobbyMapSearchRequest(Transport *t,
                                                 const char *relPath,
                                                 const char *query);
/* Request the raw .map bytes of data/maps/<relPath> for an in-chooser
 * preview. Reply streams back over CHANNEL_BULK behind a bulk-transfer
 * stream header (or _ERR) into the ClientSim's lobbyMapPreview* accumulator. */
void transportUdpClientSendLobbyMapPreviewRequest(Transport *t,
                                                  const char *relPath);
void transportUdpClientSendLobbyMapUploadBegin(Transport *t, uint32_t totalLen,
                                               const char *name);
/* Pre-upload optimisation: try to skip the byte transfer if the server
 * already has an identical file at relPath (relative to data/maps/).
 * Server replies PACKET_LOBBY_MAP_UPLOAD_DONE on match, or
 * PACKET_LOBBY_MAP_USE_LOCAL_NACK on miss — caller falls back to
 * UploadBegin/Chunk on NACK. */
void transportUdpClientSendLobbyMapUseLocal(Transport *t, uint32_t totalLen,
                                             const char *name,
                                             const char *relPath,
                                             const char md5Hex[32]);

/* Lobby map upload entry points — the chunked PACKET_LOBBY_MAP_UPLOAD_*
 * state machine that used to live in imgui_lobby's per-frame pump. The
 * transport owns the read, validation, USE_LOCAL pre-check, chunk
 * dispatch, ACK/DONE state machine, and the watchdog. Both return
 * false on file-not-found / validate-failed / no-transport /
 * upload-already-in-flight; on true the transport pump (driven by
 * udpClientTick) carries the upload to completion. The frontend reads
 * progress via the lobbyMapUpload* status fields on ClientSim. */
bool transportUdpClientStartLobbyMapUploadFromPath(Transport *t,
                                                    const char *localFilePath);
bool transportUdpClientStartLobbyMapUploadFromBytes(Transport *t,
                                                     const uint8_t *buf,
                                                     size_t len,
                                                     const char *mapName);

/* Current upload progress as 0..100 (bytesSent / fileLen * 100). */
uint8_t transportUdpClientGetLobbyMapUploadProgressPercent(Transport *t);

/* Server-side: validates a length-prefixed upload filename against the
 * reserved-name / control-char / suffix-cap rules. Exposed for unit
 * coverage of the validation matrix; production callers live inside
 * transport_udp_server.c. */
bool uploadFilenameIsSafe(const char *name, size_t nameLen);

/* Client-side: parsers for the chunked MAP_LIST_RSP / MAP_SEARCH_RSP
 * responses. The dispatcher in transport_udp_client.c calls these per
 * packet; exposing them lets unit tests feed crafted byte streams
 * through the accumulator path without standing up a full transport
 * context. `buf` includes the 8-byte packet header. */
struct ClientSim;
void udpClientHandleLobbyMapListRsp(struct ClientSim *cs,
                                    const uint8_t *buf, int len);
void udpClientHandleLobbyMapSearchRsp(struct ClientSim *cs,
                                      const uint8_t *buf, int len);
/* Client-side handler for a failed MAP_PREVIEW request. The map bytes
 * themselves now arrive over CHANNEL_BULK and are reassembled by the
 * transport's bulk receiver; ERR flags the request failed. `buf` includes
 * the 8-byte packet header. */
void udpClientHandleLobbyMapPreviewErr(struct ClientSim *cs,
                                       const uint8_t *buf, int len);

/* Re-authenticate WBN token after lobby reset between rounds. */
void transportUdpClientSendWbnReauth(Transport *t);

/* Returns the server's reject reason string after a failed join.
 * Returns NULL if no reject reason is available. */
const char *transportUdpClientGetJoinRejectReason(Transport *t);

/* Cumulative count of successful map resyncs (desync recovery) this session,
 * for the Net Info overlay. */
uint32_t transportUdpClientGetMapResyncCount(Transport *t);

/* Report the result of a full-sync map-checksum compare (matched / mismatch).
 * Drives the map-resync state machine: starts a resync on a mismatch (subject
 * to suppression + backoff) or disconnects after the backoff cap. Called from
 * the snapshot apply path; a no-op unless the client is connected. */
void transportUdpClientReportMapChecksum(Transport *t, bool matched);

/* Returns the downloaded map data after successful join.
 * Returns NULL if no map has been downloaded yet.
 * outLen receives the length of the compressed data. */
const BYTE *transportUdpClientGetMapData(Transport *t, int *outLen);

/* Returns map-download progress as 0..100. Returns 100 when nothing is
 * in flight (no buffer allocated yet, or zero-sized total — neither
 * happens in practice). */
uint8_t transportUdpClientGetMapDownloadPercent(Transport *t);


/*********************************************************
 * UDP Transport — Server Side
 *********************************************************/

/* Ping enforcement thresholds */
#define PING_WARN_THRESHOLD_MS  400   /* RTT ms — warn after consecutive breaches */
#define PING_KICK_THRESHOLD_MS  500   /* RTT ms — kick after consecutive breaches */
#define PING_WARN_COUNT         3     /* consecutive pings at warn threshold before warning */
#define PING_KICK_COUNT         5     /* consecutive pings at kick threshold before kick */

/* Per-client connection info tracked by the server */
typedef struct UdpServerClient {
    struct sockaddr_in addr;
    bool connected;
    uint64_t connId;             /* Random per-session id the client echoes on
                                  * its INPUT packets. Lets the slot survive a
                                  * NAT rebind: a connId match from a new source
                                  * address re-homes addr. 0 until assigned at
                                  * join; 0 on the wire means "absent". */
    uint8_t playerNum;
    char playerName[PACKET_MAX_PLAYER_NAME];
    uint32_t lastReceivedTick;   /* For timeout detection */
    uint32_t outSequence;        /* Outgoing packet sequence */
    uint32_t inboundCmdSeq;      /* Highest contiguously processed cmdSeq from
                                  * PACKET_COMMAND_TICK on this client. 0 before
                                  * the client sends its first command. Updated
                                  * atomically with serverSimApplyCommand. */
    uint32_t lastPingTime;       /* When we last sent a ping */
    uint16_t pingMs;             /* Last measured ping */
    uint32_t lastPongSentMs;     /* SDL_GetTicks() when last PONG was sent */
    char countryCode[3];         /* ISO 3166-1 alpha-2 from GeoIP lookup */
    uint16_t inputsThisTick;     /* Inputs applied this tick cycle (for rate limiting) */
    bool wantRejoin;             /* Client requested rejoin (restore pills/bases) */
    uint8_t pingWarnStrikes;     /* consecutive pings >= warn threshold */
    uint8_t pingKickStrikes;     /* consecutive pings >= kick threshold */
    bool    pingWarned;          /* warning already sent this streak */
    uint16_t lastEnforcedPingMs; /* pingMs value last time enforcement ran */
    bool    nameStickySuffix;    /* Phase 5: server-renamed by verified-priority
                                  * collision; keep the suffixed name for the
                                  * rest of the session.  Cleared on disconnect. */
    /* Provisional-claim bookkeeping: set when a will-authenticate joiner
     * was admitted under a temporary -unverified[-N] name because its
     * desired bare name was held by an unverified squatter.  Reauth
     * (transportUdpServerHandleWbnReauth) verifies against claimDesiredName
     * and, on success, resolves the claim — promoting this slot to the bare
     * name and renaming the squatter.  Cleared on resolve and on
     * disconnect/slot-reset. */
    bool claimPending;
    char claimDesiredName[PACKET_MAX_PLAYER_NAME];
    /* Set once at JOIN_REQUEST and not refreshed mid-connection.  Server does
     * not push updates if e.g. a Steam Deck docks mid-game; this is
     * intentional, not a bug. */
    uint8_t clientType;          /* immutable after JOIN_REQUEST */
    uint8_t clientHints;         /* immutable after JOIN_REQUEST; SUPPORTER|STEAM_BUILD only */
    WbnJoinState wbnJoin;        /* deferred PLAYER_JOIN bookkeeping for this
                                  * slot/session — armed at join and at each
                                  * session rotation, resolved by reauth or
                                  * the per-tick grace sweep. */
    bool wbnWasVerified;         /* Durable per-connection "this client is a
                                  * WBN player" bit, set at join/reauth and
                                  * held across rounds.  The rekey-rotation
                                  * gate reads THIS, not the sim-side
                                  * PLAYER_FLAG_WBN_VERIFIED, because
                                  * serverSimReturnToLobby clears that flag
                                  * just before the rekey broadcast runs.
                                  * Cleared on disconnect/slot-reset. */
    /* WEB (CLIENT_TYPE_WEB) join-code identity, verified ONCE per connection
     * and re-stamped from here on later reauths.  The join_code expires at TTL
     * (~300s, shorter than a round) and the server_key rotates between rounds,
     * so re-verifying would fail; caching sidesteps both.  Zeroed at JOIN and
     * on disconnect/slot-clear. */
    bool wbnWebIdentityCached;                  /* first WEB verify has succeeded */
    bool wbnWebIsLoggedIn;                      /* cached is_logged_in */
    char wbnWebName[PACKET_MAX_PLAYER_NAME];    /* cached WBN player_name */
    char wbnWebCountry[3];                      /* cached ISO-2 + NUL */
    int  wbnWebUserId;                          /* cached user_id, -1 when null */
    SubscriberHandle controlSub; /* per-client subscription on the server's
                                  * control-event bus; the deliver callback
                                  * encodes via the codec table and unicasts
                                  * to this client.  SUBSCRIBER_HANDLE_INVALID
                                  * when no subscription is active. */
} UdpServerClient;

/* Creates a server-side UDP transport.
 * Binds to the given port and starts accepting connections.
 * sim: the authoritative ServerSim that inputs will be applied to.
 *      sim->maxPlayers is the join-slot cap (set by the lifecycle layer
 *      before calling here).
 * password: game password (empty string if none). */
bool transportUdpServerCreate(unsigned short port,
                              const char *addrToUse,
                              struct ServerSim *sim,
                              const char *password);

/* Destroys the server-side UDP transport. */
void transportUdpServerDestroy(void);

/* Server per-tick API. Call once per tick after serverSimTick().
 * Use the recv/drainRecvQueue pair (selected via
 * transportUdpServerHasRecvThread) to receive inputs, then
 * transportUdpServerDrainEvents and transportUdpServerSend. */
void transportUdpServerRecv(struct ServerSim *sim);
void transportUdpServerSend(struct ServerSim *sim);

/* Drain the recv thread's packet queue (use when recv thread is active). */
void transportUdpServerDrainRecvQueue(struct ServerSim *sim);

/* Enable runtime network impairment on both the inbound (client->server)
 * and outbound (server->client) datagram paths from an impairment spec
 * ("delay=75,jitter=30,loss=2,burst=2"; see netImpairParseConfig). On a
 * parse failure impairment is left off and a warning is logged. Drives off
 * the process-global bolo_rand stream, so seed it (bolo_srand) for a
 * reproducible run. */
void transportUdpServerSetNetImpair(const char *spec);

/* Run deferred sim-side removals for slots force-disconnected from inside
 * a control deliver callback (queue overflow). Call at a safe point in the
 * tick, outside any control-event publish. */
void transportUdpServerDrainPendingRemovals(struct ServerSim *sim);

/* Returns true if a dedicated recv thread is running. */
bool transportUdpServerHasRecvThread(void);

/* Drain sim events into per-client reliable queues.
 * Call after each serverSimTick() so events aren't lost when
 * multiple ticks run before transportUdpServerSend(). */
void transportUdpServerDrainEvents(struct ServerSim *sim);

/* Returns the number of currently connected clients. */
int transportUdpServerGetClientCount(void);

/* Returns the number of currently connected tankless spectators. */
int transportUdpServerGetSpectatorCount(void);

/* Returns ping for a given player (0 if not connected). */
uint16_t transportUdpServerGetClientPing(BYTE playerNum);

/* Test seam: overwrite a connected slot's measured ping so the high-ping
 * enforcement path (transportUdpServerEnforcePing) can be driven
 * deterministically. Real RTT over the unit tests' loopback socket is ~0ms and
 * would need seconds of wall-clock impairment to cross
 * PING_KICK_THRESHOLD_MS. No production caller — the measurement itself is
 * written by the PONG handler. No-op for an out-of-range or unconnected slot. */
void transportUdpServerSetClientPingForTest(BYTE playerNum, uint16_t pingMs);

/* Writes "ip:port" for a connected player into out; returns false (and an
 * empty string) for an out-of-range slot or one with no UDP client (bots,
 * the in-process host). out must be non-NULL with outLen > 0. */
bool transportUdpServerGetClientAddrStr(BYTE playerNum, char *out, size_t outLen);

/* Check all connected clients and warn/kick for sustained high ping. */
void transportUdpServerEnforcePing(struct ServerSim *sim);

/* Kick a player by name (case-insensitive match). */
void transportUdpServerKickPlayer(struct ServerSim *sim, const char *playerName);

/* Resolve playerName to a connected human slot and make it the lobby
 * host. Returns true if a matching player was found and set. */
bool transportUdpServerSetHostByName(struct ServerSim *sim, const char *playerName);

/* Dispatcher-side hooks for the ranked-only commands. The full
 * bodies live in transport_udp_server.c because they touch
 * udpServer.clients[] state, winbolonet, and SDL threading. */
bool transportUdpServerStartBalanceRequest(struct ServerSim *sim,
                                           uint8_t teamSize,
                                           bool includeBots);
void transportUdpServerHandleWbnReauth(struct ServerSim *sim,
                                       BYTE slot,
                                       const char *token);

/* Lock/unlock the game to prevent new players from joining.
 * Broadcasts a server message event to all clients. */
void transportUdpServerSetLock(struct ServerSim *sim, bool locked);
bool transportUdpServerGetLock(void);

/* Broadcast a server message to all connected clients (for "say" command). */
void transportUdpServerSendServerMessage(const char *message);

/* Print player status to stdout (for "status" command).
 * If toFile is TRUE, also write to "status.txt". */
void transportUdpServerPrintStatus(bool toFile);

/* Check for client timeouts — safe to call in any server state.
 * Disconnects clients that haven't sent packets within CLIENT_TIMEOUT_TICKS. */
void transportUdpServerCheckTimeouts(struct ServerSim *sim);

/* Returns true if any flag in `active` (MAX_TANKS-sized boolean
 * array) is set for an index other than `exceptIdx`. Pure
 * function — no globals, no side effects. Used by the
 * UPLOAD_BEGIN and USE_LOCAL handlers to single-thread map
 * uploads through the sim's lone preview slot: letting two
 * clients race purely produces data-loss UX. Public so unit
 * tests can verify the predicate without seeding udpServer. */
bool lobbyAnyOtherUploadActive(const bool *active, int exceptIdx);

/* Reset per-client and per-slot state for a fresh game.  Marks every
 * connected client as needing a player-list refresh, flags map download
 * complete, and clears reliable / map event queue sequence numbers for
 * all slots.  Callers run this on the countdown→running transition
 * before publishing the CTRL_GAME_PHASE_RUNNING event so the resets
 * land before the codec encodes PACKET_GAME_START. */
void transportUdpServerOnGameStart(struct ServerSim *sim);

/* The transport's own monotonic tick counter — advances every call to
 * transportUdpServerRecv / transportUdpServerDrainRecvQueue regardless of
 * sim state.  Used by callers that need a "clock that never freezes"
 * (e.g. the lobby retransmit cadence in server_lifecycle.c, which can't
 * gate on sim->tick because that field stops advancing during countdown
 * and gameOver states). */
uint32_t transportUdpServerGetTickCount(void);

/* Returns true if the UDP server has any connected client (including
 * the host's own loopback client when the host runs in
 * acceptRemoteClients mode).  Used by serverSimLobbyCheckAllReady to
 * decide between the in-place start (no countdown, pure in-process SP)
 * and the countdown path (anything that fans state over the wire and
 * therefore needs a settling window before client UIs flip to game
 * render mode). */
bool transportUdpServerHasAnyClient(void);

/* Refresh the server's compressed map data and re-prime each connected
 * client for download (resend JOIN_ACCEPT, reset chunk tracking).
 * Callers run this before publishing CTRL_LOBBY_MAP_CHANGE so the
 * per-client prep work lands before the codec encodes the
 * PACKET_LOBBY_MAP_CHANGE notification through the subscriber path. */
void transportUdpServerOnLobbyMapChange(struct ServerSim *sim);

/* ── Round-log source ─────────────────────────────────────────────────
 * Where PACKET_ROUND_LOG_REQ gets its bytes.  The replay recorder
 * (server_dedicated_log.c) registers itself here at install time and the
 * transport calls only through this table, naming no recorder symbol.  That
 * direction matters: a direct call the other way would pull the recorder —
 * and the WinBolo.net upload it needs — into every target that links the
 * transport, including the unit tests, the gym and the fuzz harnesses.  With
 * no source registered the server answers ROUND_LOG_ERR_DISABLED, which is
 * the honest answer for a build with no recorder in it. */
typedef enum {
    ROUND_LOG_READ_OK = 0,
    ROUND_LOG_READ_NONE,       /* no completed round to serve             */
    ROUND_LOG_READ_TOO_LARGE,  /* over ROUND_LOG_MAX_BYTES; never read    */
    ROUND_LOG_READ_ERROR       /* stat / open / read / allocation failure */
} RoundLogReadResult;

typedef struct {
    /* Whether the last completed round may be served right now.  Asked on
     * every request rather than latched, so a policy that depends on runtime
     * state (whether WinBolo.net is running, say) tracks that state. */
    bool (*serveEnabled)(void);
    /* Read the last completed round's log.  On ROUND_LOG_READ_OK, *outBuf is
     * a malloc'd buffer of *outLen bytes the caller owns and frees, and
     * outName holds the log file's basename; nothing is written on any other
     * result.  The size is checked before the read, so a file over the cap
     * never enters memory. */
    RoundLogReadResult (*read)(uint8_t **outBuf, uint32_t *outLen,
                               char *outName, size_t outNameSize);
} RoundLogSource;

/* Install the round-log source, or clear it by passing NULL (or a table with
 * a NULL member).  The struct is copied, so the caller need not keep it
 * alive, and the registration outlives a transport create/destroy cycle. */
void transportUdpServerSetRoundLogSource(const RoundLogSource *src);

/* Broadcast PACKET_WBN_REKEY to every connected WBN-participating client
 * carrying the current server_key.  Called after each round-end
 * winbolonetBeginSession succeeds so still-connected clients can mint a
 * fresh player_key against the rotated session and re-auth via the
 * existing lobby-snapshot machinery.  No-op when WBN isn't running. */
void transportUdpServerBroadcastWbnRekey(struct ServerSim *sim);

/* Boot every connected client and bot, notifying real clients with
 * PACKET_SERVER_SHUTDOWN first so they leave cleanly. Used by the no-lobby
 * map-rotation path at a round boundary — the next round carries nobody
 * forward. */
void transportUdpServerDisconnectAll(struct ServerSim *sim);

/* Set a bot's name in the server transport client array so it appears
 * in lobby state/update broadcasts. Call after botManagerAddBot(). */
void transportUdpServerSetBotName(BYTE playerNum, const char *name);

/* Get a connected client's player name (NULL if slot invalid/disconnected). */
const char *transportUdpServerGetPlayerName(BYTE playerNum);

/* Get a connected client's 2-char ISO country code (NULL if slot invalid
 * or disconnected). The pointer is into the transport's per-slot storage
 * — durable across serverSimResetGameWorld, which destroys the sim's
 * mirror in `players->item[i].location`. */
const char *transportUdpServerGetClientCountryCode(BYTE playerNum);

/* Get a connected client's clientType (CLIENT_TYPE_* constant) as
 * recorded at join time. Returns CLIENT_TYPE_UNKNOWN if slot invalid
 * or disconnected. Durable across serverSimResetGameWorld. */
uint8_t transportUdpServerGetClientType(BYTE playerNum);

/* Get a slot's connection id (0 if slot invalid or unassigned). */
uint64_t transportUdpServerGetClientConnId(BYTE playerNum);

/* Resolve the slot owning an inbound INPUT by its connection id. Returns the
 * matching connected slot, or -1 when connId is 0 or matches no slot (the
 * caller then falls back to an IP:port lookup). On a match from a source
 * address that differs from the slot's stored one, *outRehome is set true so
 * the caller re-homes the slot. A pure read of the client table — it does not
 * mutate, so the match-and-rehome decision is testable in isolation. */
int transportUdpServerFindByConnId(const UdpServerClient *clients,
                                   uint64_t connId,
                                   const struct sockaddr_in *fromAddr,
                                   bool *outRehome);

/* Send an INFO_RESPONSE packet to the tracker server so the game
 * appears in the server browser. */
void transportUdpServerSendTrackerUpdate(struct ServerSim *sim,
                                         const char *trackerAddr,
                                         unsigned short trackerPort);

/* Override the public address advertised in INFO_PACKET responses
 * (broadcast info requests + tracker updates).  Pass externalPort=0 (or
 * a NULL/empty externalIp) to revert to the internal port and zero
 * address.
 *
 * Used by the GUI host's UPnP/NAT-PMP path: once libplum reports the
 * external mapping, future INFO_PACKETs advertise the gateway's
 * external IP:port rather than the host's internal port (which
 * joiners on the public Internet can't reach). */
void transportUdpServerSetPublicAddress(const char *externalIp,
                                        unsigned short externalPort);

/* Send an 8-byte WBKA + game-token sentinel to the tracker over the
 * same socket the server is bound to, so the host's NAT mapping for
 * that source port stays alive between heavier tracker updates and
 * the tracker can disambiguate multiple games behind one NAT via the
 * (sourceIp, starttime) tuple. */
void transportUdpServerSendNatKeepalive(struct ServerSim *sim,
                                        const char *trackerAddr,
                                        unsigned short trackerPort);

/* Send an 8-byte PACKET_PUNCH_PROBE_REQUEST to the tracker on the
 * server's socket. The tracker replies (PACKET_PUNCH_PROBE_REPLY)
 * with the source IP:port it sees us as, so we can detect symmetric
 * NAT (libplum's external address vs the tracker's reflexive view)
 * and confirm bidirectional reachability before joiners attempt to
 * connect. */
void transportUdpServerSendPunchProbe(const char *trackerAddr,
                                      unsigned short trackerPort);

/* Send one punch packet per queued entry, throttled to roughly
 * PUNCH_BURST_INTERVAL ticks between sends. Called from the server
 * lifecycle tick. */
void transportUdpServerDrainPunchQueue(void);

/* Splice channel-delivered game events into `events` ahead of the map-tail
 * events staged at [tailStart, tailStart+tailCount), preserving game-then-map
 * order. Counts are clamped to `cap` so the splice never indexes past
 * events[cap]. Returns the new total event count. */
int spliceGameEventsBeforeTail(GameEvent *events, int tailStart, int tailCount,
                               const GameEvent *chan, int chanCount, int cap);

/* ── Test-only channel-mux scaffolding ───────────────────────────────────
 * Honest access to the parallel reliable-ordered channel layer (channel_mux.c)
 * for the loopback channel integration test.  Reliable game events now ride
 * channel 0 (CHANNEL_GAME); the other channels still run empty.
 *   *Send:    queue a whole message on a slot/channel's send side.
 *   *Receive: pop the next in-order message off a channel's receive side.
 *   *Stats:   read receive-side expectedSeq, send-side ackedSeq, and the
 *             running count of channel frames consumed on that endpoint.
 *   *PendingRemove: read a slot's deferred-disconnect flag (set when a
 *             channel send overflows mid-tick, cleared by the removal drain). */
bool transportUdpServerChannelTestSend(int slot, uint8_t ch,
                                       const uint8_t *msg, uint16_t len);
bool transportUdpServerTestPendingRemove(int slot);
/* Read a slot's server-side join-download-complete flag. The client reports
 * CONNECTED once it has the full map, but the server only flips this once the
 * download's bytes are acked back on CHANNEL_BULK — a round-trip later. A test
 * that drives the real game-event producer must wait on this, not just on the
 * client's connect state, or the producer skips the slot as still-downloading. */
bool transportUdpServerTestDownloadComplete(int slot);
/* Stage one terrain change for a slot as a real tick does: mutate the live
 * server map (so its checksum tracks the change) and enqueue an
 * EVENT_MAP_CHANGE into the slot's map-event hold queue, so it flows through
 * the real hold → tagged channelSend(CHANNEL_MAP) drain. Call between ticks. */
bool transportUdpServerTestAddMapEvent(ServerSim *sim, int slot, uint8_t x,
                                       uint8_t y, uint8_t terrain);
/* Queue one whole game event on a slot's reliable game channel (CHANNEL_GAME),
 * as the real producer does — lets a test stage a distinguishable ch0 event
 * (e.g. one left unacked across game start). False on a bad slot/event or a
 * full window. */
bool transportUdpServerTestAddGameEvent(int slot, const GameEvent *ev);
/* Fabricate a connected slot with a fresh channel mux so a server unit test can
 * drive transportUdpServerOnGameStart over two distinct slots without sockets. */
void transportUdpServerTestForceConnect(int slot, BYTE playerNum);
/* Decode the CTRL_CHANNEL_RESET this slot has queued on CHANNEL_CONTROL into its
 * two channel baselines. False if none is queued / it fails to decode. */
bool transportUdpServerTestPeekChannelReset(int slot, uint32_t *ch0Baseline,
                                            uint32_t *ch1Baseline);
void transportUdpServerChannelTestStats(int slot, uint8_t ch,
                                        uint32_t *expectedSeq,
                                        uint32_t *ackedSeq,
                                        uint32_t *framesRx);
bool transportUdpClientChannelTestReceive(Transport *t, uint8_t ch,
                                          uint8_t *out, uint16_t *outLen);
void transportUdpClientChannelTestStats(Transport *t, uint8_t ch,
                                        uint32_t *expectedSeq,
                                        uint32_t *ackedSeq,
                                        uint32_t *framesRx);
/* Begin a real map resync now (fresh generation + request), bypassing the
 * checksum-mismatch trigger; the server accept + blob install advance
 * installedMapGen as the harness pumps. Returns false if one is already
 * outstanding. */
bool transportUdpClientTestBeginResync(Transport *t);
/* Read installedMapGen (the generation gate floor) and mapResyncCount
 * (cumulative successful installs). */
void transportUdpClientTestMapState(Transport *t, uint32_t *installedMapGen,
                                    uint32_t *mapResyncCount);
/* Drive the real resync finalize on a caller-supplied blob (arming the resync
 * precondition with a fresh generation). A corrupt blob must leave
 * installedMapGen/mapResyncCount unchanged; a valid one advances them. Returns
 * false on bad args or allocation failure. */
bool transportUdpClientTestFinalizeResync(Transport *t, const BYTE *buf, int len);
/* Read the resync debounce state: whether a resync is in flight and the
 * consecutive-mismatch streak that gates a new request. */
void transportUdpClientTestResyncState(Transport *t, bool *resyncActive,
                                       uint32_t *mismatchStreak);
/* Read a connected spectator's armed seed blob (the spectator-owned copy of the
 * delayed ring keyframe). Returns the blob pointer with *outLen set to its
 * length and *outKind to the in-flight BulkSender kind (BULK_KIND_SPEC_SEED once
 * armed); NULL when the slot is invalid, disconnected, or not yet seeded.
 * outLen / outKind may be NULL. The pointer is freed when the seed completes. */
const uint8_t *transportUdpServerGetSpectatorSeed(int s, uint32_t *outLen,
                                                  uint8_t *outKind);
/* Read a connected spectator's forward-feed cursor: *outSeq = lastEmittedSeq
 * (the highest ring recordSeq emitted as a forward record) and *outKind = the
 * in-flight BulkSender kind (BULK_KIND_SPEC_RECORD once the feed has armed a
 * record). Returns false when the slot is invalid or disconnected; outSeq /
 * outKind may be NULL. */
bool transportUdpServerGetSpectatorFeedSeq(int s, uint32_t *outSeq,
                                           uint8_t *outKind);
/* Read a connected spectator's cold-start countdown state: returns true while
 * the spectator is waiting for the delayed ring to accumulate specDelayTicks of
 * history (no seed armed yet), with *outRemaining set to the ticks still owed;
 * false once it has transitioned to the normal seed, or when the slot is
 * invalid / disconnected. outRemaining may be NULL. */
bool transportUdpServerGetSpectatorCountdown(int s, uint32_t *outRemaining);
/* Test-only: mark a connected spectator's whole CHANNEL_BULK send window acked,
 * simulating a peer that keeps up so the seed completes and the forward feed's
 * window keeps draining without a real spectator channel endpoint. */
void transportUdpServerTestSpectatorAckBulk(int s);
/* Read a spectator's CHANNEL_CONTROL send sequence (nextSeq). The deliver
 * allowlist (serverSpectatorDeliverControl) is the only writer of that channel
 * for a live spectator, so a test can publish one control event and check
 * whether this advanced (event passed) or held (event dropped). Returns 0 for
 * an invalid slot. */
uint32_t transportUdpServerGetSpectatorControlSeq(int s);

/* Player-slot peer of the above: read a connected client's CHANNEL_CONTROL
 * send sequence (nextSeq). udpClientDeliverControl is the only writer, so a
 * test can publish one control event and check whether this advanced (the
 * per-recipient filters passed it) or held (filtered). Returns 0 for an
 * invalid slot. */
uint32_t transportUdpServerGetClientControlSeq(int slot);

/* True when spectator slot s is a live control-bus subscriber (lobby/countdown),
 * false when it is a delayed-ring reader or the slot is out of range. Lets a
 * test observe the live↔delayed cutover. */
bool transportUdpServerGetSpectatorLive(int s);

#endif /* TRANSPORT_UDP_H */
