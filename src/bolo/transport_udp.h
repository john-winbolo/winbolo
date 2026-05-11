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

/* WBN auth token: 64-char hex string + null */
#define WBN_TOKEN_WIRE_LEN 65

/* Join request packet (client -> server) */
typedef struct {
    PacketHeader hdr;
    char playerName[PACKET_MAX_PLAYER_NAME];
    char password[MAP_STR_SIZE];
    uint8_t versionMajor;
    uint8_t versionMinor;
    uint8_t versionRevision;
    char wbnToken[WBN_TOKEN_WIRE_LEN];
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

/* State of the client join handshake */
typedef enum {
    UDP_CLIENT_DISCONNECTED,
    UDP_CLIENT_JOINING,
    UDP_CLIENT_DOWNLOADING_MAP,
    UDP_CLIENT_CONNECTED,
    UDP_CLIENT_ERROR,
    UDP_CLIENT_SERVER_SHUTDOWN
} UdpClientJoinState;

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
                                   const char *password,
                                   const char *wbnToken,
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

/* Send a chat message to the server.
 * destPlayer: 0xFF = all players, else specific player number. */
void transportUdpClientSendChat(Transport *t, uint8_t destPlayer,
                                const char *message);

/* Send a name change request to the server.
 * newName: the desired new player name. */
void transportUdpClientSendNameChange(Transport *t, const char *newName);

/* Alliance operations */
void transportUdpClientSendAllianceRequest(Transport *t, uint8_t toPlayer);
void transportUdpClientSendAllianceAccept(Transport *t, uint8_t toPlayer);
void transportUdpClientSendAllianceLeave(Transport *t);

/* Send a lock toggle to the server.
 * allow: TRUE = allow new players, FALSE = disallow. */
void transportUdpClientSendLockToggle(Transport *t, bool allow);

/* Send team selection to server. teamNumber: 0-16. */
void transportUdpClientSendTeamSet(Transport *t, uint8_t teamNumber);

/* Send ready/unready to server. */
void transportUdpClientSendReady(Transport *t, bool ready);

/* Request server add a bot. */
/* Add a bot.
 *   teamNumber: target team (1..15); 0 lets the server pick a default.
 *   brainPath:  brain catalogue entry to assign; NULL/"" = server default.
 *   botName:    pool-picked display name; NULL/"" = server falls back to
 *               "Bot <slot>". */
void transportUdpClientSendAddBot(Transport *t, uint8_t teamNumber,
                                  const char *brainPath,
                                  const char *botName);

/* Request server remove a bot at the given slot. */
void transportUdpClientSendRemoveBot(Transport *t, uint8_t playerNum);

/* Re-authenticate WBN token after lobby reset between rounds. */
void transportUdpClientSendWbnReauth(Transport *t);

/* Request team balance from WBN (host only, enforcement is server-side). */
void transportUdpClientSendBalanceRequest(Transport *t, uint8_t teamSize);

/* Confirm and apply the current balance proposal. */
void transportUdpClientSendBalanceApply(Transport *t);

/* Dismiss the current balance proposal. */
void transportUdpClientSendBalanceDismiss(Transport *t);

/* Toggle map skip vote (server identifies player by source address). */
void transportUdpClientSendMapSkipVote(Transport *t);

/* ── Layout A lobby commands ───────────────────────────────────────
 * Each function ships one PACKET_LOBBY_* request to the server.
 * Server validates (host check OR openHost; lock check), applies,
 * broadcasts the corresponding _CHG event, then broadcasts an
 * AUTO_UNREADY. Failed validation comes back as a per-recipient
 * PACKET_LOBBY_REJECT (no UI surface yet — silent reject is fine
 * for v1, errors logged server-side). */

/* Set a single lobby setting (e.g. game type, hidden mines, time
 * limit, AI policy, autoLockOnGameStart). settingType is one of
 * the LST_* constants in netpacks.h; value is settingType-specific
 * (bool=1 byte, enum=1 byte, uint16=2 bytes BE). */
void transportUdpClientSendLobbySetting(Transport *t, uint8_t settingType,
                                        const uint8_t *value, uint8_t valueLen);

/* Toggle the openHost flag (host-only). */
void transportUdpClientSendLobbyOpenHost(Transport *t, bool openHost);

/* Set a team's metadata. Single packet handles create + rename +
 * recolor + naming-pool change. teamId in 1..MAX_TANKS-1. */
void transportUdpClientSendLobbyTeamMeta(Transport *t, uint8_t teamId,
                                         uint8_t color, uint8_t namingPool,
                                         const char *name);

/* Clear a team's metadata (back to defaults). Members stay on the
 * teamId; host can manually move them after. */
void transportUdpClientSendLobbyTeamClear(Transport *t, uint8_t teamId);

/* Set a bot's name + difficulty + personality. */
void transportUdpClientSendLobbyBotConfig(Transport *t, uint8_t slot,
                                          uint8_t difficulty, uint8_t personality,
                                          const char *name);

/* Change which Lua brain a lobby bot uses. Host (or openHost) only. */
void transportUdpClientSendLobbySetBotBrain(Transport *t, uint8_t slot,
                                            const char *brainPath);

/* Kick a player out of the lobby. Host action. */
void transportUdpClientSendLobbyKick(Transport *t, uint8_t slot);

/* Returns the server's reject reason string after a failed join.
 * Returns NULL if no reject reason is available. */
const char *transportUdpClientGetJoinRejectReason(Transport *t);

/* Returns the downloaded map data after successful join.
 * Returns NULL if no map has been downloaded yet.
 * outLen receives the length of the compressed data. */
const BYTE *transportUdpClientGetMapData(Transport *t, int *outLen);

/* Returns the game settings received from the server during join. */
void transportUdpClientGetGameSettings(Transport *t, gameType *game,
                                       bool *hiddenMines, int32_t *startDelay,
                                       int32_t *gameLen);


/*********************************************************
 * UDP Transport — Server Side
 *********************************************************/

/* Ping enforcement thresholds */
#define PING_WARN_THRESHOLD_MS  400   /* RTT ms — warn after consecutive breaches */
#define PING_KICK_THRESHOLD_MS  500   /* RTT ms — kick after consecutive breaches */
#define PING_WARN_COUNT         3     /* consecutive pings at warn threshold before warning */
#define PING_KICK_COUNT         5     /* consecutive pings at kick threshold before kick */

/* Per-client connection info tracked by the server */
typedef struct {
    struct sockaddr_in addr;
    bool connected;
    uint8_t playerNum;
    char playerName[PACKET_MAX_PLAYER_NAME];
    uint32_t lastReceivedTick;   /* For timeout detection */
    uint32_t outSequence;        /* Outgoing packet sequence */
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
} UdpServerClient;

/* Creates a server-side UDP transport.
 * Binds to the given port and starts accepting connections.
 * sim: the authoritative ServerSim that inputs will be applied to.
 * password: game password (empty string if none).
 * maxPlayers: maximum allowed players (0 = MAX_TANKS). */
bool transportUdpServerCreate(unsigned short port,
                              const char *addrToUse,
                              struct ServerSim *sim,
                              const char *password,
                              BYTE maxPlayers);

/* Destroys the server-side UDP transport. */
void transportUdpServerDestroy(void);

/* Server tick: receive all pending inputs, broadcast snapshots.
 * Call this once per tick after serverSimTick(). */
void transportUdpServerTick(struct ServerSim *sim);

/* Split receive/send for callers that need to tick the sim in between. */
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

/* Lock/unlock the game to prevent new players from joining.
 * Broadcasts a server message event to all clients. */
void transportUdpServerSetLock(struct ServerSim *sim, bool locked);
bool transportUdpServerGetLock(void);

/* Broadcast a server message to all connected clients (for "say" command). */
void transportUdpServerSendServerMessage(const char *message);

/* Print player status to stdout (for "status" command).
 * If toFile is TRUE, also write to "status.txt". */
void transportUdpServerPrintStatus(bool toFile);

/* Returns the max players setting. */
BYTE transportUdpServerGetMaxPlayers(void);

/* Check for client timeouts — safe to call in any server state.
 * Disconnects clients that haven't sent packets within CLIENT_TIMEOUT_TICKS. */
void transportUdpServerCheckTimeouts(struct ServerSim *sim);

/* ---- Lobby broadcast functions ---- */

/* Broadcast full lobby state snapshot to all connected clients. */
void transportUdpServerBroadcastLobbyState(struct ServerSim *sim);

/* Broadcast a single-player lobby update to all connected clients. */
void transportUdpServerBroadcastLobbyUpdate(struct ServerSim *sim, BYTE playerNum);

/* Broadcast countdown seconds remaining to all connected clients. */
void transportUdpServerBroadcastCountdown(struct ServerSim *sim, uint8_t secondsRemaining);

/* Broadcast game start signal to all connected clients. */
void transportUdpServerBroadcastGameStart(struct ServerSim *sim);

/* Broadcast game over signal to all connected clients. */
void transportUdpServerBroadcastGameOver(struct ServerSim *sim);

/* Notify all connected clients that the map has changed, refresh the
 * server's compressed map data, and trigger re-download for each client. */
void transportUdpServerNotifyMapChange(struct ServerSim *sim);

/* Broadcast a team balance proposal (one team assignment per slot) to all clients. */
void transportUdpServerBroadcastBalanceProposal(struct ServerSim *sim, uint8_t teamForSlot[MAX_TANKS]);

/* Broadcast the current map skip vote state (one byte per slot) to all clients. */
void transportUdpServerBroadcastMapSkipState(struct ServerSim *sim);

/* ── Layout A lobby — server broadcast helpers ─────────────────────
 * Each is sent in response to an applied client command, plus
 * (for SETTING and OPEN_HOST) on initial state sync. The TEAM_META
 * helper reads the current team metadata from sim->teams[teamId];
 * BOT_CONFIG_CHG reads sim->botConfigs[slot] + the bot's name from
 * the udpServer client array. AUTO_UNREADY also clears server-side
 * ready flags before sending the signal. */
void transportUdpServerBroadcastLobbySettingChg(struct ServerSim *sim,
                                                uint8_t settingType,
                                                const uint8_t *value,
                                                uint8_t valueLen);
void transportUdpServerBroadcastLobbyOpenHostChg(struct ServerSim *sim,
                                                 bool openHost);
void transportUdpServerBroadcastLobbyTeamMetaChg(struct ServerSim *sim,
                                                 uint8_t teamId);
void transportUdpServerBroadcastLobbyBotConfigChg(struct ServerSim *sim,
                                                  uint8_t slot);
void transportUdpServerBroadcastLobbyAutoUnready(struct ServerSim *sim);

/* Set a bot's name in the server transport client array so it appears
 * in lobby state/update broadcasts. Call after botManagerAddBot(). */
void transportUdpServerSetBotName(BYTE playerNum, const char *name);

/* Get a connected client's player name (NULL if slot invalid/disconnected). */
const char *transportUdpServerGetPlayerName(BYTE playerNum);

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
