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
#include "game_sim.h"        /* GameSim layout — used by the sim field below */
#include "position_history.h" /* PosHistory — used by posHistory / lgmPosHistory */
#include "mapgen.h" /* MapGenConfig — embedded by value in randomMapConfig */
#include "brain_list_internal.h" /* BRAIN_LIST_PATH_LEN — brainPaths mirror */
#include "upload_policy.h"  /* UploadPolicy — broadcast in lobby-settings event */

struct ServerSim {
    GameSim      sim;    /* MUST be first member */

    /* Tick state */
    uint32_t     tick;
    int32_t      startDelay;
    int32_t      gameLength;
    int32_t      tickLimit;          /* 0 = unlimited; counts running game-ticks */
    int32_t      ticksRun;           /* Running-state tick counter */
    int32_t      gameTickLimit;      /* 0 = unlimited; ends the running game when reached (no loop exit). */
    int32_t      gameTicksRun;       /* Running-state tick counter paired with gameTickLimit; resets each game. */

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

    /* Per-bot brain selection as an index into brainList. 0xFF means
     * "use the global botBrainPath" (the CLI-configured default). The
     * lobby AiConfig dropdown writes here via
     * PACKET_LOBBY_SET_BOT_BRAIN so different bots in the same lobby
     * can run different brains. */
    uint8_t         botBrainIdx[MAX_TANKS];

    /* Discovered brain codebases under brains/ — sent to clients via
     * PACKET_LOBBY_BRAIN_LIST so the AiConfig combo can list them. */
    BrainList       brainList;

    /* Server-private mirror of the on-disk paths for each entry in
     * brainList. Populated in lockstep with brainList by brainListScan
     * and indexed identically (brainPaths[i] is the disk path for
     * brainList.entries[i]). Kept off the public catalogue so the path
     * never appears on the public API or the wire. */
    char            brainPaths[BRAIN_LIST_MAX][BRAIN_LIST_PATH_LEN];

    /* Layout A lobby flags — all persist across rounds. */
    bool     openHost;             /* anyone can edit when true */
    bool     allowNewPlayers;      /* live state — drives PACKET_LOCK_TOGGLE */
    bool     autoLockOnGameStart;  /* if true, set allowNewPlayers=false on game start */
    bool     savedAllowNewPlayers; /* what allowNewPlayers was before autoLockOnGameStart fired */
    bool     ranked;               /* LST_RANKED — server enforces bots-off and
                                    * rejects gameType=Open while true. Toggle-on
                                    * also removes existing bots from teams. */
    bool     firstJoinerBecomesHost; /* dedicated-server promotion mode. Set
                                      * via -firstjoinhost. When true and slot
                                      * 0 is empty (or unowned), the next
                                      * incoming player is promoted to host.
                                      * Consumed by the join handler. */
    uint16_t serverLocks;          /* LOBBY_LOCK_* bitmask, set from CLI */
    UploadPolicy uploadPolicy;     /* mirrored from server-startup config */

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

    /* Lobby preview-map state. When a player picks a map from the
     * chooser (Server Maps click OR a completed upload), the server
     * stashes the prior committed map here before applying the
     * preview — so Cancel can revert without re-reading from disk.
     * NULL when no preview is pending; freed on Commit. Stays
     * preserved across successive previews (we keep the ORIGINAL
     * committed map, not the most-recent preview, so one Cancel
     * rolls back to where the user started). */
    BYTE        *previousMapData;
    int          previousMapDataLen;
    char         previousMapName[MAP_STR_SIZE];

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

    /* In-game vote state (back-to-lobby + surrender). Indexed by
     * (kind - 1). See netpacks.h GAME_VOTE_KIND_* / docs/voting_plan.md. */
    struct ServerGameVote {
        uint8_t  kind;             /* GAME_VOTE_KIND_* (BACK_TO_LOBBY/SURRENDER) */
        uint8_t  active;           /* GAME_VOTE_ACTIVE_* */
        uint8_t  triggerSrc;       /* GAME_VOTE_TRIGGER_* */
        uint8_t  teamId;           /* surrender only; 0 = all-teams */
        uint16_t votesMask;        /* bit i = slot i voted yes */
        uint16_t answeredMask;     /* bit i = slot i has answered (yes or no) */
        uint64_t startMs;          /* SDL_GetTicks-style epoch (server frame time) */
        uint64_t deadlineMs;       /* startMs + 60_000 */
        uint64_t lastHeartbeatMs;  /* drives 1Hz broadcast */
        uint64_t concludedAtMs;    /* >0 once active != RUNNING; for auto-dismiss */
        /* Pre-pass grace: once the vote first becomes unanimous we
         * give voters 5 s to change their mind before concluding
         * PASSED. 0 = not pending, else the wall-clock at which the
         * pass will actually fire. If a voter retracts during the
         * grace (yesCount drops below threshold), this clears back
         * to 0 and the vote keeps running normally. */
        uint64_t pendingPassUntilMs;
    } gameVotes[2];
    uint64_t gameVoteWallMs;       /* monotonic ms since serverSim start */
    bool     baseMonopolyTriggeredThisRound;

    /* Forced return-to-lobby countdown (e.g. from a vote-pass). When
     * > 0, the running-state tick decrements this each call; at 0
     * we call serverSimEnterGameOver. Carried in every snapshot
     * header so clients can render their own "Returning to lobby in
     * N" indicator off the value. */
    int32_t  returnToLobbyTicks;
    /* When a vote-pass triggers the game-over transition, lifecycle should
     * skip buildWinMessage so the players don't get the generic
     * "Game over!" line on top of the 3/2/1 countdown. Cleared once
     * consumed. */
    bool     suppressNextWinMessage;

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
};

BOLO_STATIC_ASSERT(offsetof(struct ServerSim, sim) == 0,
                   ServerSim_sim_must_be_first_member);

#endif /* SERVER_SIM_INTERNAL_H */
