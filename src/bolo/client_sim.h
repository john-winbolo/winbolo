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
#include "netpacks.h"     /* For PACKET_MAX_PLAYER_NAME */
#include "screen.h"
#include "brain.h"  /* For BuildInfo, ObjectInfo, aiType */
#include "players.h" /* For PlayerBitMap */
#include "bolo_packets.h" /* For netType, netStatus enums */
#include "messages.h"     /* For MessageState */
#include "scroll.h"       /* For ScrollState */

/* Client-side mirror of server lobby slot state */
typedef struct {
    bool connected;
    char playerName[PACKET_MAX_PLAYER_NAME];
    uint8_t teamNumber;    /* 0 = unassigned, 1-16 */
    bool ready;
    bool isBot;
    uint16_t pingMs;       /* Player ping in ms */
    char countryCode[3];   /* ISO 3166-1 alpha-2 (e.g. "US") */
    uint8_t clientFlags;   /* PLAYER_FLAG_* bits */
    uint8_t clientType;    /* ClientType enum */
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
    float fx;            /* float position accumulator — authoritative position */
    float fy;
    float vx;            /* float velocity per tick (SHELL_SPEED * cos/sin) */
    float vy;
    TURNTYPE angle;
    uint8_t length;
    uint8_t owner;
    bool onBoat;         /* Was the shell launched from a boat */
    uint32_t fireTick;   /* The input tick that created this shell */
    bool active;
} PredictedShell;

#ifndef CLIENTSIM_TYPEDEF
#define CLIENTSIM_TYPEDEF
typedef struct ClientSim ClientSim;
#endif
struct ClientSim {
    GameSim     sim;    /* MUST be first member */
    BYTE        myPlayerNum; /* Server-assigned player number; tanks[myPlayerNum] is our tank */

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

    /* Per-instance message state (was messages.c globals) */
    MessageState messages;

    /* Per-instance scroll state (was scroll.c globals) */
    ScrollState scroll;

    /* Per-instance label state (was labels.c globals) */
    bool        labelOwnTank;
    labelLen    labelMessage;
    labelLen    labelTankLabel;

    /* Per-instance last player name (was players.c global) */
    char        myLastPlayerName[PLAYER_NAME_LEN];

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
    bool             mapSkipAvailable;  /* Server has map rotation with >1 map */
    bool             mapSkipVotes[16];  /* Mirror of server vote state */
    bool             mapSkipMyVote;     /* Local tracking of own vote */

    /* Team balance proposal from WBN */
    uint8_t  balanceProposal[16];      /* Proposed team per slot (0 = none) */
    bool     balanceProposalActive;    /* TRUE if a proposal is being displayed */

    /* Steam achievement: first capture tracking (per-game) */
    bool     hasAnyBaseCaptured;
    bool     hasAnyPillCaptured;

    /* Steam achievement: per-game death/loss counters (zeroed by memset in clientSimCreate) */
    uint16_t myDeathsThisGame;
    uint16_t myLgmLossesThisGame;

    /* Steam achievement: player count tracking (ACH_PLAYERS_6/8/16) */
    uint8_t  maxPlayersSeenThisGame;

    /* Steam achievement: lonely lobby tracking (ACH_LONELY_LOBBY) */
    uint32_t lobbyAloneStartTick;

    /* Steam achievement: rapid death tracking (ACH_RAPID_DEATH) */
    uint32_t deathTimestamps[10];
    uint8_t  deathTimestampIdx;
};

/* Access the local player's tank and LGM by server player number */
#define MY_TANK(cs) ((cs)->sim.tanks[(cs)->myPlayerNum])
#define MY_LGM(cs)  ((cs)->sim.lgmen[(cs)->myPlayerNum])

/* Recover the owning ClientSim from a GameSim* for the frontEnd
 * active-cs gate. Relies on the "GameSim sim MUST be first member"
 * invariant declared above (and mirrored in server_sim.h). When the
 * GameSim belongs to a ServerSim (sim->isServer == true) the cast
 * would yield a bogus pointer, so we return NULL; the gate then
 * suppresses safely because NULL never matches the registered active
 * humanSim. Callers in bolo/<*> with only a GameSim* in scope use this
 * to feed frontEnd*(cs, ...) calls without having to thread cs
 * through every signature. */
static inline struct ClientSim *clientSimFromSim(struct GameSim *sim) {
    if (sim == NULL || sim->isServer) return NULL;
    return (struct ClientSim *)sim;
}

/* Lifecycle API — initializes/destroys the ClientSim struct */
bool clientSimCreate(ClientSim *cs, gameType game, bool hiddenMines, int srtDelay, int32_t gmeLen);
void clientSimDestroy(ClientSim *cs);
void clientSimSetPlayerNum(ClientSim *cs, BYTE playerNum);

/*********************************************************
 *NAME:          clientSimSetupSelf
 *PURPOSE:
 *  Initialise the local self-record on a freshly-created
 *  ClientSim. Replaces the inline tankCreate +
 *  playersSetSelf + playersSetClientType +
 *  playersSetClientFlags sequences in screen.c and
 *  bot_manager.c. Pure data-model init — no UI side effects.
 *  The caller is responsible for any frontend updates
 *  (frontEndSetPlayer, frontEndUpdateTankStatusBars).
 *
 *  cs->myPlayerNum must already be set (via
 *  clientSimSetPlayerNum) before this call.
 *
 *ARGUMENTS:
 *  cs          - The ClientSim to initialise self on
 *  playerNum   - The player slot for the self-record
 *  playerName  - Display name (or brain name for bots)
 *  clientType  - CLIENT_TYPE_* enum value; pass 0 for bots
 *  clientFlags - PLAYER_FLAG_* bitmask; pass 0 for bots
 *********************************************************/
void clientSimSetupSelf(ClientSim *cs, BYTE playerNum,
                        const char *playerName,
                        uint8_t clientType, uint8_t clientFlags);

void clientSimKeysTick(ClientSim *cs, const InputPacket *pkt);
void clientSimGameTick(ClientSim *cs, const InputPacket *pkt, bool isBrain);
void clientSimSyncFromSnapshot(ClientSim *cs, const SnapshotHeader *hdr,
                               const TankSnapshot *tanks, int tankCount,
                               const ShellSnapshot *shellSnaps, int shellCount,
                               const TkExplosionSnapshot *tkExplSnaps, int tkExplosionCount,
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
bool netSetup(ClientSim *cs, netType value, unsigned short myPort, char *targetIp, unsigned short targetPort, char *password, bool usCreate, char *trackerAddr, unsigned short trackerPort, bool useTracker, bool wantRejoin, bool useWinboloNet, char *wbnToken);
void netDestroy(ClientSim *cs);
void netSendTrackerUpdate(void);
void netProcessedDnsLookup(ClientSim *cs, char *ip, char *host);

/*********************************************************
 * Read accessors.
 *
 * One per externally-read ClientSim field. Callers outside
 * src/bolo/client_sim.c, client_sim_control.c,
 * client_snapshot.c, and transport_udp_client.c use these
 * instead of touching fields directly. No mutator versions
 * here — writes go through the public mutator/control-event
 * API. Indexed accessors are bounds-checked: out-of-range
 * returns NULL for pointer types, false/0 for scalars.
 *********************************************************/

/* Scalar (bool/enum) accessors */
bool         clientSimIsRunning(const ClientSim *cs);
bool         clientSimIsBot(const ClientSim *cs);
bool         clientSimIsInPillView(const ClientSim *cs);
bool         clientSimIsNeedScreenReCalc(const ClientSim *cs);
bool         clientSimIsInLobby(const ClientSim *cs);
bool         clientSimIsMapDownloadComplete(const ClientSim *cs);
bool         clientSimIsMapSkipAvailable(const ClientSim *cs);
bool         clientSimIsMapSkipMyVote(const ClientSim *cs);
bool         clientSimIsLobbyHiddenMines(const ClientSim *cs);
bool         clientSimIsBalanceProposalActive(const ClientSim *cs);
bool         clientSimIsLabelOwnTank(const ClientSim *cs);
buildSelect  clientSimGetCurrentBuildSelect(const ClientSim *cs);
gameType     clientSimGetLobbyGameType(const ClientSim *cs);
labelLen     clientSimGetLabelMessage(const ClientSim *cs);
labelLen     clientSimGetLabelTankLabel(const ClientSim *cs);

/* Scalar (integer) accessors */
BYTE           clientSimGetMyPlayerNum(const ClientSim *cs);
BYTE           clientSimGetXOffset(const ClientSim *cs);
BYTE           clientSimGetYOffset(const ClientSim *cs);
BYTE           clientSimGetPillViewX(const ClientSim *cs);
BYTE           clientSimGetPillViewY(const ClientSim *cs);
BYTE           clientSimGetPendingBuildAction(const ClientSim *cs);
BYTE           clientSimGetPendingBuildX(const ClientSim *cs);
BYTE           clientSimGetPendingBuildY(const ClientSim *cs);
int            clientSimGetCursorPosX(const ClientSim *cs);
int            clientSimGetCursorPosY(const ClientSim *cs);
int            clientSimGetGmeStartDelay(const ClientSim *cs);
int            clientSimGetCountdownSeconds(const ClientSim *cs);
int            clientSimGetServerShellCount(const ClientSim *cs);
int            clientSimGetPredictedShellCount(const ClientSim *cs);
int            clientSimGetBrainEventCount(const ClientSim *cs);
int32_t        clientSimGetGmeLength(const ClientSim *cs);
int32_t        clientSimGetLobbyTimeLimit(const ClientSim *cs);
uint8_t        clientSimGetLobbyAiType(const ClientSim *cs);
uint8_t        clientSimGetLobbyPillCount(const ClientSim *cs);
uint8_t        clientSimGetLobbyBaseCount(const ClientSim *cs);
uint8_t        clientSimGetLobbyStartCount(const ClientSim *cs);
uint8_t        clientSimGetBrainLastAssistMsg(const ClientSim *cs);
uint32_t       clientSimGetLastServerTick(const ClientSim *cs);
unsigned short clientSimGetServerPort(const ClientSim *cs);
time_t         clientSimGetTimeStart(const ClientSim *cs);

/* String (char[]) accessors */
const char *clientSimGetMapName(const ClientSim *cs);
const char *clientSimGetLobbyChatHistory(const ClientSim *cs);
const char *clientSimGetMyLastPlayerName(const ClientSim *cs);

/* Indexed-array accessors (bounds-checked; out-of-range
 * returns NULL for pointer types, false/0 for scalars). */
const ClientLobbySlot *clientSimGetLobbySlot(const ClientSim *cs, BYTE n);
bool                   clientSimIsMapSkipVote(const ClientSim *cs, BYTE n);
uint8_t                clientSimGetBalanceProposal(const ClientSim *cs, BYTE n);

/* Array-pointer accessors (return pointer to backing storage).
 * brainMap is returned non-const because external callers
 * memset it and pass it to writers; the other arrays are
 * read-only externally so far. */
BYTE                 *clientSimGetBrainMap(ClientSim *cs);
const ShellSnapshot  *clientSimGetServerShellSnaps(const ClientSim *cs);
const PredictedShell *clientSimGetPredictedShells(const ClientSim *cs);
const GameEvent      *clientSimGetBrainEvents(const ClientSim *cs);

/* Struct-by-value accessor. */
struct in_addr clientSimGetServerAddress(const ClientSim *cs);

/* Cross-struct / interior-pointer accessors — return
 * non-const pointers because callers continue to mutate
 * these substructs through their existing APIs (same
 * exception class as serverSimGetGameSim). */
GameSim       *clientSimGetGameSim(ClientSim *cs);
MessageState  *clientSimGetMessages(ClientSim *cs);
ScrollState   *clientSimGetScroll(ClientSim *cs);
InterpContext *clientSimGetInterpCtx(ClientSim *cs);
screen        *clientSimGetView(ClientSim *cs);
screenMines   *clientSimGetMineView(ClientSim *cs);

/*********************************************************
 * Mutators.
 *
 * Add a mutator here whenever a caller outside the
 * marker-including files needs to write a ClientSim field
 * that doesn't yet have a setter. Keep one mutator per
 * field; coarse "do everything at once" calls don't belong
 * here.
 *********************************************************/

void clientSimSetBalanceProposalActive(ClientSim *cs, bool active);
void clientSimClearBalanceProposal(ClientSim *cs);
void clientSimSetMapSkipMyVote(ClientSim *cs, bool vote);

#endif /* CLIENT_SIM_H */
