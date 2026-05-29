/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * trackerPort: ignored if trackerAddr empty. */
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
                                   unsigned short trackerPort);

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
 * bytes/sec received, bytes/sec sent, and cumulative error count. */
void transportUdpClientGetNetStats(Transport *t, int *ppsRecv, int *ppsSent,
                                   int *bpsRecv, int *bpsSent, int *numErrors);

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
void transportUdpClientSendLobbyMapUploadBegin(Transport *t, uint32_t totalLen,
                                               const char *name);
void transportUdpClientSendLobbyMapUploadChunk(Transport *t, uint32_t offset,
                                               const uint8_t *data,
                                               uint16_t dataLen);
/* Pre-upload optimisation: try to skip the byte transfer if the server
 * already has an identical file at relPath (relative to data/maps/).
 * Server replies PACKET_LOBBY_MAP_UPLOAD_DONE on match, or
 * PACKET_LOBBY_MAP_USE_LOCAL_NACK on miss — caller falls back to
 * UploadBegin/Chunk on NACK. */
void transportUdpClientSendLobbyMapUseLocal(Transport *t, uint32_t totalLen,
                                             const char *name,
                                             const char *relPath,
                                             const uint8_t md5[16]);

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

/* Re-authenticate WBN token after lobby reset between rounds. */
void transportUdpClientSendWbnReauth(Transport *t);

/* Returns the server's reject reason string after a failed join.
 * Returns NULL if no reject reason is available. */
const char *transportUdpClientGetJoinRejectReason(Transport *t);

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
    bool needsPlayerList;        /* Send existing player names after map download */
    uint16_t inputsThisTick;     /* Inputs applied this tick cycle (for rate limiting) */
    bool wantRejoin;             /* Client requested rejoin (restore pills/bases) */
    uint8_t pingWarnStrikes;     /* consecutive pings >= warn threshold */
    uint8_t pingKickStrikes;     /* consecutive pings >= kick threshold */
    bool    pingWarned;          /* warning already sent this streak */
    uint16_t lastEnforcedPingMs; /* pingMs value last time enforcement ran */
    bool    nameStickySuffix;    /* Phase 5: server-renamed by verified-priority
                                  * collision; keep the suffixed name for the
                                  * rest of the session.  Cleared on disconnect. */
    /* Set once at JOIN_REQUEST and not refreshed mid-connection.  Server does
     * not push updates if e.g. a Steam Deck docks mid-game; this is
     * intentional, not a bug. */
    uint8_t clientType;          /* immutable after JOIN_REQUEST */
    uint8_t clientHints;         /* immutable after JOIN_REQUEST; SUPPORTER|STEAM_BUILD only */
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

/* Returns true if a dedicated recv thread is running. */
bool transportUdpServerHasRecvThread(void);

/* Drain sim events into per-client reliable queues.
 * Call after each serverSimTick() so events aren't lost when
 * multiple ticks run before transportUdpServerSend(). */
void transportUdpServerDrainEvents(struct ServerSim *sim);

/* Returns the number of currently connected clients. */
int transportUdpServerGetClientCount(void);

/* Returns ping for a given player (0 if not connected). */
uint16_t transportUdpServerGetClientPing(BYTE playerNum);

/* Check all connected clients and warn/kick for sustained high ping. */
void transportUdpServerEnforcePing(struct ServerSim *sim);

/* Kick a player by name (case-insensitive match). */
void transportUdpServerKickPlayer(struct ServerSim *sim, const char *playerName);

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

/* Drive the lobby/countdown/gameover retransmit scan.  For every
 * connected client with unacked control events, sends a fresh
 * PACKET_CONTROL_TICK carrying the unacked tail.  Snapshots cover
 * retransmit automatically during running; this exists for the phases
 * where snapshots don't flow.  Called from server_lifecycle.c at a
 * 4-tick (~80ms at 50 Hz) cadence. */
void transportUdpServerRetransmitUnackedControl(void);

/* Broadcast PACKET_WBN_REKEY to every connected WBN-participating client
 * carrying the current server_key.  Called after each
 * winbolonetReturnToLobby succeeds so still-connected clients can mint a
 * fresh player_key against the rotated session and re-auth via the
 * existing lobby-snapshot machinery.  No-op when WBN isn't running. */
void transportUdpServerBroadcastWbnRekey(struct ServerSim *sim);

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

#endif /* TRANSPORT_UDP_H */
