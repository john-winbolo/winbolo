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
 *Name:          Client Simulation
 *Filename:      client_sim.h
 *Author:        John Morrison
 *Purpose:
 *  ClientSim struct that embeds GameSim as its first member.
 *  Holds all client-side game state that was previously
 *  scattered across screen.c globals.
 *********************************************************/

#ifndef CLIENT_SIM_H
#define CLIENT_SIM_H

#include <stdio.h>    /* For FILENAME_MAX */
#include <time.h>     /* For time_t */
#include "game_sim.h"
#include "client_state.h"
#include "interpolation.h"
#include "input_packet.h"
#include "screen.h"
#include "brain.h"  /* For BuildInfo, ObjectInfo, aiType */
#include "players.h" /* For PlayerBitMap */
#include "bolo_packets.h" /* For netType, netStatus enums */

/* Client-side mirror of server lobby slot state */
typedef struct {
    bool connected;
    char playerName[32];   /* PACKET_MAX_PLAYER_NAME */
    uint8_t teamNumber;    /* 0 = unassigned, 1-16 */
    bool ready;
    bool isBot;
    uint16_t pingMs;       /* Player ping in ms */
    char countryCode[3];   /* ISO 3166-1 alpha-2 (e.g. "US") */
} ClientLobbySlot;

/* Callback typedefs for new transport message sending */
typedef void (*NetChatSendFunc)(uint8_t destPlayer, const char *message);
typedef void (*NetNameChangeSendFunc)(const char *newName);
typedef void (*NetAllianceRequestFunc)(uint8_t toPlayer);
typedef void (*NetAllianceAcceptFunc)(uint8_t toPlayer);
typedef void (*NetAllianceLeaveFunc)(void);
typedef void (*NetLockToggleSendFunc)(bool allow);

/* Maximum predicted shells the client can track at once */
#define MAX_PREDICTED_SHELLS 8

/* A client-side predicted shell, created instantly on fire input
 * and removed once the server has processed the fire tick. */
typedef struct {
    WORLD x;
    WORLD y;
    TURNTYPE angle;
    uint8_t length;
    uint8_t owner;
    bool onBoat;         /* Was the shell launched from a boat */
    uint32_t fireTick;   /* The input tick that created this shell */
    bool active;
} PredictedShell;

typedef struct ClientSim {
    GameSim     sim;    /* MUST be first member */

    /* Client-side prediction state */
    ClientState clientState;

    /* Interpolation state for other players' tanks */
    InterpContext interpCtx;

    /* Server shell snapshots for UDP mode */
    ShellSnapshot serverShellSnaps[MAX_SNAPSHOT_SHELLS];
    int         serverShellCount;

    /* Client-side predicted shells (Phase 4 of client-prediction) */
    PredictedShell predictedShells[MAX_PREDICTED_SHELLS];
    int         predictedShellCount;

    /* Pending human-player build request */
    BYTE        pendingBuildAction;
    BYTE        pendingBuildX;
    BYTE        pendingBuildY;

    /* Current building item selected (was BsCurrent) */
    buildSelect currentBuildSelect;

    /* Are we running? */
    bool        running;

    /* Is this a bot ClientSim? (bot sims must not trigger frontend UI calls) */
    bool        isBot;

    /* Brain state (per-instance, moved from static globals in client_sim.c) */
    uint32_t    brainHoldKeys;
    uint32_t    brainTapKeys;
    BuildInfo  *brainBuildInfo;
    PlayerBitMap brainsWantAllies;
    PlayerBitMap brainsMessageDest;
    char        brainsMessage[FILENAME_MAX];
    unsigned short brainsNumObjects;
    ObjectInfo  brainObjects[1024];
    aiType      allowComputerTanks;

    /* Brain event buffer — filled in clientSimSyncFromSnapshot, consumed in screenMakeBrainInfoCS */
#define MAX_BRAIN_EVENTS 512
    GameEvent  brainEvents[MAX_BRAIN_EVENTS];
    int        brainEventCount;
    uint32_t   lastServerTick;
    uint8_t    lastServerArmour;    /* Previous server snapshot armour for death detection */
    uint8_t    brainLastAssistMsg;  /* ASSIST_MSG_* or 0 */

    /* Per-instance fog-of-war brain map (was global sbm[256][256] in screenbrainmap.c) */
    BYTE        brainMap[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];

    /* Client viewport / display state (was screen.c module-level statics) */
    screen      view;
    screenMines mineView;
    BYTE        xOffset;
    BYTE        yOffset;
    bool        inPillView;
    BYTE        pillViewX;
    BYTE        pillViewY;
    int         cursorPosX;
    int         cursorPosY;
    char        mapName[MAP_STR_SIZE];
    int         gmeStartDelay;
    int32_t     gmeLength;
    time_t      timeStart;
    bool        needScreenReCalc;

    /* Network state (moved from network.c globals) */
    netType     networkGameType;
    netStatus   netStat;
    NetChatSendFunc         chatSendFunc;
    NetNameChangeSendFunc   nameChangeSendFunc;
    NetAllianceRequestFunc  allianceRequestFunc;
    NetAllianceAcceptFunc   allianceAcceptFunc;
    NetAllianceLeaveFunc    allianceLeaveFunc;
    NetLockToggleSendFunc   lockToggleSendFunc;

    /* Server address info (for brain info, replaces netClientGetServerAddress) */
    struct in_addr serverAddress;
    unsigned short serverPort;

    /* Lobby state (client-side mirror of server lobby) */
    ClientLobbySlot  lobbySlots[16];    /* MAX_TANKS */
    int              countdownSeconds;  /* 0 = not counting down */
    bool             mapDownloadComplete; /* Gate for ready button */
    bool             inLobby;           /* TRUE if server is lobby-enabled */
    char             lobbyChatHistory[4096]; /* Lobby chat buffer with player names */

    /* Lobby game settings (received from server in LOBBY_STATE packet) */
    gameType         lobbyGameType;
    bool             lobbyHiddenMines;
    uint8_t          lobbyAiType;       /* 0=none, 1=yes, 2=yesAdvantage, 3=full */
    int32_t          lobbyTimeLimit;    /* Game length in ticks (-1 = unlimited) */
    uint8_t          lobbyPillCount;
    uint8_t          lobbyBaseCount;
    uint8_t          lobbyStartCount;
} ClientSim;

/* Lifecycle API — initializes/destroys the ClientSim struct */
bool clientSimCreate(ClientSim *cs, gameType game, bool hiddenMines, int srtDelay, int32_t gmeLen);
void clientSimDestroy(ClientSim *cs);
void clientSimKeysTick(ClientSim *cs, const InputPacket *pkt);
void clientSimGameTick(ClientSim *cs, const InputPacket *pkt, bool isBrain);
void clientSimSyncFromSnapshot(ClientSim *cs, const SnapshotHeader *hdr,
                               const TankSnapshot *tanks, int tankCount,
                               const ShellSnapshot *shellSnaps, int shellCount,
                               const ExplosionSnapshot *explSnaps, int explosionCount,
                               const BaseSnapshot *baseSnaps, int baseCount,
                               const PillSnapshot *pillSnaps, int pillCount,
                               const GameEvent *events, int eventCount,
                               BYTE playerNum);
void clientSimDisplayTick(ClientSim *cs, bool isBrain);

/* Advance predicted shells by one tick (move forward, decrement length) */
void clientSimAdvancePredictedShells(ClientSim *cs);

/* No-op — predicted shells expire naturally via length counter */
void clientSimReconcilePredictedShells(ClientSim *cs, uint32_t lastProcessedInput);

/* Brain state accessors — used by screen.c for brain input handling.
 * Each takes a ClientSim* so multiple instances can have independent brain state. */
uint32_t *clientSimGetBrainHoldKeys(ClientSim *cs);
uint32_t *clientSimGetBrainTapKeys(ClientSim *cs);
BuildInfo **clientSimGetBrainBuildInfo(ClientSim *cs);
PlayerBitMap *clientSimGetBrainsWantAllies(ClientSim *cs);
PlayerBitMap *clientSimGetBrainsMessageDest(ClientSim *cs);
char *clientSimGetBrainsMessage(ClientSim *cs);
unsigned short *clientSimGetBrainsNumObjects(ClientSim *cs);
ObjectInfo *clientSimGetBrainObjects(ClientSim *cs);
aiType *clientSimGetAllowComputerTanks(ClientSim *cs);

/* Network state accessors (per-instance) */
netType clientSimGetNetType(ClientSim *cs);
netStatus clientSimGetNetStatus(ClientSim *cs);
void clientSimSetNetType(ClientSim *cs, netType value);
void clientSimSetNetStatus(ClientSim *cs, netStatus value);

/* Callback setters (per-instance) */
void clientSimSetChatSendFunc(ClientSim *cs, NetChatSendFunc func);
void clientSimSetNameChangeSendFunc(ClientSim *cs, NetNameChangeSendFunc func);
void clientSimSetAllianceRequestFunc(ClientSim *cs, NetAllianceRequestFunc func);
void clientSimSetAllianceAcceptFunc(ClientSim *cs, NetAllianceAcceptFunc func);
void clientSimSetAllianceLeaveFunc(ClientSim *cs, NetAllianceLeaveFunc func);
void clientSimSetLockToggleSendFunc(ClientSim *cs, NetLockToggleSendFunc func);

/* Lobby chat helper — appends "name: message\n" to lobbyChatHistory */
void clientSimAppendLobbyChat(ClientSim *cs, const char *name, const char *message);

/* Message functions (per-instance) */
void clientSimMessageSendAllPlayers(ClientSim *cs, BYTE playerNum, char *message);
void clientSimMessageSendPlayer(ClientSim *cs, BYTE playerNum, BYTE destPlayer, char *message);
void clientSimSendChangePlayerName(ClientSim *cs, BYTE playerNum, char *newName);
void clientSimRequestAlliance(ClientSim *cs, BYTE playerNum, BYTE requestTo);
void clientSimAllianceAccept(ClientSim *cs, BYTE playerNum);
void clientSimLeaveAlliance(ClientSim *cs, BYTE playerNum);
void clientSimSetAllowNewPlayers(ClientSim *cs, bool allow);

void netGetStats(ClientSim *cs, char *status, int *ping, int *ppsec, int *retrans);
void netGetServerAddressStr(ClientSim *cs, char *dest);
void netGetOurAddressStr(ClientSim *cs, char *dest);
BYTE netGetDownloadPos(void);
void netSecond(void);
int netGetNetTime(void);
bool netSetup(ClientSim *cs, netType value, unsigned short myPort, char *targetIp, unsigned short targetPort, char *password, bool usCreate, char *trackerAddr, unsigned short trackerPort, bool useTracker, bool wantRejoin, bool useWinboloNet, char *wbnPassword);
void netDestroy(ClientSim *cs);
void netSendTrackerUpdate(void);
void netProcessedDnsLookup(ClientSim *cs, char *ip, char *host);

#endif /* CLIENT_SIM_H */
