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
#include "overview_types.h" /* For OverviewMap — clientSimGetOverviewMap return */
#include "wire_limits.h"  /* For PACKET_MAX_PLAYER_NAME */
#include "alliance_enums.h" /* For pillAlliance, baseAlliance */
#include "screentank.h"     /* For tankAlliance */
#include "screenlgm.h"      /* For screenLgm  — clientSimPrepareOverviewEntities */
#include "screenbullet.h"   /* For screenBullets — clientSimPrepareOverviewEntities */
#include "brain.h"  /* For BuildInfo, ObjectInfo */
#include "brain_list.h"   /* BrainList — value type used by clientSimGetLobbyBrainList */
#include "round_stats.h"  /* RoundStatsSummary — clientSimGetLastRoundStats return */
#include "upload_policy.h" /* UploadPolicy — clientSimGetUploadPolicy return */
#include "view_policy.h"   /* ViewPolicy / ViewCategory — clientSimGetViewPolicy */
#include "ping_display.h" /* PingBand — clientSimGetPlayerPingBand return */
#include "server_voice_mode.h" /* ServerVoiceMode — clientSimGetServerVoiceMode return */

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
    uint8_t startIdx;      /* reserved map start, 1-based; 0xFF = none */
    bool fielded;          /* On the field this round. False for a seat held
                              in the roster with no tank behind it — the
                              roster draws those dimmed. */
} ClientLobbySlot;

/* Client-side mirror of a server spectator roster slot. Spectators
 * have no team/ready/bot/ping/start, so this is a trimmed slot. */
typedef struct {
    bool    connected;
    char    playerName[PACKET_MAX_PLAYER_NAME];
    char    countryCode[3];   /* ISO 3166-1 alpha-2 + NUL; "" if unknown */
    uint8_t clientType;       /* ClientType enum */
    uint8_t clientFlags;      /* PLAYER_FLAG_* bits */
} ClientSpectatorSlot;

/* Live per-slot scoreboard counters, accumulated on the client from the
 * reliable game-event stream (clientSimApplyGameEvents). Field names
 * mirror RoundPlayerSummary so the end-of-round recap and the live board
 * read the same way; dmgDealt and builds are absent because no client
 * event carries them. */
typedef struct {
    uint16_t kills;
    uint16_t deaths;
    uint16_t baseCaptures;
    uint16_t pillCaptures;
    uint16_t lgmKills;
    uint16_t lgmDeaths;
} ClientPlayerStats;

/* Callback typedefs for new transport message sending.
 *
 * NetChatSendFunc receives the owning ClientSim so the callback body
 * can route the chat through the sim's own transport — making it safe
 * to reuse a single callback function for every ClientSim (host human,
 * bots) instead of one closure per sim. Pre-cs argument, the SDL3
 * frontend's callback hard-coded `humanSim` as the routing target,
 * which broke when wired onto a bot's ClientSim. */
typedef void (*NetChatSendFunc)(struct ClientSim *cs, uint8_t fromPlayer,
                                uint8_t destPlayer, const char *message);
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

/* Received smart pings this client is still drawing. Small: a ping lives
 * PING_DISPLAY_MS and the server rate-limits each sender, so a full team
 * cannot keep more than a handful alive at once. Oldest is overwritten when
 * it fills — a flood should push the stale ones off, not drop the new ones. */
#define MAX_CLIENT_PINGS 16

/* One received ping, in the shape the renderer wants: who sent it, which
 * kind, where in WORLD units, and the SDL_GetTicks() millisecond it landed
 * so the drawer can age it without knowing anything about sim ticks.
 *
 * The sender's name is copied in beside the slot rather than looked up when
 * the marker is drawn. Two reasons: the map overview renders off a snapshot
 * and has no sim to ask, and a slot can be left and taken by somebody else
 * inside the five seconds a ping lives — the name that goes on the marker
 * should be the one that sent it. Empty for a slot that was not in use. */
typedef struct {
    uint8_t  sender;
    uint8_t  kind;      /* PING_KIND_* */
    uint16_t worldX;
    uint16_t worldY;
    uint32_t recvMs;
    char     senderName[PACKET_MAX_PLAYER_NAME];
} ClientPing;

/* A client-side predicted shell, created instantly on fire input
 * and removed once the server has processed the fire tick. */
typedef struct {
    WORLD x;
    WORLD y;
    float fx;            /* float position accumulator — authoritative position */
    float fy;
    float vx;            /* float velocity per tick (shell_speed * cos/sin) */
    float vy;
    TURNTYPE angle;
    uint8_t length;
    uint8_t owner;
    bool onBoat;         /* Was the shell launched from a boat */
    uint32_t fireTick;   /* The input tick that created this shell */
    bool active;
} PredictedShell;

/* A render-only forward-projection of another player's shell. Anchored to
 * the latest snapshot and dead-reckoned forward by the snapshot's age so a
 * human sees incoming shells at their true present position rather than
 * ~RTT/2 in the past. Built for human clients only; the bot AI keeps reading
 * the raw serverShellSnaps. Shells fly deterministically until impact, so the
 * straight-line projection is exact between hits. */
typedef struct {
    float fx;            /* float position accumulator (world units) */
    float fy;
    float vx;            /* per-game-tick velocity (shell_speed * cos/sin) */
    float vy;
    uint8_t angle;       /* snapshot angle (bradians 0-255), for render frame */
    uint8_t owner;
    uint8_t length;      /* remaining life, decremented per advance */
    bool active;
} ProjectedShell;

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
 *  Caller must follow with clientSimCreate() to populate
 *  before use. Pairs with clientSimDestroy, which frees
 *  the pointer.
 *
 *  After ClientSim opacity, external callers cannot
 *  declare a ClientSim by value (incomplete type) or
 *  embed it as a struct field. Use this for the
 *  allocation and release with clientSimDestroy.
 *********************************************************/
ClientSim *clientSimAlloc(void);

/* Lifecycle API — initializes/destroys the ClientSim struct.
 * Game settings (game type, hidden mines, start delay, length)
 * are not parameters: the UDP path installs them via JOIN_ACCEPT,
 * the local path inherits them from the bound ServerSim, and
 * direct callers use the clientSimSet{GameType,HiddenMines,
 * GmeStartDelay,GmeLength} setters when needed. */
bool clientSimCreate(ClientSim *cs);
void clientSimDestroy(ClientSim *cs);
/* Resets the transient game world (tanks, LGMs, per-tick sim systems)
 * to a clean slate on entering the lobby, leaving the map, player
 * identity, connection, and lobby mirror intact. Mirrors
 * serverSimResetGameWorld so the last round doesn't linger behind the
 * lobby. Driven by CTRL_GAME_PHASE_LOBBY so it is identical on SP,
 * host, and remote clients. */
void clientSimResetWorld(ClientSim *cs);
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

/*********************************************************
 *NAME:          clientSimOnAssignedSlot
 *PURPOSE:
 *  Single entry point for "we just learned our slot from
 *  the server."  Both the SP local-transport connect path
 *  (clientSimConnectLocalBody) and the UDP JOIN_ACCEPT
 *  handler MUST call this and only this — internally it
 *  runs the setPlayerNum + setupSelf + applyLocalTankPrefs
 *  sequence that both transports need.
 *
 *  Centralising removes the asymmetric-runtime bug class
 *  where the UDP path forgot to call clientSimSetupSelf
 *  and ran with MY_TANK(cs) == NULL for the lifetime of
 *  the session, crashing every per-frame code path the
 *  moment inLobby flipped to false.
 *
 *ARGUMENTS:
 *  cs          - Freshly-allocated ClientSim
 *  playerNum   - Slot the server assigned us
 *  playerName  - Display name (must outlive the call)
 *  clientType  - CLIENT_TYPE_* from JOIN context
 *  clientFlags - PLAYER_FLAG_* bitmask
 *********************************************************/
void clientSimOnAssignedSlot(ClientSim *cs, BYTE playerNum,
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
/* Buffer reliable game events for brain consumption and apply their
 * effect/sim side effects. Carries no snapshot-frame state, so both the
 * snapshot apply path and the reliable-channel drain route game events
 * through it. `events` may be NULL. */
void clientSimApplyGameEvents(ClientSim *cs, const GameEvent *events,
                              int eventCount, BYTE playerNum);
void clientSimDisplayTick(ClientSim *cs, bool isBrain);

/* Advance predicted shells by one tick (move forward, decrement length) */
void clientSimAdvancePredictedShells(ClientSim *cs);

/* No-op — predicted shells expire naturally via length counter */
void clientSimReconcilePredictedShells(ClientSim *cs, uint32_t lastProcessedInput);

/* Forward-projection of other players' shells (render-only, human clients).
 * Projects each snapshot shell by its age so incoming shells are drawn at
 * their true present position instead of ~RTT/2 in the past, returning dodge
 * time at high ping. The projection is capped so a wild ping estimate cannot
 * fling a shell arbitrarily far ahead. */
#define PROJECTION_MAX_TICKS 10   /* ~200ms / 20ms game tick */

/* Convert a one-way-latency ping (ms) to a capped snapshot age in game ticks
 * (20ms each), the unit the projection velocity steps in. Pure helper. */
int  clientShellProjectAgeTicks(uint16_t pingMs);

/* Pure projection: derive the per-tick velocity from angle+shellSpeed and the
 * anchored float position snap + velocity*ageTicks. Takes the speed rather
 * than reading a constant, so it stays unit-testable without a ClientSim
 * while still following the sim's shell_speed. */
void clientShellProject(uint16_t snapX, uint16_t snapY, uint8_t angle,
                        int ageTicks, int shellSpeed,
                        float *outFx, float *outFy,
                        float *outVx, float *outVy);

/* (Re)build the projected-shell array from the current serverShellSnaps,
 * anchoring each to snap + velocity*age and matching against the previous
 * frame's shells by owner+nearest-position so the float accumulator carries
 * smoothly across snapshots. Call for human clients only. */
void clientSimRebuildProjectedShells(ClientSim *cs, uint16_t pingMs);

/* Store the min-over-window RTT (ms) the snapshot path uses to anchor
 * remote-shell forward-projection. The transport sets this each PONG; it is
 * the functional (not display) ping. */
void clientSimSetProjectionPing(ClientSim *cs, uint16_t pingMs);

/* Advance projected shells one game tick (move forward, cull on visual
 * collision, decrement length) between snapshots. */
void clientSimAdvanceProjectedShells(ClientSim *cs);

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

/* Submit a ClientCommand to the bound server. UDP transports enqueue
 * into the reliable carrier (retransmit until ACKed); local transports
 * apply directly under the threads mutex. cmdSeq is assigned internally
 * for UDP; ignored for local. */
struct ClientCommand;
void clientSimSubmitCommand(ClientSim *cs, const struct ClientCommand *cmd);

/* Callback setters (per-instance) */
void clientSimSetChatSendFunc(ClientSim *cs, NetChatSendFunc func);
void clientSimSetNameChangeSendFunc(ClientSim *cs, NetNameChangeSendFunc func);
void clientSimSetAllianceRequestFunc(ClientSim *cs, NetAllianceRequestFunc func);
void clientSimSetAllianceAcceptFunc(ClientSim *cs, NetAllianceAcceptFunc func);
void clientSimSetAllianceLeaveFunc(ClientSim *cs, NetAllianceLeaveFunc func);
void clientSimSetLockToggleSendFunc(ClientSim *cs, NetLockToggleSendFunc func);

/* Install (or clear, with cb=NULL) a read-only control-event observer.
 * Preserved across clientSimCreate's memset for the same reason the
 * transport binding is: the observer is owned by an external party
 * (the test harness) whose lifetime is independent of the ClientSim. */
void clientSimSetControlObserver(ClientSim *cs, ControlObserverCb cb, void *ctx);

/* Transport-specific observer slot. Installed by the UDP transport for
 * side-effects that live outside the bolo library (joinState transitions,
 * re-join trigger, WBN re-auth, ACH_LONELY_LOBBY). Kept distinct from
 * the test observer so a test harness doesn't displace it. */
void clientSimSetTransportControlObserver(ClientSim *cs, ControlObserverCb cb, void *ctx);

/* Pending alliance request from another player. Returns 0xFF when none.
 * Set by the CTRL_ALLIANCE_REQUEST subscriber arm; frontends poll once
 * per frame and pop the dialog when non-0xFF, then call clear. */
BYTE clientSimGetPendingAllianceRequest(const ClientSim *cs);
void clientSimClearPendingAllianceRequest(ClientSim *cs);

/* Lobby chat helper — appends "name: message\n" to lobbyChatHistory */
void clientSimAppendLobbyChat(ClientSim *cs, const char *name, const char *message);
/* Team lobby chat helper — appends "name: message\n" to lobbyTeamChatHistory */
void clientSimAppendLobbyTeamChat(ClientSim *cs, const char *name, const char *message);

/* Clear the lobby chat log (broadcast + team). Mirrors the game-start clear the
 * CTRL_GAME_PHASE_RUNNING handler does; used by the spectator transport at the
 * live->delayed cutover, which a spectator reaches instead of the running flip. */
void clientSimClearLobbyChatHistory(ClientSim *cs);

/* Player-to-player chat delivery: routes to lobby chat or in-game inbox
   depending on whether the client is still in the lobby. */
void clientSimIncomingMessage(ClientSim *cs, BYTE playerNum, char *messageStr);

/* Message functions (per-instance) */
void clientSimMessageSendAllPlayers(ClientSim *cs, BYTE playerNum, char *message);
void clientSimMessageSendPlayer(ClientSim *cs, BYTE playerNum, BYTE destPlayer, char *message);
void clientSimSendChangePlayerName(ClientSim *cs, BYTE playerNum, char *newName);
/* Reads the local player's name into value, which holds at most
   valueSize bytes including the NUL; longer names are truncated. */
void clientSimGetPlayerName(ClientSim *cs, char *value, size_t valueSize);
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

/* True from the tick the classic main view cuts to static through to the
 * respawn, which is where the map overview fades to black: the player watches
 * their own explosion first, and black is the last thing they see before the
 * tank is back. What killed them decides where that starts. False the moment
 * the tank is back. */
bool clientSimIsMyTankDeathBlackout(ClientSim *cs);

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
/* Colour band for that ping, tracked with hysteresis so a player parked on a
 * threshold doesn't strobe between colours. Player rows should use this
 * rather than re-deriving a band from the number. */
PingBand clientSimGetPlayerPingBand(ClientSim *cs, BYTE playerNum);
uint8_t  clientSimGetPlayerClientFlags(ClientSim *cs, BYTE playerNum);
uint8_t  clientSimGetPlayerClientType(ClientSim *cs, BYTE playerNum);
/* destSize is the size of dest in bytes, including the NUL; a longer
 * location is truncated rather than overrunning the caller. */
void     clientSimGetPlayerLocation(ClientSim *cs, BYTE playerNum, char *dest,
                                    size_t destSize);
uint8_t  clientSimGetPlayerAccountFlags(ClientSim *cs, BYTE playerNum);
void     clientSimGetPlayerCountryCode(ClientSim *cs, BYTE playerNum, char *dest);
bool     clientSimIsPlayerAlly(ClientSim *cs, BYTE playerA, BYTE playerB);

void netGetStats(ClientSim *cs, char *status, int *ping, int *ppsec, int *retrans);
void netGetServerAddressStr(ClientSim *cs, char *dest);
void netGetOurAddressStr(ClientSim *cs, char *dest);
void netSecond(void);
int netGetNetTime(void);
bool netSetup(ClientSim *cs, netType value, unsigned short myPort, char *targetIp, unsigned short targetPort, char *password, bool usCreate, char *trackerAddr, unsigned short trackerPort, bool useTracker, bool wantRejoin, bool useWinboloNet, const char *wbnApiToken, const char *wbnServerKey);
void netDestroy(ClientSim *cs);
void netSendTrackerUpdate(void);
void netProcessedDnsLookup(ClientSim *cs, char *ip, char *host);

/* Queues an asynchronous reverse-DNS lookup of the server address 'ip' on the
 * background DNS thread (for the lobby / net-info UI only — visual, never used
 * to connect). Debounced: safe to call every frame; a lookup is queued at most
 * once per distinct IP. A no-op when the DNS thread isn't running. */
void clientSimRequestServerHostname(ClientSim *cs, const char *ip);

/* Reverse-DNS of the server address, for the lobby / net-info UI only (visual,
 * never used to connect). Returns true and fills 'out' with the resolved
 * hostname when a PTR record is known for 'ip'; returns false (out untouched)
 * if 'ip' has no resolved name yet or resolved to no PTR record. Thread-safe:
 * guards the shared fields with the client mutex. */
bool clientSimGetServerHostname(ClientSim *cs, const char *ip, char *out, size_t outLen);

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
/* True in any of the item views (pill, base or ally) — i.e. whenever the
 * camera is parked on something instead of following the local tank. */
bool         clientSimIsInItemView(const ClientSim *cs);
/* What the camera is parked on. The kind is a ViewStateKind value
 * (VIEW_KIND_TANK / _PILL / _BASE / _ALLY in client_command.h); the target is
 * the pill or base index, or the ally's player number, and is 0 in tank
 * view. */
uint8_t      clientSimGetViewKind(const ClientSim *cs);
BYTE         clientSimGetViewTarget(const ClientSim *cs);
bool         clientSimIsNeedScreenReCalc(const ClientSim *cs);
bool         clientSimIsInLobby(const ClientSim *cs);
bool         clientSimIsMapDownloadComplete(const ClientSim *cs);

/* Monotonic count of installed maps this client has discarded because a
 * re-accept armed a fresh download (a mid-lobby map change, or a
 * return-to-lobby). The initial join does not count — nothing was installed
 * to discard. Never decreases.
 *
 * Sample it before triggering a map change and compare afterwards to
 * establish that the change really did invalidate the installed map. The
 * invalidation itself is a transient — the old map is gone only until the
 * new one lands — so an observer polling clientSimGetServerMapData for NULL
 * can step over the whole window and see nothing. */
uint32_t     clientSimGetMapInvalidateCount(const ClientSim *cs);
/* Map-download progress as 0..100. Returns 100 for the local transport
 * (no download needed) and 0 when no transport is bound. UDP path reads
 * mapDownloadReceived/Total from the transport. */
uint8_t      clientSimGetMapDownloadPercent(const ClientSim *cs);
bool         clientSimIsMapSkipAvailable(const ClientSim *cs);
bool         clientSimIsLobbyAvailable(const ClientSim *cs);
bool         clientSimIsMapSkipMyVote(const ClientSim *cs);
bool         clientSimIsLobbyHiddenMines(const ClientSim *cs);
/* Whether the server accepts smart pings. Positive on purpose: the value
 * is stored and sent in the negative sense (see lobbySmartPingsOff) so a
 * server that predates the setting reads as allowed, and this accessor is
 * where that flips back so no UI code has to think in negatives. Answers
 * true for a NULL sim and until the first lobby-settings event lands. */
bool         clientSimIsLobbyAllowSmartPings(const ClientSim *cs);
bool         clientSimIsBalanceProposalActive(const ClientSim *cs);
/* SDL_GetTicks() at the most recent non-empty CTRL_BALANCE_PROPOSAL
 * arrival — used by the lobby's "Teams balanced" status label so the
 * success state outlives the auto-apply clear publish that follows
 * the proposal arrival on the same mutex hold. Returns 0 if no
 * proposal has arrived this session. */
uint64_t     clientSimGetLastBalanceProposalArrivedMs(const ClientSim *cs);
/* SDL_GetTicks at the most recent CTRL_BALANCE_FAILED arrival (0 if
 * never), and the carried reason byte. Reason 0 means "no failure on
 * record"; non-zero values follow the wire enum in control_event.h
 * (1=http, 2=wbn error body, 3=internal). */
uint64_t     clientSimGetLastBalanceFailedMs(const ClientSim *cs);
uint8_t      clientSimGetLastBalanceFailedReason(const ClientSim *cs);
bool         clientSimIsLabelOwnTank(const ClientSim *cs);
buildSelect  clientSimGetCurrentBuildSelect(const ClientSim *cs);
gameType     clientSimGetLobbyGameType(const ClientSim *cs);
labelLen     clientSimGetLabelMessage(const ClientSim *cs);
labelLen     clientSimGetLabelTankLabel(const ClientSim *cs);

/* Scalar (integer) accessors */
BYTE           clientSimGetMyPlayerNum(const ClientSim *cs);
/* True when this ClientSim connected as a tankless spectator. Such a sim
 * holds no tank and never claims a player slot, so its myPlayerNum stays 0
 * (an alias of real slot 0); callers use this to tell a viewer apart from
 * the player who actually occupies slot 0. */
bool           clientSimIsSpectator(const ClientSim *cs);
BYTE           clientSimGetXOffset(const ClientSim *cs);
BYTE           clientSimGetYOffset(const ClientSim *cs);
/* Sub-tile view offset in 1/256-tile units (0..255). Renderer adds this
 * as a sub-pixel drag offset so the view glides across tile boundaries
 * instead of snapping each tile shift. */
int            clientSimGetSubPosX(const ClientSim *cs);
int            clientSimGetSubPosY(const ClientSim *cs);
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
int            clientSimGetProjectedShellCount(const ClientSim *cs);
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
const char *clientSimGetLobbyTeamChatHistory(const ClientSim *cs);
const char *clientSimGetMyLastPlayerName(const ClientSim *cs);

/* Indexed-array accessors (bounds-checked; out-of-range
 * returns NULL for pointer types, false/0 for scalars). */
const ClientLobbySlot *clientSimGetLobbySlot(const ClientSim *cs, BYTE n);

/* Live scoreboard counters for one player slot; out-of-range slot
 * returns NULL. Zeroed at the start of each game. */
const ClientPlayerStats *clientSimGetPlayerStats(const ClientSim *cs,
                                                 BYTE playerNum);

/* Spectator roster slot mirror; out-of-range idx returns NULL. */
const ClientSpectatorSlot *clientSimGetSpectatorSlot(const ClientSim *cs, uint8_t idx);

/* Count of currently-connected lobby slots (humans + bots).
 * Matches what the lobby UI's player table renders. */
BYTE clientSimGetLobbyNumConnected(const ClientSim *cs);
/*********************************************************
 *NAME:          clientSimGetVoiceTalkingMap
 *PURPOSE:
 *  Who the server says is producing voice right now, one
 *  bit per player slot.
 *
 *  Meaningful in the lobby and the countdown only. There
 *  voice is all-talk, so the set says nothing a listener
 *  could not already hear. In a running game voice follows
 *  the alliance and the server does not send the set at
 *  all, so this reads empty — by design, not because the
 *  events were missed. The server sends one empty set as
 *  the round starts, so nobody is left showing as talking.
 *
 *  The set is raw: it names everyone talking, including
 *  players this client has muted. That is the point of it —
 *  a muted player's voice never arrives, so this is the
 *  only thing that says they are speaking. Intersecting it
 *  with the local mute list is the caller's job.
 *
 *ARGUMENTS:
 *  cs - The ClientSim to read
 *********************************************************/
PlayerBitMap clientSimGetVoiceTalkingMap(const ClientSim *cs);

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

/* Whether the local player is part of a vote's electorate — i.e. whether
 * this vote should be visible to them at all. Surrender votes belong to
 * the surrendering team (teamId) and are private to it: non-members get
 * neither the widget nor the newswire lines, and the server declines to
 * send them the state in the first place (see udpClientDeliverControl).
 * Every other kind is open to all connected players. Mirrors the server's
 * eligibility rule in gameVoteEligibleMask(). teamId comes from the vote
 * snapshot; kind is GAME_VOTE_KIND_*. */
bool clientSimMayAnswerGameVote(const ClientSim *cs, uint8_t kind,
                                uint8_t teamId);

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
const ProjectedShell *clientSimGetProjectedShells(const ClientSim *cs);
const GameEvent      *clientSimGetBrainEvents(const ClientSim *cs);

/* Copy out the smart pings this client is still showing, newest last, and
 * drop the ones older than PING_DISPLAY_MS on the way. nowMs is the caller's
 * SDL_GetTicks() reading — the ping ring is written from the network thread
 * and read from the render thread, so the clock comes from the caller rather
 * than being sampled twice. Returns how many entries were written into `out`
 * (never more than maxOut, never more than MAX_CLIENT_PINGS). */
int clientSimGetPings(const ClientSim *cs, uint32_t nowMs,
                      ClientPing *out, int maxOut);

/* Reflect and persist (within the session) this client's per-player smart-ping
 * mute — the state the players-menu toggle shows. Setting it only records the
 * toggle; the caller sends CMD_PLAYER_PING_MUTE separately, and the server is
 * the authority that actually stops delivering the muted player's pings. */
void clientSimSetPingMuted(ClientSim *cs, uint8_t player, bool muted);
bool clientSimIsPingMuted(const ClientSim *cs, uint8_t player);

/* The overview's fog memory: the tile every square carried the last time the
 * player could see it, plus the regions they can see right now. Maintained
 * every display tick. NULL when cs is NULL. */
const OverviewMap    *clientSimGetOverviewMap(const ClientSim *cs);

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
/* The squares of the back buffer the player cannot see into. All false while
 * buildings do not block sight. */
screenHidden  *clientSimGetHiddenView(ClientSim *cs);

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
void clientSimSetGameType(ClientSim *cs, gameType v);
void clientSimSetHiddenMines(ClientSim *cs, bool v);
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

/* True when the bound transport advances the server itself — its tick()
 * runs serverSimTick, as the active local transport does
 * (clientSimConnectLocal). False for the passive local transport
 * (clientSimConnectLocalPassive — the host's timer thread ticks the
 * server) and for UDP (the remote server ticks itself). Frontend tick
 * loops use this to decide whether an extra keys-half clientSimNetTick
 * would double-step the sim (active local) or is a harmless snapshot
 * pull that halves apply latency (passive local / UDP). */
bool clientSimTransportTicksServer(const ClientSim *cs);
bool clientSimIsLanOnly(const ClientSim *cs);
void clientSimSetIsSinglePlayer(ClientSim *cs, bool v);
void clientSimSetIsLanOnly(ClientSim *cs, bool v);

/* ────────────────────────────────────────────────────────────────
 * Layout A lobby accessors (client-side mirror of server state).
 * Indexed accessors return 0 / "" / NULL for out-of-range indices.
 * ──────────────────────────────────────────────────────────────── */
bool        clientSimGetLobbyOpenHost(const ClientSim *cs);
BYTE        clientSimGetLobbyHostSlot(const ClientSim *cs);
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
uint32_t    clientSimGetLobbyServerLocks(const ClientSim *cs);

/* The scenario the lobby's map is running, mirrored via
 * CTRL_LOBBY_SETTINGS. The source is 0 when there is no scenario, and
 * non-zero values follow the LobbyScenarioSource enum in control_event.h
 * (1=beside the map, 2=one the host picked). The three strings are empty
 * whenever the source is 0, and never NULL. extraTeams is what the
 * scenario says about a host adding teams of its own. */
uint8_t     clientSimGetLobbyScenarioSource(const ClientSim *cs);
const char *clientSimGetLobbyScenarioName(const ClientSim *cs);
const char *clientSimGetLobbyScenarioFileName(const ClientSim *cs);
const char *clientSimGetLobbyScenarioDescription(const ClientSim *cs);
bool        clientSimGetLobbyScenarioExtraTeams(const ClientSim *cs);

/* Server map-upload policy as last broadcast in the lobby-settings event.
 * Defaults to UPLOAD_POLICY_ALLOW until the first event arrives. */
UploadPolicy clientSimGetUploadPolicy(const ClientSim *cs);

/* Server visibility rules (pillboxes / bases / allied tanks) as last
 * broadcast in the lobby-settings event. Raw mirror. Until the first event
 * arrives the policies read back the set an unconfigured server starts on
 * (Key for pillboxes, off for bases and allied tanks, matching serverSimInit)
 * and the decays read back 0 seconds; a payload that predates the fields
 * carries viewPolicyAlways and a zero decay for every category and is
 * mirrored as it stands. Out-of-range categories read back viewPolicyAlways
 * and 0. */
ViewPolicy  clientSimGetViewPolicy(const ClientSim *cs, ViewCategory cat);
uint16_t    clientSimGetViewDecaySecs(const ClientSim *cs, ViewCategory cat);

/* True when the server has classic mode on, as last broadcast in the
 * lobby-settings event. Reads back false until the first event arrives,
 * and a payload that predates the field leaves it false too. */
bool        clientSimGetClassicMode(const ClientSim *cs);

/* True when the server is sending allies who stand in trees, as last
 * broadcast in the lobby-settings event. Reads back false until the first
 * event arrives, and a payload that predates the field leaves it false
 * too — which matches the classic behaviour the option turns off. */
bool        clientSimGetAlliesInTrees(const ClientSim *cs);

/* What the server last asked the map overview for in the lobby-settings
 * event: which block of squares it keeps live around the player's own tank
 * (OverviewWindow), and what stops the player seeing inside that block
 * (LineOfSightMode). The client honours both rather than choosing for
 * itself, but the server keeps sending the same map data either way, so a
 * modified client can ignore them and see what an honest one cannot. Both
 * read back Expanded and off until the first event arrives, and a payload
 * that predates the fields leaves them there too. */
uint8_t     clientSimGetOverviewWindow(const ClientSim *cs);
uint8_t     clientSimGetLineOfSight(const ClientSim *cs);

/* What the server does with the voice its clients send it, as last broadcast
 * in the lobby-settings event. serverVoiceOff means voice sent from here is
 * dropped, so a client on such a server captures and sends none. Reads back
 * serverVoiceOn until the first event arrives, which is what every server did
 * before the setting existed. */
ServerVoiceMode clientSimGetServerVoiceMode(const ClientSim *cs);

uint8_t     clientSimGetLobbyTeamInUse(const ClientSim *cs, BYTE teamId);
uint8_t     clientSimGetLobbyTeamColor(const ClientSim *cs, BYTE teamId);
uint8_t     clientSimGetLobbyTeamPool(const ClientSim *cs, BYTE teamId);
/* The team's START_SIDE_* choice as last broadcast; START_SIDE_ANY (0)
 * until the first team-meta event for that team arrives. */
uint8_t     clientSimGetLobbyTeamStartSide(const ClientSim *cs, BYTE teamId);
const char *clientSimGetLobbyTeamName(const ClientSim *cs, BYTE teamId);

/* Which of the brain's declared modes this bot runs in — an index into
 * brainListLoadModes(<the slot's brain>)'s list, 0 being the default mode
 * every ordinary game uses. clientSimGetLobbyBotDifficulty is then an index
 * into THAT mode's level list. */
uint8_t     clientSimGetLobbyBotMode(const ClientSim *cs, BYTE slot);
uint8_t     clientSimGetLobbyBotDifficulty(const ClientSim *cs, BYTE slot);
uint8_t     clientSimGetLobbyBotPersonality(const ClientSim *cs, BYTE slot);
/* Returns the catalogue index of the brain assigned to a lobby bot slot.
 * 0xFF means the bot uses the server's default brain; for any other
 * value the caller can look up clientSimGetLobbyBrainList(cs)->entries[idx]
 * to recover the display name and wire path. Out-of-range slot returns
 * 0xFF. */
uint8_t     clientSimGetLobbyBotBrain(const ClientSim *cs, BYTE slot);

const BrainList *clientSimGetLobbyBrainList(const ClientSim *cs);

/* Last finished round's scoreboard + awards, or NULL if none has been
 * received since the last countdown (round-only scope). */
const RoundStatsSummary *clientSimGetLastRoundStats(const ClientSim *cs);

/* Counter bumped each time another player reports having rated or commented
 * on the round clientSimGetLastRoundStats describes. Only movement matters —
 * a caller holding its own last-seen value re-reads that round's WinBolo.net
 * page when the two differ. 0 for a NULL cs. */
uint32_t clientSimGetRatingPostedSeq(const ClientSim *cs);

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
/* Whether the server said this map has a script beside it. False for a
 * folder, for an index out of range, and for every entry from a server
 * that runs no scenario library. */
bool        clientSimGetLobbyMapListScripted(const ClientSim *cs, int idx);
bool        clientSimGetLobbyMapListReady(const ClientSim *cs);
const char *clientSimGetLobbyMapListReqPath(const ClientSim *cs);
bool        clientSimGetLobbyMapListInFlight(const ClientSim *cs);
/* Monotonic counter, ticked whenever the server's map directory changes
 * under the client: a completed MAP_LIST_RSP, a completed MAP_SEARCH_RSP,
 * or a finished upload (which invalidates the cached listing as well as
 * bumping this). A caller holding its own last-seen value re-reads the
 * caches when the two differ; only movement matters. 0 for a NULL cs. */
uint32_t    clientSimGetLobbyMapListSeq(const ClientSim *cs);

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

/* Server-map preview byte stream — populated asynchronously by the
 * CHANNEL_BULK preview receiver (or _ERR) after the client sends a
 * MAP_PREVIEW_REQ (see clientSimNetSendLobbyMapPreviewRequest). The GUI
 * polls Ready/Error/Path each frame; on Ready it rasterises Bytes/Len
 * into the chooser preview, then calls clientSimClearLobbyMapPreview.
 * ReqPath echoes the path we asked for so a stale response (user moved
 * on to another map) can be ignored. */
bool           clientSimGetLobbyMapPreviewReady(const ClientSim *cs);
bool           clientSimGetLobbyMapPreviewInFlight(const ClientSim *cs);
bool           clientSimGetLobbyMapPreviewError(const ClientSim *cs);
const char    *clientSimGetLobbyMapPreviewReqPath(const ClientSim *cs);
const char    *clientSimGetLobbyMapPreviewPath(const ClientSim *cs);
const uint8_t *clientSimGetLobbyMapPreviewBytes(const ClientSim *cs);
uint32_t       clientSimGetLobbyMapPreviewLen(const ClientSim *cs);
void           clientSimClearLobbyMapPreview(ClientSim *cs);

/* Spectator feed drain — the session uses these to pull the captured seed and
 * the ordered forward records the bulk sink reassembled while connected as a
 * tankless spectator. The raw bytes are translated/fed to the decoder in a
 * later slice; this slice only captures and exposes them.
 *
 * One forward record handed back by clientSimSpectatorPopRecord: the server's
 * 9-byte transport header is already stripped. payload is owned by the caller
 * (free it) and is NULL when payloadLen is 0. */
typedef struct {
    bool      isKeyframe;
    uint32_t  gameTick;
    uint32_t  segment;
    uint8_t  *payload;     /* caller-owned; NULL when payloadLen == 0 */
    uint32_t  payloadLen;
} ClientSpectatorRecord;

/* True once the seed blob has been fully received. */
bool     clientSimSpectatorSeedReady(const ClientSim *cs);
/* Hand the seed blob to the caller, transferring ownership (*outBlob must be
 * freed). Returns false (outputs untouched) if no seed is ready; the feed no
 * longer holds the seed after a successful take. */
bool     clientSimSpectatorTakeSeed(ClientSim *cs, uint8_t **outBlob,
                                    uint32_t *outLen);
/* Number of forward records currently queued. */
uint32_t clientSimSpectatorRecordCount(const ClientSim *cs);
/* Pop the oldest queued forward record into *out (ownership of out->payload
 * passes to the caller). Returns false (out untouched) when the queue is empty. */
bool     clientSimSpectatorPopRecord(ClientSim *cs, ClientSpectatorRecord *out);
/* Latest cold-start countdown the server sent while the delayed ring fills.
 * Returns true and writes *outRemaining (remaining game ticks, ~50/sec) once a
 * countdown has been received; false (untouched) before the first one. */
bool     clientSimSpectatorCountdown(const ClientSim *cs, uint32_t *outRemaining);

/* True while a tankless spectator is in live-lobby mode (fed the live lobby
 * control bus); false once it has been cut to the delayed ring feed. Mirrored
 * one-way from the transport's specLiveLobby. Stays false for a non-spectator
 * sim. The spectator session host reads this to alternate the read-only lobby
 * and the delayed game view. */
bool     clientSimSpectatorIsLiveLobby(const ClientSim *cs);

/* Map upload progress reflection. status: 0=idle, 1=announce sent,
 * 2=ack received (chunks in flight), 3=done, 4=rejected. */
uint8_t     clientSimGetLobbyMapUploadStatus(const ClientSim *cs);
uint8_t     clientSimGetLobbyMapUploadRejectCode(const ClientSim *cs);
const char *clientSimGetLobbyMapUploadFinalPath(const ClientSim *cs);
/* True (and clears the flag) if the server NACK'd a USE_LOCAL request
 * since the last call — drives the BEGIN/CHUNK fallback inside the
 * UDP transport's upload pump. No frontend caller. */
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
/* Enter an item view, or cycle to the next item once in it, with horz and
 * vert both 0; step to the nearest item in that direction otherwise.
 * Cycling wraps around the items; it drops back to the tank view only when
 * there is nothing of that kind left to watch.
 * clientSimStepView steps within whichever item view is current and does
 * nothing in the tank view.
 * Pill and base selection is made here on the client, which holds every pill
 * and base position from the map. Which ally to watch is the server's answer
 * to a request, so clientSimAllyView — and clientSimStepView while in an ally
 * view — sends one, and the view changes when the answer arrives. */
void         clientSimPillView(ClientSim *cs, int horz, int vert);
void         clientSimBaseView(ClientSim *cs, int horz, int vert);
void         clientSimAllyView(ClientSim *cs, int horz, int vert);
void         clientSimStepView(ClientSim *cs, int horz, int vert);
/* Report the current view to the server (CMD_VIEW_STATE) when it has changed
 * since the last report. Called once per display tick.
 * clientSimResetViewStateReport forgets what was last reported, so the next
 * tick sends again — used where the client resets its view for a new round or
 * a freshly installed map. */
void         clientSimSyncViewState(ClientSim *cs);
void         clientSimResetViewStateReport(ClientSim *cs);
void         clientSimRecalc(ClientSim *cs);
/* Refill the back buffer. sight is the mask of squares the player cannot see
 * into, worked out by the caller and NULL for a view nothing blocks sight in;
 * struct ViewSight is declared in the viewport's own header, so a caller that
 * has no use for it never needs the definition. */
struct ViewSight;
void         clientSimUpdateView(ClientSim *cs, updateType value,
                                 const struct ViewSight *sight);
void         clientSimPanX(ClientSim *cs, int dxTiles);
void         clientSimPanY(ClientSim *cs, int dyTiles);
bool         clientSimTankIsDead(ClientSim *cs);
bool         clientSimTankScroll(ClientSim *cs);
void         clientSimCenterTank(ClientSim *cs);
void         clientSimSetAutoScroll(ClientSim *cs, bool isAuto);
void         clientSimSetAutoScrollOverride(ClientSim *cs, bool value);
/* Scroll-method selector. Values match ScrollMechanism (0 = winbolo v1 manual,
   1 = winbolo v1 autoscroll, 2 = enhanced, 3 = andrew enhanced). Exposed as
   int so GUI callers needn't include the internal scroll header. Process-global. */
int          clientSimGetScrollMechanism(void);
void         clientSimSetScrollMechanism(int mech);

/* Whether the overview window in force (clientSimGetOverviewWindow) places the
   block of live squares from the classic view, as against centring it on the
   tank. It is the one thing a GUI caller has to know about the two windows
   apart from their names: with the block on the classic view the scroll keys
   are what drags it, so they go back to the classic scroll, and with the block
   on the tank they move nothing and the map overview pans its own camera with
   them instead. False for a NULL sim. */
bool         clientSimOverviewWindowFollowsView(const ClientSim *cs);

/* The centre of the block of live squares round the player's own tank, in map
   squares and including the sub-square part, for a camera that has to follow
   the block rather than the tank. That is the classic view's own centre, under
   the window placed from that view. False — leaving the outputs alone — under
   the window whose block is centred on the tank, where the tank position is
   what to follow, and whenever there is no live tank view to read that centre
   from. */
bool         clientSimGetOverviewWindowCentreF(const ClientSim *cs, float *outX,
                                               float *outY);

/* My-tank helpers for clients that need the local tank's current map
 * tile (e.g. gamepad build cursor).  Return false when the local tank
 * is destroyed / not yet spawned.
 *
 * clientSimGetMyTankMapPos also returns false while the tank is dead and
 * waiting to respawn: the underlying read gives the map origin then rather
 * than anywhere the tank is. It leaves *mapX / *mapY alone when it fails, so
 * a caller keeps whatever fallback it seeded them with. */
bool         clientSimGetMyTankMapPos(ClientSim *cs, BYTE *mapX, BYTE *mapY);

/* The same position at sub-square precision: map squares with the fraction
 * giving where inside the square the tank sits. Same false cases as the
 * BYTE version; the map overview's follow camera glides on this where the
 * whole-square read would step a square at a time. */
bool         clientSimGetMyTankMapPosF(ClientSim *cs, float *mapX, float *mapY);
bool         clientSimGetGunsightTile(ClientSim *cs, BYTE *mapX, BYTE *mapY);

/* The gunsight's map square and the pixel offset inside it, for a caller that
   draws the crosshair itself. False, with nothing written, when there is no
   tank, when the tank is dead and waiting to respawn, or when the player has
   the sight hidden — the Show Gunsight preference and auto-hide drive the same
   flag. Unlike clientSimGetGunsightTile this never reports the map origin for a
   dead tank. */
bool         clientSimGetGunsightPos(ClientSim *cs, BYTE *mapX, BYTE *mapY,
                                     BYTE *pixelX, BYTE *pixelY);

/* False when the local player has no tank, or has one that is dead and
   waiting to respawn. The overview uses this to stop a dead tank's position
   revealing map or dragging the camera. */
bool         clientSimIsMyTankAlive(const ClientSim *cs);

/* Fill the three per-frame entity lists over the whole map, for a caller that
   does its own visibility filtering. The caller creates and destroys the
   lists. Positions come back as absolute map squares, since the rect starts
   at 0,0. */
void clientSimPrepareOverviewEntities(ClientSim *cs, screenTanks *tks,
                                      screenLgm *lgms, screenBullets *sb);

/* Everything the map overview's render reads from the sim, as plain data.
   The host fills it with the client mutex held and draws from it after the
   mutex is released, so the render's own time — most of it SDL flushing at
   each render-target switch — is no longer spent inside the lock that a hosted
   server's timer thread waits on.

   The memory is copied only when the sim's generation counter has moved since
   the last fill; the entity lists are rebuilt on every fill and have already
   been through the live-square filter, so they hold exactly what is drawn.
   Filled from a NULL sim, it reads as no map, nothing alive and no item view.
   The handle is the host's to create and destroy. */
typedef struct OverviewSnapshot OverviewSnapshot;

/* One pill or base and the number the views draw on it, at its absolute map
   square. The snapshot lists every one the sim has; whether a square shows
   its number is the render's decision, made against the memory's tile and
   live flag for that square. */
typedef struct OverviewItemLabel {
  BYTE mapX;
  BYTE mapY;
  BYTE number;   /* the value drawn, already decremented as the views do */
  bool isBase;
} OverviewItemLabel;

OverviewSnapshot *overviewSnapshotCreate(void);
void              overviewSnapshotDestroy(OverviewSnapshot *s);

/* Fill from the live sim. Call with the client mutex held; render from the
   result with it released. */
void clientSimFillOverviewSnapshot(ClientSim *cs, OverviewSnapshot *s);

/* Readers for the render. Each takes a NULL snapshot as nothing to draw. */
const OverviewMap   *overviewSnapshotMap(const OverviewSnapshot *s);
const screenTanks   *overviewSnapshotTanks(const OverviewSnapshot *s);
const screenLgm     *overviewSnapshotLgms(const OverviewSnapshot *s);
const screenBullets *overviewSnapshotBullets(const OverviewSnapshot *s);
/* Whether the local player's own tank survived the filter — the reticle is
   drawn only when the tank sprite was. */
bool    overviewSnapshotSelfDrawn(const OverviewSnapshot *s);
BYTE    overviewSnapshotMyPlayerNum(const OverviewSnapshot *s);
/* The local tank, on the same terms as clientSimIsMyTankAlive and
   clientSimGetMyTankMapPosF: dead and waiting to respawn reads as not alive,
   and the position is written only for a living tank. */
bool    overviewSnapshotTankAlive(const OverviewSnapshot *s);
bool    overviewSnapshotTankPos(const OverviewSnapshot *s, float *mapX,
                                float *mapY);
bool    overviewSnapshotBlackout(const OverviewSnapshot *s);
/* The gunsight as clientSimGetGunsightPos reported it: false, with nothing
   written, when it declined. */
bool    overviewSnapshotGunsight(const OverviewSnapshot *s, BYTE *mapX,
                                 BYTE *mapY, BYTE *pixelX, BYTE *pixelY);
bool    overviewSnapshotInItemView(const OverviewSnapshot *s);
uint8_t overviewSnapshotViewKind(const OverviewSnapshot *s);
BYTE    overviewSnapshotViewTarget(const OverviewSnapshot *s);
/* The square an item view watches — clientSimGetPillViewX / Y at fill time. */
void    overviewSnapshotItemViewSquare(const OverviewSnapshot *s, int *mapX,
                                       int *mapY);
/* The centre of the live block as clientSimGetOverviewWindowCentreF reported
   it, at sub-square precision: false, with nothing written, where it declined
   — the window that centres its block on the tank, and no live view to read a
   centre from. A camera that follows the block falls back to the tank there. */
bool    overviewSnapshotWindowCentre(const OverviewSnapshot *s, float *mapX,
                                     float *mapY);
/* Every pill and base at its square, with the number the classic view puts
   on it: the first the sim lists at that square, counted from 0. */
int                      overviewSnapshotItemLabelCount(const OverviewSnapshot *s);
const OverviewItemLabel *overviewSnapshotItemLabels(const OverviewSnapshot *s);

/* The smart pings the overview draws on the ground, oldest first — the same
   list clientSimGetPings hands the classic view, taken at fill time so the
   render half never reads the ring the network thread writes. Always a valid
   pointer for a non-NULL snapshot; the count is what matters. */
const ClientPing *overviewSnapshotPings(const OverviewSnapshot *s);
int               overviewSnapshotPingCount(const OverviewSnapshot *s);

void         clientSimShowMessages(ClientSim *cs, BYTE msgType, bool isShown);
void         clientSimNetStatusMessage(ClientSim *cs, char *messageStr);

/* Submits a build request for the local LGM through InputPacket,
 * gated on tank armour and net status. */
void         clientSimManMove(ClientSim *cs, buildSelect buildS);

/* Same as clientSimManMove but takes absolute map coords directly,
 * bypassing the mouse/touch cursor + xOffset flow. Used by the
 * gamepad build cursor which already tracks the absolute target tile. */
void         clientSimManMoveToMap(ClientSim *cs, BYTE mapX, BYTE mapY, buildSelect buildS);

/* Cycle the current build selection by `delta` (positive or negative)
 * through the standard order: Trees -> Road -> Building -> Pillbox -> Mine. */
void         clientSimCycleBuildSelect(ClientSim *cs, int delta);

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
/* What a full tank holds on this sim: the four caps the stats above are
   drawn against. A scenario can change any of them, so a bar that divides
   by a literal 40 draws the wrong length the moment one does. This is the
   frontend's only route to the rules — src/gui/ cannot see the sim. */
void         clientSimGetTankFullStats(ClientSim *cs, BYTE *shellsAmount, BYTE *minesAmount, BYTE *armourAmount, BYTE *treesAmount);

/* Whether this sim is running the classic game's numbers, every rule of
   them, and which rule is the first that is not.
   clientSimRulesFirstDifference answers with the index the scenario surface
   names a rule by, or -1 when the whole table is classic.

   Asked by code whose own numbers describe the classic game and which has
   nothing sensible to do on a sim running anything else — the observation
   writers, whose normalisation is a fixed scale a model was trained
   against. This is their only route to the question: the table itself is
   internal, and a caller outside src/bolo/ can reach the brain's view of it,
   which carries part of the table and would call a sim classic on the
   strength of the part it can see. */
bool         clientSimRulesAreClassic(ClientSim *cs);
int          clientSimRulesFirstDifference(ClientSim *cs);
/* What a full base holds on this sim: the three caps the base status bars
   are drawn against, reached the same way and for the same reason as the
   tank's caps above. */
void         clientSimGetBaseFullStats(ClientSim *cs, BYTE *shellsAmount, BYTE *minesAmount, BYTE *armourAmount);
void         clientSimGetKillsDeaths(ClientSim *cs, int *kills, int *deaths);

/*********************************************************
 *NAME:          clientSaveMap
 *PURPOSE:
 * Saves the map. Returns whether the operation was
 * successful or not.
 *
 *ARGUMENTS:
 *  fileName - path and filename to save
 *********************************************************/
bool clientSaveMap(ClientSim *cs, char *fileName);

/* Notifies the local LGM and player table that the server connection
 * has been lost; called by the frontend when a UDP shutdown is seen. */
void         clientSimConnectionLost(ClientSim *cs);

#endif /* CLIENT_SIM_H */
