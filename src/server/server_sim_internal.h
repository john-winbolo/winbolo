/*********************************************************
 * server_sim_internal.h
 *
 * Implementation-only header for the ServerSim type.
 * Files that include this header see the full struct
 * definition and may access its fields directly.
 *
 * Outside src/server/server_sim.c and
 * src/server/server_lifecycle.c, callers must include
 * server_sim.h (which exposes only a forward declaration)
 * and use the public accessor/mutator API.
 *********************************************************/
#ifndef SERVER_SIM_INTERNAL_H
#define SERVER_SIM_INTERNAL_H

#include <stddef.h>
#include "server_sim.h"

typedef struct ServerSim {
    GameSim      sim;    /* MUST be first member */

    /* Tick state */
    uint32_t     tick;
    int32_t      startDelay;
    int32_t      gameLength;
    int32_t      tickLimit;          /* 0 = unlimited; counts running game-ticks */
    int32_t      ticksRun;           /* Running-state tick counter */

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
     * lobby AiConfig dropdown writes here via
     * PACKET_LOBBY_SET_BOT_BRAIN so different bots in the same lobby
     * can run different brains. */
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

    /* In-process control event subscribers (bot ClientSims, SP humanSim). */
    ControlSubscriber subscribers[MAX_TANKS + 1];
    uint16_t          subscriberGen[MAX_TANKS + 1];
    int               numSubscribers;
    bool              publishing;
} ServerSim;

BOLO_STATIC_ASSERT(offsetof(struct ServerSim, sim) == 0,
                   ServerSim_sim_must_be_first_member);

#endif /* SERVER_SIM_INTERNAL_H */
