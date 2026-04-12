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
 * password: game password (empty string if none). */
Transport transportUdpClientCreate(struct ClientSim *clientSim,
                                   const char *serverAddr,
                                   unsigned short serverPort,
                                   const char *playerName,
                                   const char *password,
                                   const char *wbnToken,
                                   bool wantRejoin);

/* Destroys a client-side UDP transport. */
void transportUdpClientDestroy(Transport *t);

/* Returns the client join state (for UI to display connecting/error). */
UdpClientJoinState transportUdpClientGetJoinState(Transport *t);

/* Returns the assigned player number after successful join. */
BYTE transportUdpClientGetPlayerNum(Transport *t);

/* Returns TRUE if a new snapshot is available.
 * If TRUE, copies the snapshot header, tank data, shell data,
 * explosion data, base/pill state, and game events into the
 * output params. Arrays may be NULL if not needed. */
bool transportUdpClientGetSnapshot(Transport *t,
                                   SnapshotHeader *hdr,
                                   TankSnapshot *tanks,
                                   int maxTanks,
                                   ShellSnapshot *shellsOut,
                                   int maxShells,
                                   ExplosionSnapshot *explosionsOut,
                                   int maxExplosions,
                                   BaseSnapshot *basesOut,
                                   int maxBases,
                                   PillSnapshot *pillsOut,
                                   int maxPills,
                                   GameEvent *eventsOut,
                                   int maxEvents);

/* Returns the current ping in milliseconds. */
uint16_t transportUdpClientGetPing(Transport *t);

/* Returns client-side network stats: packets/sec received, packets/sec sent,
 * and cumulative error count (stale/truncated packets). */
void transportUdpClientGetNetStats(Transport *t, int *ppsRecv, int *ppsSent, int *numErrors);

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
void transportUdpClientSendAddBot(Transport *t);

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
    char countryCode[3];         /* ISO 3166-1 alpha-2 from GeoIP lookup */
    bool needsPlayerList;        /* Send existing player names after map download */
    uint16_t inputsThisTick;     /* Inputs applied this tick cycle (for rate limiting) */
    bool wantRejoin;             /* Client requested rejoin (restore pills/bases) */
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

/* Drain sim events into per-client reliable queues.
 * Call after each serverSimTick() so events aren't lost when
 * multiple ticks run before transportUdpServerSend(). */
void transportUdpServerDrainEvents(struct ServerSim *sim);

/* Returns the number of currently connected clients. */
int transportUdpServerGetClientCount(void);

/* Returns ping for a given player (0 if not connected). */
uint16_t transportUdpServerGetClientPing(BYTE playerNum);

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

#endif /* TRANSPORT_UDP_H */
