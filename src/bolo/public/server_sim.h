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
 *Name:          Server Simulation
 *Filename:      server_sim.h
 *Author:        John Morrison
 *Purpose:
 *  Standalone authoritative game simulation. Runs the
 *  full game from InputPacket inputs, independent of
 *  servercore.c and screen.c.
 *********************************************************/

#ifndef SERVER_SIM_H
#define SERVER_SIM_H

#include <SDL3/SDL.h>
#include "input_packet.h"
#include "gametype.h"          /* gameType enum + brings global.h */
#include "alliance_enums.h"    /* baseAlliance, pillAlliance */
#include "screentank.h"        /* tankAlliance */
#include "brain_list.h"        /* BrainList — returned by serverSimGetBrainList */

/* MapGenConfig is defined in src/mapeditor/mapeditor_generate.h.
 * Forward-declared here so the public server_sim header doesn't
 * pull the mapeditor subtree into every translation unit that
 * includes server_sim.h. Callers that build a config and invoke
 * serverSimEnableRandomMap / serverSimCreateRandomMap include
 * mapeditor_generate.h directly. */
struct MapGenConfig;

#ifndef CLIENTSIM_TYPEDEF
#define CLIENTSIM_TYPEDEF
typedef struct ClientSim ClientSim;
#endif

#ifndef GAMESIM_TYPEDEF
#define GAMESIM_TYPEDEF
typedef struct GameSim GameSim;
#endif

/* Forward decl — full definition in bolo/control_event.h. Kept opaque here so
 * server_sim.h doesn't drag client_sim.h's include closure into every TU. */
struct ControlEvent;

typedef int SubscriberHandle;
#define SUBSCRIBER_HANDLE_INVALID (-1)

/* Threading: serverSimPublishControl, serverSimRegisterSubscriber, and
 * serverSimUnregisterSubscriber are not synchronized. All callers must run on
 * the same thread (today: the main game-tick thread, which drives both
 * transport_local and transport_udp_*). If a subscriber's deliver callback ever
 * runs on a worker thread, or publish is called from outside the tick thread,
 * add a mutex. */
typedef struct {
    void (*deliver)(void *ctx, const struct ControlEvent *evt);
    void *ctx;
    uint16_t generation;   /* matches subscriberGen[slot] when active */
} ControlSubscriber;

#ifndef _AITYPE_ENUM
#define _AITYPE_ENUM
typedef enum {
  aiNone,
  aiYes,
  aiYesAdvantage,
  aiFull
} aiType;
#endif

/* Full state sync interval in ticks (5 seconds at 50 ticks/sec) */
#define FULL_SYNC_INTERVAL 250

/* Lobby countdown: 5 seconds at 50Hz */
#define LOBBY_COUNTDOWN_TICKS 250

/* Game-over hold: 3 seconds at 50Hz */
#define GAMEOVER_HOLD_TICKS 150

typedef enum {
  serverStateLobby,     /* Waiting for players, accepting joins */
  serverStateCountdown, /* All ready, countdown to game start */
  serverStateRunning,   /* Game in progress */
  serverStateGameOver   /* Game ended, transition back to lobby */
} ServerState;

typedef struct {
  uint8_t teamNumber;  /* 1-16 (0 = unassigned) */
  bool ready;
  bool isBot;          /* Managed by bot system, not by player packets */
} LobbyPlayer;

/* Per-team metadata — used by the Layout A lobby UI for color tinting,
 * editable team names, and per-team bot naming pools. The `in_use` flag
 * is 0 for any team number that has no metadata yet (renders with
 * default name "Team N" and a fallback color). Team membership lives
 * in LobbyPlayer.teamNumber; this struct carries presentation only. */
#ifndef LOBBY_TEAM_NAME_LEN
#define LOBBY_TEAM_NAME_LEN 32
#endif
typedef struct {
  uint8_t in_use;       /* 0 = defaults; 1 = customised */
  uint8_t color;        /* index into client-side kTeamColors[] */
  uint8_t namingPool;   /* index into client-side bot pool table */
  char    name[LOBBY_TEAM_NAME_LEN];
} TeamMetadata;

/* Per-bot config — extends bot identity with difficulty + personality
 * the Layout A AiConfig sub-panel writes. Brain consumption is deferred
 * (NewAutopilot accepts the values via brain.set_config but ignores
 * them in v1). Indexed by slot (matches bot's playerNum). */
typedef struct {
  uint8_t difficulty;   /* 0=easy, 1=normal, 2=hard */
  uint8_t personality;  /* 0=normal, 1=aggressive, 2=defensive, 3=sniper */
} LobbyBotConfig;

#ifndef BALANCEPROPOSAL_TYPEDEF
#define BALANCEPROPOSAL_TYPEDEF
typedef struct BalanceProposal BalanceProposal;
#endif
struct BalanceProposal {
  uint8_t teamForSlot[MAX_TANKS]; /* Proposed team number per slot (0 = unassigned by WBN) */
  bool pending;                   /* True while proposal is active and hasn't been applied/dismissed */
  bool requestInFlight;           /* True while the HTTP call is running (prevents duplicate requests) */
  bool broadcastNeeded;           /* True when thread finishes; cleared after first broadcast */
  uint8_t teamSize;               /* Requested team size, passed through to WBN API */
  SDL_AtomicInt shutdownFlag;     /* Set to 1 on shutdown; balance thread checks before accessing sim */
};

typedef struct ServerSim ServerSim;

/*********************************************************
 *NAME:          serverSimCreate
 *PURPOSE:
 *  Allocates and initializes a ServerSim by loading a map
 *  file. Returns the new ServerSim on success, or NULL on
 *  failure.
 *
 *ARGUMENTS:
 *  mapFileName - Path to the .map file
 *  game        - Game type (open/tournament/strict)
 *  hiddenMines - Are hidden mines allowed
 *  startDelay  - Game start delay (in ticks)
 *  gameLen     - Game length in ticks (-1 = unlimited)
 *********************************************************/
ServerSim *serverSimCreate(char *mapFileName, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen);

/*********************************************************
 *NAME:          serverSimCreateCompressed
 *PURPOSE:
 *  Allocates and initializes a ServerSim from a compressed
 *  map buffer (e.g. built-in maps). Returns the new
 *  ServerSim on success, or NULL on failure.
 *
 *ARGUMENTS:
 *  buff        - Compressed map data
 *  buffLen     - Length of compressed data
 *  game        - Game type (open/tournament/strict)
 *  hiddenMines - Are hidden mines allowed
 *  startDelay  - Game start delay (in ticks)
 *  gameLen     - Game length in ticks (-1 = unlimited)
 *********************************************************/
ServerSim *serverSimCreateCompressed(BYTE *buff, int buffLen, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen);

/*********************************************************
 *NAME:          serverSimDestroy
 *PURPOSE:
 *  Destroys a ServerSim, frees all owned resources, and
 *  frees the ServerSim itself. Accepts NULL (no-op). The
 *  caller must not call free(sim) afterward.
 *
 *ARGUMENTS:
 *  sim - Pointer to the ServerSim to destroy (may be NULL)
 *********************************************************/
void serverSimDestroy(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimTick
 *PURPOSE:
 *  Advances the simulation by one tick. On odd ticks,
 *  only tankTurn is applied (keys tick). On even ticks,
 *  tankUpdate + all world systems are updated (game tick).
 *
 *ARGUMENTS:
 *  sim - Pointer to the ServerSim
 *********************************************************/
void serverSimTick(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimApplyInput
 *PURPOSE:
 *  Buffers an InputPacket for processing on the next tick.
 *
 *ARGUMENTS:
 *  sim   - Pointer to the ServerSim
 *  input - The InputPacket to buffer
 *********************************************************/
void serverSimApplyInput(ServerSim *sim, const InputPacket *input);

/*********************************************************
 *NAME:          serverSimAddPlayer
 *PURPOSE:
 *  Connects a player slot and creates their tank.
 *
 *ARGUMENTS:
 *  sim        - Pointer to the ServerSim
 *  playerNum  - Player slot (0..MAX_TANKS-1)
 *  playerName - Player name (may be NULL for unnamed)
 *  wantRejoin - If true, restore ownership of pills/bases from previous session
 *********************************************************/
void serverSimAddPlayer(ServerSim *sim, BYTE playerNum, const char *playerName, bool wantRejoin);

/*********************************************************
 *NAME:          serverSimRemovePlayer
 *PURPOSE:
 *  Disconnects a player slot and cleans up all per-player
 *  state (tank, lgm, input queue).
 *
 *ARGUMENTS:
 *  sim       - Pointer to the ServerSim
 *  playerNum - Player slot (0..MAX_TANKS-1)
 *********************************************************/
void serverSimRemovePlayer(ServerSim *sim, BYTE playerNum);

/*********************************************************
 *NAME:          ServerSimBotConfig
 *PURPOSE:
 *  Bot configuration for serverSimAddBot. Spawn position
 *  is not configurable — startsAssignBatch() inside
 *  serverSimResetGameWorld places each tank deterministically
 *  from map start data.
 *********************************************************/
typedef struct ServerSimBotConfig {
    const char *brainPath;
    const char *brainName;
    aiType      ai;
    gameType    gameType;
    bool        hiddenMines;
    BYTE        teamNumber;  /* 0 = no team */
} ServerSimBotConfig;

/*********************************************************
 *NAME:          serverSimAddBot
 *PURPOSE:
 *  Adds a bot in lobby state. Validates inputs, calls
 *  serverSimAddPlayer to register the slot, then marks
 *  the slot as a ready bot with the configured team.
 *
 *  Returns true on success. Returns false (without partial
 *  writes) if cfg is NULL, cfg->brainPath is NULL,
 *  playerNum >= MAX_TANKS, or sim->playerConnected[playerNum]
 *  is already TRUE.
 *
 *ARGUMENTS:
 *  sim       - Pointer to the ServerSim
 *  playerNum - Player slot (0..MAX_TANKS-1)
 *  cfg       - Bot configuration (must not be NULL)
 *********************************************************/
bool serverSimAddBot(ServerSim *sim, BYTE playerNum,
                     const ServerSimBotConfig *cfg);

/* Per-bot info populated by serverSimGetBotInfo. POD; no
 * allocations or ownership. brainName is fixed-size: the
 * bot's registered display name copied via SDL_strlcpy. */
typedef struct {
    bool     isBot;
    bool     hasBrain;          /* aiFull with a live brain */
    char     brainName[64];     /* brain identity: basename of the brain
                                 * script path, with .lua stripped and
                                 * "init" replaced by the parent directory
                                 * name (e.g. "NewAutopilot" for
                                 * brains/NewAutopilot/init.lua) */
    double   lastThinkMs;       /* most recent brain tick */
    double   targetMs;          /* target the next tick will use */
    uint32_t overrunCount;      /* cumulative since session start */
} BotInfo;

/* Bot pool snapshot populated by serverSimGetBotPoolStats. POD;
 * no allocations or ownership. */
typedef struct {
    int      workerCount;       /* botWorkerPoolGetSize() */
    int      activeBots;        /* currently-active bot count */
    double   ewmaSerialMs;      /* serial-stage EWMA */
    double   currentTargetMs;   /* per-bot budget for next tick */
    double   lastBrainPhaseMs;  /* wall-clock of last brain dispatch */
    double   ewmaBrainPhaseMs;  /* EWMA of brain dispatch wall-clock */
    double   lastSerialMs;      /* last serial-stage cost (ms) */
    uint32_t totalOverruns;     /* sum of overrunCount across bots */
} BotPoolStats;

/* Max candidate count for BrainGoalInfo (defined in bot_manager.h,
 * which is internal-only). Lives here so it can be referenced from
 * the public surface without exposing bot_manager.h. */
#define BRAIN_GOAL_MAX_CANDIDATES 32

/*********************************************************
 * Bot pool wrappers.
 *
 * These thin-forward to bot_manager.c, which owns the
 * process-wide bot pool (workers + per-slot state). Public
 * surface so non-server callers don't need to include
 * bot_manager.h. The ServerSim * argument is currently
 * advisory on the queries — bot state is module-global — but
 * keeps the signatures stable for the day bot state moves
 * onto ServerSim.
 *********************************************************/

/* === Bot pool lifecycle (process-wide) === */
/* Pool state is module-global, not per-ServerSim. Call once
 * at startup. `threads` is the total number of concurrent
 * brain-tick runners including the producer (main) thread;
 * the pool is sized to threads-1. Pass 0 to auto-size from
 * logical CPU cores. */
bool serverSimBotPoolInit(int threads);

/* Request a live resize of the worker pool. Stashes the
 * value as pending; the next serverSimBotTick applies it. */
void serverSimRequestBotThreads(int total_runners);

/* Current total runner count (workers + producer). */
int  serverSimGetBotThreads(void);

/* Pending thread-count request, or -1 if no resize pending. */
int  serverSimGetPendingBotThreads(void);

/* === Default debug mode for new bots === */
/* Set the BRAIN_DEBUG_MODE value bots inherit at creation.
 * Affects bots created AFTER this call. */
void serverSimSetBotDefaultDebugMode(bool enabled);

/* === Pre-think hook === */
/* Register a callback invoked just before each bot's brain
 * runs (with the bot's playerNum) and again with -1 after.
 * Pass NULL to clear. */
void serverSimSetBotPreThinkHook(void (*hook)(int playerNum));

/* === Per-sim bot lifecycle === */

/* Create a fully-initialised bot: lobby slot, brain, ClientSim,
 * transport, map data. Forwards to bot_manager.c. Distinct from
 * serverSimAddBot, which only registers the lobby slot — the
 * full constructor calls serverSimAddBot internally. */
bool serverSimCreateBot(ServerSim *sim, BYTE playerNum,
                        const char *brainPath, const char *brainName,
                        aiType ai, gameType game, bool hiddenMines);

void serverSimRemoveBot(ServerSim *sim, BYTE playerNum);
void serverSimDestroyBots(ServerSim *sim);
void serverSimOnBotGameStart(ServerSim *sim);
void serverSimSetBotTeams(ServerSim *sim,
                          const BYTE *teamOf, BYTE numPlayers);

/* === Per-tick bot advance === */
/* Run brains for one tick. Distinct from serverSimTick
 * (world step): typical pattern is serverSimBotTick first,
 * then the world tick. */
void serverSimBotTick(ServerSim *sim, aiType ai);

/* === Bot queries (POD-returning) === */
BYTE   serverSimGetNumBots(ServerSim *sim);
bool   serverSimHasAnyBot(ServerSim *sim);
bool   serverSimIsBot(ServerSim *sim, BYTE playerNum);
double serverSimGetBotLastThinkMs(ServerSim *sim, BYTE playerNum);
bool   serverSimGetBotInfo(ServerSim *sim, BYTE playerNum, BotInfo *out);
void   serverSimGetBotPoolStats(ServerSim *sim, BotPoolStats *out);
bool   serverSimToggleAllBrainDebugMode(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimSetTeam
 *PURPOSE:
 *  Sets a player's lobby team. teamNumber > 16 is coerced
 *  to 1, preserving the existing servermain.c range
 *  behavior. No-op if playerNum >= MAX_TANKS.
 *
 *ARGUMENTS:
 *  sim        - Pointer to the ServerSim
 *  playerNum  - Player slot (0..MAX_TANKS-1)
 *  teamNumber - 0 = no team; 1-16 = team number
 *********************************************************/
void serverSimSetTeam(ServerSim *sim, BYTE playerNum, BYTE teamNumber);

/*********************************************************
 *NAME:          serverSimSetReady
 *PURPOSE:
 *  Sets a player's lobby-ready flag. No-op if
 *  playerNum >= MAX_TANKS or lobby is not enabled.
 *
 *ARGUMENTS:
 *  sim       - Pointer to the ServerSim
 *  playerNum - Player slot (0..MAX_TANKS-1)
 *  ready     - true = ready, false = not ready
 *********************************************************/
void serverSimSetReady(ServerSim *sim, BYTE playerNum, bool ready);

/*********************************************************
 *NAME:          serverSimSetBotAiType
 *PURPOSE:
 *  Sets the AI advantage level cached for lobby bot
 *  creation.
 *********************************************************/
void serverSimSetBotAiType(ServerSim *sim, aiType ai);

/*********************************************************
 *NAME:          serverSimSetBotBrainPath
 *PURPOSE:
 *  Sets the brain path cached for lobby bot creation.
 *  Truncates to fit the internal buffer; always null-
 *  terminates. NULL or empty path clears it.
 *********************************************************/
void serverSimSetBotBrainPath(ServerSim *sim, const char *path);

/*********************************************************
 *NAME:          serverSimSetEmptyResetEnabled
 *PURPOSE:
 *  Enables or disables the empty-server auto-reset
 *  feature.
 *********************************************************/
void serverSimSetEmptyResetEnabled(ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          serverSimSetHasPassword
 *PURPOSE:
 *  Marks whether the server has a password set.
 *********************************************************/
void serverSimSetHasPassword(ServerSim *sim, bool hasPassword);

/*********************************************************
 *NAME:          serverSimSetLobbyEnabled
 *PURPOSE:
 *  Marks whether the lobby phase is active. false = no
 *  lobby (run immediately on the local-headless setup
 *  paths). Today this is also set false by some round-
 *  start logic; do NOT alter that.
 *********************************************************/
void serverSimSetLobbyEnabled(ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          serverSimEnableRandomMap
 *PURPOSE:
 *  Enables random map mode and caches the generator
 *  config for between-round regeneration. Sets
 *  randomMapEnabled = true, copies *cfg into
 *  randomMapConfig, and sets randomMapFixedSeed.
 *********************************************************/
void serverSimEnableRandomMap(ServerSim *sim,
                              const struct MapGenConfig *cfg,
                              bool fixedSeed);

/*********************************************************
 *NAME:          serverSimEnterLobby
 *PURPOSE:
 *  Sets the server state to serverStateLobby. Used by
 *  the multiplayer-host startup path to begin in lobby.
 *  No other side effects — for the round-start sequence
 *  use serverSimStartGame.
 *********************************************************/
void serverSimEnterLobby(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimInstallMapDirList
 *PURPOSE:
 *  Installs the validated map rotation list. Takes
 *  ownership of `files` (the array and each strdup'd
 *  entry inside it). Caller must not free these
 *  afterward.
 *********************************************************/
void serverSimInstallMapDirList(ServerSim *sim,
                                char **files, int count);

/*********************************************************
 *NAME:          serverSimPrependEvents
 *PURPOSE:
 *  Insert `count` events at the front of the events buffer
 *  in front of any events already there. No-op if the
 *  resulting count would exceed MAX_SNAPSHOT_EVENTS or if
 *  count == 0.
 *
 *  Used by braintest_main.c's double-tick path that
 *  serverSimTicks twice and wants to preserve the first
 *  tick's events as a prefix on the second tick's events.
 *
 *ARGUMENTS:
 *  sim    - The ServerSim
 *  events - Source array of events to prepend
 *  count  - Number of events from `events` to prepend
 *********************************************************/
void serverSimPrependEvents(ServerSim *sim,
                            const GameEvent *events,
                            uint8_t count);

/*********************************************************
 *NAME:          serverSimSetAutoCloseOnEmpty
 *PURPOSE:
 *  Configures whether the server should close when all
 *  players leave.
 *********************************************************/
void serverSimSetAutoCloseOnEmpty(ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          serverSimSetBalanceBroadcastNeeded
 *PURPOSE:
 *  Sets the balance proposal's broadcastNeeded flag. Set
 *  true by the balance worker thread when it finishes;
 *  cleared after the main thread broadcasts.
 *********************************************************/
void serverSimSetBalanceBroadcastNeeded(ServerSim *sim, bool needed);

/*********************************************************
 *NAME:          serverSimSetBalanceRequestInFlight
 *PURPOSE:
 *  Sets the balance proposal's requestInFlight flag. True
 *  while the HTTP call is in flight (prevents duplicates);
 *  cleared once the worker returns or aborts.
 *********************************************************/
void serverSimSetBalanceRequestInFlight(ServerSim *sim, bool inFlight);

/*********************************************************
 *NAME:          serverSimSetEmptyResetMinutes
 *PURPOSE:
 *  Sets the number of minutes an empty server waits
 *  before resetting to lobby.
 *********************************************************/
void serverSimSetEmptyResetMinutes(ServerSim *sim, int minutes);

/*********************************************************
 *NAME:          serverSimSetMapName
 *PURPOSE:
 *  Copies name into the sim's mapName buffer,
 *  truncating as needed. NULL or empty name clears it.
 *********************************************************/
void serverSimSetMapName(ServerSim *sim, const char *name);

/*********************************************************
 *NAME:          serverSimSetMessageLogFile
 *PURPOSE:
 *  Sets the file path used by server-message logging and
 *  enables file-based logging. Truncates the path to fit
 *  the internal buffer; always null-terminates. Passing
 *  NULL or empty disables file-based logging and clears
 *  the path.
 *
 *ARGUMENTS:
 *  sim  - Pointer to the ServerSim
 *  path - File path, or NULL/empty to disable
 *********************************************************/
void serverSimSetMessageLogFile(ServerSim *sim, const char *path);

/*********************************************************
 *NAME:          serverSimSetQuiet
 *PURPOSE:
 *  Sets the server's quiet flag. When true, non-assistant
 *  messages and console output are suppressed.
 *
 *ARGUMENTS:
 *  sim   - Pointer to the ServerSim
 *  quiet - true = suppress messages, false = print
 *********************************************************/
void serverSimSetQuiet(ServerSim *sim, bool quiet);

/*********************************************************
 *NAME:          serverSimSetQuitOnWin
 *PURPOSE:
 *  Configures whether the server should quit after
 *  a win is detected.
 *********************************************************/
void serverSimSetQuitOnWin(ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          serverSimSetServerPort
 *PURPOSE:
 *  Records the UDP port the server is listening on. Set
 *  once at startup; consumed by server-info responses.
 *********************************************************/
void serverSimSetServerPort(ServerSim *sim, unsigned short port);

/*********************************************************
 *NAME:          serverSimSetTickLimit
 *PURPOSE:
 *  Sets the running-tick limit. 0 = unlimited.
 *********************************************************/
void serverSimSetTickLimit(ServerSim *sim, int32_t ticks);

/*********************************************************
 *NAME:          serverSimSetUserLogFileName
 *PURPOSE:
 *  Copies name into the user log file buffer. NULL or
 *  empty clears.
 *********************************************************/
void serverSimSetUserLogFileName(ServerSim *sim, const char *name);

/*********************************************************
 *NAME:          serverSimSetWantLogging
 *PURPOSE:
 *  Configures whether the server should start logging
 *  when entering a running game.
 *********************************************************/
void serverSimSetWantLogging(ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          serverSimGetTankState
 *PURPOSE:
 *  Retrieves the world position of a player's tank.
 *  Returns TRUE if the tank exists.
 *
 *ARGUMENTS:
 *  sim       - Pointer to the ServerSim
 *  playerNum - Player slot
 *  wx        - Output: world X coordinate
 *  wy        - Output: world Y coordinate
 *********************************************************/
bool serverSimGetTankState(ServerSim *sim, BYTE playerNum, WORLD *wx, WORLD *wy);

/*********************************************************
 *NAME:          serverSimAddEvent
 *PURPOSE:
 *  Adds a game event to the sim's event buffer.
 *  Events are cleared at the start of each tick.
 *
 *ARGUMENTS:
 *  sim   - Pointer to the ServerSim
 *  event - The event to add
 *********************************************************/
void serverSimAddEvent(ServerSim *sim, const GameEvent *event);

/*********************************************************
 *NAME:          serverSimGetCompressedMap
 *PURPOSE:
 *  Serializes the current map, bases, pillboxes, and starts
 *  into a compressed buffer suitable for sending to clients.
 *  Returns the length of the compressed data.
 *
 *ARGUMENTS:
 *  sim    - Pointer to the ServerSim
 *  output - Buffer to receive compressed data (must be at
 *           least 65536 bytes)
 *********************************************************/
int serverSimGetCompressedMap(ServerSim *sim, BYTE *output);

/*********************************************************
 *NAME:          serverSimGetActive
 *PURPOSE:
 *  Returns the currently active ServerSim, or NULL if
 *  no sim tick is in progress. Used by servercore.c
 *  routing functions to access sim state directly.
 *********************************************************/
ServerSim *serverSimGetActive(void);

/*********************************************************
 *NAME:          serverSimBuildSnapshot
 *PURPOSE:
 *  Builds a complete snapshot from the current sim state.
 *  Used by both transport_local and transport_udp.
 *
 *ARGUMENTS:
 *  sim           - Pointer to the ServerSim
 *  clientIdx     - Player slot requesting the snapshot
 *  hdr           - Output: filled SnapshotHeader
 *  tanksOut      - Output: TankSnapshot array
 *  maxTanks      - Max entries in tanksOut
 *  shellsOut     - Output: ShellSnapshot array
 *  maxShells     - Max entries in shellsOut
 *  tkExplOut     - Output: TkExplosionSnapshot array
 *  maxTkExpl     - Max entries in tkExplOut
 *  basesOut      - Output: BaseSnapshot array
 *  maxBases      - Max entries in basesOut
 *  pillsOut      - Output: PillSnapshot array
 *  maxPills      - Max entries in pillsOut
 *  eventsOut     - Output: GameEvent array
 *  maxEvents     - Max entries in eventsOut
 *  noCull        - If true, skip viewport-based culling so the snapshot
 *                  contains every tank, shell, explosion, base, pill,
 *                  and event regardless of distance from clientIdx. Used
 *                  by BrainTest's god-view recording; network paths pass
 *                  false.
 *********************************************************/
void serverSimBuildSnapshot(ServerSim *sim, BYTE clientIdx,
                            SnapshotHeader *hdr,
                            TankSnapshot *tanksOut, int maxTanks,
                            ShellSnapshot *shellsOut, int maxShells,
                            TkExplosionSnapshot *tkExplOut, int maxTkExpl,
                            BaseSnapshot *basesOut, int maxBases,
                            PillSnapshot *pillsOut, int maxPills,
                            GameEvent *eventsOut, int maxEvents,
                            bool noCull);

/*********************************************************
 *NAME:          serverSimInformation
 *PURPOSE:
 *  Prints game information to stdout (for "info" command).
 *
 *ARGUMENTS:
 *  sim    - Pointer to the ServerSim
 *  locked - Whether the game is currently locked
 *********************************************************/
void serverSimInformation(ServerSim *sim, bool locked);

/*********************************************************
 *NAME:          serverSimSaveMap
 *PURPOSE:
 *  Saves the current map state to a file.
 *  Returns TRUE on success.
 *********************************************************/
bool serverSimSaveMap(ServerSim *sim, char *fileName);

/*********************************************************
 *NAME:          serverSimCheckGameWin
 *PURPOSE:
 *  Checks if all bases are owned by a single alliance.
 *  Returns TRUE if the game is won.
 *********************************************************/
bool serverSimCheckGameWin(ServerSim *sim, bool printWinners);

/*********************************************************
 *NAME:          serverSimCheckAutoClose
 *PURPOSE:
 *  Returns TRUE if at least one player has connected and
 *  then all players have disconnected.
 *********************************************************/
bool serverSimCheckAutoClose(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimGetNumPlayers
 *PURPOSE:
 *  Returns the number of currently connected players.
 *********************************************************/
BYTE serverSimGetNumPlayers(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimGetNumNeutralBases
 *PURPOSE:
 *  Returns the number of neutral bases.
 *********************************************************/
BYTE serverSimGetNumNeutralBases(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimGetNumNeutralPills
 *PURPOSE:
 *  Returns the number of neutral pillboxes.
 *********************************************************/
BYTE serverSimGetNumNeutralPills(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimIsRunning
 *PURPOSE:
 *  Returns TRUE if there is an active running simulation.
 *  Replaces checking serverCoreGameRunning global.
 *********************************************************/
bool serverSimIsRunning(void);

/*********************************************************
 *NAME:          serverSimConsoleMessage
 *PURPOSE:
 *  Sends a console message via the active sim's callback.
 *  Safe to call even if no active sim (no-op in that case).
 *  Replaces serverCoreServerConsoleMessage().
 *********************************************************/
void serverSimConsoleMessage(const char *msg);

/*********************************************************
 *NAME:          serverSimAbortCountdown
 *PURPOSE:
 *  Returns the server from serverStateCountdown to
 *  serverStateLobby and resets the countdown timer.
 *  Used by the UDP transport when a player unreadies
 *  during the countdown window.
 *********************************************************/
void serverSimAbortCountdown(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimClearBalanceProposal
 *PURPOSE:
 *  Zeroes out the WBN team balance proposal — clears
 *  pending/in-flight/broadcast flags and all per-slot
 *  team assignments. Used after a proposal is applied,
 *  dismissed, or cancelled by a state transition.
 *********************************************************/
void serverSimClearBalanceProposal(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimEnterGameOver
 *PURPOSE:
 *  Transitions the server to game-over state. If lobby is
 *  enabled, starts the game-over hold timer. If not, the
 *  server will shut down.
 *********************************************************/
void serverSimEnterGameOver(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimReturnToLobby
 *PURPOSE:
 *  Transitions from game-over back to lobby state.
 *  Only called when lobbyEnabled is true.
 *********************************************************/
void serverSimReturnToLobby(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimLobbyCheckAllReady
 *PURPOSE:
 *  Checks if all connected players are ready. If so,
 *  transitions to countdown state.
 *********************************************************/
void serverSimLobbyCheckAllReady(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimStartGame
 *PURPOSE:
 *  Called when countdown reaches zero. Resets the game
 *  world, applies team alliances, creates tanks for all
 *  connected players, and transitions to running state.
 *********************************************************/
void serverSimStartGame(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimReapplyTeamAlliances
 *PURPOSE:
 *  Runs the team-pair alliance pass over the currently
 *  connected players: for every pair (a, b) where both
 *  share the same non-zero lobby teamNumber, mutual-allies
 *  them via playersAcceptAlliance and publishes a
 *  CTRL_ALLIANCE_ACCEPT control event.
 *
 *  TECH DEBT: serverSimStartGame already calls this pass
 *  internally. This entry point exists because the SP-host
 *  flow in gamefront.c currently calls serverSimStartGame
 *  before the human player and bots have been added /
 *  before their team numbers are set, then has to re-run
 *  the pass once the lobby state is populated. A future
 *  restructure of the SP-host flow (so that players are
 *  added and teams set before serverSimStartGame is called)
 *  will let the external call site go away.
 *********************************************************/
void serverSimReapplyTeamAlliances(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimResetGameWorld
 *PURPOSE:
 *  Resets the game world using cached map data. Destroys
 *  all tanks/LGMs and recreates world systems. Does NOT
 *  destroy player connections or lobby state.
 *********************************************************/
void serverSimResetGameWorld(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimChangeMap
 *PURPOSE:
 *  Changes the map while in lobby state. Loads a new map
 *  file, updates cached data, and resets ready state.
 *  Returns TRUE on success, FALSE if not in lobby or
 *  map load fails.
 *********************************************************/
bool serverSimChangeMap(ServerSim *sim, char *mapFileName);

/*********************************************************
 *NAME:          serverSimMapDirBuild
 *PURPOSE:
 *  Scans a directory for .map files, validates each by
 *  attempting to load it, and populates the mapDirFiles
 *  array with paths that loaded successfully.
 *  Returns TRUE if at least one valid map was found.
 *********************************************************/
bool serverSimMapDirBuild(ServerSim *sim, const char *dirPath);

/*********************************************************
 *NAME:          serverSimScanMapDir
 *PURPOSE:
 *  Scans dirPath for *.map files, validates each, and
 *  returns the validated paths in a newly-allocated array.
 *  Caller owns the returned array and each string in it
 *  (free each entry with SDL_free, then free the array
 *  with free).
 *
 *  Used by serverSimMapDirBuild (which stuffs the result
 *  into a ServerSim's rotation list) and by the dedicated
 *  server's startup path (which needs the list before any
 *  ServerSim exists, to pick the first map).
 *
 *ARGUMENTS:
 *  dirPath   - Directory to scan
 *  outFiles  - On success, *outFiles is set to a malloc'd
 *              array of SDL_strdup'd paths
 *  outCount  - On success, *outCount is set to entry count
 *RETURNS: TRUE on success, FALSE on empty/missing dir or
 *  no valid maps. On FALSE *outFiles is untouched.
 *********************************************************/
bool serverSimScanMapDir(const char *dirPath,
                         char ***outFiles, int *outCount);

/*********************************************************
 *NAME:          serverSimMapDirPickRandom
 *PURPOSE:
 *  Selects a random map from the mapDirFiles list and
 *  calls serverSimChangeMap to load it. Falls back to
 *  the current cached map if the chosen map fails to load.
 *  Returns TRUE on success.
 *********************************************************/
bool serverSimMapDirPickRandom(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimMapDirDestroy
 *PURPOSE:
 *  Frees the mapDirFiles array.
 *********************************************************/
void serverSimMapDirDestroy(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimMapSkipVoteToggle
 *PURPOSE:
 *  Toggles a player's map skip vote. If the vote count
 *  reaches majority of connected humans, picks a new
 *  random map and resets all votes.
 *********************************************************/
void serverSimMapSkipVoteToggle(ServerSim *sim, uint8_t playerNum);

/*********************************************************
 *NAME:          serverSimMapSkipVotesReset
 *PURPOSE:
 *  Clears all map skip votes.
 *********************************************************/
void serverSimMapSkipVotesReset(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimSendWbnWinEvents
 *PURPOSE:
 *  Sends WINBOLO_NET_EVENT_WIN for each player in the
 *  winning alliance. No-op if the game was not won by
 *  a single alliance.
 *********************************************************/
void serverSimSendWbnWinEvents(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimBuildWinMessage
 *PURPOSE:
 *  Builds a message string listing the winners of the game.
 *  Returns TRUE if the game was won (message populated),
 *  FALSE if no winner (e.g. time limit or manual end).
 *********************************************************/
bool serverSimBuildWinMessage(ServerSim *sim, char *buf, size_t bufSize);

/*********************************************************
 *NAME:          serverSimCheckEmptyReset
 *PURPOSE:
 *  Checks if the server has been empty long enough to
 *  trigger a lobby reset. Returns TRUE if reset should
 *  happen.
 *********************************************************/
bool serverSimCheckEmptyReset(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimCreateRandomMap
 *PURPOSE:
 *  Allocates and initializes a ServerSim using procedural
 *  map generation instead of loading from disk. Returns
 *  the new ServerSim on success, or NULL on failure.
 *********************************************************/
ServerSim *serverSimCreateRandomMap(const struct MapGenConfig *cfg,
                                    gameType game, bool hiddenMines,
                                    int32_t startDelay, int32_t gameLen);

/*********************************************************
 *NAME:          serverSimRandomMapRegenerate
 *PURPOSE:
 *  Generates a new random map for between-round rotation.
 *  Only works when randomMapEnabled is true and state is
 *  lobby. Returns TRUE on success.
 *********************************************************/
bool serverSimRandomMapRegenerate(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimRegisterSubscriber
 *PURPOSE:
 *  Adds an in-process subscriber and synchronously delivers
 *  the current server state to it before returning. Returns
 *  an opaque handle, or SUBSCRIBER_HANDLE_INVALID if no
 *  free slot is available.
 *********************************************************/
SubscriberHandle serverSimRegisterSubscriber(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx);

/*********************************************************
 *NAME:          serverSimRegisterClientSubscriber
 *PURPOSE:
 *  Convenience over serverSimRegisterSubscriber for the
 *  common case of a ClientSim that wants server-published
 *  ControlEvents applied to it automatically. The apply
 *  step is handled inside bolo so frontends don't have to
 *  write a one-line forwarder.
 *********************************************************/
SubscriberHandle serverSimRegisterClientSubscriber(ServerSim *sim,
                                                   ClientSim *cs);

/*********************************************************
 *NAME:          serverSimRequestBalanceProposal
 *PURPOSE:
 *  Initiates a WBN team-balance proposal request. Wraps
 *  the winbolonet HTTP call and keeps the proposal pointer
 *  inside server_sim. Synchronous — caller is responsible
 *  for invoking from a worker thread if non-blocking
 *  behavior is desired.
 *********************************************************/
void serverSimRequestBalanceProposal(ServerSim *sim,
                                     uint8_t totalPlayers,
                                     uint8_t teamSize);

/*********************************************************
 *NAME:          serverSimUnregisterSubscriber
 *PURPOSE:
 *  Removes a subscriber. Idempotent on stale or invalid
 *  handles (no-op).
 *********************************************************/
void serverSimUnregisterSubscriber(ServerSim *sim, SubscriberHandle h);

/*********************************************************
 *NAME:          serverSimPublishControl
 *PURPOSE:
 *  Fans out a ControlEvent to every active subscriber.
 *  Asserts the four dispatcher invariants (no reentry, no
 *  self-subscription, snapshot iteration).
 *********************************************************/
void serverSimPublishControl(ServerSim *sim, const struct ControlEvent *evt);

/*********************************************************
 *NAME:          serverSimFillGamePhaseEvent
 *               serverSimFillLobbySettingsEvent
 *               serverSimFillLobbySlotEvent
 *               serverSimFillPlayerJoinEvent
 *PURPOSE:
 *  Populate a ControlEvent of the corresponding type from
 *  the current ServerSim state. Used by both the initial
 *  state sync (serverSimSyncSubscriber) and live publish
 *  call sites that need an event payload.
 *********************************************************/
void serverSimFillGamePhaseEvent(const ServerSim *sim, struct ControlEvent *evt);
void serverSimFillLobbySettingsEvent(ServerSim *sim, struct ControlEvent *evt);
void serverSimFillLobbySlotEvent(ServerSim *sim, BYTE i, struct ControlEvent *evt);
void serverSimFillPlayerJoinEvent(ServerSim *sim, BYTE i, struct ControlEvent *evt);

/* Layout A — per-team / per-bot / brain-list events. The matching
 * client-side handlers live in clientSimApplyControl. */
void serverSimFillLobbyTeamMetaEvent(const ServerSim *sim, BYTE teamId, struct ControlEvent *evt);
void serverSimFillLobbyBotConfigEvent(ServerSim *sim, BYTE slot, struct ControlEvent *evt);
void serverSimFillLobbyBotBrainEvent(const ServerSim *sim, BYTE slot, struct ControlEvent *evt);
void serverSimFillLobbyBrainListEvent(const ServerSim *sim, struct ControlEvent *evt);

/*********************************************************
 * Read accessors.
 *
 * One per externally-read ServerSim field. Callers outside
 * src/server/<*.c> use these instead of touching fields
 * directly. No mutator versions here — writes go through
 * the public mutator API (serverSimAddBot / SetTeam /
 * SetReady / StartGame / AddPlayer / RemovePlayer / ...).
 *********************************************************/

/* Scalar (bool/enum) accessors */
bool          serverSimBalanceShutdownRequested(const ServerSim *sim);
bool          serverSimIsLobbyEnabled(const ServerSim *sim);
bool          serverSimHasPassword(const ServerSim *sim);
bool          serverSimIsRandomMapEnabled(const ServerSim *sim);
bool          serverSimIsQuiet(const ServerSim *sim);
bool          serverSimGetServerMessageUseLogFile(const ServerSim *sim);
ServerState   serverSimGetState(const ServerSim *sim);
aiType        serverSimGetBotAiType(const ServerSim *sim);

/* Scalar (integer) accessors */
uint32_t       serverSimGetTick(const ServerSim *sim);
uint32_t       serverSimGetTimeCreated(const ServerSim *sim);
int32_t        serverSimGetStartDelay(const ServerSim *sim);
int32_t        serverSimGetGameLength(const ServerSim *sim);
int32_t        serverSimGetCountdownTicks(const ServerSim *sim);
unsigned short serverSimGetServerPort(const ServerSim *sim);
int            serverSimGetMapDirCount(const ServerSim *sim);
uint8_t        serverSimGetEventCount(const ServerSim *sim);
uint16_t       serverSimGetMapEventCount(const ServerSim *sim);

/* String (char[]) accessors */
const char *serverSimGetMapName(const ServerSim *sim);
const char *serverSimGetBotBrainPath(const ServerSim *sim);
const char *serverSimGetServerMessageLogFile(const ServerSim *sim);

/* Indexed-array accessors (bounds-checked; out-of-range
 * returns NULL for pointer types, false/0 for scalars). */
const LobbyPlayer *serverSimGetLobbyPlayer(const ServerSim *sim, BYTE n);
bool               serverSimIsPlayerConnected(const ServerSim *sim, BYTE n);
uint32_t           serverSimGetLastProcessedInput(const ServerSim *sim, BYTE n);
bool               serverSimIsMapSkipVote(const ServerSim *sim, BYTE n);
uint16_t           serverSimGetPlayerPing(const ServerSim *sim, BYTE n);

/* Array-pointer accessors (return pointer to backing storage). */
char *const     *serverSimGetMapDirFiles(const ServerSim *sim);
const GameEvent *serverSimGetEvents(const ServerSim *sim);
const GameEvent *serverSimGetMapEvents(const ServerSim *sim);

/* Struct-pointer accessors. */
const BalanceProposal *serverSimGetBalanceProposal(const ServerSim *sim);

/* Cross-struct accessor — returns the embedded GameSim. NOT
 * const-qualified: callers of GameSim mutate it freely. */
GameSim *serverSimGetGameSim(ServerSim *sim);

/* ────────────────────────────────────────────────────────────────
 * Lobby Layout A accessors / mutators
 * ──────────────────────────────────────────────────────────────── */

/* Per-bot brain path. Set via PACKET_LOBBY_SET_BOT_BRAIN. Returns NULL
 * (or empty) for slots without a per-bot override; callers should fall
 * back to serverSimGetBotBrainPath in that case. */
const char *serverSimGetBotBrainPathFor(const ServerSim *sim, BYTE slot);
void        serverSimSetBotBrainPathFor(ServerSim *sim, BYTE slot,
                                        const char *path);

/* The brain catalogue (read-only). Populated at serverSimInit by
 * brainListScan; shipped to clients via PACKET_LOBBY_BRAIN_LIST. */
const BrainList *serverSimGetBrainList(const ServerSim *sim);

/* Comma-separated list of admin IPs (from -admins CLI flag). Empty
 * string if no admins are configured. */
const char *serverSimGetAdminIps(const ServerSim *sim);
void        serverSimSetAdminIps(ServerSim *sim, const char *csvIps);

/* openHost — when true, any connected client may issue lobby edit
 * commands (add bots, change settings, etc.). */
bool        serverSimGetOpenHost(const ServerSim *sim);
void        serverSimSetOpenHost(ServerSim *sim, bool v);

/* serverLocks bitmask (LOBBY_LOCK_* in netpacks.h) — set from CLI,
 * never changes after startup. */
uint16_t    serverSimGetServerLocks(const ServerSim *sim);
void        serverSimSetServerLocks(ServerSim *sim, uint16_t locks);

/* autoLockOnGameStart — when true, sets allowNewPlayers=false the
 * moment the lobby transitions out of serverStateLobby. */
bool        serverSimGetAutoLockOnGameStart(const ServerSim *sim);
void        serverSimSetAutoLockOnGameStart(ServerSim *sim, bool v);

/* Cached game-settings mirrors. authoritative state lives in GameSim;
 * these expose the most-recently-broadcast value for lock checks and
 * SETTING_CHG diffs. */
uint8_t     serverSimGetAiPolicy(const ServerSim *sim);
void        serverSimSetAiPolicy(ServerSim *sim, uint8_t v);
bool        serverSimGetTimeLimit(const ServerSim *sim);
void        serverSimSetTimeLimit(ServerSim *sim, bool v);
uint16_t    serverSimGetTimeMinutes(const ServerSim *sim);
void        serverSimSetTimeMinutes(ServerSim *sim, uint16_t v);

/* Per-team metadata (color, name, naming pool, in_use flag). Returns
 * NULL for out-of-range teamId (0 or >= MAX_TANKS). Mutable so the
 * UDP TEAM_META handler can write through it. */
const TeamMetadata *serverSimGetTeamMeta(const ServerSim *sim, BYTE teamId);
TeamMetadata       *serverSimGetTeamMetaMut(ServerSim *sim, BYTE teamId);

/* Per-bot config (difficulty + personality). Mutable for the UDP
 * BOT_CONFIG handler. */
const LobbyBotConfig *serverSimGetBotConfig(const ServerSim *sim, BYTE slot);
LobbyBotConfig       *serverSimGetBotConfigMut(ServerSim *sim, BYTE slot);

/* Per-slot ready / team / isBot mutators (server-controlled state). */
LobbyPlayer *serverSimGetLobbyPlayerMut(ServerSim *sim, BYTE n);

/* Mutable game-length (transition-out-of-lobby and time-limit edits
 * write through this). */
void serverSimSetGameLength(ServerSim *sim, int32_t ticks);

/* Mutable countdown-ticks (transition-into-countdown and auto-unready
 * paths both poke this). */
void serverSimSetCountdownTicks(ServerSim *sim, int32_t ticks);

/* Mutable server-state setter — used by start-game / return-to-lobby
 * paths in transport_udp_server. */
void serverSimSetState(ServerSim *sim, ServerState s);


/* viewPlayer — which player perspective the sim renders from. */
BYTE serverSimGetViewPlayer(const ServerSim *sim);
void serverSimSetViewPlayer(ServerSim *sim, BYTE playerNum);

/* Tutorial-mode flag mirrored on the embedded GameSim. */
bool serverSimIsTutorial(const ServerSim *sim);
void serverSimSetTutorial(ServerSim *sim, bool v);

/* --- Live-sim map / pill / base / start readers ---
 * Server-side perspective of the same map/pill/base/start state
 * the client-side wrappers expose on client_sim.h. Used by
 * binaries that own a ServerSim directly (bg_game, braintest,
 * headless, gym). */

BYTE         serverSimGetMapTerrain(const ServerSim *sim, BYTE x, BYTE y);
bool         serverSimMapIsMine(const ServerSim *sim, BYTE x, BYTE y);
bool         serverSimPillExistsAt(const ServerSim *sim, BYTE x, BYTE y);
BYTE         serverSimPillGetScreenHealthAt(ServerSim *sim, BYTE x, BYTE y);
bool         serverSimBaseExistsAt(const ServerSim *sim, BYTE x, BYTE y);
baseAlliance serverSimBaseGetAllianceAt(ServerSim *sim, BYTE x, BYTE y);
bool         serverSimBaseAmOwnerAt(ServerSim *sim, BYTE player, BYTE x, BYTE y);

BYTE         serverSimGetPillCount(const ServerSim *sim);
BYTE         serverSimGetBaseCount(const ServerSim *sim);
BYTE         serverSimGetStartCount(const ServerSim *sim);

bool         serverSimGetPill(ServerSim *sim, BYTE i,
                              BYTE *x, BYTE *y, BYTE *owner, BYTE *armour,
                              bool *inTank);
bool         serverSimGetBase(ServerSim *sim, BYTE i,
                              BYTE *x, BYTE *y, BYTE *owner);
bool         serverSimGetBaseStats(ServerSim *sim, BYTE i,
                                   BYTE *shells, BYTE *mines, BYTE *armour);
bool         serverSimGetStart(ServerSim *sim, BYTE i,
                               BYTE *x, BYTE *y, BYTE *dir);

/* --- Live-sim render-state readers ---
 * Copy-out snapshots of moving objects (tanks, shells, explosions,
 * LGMs, tank-explosion debris) for map renderers. Callers iterate
 * the returned POD arrays instead of walking sim-internal linked
 * lists or indexing the tank/lgm arrays directly. */

typedef struct TankRenderInfo {
    WORLD world_x;
    WORLD world_y;
    BYTE  dir;       /* 0-15, already converted from TURNTYPE */
    bool  on_boat;
    bool  alive;     /* false when slot is empty or tank is in death-wait */
} TankRenderInfo;

/* Populate *out from sim->tanks[i]. Returns false (without touching
 * *out) if i >= MAX_TANKS or the slot is empty. When the slot is
 * occupied but the tank is in death-wait, returns true with
 * out->alive = false. */
bool serverSimGetTankRender(ServerSim *sim, BYTE i, TankRenderInfo *out);

/* Tank alliance from selfPlayer's perspective. Independent of
 * sim->sim.viewPlayer so callers don't need to mutate that global
 * just to colour tanks for a different camera. */
tankAlliance serverSimGetTankAllianceFor(ServerSim *sim,
                                         BYTE selfPlayer,
                                         BYTE tankNum);

typedef struct ShellRender {
    WORLD    x;
    WORLD    y;
    TURNTYPE angle;
} ShellRender;

/* Copy live (non-dead) shells into out[0..min(n,cap)-1]; return n. */
int serverSimGetShellSnapshot(ServerSim *sim, ShellRender out[], int cap);

typedef struct ExplosionRender {
    BYTE mx;
    BYTE my;
    BYTE px;
    BYTE py;
    BYTE length;
} ExplosionRender;

int serverSimGetExplosionSnapshot(ServerSim *sim, ExplosionRender out[], int cap);

typedef struct LgmRender {
    WORLD x;
    WORLD y;
    BYTE  frame;
} LgmRender;

/* Returns false (without touching *out) if i >= MAX_TANKS, the LGM
 * slot is empty, or the LGM is in-tank / dead. */
bool serverSimGetLgmRender(ServerSim *sim, BYTE i, LgmRender *out);

typedef struct TankExplosionRender {
    WORLD x;
    WORLD y;
} TankExplosionRender;

int serverSimGetTankExplosionSnapshot(ServerSim *sim,
                                      TankExplosionRender out[], int cap);

/* Visible mine at (x, y)? Combines map-bit + visibility list. */
bool serverSimMineExistsAt(ServerSim *sim, BYTE x, BYTE y);

#endif /* SERVER_SIM_H */
