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
#include "types.h"             /* TankModifiers — carried in TankInfo */
#include "scenario_table.h"    /* ScnTable — a bot's init table */
#include "brain_list.h"        /* BrainList — returned by serverSimGetBrainList */
#include "client_command.h"    /* ClientCommand / CmdResult — serverSimApplyCommand */
#include "attribution_track.h" /* AttrSlotIdentity — track accessors below */
#include "view_policy.h"       /* ViewPolicy / ViewCategory — view-policy accessors below */
#include "server_voice_mode.h" /* ServerVoiceMode — voice-mode accessors below */
#include "upload_policy.h"     /* ScriptUploadPolicy — script-upload accessors below */

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
    /* The second channel. A subscriber that wants the tick's game events as
     * well as the control stream sets this with
     * serverSimSetSubscriberEventDeliver after it has registered. NULL — which
     * is what registration leaves, and what every subscriber that asks for
     * nothing more keeps — means control events alone. */
    void (*deliverEvent)(void *ctx, const GameEvent *evt);
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
  /* On the field. A seat can be held without being played: an unfielded seat
     is a connected bot seat with no bot manager entry, no ClientSim and no
     tank, which shows in the roster and is skipped by the start sequence
     until something fields it. Every other way of taking a seat sets this
     true. */
  bool fielded;
  /* The seat outlives the bot in it. Taking the bot out of a seat marked
     this way returns the seat to unfielded rather than emptying it, so the
     next wave has somewhere to land. Set when the seat is seeded unfielded;
     survives being fielded. */
  bool keepSeat;
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
  /* 1 when startSide was filled in for this team because the one other team
     in a two-team lobby named the opposite side, or because the lobby opened
     with its default pair (serverSimApplyDefaultTeamSides); 0 when
     a player named it (or named no side). Server-side only: it is on no wire
     packet and no ControlEvent, and the client never sees it. The lobby reads it to decide
     whether a later change by that other team may rewrite this side — a side
     a player named is never written over. */
  uint8_t sideAutoFilled;
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
 * full constructor calls serverSimAddBot internally.
 *
 * team is the slot's team, 0 for none. It travels in the bot config so
 * the add writes it before it picks the slot's lobby start from it; a
 * caller that sets the team afterwards has already missed that pick.
 *
 * init is this bot's configuration, reaching its brain as the
 * BRAIN_INIT Lua table; it is copied, and NULL means no pairs. Each
 * bot carries its own, so a caller creating several in a row hands
 * each one a different table without ordering mattering. */
bool serverSimCreateBot(ServerSim *sim, BYTE playerNum,
                        const char *brainPath, const char *brainName,
                        aiType ai, gameType game, bool hiddenMines,
                        BYTE team, const ScnTable *init);

void serverSimRemoveBot(ServerSim *sim, BYTE playerNum);
/* Every bot out of the roster, seats held for a bot that was never fielded
 * included, and the scenario's seats with them. The lobby no longer holds
 * what a template built, so the next scenario decision seats it again even
 * where it reaches the template already attached. */
void serverSimRemoveAllBots(ServerSim *sim);
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
bool              serverSimBotSetLuaGlobalString(ServerSim *sim, BYTE playerNum,
                                                 const char *name,
                                                 const char *value);

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
 *NAME:          serverSimSetScenarioDir
 *               serverSimGetScenarioDir
 *PURPOSE:
 *  The directory of scenarios this server offers on their
 *  own, independently of any map: -scenariodir on the
 *  dedicated server, the "Scenario Dir" preference on a
 *  desktop host. The getter falls back to the built-in
 *  "data/scenarios" the way the map root falls back to
 *  "data/maps"; NULL or "" to the setter goes back to that
 *  default. No trailing slash.
 *
 *  Public rather than beside the map root in
 *  server_sim_lifecycle.h: a desktop host sets this one from
 *  its own preferences, and a GUI translation unit sees
 *  public/ only.
 *
 *  Setting it reads nothing. The directory is read when a
 *  client asks for the list, through the lister registered
 *  with serverSimSetScenarioLister, and a directory that is
 *  not there answers an empty list rather than an error —
 *  a server offering no scenarios of its own is the
 *  ordinary case.
 *
 *ARGUMENTS:
 *  sim - Pointer to the ServerSim
 *  dir - The directory, or NULL for the built-in default
 *********************************************************/
void        serverSimSetScenarioDir(ServerSim *sim, const char *dir);
const char *serverSimGetScenarioDir(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimSetScenarioRecordText
 *               serverSimGetScenarioRecordText
 *PURPOSE:
 *  The scripts.json text for this round's recording
 *  (src/bolo/public/scripts_record.h). The scenario host
 *  sets it at the end of a round boot that loaded scripts
 *  and clears it for a round that runs none; logStop reads
 *  it, writes it into the .wbv beside the attribution
 *  track and clears it, so each recording carries it once.
 *  A scenario host detaching does not clear it: a host that
 *  leaves mid-round detaches before its log is closed.
 *
 *  The setter copies len bytes into a buffer the sim owns
 *  and frees the one it replaces. NULL or a len of zero
 *  clears it. A len over SCN_RECORD_TEXT_MAX stores nothing
 *  — the text is cleared and a warning is logged — so an
 *  over-long description is never written.
 *
 *  The getter returns the text and its length in *len, or
 *  NULL with *len = 0 when there is none. The pointer is
 *  valid until the next set.
 *
 *  No lock, the same as serverSimGetTrackBuffer: the round
 *  boot and logStop both run on the thread that owns the
 *  sim.
 *
 *ARGUMENTS:
 *  sim  - Pointer to the ServerSim
 *  text - The JSON text; it need not be NUL-terminated
 *  len  - Its length in bytes
 *********************************************************/
void        serverSimSetScenarioRecordText(ServerSim *sim, const char *text,
                                           size_t len);
const char *serverSimGetScenarioRecordText(const ServerSim *sim, size_t *len);

/*********************************************************
 *NAME:          serverSimGetUploadsDir
 *PURPOSE:
 *  Where an uploaded map lands on disk: the configured
 *  persist directory, or "<map root>/Uploads" when none is
 *  configured. This is the same answer the virtual
 *  "Uploads" folder resolves to, so a map file sitting
 *  under it is a map a client sent this server.
 *
 *  Public because the scenario library asks it. A map's
 *  path is the only record that it arrived as an upload —
 *  the bytes are written straight to their final name and
 *  nothing is stamped on the file — so the switch that
 *  decides whether an uploaded map's script may run has
 *  this prefix to compare against and nothing else.
 *
 *  out is left empty when sim is NULL.
 *
 *ARGUMENTS:
 *  sim    - Pointer to the ServerSim
 *  out    - Buffer the directory is written to, no
 *           trailing slash
 *  outLen - Size of out; FILENAME_MAX holds every answer
 *********************************************************/
void        serverSimGetUploadsDir(const ServerSim *sim, char *out,
                                   size_t outLen);

/*********************************************************
 *NAME:          serverSimSetWorkshopMapDir
 *               serverSimGetWorkshopMapDir
 *PURPOSE:
 *  The directory subscribed Workshop items are copied to,
 *  which the map listing offers as the virtual folder
 *  "Workshop". "" or NULL clears it.
 *
 *  While it is set, "Workshop" and "Workshop/<name>" name
 *  that directory and the files in it, for the listing,
 *  the preview read and the lobby's set-map command alike,
 *  as "Uploads" names the persist directory. The root of
 *  the listing shows the folder only while the directory
 *  exists. Unset, which is how the dedicated server always
 *  runs, "Workshop" is an ordinary path under the map root.
 *
 *  Setting it reads nothing. The getter answers "" when
 *  nothing is set.
 *
 *ARGUMENTS:
 *  sim - Pointer to the ServerSim
 *  dir - The directory, no trailing slash; NULL or "" to
 *        clear it
 *********************************************************/
void        serverSimSetWorkshopMapDir(ServerSim *sim, const char *dir);
const char *serverSimGetWorkshopMapDir(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimGetSelectedScenario
 *PURPOSE:
 *  Which scenario from that directory the lobby host has
 *  picked, by the file name the directory listing gave.
 *  Empty is none — the ordinary state of a server whose host
 *  has picked nothing.
 *
 *  Or the committed map's own script, where the host's list
 *  names that row at the front. The row is on the list to
 *  say where the round composes the script, and it names a
 *  file the map brought rather than one the directory
 *  holds, so a caller that means to open it in the
 *  scenarios directory reads the row rather than this name.
 *  serverSimGetScript answers the rows, and bound is what
 *  tells the two kinds apart.
 *
 *  Never answers NULL: a sim with no selection answers "",
 *  so a caller can print or compare it without a guard.
 *  There is no fallback the way the directory has one; no
 *  selection is a state, not a missing setting.
 *
 *  Public because the scenario library reads it back to
 *  decide what plays: a pick beats the committed map's own
 *  script, and none hands the map its own back. Changing it
 *  is the sim's own business and lives on
 *  server_sim_lifecycle.h — a frontend that wants a
 *  different scenario sends CMD_LOBBY_SET_SCENARIO, which is
 *  what asks for that decision again and publishes the
 *  result.
 *
 *ARGUMENTS:
 *  sim  - Pointer to the ServerSim
 *********************************************************/
const char *serverSimGetSelectedScenario(const ServerSim *sim);

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
 *NAME:          serverSimGetNumFielded
 *PURPOSE:
 *  Returns how many players are on the field — seats that
 *  are playing the round rather than sitting it out. This
 *  is the count "how many players are there" wants, not
 *  the length of the roster: a seat can hold a place in
 *  the lobby without being on the field, and an unfielded
 *  seat is one of those.
 *********************************************************/
BYTE serverSimGetNumFielded(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimIsSeatFielded
 *PURPOSE:
 *  Whether seat playerNum is on the field. False for an
 *  empty seat and for a seat held without a bot in it.
 *********************************************************/
bool serverSimIsSeatFielded(const ServerSim *sim, BYTE playerNum);

/*********************************************************
 *NAME:          serverSimRefreshWbnLobbyInfo
 *PURPOSE:
 *  Rebuilds the WinBolo.net lobby snapshot from current sim
 *  state and stashes it via winbolonetSetLobbyInfo so the
 *  register/update/lobby_update bodies pick up the latest
 *  map, settings and human/bot counts.
 *********************************************************/
void serverSimRefreshWbnLobbyInfo(ServerSim *sim);

/* This header does not include control_event.h, so it keeps its own copies of
 * the lobby's figures; server_sim_round.c holds each to the one it names. */
#define SERVER_SCRIPT_NAME_LEN 64   /* LOBBY_SCENARIO_NAME_LEN */
#define SERVER_SCRIPT_DESC_LEN 256  /* LOBBY_SCENARIO_DESC_LEN */
#define SERVER_SCRIPT_MODS_MAX 9    /* LOBBY_SCRIPT_LIST_MAX - 1 */
typedef struct {
    bool hasScenario;                          /* a scenario, not a mod, decides the round */
    char scenarioName[SERVER_SCRIPT_NAME_LEN];
    char scenarioDescription[SERVER_SCRIPT_DESC_LEN];
    BYTE scenarioMaxPlayers;                   /* human cap, 0 = none */
    BYTE modCount;
    char modNames[SERVER_SCRIPT_MODS_MAX][SERVER_SCRIPT_NAME_LEN];
} ServerScriptSummary;

/*********************************************************
 *NAME:          serverSimGetScriptSummary
 *PURPOSE:
 *  Fills *out with the scripts the round runs, as a server
 *  advertises them: the scenario that decides the round (its
 *  name, description and human cap) when there is one, and
 *  the names of the mods that run, in the lobby's list
 *  order. No mods with Mods Enabled off. Names are the
 *  manifest's, or the file's where the manifest named none,
 *  cut to 63 bytes on a character boundary. The WinBolo.net
 *  lobby snapshot and the info-request reply both read it.
 *********************************************************/
void serverSimGetScriptSummary(const ServerSim *sim, ServerScriptSummary *out);

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
 *NAME:          serverSimSetSubscriberEventDeliver
 *PURPOSE:
 *  Gives the subscriber registered under `h` the game-event
 *  channel as well. The callback is handed every GameEvent
 *  serverSimAddEvent raises, whole, the way the deliver
 *  callback is handed every ControlEvent. NULL takes the
 *  channel away again. Returns FALSE if the handle names no
 *  live subscriber.
 *
 *  Kept apart from registration so a subscriber that wants
 *  control events alone says nothing and is unaffected.
 *********************************************************/
bool serverSimSetSubscriberEventDeliver(
    ServerSim *sim,
    SubscriberHandle h,
    void (*deliverEvent)(void *, const GameEvent *));

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
 *NAME:          serverSimAcceptAllianceQuiet
 *PURPOSE:
 *  The same alliance accept, with the newswire line off.
 *  For seating rather than for something somebody did: a
 *  bot put on its team is setup, and the lobby path that
 *  does the same job with a rebake draws nothing either.
 *  The replay log and the tracker still record it.
 *********************************************************/
void serverSimAcceptAllianceQuiet(ServerSim *sim, BYTE accepter,
                                  BYTE newMember);

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

/*********************************************************
 *NAME:          serverSimFillEntitySyncEvent
 *PURPOSE:
 *  Populate a CTRL_ENTITY_SYNC from the live pill, base and
 *  start lists: bit i of a mask is set when index i holds an
 *  item that is on the map.
 *
 *  Returns false, leaving *evt untouched, when every index of
 *  every list is on the map. Installing a compressed map
 *  marks exactly that, so a caller sending this to a client
 *  that has just installed one would be saying what the
 *  install already said; every caller therefore skips the
 *  send on false.
 *********************************************************/
bool serverSimFillEntitySyncEvent(ServerSim *sim, struct ControlEvent *evt);

/*********************************************************
 *NAME:          serverSimFillSimRulesEvent
 *PURPOSE:
 *  Populate a CTRL_SIM_RULES from the gameplay numbers this
 *  sim is running on — every rule a client reads, and none
 *  of the ones only the server does.
 *
 *ARGUMENTS:
 *  sim - The sim whose table is being stated
 *  evt - Event to fill
 *********************************************************/
void serverSimFillSimRulesEvent(const ServerSim *sim, struct ControlEvent *evt);

/*********************************************************
 *NAME:          serverSimPublishSimRules
 *PURPOSE:
 *  Publish the table this sim is running on to every
 *  subscriber. Called at the end of each of the two
 *  authoritative round starts — serverSimStartGame and
 *  serverSimStartGameInPlace, one of which every start path
 *  runs — so every round states its table exactly once
 *  however it was started. Called again whenever a rule the
 *  event carries changes mid-round; a joining client is given
 *  the same event by the sync replay instead.
 *
 *ARGUMENTS:
 *  sim - The sim whose table is being stated
 *********************************************************/
void serverSimPublishSimRules(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimGetScenarioRule
 *PURPOSE:
 *  What one rule of the simulation's table is set to, as the
 *  double the set-rule op carries a value in. The double is
 *  exact for every integer rule in the table and for every
 *  value a float rule can hold, so this reads back what a
 *  write put there rather than an approximation of it.
 *
 *  rule is an index into the rule list scenario_defs.h
 *  builds, carried as a plain uint16_t so this call names
 *  nothing a frontend cannot see. An index that names no
 *  rule returns false and leaves *out alone.
 *
 *ARGUMENTS:
 *  sim  - The sim whose table is being read
 *  rule - Which rule, by its index in the list
 *  out  - Filled with the rule's value
 *********************************************************/
bool serverSimGetScenarioRule(const ServerSim *sim, uint16_t rule,
                              double *out);

/*********************************************************
 *NAME:          serverSimIsScenarioActing
 *PURPOSE:
 *  Whether what the engine is doing right now is a
 *  scenario's doing. True for the duration of an op handler
 *  and of the per-tick roster drain, which is where a bot a
 *  script asked for actually lands — a tick after the op
 *  that asked for it, and so outside the handler.
 *
 *  Neither event channel carries an actor, and neither needs
 *  to: a remote client runs no hooks. This is what a host
 *  reads as it queues an event, so the hook it later runs
 *  can say whether the fact was the script's own doing. A
 *  handler that ignores its own edits tests that.
 *
 *  False for a NULL sim and for a sim with no scenario.
 *
 *ARGUMENTS:
 *  sim - The sim being asked
 *********************************************************/
bool serverSimIsScenarioActing(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimScenarioSeatLobby
 *PURPOSE:
 *  Empties the seats the attached scenario put in the lobby
 *  and seats its lobby template again from scratch, so the
 *  lobby holds the teams and bot counts the scenario asks
 *  for.
 *
 *  A map commit reaches this through the map change, where
 *  the commit brought a different template. A
 *  process that boots straight onto a scripted map makes no
 *  commit, so it calls this itself — after the bot pool is
 *  up and the server's brain path is set, because a team the
 *  template fields with no brain of its own falls back to
 *  the server's and builds its bot through the manager.
 *
 *  A seat takes the first slot nobody is in, so a caller
 *  that wants a particular slot held — its own player's, or
 *  one a bot it adds by number is going into — puts that
 *  player or bot in first.
 *
 *  Safe with no scenario attached: there is then no template
 *  and this seats nothing.
 *
 *ARGUMENTS:
 *  sim - The sim whose lobby is being seated
 *********************************************************/
void serverSimScenarioSeatLobby(ServerSim *sim);

/* True when the map's script declared a lobby of its own, the one
 * serverSimScenarioSeatLobby lays down. A host that would otherwise seed a
 * bot of its own asks this first: a script that describes the lobby is the
 * one that says who sits in it. */
bool serverSimScenarioHasLobbyTemplate(const ServerSim *sim);

/*********************************************************
 *NAME:          serverSimScenarioApplyLobbyRules
 *PURPOSE:
 *  Brings the lobby's own settings into line with the
 *  scenario attached now — the game type, the ranked flag
 *  and the AI policy. A scripted map puts the lobby on
 *  gameScripted, takes ranked off and moves an AI policy
 *  that allows no bots up to one that does; the three
 *  settings it displaced are remembered, and a map with no
 *  scenario gives them back.
 *
 *  A map commit reaches this straight after the seating
 *  above. A process that boots straight onto a scripted map
 *  has no commit to reach either through and calls both
 *  itself; without this the round plays the operator's game
 *  type, so the game the scenario declares is never asked
 *  for. A boot whose round starts inside serverInstanceStartup
 *  calls this before that startup — the rules have to be in
 *  force before the first tank is built.
 *
 *  Safe with no scenario attached: a lobby that never had one
 *  is left exactly as it is.
 *
 *  When the game type changes here, the lobby's bots follow it
 *  as they do when the host changes it by hand
 *  (serverSimFollowGameTypeBotModes).
 *
 *ARGUMENTS:
 *  sim - The sim whose lobby settings are being brought into
 *        line
 *********************************************************/
void serverSimScenarioApplyLobbyRules(ServerSim *sim);

/* Layout A — per-team / per-bot / brain-list events. The matching
 * client-side handlers live in clientSimApplyControl. */
void serverSimFillLobbyTeamMetaEvent(const ServerSim *sim, BYTE teamId, struct ControlEvent *evt);
void serverSimFillLobbyBotConfigEvent(ServerSim *sim, BYTE slot, struct ControlEvent *evt);
void serverSimFillLobbyBotBrainEvent(const ServerSim *sim, BYTE slot, struct ControlEvent *evt);
void serverSimFillLobbyBrainListEvent(const ServerSim *sim, struct ControlEvent *evt);

/* Read every brain's LOBBY TEXTS (announce.txt + commands.txt) off disk into
 * the sim's cache, skipping any brain whose two files have not changed since
 * the last read. Called once where the brains are scanned, and again when a
 * round hands the lobby back, which is the seam an operator would have edited
 * a brain's texts across. Never on the tick path. */
void serverSimRefreshBrainDocs(ServerSim *sim);

/* Drops the cache serverSimRefreshBrainDocs built. serverSimDestroy calls it;
 * nothing else needs to. */
void serverSimFreeBrainDocs(ServerSim *sim);

/* Send one CTRL_LOBBY_BRAIN_ANNOUNCE per brain through `deliver`, out of the
 * cache above: the brain's announce.txt, and the length and generation of
 * its commands.txt without the text. Only for brains that ship at least one
 * of the two files, so a server whose brains carry none emits nothing and a
 * sim that never refreshed emits nothing either.
 *
 * Called beside the brain list, on the two paths where a real client is
 * listening: a joiner's (or spectator's) sync replay, and the broadcast bus
 * when a round hands the lobby back. NOT from the delayed spectator ring's
 * control snapshot, which is rebuilt per keyframe and whose size cap does not
 * count them. */
void serverSimEmitBrainAnnounces(const ServerSim *sim,
                                 void (*deliver)(void *, const struct ControlEvent *),
                                 void *ctx);

/* Take the bot-name catalogue this server hands out from the pools loaded
 * now (lobby_bot_pools.h), replacing the one held. serverSimCreate calls it,
 * but the pools loaded then are not always the final ones: WinBoloDS makes
 * the sim first, loads -botnames or the shipped file after, and calls this
 * again. A test that installs other pools after the sim exists does the same. */
void serverSimRefreshBotPools(ServerSim *sim);

/* Fill the CTRL_LOBBY_BOT_POOL_INFO that names the catalogue held. */
void serverSimFillBotPoolInfoEvent(const ServerSim *sim,
                                   struct ControlEvent *evt);

/* The held catalogue's compressed blob and id, for a
 * PACKET_LOBBY_BOT_POOL_REQ answer. False when there is none. *outBlob
 * points into the sim and stays valid until the next refresh. */
bool serverSimGetBotPoolBlob(const ServerSim *sim, const uint8_t **outBlob,
                             uint32_t *outLen, uint32_t *outId);

/* Brain `brainIdx`'s commands.txt as the cache holds it: compressed with
 * brainDocsCompress (brain_list.h). *outZ points into the cache and stays
 * valid until the next serverSimRefreshBrainDocs or serverSimFreeBrainDocs,
 * so a caller copies it out under the same lock it read it under. False when
 * the index is not a brain or the brain ships no commands.txt; the outs are
 * then untouched. Any out may be NULL. */
bool serverSimGetBrainDocs(const ServerSim *sim, int brainIdx,
                           uint32_t *outGen, uint16_t *outLen,
                           const uint8_t **outZ, uint16_t *outZLen);

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

/* True when this input is the one that breaks a stall-advance lockout: a tick
 * strictly newer than anything the slot has ever received, arriving at or
 * below the last processed tick while the slot is being stall-advanced.
 * Taking it moves the slot's last processed tick back under it, so the
 * dequeue sees it fresh and the stall-advance run ends. The input intake path
 * asks this alongside its own newer-than check — an input that answers true
 * would otherwise be dropped before it ever reaches the sim. */
bool serverSimInputWouldRebase(const ServerSim *sim, BYTE n, uint32_t tick);
bool               serverSimIsMapSkipVote(const ServerSim *sim, BYTE n);

/* The lobby's view of one seat, in one call: who holds it, which team
 * they are on, and whether they are playing right now. This is what
 * team-keyed placement and "how big is the other side" read. */
typedef struct ServerSimRosterSlot {
    bool    connected;  /* Someone holds this seat. Always true on a
                           successful read — the call refuses an empty
                           seat — and is here because it is part of the
                           seat picture callers marshal. */
    bool    is_bot;
    uint8_t team;       /* 1-16; 0 = unassigned */
    char    name[PLAYER_NAME_LEN]; /* NUL-terminated player name */
    bool    ready;      /* Has said it is ready to start */
    bool    fielded;    /* Playing the round rather than sitting it out.
                           False for a seat held in the roster with no bot,
                           no ClientSim and no tank behind it. */
    bool    alive;      /* Has a tank in the world and is not in death-wait */
} ServerSimRosterSlot;

/* Populate *out for seat i. Returns false (without touching *out) if
 * i >= MAX_TANKS or the seat is empty. */
bool serverSimGetRosterSlot(ServerSim *sim, BYTE i, ServerSimRosterSlot *out);

/* Whether seats a and b are allied in the game: the table every game rule
 * reads, which players change in play with an alliance request, accept and
 * leave. It can differ from the two seats' lobby teams. A seat is allied with
 * itself. False if either seat is out of range or empty. */
bool serverSimIsAllied(ServerSim *sim, BYTE a, BYTE b);

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
    bool    scripted;  /* a round on this map here plays by a script: one is
                          beside it and this server runs scripts. Always
                          false for a folder, and false throughout when
                          nothing has registered the question with
                          serverSimSetScenarioMapScripted — a build with no
                          scenario library reports every map plain, and so
                          does a server with scripts switched off. */
} ServerMapEntry;

int serverSimEnumerateMapDir(ServerSim *sim, const char *relPath,
                              ServerMapEntry *entries, int maxEntries);

/* Recursive search variant. Walks data/maps/<relPath> and every
 * nested directory, returning .map files whose basename contains
 * `query` (case-insensitive substring) up to maxEntries results.
 * Each returned entry's `name` is the path relative to relPath
 * (e.g. "Subdir/Foo.map") and isFolder is always false. relPath
 * "" or NULL = search the whole library. Empty query returns 0
 * (caller wanted enumerate, not search). wantScripted asks the scenario
 * library for each hit's `scripted` flag; false leaves it false and
 * skips the lookup (the network search reply has no byte for it). */
int serverSimSearchMapDir(ServerSim *sim, const char *relPath,
                           const char *query,
                           ServerMapEntry *entries, int maxEntries,
                           bool wantScripted);

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

/* ── Server-side scenario directory enumeration ─────────────────────
 *
 * The scenarios this server offers on their own, read out of the
 * directory serverSimSetScenarioDir named. Here for the reason
 * serverSimEnumerateMapDir is here: the lobby's scenario chooser is a
 * gui translation unit, which sees public/ only, and the scenario
 * surface that does this work is in scenario_api/.
 *
 * entries:    Caller-allocated output array.
 * maxEntries: Capacity of entries[]; the function writes at most this
 *             many; if the directory holds more, the extras are
 *             dropped and the function returns maxEntries.
 *
 * Returns the number of entries written. 0 for a server with no
 * scenarios directory, nothing in it, or no scenario library in this
 * build — none of the three is a fault, so none is told apart, and
 * there is no failure return. File-name order, case-insensitive.
 *
 * The three lengths below mirror SCN_DIR_FILE_LEN / _NAME_LEN /
 * _DESC_LEN in src/bolo/scenario_api/scenario_defs.h, which this
 * header does not include because a gui translation unit reads this
 * one and cannot reach that one. The implementation sees both and
 * holds each pair against the other, so the two cannot drift and a
 * copy into this shape cannot cut a name or a description short. */
#define SERVER_SCENARIO_FILE_LEN 128
#define SERVER_SCENARIO_NAME_LEN 64
#define SERVER_SCENARIO_DESC_LEN 256
/* Where a row came from: ServerScenarioEntry.source, and the source byte
 * the scenario-list packet and the script-list event carry. The same
 * values as SCN_DIR_SOURCE_* in scenario_defs.h, mirrored and held
 * against them for the reason the lengths are. */
#define SERVER_SCENARIO_SOURCE_SERVER   0 /* the server's own directories */
#define SERVER_SCENARIO_SOURCE_UPLOAD   1 /* a player uploaded it */
#define SERVER_SCENARIO_SOURCE_WORKSHOP 2 /* a Steam Workshop item */
typedef struct {
    char     file[SERVER_SCENARIO_FILE_LEN];  /* the name in the scenarios
                                                 directory */
    char     name[SERVER_SCENARIO_NAME_LEN];  /* the manifest's, or "" */
    char     description[SERVER_SCENARIO_DESC_LEN];
    uint8_t  maxPlayers;  /* 0 = the server's own cap */
    uint8_t  bots;        /* seats the template asks for */
    bool     bound;       /* belongs to one map; not selectable as a mod */
    bool     keepsWinCondition;  /* keeps the round's win condition, which is
                                    what a mod does and a scenario does not.
                                    Not the same question as bound: a mod and
                                    an unbound scenario are both unbound. */
    uint8_t  source;      /* SERVER_SCENARIO_SOURCE_* */
    uint64_t workshopId;  /* the Workshop item, 0 for none */
    uint64_t workshopAuthor;  /* the SteamID64 that published it, 0 for none.
                                 Filled by this computer's own listings and
                                 never by what a server sends. */
} ServerScenarioEntry;

int serverSimEnumerateScenarioDir(ServerSim *sim,
                                  ServerScenarioEntry *entries,
                                  int maxEntries);

/* One script file's details (scenario_details.h): the rules its manifest
 * sets and what its callbacks do, packed into out, which holds cap bytes.
 * The lobby's details dialog asks for these one file at a time, when it
 * opens, and a remote client asks for them with
 * PACKET_LOBBY_SCENARIO_DETAILS_REQ, which the server answers out of this.
 *
 * The committed map's own script is looked for first, under the name the
 * lobby's script list gives it, and the scenarios directory after that, so
 * a map's script that is not in the directory still answers.
 *
 * Returns the blob's length, which is 0 for a file that sets no rule and
 * describes no callback, or -1 for a file neither place holds (and for a
 * blob that does not fit cap). */
int serverSimScenarioDetails(ServerSim *sim, const char *file, uint8_t *out,
                             size_t cap);

/* What serverSimScriptFileRead found. */
typedef enum {
    SERVER_SCRIPT_READ_FOUND = 0,
    SERVER_SCRIPT_READ_NOT_FOUND,
    SERVER_SCRIPT_READ_DISABLED,
    SERVER_SCRIPT_READ_TOO_LARGE
} ServerScriptReadResult;

/* One of the server's script files, whole and as it sits on disk, for a
 * player who asked for a copy with PACKET_LOBBY_SCRIPT_FETCH_REQ: a
 * .scenario as its ZIP bytes, a .lua as its source. file is a name from the
 * scenario listing, and is only ever compared with the names a directory
 * read found, never opened as a path.
 *
 * The committed map's own script is not served (its file is the map), and
 * answers NOT_FOUND, as does a name no directory holds. DISABLED means the
 * process runs no scripts, TOO_LARGE a file over
 * LOBBY_PACKAGE_UPLOAD_MAX_BYTES. On FOUND, *outBytes is malloc'd and the
 * caller frees it; otherwise *outBytes is NULL and *outLen is 0. */
ServerScriptReadResult serverSimScriptFileRead(ServerSim *sim,
                                               const char *file,
                                               uint8_t **outBytes,
                                               uint32_t *outLen);

/* One script file's settings block (scenario_settings.h): the settings its
 * manifest lets the host choose, packed into out, which holds cap bytes.
 * Looked up the way serverSimScenarioDetails looks up the details: the
 * committed map's own script first, then the scenarios directory.
 *
 * Returns the blob's length, which is 0 for a file that declares no
 * setting, or -1 for a file neither place holds (and for a blob that does
 * not fit cap). */
int serverSimScenarioSettingsDecl(ServerSim *sim, const char *file,
                                  uint8_t *out, size_t cap);

/* How many values the server keeps for scripts' settings at once. A value
 * equal to its default is not kept, so this is values a host moved off the
 * default, across every script the session has seen. */
#define SERVER_SCRIPT_SETTING_VALUES_MAX 48

/* The value the host chose for file's setting id, in *out. False when none
 * is kept, which means the declared default. The value is the one
 * serverSimSetScriptSetting checked; a caller holding the declaration
 * resolves it again with scnSettingResolve all the same, because the file
 * may have been edited since. */
bool serverSimGetScriptSetting(const ServerSim *sim, const char *file,
                               const char *id, int32_t *out);

/* Sets file's setting id to value, checked against the declaration
 * serverSimScenarioSettingsDecl reads for file. A value below the range is
 * clamped to the lowest entry and one above it to the highest; one inside
 * the range but off the step falls back to the default
 * (scnSettingClamp). The default is also what clears a kept value. Publishes the result as
 * a CTRL_LOBBY_SCRIPT_SETTING SET.
 *
 * Returns the value now in effect, in *resolved when it is not NULL.
 * False, and nothing changed or published, for a file with no declaration,
 * an id it does not declare, a bool setting given anything but 0 or 1, or a
 * store that is full. */
bool serverSimSetScriptSetting(ServerSim *sim, const char *file,
                               const char *id, int32_t value,
                               int32_t *resolved);

/* Every kept value, as one CTRL_LOBBY_SCRIPT_SETTING CLEAR and then one SET
 * per value, into deliver. The join sync is the caller. */
void serverSimReplayScriptSettings(
    const ServerSim *sim, void (*deliver)(void *, const struct ControlEvent *),
    void *ctx);

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
 * LST_PILL_VIEW / LST_BASE_VIEW / LST_ALLY_VIEW. A sim nobody has
 * configured holds pill = viewPolicyKey, base = viewPolicyOff,
 * ally = viewPolicyOff, all with VIEW_DECAY_DEFAULT_SECS.
 *
 * The setter clamps decaySecs to VIEW_DECAY_MIN_SECS..VIEW_DECAY_MAX_SECS
 * and ignores an out-of-range category or policy. The getters answer
 * viewPolicyAlways and VIEW_DECAY_DEFAULT_SECS for a NULL sim or an
 * out-of-range category — what a reader assumes of a sender that named
 * no policy, not what a sim starts on. */
void        serverSimSetViewPolicy(ServerSim *sim, ViewCategory cat,
                                   ViewPolicy policy, uint16_t decaySecs);
ViewPolicy  serverSimGetViewPolicy(const ServerSim *sim, ViewCategory cat);
uint16_t    serverSimGetViewDecaySecs(const ServerSim *sim, ViewCategory cat);

/* Classic mode — the host is asking for the classic Bolo view. Turning
 * it on sets pill view to key and base and ally view to off, keeping
 * each category's own decay seconds, turns allies in trees off, and
 * sets the overview window to Classic with line of sight off; the lobby
 * then refuses edits to those until it is turned off. Turning it off
 * clears the flag and nothing else: every value it wrote stays where
 * classic mode put it. Off by default.
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

/* Positional sound — when on, each sound event to a human carries which
 * side the sound is on and a banded distance. Off is the classic
 * behaviour: every sound is sent centred, with only its near or far
 * variant chosen. Bots and the recording keep real squares either way.
 * Off by default, and turning classic mode on forces it off. */
void        serverSimSetPositionalSound(ServerSim *sim, bool on);
bool        serverSimGetPositionalSound(const ServerSim *sim);

/* Overview window — which block of squares the map overview keeps live
 * around the player's own tank, and line of sight — whether anything
 * stops the player seeing inside that block. Values are OverviewWindow
 * and LineOfSightMode from view_policy.h, carried a byte wide. A sim
 * nobody has configured holds the classic window with sight off.
 *
 * Turning classic mode on writes both — the narrow window, with nothing
 * blocking sight inside it — so an operator lock on either locks classic
 * mode as well; see serverSimAddImpliedLocks. Each setter ignores a
 * value outside its enum. Each getter answers overviewWindowExpanded
 * and lineOfSightOff for a NULL sim — what a reader assumes of a sender
 * that named neither, not what a sim starts on. */
void        serverSimSetOverviewWindow(ServerSim *sim, uint8_t window);
uint8_t     serverSimGetOverviewWindow(const ServerSim *sim);
void        serverSimSetLineOfSight(ServerSim *sim, uint8_t mode);
uint8_t     serverSimGetLineOfSight(const ServerSim *sim);

/* Smart pings — whether a client may drop a ping marker on the map. The
 * host owns it from the lobby (LST_SMART_PINGS_OFF), and the server
 * refuses CMD_PING outright while it is set, so a modified client gains
 * nothing by ignoring the setting.
 *
 * Named and stored in the negative sense on purpose: false means pings
 * are ALLOWED. That is what every build before the setting existed did,
 * and it is what a zeroed sim, an absent wire byte and a NULL sim all
 * read as. The lobby UI reads it through the positive accessor
 * clientSimIsLobbyAllowSmartPings, so no display code deals in negatives. */
void        serverSimSetSmartPingsOff(ServerSim *sim, bool off);
bool        serverSimGetSmartPingsOff(const ServerSim *sim);

/* Mods/Scenario — whether the round composes the scripts on the lobby's
 * pick list. The host owns it from the lobby (LST_MODS_OFF), and
 * scnDecideScenario (src/scenario/scenario_host.c) is the one reader: it
 * skips every pick while this is set, mods and picked scenarios alike.
 *
 * The map's own script still plays. It is not a pick, whatever place on the
 * list it has been given, and with no picked scenario composing it comes
 * back at the front the way it does for a list that picked none.
 *
 * The pick list is not touched. Turning the setting off is not the same as
 * emptying the list: the entries stay in the order the host put them in and
 * come back composed the moment it goes on again, the same way the lobby's
 * password box keeps its text while the box beside it is unchecked.
 *
 * Named and stored in the negative sense on purpose, matching smart pings
 * above: false means the mods RUN. That is what every build before the
 * setting existed did, and it is what a zeroed sim, an absent wire byte and
 * a NULL sim all read as. The lobby UI reads it through the positive
 * accessor clientSimGetLobbyModsEnabled, so no display code deals in
 * negatives. */
void        serverSimSetModsOff(ServerSim *sim, bool off);
bool        serverSimGetModsOff(const ServerSim *sim);

/* Voice mode — how the server handles the voice its clients send it.
 * serverVoiceOff forwards nothing; serverVoiceProximity is not
 * implemented and forwards like serverVoiceOn. Set once from
 * ServerInstanceConfig.voiceMode at startup and not changed after; there
 * is no lobby setting for it. The setter ignores an out-of-range mode,
 * and the getter returns serverVoiceOn for a NULL sim. */
void            serverSimSetVoiceMode(ServerSim *sim, ServerVoiceMode mode);
ServerVoiceMode serverSimGetVoiceMode(const ServerSim *sim);

/* Script upload policy — how the server treats scripts players send it:
 * OFF refuses them and does not run a script an uploaded map carries,
 * ALLOW keeps them for the session, PERSIST keeps them for good. Set once
 * from ServerInstanceConfig.scriptUploadPolicy at startup and carried to
 * clients on the lobby-settings event. The getter returns
 * SCRIPT_UPLOAD_ALLOW for a NULL sim. */
void               serverSimSetScriptUploadPolicy(ServerSim *sim, ScriptUploadPolicy p);
ScriptUploadPolicy serverSimGetScriptUploadPolicy(const ServerSim *sim);

/* Script sharing — whether players may save a copy of this server's mods
 * and scenarios. Set once from ServerInstanceConfig.noScriptSharing at
 * startup and carried to clients on the lobby-settings event. A new sim
 * shares. The setter ignores a NULL sim, and the getter returns true for
 * one. */
void serverSimSetScriptSharing(ServerSim *sim, bool on);
bool serverSimGetScriptSharing(const ServerSim *sim);

/* The directory a script a player uploads lands in: the persist directory
 * under PERSIST, the session directory under ALLOW, "" under OFF. Resolved
 * once by serverInstanceStartup. The transport writes there, and the
 * scenario host reads it as the lowest-precedence directory of the merged
 * script listing. NULL sets "". The getter never returns NULL. */
void        serverSimSetScriptUploadDir(ServerSim *sim, const char *dir);
const char *serverSimGetScriptUploadDir(const ServerSim *sim);

/* The session directory: the landing directory under ALLOW on a host that
 * takes remote clients, "" otherwise. Only this one is ever emptied. NULL
 * sets "". The getter never returns NULL. */
void        serverSimSetScriptSessionDir(ServerSim *sim, const char *dir);
const char *serverSimGetScriptSessionDir(const ServerSim *sim);

/* Removes every regular file directly in the session directory, and nothing
 * in a directory below it. A no-op when the session directory is "" or not
 * there. Answers how many files went. Called at startup, at shutdown and by
 * the lobby reset. */
int         serverSimEmptyScriptSessionDir(ServerSim *sim);

/* A count of the changes this process has made to a scripts directory: a
 * file an upload put in place, or files the session emptying removed. The
 * scenario library's directory listing keeps what it read against this as
 * well as against the directory's modify time, which the kernel stamps too
 * coarsely to see a change made straight after a read. Process-wide because
 * that listing cache is process-wide. Safe from any thread. */
void        serverSimNoteScriptDirsChanged(void);
uint32_t    serverSimScriptDirsGen(void);

/* Resolve the script upload policy from its command-line or preference
 * word. A non-empty word is matched against off / allow / persist
 * ignoring case and always wins over legacyOff; an unknown word logs a
 * warning naming it and resolves to SCRIPT_UPLOAD_ALLOW. With no word
 * (NULL or empty), legacyOff — the old -nouploadscripts flag or
 * "Run Upload Scripts" preference set to No — gives SCRIPT_UPLOAD_OFF,
 * else SCRIPT_UPLOAD_ALLOW. */
ScriptUploadPolicy scriptUploadPolicyResolve(const char *word, bool legacyOff);

/* The preference spelling of a policy: "Off", "Allow" or "Persist".
 * Anything out of range reads as "Allow". */
const char        *scriptUploadPolicyWord(ScriptUploadPolicy p);

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
uint32_t    serverSimGetServerLocks(const ServerSim *sim);

/* Expand a LOBBY_LOCK_* mask with the locks it implies, and return it.
 * One setting can write another's value, and a lock the host can reach
 * around is not a lock; this is where that is settled, once, for both
 * the server's REJECT_LOCKED check and the client's disabled controls.
 *
 * Locking pill / base / ally view, allies in trees, the overview window
 * or line of sight also locks classic mode, which writes all six.
 * serverSimSetServerLocks runs every mask through this, so callers
 * rarely need it directly — it is exposed so the CLI can report the
 * expanded set and tests can check the mapping without a sim.
 * Idempotent. */
uint32_t    serverSimAddImpliedLocks(uint32_t locks);

/* Map an LST_* setting id to the LOBBY_LOCK_* bit that gates it.
 * Returns 0 for settings with no lock, 0xFFFFFFFF for unknown ids. The
 * PACKET_LOBBY_SET_SETTING handler uses this to decide whether to
 * REJECT_LOCKED; tests use it to verify the lock table is correct. */
uint32_t    serverSimGetSettingLockBit(uint8_t lstSettingType);
/* True iff sim has the lock bit for `lstSettingType` set AND that
 * setting actually has a lock bit (i.e., bit != 0 and
 * bit != 0xFFFFFFFF). */
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

/* ── A newly added bot's brain mode and difficulty ─────────────────────
 *
 * Every bot a lobby gains — the host's Add Bot, a scenario's own seed,
 * single player's add and setup paths — resolves its mode and difficulty
 * through serverSimResolveNewBotConfig, in this order:
 *
 *   1. the caller's base (the lobby default, or single player's own
 *      chosen level). The base gives the level only: the mode is the game
 *      type's starting mode (open_default on Open, else mode 0);
 *   2. what the map requires for the bot's side — the attached scenario's
 *      lobby template, read for that team; on Survival, the horde's seats
 *      are the default mode at Hard;
 *   3. what a person last picked BY HAND, when the caller honours it — on a
 *      team step 2 configured, the level last chosen on a seat of that team
 *      and never the mode; on every other team, the one pair the lobby
 *      remembers, which may set both.
 *
 * Bots that first appear on a map (the seed, single player's setup bots)
 * do not honour step 3; the Add Bot button does. See server_sim_lobby.c. */

/* Resolve a new bot's mode and difficulty for `team` on the brain at
 * `brainPath`. *ioMode / *ioLevel carry the caller's base in and the answer
 * out. Returns false, leaving both untouched, when the brain ships no
 * modes.txt. Writes no config and publishes nothing. */
bool serverSimResolveNewBotConfig(const ServerSim *sim, int team,
                                  const char *brainPath,
                                  bool honourManualPick,
                                  uint8_t *ioMode, uint8_t *ioLevel);

/* ── A scenario's own mode and difficulty ──────────────────────────────
 *
 * A scenario names the two by KEY — the words in the brain's own modes.txt
 * ("default", "hard") — because a script cannot know what index a brain
 * puts them at, and the two bytes the lobby carries are indices. This turns
 * one pair of keys into that pair of indices.
 *
 * Both keys are optional and "" means "leave this one alone", which is what
 * every scenario written before the two fields existed says. A mode key that
 * moves the seat to a different mode takes that mode's own default level
 * unless a level key names one, because a level index only means something
 * inside one mode's list.
 *
 * Case-insensitive, as brainModesFindMode and brainModeFindLevel are. */
typedef enum {
    BOT_CFG_KEYS_OK = 0,      /* applied, or neither key was given */
    BOT_CFG_KEYS_NO_MANIFEST, /* the brain ships no modes.txt to ask */
    BOT_CFG_KEYS_NO_MODE,     /* the mode key names no mode of this brain */
    BOT_CFG_KEYS_NO_LEVEL     /* the level key names no level of that mode */
} BotConfigKeyResult;

/* Resolve `modeKey` / `levelKey` against the brain at `brainPath`.
 * *ioMode / *ioLevel carry the current pair in and the answer out, and are
 * left untouched on any answer but BOT_CFG_KEYS_OK. Writes no config and
 * publishes nothing — the caller does both, because the order matters: the
 * config has to be in the slot BEFORE the brain is created. */
BotConfigKeyResult serverSimResolveBotConfigKeys(const char *brainPath,
                                                 const char *modeKey,
                                                 const char *levelKey,
                                                 uint8_t *ioMode,
                                                 uint8_t *ioLevel);

/* Write a slot's mode and difficulty and queue the bot-config event that
 * shows them, without the publish-now, the rename and the auto-unready
 * serverSimSetBotConfig carries. For a seat that does not exist yet: the
 * two bytes have to be in botConfigs before the brain is created, and a
 * publish to a slot no client has heard of yet describes nothing. Queue the
 * publish again once the seat is connected — the flush drops the queued bit
 * for a slot that is not. */
void serverSimSetBotConfigQuiet(ServerSim *sim, BYTE slot,
                                uint8_t mode, uint8_t difficulty);

/* Give a lobby bot joining `team` its mode and difficulty — the lobby
 * default as the base, then serverSimResolveNewBotConfig — and queue the
 * bot-config event that shows it in the lobby. Safe before or after the
 * add: pass the brain the bot runs, since before the add the slot has
 * none. A lobby brain reloads from this config at round start. */
void serverSimApplyNewBotDefaults(ServerSim *sim, BYTE slot, int team,
                                  const char *brainPath,
                                  bool honourManualPick);

/* Record the slot's current mode and difficulty as the host's manual pick.
 * Call it only where a person chose them: CMD_LOBBY_BOT_CONFIG, and only
 * when that command actually changed the mode or difficulty — the gear
 * popup sends the same command for renames and personality edits. An
 * automatic config write must never call it, or the game's own defaults
 * pass for the host's choice. */
void serverSimRememberManualBotPick(ServerSim *sim, BYTE slot);

/* Record that a person changed this bot's MODE by hand. Called by the
 * CMD_LOBBY_BOT_CONFIG arm when the mode differs from the seat's previous
 * one. A marked seat keeps its mode through serverSimFollowGameTypeBotModes.
 * The mark goes when the seat's player leaves, when the seat is given a new
 * bot's defaults (serverSimApplyNewBotDefaults), and when the lobby is reset
 * to its defaults. */
void serverSimMarkBotModeSetByHand(ServerSim *sim, BYTE slot);

/* After the game type changed from `oldType` to the current one: every bot
 * whose brain starts in a different mode under the new type (an Open game
 * starts in the manifest's open_default, brainModesStartMode), whose mode is
 * still the old type's starting mode, and which nobody set a mode for by
 * hand, moves to the new starting mode. The level moves by key. A team a
 * scenario template configures is left alone. Queues the bot-config event
 * for each seat it moves. */
void serverSimFollowGameTypeBotModes(ServerSim *sim, gameType oldType);

/* Bot-config events waiting to be published, a couple per lobby tick. A
 * scenario seeds its ten bots in one call stack while no client ack can be
 * read, and the reliable control channel holds 64 unacked events; queuing
 * those ten events for the lobby tick keeps the seed's burst exactly as
 * large as it was. Flush skips slots that are no longer bots. */
void serverSimQueueBotConfigPublish(ServerSim *sim, BYTE slot);
void serverSimFlushBotConfigPublishes(ServerSim *sim);

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

/* The counts above are slot counts. The pill and base readers below return
 * false for a number out of range and for a slot whose item is not on the
 * map (taken off by a scenario, or by the loader for one in the mined
 * border); the *Info readers report that as their active field instead. */
bool         serverSimGetPill(ServerSim *sim, BYTE i,
                              BYTE *x, BYTE *y, BYTE *owner, BYTE *armour,
                              bool *inTank);
/* A pill's firing interval alongside serverSimGetPill: the ticks between
 * shots, which halve towards PILLBOX_MAX_FIRERATE as the pill is hit and
 * climb back as it calms. Reader for binaries that own a ServerSim
 * directly. The pill's reload and coolDown counters are deliberately not
 * exposed: pillsGetPill does not copy them, and both move every half-step,
 * so they suit a recording rather than a change-only log. */
bool         serverSimGetPillSpeed(ServerSim *sim, BYTE i, BYTE *speed);
bool         serverSimGetBase(ServerSim *sim, BYTE i,
                              BYTE *x, BYTE *y, BYTE *owner);
bool         serverSimGetBaseStats(ServerSim *sim, BYTE i,
                                   BYTE *shells, BYTE *mines, BYTE *armour);
bool         serverSimGetStart(ServerSim *sim, BYTE i,
                               BYTE *x, BYTE *y, BYTE *dir);

/* One pill in one call — what serverSimGetPill and serverSimGetPillSpeed
 * return between them, in a struct. Both of those stay for their callers. */
typedef struct ServerSimPillInfo {
    BYTE x;
    BYTE y;
    BYTE owner;    /* a player slot, or NEUTRAL */
    BYTE armour;   /* 0-15; 0 = dead on the ground */
    BYTE speed;    /* ticks between shots */
    bool in_tank;  /* being carried rather than sitting on a square */
    bool active;   /* This index holds a live pill. A removed pill leaves its
                      index in place so the indices above it keep their
                      numbers, so this is what tells a live pill from a
                      removed one at an index that is still in range. */
} ServerSimPillInfo;

/* Populate *out for pill i (1-based, as serverSimGetPill). Returns false
 * (without touching *out) if i is 0 or past the pill count. */
bool serverSimGetPillInfo(ServerSim *sim, BYTE i, ServerSimPillInfo *out);

/* One base in one call — what serverSimGetBase and serverSimGetBaseStats
 * return between them, in a struct. Both of those stay for their callers. */
typedef struct ServerSimBaseInfo {
    BYTE x;
    BYTE y;
    BYTE owner;    /* a player slot, or NEUTRAL */
    BYTE armour;
    BYTE shells;
    BYTE mines;
    bool active;   /* This index holds a live base — as ServerSimPillInfo. */
} ServerSimBaseInfo;

/* Populate *out for base i (1-based, as serverSimGetBase). Returns false
 * (without touching *out) if i is 0 or past the base count. */
bool serverSimGetBaseInfo(ServerSim *sim, BYTE i, ServerSimBaseInfo *out);

/* One start in one call, alongside serverSimGetStart, which stays for its
 * callers. */
typedef struct ServerSimStartInfo {
    BYTE x;
    BYTE y;
    BYTE dir;     /* 0-15, as serverSimGetStart reports it */
    bool active;  /* This index holds a live start — as ServerSimPillInfo. */
} ServerSimStartInfo;

/* Populate *out for start i (1-based, as serverSimGetStart). Returns false
 * (without touching *out) if i is 0 or past the start count. */
bool serverSimGetStartInfo(ServerSim *sim, BYTE i, ServerSimStartInfo *out);

/* Bytes in the terrain array serverSimGetMapTerrainBuffer copies out. */
#define SERVER_SIM_TERRAIN_BYTES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)

/* Copy the whole terrain array out in one call, for a caller that scans
 * every square at setup instead of making 65,536 serverSimGetMapTerrain
 * calls. The layout is row-major: out[(y * 256) + x] is the square at
 * (x, y), so one row of the map is 256 contiguous bytes. Each byte is
 * what serverSimGetMapTerrain returns for that square, border squares
 * included — they read DEEP_SEA from both. Returns false and copies
 * nothing when cap is under SERVER_SIM_TERRAIN_BYTES. */
bool serverSimGetMapTerrainBuffer(const ServerSim *sim, BYTE *out, size_t cap);

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
    BYTE  map_x;     /* Map square the tank is standing on */
    BYTE  map_y;
    BYTE  dir256;    /* The same facing as dir, at its full 0-255 resolution.
                        dir is this quantised to the 16 drawn frames. */
    BYTE  shells;
    BYTE  mines;
    BYTE  armour;
    BYTE  trees;
    BYTE  pills;     /* How many pills the tank is carrying */
    bool  is_bot;
    TankModifiers mods; /* Per-tank percentages; a zeroed set is classic */
} TankInfo;

/* Populate *out for connected player slot i. Returns false (without
 * touching *out) if i >= MAX_TANKS or the slot is not connected. When
 * connected but the tank object is absent (countdown / death-wait),
 * has_tank = false, alive = false, and the position, stock, score and
 * modifier fields are 0; name and is_bot are always filled. */
bool serverSimGetTankInfo(ServerSim *sim, BYTE i, TankInfo *out);

/* Tank alliance from selfPlayer's perspective. Independent of
 * sim->sim.viewPlayer so callers don't need to mutate that global
 * just to colour tanks for a different camera. */
tankAlliance serverSimGetTankAllianceFor(ServerSim *sim,
                                         BYTE selfPlayer,
                                         BYTE tankNum);

/* Where a tank's builder is in its round trip. */
typedef enum {
    builderStateInTank      = 0, /* riding in the tank */
    builderStateGoing       = 1, /* walking out to the square it was sent to */
    builderStateReturning   = 2, /* walking back to the tank */
    builderStateParachuting = 3, /* killed, dropping back in to a tank that
                                    is still standing */
    builderStateDead        = 4  /* killed with no standing tank to drop to */
} BuilderState;

/* A job a builder can be given. The values are the codes the engine's own
 * request path takes, pinned to them by static assert in
 * server_sim_accessors.c, so one of these can be both read back off a
 * builder and handed to a builder order. */
typedef enum {
    builderJobTrees    = 0,  /* harvest trees */
    builderJobRoad     = 1,  /* build road */
    builderJobBuilding = 2,  /* build or repair a wall */
    builderJobPill     = 3,  /* place a carried pill, or repair one */
    builderJobMine     = 4,  /* lay a mine */
    builderJobBoat     = 5,  /* build a boat */
    builderJobNone     = 6   /* nothing in progress */
} BuilderJob;

/* A tank's builder, as the tank read gives the tank. */
typedef struct ServerSimBuilderInfo {
    BuilderState state;
    /* Where the builder is. The engine tracks this while he is out of the
     * tank; in builderStateInTank he is wherever his tank is and these
     * read 0, so check the state before using them. */
    WORLD        world_x;
    WORLD        world_y;
    BYTE         map_x;   /* Map square the builder is standing on */
    BYTE         map_y;
    BuilderJob   job;     /* The job in progress, or builderJobNone */
    BYTE         trees;   /* Trees the builder is carrying */
    BYTE         mines;   /* Mines the builder is carrying */
} ServerSimBuilderInfo;

/* Populate *out for connected player slot i. Returns false (without
 * touching *out) if i >= MAX_TANKS, the slot is not connected, or the
 * slot has no builder object yet (countdown / between rounds). */
bool serverSimGetBuilderInfo(ServerSim *sim, BYTE i, ServerSimBuilderInfo *out);

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
