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
#include "../bolo/game_sim.h"
#include "../bolo/position_history.h"
#include "../bolo/input_packet.h"
#include "../bolo/brain_list.h"
#include "../mapeditor/mapeditor_generate.h"

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

typedef struct ServerSim {
    GameSim      sim;    /* MUST be first member */

    /* Tick state */
    uint32_t     tick;
    int32_t      startDelay;
    int32_t      gameLength;

    /* Server state machine */
    ServerState  state;
    bool         lobbyEnabled;       /* false = no-lobby mode (skip lobby, play immediately) */
    LobbyPlayer  lobbyPlayers[MAX_TANKS];

    /* ── Lobby Layout A — auto-ally team metadata + bot configs ────
     * teams[] is presentation: name, color, naming pool — keyed by
     * teamNumber 1..MAX_TANKS-1. teams[0] is reserved for "Unassigned"
     * and never has metadata. Persists across rounds with the rest of
     * the lobby state. */
    TeamMetadata    teams[MAX_TANKS];
    LobbyBotConfig  botConfigs[MAX_TANKS];

    /* Per-bot brain path. Empty = "use the global botBrainPath". The
     * lobby AiConfig dropdown writes here via PACKET_LOBBY_SET_BOT_BRAIN
     * so different bots in the same lobby can run different brains. */
    char            botBrainPaths[MAX_TANKS][260];

    /* Discovered brain codebases under brains/ — sent to clients via
     * PACKET_LOBBY_BRAIN_LIST so the AiConfig combo can list them. */
    BrainList       brainList;

    /* Admin IPs (-admins CLI flag). When a client connects from any of
     * these IPs, the server tags them with PLAYER_FLAG_ADMIN and grants
     * them host-level lobby authority. Comma-separated string of IPv4
     * literals; the first '\0' terminates the list. */
    char            adminIps[1024];

    /* Layout A lobby flags — all persist across rounds. */
    bool     openHost;             /* anyone can edit when true */
    bool     allowNewPlayers;      /* live state — drives PACKET_LOCK_TOGGLE */
    bool     autoLockOnGameStart;  /* if true, set allowNewPlayers=false on game start */
    bool     savedAllowNewPlayers; /* what allowNewPlayers was before autoLockOnGameStart fired */
    uint16_t serverLocks;          /* LOBBY_LOCK_* bitmask, set from CLI */

    /* Game-settings mirrors — needed for live mid-lobby change broadcasts.
     * The authoritative values live in GameSim/serverSim CLI args; these
     * track the most recently broadcast value so we can detect/refuse
     * locked changes and emit SETTING_CHG diffs cleanly. */
    uint8_t  aiPolicy;             /* mirrors aiType passed at create */
    bool     timeLimit;            /* derived from gameLength != UNLIMITED */
    uint16_t timeMinutes;          /* user-facing minutes (display + edit) */
    int32_t      countdownTicks;     /* Countdown timer (in ticks) */
    int32_t      originalGameLength; /* Cached for reset between rounds */
    bool         hadPlayersEver;     /* For auto-close detection */
    bool         quitOnWin;          /* Server should check for win condition */
    bool         autoCloseOnEmpty;   /* Server should close when all players leave */
    char         pendingWinMessage[512]; /* Win message to send after returning to lobby */
    bool         emptyResetEnabled;  /* Reset to lobby when empty for emptyResetMinutes */
    int          emptyResetMinutes;  /* Minutes before empty reset (default 5) */
    int32_t      emptyResetTicks;    /* Countdown ticks remaining (-1 = not counting) */

    /* Map reload — cached compressed map for between-round resets */
    BYTE        *cachedMapData;      /* Compressed map buffer (malloc'd) */
    int          cachedMapDataLen;   /* Length of compressed data */

    /* Info packet fields — stored at creation for server browser responses */
    char         mapName[MAP_STR_SIZE];
    uint32_t     timeCreated;
    unsigned short serverPort;

    /* Per-player input queues — allows 2 inputs per server timer callback */
#define SERVER_INPUT_QUEUE_SIZE 8  /* Must be power of 2 */
    InputPacket  inputQueue[MAX_TANKS][SERVER_INPUT_QUEUE_SIZE];
    uint8_t      inputQueueHead[MAX_TANKS];  /* Next slot to write */
    uint8_t      inputQueueTail[MAX_TANKS];  /* Next slot to read */
    bool         playerConnected[MAX_TANKS];
    uint32_t     lastProcessedInput[MAX_TANKS];  /* Tick of last processed input per player */
    uint8_t      lastInputButtons[MAX_TANKS];    /* Last button bitmask for stall continuity */
    uint16_t     playerPing[MAX_TANKS];           /* Per-player ping in ms (server-measured RTT) */

    /* Input jitter buffer — delay processing until buffer reaches target depth */
#define JITTER_BUFFER_MIN       1   /* Minimum buffer depth (ticks) */
#define JITTER_BUFFER_MAX       4   /* Maximum buffer depth (ticks) */
#define JITTER_BUFFER_DEFAULT   2   /* Starting depth before we have data */
#define JITTER_GROW_THRESHOLD   2   /* Consecutive stalls before growing */
#define JITTER_SHRINK_INTERVAL 100  /* Ticks of no stalls before shrinking */
#define LAG_COMP_MAX_TICKS 12       /* 250ms one-way max compensation (12 game ticks) */
    uint8_t inputBufferFilled[MAX_TANKS];  /* true once initial fill reached */
    uint8_t  jitterTarget[MAX_TANKS];      /* Current adaptive buffer depth */
    uint8_t  jitterStallCount[MAX_TANKS];  /* Consecutive ticks queue was empty when expected */
    uint16_t jitterStableTicks[MAX_TANKS]; /* Ticks since last stall */

    PosHistory   posHistory[MAX_TANKS];           /* Position history for lag compensation */
    PosHistory   lgmPosHistory[MAX_TANKS];        /* LGM position history for lag compensation */

    /* Which player is currently being processed in the tick loop.
     * Used by serverSimCbMessageAdd to target assistant messages. */
    uint8_t      currentTickPlayer;

    /* Event buffer — populated during tick, consumed by snapshot sender */
    GameEvent    events[MAX_SNAPSHOT_EVENTS];
    uint8_t      eventCount;

    /* Separate map-change event buffer — never competes with sound/game
     * events for slots, so map changes are never silently dropped. */
#define MAX_MAP_EVENTS 256
    GameEvent    mapEvents[MAX_MAP_EVENTS];
    uint16_t     mapEventCount;

    /* Previous pill/base state for change detection */
    PillSnapshot prevPills[MAX_SNAPSHOT_PILLS];
    BaseSnapshot prevBases[MAX_SNAPSHOT_BASES];
    uint8_t      prevPillCount;
    uint8_t      prevBaseCount;

    /* Full state sync tracking */
    uint32_t     lastFullSyncTick;

    /* Bot configuration — cached from CLI args for lobby bot creation */
    char         botBrainPath[260];       /* Brain path for lobby bot creation */
    aiType       botAiType;               /* AI advantage level for bots */

    /* WBN registration — cached from CLI args for re-registration between rounds */
    bool         hasPassword;             /* Server has a password set */

    /* WBN team balance proposal */
    BalanceProposal balanceProposal;

    bool mapSkipVotes[MAX_TANKS]; /* per-slot map skip vote */

    /* Map directory rotation — validated map file paths for random selection */
    char       **mapDirFiles;             /* Array of validated map file paths (malloc'd) */
    int          mapDirCount;             /* Number of valid maps in the array */

    /* Random map generation (for -randommap mode) */
    bool         randomMapEnabled;       /* true when using -randommap */
    MapGenConfig randomMapConfig;        /* last used config */
    bool         randomMapFixedSeed;     /* true = same map every round */

    /* Server message configuration (was servermessages.c globals) */
    bool         isServerQuiet;
    char         serverMessageLogFile[FILENAME_MAX];
    bool         serverMessageUseLogFile;

    /* Log recording configuration (set from CLI args, used by serverSimStartGame) */
    bool         wantLogging;
    char         userLogFileName[512];
} ServerSim;

/*********************************************************
 *NAME:          serverSimCreate
 *PURPOSE:
 *  Creates and initializes a ServerSim by loading a map
 *  file. Returns TRUE on success.
 *
 *ARGUMENTS:
 *  sim         - Pointer to the ServerSim to initialize
 *  mapFileName - Path to the .map file
 *  game        - Game type (open/tournament/strict)
 *  hiddenMines - Are hidden mines allowed
 *  startDelay  - Game start delay (in ticks)
 *  gameLen     - Game length in ticks (-1 = unlimited)
 *********************************************************/
bool serverSimCreate(ServerSim *sim, char *mapFileName, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen);

/*********************************************************
 *NAME:          serverSimCreateCompressed
 *PURPOSE:
 *  Creates and initializes a ServerSim from a compressed
 *  map buffer (e.g. built-in maps). Returns TRUE on success.
 *
 *ARGUMENTS:
 *  sim         - Pointer to the ServerSim to initialize
 *  buff        - Compressed map data
 *  buffLen     - Length of compressed data
 *  game        - Game type (open/tournament/strict)
 *  hiddenMines - Are hidden mines allowed
 *  startDelay  - Game start delay (in ticks)
 *  gameLen     - Game length in ticks (-1 = unlimited)
 *********************************************************/
bool serverSimCreateCompressed(ServerSim *sim, BYTE *buff, int buffLen, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen);

/*********************************************************
 *NAME:          serverSimDestroy
 *PURPOSE:
 *  Destroys a ServerSim and frees all owned resources.
 *
 *ARGUMENTS:
 *  sim - Pointer to the ServerSim to destroy
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
 *NAME:          serverSimClearActive
 *PURPOSE:
 *  Clears the active sim pointer if it matches the given
 *  sim. Call this before freeing a ServerSim that was not
 *  cleaned up via serverSimDestroy.
 *********************************************************/
void serverSimClearActive(ServerSim *sim);

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
 *NAME:          serverSimResetGameWorld
 *PURPOSE:
 *  Resets the game world using cached map data. Destroys
 *  all tanks/LGMs and recreates world systems. Does NOT
 *  destroy player connections or lobby state.
 *********************************************************/
void serverSimResetGameWorld(ServerSim *sim);

/*********************************************************
 *NAME:          serverSimSyncLobbyToClient
 *PURPOSE:
 *  Single-player path: copy the server's lobby state into a
 *  ClientSim's lobby* / lobbySlots fields directly, in place
 *  of the PACKET_LOBBY_STATE round-trip used in multiplayer.
 *  Call once after entering the lobby and again whenever the
 *  server-side lobby state mutates.
 *
 *ARGUMENTS:
 *  sim - The local ServerSim (lobbyEnabled=true, state=lobby).
 *  cs  - The local ClientSim to write into.
 *********************************************************/
void serverSimSyncLobbyToClient(ServerSim *sim, struct ClientSim *cs);

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
 *  Creates and initializes a ServerSim using procedural
 *  map generation instead of loading from disk.
 *  Returns TRUE on success.
 *********************************************************/
bool serverSimCreateRandomMap(ServerSim *sim, const MapGenConfig *cfg,
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

#endif /* SERVER_SIM_H */
