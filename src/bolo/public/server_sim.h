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
#include "client_command.h"    /* ClientCommand / CmdResult — serverSimApplyCommand */
#include "attribution_track.h" /* AttrSlotIdentity — track accessors below */
#include "view_policy.h"       /* ViewPolicy / ViewCategory — view-policy accessors below */
#include "server_voice_mode.h" /* ServerVoiceMode — voice-mode accessors below */

/* MapGenConfig is defined in src/bolo/public/mapgen.h.
 * Forward-declared here so the public server_sim header doesn't
 * pull the mapgen header into every translation unit that
 * includes server_sim.h. Callers that build a config and invoke
 * serverSimEnableRandomMap / serverSimCreateRandomMap include
 * mapgen.h directly. */
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

/* Forward declarations for internal types returned by braintest
 * accessors as opaque pointers. The structs themselves live in
 * the internal tier (BrainPathfinder) or the public brain_overlay.h
 * (OverlayCmdBuffer). */
#ifndef BRAINPATHFINDER_TYPEDEF
#define BRAINPATHFINDER_TYPEDEF
typedef struct BrainPathfinder  BrainPathfinder;
#endif
#ifndef OVERLAYCMDBUFFER_TYPEDEF
#define OVERLAYCMDBUFFER_TYPEDEF
typedef struct OverlayCmdBuffer OverlayCmdBuffer;
#endif

typedef int SubscriberHandle;
#define SUBSCRIBER_HANDLE_INVALID (-1)

/* Threading: serverSimPublishControl, serverSimRegisterSubscriber, and
 * serverSimUnregisterSubscriber are not synchronized. All callers must run on
 * the same thread (today: the main game-tick thread, which drives both
 * transport_local and transport_udp_*). If a subscriber's deliver callback ever
 * runs on a worker thread, or publish is called from outside the tick thread,
 * add a mutex.
 *
 * Registration order: a player's own subscriber (bot via botManagerAddBot,
 * local human via gamefront, per-client UDP via the join handler) must
 * register AFTER serverSimAddPlayer has published CTRL_PLAYER_JOIN for that
 * player, so the new player's subscriber doesn't receive its own join. The
 * dispatcher's self-skip is the second line of defense; registration order
 * is the primary one. */
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
  uint8_t startIdx;    /* reserved map start, 1-based; 0xFF = none */
} LobbyPlayer;

/* Per-team metadata — used by the Layout A lobby UI for color tinting,
 * editable team names, per-team bot naming pools and the team's start
 * side. The `in_use` flag is 0 for any team number that has no metadata
 * yet (renders with default name "Team N" and a fallback color). Team
 * membership lives in LobbyPlayer.teamNumber. */
#ifndef LOBBY_TEAM_NAME_LEN
#define LOBBY_TEAM_NAME_LEN 32
#endif
typedef struct {
  uint8_t in_use;       /* 0 = defaults; 1 = customised */
  uint8_t color;        /* index into client-side kTeamColors[] */
  uint8_t namingPool;   /* index into client-side bot pool table */
  uint8_t startSide;    /* START_SIDE_* choice (start_sides.h); START_SIDE_ANY = no side */
  char    name[LOBBY_TEAM_NAME_LEN];
} TeamMetadata;

/* Bot difficulty in the DEFAULT mode, as it has always been carried on the
 * wire (CTRL_/PACKET_LOBBY_BOT_CONFIG) and shown in the lobby. The values
 * are frozen: 1 was labelled "normal" before it was labelled "Medium".
 * botDifficultyName (bot_manager.h) turns one of these into a word.
 *
 * Since modes arrived the byte is really an INDEX into the selected mode's
 * level list (brain_list.h). The default mode's levels are easy / medium /
 * hard in that order, so these three constants still name the right slots
 * and every pre-mode caller keeps working unchanged. */
#define BOT_DIFFICULTY_EASY    0
#define BOT_DIFFICULTY_MEDIUM  1
#define BOT_DIFFICULTY_HARD    2
#define BOT_DIFFICULTY_MAX     2

/* Per-bot config — extends bot identity with mode + difficulty +
 * personality the Layout A AiConfig sub-panel writes. mode and difficulty
 * reach the brain as BRAIN_INIT_ARG tokens at bot-brain creation
 * (bot_manager.c); the brain stores them and behaves the same at every
 * setting for now. personality is still unconsumed. Indexed by slot
 * (matches bot's playerNum). */
typedef struct {
  uint8_t mode;         /* index into the brain's mode list; 0 = default */
  uint8_t difficulty;   /* index into that mode's level list (0=easy...) */
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
  bool includeBots;               /* TRUE if bots were sent to WBN and end up in the proposal.
                                   * FALSE means bots are kicked when the proposal is applied. */
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
 *  mapName     - Display name for the map (e.g. "Everard Island").
 *                Stored on the sim so clients can report it in UI
 *                and Steam rich presence. Pass "" if unknown.
 *  game        - Game type (open/tournament/strict)
 *  hiddenMines - Are hidden mines allowed
 *  startDelay  - Game start delay (in ticks)
 *  gameLen     - Game length in ticks (-1 = unlimited)
 *********************************************************/
ServerSim *serverSimCreateCompressed(BYTE *buff, int buffLen, const char *mapName, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen);

/*********************************************************
 *NAME:          serverSimReloadMap
 *PURPOSE:
 *  In-place swap the current map for the one at mapFileName.
 *  Reads the new .map file, replaces the running ServerSim's
 *  mp / pb / bs / ss in place, refreshes the cached compressed
 *  map blob (used for client downloads and inter-round resets),
 *  updates the map name, and clears any stale player-ready
 *  states. Only valid while the sim is in serverStateLobby —
 *  swapping mid-game would desync clients.
 *
 *  On failure (file unreadable, parse error, wrong state) the
 *  previous map is preserved and the function returns false.
 *
 *ARGUMENTS:
 *  sim         - Running ServerSim (must be in lobby state)
 *  mapFileName - Path to the .map file to load
 *RETURNS:      true on successful swap, false otherwise.
 *********************************************************/
bool serverSimReloadMap(ServerSim *sim, const char *mapFileName);

/* serverSimReloadCompressedInMemory: moved to
 * internal/server_sim_lifecycle.h — applied by the UDP
 * MAP_UPLOAD_DONE handler and the SP-host local-transport branch
 * of clientSimNetSendLobbyMapUploadBytes. */

/* Procedural-map preview variant. Regenerates the map from the
 * given MapGenConfig, stashes the prior committed map for Cancel
 * roll-back (same mechanism as serverSimReloadMap), refreshes
 * cachedMapData, and sets mapName to "rand_<seed>". Caller is
 * responsible for whatever broadcast / publish follows. Only valid
 * in lobby state. */
bool serverSimReloadRandomMap(ServerSim *sim,
                               const struct MapGenConfig *cfg);

/* Lobby preview-map cycle. serverSimReloadMap stashes the prior
 * committed map under the hood; these two close the loop:
 *   - serverSimHasPreviewMap: true if a preview is pending (i.e. a
 *     stash exists), false otherwise.
 *   - serverSimRevertPreview: load the stashed map back into the
 *     sim, refresh cachedMapData, drop the stash. Returns false if
 *     there was nothing to revert or the rollback failed.
 *   - serverSimCommitPreview: free the stash; the sim already
 *     reflects the previewed map so no reload is needed.
 * Caller is responsible for triggering whatever broadcast / re-
 * download the transport layer needs after a revert (the sim mp /
 * pb / bs / ss have changed). */
bool serverSimHasPreviewMap(const ServerSim *sim);
bool serverSimRevertPreview(ServerSim *sim);
void serverSimCommitPreview(ServerSim *sim);

/* The map name that was committed BEFORE the current preview cycle
 * started — i.e. the name to which a Cancel would roll back. Returns
 * an empty string when no preview is in flight. Lifetime: valid
 * until the next ServerSim mutation. */
const char *serverSimGetPreviousMapName(const ServerSim *sim);

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
 *NAME:          serverSimSetPlayerCountry
 *PURPOSE:
 *  Sets the ISO 3166-1 alpha-2 country code on a connected
 *  player slot. Intended for the in-process SP / tutorial
 *  path, where no GeoIP lookup runs and the local client
 *  supplies its own resolved country (e.g. from
 *  winbolonetGetCountryCode). Network-joined slots receive
 *  their country from the server's GeoIP lookup and should
 *  not be re-set through this accessor.
 *
 *  Input is validated: NULL, length != 2, or any non-alpha
 *  byte is ignored (the previous value stays). A valid
 *  two-character code is stored uppercased.
 *
 *ARGUMENTS:
 *  sim       - Pointer to the ServerSim
 *  playerNum - Player slot (0..MAX_TANKS-1)
 *  cc        - 2-char ISO 3166-1 alpha-2 country code (any case)
 *********************************************************/
void serverSimSetPlayerCountry(ServerSim *sim, BYTE playerNum, const char *cc);

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
                                 * name (e.g. "GoalHunter" for
                                 * brains/GoalHunter/init.lua) */
    double   lastThinkMs;       /* most recent brain tick */
    double   maxThinkMs;        /* peak brain tick this game */
    double   targetMs;          /* target the next tick will use */
    uint32_t overrunCount;      /* overruns this game */
    uint32_t thinkCount;        /* total thinks this game
                                 * (overrun-rate denominator) */
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
    double   maxThinkMs;        /* peak per-bot think across the pool this
                                 * game (ms) */
    uint32_t totalOverruns;     /* sum of overrunCount across bots (this game) */
    uint32_t totalThinks;       /* sum of thinkCount across bots this game
                                 * (overrun-rate denominator) */
} BotPoolStats;

/* Max candidate count for BrainGoalInfo. */
#define BRAIN_GOAL_MAX_CANDIDATES 32

/* Goal info for debug viewer (BrainTest). Read by
 * serverSimGetBotGoalInfo from the brain's Lua state each frame. */
typedef struct {
    char kind[32];          /* goal kind string (e.g. "attack_pill") */
    int  mx, my;            /* goal target map coords */
    int  target_id;         /* item index (base/pill number, -1 if none) */
    char substate[32];      /* attack substate (or empty) */

    /* Last evaluated candidate pool from cost competition */
    struct {
        char  desc[120];
        float cost;
        bool  winner;
        float phase_weight;
    } candidates[BRAIN_GOAL_MAX_CANDIDATES];
    int num_candidates;
} BrainGoalInfo;

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

/* Tear down the process-wide worker pool. Call once at
 * shutdown, after every ServerSim that may dispatch to the
 * pool has been destroyed. Idempotent. */
void serverSimBotPoolDestroy(void);

/* Request a live resize of the worker pool. Stashes the
 * value as pending; the next serverSimBotTick applies it. */
void serverSimRequestBotThreads(ServerSim *sim, int total_runners);

/* Current total runner count (workers + producer). */
int  serverSimGetBotThreads(ServerSim *sim);

/* Pending thread-count request, or -1 if no resize pending. */
int  serverSimGetPendingBotThreads(ServerSim *sim);

/* === Default debug mode for new bots === */
/* Set the BRAIN_DEBUG_MODE value bots inherit at creation.
 * Affects bots created AFTER this call. */
void serverSimSetBotDefaultDebugMode(ServerSim *sim, bool enabled);

/* === Pre-think hook === */
/* Register a callback invoked just before each bot's brain
 * runs (with the bot's playerNum) and again with -1 after.
 * Pass NULL to clear. */
void serverSimSetBotPreThinkHook(ServerSim *sim, void (*hook)(int playerNum));

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
/* Mark a slot whose in-process consumer reads the map square a sound was
 * raised at — the gym agent, the headless brain harness. A human client is
 * sent a near/far tier and a bearing in place of the square; a bot-manager
 * bot keeps the square without being marked. Only the process that owns the
 * ServerSim can call this, so the wire cannot claim it. Cleared when the slot
 * is joined or freed. */
void   serverSimSetSoundSquares(ServerSim *sim, BYTE playerNum, bool keep);
/* Set or clear muterSlot's smart-ping mute bit for targetPlayer. Both indices
 * are bounds-checked; the mute is private to muterSlot and independent of the
 * voice/chat mute. Called from the CMD_PLAYER_PING_MUTE dispatch arm. */
void   serverSimSetPingMute(ServerSim *sim, BYTE muterSlot, BYTE targetPlayer,
                            bool muted);
double serverSimGetBotLastThinkMs(ServerSim *sim, BYTE playerNum);
bool   serverSimGetBotInfo(ServerSim *sim, BYTE playerNum, BotInfo *out);
void   serverSimGetBotPoolStats(ServerSim *sim, BotPoolStats *out);
bool   serverSimToggleAllBrainDebugMode(ServerSim *sim);
int    serverSimGetActiveBotCount(ServerSim *sim);

/* === Bot brain introspection (BrainTest debug viewer) === */
BrainPathfinder  *serverSimGetBotBrainPathfinder(ServerSim *sim, BYTE playerNum);
OverlayCmdBuffer *serverSimGetBotOverlayCmds(ServerSim *sim, BYTE playerNum);
bool              serverSimGetBotGoalInfo(ServerSim *sim, BYTE playerNum,
                                          BrainGoalInfo *out);
bool              serverSimBotExecLua(ServerSim *sim, BYTE playerNum,
                                      const char *src);
char             *serverSimBotEvalLuaString(ServerSim *sim, BYTE playerNum,
                                            const char *src);

/* serverSimSetTeam: moved to internal/server_sim_lifecycle.h —
 * applied by UDP PACKET_LOBBY_TEAM_SET / PACKET_LOBBY_ADD_BOT
 * handlers and the SP-host local-transport branch of
 * clientSimNetSendTeamSet. */
/* serverSimSetBotAiType: moved to internal/server_sim_lifecycle.h
 * — applied by serverInstanceStartup from cfg.botAiType and by
 * the shared serverSimApplyLobbySetting helper. */

/*********************************************************
 *NAME:          serverSimSetEmptyResetEnabled
 *PURPOSE:
 *  Enables or disables the empty-server auto-reset
 *  feature.
 *********************************************************/
void serverSimSetEmptyResetEnabled(ServerSim *sim, bool enabled);

/* serverSimSetLobbyEnabled: moved to internal/server_sim_lifecycle.h
 * — applied by serverInstanceStartup (skipLobby / lobbyEnabled
 * branches). */

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
 *NAME:          serverSimInstallMapDirList
 *PURPOSE:
 *  Installs the validated map rotation list. Takes
 *  ownership of `files` (the array and each strdup'd
 *  entry inside it). Caller must not free these
 *  afterward. `dirPath` is captured as the server-side
 *  map root (mirroring serverSimMapDirBuild) so the lobby
 *  map chooser, SET_MAP resolution and rotation all read
 *  from the same directory; pass NULL to leave the root
 *  unchanged.
 *********************************************************/
void serverSimInstallMapDirList(ServerSim *sim,
                                char **files, int count,
                                const char *dirPath);

/*********************************************************
 *NAME:          serverSimSetAutoCloseOnEmpty
 *PURPOSE:
 *  Configures whether the server should close when all
 *  players leave.
 *********************************************************/
void serverSimSetAutoCloseOnEmpty(ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          serverSimSetMapRotate
 *PURPOSE:
 *  Enables no-lobby map-rotation mode. In this mode a win
 *  or an empty server boots every player, picks the next
 *  map from -mapdir and restarts a fresh running round
 *  instead of quitting. Implies no-lobby; the server only
 *  stops on Ctrl-C / the quit console command.
 *********************************************************/
void serverSimSetMapRotate(ServerSim *sim, bool enabled);

/*********************************************************
 *NAME:          serverSimIsMapRotateEnabled
 *PURPOSE:
 *  Returns whether no-lobby map-rotation mode is enabled.
 *********************************************************/
bool serverSimIsMapRotateEnabled(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimIsTerminalGameOver
 *PURPOSE:
 *  Returns whether the current game-over state should shut
 *  the dedicated server down. False for lobby servers (they
 *  return to the lobby) and map-rotation servers (they boot
 *  everyone and rotate); true only for a plain no-lobby
 *  server. The command loop polls this to decide whether to
 *  exit, so it must never treat a transient rotation
 *  game-over as terminal.
 *********************************************************/
bool serverSimIsTerminalGameOver(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimMapRotateRound
 *PURPOSE:
 *  Sim-core half of a map-rotation round restart: opens the
 *  WBN session-rotation window, picks the next map and starts
 *  a fresh empty running round. The caller (the server
 *  lifecycle) boots all clients first and runs the WBN
 *  session rotation / round-log upload around this call.
 *********************************************************/
void serverSimMapRotateRound(ServerSim *sim);

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
 *NAME:          serverSimSetGameTickLimit
 *PURPOSE:
 *  Ends the current running game after the given number of
 *  running-state ticks by transitioning to GAME_OVER. 0 = no
 *  limit. Unlike serverSimSetTickLimit (whose console message
 *  reads "Exiting" and which in -nolobby mode terminates the
 *  process via the main loop's GAME_OVER exit gate), this
 *  limit is intended purely as a game-end signal — the lobby
 *  return-to-lobby cycle proceeds normally in lobby mode.
 *  Resets after firing.
 *********************************************************/
void serverSimSetGameTickLimit(ServerSim *sim, int32_t ticks);

/*********************************************************
 *NAME:          serverSimSetSnapshotHook
 *PURPOSE:
 *  Registers a periodic observer that is invoked every
 *  intervalTicks running ticks (the same ticks
 *  serverSimSetTickLimit counts), from inside the sim step
 *  before that step does any work — so the callback always
 *  sees fully settled state and never a half-applied tick.
 *  It runs on whatever thread drives the sim (the dedicated
 *  server's game timer), under that driver's tick lock.
 *
 *  The callback must only read the sim. intervalTicks of 0,
 *  or a NULL cb, disables the hook. Follows the same
 *  register-a-callback pattern as mapSetChangeCallback.
 *
 *  Used by WinBoloDS -snapjson/-snapinterval to build a
 *  JSONL time series of global game state.
 *********************************************************/
void serverSimSetSnapshotHook(ServerSim *sim, void (*cb)(ServerSim *sim),
                              int32_t intervalTicks);

/*********************************************************
 *NAME:          serverSimGetPlayerKills
 *PURPOSE:
 *  The server's authoritative kill count for a slot in the
 *  current round (roundStats[slot].kills, credited in the
 *  shell-death path).
 *
 *  Not the same number as TankInfo.kills: that reads
 *  tank->numKills, which only tankAddKill writes, and
 *  tankAddKill is called solely from the client snapshot
 *  path for the local player. On a dedicated server nothing
 *  ever calls it, so TankInfo.kills is permanently 0 —
 *  server-side readers want this instead. Returns 0 for an
 *  out-of-range slot.
 *********************************************************/
uint16_t serverSimGetPlayerKills(const ServerSim *sim, BYTE slot);

/*********************************************************
 *NAME:          serverSimGetDeathCauses
 *PURPOSE:
 *  Copies the slot's per-cause death tally into out,
 *  indexed by DEATH_CAUSE_* (out must hold DEATH_CAUSE_NUM
 *  entries). Zeroes out and returns false for an
 *  out-of-range slot.
 *
 *  DEATH_CAUSE_DROWNED_UNFORCED is a sub-classification of
 *  a drowning, not a sixth independent cause, so the sum
 *  that equals the slot's death count is
 *  OTHER + DROWNED + DROWNED_UNFORCED + SHELL_TANK +
 *  SHELL_PILL + MINE -- i.e. every entry, with the two
 *  drowning slots being mutually exclusive.
 *
 *  Observation only: the counters are written next to
 *  numDeaths++ in tankDeath and read nowhere inside the sim.
 *********************************************************/
bool serverSimGetDeathCauses(const ServerSim *sim, BYTE slot,
                             uint32_t out[DEATH_CAUSE_NUM]);

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
 *  sim       - Pointer to the ServerSim
 *  output    - Buffer to receive compressed data
 *  outputCap - Size of that buffer in bytes. Nothing is written
 *              past it; a map that does not fit returns 0.
 *********************************************************/
int serverSimGetCompressedMap(ServerSim *sim, BYTE *output, int outputCap);

/* Refresh a ClientSim's map/pill/base/start state from this server's
 * current compressed map cache. Preserves player table, lobby slots,
 * callbacks, and the existing tank — only the terrain structures get
 * replaced. Used at SP game-start to sync the host's ClientSim to a
 * lobby map change. Returns false if the compressed map can't be
 * read or applied. */
bool serverSimReloadClientMap(ServerSim *sim, ClientSim *cs);

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
 *NAME:          serverSimGetNumHumans
 *PURPOSE:
 *  Returns the number of connected non-bot (human) players.
 *********************************************************/
BYTE serverSimGetNumHumans(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimRefreshWbnLobbyInfo
 *PURPOSE:
 *  Rebuilds the WinBolo.net lobby snapshot from current sim
 *  state and stashes it via winbolonetSetLobbyInfo so the
 *  register/update/lobby_update bodies pick up the latest
 *  map, settings and human/bot counts.
 *********************************************************/
void serverSimRefreshWbnLobbyInfo(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimWbnLobbyUpdate
 *PURPOSE:
 *  Sends a server/lobby_update snapshot. Rate-limited to one
 *  send per WBN_LOBBY_UPDATE_INTERVAL seconds unless force is
 *  TRUE (used right before the lobby countdown starts); a
 *  throttled call instead marks the snapshot dirty for the
 *  next tick.
 *********************************************************/
void serverSimWbnLobbyUpdate(ServerSim *sim, bool force);

/*********************************************************
 *NAME:          serverSimWbnLobbyTick
 *PURPOSE:
 *  Flushes a deferred (dirty) lobby_update if the rate-limit
 *  window has passed. Called from the periodic WBN tick.
 *********************************************************/
void serverSimWbnLobbyTick(ServerSim *sim);

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

/* serverSimStartGame: moved to internal/server_sim_lifecycle.h —
 * applied by serverInstanceStartup (skipLobby branch) and by the
 * UDP server's countdown-finished handler. */

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
 *NAME:          serverSimSetNewswireMute
 *PURPOSE:
 *  Silence (or restore) the ENGINE-GENERATED newswire for
 *  this server and every client attached to it. While muted,
 *  clients drop event newswire lines (player quit, base and
 *  pill captures, builder lost, name handover) and the server
 *  drops its own "X has joined." / "X has left." broadcasts.
 *  Server text published on purpose (game.message, vote and
 *  wave banners) and player chat are NOT affected.
 *
 *  Publishes CTRL_NEWSWIRE_MUTE only on an actual change, and
 *  the join sync replays the current state so a client that
 *  connects mid-window starts muted as well.
 *
 *ARGUMENTS:
 *  sim   - the server simulation
 *  muted - TRUE to silence, FALSE to restore
 *********************************************************/
void serverSimSetNewswireMute(ServerSim *sim, bool muted);

/*********************************************************
 *NAME:          serverSimGetNewswireMuted
 *PURPOSE:
 *  TRUE while serverSimSetNewswireMute has the engine
 *  newswire silenced.
 *********************************************************/
bool serverSimGetNewswireMuted(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimReassignStarts
 *PURPOSE:
 *  Re-runs the batch start placement over the currently
 *  connected players and re-creates their tanks at the new
 *  positions. No-op unless the sim is running.
 *
 *  TECH DEBT: the twin of serverSimReapplyTeamAlliances
 *  above, and there for the same reason. -nolobby (and the
 *  SP-host flow) call serverSimStartGame before the bots
 *  have been added and their team numbers set, so the batch
 *  pass inside it sees an empty roster; every bot added
 *  afterwards then falls back to the team-blind per-player
 *  pick, which put two rivals two squares apart in the same
 *  corner. Call this once the roster and teams are final.
 *  It goes away with the alliance pass when players are
 *  added before serverSimStartGame is called.
 *********************************************************/
void serverSimReassignStarts(ServerSim *sim);

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
 * In-game vote system (back-to-lobby + surrender).
 *  - serverSimGameVoteTick is called every server frame from the
 *    transport layer. It handles per-second heartbeats, the 60s
 *    timeout, and the post-pass 3/2/1 chat countdown.
 *  - serverSimGameVoteToggle (declared in server_sim_internal.h)
 *    handles incoming PACKET_GAME_VOTE_TOGGLE. Callers are the wire
 *    handler in transport_udp_server.c and the in-process handler in
 *    transport_local.c — both sim co-owners; clients reach it through
 *    clientSimNetSendGameVoteToggle.
 *  - serverSimGameVoteResetAll clears all in-flight votes (called on
 *    game start / lobby return).
 *  - serverSimCountActiveTeams returns true iff there is exactly one
 *    active team count value, populated into outActiveTeamCount.
 *********************************************************/
void serverSimGameVoteTick(ServerSim *sim, uint64_t nowMs);
void serverSimGameVoteResetAll(ServerSim *sim);
bool serverSimGameVoteIsRunning(const ServerSim *sim, uint8_t kind);
uint8_t serverSimCountActiveTeams(const ServerSim *sim);

/* Snapshot of a single game vote slot, suitable for wire serialisation
 * by the transport layer. Returns false if `kind` is unknown. */
typedef struct {
    uint8_t  kind;
    uint8_t  active;           /* GAME_VOTE_ACTIVE_* */
    uint8_t  triggerSrc;       /* GAME_VOTE_TRIGGER_* */
    uint8_t  teamId;
    uint8_t  threshold;        /* yes-count needed to pass */
    uint8_t  yesCount;
    uint8_t  noCount;
    uint8_t  eligibleCount;    /* total eligible voters */
    uint8_t  secondsRemaining; /* 0..60 */
    uint16_t votes;            /* bitmask of slots that voted yes */
} ServerGameVoteSnapshot;

bool serverSimGetGameVoteSnapshot(const ServerSim *sim, uint8_t kind,
                                  ServerGameVoteSnapshot *out);

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
 *NAME:          serverSimSendWbnSurrenderWinEvents
 *PURPOSE:
 *  Sends WINBOLO_NET_EVENT_WIN for each player on the side
 *  that did NOT surrender (i.e. on a real team other than
 *  surrenderTeam). Used when a surrender vote ends the round,
 *  where the base-ownership sweep never fires. No-op when
 *  surrenderTeam is 0.
 *********************************************************/
void serverSimSendWbnSurrenderWinEvents(ServerSim *sim, uint8_t surrenderTeam);

/*********************************************************
 *NAME:          serverSimBuildSurrenderWinMessage
 *PURPOSE:
 *  Builds a "Game Won! Winners: ..." message listing every
 *  player on the side that did NOT surrender. Returns TRUE if
 *  any winner was listed, FALSE (message "Game over!") if not.
 *********************************************************/
bool serverSimBuildSurrenderWinMessage(ServerSim *sim, uint8_t surrenderTeam,
                                       char *buf, size_t bufSize);

/*********************************************************
 *NAME:          serverSimResolveGameOver
 *PURPOSE:
 *  Resolves game-outcome policy at the running->gameOver
 *  transition: builds the win/exit message into the sim's
 *  pendingWinMessage and sends the matching WBN win events,
 *  branching on the sim's RETURN_REASON_* for the round that just
 *  ended. No-op when the sim has no lobby.
 *  Called once from the host's game-over transition so every
 *  host (dedicated + in-process) runs identical crediting.
 *********************************************************/
void serverSimResolveGameOver(ServerSim *sim);

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
 *NAME:          serverSimReplayLobbyChat
 *PURPOSE:
 *  Re-deliver the current-session lobby-chat catch-up
 *  buffer (broadcast player chat + spectator chat, oldest
 *  first) through the caller's deliver callback. Used at
 *  the spectator delayed->live drain-flip so a returning
 *  spectator sees the lobby chat sent while it was still
 *  finishing the delayed game. Call after re-registering
 *  the subscriber so the lobby phase is set first. Not
 *  invoked on a fresh accept or player join.
 *********************************************************/
void serverSimReplayLobbyChat(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx);

typedef void (*SpectatorRosterEnumFn)(
    void *enumCtx,
    void (*deliver)(void *, const struct ControlEvent *),
    void *deliverCtx);

/*********************************************************
 *NAME:          serverSimSetSpectatorRosterEnumerator
 *PURPOSE:
 *  Register a callback the sim invokes during sync-replay
 *  to emit one CTRL_SPECTATOR_SLOT per connected spectator.
 *  The roster lives in the transport layer; this lets the
 *  sim's replay (and the ring keyframe control snapshot,
 *  which reuses the same producer) carry it without the sim
 *  owning spectator state. Pass fn=NULL to clear.
 *********************************************************/
void serverSimSetSpectatorRosterEnumerator(ServerSim *sim,
                                           SpectatorRosterEnumFn fn,
                                           void *enumCtx);

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
                                     uint8_t teamSize,
                                     const uint8_t *botSlots,
                                     uint8_t numBotSlots);

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
 *NAME:          serverSimReceiveChat
 *PURPOSE:
 *  Authoritative entry for any chat the server accepts,
 *  regardless of transport. Per docs/ARCHITECTURE.md the
 *  same event must reach in-process subscribers AND any
 *  UDP-connected clients — this function publishes
 *  CTRL_CHAT which fans out via both paths (in-process
 *  CTRL_CHAT handler in client_sim_control.c +
 *  per-client codec encoder in transport_udp_server.c).
 *
 *  Called from:
 *    - transport_udp_server.c PACKET_CHAT_MESSAGE handler
 *      after decoding a wire packet from a remote client.
 *    - bot_manager.c botChatSendCallback after a bot's
 *      brain.sendmessage flows through playersSendAiMessage.
 *
 *ARGUMENTS:
 *  sim          - The server sim
 *  fromPlayer   - Sender slot (must be < MAX_TANKS)
 *  destPlayer   - Recipient slot, or 0xFF for broadcast
 *  body         - Raw chat bytes (no Pascal-length prefix)
 *  bodyLen      - Length of body (clamped to
 *                  PACKET_MAX_CHAT_MESSAGE)
 *********************************************************/
void serverSimReceiveChat(ServerSim *sim, BYTE fromPlayer, BYTE destPlayer,
                          const void *body, size_t bodyLen);

/*********************************************************
 *NAME:          serverSimReceiveSpectatorChat
 *PURPOSE:
 *  Authoritative entry for a lobby chat line from a
 *  tankless spectator. A spectator owns no player slot,
 *  so the message is stamped with its specIdx and
 *  published as CTRL_SPECTATOR_CHAT, which fans to
 *  players (bus) and spectators (deliver allowlist), and
 *  is recorded into the .wbv as log_SpectatorChat.
 *
 *ARGUMENTS:
 *  sim          - The server sim
 *  specIdx      - Sender spectator slot (< MAX_SPECTATORS)
 *  body         - Raw chat bytes (no Pascal-length prefix)
 *  bodyLen      - Length of body (clamped to
 *                  PACKET_MAX_CHAT_MESSAGE)
 *********************************************************/
void serverSimReceiveSpectatorChat(ServerSim *sim, uint8_t specIdx,
                                   const void *body, size_t bodyLen);

/*********************************************************
 *NAME:          serverSimApplyCommand
 *PURPOSE:
 *  Single apply path for client→server commands. Switches
 *  on cmd->type; each command arm runs its own authority
 *  check, validation, mutation, and downstream publishes
 *  (the dispatcher itself is just the switch). UDP server
 *  decode handlers and local/SP-host clients both reach the
 *  same arm, so the two transports cannot disagree on apply.
 *
 *  Mutex ownership: the caller holds threadsMutex. The
 *  dispatcher does not acquire it. UDP server callers are
 *  already on the server thread and hold it. Local/SP-host
 *  callers in client_net.c, and bots running on a
 *  transportLocalCreatePassive transport, must wrap the
 *  call in threadsWaitForMutex() / threadsReleaseMutex().
 *  Debug builds assert this via threadsCurrentlyHoldsMutex.
 *
 *  Sender attribution: senderSlot is the slot the command
 *  is attributed to, unconditionally. UDP path resolves it
 *  via serverFindClient(fromAddr) → clientIdx. Local path
 *  uses clientSimGetMyPlayerNum(cs); each local transport
 *  is per-slot by construction so the value is unambiguous.
 *
 *  State guards live in each command arm, not in a shared
 *  prelude — most lobby commands require lobby phase, game-
 *  time commands have their own phase guards, and ranked-
 *  only commands gate on serverSimGetRanked.
 *********************************************************/
CmdResult serverSimApplyCommand(ServerSim *sim, int senderSlot,
                                const ClientCommand *cmd);

/*********************************************************
 *NAME:          serverSimAcceptAlliance
 *               serverSimLeaveAlliance
 *               serverSimSetPlayerName
 *PURPOSE:
 *  Apply an authoritative state change on the server and
 *  publish the matching ControlEvent in one call. Wraps the
 *  inline mutate-then-publish pair used by the UDP server's
 *  PACKET_ALLIANCE_ACCEPT / PACKET_ALLIANCE_LEAVE /
 *  PACKET_NAME_CHANGE handlers so callers outside src/bolo/
 *  do not have to reach into the players sub-system directly.
 *********************************************************/
void serverSimAcceptAlliance(ServerSim *sim, BYTE accepter, BYTE newMember);
void serverSimLeaveAlliance(ServerSim *sim, BYTE playerNum);
void serverSimSetPlayerName(ServerSim *sim, BYTE playerNum, const char *name);

/*********************************************************
 *NAME:          serverSimFillGamePhaseEvent
 *               serverSimFillLobbySettingsEvent
 *               serverSimFillLobbySlotEvent
 *               serverSimFillPlayerJoinEvent
 *               serverSimFillPlayerLeaveEvent
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
void serverSimFillPlayerLeaveEvent(ServerSim *sim, BYTE i, struct ControlEvent *evt);

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
bool          serverSimIsAcceptingJoins(const ServerSim *sim);
/* Cap on join slots; 0 in the struct field falls back to MAX_TANKS. */
BYTE          serverSimGetMaxPlayers(const ServerSim *sim);

/* Recompute the effective join cap: the operator's configured limit
 * further clamped by the active scenario's max_players (stricter
 * wins). Called at instance startup and after every map change. */
void          serverSimApplyScenarioPlayerCap(ServerSim *sim);

/* (Re)seed lobby Team 2 with the scenario's enemy bots (the script's
 * enemy_bots(game) hook decides how many): real, pool-named lobby bots
 * in the high slots that the host edits with the normal team controls.
 * Clears previous seeds first; no-op outside a scenario lobby. */
void          serverSimSeedScenarioEnemyTeam(ServerSim *sim);

/* The scenario lobby takeover, applied at map COMMIT time only (Choose
 * Map, rotation's instant pick, instance startup) — never on a mere
 * chooser preview, which must not wipe teams or override the host's
 * game-type choice: auto-selects the Scenario game type (bots
 * force-allowed; ranked exempt; committing a plain map downgrades a
 * lingering Scenario type to Open), seeds/clears the enemy team, and
 * republishes the settings block. */
void          serverSimApplyScenarioCommit(ServerSim *sim);
/* Drop the sim's scripted scenario (if any): the VM is destroyed and no
 * further hooks run. For hosts that want a scenario map's terrain
 * without its script — e.g. the menu-background splash sim. */
void          serverSimDropScenario(ServerSim *sim);
/* Cap on AI bots addable in the lobby; 0 = no cap. */
BYTE          serverSimGetMaxBots(const ServerSim *sim);
/* Cap on spectator connections; 0 = spectating disabled (no MAX_TANKS fallback). */
BYTE          serverSimGetMaxSpectators(const ServerSim *sim);
/* Set the spectator cap at runtime (0 disables spectating). */
void          serverSimSetMaxSpectators(ServerSim *sim, BYTE n);
/* Spectator view delay in ticks (50 ticks/s); 0 = live. */
uint32_t      serverSimGetSpecDelayTicks(const ServerSim *sim);
/* Set the spectator view delay in ticks at runtime (0 = live). The live ring's
 * retention is sized from this at creation, so set it before
 * serverInstanceCreateSpectatorRing if a non-default delay must be retained. */
void          serverSimSetSpecDelayTicks(ServerSim *sim, uint32_t ticks);
/* Number of connected slots flagged as lobby bots — the value the
 * -maxbots cap is compared against. */
BYTE          serverSimGetLobbyBotCount(const ServerSim *sim);
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

/* Per-round attribution track (see attribution_track.h). Valid from the first
 * appended record until the next serverSimResetGameWorld. */
const uint8_t *serverSimGetTrackBuffer(const ServerSim *sim, size_t *outLen,
                                       uint32_t *outRecordCount, bool *outTruncated);
const AttrSlotIdentity *serverSimGetTrackIdentity(const ServerSim *sim); /* [MAX_TANKS] */

/* String (char[]) accessors */
const char *serverSimGetMapName(const ServerSim *sim);
/* 32-char hex of the active map's bytes; "" when random/unknown. */
const char *serverSimGetMapMd5Hex(const ServerSim *sim);
const char *serverSimGetBotBrainPath(const ServerSim *sim);
const char *serverSimGetServerMessageLogFile(const ServerSim *sim);

/* Indexed-array accessors (bounds-checked; out-of-range
 * returns NULL for pointer types, false/0 for scalars). */
const LobbyPlayer *serverSimGetLobbyPlayer(const ServerSim *sim, BYTE n);
bool               serverSimIsPlayerConnected(const ServerSim *sim, BYTE n);
uint32_t           serverSimGetLastProcessedInput(const ServerSim *sim, BYTE n);
bool               serverSimIsMapSkipVote(const ServerSim *sim, BYTE n);

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

/* Per-bot brain selection as an index into the server's brain
 * catalogue. brainIdx == 0xFF means "use the CLI-configured default
 * brain". Set via PACKET_LOBBY_SET_BOT_BRAIN. */
void        serverSimSetBotBrainIdxFor(ServerSim *sim, BYTE slot,
                                       uint8_t brainIdx);

/* Apply a bot-config change atomically — write mode / difficulty /
 * personality to the slot, optionally rename the bot (when validatedName is
 * non-NULL and non-empty), publish CTRL_LOBBY_BOT_CONFIG +
 * CTRL_LOBBY_SLOT, and clear humans' ready state. Callers (UDP
 * PACKET_LOBBY_BOT_CONFIG handler, SP-host clientSimNetSendLobbyBotConfig)
 * must validate the name beforehand — see lobbyBotNameAcceptable.
 * Pass NULL or an empty string to leave the name unchanged.
 *
 * mode indexes the brain's mode list and difficulty indexes THAT mode's
 * level list (brain_list.h); mode 0 with 0/1/2 is the pre-manifest
 * easy/medium/hard.
 *
 * Safe to call on a slot with no bot in it yet, and the SP-host and the
 * dedicated server both do: both bytes have to be in the slot's config
 * BEFORE the bot's brain is created, because that is where bot_manager
 * reads them to build the brain's "mode=" / "difficulty=" init tokens. */
void        serverSimSetBotConfig(ServerSim *sim, BYTE slot,
                                  uint8_t mode, uint8_t difficulty,
                                  uint8_t personality,
                                  const char *validatedName);

/* Default-mode difficulty <-> word. "easy" / "medium" / "hard" is what
 * -difficulty accepts on the dedicated server's command line and what the
 * "Chosen Difficulty" preference stores for the default mode. (What the
 * brain is handed is the level KEY from the brain's own modes.txt, which
 * for the default mode is these same three words.) botDifficultyName never
 * returns NULL: an out-of-range value reads as "hard". botDifficultyFromName
 * is case-insensitive, takes "normal" as an alias for "medium" (its former
 * label), and returns false without touching *out on anything else.
 * Implemented in bot_manager.c. */
const char *botDifficultyName(uint8_t difficulty);
bool        botDifficultyFromName(const char *name, uint8_t *out);

/* Resolve a catalogue index back to its disk path. Returns the
 * CLI-configured default path for brainIdx == 0xFF, NULL when the
 * index is out of range. The returned pointer is owned by the sim
 * and is valid until the sim is destroyed. */
const char *serverSimGetBrainPathForIdx(const ServerSim *sim,
                                        uint8_t brainIdx);

/* The server's brain catalogue (auto-scanned from brains/). Indices line
 * up with serverSimSetBotBrainIdxFor / the published lobby brain list.
 * Returns NULL for a NULL sim; the pointer is owned by the sim. */
const BrainList *serverSimGetBrainList(const ServerSim *sim);

/* ── Server-side map directory enumeration ──────────────────────────
 *
 * Lists entries (subdirectories and .map files) at a path relative to
 * the server's data/maps/ root. Used by the lobby's map chooser so a
 * connected client can browse the *server's* map library.
 *
 * relPath:    Relative to data/maps; "" or NULL = root. ".." segments
 *             are rejected (escape attempts return -1).
 * entries:    Caller-allocated output array.
 * maxEntries: Capacity of entries[]; the function writes at most this
 *             many; if the directory has more, the extras are dropped
 *             and the function returns maxEntries.
 *
 * Returns the number of entries written, or -1 on validation /
 * read failure. Folders sort first (alphabetically), then files
 * (alphabetically). */
typedef struct {
    char    name[128];
    bool    isFolder;
    int64_t modTime;   /* file mtime, ns since UNIX epoch (SDL_Time);
                          0 if unknown. Folders carry the directory
                          mtime so the table sort still reads sensibly
                          for them. */
    int64_t size;      /* file size in bytes; 0 for folders. */
} ServerMapEntry;

int serverSimEnumerateMapDir(ServerSim *sim, const char *relPath,
                              ServerMapEntry *entries, int maxEntries);

/* Recursive search variant. Walks data/maps/<relPath> and every
 * nested directory, returning .map files whose basename contains
 * `query` (case-insensitive substring) up to maxEntries results.
 * Each returned entry's `name` is the path relative to relPath
 * (e.g. "Subdir/Foo.map") and isFolder is always false. relPath
 * "" or NULL = search the whole library. Empty query returns 0
 * (caller wanted enumerate, not search). */
int serverSimSearchMapDir(ServerSim *sim, const char *relPath,
                           const char *query,
                           ServerMapEntry *entries, int maxEntries);

/* Read the on-disk .map file at data/maps/<relPath> into a heap
 * buffer. Returns true and fills outBytes (malloc'd; caller frees
 * with free()) + outLen on success. The client preview-fetch path
 * calls this from the server's REQ handler, then streams the bytes
 * back in chunks. Implementation rejects path traversal and any
 * relPath that doesn't resolve to a regular file.
 *
 * Used by the Server Maps preview protocol: server sources the
 * bytes, client rasterises them locally with the same renderer it
 * uses for the Upload tab. */
bool serverSimReadMapFile(ServerSim *sim, const char *relPath,
                           uint8_t **outBytes, size_t *outLen);

/* autoLockOnGameStart — when true, sets allowNewPlayers=false the
 * moment the lobby transitions out of serverStateLobby. */
bool        serverSimGetAutoLockOnGameStart(const ServerSim *sim);
/* serverSimSetAutoLockOnGameStart: moved to
 * internal/server_sim_lifecycle.h — applied by serverInstanceStartup
 * and by the shared serverSimApplyLobbySetting helper. */

/* Ranked-game flag. Setting it true also flips ai=none. The server's
 * LST_GAME_TYPE handler refuses to switch to gameOpen while ranked,
 * and the LST_AI_POLICY handler refuses anything but aiNone. Existing
 * bot slots are NOT removed by this setter — the LST_RANKED packet
 * handler in the transport layer does that explicitly when the toggle
 * flips on, and it also resets every player's ready bit so the host
 * can confirm the new configuration. */
bool        serverSimGetRanked(const ServerSim *sim);

/* True iff the current lobby shape qualifies as a ranked match —
 * exactly two teams in use, equal sizes of 1/2/3 connected humans
 * (1v1, 2v2, 3v3), no bots counted. Independent of whether the
 * lobby is currently flagged Ranked; callers check both. Used by
 * the server's PACKET_LOBBY_READY handler to silently drop Ready
 * requests on ranked lobbies that don't qualify yet, and by the
 * unit tests to lock the shape rules. */
bool        serverSimRankedShapeReady(const ServerSim *sim);

/* serverSimSetRanked: moved to internal/server_sim_lifecycle.h —
 * applied by serverInstanceStartup and by the shared
 * serverSimApplyLobbySetting helper. */

/* Per-category visibility rules (pillboxes / bases / allied tanks).
 * Set from the dedicated-server and headless CLI switches and from the
 * GUI hosting prefs after the sim is created, and from the lobby via
 * LST_PILL_VIEW / LST_BASE_VIEW / LST_ALLY_VIEW. Defaults are
 * pill = viewPolicyAlways, base = viewPolicyOff, ally = viewPolicyAlways,
 * all with VIEW_DECAY_DEFAULT_SECS.
 *
 * The setter clamps decaySecs to VIEW_DECAY_MIN_SECS..VIEW_DECAY_MAX_SECS
 * and ignores an out-of-range category or policy. The getters return the
 * defaults for a NULL sim or an out-of-range category. */
void        serverSimSetViewPolicy(ServerSim *sim, ViewCategory cat,
                                   ViewPolicy policy, uint16_t decaySecs);
ViewPolicy  serverSimGetViewPolicy(const ServerSim *sim, ViewCategory cat);
uint16_t    serverSimGetViewDecaySecs(const ServerSim *sim, ViewCategory cat);

/* Classic mode — the host is asking for the classic Bolo view. Turning
 * it on sets pill view to key and base and ally view to off, keeping
 * each category's own decay seconds, and the lobby then refuses edits
 * to those three until it is turned off. Turning it off clears the flag
 * and nothing else: the three policies stay where classic mode put
 * them. Off by default.
 *
 * Because it writes those values, an operator lock on any of them locks
 * classic mode as well — see serverSimAddImpliedLocks. This setter is
 * not lock-aware; its callers check the mask first. */
void        serverSimSetClassicMode(ServerSim *sim, bool on);
bool        serverSimGetClassicMode(const ServerSim *sim);

/* Allies in trees — when on, an allied tank standing in trees is sent to
 * its allies instead of being withheld, with the usual fog of war still
 * applying. Off is the classic behaviour: an ally in trees is never
 * visible past the viewer's own immediate sight box. Off by default, and
 * turning classic mode on forces it off. */
void        serverSimSetAlliesInTrees(ServerSim *sim, bool on);
bool        serverSimGetAlliesInTrees(const ServerSim *sim);

/* Voice mode — how the server handles the voice its clients send it.
 * serverVoiceOff forwards nothing; serverVoiceProximity is not
 * implemented and forwards like serverVoiceOn. Set once from
 * ServerInstanceConfig.voiceMode at startup and not changed after; there
 * is no lobby setting for it. The setter ignores an out-of-range mode,
 * and the getter returns serverVoiceOn for a NULL sim. */
void            serverSimSetVoiceMode(ServerSim *sim, ServerVoiceMode mode);
ServerVoiceMode serverSimGetVoiceMode(const ServerSim *sim);

/* openHost — when true, any connected player has host-level edit
 * authority on lobby state (see lobbyClientMayEdit). */
bool        serverSimGetOpenHost(const ServerSim *sim);

/* hostSlot — the player slot currently holding the lobby host role.
 * Defaults to 0. */
BYTE        serverSimGetHostSlot(const ServerSim *sim);

/* serverLocks — LOBBY_LOCK_* bitmask set from CLI at server start.
 * Locked settings refuse PACKET_LOBBY_SET_SETTING with REJECT_LOCKED.
 * The stored mask is the CLI mask plus its implied locks, so what the
 * getter returns — and what the lobby-settings event carries to every
 * client's lock badges — may hold more bits than the operator typed. */
uint16_t    serverSimGetServerLocks(const ServerSim *sim);

/* Expand a LOBBY_LOCK_* mask with the locks it implies, and return it.
 * One setting can write another's value, and a lock the host can reach
 * around is not a lock; this is where that is settled, once, for both
 * the server's REJECT_LOCKED check and the client's disabled controls.
 *
 * Locking pill / base / ally view or allies in trees also locks classic
 * mode, which writes all four. serverSimSetServerLocks runs every mask
 * through this, so callers rarely need it directly — it is exposed so
 * the CLI can report the expanded set and tests can check the mapping
 * without a sim. Idempotent. */
uint16_t    serverSimAddImpliedLocks(uint16_t locks);

/* Map an LST_* setting id to the LOBBY_LOCK_* bit that gates it.
 * Returns 0 for settings with no lock, 0xFFFF for unknown ids. The
 * PACKET_LOBBY_SET_SETTING handler uses this to decide whether to
 * REJECT_LOCKED; tests use it to verify the lock table is correct. */
uint16_t    serverSimGetSettingLockBit(uint8_t lstSettingType);
/* True iff sim has the lock bit for `lstSettingType` set AND that
 * setting actually has a lock bit (i.e., bit != 0 and bit != 0xFFFF). */
bool        serverSimIsSettingLocked(const ServerSim *sim,
                                     uint8_t lstSettingType);

/* firstJoinerBecomesHost — dedicated-server option. When true and slot
 * 0 is unoccupied at join time, the incoming player is promoted to
 * host (admin rights). Set via -firstjoinhost; no UI counterpart. */
bool        serverSimGetFirstJoinerBecomesHost(const ServerSim *sim);
void        serverSimSetFirstJoinerBecomesHost(ServerSim *sim, bool v);

/* Cached game-settings mirrors. authoritative state lives in GameSim;
 * these expose the most-recently-broadcast value for lock checks and
 * SETTING_CHG diffs. */
void        serverSimSetAiPolicy(ServerSim *sim, uint8_t v);
bool        serverSimGetTimeLimit(const ServerSim *sim);
void        serverSimSetTimeLimit(ServerSim *sim, bool v);
uint16_t    serverSimGetTimeMinutes(const ServerSim *sim);
void        serverSimSetTimeMinutes(ServerSim *sim, uint16_t v);

/* Per-bot config (difficulty + personality). */
const LobbyBotConfig *serverSimGetBotConfig(const ServerSim *sim, BYTE slot);

/* Mutable game-length (transition-out-of-lobby and time-limit edits
 * write through this). */
void serverSimSetGameLength(ServerSim *sim, int32_t ticks);

/* Lobby-time game-setting writers. The lobby UI used to poke
 * serverSimGetGameSim(sim)->game / ->hiddenMines directly; these
 * keep the field on GameSim opaque. Only meaningful in lobby state. */
gameType serverSimGetGameType(const ServerSim *sim);
void serverSimSetGameType(ServerSim *sim, gameType gt);
void serverSimSetHiddenMines(ServerSim *sim, bool hiddenMines);
void serverSimSetState(ServerSim *sim, ServerState s);
void serverSimSetCountdownTicks(ServerSim *sim, int32_t ticks);

/* Switch a lobby bot to a new brain script. Updates both the
 * per-slot brain-index mirror (serverSimSetBotBrainIdxFor) and the
 * bot manager's live state in a single call — these are always
 * paired at call sites. brainIdx == 0xFF resolves to the
 * CLI-configured default brain. */
void serverSimSwitchBotBrain(ServerSim *sim, BYTE slot, uint8_t brainIdx);

/* Rename a bot's display name in the players table without
 * touching alliance/team/transport state. SP-only path used by the
 * AiConfig "Bot N" rename UI. */
void serverSimRenameBotSlot(ServerSim *sim, BYTE slot, const char *name);

/* SP-host lobby publishers.
 *
 * The dedicated server's transportUdpServerBroadcastLobby*Chg
 * functions do two things: (a) send a UDP packet to all clients,
 * and (b) publish a control event for in-process subscribers.
 * Single-player has no UDP clients — these wrappers do just (b),
 * so the local humanSim picks up the state change through the
 * subscriber chain set up by serverSimRegisterClientSubscriber. */
void serverSimPublishLobbySlot(ServerSim *sim, BYTE slot);
void serverSimPublishLobbyBotBrain(ServerSim *sim, BYTE slot);
void serverSimPublishLobbyBotConfig(ServerSim *sim, BYTE slot);
void serverSimPublishLobbyTeamMeta(ServerSim *sim, BYTE teamId);
void serverSimPublishLobbySettings(ServerSim *sim);

/* viewPlayer — which player perspective the sim renders from. */
BYTE serverSimGetViewPlayer(const ServerSim *sim);
/* serverSimSetViewPlayer: moved to internal/server_sim_lifecycle.h
 * — applied by serverInstanceStartup from cfg.viewPlayer and by
 * the bg_game rendering camera-perspective override (under a
 * per-file T2 grant). */

/* Tutorial-mode flag mirrored on the embedded GameSim. */
bool serverSimIsTutorial(const ServerSim *sim);
void serverSimSetTutorial(ServerSim *sim, bool v);
void serverSimSetTutorialStartIdx(ServerSim *sim, BYTE idx);
bool serverSimTakeTutorialRespawn1(ServerSim *sim);

/* Pause flag on the embedded GameSim. Unlocked: caller must hold
 * threadsMutex. */
void serverSimSetPaused(ServerSim *sim, bool paused);

/* --- Live-sim map / pill / base / start readers ---
 * Server-side perspective of the same map/pill/base/start state
 * the client-side wrappers expose on client_sim.h. Used by
 * binaries that own a ServerSim directly (bg_game, braintest,
 * headless, gym). */

BYTE         serverSimGetMapTerrain(const ServerSim *sim, BYTE x, BYTE y);
bool         serverSimMapIsMine(const ServerSim *sim, BYTE x, BYTE y);
bool         serverSimPillExistsAt(const ServerSim *sim, BYTE x, BYTE y);
BYTE         serverSimPillGetScreenHealthAt(ServerSim *sim, BYTE x, BYTE y, BYTE viewPlayer);
bool         serverSimBaseExistsAt(const ServerSim *sim, BYTE x, BYTE y);
baseAlliance serverSimBaseGetAllianceAt(ServerSim *sim, BYTE x, BYTE y, BYTE viewPlayer);
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
    /* The full 0-255 facing dir was quantised from. No reader yet: it is
       here so the menu background's tank draw (mapViewRenderCentered) can
       take the same draw-time rotation path the live game has behind
       WB_SKIN_DRAWTIME_ROTATION, which reads its angle off the screen tank
       list via screenTanksGetSubPixel. Filled by serverSimGetTankRender
       either way. */
    TURNTYPE angle;
    bool  on_boat;
    bool  alive;     /* false when slot is empty or tank is in death-wait */
} TankRenderInfo;

/* Populate *out from sim->tanks[i]. Returns false (without touching
 * *out) if i >= MAX_TANKS or the slot is empty. When the slot is
 * occupied but the tank is in death-wait, returns true with
 * out->alive = false. */
bool serverSimGetTankRender(ServerSim *sim, BYTE i, TankRenderInfo *out);

/* Fuller per-slot snapshot for scoreboard / end-of-game readers:
 * identity (name) and score (kills/deaths) alongside render state.
 * Unlike TankRenderInfo this is keyed on a *connected player slot*
 * rather than a live tank object. */
typedef struct TankInfo {
    char  name[PLAYER_NAME_LEN]; /* NUL-terminated player name */
    WORLD world_x;
    WORLD world_y;
    BYTE  dir;       /* 0-15, already converted from TURNTYPE */
    bool  on_boat;
    bool  alive;     /* false in death-wait or before the tank spawns */
    bool  has_tank;  /* false if connected but no live tank object yet */
    int   kills;
    int   deaths;
} TankInfo;

/* Populate *out for connected player slot i. Returns false (without
 * touching *out) if i >= MAX_TANKS or the slot is not connected. When
 * connected but the tank object is absent (countdown / death-wait),
 * has_tank = false, alive = false, and the position/score fields are 0;
 * name is always filled. */
bool serverSimGetTankInfo(ServerSim *sim, BYTE i, TankInfo *out);

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

#ifdef WB_NETDEBUG
/* Net-debug rig accessors. Exist only in WB_NETDEBUG builds — they
 * read the per-player sim-executed counters the rig measures. Reset
 * zeroes both counter arrays; getters return 0 for an out-of-range
 * playerNum. */
void     serverSimNetdebugResetCounters(ServerSim *sim);
uint32_t serverSimNetdebugGetExecTurnTicks(ServerSim *sim, BYTE playerNum);
uint32_t serverSimNetdebugGetMineLays(ServerSim *sim, BYTE playerNum);
#endif

#endif /* SERVER_SIM_H */
