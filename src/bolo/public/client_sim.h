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
#include "global.h"   /* For PlayerBitMap (via platform_types.h), BYTE, etc. */
#include "input_packet.h"
#include "client_enums.h" /* For aiType, buildSelect, gameType, labelLen, netType, netStatus */
#include "viewport_types.h" /* For screen, screenMines, screenGunsight */
#include "wire_limits.h"  /* For PACKET_MAX_PLAYER_NAME */
#include "alliance_enums.h" /* For pillAlliance, baseAlliance */
#include "screentank.h"     /* For tankAlliance */
#include "brain.h"  /* For BuildInfo, ObjectInfo */
#include "brain_list.h"   /* BrainList — value type used by clientSimGetLobbyBrainList */
#include "upload_policy.h" /* UploadPolicy — clientSimGetUploadPolicy return */

#ifndef GAMESIM_TYPEDEF
#define GAMESIM_TYPEDEF
typedef struct GameSim GameSim;
#endif

/* Forward declarations for state types whose full layout lives in
 * src/bolo/internal/. External callers reach them only through the
 * pointer-returning accessors below. */
#ifndef INTERPCONTEXT_TYPEDEF
#define INTERPCONTEXT_TYPEDEF
typedef struct InterpContext InterpContext;
#endif
#ifndef MESSAGESTATE_TYPEDEF
#define MESSAGESTATE_TYPEDEF
typedef struct MessageState  MessageState;
#endif
#ifndef SCROLLSTATE_TYPEDEF
#define SCROLLSTATE_TYPEDEF
typedef struct ScrollState   ScrollState;
#endif

/* Messages status on/off */
#define MSG_NEWSWIRE 0
#define MSG_ASSISTANT 1
#define MSG_AI 2
#define MSG_NETWORK 3
#define MSG_NETSTATUS 4

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

/* Forward decl — full definition in bolo/control_event.h. Kept opaque
 * here so client_sim.h doesn't pull control_event.h's include closure
 * into every TU. */
struct ControlEvent;

/* Read-only observer for ControlEvents arriving at this ClientSim.
 * Invoked from clientSimApplyControl before any state mutation, so
 * the callback sees every event the dispatcher receives (including
 * self-skip cases). The event is const and the callback returns
 * void — observers cannot influence dispatch. Single slot per
 * ClientSim; intended for the WinBoloHeadless --log-events test
 * harness, not for production code. */
typedef void (*ControlObserverCb)(void *ctx, const struct ControlEvent *evt);

/* Maximum predicted shells the client can track at once */
#define MAX_PREDICTED_SHELLS 8

/* Brain event ring buffer size — used to size cached event arrays
 * inside ClientSim and external mirror arrays (e.g. winbolo_gym
 * cachedEvents). */
#define MAX_BRAIN_EVENTS 512

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

/* Forward declaration — full definition in client_sim_internal.h */
struct ViewPort;

/* MY_TANK, MY_LGM, and clientSimFromSim moved to
 * src/bolo/internal/client_sim_internal.h — they need GameSim's
 * full layout, which the public header does not expose. */

/*********************************************************
 *NAME:          clientSimAlloc
 *PURPOSE:
 *  Allocates a ClientSim on the heap, zero-initialised.
 *  Caller must follow with clientSimCreate() or
 *  clientLoadCompressedMap() to populate before use.
 *  Pairs with clientSimDestroy, which frees the pointer.
 *
 *  After ClientSim opacity, external callers cannot
 *  declare a ClientSim by value (incomplete type) or
 *  embed it as a struct field. Use this for the
 *  allocation and release with clientSimDestroy.
 *********************************************************/
ClientSim *clientSimAlloc(void);

/* Lifecycle API — initializes/destroys the ClientSim struct */
bool clientSimCreate(ClientSim *cs, gameType game, bool hiddenMines, int srtDelay, int32_t gmeLen);
void clientSimDestroy(ClientSim *cs);
void clientSimSetPlayerNum(ClientSim *cs, BYTE playerNum);

/* Reset all map-dependent state in cs back to a freshly-created
 * empty configuration, WITHOUT freeing cs or tearing down its
 * transport binding (clientSimConnectUdp/Local state survives).
 *
 * Used by the UDP-join flow: after the join handshake completes,
 * the empty ClientSim is reset in-place before clientLoadCompressedMap
 * reads the server's map blob (which lives inside the transport).
 * Previously this was done by clientSimDestroy + clientSimAlloc,
 * which dropped the transport and broke the back-pointer chain. */
void clientSimResetForMapLoad(ClientSim *cs);

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
aiType clientSimGetAiType(ClientSim *cs);
void clientSimSetAiType(ClientSim *cs, aiType value);

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

/* Install (or clear, with cb=NULL) a read-only control-event observer.
 * Survives clientSimResetForMapLoad / in-place clientSimCreate rebuilds
 * for the same reason the transport binding does: the observer is owned
 * by an external party (the test harness) whose lifetime is independent
 * of the ClientSim's map-reload cycle. */
void clientSimSetControlObserver(ClientSim *cs, ControlObserverCb cb, void *ctx);

/* Lobby chat helper — appends "name: message\n" to lobbyChatHistory */
void clientSimAppendLobbyChat(ClientSim *cs, const char *name, const char *message);

/* Player-to-player chat delivery: routes to lobby chat or in-game inbox
   depending on whether the client is still in the lobby. */
void clientSimIncomingMessage(ClientSim *cs, BYTE playerNum, char *messageStr);

/* Message functions (per-instance) */
void clientSimMessageSendAllPlayers(ClientSim *cs, BYTE playerNum, char *message);
void clientSimMessageSendPlayer(ClientSim *cs, BYTE playerNum, BYTE destPlayer, char *message);
void clientSimSendChangePlayerName(ClientSim *cs, BYTE playerNum, char *newName);
void clientSimGetPlayerName(ClientSim *cs, char *value);
bool clientSimSetPlayerName(ClientSim *cs, char *value);

/* High-level send-message wrappers used by the players-panel UI. */
void clientSimSendMessageAllPlayers(ClientSim *cs, char *messageStr);
void clientSimSendMessageAllAllies(ClientSim *cs, char *messageStr);
void clientSimSendMessageAllNearby(ClientSim *cs, char *messageStr);
void clientSimSendMessageAllSelected(ClientSim *cs, char *messageStr);

void clientSimRequestAlliance(ClientSim *cs, BYTE playerNum, BYTE requestTo);
void clientSimAllianceAccept(ClientSim *cs, BYTE playerNum);
void clientSimLeaveAlliance(ClientSim *cs, BYTE playerNum);

/* Local-player alliance actions (disambiguate from the targeted variants above). */
void clientSimLeaveAllianceSelf(ClientSim *cs);
void clientSimRequestAllianceSelected(ClientSim *cs);

void clientSimSetAllowNewPlayers(ClientSim *cs, bool allow);

/* Players-panel selection helpers — operate on local player check-state. */
void clientSimTogglePlayerCheckState(ClientSim *cs, BYTE playerNum);
void clientSimCheckAllNonePlayers(ClientSim *cs, bool isChecked);
void clientSimCheckAlliedPlayers(ClientSim *cs);
void clientSimCheckNearbyPlayers(ClientSim *cs);
int  clientSimGetNumCheckedPlayers(ClientSim *cs);
int  clientSimGetNumAllies(ClientSim *cs);
int  clientSimGetNumNearbyTanks(ClientSim *cs);

/* Tank preferences (per-instance) — operate on the local player's tank */
bool clientSimGetTankAutoSlowdown(ClientSim *cs);
void clientSimSetTankAutoSlowdown(ClientSim *cs, bool useSlowdown);
bool clientSimGetTankAutoHideGunsight(ClientSim *cs);
void clientSimSetTankAutoHideGunsight(ClientSim *cs, bool useAutohide);
void clientSimSetGunsight(ClientSim *cs, bool shown);
BYTE clientSimGetTank256Dir(ClientSim *cs);

/* Death-state queries for the local-player tank. Used by frontends to
 * decide whether to render death-screen effects without holding a tank
 * pointer themselves. */
int  clientSimGetMyTankDeathWait(ClientSim *cs);
int  clientSimGetMyTankLastDeath(ClientSim *cs);

/* Game info (per-instance) */
bool clientSimGetAllowHiddenMines(ClientSim *cs);
BYTE clientSimGetNumPlayers(ClientSim *cs);

/* Tutorial-mode flag mirrored on the embedded GameSim. */
bool clientSimIsTutorial(const ClientSim *cs);
void clientSimSetTutorial(ClientSim *cs, bool v);

/* Game type (open/tournament/strict) for the running sim — distinct from
 * clientSimGetLobbyGameType, which reflects the pre-load lobby selection. */
gameType clientSimGetGameType(const ClientSim *cs);

/* Per-player accessors that forward to the embedded players struct. */
uint16_t clientSimGetPlayerPing(ClientSim *cs, BYTE playerNum);
uint8_t  clientSimGetPlayerClientFlags(ClientSim *cs, BYTE playerNum);
uint8_t  clientSimGetPlayerClientType(ClientSim *cs, BYTE playerNum);
void     clientSimGetPlayerLocation(ClientSim *cs, BYTE playerNum, char *dest);
uint8_t  clientSimGetPlayerAccountFlags(ClientSim *cs, BYTE playerNum);
void     clientSimGetPlayerCountryCode(ClientSim *cs, BYTE playerNum, char *dest);
bool     clientSimIsPlayerAlly(ClientSim *cs, BYTE playerA, BYTE playerB);

void netGetStats(ClientSim *cs, char *status, int *ping, int *ppsec, int *retrans);
void netGetServerAddressStr(ClientSim *cs, char *dest);
void netGetOurAddressStr(ClientSim *cs, char *dest);
BYTE netGetDownloadPos(void);
void netSecond(void);
int netGetNetTime(void);
bool netSetup(ClientSim *cs, netType value, unsigned short myPort, char *targetIp, unsigned short targetPort, char *password, bool usCreate, char *trackerAddr, unsigned short trackerPort, bool useTracker, bool wantRejoin, bool useWinboloNet, const char *wbnApiToken, const char *wbnServerKey);
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
/* SDL_GetTicks() at the most recent non-empty CTRL_BALANCE_PROPOSAL
 * arrival — used by the lobby's "Teams balanced" status label so the
 * success state outlives the auto-apply clear publish that follows
 * the proposal arrival on the same mutex hold. Returns 0 if no
 * proposal has arrived this session. */
uint64_t     clientSimGetLastBalanceProposalArrivedMs(const ClientSim *cs);
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

/* Count of currently-connected lobby slots (humans + bots).
 * Matches what the lobby UI's player table renders. */
BYTE clientSimGetLobbyNumConnected(const ClientSim *cs);
bool                   clientSimIsMapSkipVote(const ClientSim *cs, BYTE n);
uint8_t                clientSimGetBalanceProposal(const ClientSim *cs, BYTE n);

/* In-game vote system — public mirror of wire constants from
 * netpacks.h. Keep these in lockstep with PACKET_GAME_VOTE_* in
 * netpacks.h; numeric values are wire-stable. */
#ifndef GAME_VOTE_KIND_BACK_TO_LOBBY
#define GAME_VOTE_KIND_BACK_TO_LOBBY  1
#define GAME_VOTE_KIND_SURRENDER      2
#define GAME_VOTE_TOGGLE_NO           0
#define GAME_VOTE_TOGGLE_YES          1
#define GAME_VOTE_TOGGLE_OPEN_ONLY    2
#define GAME_VOTE_ACTIVE_NONE         0
#define GAME_VOTE_ACTIVE_RUNNING      1
#define GAME_VOTE_ACTIVE_PASSED       2
#define GAME_VOTE_ACTIVE_FAILED       3
#define GAME_VOTE_ACTIVE_CANCELLED    4
#define GAME_VOTE_TRIGGER_MANUAL          0
#define GAME_VOTE_TRIGGER_BASE_MONOPOLY   1
#define GAME_VOTE_TRIGGER_POST_SURRENDER  2
#endif

/* In-game vote mirror accessors. kind = GAME_VOTE_KIND_*. The
 * snapshot mirrors the wire payload; widgetVisible is local-only. */
typedef struct {
    uint8_t  kind;
    uint8_t  active;
    uint8_t  triggerSrc;
    uint8_t  teamId;
    uint8_t  threshold;
    uint8_t  yesCount;
    uint8_t  noCount;
    uint8_t  eligibleCount;
    uint8_t  secondsRemaining;
    uint16_t votes;
    bool     widgetVisible;
    uint32_t concludedAtMs;   /* SDL_GetTicks() at conclusion, 0 = still running */
} ClientGameVoteSnapshot;

bool clientSimGetGameVote(const ClientSim *cs, uint8_t kind,
                          ClientGameVoteSnapshot *out);
void clientSimSetGameVoteWidgetVisible(ClientSim *cs, uint8_t kind, bool visible);

/* Per-frame tick that emits "Returning to lobby in N" newswire lines
 * for a vote-driven back-to-lobby transition. Server just enters
 * gameOver and lets countdownTicks drain; the client times its own
 * 3/2/1 announcement off the local clock so we don't pay for a
 * per-second chat broadcast. Call this from the main render loop. */
void clientSimTickLobbyReturnCountdown(ClientSim *cs);
/* Returns true if local player has voted yes on this kind. */
bool clientSimGameVoteMyVote(const ClientSim *cs, uint8_t kind);

/* Seconds remaining until the server forces a return to lobby (e.g.
 * triggered by a vote pass). 0 = no pending forced transition.
 * Sourced from the snapshot header's returnToLobbyTicks. */
uint8_t clientSimGetReturnToLobbySecs(const ClientSim *cs);

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

/* Bundled viewport accessor — for bolo-internal callers (viewport.c,
 * screen.c, etc.) that want to operate on the whole ViewPort substruct
 * rather than poking individual scalar fields. The scalar accessors
 * above still exist and are the preferred entry point for external
 * callers. */
const struct ViewPort *clientSimViewport(const ClientSim *cs);
struct ViewPort       *clientSimViewportMut(ClientSim *cs);

/* Writable scalar slots — for callees that write through an
 * address (scroll*, pillsGetNextView, pillsMoveView). Use the
 * by-value clientSimGet<X> accessors for plain reads; these are
 * only for the pass-by-pointer case. */
BYTE *clientSimGetXOffsetPtr(ClientSim *cs);
BYTE *clientSimGetYOffsetPtr(ClientSim *cs);
BYTE *clientSimGetPillViewXPtr(ClientSim *cs);
BYTE *clientSimGetPillViewYPtr(ClientSim *cs);

/* Writable buffer access — pair with const-returning
 * clientSimGetMapName. Used by sites that strcpy/strncpy
 * into mapName or pass &mapName[0] to a buffer-filling
 * function (utilExtractMapName). MAP_STR_SIZE is the buffer
 * capacity; caller bounds-checks. */
char *clientSimGetMapNameMutable(ClientSim *cs);

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

/* Viewport / cursor / view state */
void clientSimSetXOffset(ClientSim *cs, BYTE v);
void clientSimSetYOffset(ClientSim *cs, BYTE v);
void clientSimSetCursorPosX(ClientSim *cs, int v);
void clientSimSetCursorPosY(ClientSim *cs, int v);
void clientSimSetNeedScreenReCalc(ClientSim *cs, bool v);
void clientSimSetInPillView(ClientSim *cs, bool v);
void clientSimSetPillViewX(ClientSim *cs, BYTE v);
void clientSimSetPillViewY(ClientSim *cs, BYTE v);
void clientSimSetView(ClientSim *cs, screen v);
void clientSimSetMineView(ClientSim *cs, screenMines v);

/* Game / round state */
void clientSimSetGmeStartDelay(ClientSim *cs, int v);
void clientSimSetGmeLength(ClientSim *cs, int32_t v);
void clientSimSetTimeStart(ClientSim *cs, time_t v);
void clientSimSetRunning(ClientSim *cs, bool v);
void clientSimSetCurrentBuildSelect(ClientSim *cs, buildSelect v);
void clientSimSetLocalTransport(ClientSim *cs, bool isLocal);

/* Pending build input (set as a triple) */
void clientSimSetPendingBuild(ClientSim *cs, BYTE action, BYTE x, BYTE y);

/* Brain state */
void clientSimSetBrainLastAssistMsg(ClientSim *cs, uint8_t v);
void clientSimSetBrainEventCount(ClientSim *cs, int v);

/* Label state */
void clientSimSetLabelMessage(ClientSim *cs, labelLen v);
void clientSimSetLabelTankLabel(ClientSim *cs, labelLen v);
void clientSimSetLabelOwnTank(ClientSim *cs, bool v);

/* Last-player-name buffer (copy semantics — strcpy into the field,
 * matching the existing call site behavior; caller bounds-checks). */
void clientSimSetMyLastPlayerName(ClientSim *cs, const char *name);

/* Lobby state */
void clientSimSetInLobby(ClientSim *cs, bool v);
void clientSimSetMapDownloadComplete(ClientSim *cs, bool v);

/* Server endpoint info — set by the frontend after the UDP transport
 * resolves the server address; consumed by brain info. */
void clientSimSetServerAddress(ClientSim *cs, struct in_addr v);
void clientSimSetServerPort(ClientSim *cs, unsigned short v);

/* Bot flag — set by bot_manager.c when creating a bot's ClientSim.
 * Distinguishes bot sims from human sims (bot sims must not trigger
 * frontend UI calls). */
void clientSimSetIsBot(ClientSim *cs, bool v);

/* Session-type flags — set by gamefront when starting a single-player
 * or LAN-only host session. The lobby UI uses these to hide
 * multiplayer-only controls and strip WBN-verified badges. */
bool clientSimIsSinglePlayer(const ClientSim *cs);

/* True when the active transport is the UDP client (vs the local /
 * in-process transport used by SP-host and MP-host's own client).
 * UI code that needs to differentiate "expects a MAP_CHANGE packet"
 * vs "the server is in-process and there is no packet" should gate
 * on this. */
bool clientSimIsUdpTransport(const ClientSim *cs);
bool clientSimIsLanOnly(const ClientSim *cs);
void clientSimSetIsSinglePlayer(ClientSim *cs, bool v);
void clientSimSetIsLanOnly(ClientSim *cs, bool v);

/* ────────────────────────────────────────────────────────────────
 * Layout A lobby accessors (client-side mirror of server state).
 * Indexed accessors return 0 / "" / NULL for out-of-range indices.
 * ──────────────────────────────────────────────────────────────── */
bool        clientSimGetLobbyOpenHost(const ClientSim *cs);
bool        clientSimGetLobbyAutoLockOnGameStart(const ClientSim *cs);
/* Ranked-game flag (LST_RANKED). When true, the server forbids bots
 * and the "Open" game type, and removes any existing bots. UI is
 * visible to every player (so they can see the ranked badge) but
 * only host/admin may toggle. */
bool        clientSimGetLobbyRanked(const ClientSim *cs);
/* Server's authoritative allowNewPlayers flag, mirrored on every client
 * via CTRL_LOBBY_SETTINGS. The lobby's "Allow New Players: [ ] Now"
 * checkbox reads this so it reflects real server state rather than the
 * host's last-clicked intent. */
bool        clientSimGetLobbyAllowNewPlayers(const ClientSim *cs);

/* True iff the host process has winbolo.net signed in / running. Used
 * by the lobby UI to hide WBN-only affordances (Balance from WBN) on
 * remote clients when the host isn't authenticated to WBN — without
 * this the button would be visible but every click would be dropped
 * by the server's wbnRunning guard. Mirrored via CTRL_LOBBY_SETTINGS. */
bool        clientSimGetLobbyWbnAvailable(const ClientSim *cs);
uint16_t    clientSimGetLobbyServerLocks(const ClientSim *cs);

/* Server map-upload policy as last broadcast in the lobby-settings event.
 * Defaults to UPLOAD_POLICY_ALLOW until the first event arrives. */
UploadPolicy clientSimGetUploadPolicy(const ClientSim *cs);

uint8_t     clientSimGetLobbyTeamInUse(const ClientSim *cs, BYTE teamId);
uint8_t     clientSimGetLobbyTeamColor(const ClientSim *cs, BYTE teamId);
uint8_t     clientSimGetLobbyTeamPool(const ClientSim *cs, BYTE teamId);
const char *clientSimGetLobbyTeamName(const ClientSim *cs, BYTE teamId);

uint8_t     clientSimGetLobbyBotDifficulty(const ClientSim *cs, BYTE slot);
uint8_t     clientSimGetLobbyBotPersonality(const ClientSim *cs, BYTE slot);
/* Returns the catalogue index of the brain assigned to a lobby bot slot.
 * 0xFF means the bot uses the server's default brain; for any other
 * value the caller can look up clientSimGetLobbyBrainList(cs)->entries[idx]
 * to recover the display name and wire path. Out-of-range slot returns
 * 0xFF. */
uint8_t     clientSimGetLobbyBotBrain(const ClientSim *cs, BYTE slot);

const BrainList *clientSimGetLobbyBrainList(const ClientSim *cs);

/* Server-supplied map directory listing — populated asynchronously
 * by PACKET_LOBBY_MAP_LIST_RSP after the client sends a
 * MAP_LIST_REQ. Use the InFlight/Ready/Path triple to coordinate
 * "we asked, waiting" vs "stale response for a different path"
 * vs "have a fresh listing". */
const char *clientSimGetLobbyMapListPath(const ClientSim *cs);
int         clientSimGetLobbyMapListCount(const ClientSim *cs);
const char *clientSimGetLobbyMapListName(const ClientSim *cs, int idx);
bool        clientSimGetLobbyMapListIsFolder(const ClientSim *cs, int idx);
int64_t     clientSimGetLobbyMapListModTime(const ClientSim *cs, int idx);
bool        clientSimGetLobbyMapListReady(const ClientSim *cs);
const char *clientSimGetLobbyMapListReqPath(const ClientSim *cs);
bool        clientSimGetLobbyMapListInFlight(const ClientSim *cs);

/* Monotonic counter, ticked on every PACKET_LOBBY_MAP_CHANGE the
 * client receives. UI code can cache the last-seen value to detect
 * map changes even when the download cycle completes inside a single
 * render frame (the loopback-host case, where the `complete = false;
 * ...; true` transition is too short for a frame-edge detector). */
uint32_t    clientSimGetLobbyMapChangeSeq(const ClientSim *cs);

/* Recursive search cache — populated by PACKET_LOBBY_MAP_SEARCH_RSP.
 * Same shape as the list cache but names are relative paths from
 * lobbyMapSearchPath. ReqPath/ReqQuery hold the most recent request
 * so the chooser can tell whether the cached response matches the
 * currently-active path/query pair. */
const char *clientSimGetLobbyMapSearchPath(const ClientSim *cs);
const char *clientSimGetLobbyMapSearchQuery(const ClientSim *cs);
int         clientSimGetLobbyMapSearchCount(const ClientSim *cs);
const char *clientSimGetLobbyMapSearchName(const ClientSim *cs, int idx);
bool        clientSimGetLobbyMapSearchIsFolder(const ClientSim *cs, int idx);
int64_t     clientSimGetLobbyMapSearchModTime(const ClientSim *cs, int idx);
bool        clientSimGetLobbyMapSearchReady(const ClientSim *cs);
const char *clientSimGetLobbyMapSearchReqPath(const ClientSim *cs);
const char *clientSimGetLobbyMapSearchReqQuery(const ClientSim *cs);
bool        clientSimGetLobbyMapSearchInFlight(const ClientSim *cs);

/* Map upload progress reflection. status: 0=idle, 1=announce sent,
 * 2=ack received (chunks in flight), 3=done, 4=rejected. */
uint8_t     clientSimGetLobbyMapUploadStatus(const ClientSim *cs);
uint8_t     clientSimGetLobbyMapUploadRejectCode(const ClientSim *cs);
const char *clientSimGetLobbyMapUploadFinalPath(const ClientSim *cs);
void        clientSimResetLobbyMapUpload(ClientSim *cs);
/* True (and clears the flag) if the server NACK'd a USE_LOCAL request
 * since the last call — the caller should fall back to the regular
 * UPLOAD_BEGIN/CHUNK flow. */
bool        clientSimConsumeUseLocalFallback(ClientSim *cs);

/* Winbolo.net preview reflection. status: 0=idle, 1=in-flight,
 * 2=ok, 3=error. errMsg is server-supplied when status == 3. */
uint8_t     clientSimGetLobbyWbnPreviewStatus(const ClientSim *cs);
const char *clientSimGetLobbyWbnPreviewErrMsg(const ClientSim *cs);
void        clientSimSetLobbyWbnPreviewStatus(ClientSim *cs, uint8_t status);
void        clientSimSetLobbyWbnPreviewErrMsg(ClientSim *cs, const char *msg);

/* Last reject from server (toast pair). Both 0 = no pending message.
 * clientSimClearLobbyLastReject clears both fields atomically after
 * the toast has been rendered. */
uint8_t clientSimGetLobbyLastRejectPacket(const ClientSim *cs);
uint8_t clientSimGetLobbyLastRejectReason(const ClientSim *cs);
void    clientSimClearLobbyLastReject(ClientSim *cs);

/* View control — mutates viewport/cursor state by composing the
 * underlying viewport ops with ClientSim's tank/scroll/gameSim. */
void         clientSimTankView(ClientSim *cs);
void         clientSimSetCursorPos(ClientSim *cs, BYTE posX, BYTE posY);
bool         clientSimGetCursorPos(ClientSim *cs, BYTE *posX, BYTE *posY);
void         clientSimPillView(ClientSim *cs, int horz, int vert);
void         clientSimRecalc(ClientSim *cs);
void         clientSimUpdateView(ClientSim *cs, updateType value);
void         clientSimPanX(ClientSim *cs, int dxTiles);
void         clientSimPanY(ClientSim *cs, int dyTiles);
bool         clientSimTankIsDead(ClientSim *cs);
bool         clientSimTankScroll(ClientSim *cs);
void         clientSimCenterTank(ClientSim *cs);
void         clientSimSetAutoScroll(ClientSim *cs, bool isAuto);
void         clientSimShowMessages(ClientSim *cs, BYTE msgType, bool isShown);
void         clientSimNetStatusMessage(ClientSim *cs, char *messageStr);

/* Submits a build request for the local LGM through InputPacket,
 * gated on tank armour and net status. */
void         clientSimManMove(ClientSim *cs, buildSelect buildS);

/* Alliance accessors. playerNum is 1-based (legacy screen-facade
 * convention); the function converts to 0-based internally. */
tankAlliance clientSimGetTankAlliance(ClientSim *cs, BYTE playerNum);
pillAlliance clientSimGetPillAlliance(ClientSim *cs, BYTE pillNum);
baseAlliance clientSimGetBaseAlliance(ClientSim *cs, BYTE baseNum);

/* Map-position lookups for the frontend label/click overlays.
 * mx/my are screen-relative; the accessors fold in viewport offsets. */
BYTE         clientSimGetPillNumPos(ClientSim *cs, BYTE mx, BYTE my);
BYTE         clientSimGetBaseNumPos(ClientSim *cs, BYTE mx, BYTE my);

/* --- Live-sim map / pill / base / start readers ---
 * Wrap direct access to the running sim's map / pillboxes / bases /
 * starts so consumers don't need bolo_map.h / pillbox.h / bases.h /
 * starts.h. Distinct from clientSimGetLobbyPillCount and friends
 * (those are pre-load lobby snapshots from the server's map-info
 * broadcast); these report counts and per-position state from the
 * live ClientSim. */
BYTE         clientSimGetMapTerrain(const ClientSim *cs, BYTE x, BYTE y);
bool         clientSimMapIsMine(const ClientSim *cs, BYTE x, BYTE y);
bool         clientSimPillExistsAt(const ClientSim *cs, BYTE x, BYTE y);
BYTE         clientSimPillGetScreenHealthAt(ClientSim *cs, BYTE x, BYTE y);
bool         clientSimBaseExistsAt(const ClientSim *cs, BYTE x, BYTE y);
baseAlliance clientSimBaseGetAllianceAt(ClientSim *cs, BYTE x, BYTE y);
bool         clientSimBaseAmOwnerAt(ClientSim *cs, BYTE player, BYTE x, BYTE y);

BYTE         clientSimGetPillCount(const ClientSim *cs);
BYTE         clientSimGetBaseCount(const ClientSim *cs);
BYTE         clientSimGetStartCount(const ClientSim *cs);

bool         clientSimGetPill(ClientSim *cs, BYTE i,
                              BYTE *x, BYTE *y, BYTE *owner, BYTE *armour,
                              bool *inTank);
bool         clientSimGetBase(ClientSim *cs, BYTE i,
                              BYTE *x, BYTE *y, BYTE *owner);
bool         clientSimGetBaseStats(ClientSim *cs, BYTE i,
                                   BYTE *shells, BYTE *mines, BYTE *armour);
bool         clientSimGetStart(ClientSim *cs, BYTE i,
                               BYTE *x, BYTE *y, BYTE *dir);

/* Local tank stat accessors. */
void         clientSimGetTankStats(ClientSim *cs, BYTE *shellsAmount, BYTE *minesAmount, BYTE *armourAmount, BYTE *treesAmount);
void         clientSimGetKillsDeaths(ClientSim *cs, int *kills, int *deaths);

/* Notifies the local LGM and player table that the server connection
 * has been lost; called by the frontend when a UDP shutdown is seen. */
void         clientSimConnectionLost(ClientSim *cs);

#endif /* CLIENT_SIM_H */
